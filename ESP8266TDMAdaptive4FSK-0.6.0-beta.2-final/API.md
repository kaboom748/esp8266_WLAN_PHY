# API ESP8266TDMAdaptive4FSK 0.6.0-beta.2

Reference du code livre dans [ESP8266TDM.h](src/ESP8266TDM.h) et
[ESP8266TDM.cpp](src/ESP8266TDM.cpp). Cette documentation ne change pas l'API.

## Cycle De Vie

`ESP8266TDM(RH_ESP8266FSK& radio)` conserve une reference au pilote. Le pilote
doit vivre au moins aussi longtemps que la liaison. Utiliser un seul
proprietaire de la radio par carte. L'API n'est pas concurrente ni destinee
aux interruptions.

`bool begin(Role role, uint32_t session=0)` initialise la liaison. Roles :
`MASTER=1`, `FOLLOWER=2`. Retourne false si deja initialisee, role invalide
ou initialisation radio impossible. Session 0 demande un identifiant aleatoire
non nul. Un identifiant impose et reutilise apres redemarrage peut empecher
la bonne detection du changement de session par le pair.

`void poll()` fait progresser la radio, les fenetres, les files et l'adaptation.
L'appeler tres regulierement depuis `loop()`. Les traitements RF peuvent
masquer les interruptions et occuper le CPU : cette methode n'est pas
strictement non bloquante et sa duree n'est pas constante.

| Methode | Resultat |
| --- | --- |
| `state()` | `SEARCH`, `SYNC` ou `LOCKED`. |
| `stateName()` | Nom textuel de l'etat. |
| `session()` | Identifiant de la session locale. |
| `cycleUs()` | Duree du cycle, 2 000 000 us apres initialisation. |
| `maintenanceUs()` | Temps de maintenance estime ; 0 sans plage libre connue, 100 000 us en pause. |
| `maintenanceWindow()` | Vrai si une marge de maintenance superieure a 2 000 us est disponible. |

Une plage de maintenance n'est pas une autorisation de bloquer indefiniment.
Budgeter le travail entier, notamment le temps de transmission UART, avec
une marge. Une impression longue peut perturber les echeances radio.

## Emission

```cpp
bool send(const uint8_t* data, uint8_t length, uint16_t* id=nullptr);
size_t write(const uint8_t* data, size_t length);
```

`send()` accepte un message binaire de **1 a 14 octets**. Il copie les donnees
et retourne true si le message est mis en file ; le buffer appelant peut alors
etre reutilise. `id`, s'il est fourni, recoit l'identifiant 16 bits du message,
qui reboucle apres 65535. False signifie : liaison non initialisee, pointeur
nul, taille nulle, taille superieure a 14 ou file pleine.

Attention au type `uint8_t` de `length` : une taille plus grande que 255 peut
etre tronquee avant l'appel. Valider la taille ou utiliser `write()`.

`write()` fragmente en messages de 14 octets maximum et retourne le nombre
d'octets effectivement copies. Un retour partiel est normal si la file est
pleine. Retour 0 si aucun octet ne peut etre accepte, si non initialise,
pointeur nul ou longueur nulle. Il n'attend pas une place libre.

Exemple de principe pour un buffer applicatif persistant :

```cpp
// Dans loop(), avec offset, data et length conserves entre les appels.
link.poll();
if (offset < length) {
  offset += link.write(data + offset, length - offset);
}
```

Cet extrait n'est pas un sketch complet. Il ne remplace pas l'initialisation
et le traitement RX presentes dans les exemples fournis. `write()` n'ajoute
pas de longueur globale et ne reassemble pas automatiquement les fragments
au recepteur ; l'application doit definir son propre format de blocs.
Il ne complete pas un ancien message partiel deja en file.

| Methode | Unite et signification |
| --- | --- |
| `availableForWrite()` | `(32 - queued()) * 14` octets ; capacite maximale en messages pleins. |
| `queued()` | Messages TX non acquittes, y compris ceux deja emis. |
| `queuedBytes()` | Somme des octets utiles non acquittes. |

Une file TX contient au maximum **32 messages / 448 octets utiles**.
Trente-deux messages de 1 octet occupent aussi toute la file. La bibliotheque
emet autant de trames que le permet la fenetre puis reporte le reste.

True de `send()` ou un retour positif de `write()` ne prouve pas une livraison.
Les messages sont gardes en RAM et reemis jusqu'a ACK ; aucun delai d'expiration
applicatif ni operation publique d'annulation/purge n'est fourni. Un ACK
signifie que la bibliotheque distante a accepte le message, pas que son
application a execute une action. Une panne d'alimentation perd la file.

## Reception

```cpp
struct Message {
  uint16_t id;
  uint8_t length;
  uint8_t data[14];
};
bool recv(Message& message);
uint8_t pendingReceive() const;
```

`recv()` retire le plus ancien message disponible ; false si la file est vide.
`pendingReceive()` compte les messages en attente. `data` est binaire, sans
terminateur nul garanti : utiliser `length` pour copier, afficher ou parser.

Vider regulierement les 32 places RX. Quand elles sont pleines, de nouvelles
donnees ne sont pas acceptees ni acquittees ; l'emetteur doit reessayer.
Le protocole acquitte un prefixe contigu : un trou peut retarder les messages
suivants. Les doublons sont filtres dans la session. Ce n'est pas une garantie
persistante d'execution exactement une fois a travers les redemarrages.

## Debit Et Adaptation

