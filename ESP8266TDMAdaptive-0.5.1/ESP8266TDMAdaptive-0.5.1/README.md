# ESP8266TDMAdaptive 0.5.1

Bibliotheque Arduino pour une liaison 2-FSK half-duplex autonome entre deux
ESP8266. Livraison figee de la version 0.5.1 deja compilee et flashee :
**aucune modification du code, des exemples, des binaires ou des reglages RF**.

Les fenetres restent de **1 seconde par sens**, soit un cycle de 2 secondes.
Les variantes 250 ms et 500 ms discutees ne font pas partie de cette livraison.

Le ZIP rassemble sources, exemples A/B, binaires, documentation, tests existants,
journaux et empreintes SHA-256, sur le meme principe que la livraison 0.4.1.
Il ne transforme pas les essais incomplets en validations reussies :
le dernier essai 0.5.1 a confirme des echanges mais n'a pas atteint le palier
demande de 5000 bit/s. Voir [VALIDATION.md](VALIDATION.md).

## Installation Arduino

1. Dans l'IDE Arduino, installer ce ZIP via Croquis > Inclure une bibliotheque >
   Ajouter la bibliotheque .ZIP.
2. Eviter une ancienne copie active de cette meme bibliotheque et les autres
   bibliotheques definissant les memes classes RH : les en-tetes seraient ambigus.
   Conserver les anciennes archives de sauvegarde.
3. Installer/selectionner le support de cartes ESP8266. La construction fournie
   utilise le coeur **ESP8266 3.1.2**, la carte **LOLIN/WEMOS D1 mini**,
   le CPU **160 MHz** et la flash **4 Mo**.
4. Ouvrir l'exemple **TDM_A** pour la premiere carte et **TDM_B** pour la seconde.
   Les deux cartes doivent utiliser cette version. La programmation est une
   action distincte : la finalisation de cette archive n'a pas reflashe les ESP.
5. Pour les diagnostics, ouvrir le port serie a **115200 bauds**, fin de ligne LF.
   L'ouverture USB/CH340 peut redemarrer la carte : attendre `TDM_READY`.

Dans le banc existant, A correspond a /dev/ttyUSB1 et B a /dev/ttyUSB0 dans
la VM. Ces noms ne definissent pas les numeros COM sous Windows.
Un seul programme doit ouvrir chaque port serie a la fois.

Apres programmation et alimentation, aucun PC ni fil de synchronisation
n'est necessaire a la liaison. Les exemples produisent automatiquement
des donnees binaires de demonstration et maintiennent la file TX pleine.

## Comportement conserve

- A fournit le calendrier. B cale sa reponse sur les trames valides de A.
- Chaque fenetre TX enchaine autant de paquets presents dans la file que le
  temps disponible permet, sans quota artificiel d'un paquet par fenetre.
- Aucun remplissage n'est ajoute apres les trames pour occuper le reste du
  creneau. Les preambules, en-tetes, CRC, FEC et octets de bourrage internes
  au format de trame existent toujours.
- Le reste trop court pour une trame complete reste silencieux. Une file vide
  emet une trame de controle; il n'y a pas de repetition de donnees pour meubler.
- Le surplus attend les creneaux suivants. Les donnees ne quittent la file TX
  qu'apres acquittement du correspondant.
- Les ACK sont cumulatifs pour les paquets contigus. Une perte au milieu
  provoque la reprise des paquets non acquittes, sans nouvelle livraison des
  doublons deja acceptes. Une file RX pleine n'acquitte pas une donnee rejetee.
- La radio conserve son chemin TX entre les paquets d'une meme salve.
  Les deux tonalites de calibration de 40 ms sont emises au debut de la salve.
- Chaque trame annonce le temps restant, par pas de 16 us. Ce recalage
  synchronise les creneaux logiciels; il ne regle pas physiquement les quartz
  ou la PLL des ESP.
- Les frequences recues sont reestimees pendant l'acquisition. Les valeurs
  f0/f1 affichees sont des estimations repliees, pas des frequences RF absolues.
- La qualite est prise en compte une fois par fenetre; les changements de
  profil sont negocies entre les salves. Apres pertes repetees, la recherche
  reprend au profil commun lent, en conservant la file TX en RAM.
