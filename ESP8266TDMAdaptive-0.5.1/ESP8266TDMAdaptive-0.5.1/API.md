# API ESP8266TDMAdaptive 0.5.1

Contrats etablis a partir des sources figees. Ce document ne change pas le code.
Les exemples Arduino TDM_A et TDM_B sont inclus sans modification.

## Initialisation et execution

`ESP8266TDM link(radio)` reference un objet `RH_ESP8266FSK` qui doit vivre
aussi longtemps que la liaison. Les objets ne sont pas concus pour etre
manipules simultanement par des interruptions et la boucle principale.

`bool begin(Role role, uint32_t session = 0)` :

- Roles admis : MASTER=1 et FOLLOWER=2, un de chaque dans la paire.
- Configure la radio et les adresses; retourne false si deja initialise,
  role invalide ou initialisation radio en echec.
- Une session nulle demande un identifiant aleatoire non nul.
  Une session forcee identique apres chaque reboot compromet la detection
  de redemarrage : garder le comportement par defaut.
- Au depart, les cartes utilisent le profil 0 (1000 us/bit).
  A laisse deux creneaux d'ecoute avant sa premiere emission.
- Regler la cible a 160 MHz; les exemples appellent aussi
  `system_update_cpu_freq(160)`.

`void poll()` fait avancer la radio, les ACK, le calendrier et l'adaptation.
Il doit etre appele continuellement. Les blocs TX et RX peuvent monopoliser
le CPU et masquer les interruptions; ne pas traiter cet appel comme un
ordonnanceur temps reel non bloquant.

`uint32_t maintenanceUs() const` donne le temps de silence connu restant
pour de courts traitements. Zero signifie aucune plage annoncee.
`maintenanceWindow()` est vrai lorsque ce budget depasse 2000 us.
Ce bool ne garantit pas qu'une operation arbitrairement longue tiendra :
prevoir son temps complet, notamment la vidange UART, avec une marge.

## Emission par messages

`bool send(const uint8_t* data, uint8_t length, uint16_t* id = nullptr)`

Accepte et copie un message de 1 a 14 octets dans la file TX. Si le retour
est true, le buffer appelant peut etre reutilise; l'identifiant est renseigne
lorsqu'un pointeur est fourni. Cet identifiant 16 bits boucle apres 65535.

False signifie : liaison non initialisee, pointeur nul, longueur nulle,
longueur superieure a 14 ou file pleine. Aucun message n'est alors ajoute.

**True signifie mis en file, pas transmis ni acquitte.**
Le compteur `statistics().acknowledged` compte les messages acquittes;
`acknowledgedBytes` leurs octets utiles. Les messages restent en file jusqu'au
retour d'un ACK valide. Ils peuvent etre retransmis.

## Emission d'un bloc long

`size_t write(const uint8_t* data, size_t length)`

Decoupe le bloc en messages de 14 octets maximum et retourne le nombre
d'octets effectivement copies dans la file. Retourne zero pour un pointeur
nul, une longueur nulle ou si rien ne peut etre accepte.

Si le retour est inferieur a length, conserver la partie non acceptee et
la soumettre plus tard. Ne pas avancer l'offset de la longueur demandee.

```cpp
size_t accepted = link.write(data + offset, total - offset);
offset += accepted;
```

Chaque fragment a son propre identifiant et sa propre livraison par recv().
Il n'y a pas d'identifiant de bloc, de longueur totale ou de reassemblage
automatique d'un grand message applicatif. L'application doit definir
ses limites de messages si elle souhaite reconstruire des blocs.
Les appels write() distincts ne sont pas fusionnes dans un fragment deja en file.

`size_t availableForWrite() const` retourne le nombre d'emplacements libres
multiplie par 14 : c'est la capacite maximale d'un prochain bloc.
Un message court occupe quand meme un emplacement complet.

`size_t queuedBytes() const` compte les octets des messages non acquittes.
`uint8_t queued() const` compte ces messages, pas les seuls messages non encore emis.

## Reception

`bool recv(Message& message)` retire le plus ancien message disponible.
Retourne false si la file est vide. Un Message contient :

| Champ | Type | Signification |
| --- | --- | --- |
| id | uint16_t | Identifiant attribue par le correspondant. |
| length | uint8_t | Nombre d'octets valides dans data, de 1 a 14. |
| data | uint8_t[14] | Charge utile binaire; pas une chaine terminee par zero. |

`pendingReceive()` compte les messages disponibles.
Consommer ces messages regulierement pour liberer les emplacements.
Un recepteur plein refuse les nouveaux messages correspondants sans les
acquitter; le transmetteur les conserve pour une prochaine tentative.

Les paquets sont livres dans l'ordre au cours de la session courante.
Les doublons d'un message deja accepte ne sont pas livres une seconde fois.
Ces garanties ne constituent pas une livraison persistante exactement
une fois a travers un redemarrage : les sessions, files et identifiants
applicatifs doivent etre interpretes dans ce contexte.

## Dimensions

- MAX_PAYLOAD : 14 octets.
- QUEUE_SIZE : 32 messages TX et 32 messages RX.
- Capacite utile maximale : 448 octets par file.
- Fenetre : 1000000 us par sens.
- `cycleUs()` : 2000000 us apres initialisation.
- Garde TX programmee : 20000 us.
- Aucune option publique pour changer la duree de fenetre dans cette version.

## Etat et adaptation

| API | Resultat ou effet |
| --- | --- |
| state(), stateName() | SEARCH, SYNC ou LOCKED. LOCKED n'est pas une garantie d'absence de pertes. |
| session() | Identifiant de session local. |
| rate(), bitUs() | Profil courant et periode physique d'un bit. |
| setRateLimit(N) | Plafond local 0..7; une valeur hors plage est ignoree. La transition est negociee. |
| rateLimit() | Plafond configure. |
| setAdaptive(false) | Desactive les propositions automatiques locales; ce n'est pas une commande de vitesse RF immediate. |
| probeWaitMs(), rateGood(), rateQuality(), rateBad(), rateLossWindow() | Diagnostics de l'adaptation. |
| pause(true) | Suspend le TX et conserve les messages en RAM. |
| pause(false) | Reprend via la recherche au profil de base. |
| paused() | Etat de pause. |

La classe autorise par defaut jusqu'au profil 7. Les exemples appellent
`setRateLimit(6)` avant begin() pour limiter les essais au profil 200 us/bit.

## Compteurs

`statistics()` retourne une copie des compteurs.

- txCells/rxCells : trames terminees/recues, y compris les controles.
- delivered/acknowledged : messages livres localement / acquittes par le pair.
- deliveredBytes/acknowledgedBytes : octets utiles correspondants.
- retries/duplicates : tentatives repetees / doublons recus.
- missed : rendez-vous manques; ce n'est pas un compteur exhaustif de chaque
  paquet physique perdu au milieu d'une salve.
- queueFull : refus de reception faute de place.
- outOfOrder : paquet refuse car un precedent manque ou la base est inconnue.
- txFailures, invalid, resyncs, recoveries : echecs et reprises.
- windowFrames/windowBytes : taille du dernier creneau TX termine; en cas
  d'abandon, ces valeurs incluent les tentatives engagees.
- txWindows, clockSyncs, clockErrorUs : creneaux, mises a jour du calendrier
  et erreur temporelle estimee a partir du protocole.

Ne pas additionner les octets recus et acquittes sur une meme carte pour
annoncer le debit d'un seul sens : ce sont les deux directions opposees.