| Methode | Contrat |
| --- | --- |
| `rate()` | Profil courant 0..7. |
| `symbolUs()` | Periode d'un symbole 4-FSK en microsecondes. |
| `bitUs()` | Ancien nom : alias de `symbolUs()`, pas duree d'un bit individuel en 4-FSK. |
| `rawBitRate()` | `2000000 / symbolUs()` en bit/s, division entiere ; 6666 au profil 5. |
| `setRateLimit(uint8_t limit)` | Plafond 0..7 ; valeur >=8 ignoree. N'impose pas une montee immediate. |
| `rateLimit()` | Plafond configure. |
| `setAdaptive(bool enabled)` | Active/desactive les demandes automatiques locales ordinaires de changement. |
| `probeWaitMs()` | Attente restante avant probe du profil suivant, en millisecondes. |
| `rateGood()` | Credits de bonne qualite, pas un nombre d'octets. |
| `rateQuality()` | Qualite lissee en Q4 ; petite valeur favorable. |
| `rateBad()` | Indicateur de mauvaise qualite du controleur. |
| `rateLossWindow()` | Historique compact de pertes, pas un pourcentage. |

Periodes des profils : `1000, 800, 600, 500, 400, 300, 200, 140 us/symbole`.
Le plafond par defaut de la classe est 7 ; les exemples appellent
`setRateLimit(5)` avant `begin()`. Le demarrage se fait au profil 0.

**`setAdaptive(false)` ne verrouille pas absolument un profil.** Le plafond,
la reprise sur perte de synchronisation et les decisions du pair peuvent
encore imposer un changement. Il n'existe pas d'API publique de debit force
ou de duree de fenetre dans cette livraison.

Le controleur evalue la qualite par fenetre RX. Il retient le pire nombre de
corrections local/distant et lisse la qualite en Q4 :
`quality = (quality * 7 + worst * 16) / 8`.
`worst >= 12` ou `quality >= 64` marque une mauvaise observation ;
`quality <= 32` permet d'accumuler des credits. Quatre credits et la fin
du cooldown sont necessaires a une proposition de montee du maitre.
La negociation et le plafond du pair restent determinants.

Trois fenetres consecutives manquees declenchent une reprise au profil
inferieur, pas directement a 0. La serie est effacee a chaque descente.
Les penalites de probe sont propres au profil ayant echoue : 30/60/120/240 s,
plafond 240 s, avec stockage 32 bits. Trente-deux observations propres sur
le profil effacent sa penalite. Les messages TX restent en file durant la
reprise. Une calibration CRC-valide recente peut etre conservee 15 s sauf
retour au profil 0 ou reprise explicite qui l'invalide.

## Pause

`pause(true)` suspend l'emission ; `pause(false)` la relance. Chaque
**changement effectif** de cet etat provoque une reprise explicite au profil 0.
Repeter la valeur deja active ne fait rien. `paused()` expose cet etat.
Ce n'est pas une mise hors tension du recepteur. Les files sont conservees
et une application peut encore appeler `send()`
ou `write()` pendant la pause jusqu'a saturation. Une pause n'est pas une
purge, ni une sauvegarde persistante des donnees.

## Statistiques

`statistics()` retourne une **copie** de `Statistics`. Il n'existe pas de
methode publique pour remettre ces compteurs a zero. Ils sont en RAM et
peuvent reboucler ; conserver les instants et traiter le debordement pour
calculer des differences sur une longue duree.

| Champs | Interpretation |
| --- | --- |
| `txCells`, `rxCells` | Cellules traitees, controles compris ; pas uniquement des messages utiles. |
| `delivered`, `deliveredBytes` | Messages/octets acceptes en RX local. |
| `acknowledged`, `acknowledgedBytes` | Messages/octets TX locaux acquittes par le pair. |
| `retries` | Retransmissions de messages. |
| `duplicates` | Doublons detectes en RX. |
| `queueFull` | Messages entrants refuses parce que la file RX est pleine ; les refus de send() ne l'incrementent pas. |
| `outOfOrder` | Messages recus hors du prefixe contigu attendu. |
| `missed`, `missStreak`, `streak` | Pertes de fenetres et series de pertes/succes. |
| `txFailures`, `invalid` | Echecs de transmission du pilote et cellules invalides. |
| `rateUps`, `rateDowns` | Changements de profil vers le haut / bas. |
| `recoveries`, `resyncs` | Reprises et pertes de synchronisation. |
| `txWindows` | Fenetres TX terminees. |
| `windowFrames`, `windowBytes` | Tentatives de la derniere fenetre TX terminee, y compris eventuelles tentatives avortees. |
| `cycle` | Identifiant 16 bits du cycle TDM. |
| `clockSyncs`, `clockErrorUs` | Recalages des echeances et dernier ecart logiciel en us. |
| `lastRxMs` | Instant local de derniere reception, selon `millis()`. |
| `peerCorrections` | Derniere indication de corrections fournie par le pair. |

Pour le debit TX utile local, utiliser la variation de `acknowledgedBytes`
divisee par une duree mesuree. Pour le debit RX local, utiliser
`deliveredBytes`. Ne pas additionner ces deux compteurs pour annoncer le
debit d'un seul sens. Les captures serie des deux cartes ne sont pas des
instantanes synchrones et peuvent etre incompletes.

## Proprietaire Du Pilote Radio

Ne pas appeler directement `setBitUs`, `setModemConfig`, `setFourFsk` ou des
changements de mode RF pendant que la liaison TDM gere le pilote. Le reglage
`setTones` du pilote concerne le mode 2-FSK, pas les quatre tonalites fixes de
cette livraison. Les diagnostics et commandes d'exemple sont documentes
dans [README.md](README.md) ; ce ne sont pas des garanties de stabilite de
toute l'API interne du pilote.