- Les tonalites TX restent **16 / 1016**, canal **6**, APWR=0, ASK=0,
  reception AGC et MODE IQ=1 par defaut. Pas d'adaptation de canal ou de puissance.
- La garde TX programmee reste **20 ms**. Ce chiffre n'est pas une mesure
  RF de garde effectuee au HackRF lors de cette livraison.

## Envoyer ses propres donnees

Avec les exemples fournis :

1. Envoyer `/auto off` sur les deux cartes. Attendre `queue=0` :
   les donnees en attente se vident, la synchronisation radio continue.
2. Envoyer `/send Bonjour B` sur A.
   B doit afficher les donnees dans une ligne RXDATA; le compteur ACK de A
   augmente apres confirmation.
3. Envoyer `/send Bonjour A` sur B pour l'autre sens.
4. `/stats` demande un etat; `/auto on` reactive le producteur de demonstration.

`/send` accepte **1 a 14 octets**, pas 14 caracteres Unicode.
La nouvelle API `write()` permet a une application d'envoyer un bloc plus long
en fragments de 14 octets. La commande serie `/send` reste limitee a 14 octets.

Les files TX et RX contiennent chacune **32 messages**, soit au maximum
**448 octets utiles par file**. La memoire n'est pas illimitee :
respecter la valeur retour de `send()` / `write()` et vider la file de reception.
Voir [API.md](API.md) pour les contrats precis.

## Debits et latence

Les profils sont des vitesses physiques pendant la modulation, pas des
debits utiles garantis. Les exemples plafonnent le profil a 6; le profil 7
reste experimental.

| Profil | us/bit | Brut, bit/s | Paquets complets calcules par TX de 1 s | Utile maximal par sens, octets/s |
| --- | ---: | ---: | ---: | ---: |
| 0 | 1000 | 1000 | 1 | 7 |
| 1 | 800 | 1250 | 1 | 7 |
| 2 | 600 | 1667 | 2 | 14 |
| 3 | 500 | 2000 | 2 | 14 |
| 4 | 400 | 2500 | 3 | 21 |
| 5 | 300 | 3333 | 4 | 28 |
| 6 | 200 | 5000 | 6 | 42 |
| 7 | 140 | 7143 | 9 | 63 |

Capacites calculees pour une file pleine de messages de 14 octets, sans perte.
La premiere trame prend 80 ms de calibration + 650 bits codes; les suivantes
650 bits codes chacune. Gardes et temps de preparation reduisent le temps utile.
Chaque sens dispose d'une fenetre par cycle de 2 secondes.
Les retransmissions, un buffer vide ou des messages courts reduisent le debit.

Derniere observation 0.5.1 : environ **23,3 octets utiles/s par sens** sur
les 12 dernieres secondes mesurees, au profil 5, avec pertes.
Un essai anterieur **0.5.0**, distinct, a mesure environ 39 a 42 octets/s
par sens sur ses intervalles de compteurs a 200 us/bit.
Ce resultat precedent ne qualifie pas automatiquement le firmware 0.5.1.

La latence depend du moment de mise en file, de la place dans la file,
du creneau disponible et des retransmissions. Un creneau d'une seconde
n'est pas une garantie de livraison en une seconde.

IQ35 designe la cadence du demodulateur pendant le travail RX :
environ 36,5 a 36,7 milliers d'estimations/s observees dans le dernier essai.
Chaque estimation utilise quatre acquisitions IQ courtes. Ce n'est ni
un debit de donnees de 35,7 kbit/s, ni un flux audio, ni un echantillonnage
brut uniformement espace.

## Commandes serie

| Commande | Effet |
| --- | --- |
| /stats | Demande les compteurs et le profil actif. |
| /auto on | Remplit la file avec des messages binaires de demonstration distincts. |
| /auto off | Arrete le producteur; laisse les files se vider. |
| /send texte | Met 1 a 14 octets dans la file; QUEUED n'est pas un ACK. |
| /limit N | Plafond negocie de 0 a 7; 6 dans les exemples au demarrage. |
| /pause | Suspend les emissions en conservant les donnees TX en RAM. |
| /run | Reprend par une recherche au profil commun lent. |
| /recal | Oublie la calibration RX pour une nouvelle acquisition. |
| /reset | Redemarre la carte; les files en RAM sont perdues. |
| /gain agc | Retablit l'AGC RX. |
| /gain RF VGA | Diagnostic de gain RX, RF 0..6 et VGA 0..7. |
| /iq 0 ou /iq 1 | Diagnostic IQ; 1 est le defaut et une nouvelle acquisition est requise. |

Les commandes de diagnostic ne constituent pas des reglages conseilles
pour augmenter la portee. Les controles de gain ne changent pas APWR/ASK.

TDM donne les paquets emis/recus, ACK, livraisons, retransmissions et files.
WINDOW donne la taille du dernier TX, les octets en attente, le recalage et
les diagnostics perdus par saturation du tampon (`logDrop`).
`crc` et `reject` incluent plusieurs hypotheses du decodeur et ne sont pas
des taux de perte directs. Une ligne UART tronquee n'est pas une preuve
de perte radio; consulter les compteurs complets et les donnees verifiables.

## Integration

Inclure `ESP8266TDM.h`, construire `RH_ESP8266FSK radio`, puis
`ESP8266TDM link(radio)`. Sur un CPU a 160 MHz :

- Pour reprendre le plafond des exemples, appeler `link.setRateLimit(6)`.
  La classe seule autorise par defaut jusqu'au profil 7.
- Appeler une seule fois `link.begin(ESP8266TDM::MASTER)` sur A et
  `link.begin(ESP8266TDM::FOLLOWER)` sur B; verifier le bool de retour.
- Appeler `link.poll()` continuellement et lire les messages avec `recv()`.
- Garder les traitements applicatifs courts et les sorties serie dans
  le budget fourni par `maintenanceUs()`.
- `poll()` peut occuper le CPU pendant un bloc radio; il n'offre pas une
  latence d'appel strictement non bloquante.

Le protocole 0.5 utilise le marqueur 0xd5 et les indicateurs FIRST/LAST.
**Ne pas melanger une carte 0.4.x et une carte 0.5.x.**
Ce n'est ni du Wi-Fi/IP ni un pont serie transparent. Ne pas utiliser
le Wi-Fi ordinaire simultanement. La cible fournie est ESP8266, pas ESP32.

## Contenu et provenance

| Chemin | Contenu |
| --- | --- |
| src/ | Sources exactes 0.5.1, sans changement lors de la finalisation. |
| examples/TDM_A et examples/TDM_B | Exemples exacts utilises pour les deux binaires. |
| firmware/ | Binaires du dernier flash 0.5.1 confirme dans la conversation. |
| tests/ | Outils existants, conserves sans modification; certains sont historiques. |
| validation/ | Journaux anterieurs, y compris echecs et avertissements. |
| API.md | Contrats d'utilisation et buffers. |
| VALIDATION.md | Resultats, limites et versions auxquelles ils s'appliquent. |
| CHANGELOG.md | Resume des changements deja effectues avant le gel. |
| RELEASE.json | Inventaire des fichiers figes et politique de cette livraison. |
| SHA256SUMS.txt | Empreintes de tous les fichiers de l'archive, sauf ce manifeste lui-meme. |
| LICENSE | Licence MIT conservee. |

Pour la construction future avec la chaine deja utilisee :
`sh build.sh` lance les tests logiciels puis compile A et B.
Aucun test ni aucune compilation n'a ete relance pour preparer cette livraison.
Les outils de test materiel peuvent ouvrir/reinitialiser les ports serie :
ils ne sont pas executes automatiquement par l'installation Arduino.

## Limites connues

- Le palier maximal et la portee ne sont pas garantis; la cause exacte des
  degradations observees n'a pas ete isolee par comparaison controlee.
- Le dernier banc materiel 0.5.1 est incomplet : son attente du profil 6
  a expire. Les etapes suivantes de ce banc ne sont donc pas validees.
- Une ligne de diagnostic tronquee subsiste dans ce journal 0.5.1.
  La journalisation par lignes completes ne peut pas etre declaree exempte
  de ce probleme sur la base des mesures disponibles.
- CRC, sessions et ACK ne sont ni du chiffrement ni de l'authentification.
  Les files et la deduplication ne persistent pas apres une coupure.
  Les commandes a effet durable doivent avoir une protection applicative
  adaptee contre les repetitions apres redemarrage.
- Cette livraison est figee et installable, mais reste une liaison
  experimentale; elle n'est pas une qualification industrielle.
