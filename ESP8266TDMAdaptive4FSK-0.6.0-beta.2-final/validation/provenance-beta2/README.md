# ESP8266TDMAdaptive4FSK 0.6.0-beta.2

Version experimentale derivee de la 4-FSK 0.6.0-beta.1, avec le controleur
d'adaptation corrige de la 16-FSK 0.8.0-beta.4. Seule l'adaptation change :
le pilote RF/IQ, les quatre tonalites, le protocole sur l'air, le decodeur,
les fenetres, la calibration de salve et les files sont conserves.
Les anciennes archives ne sont pas modifiees. Voir VALIDATION.txt pour
les resultats de cette livraison; ce n'est pas une certification de produit.

## Adaptation Du Debit

- Apres trois fenetres RX manquees, descente d'un seul profil (5 -> 4,
  puis 4 -> 3 si necessaire), au lieu d'un retour immediat au profil 0.
- Remise a zero de la serie de pertes apres chaque descente, pour laisser
  trois nouvelles occasions de reception au profil inferieur.
- Conservation de la calibration CRC-valide tant qu'elle reste recente
  (15 s) et que le repli ne rejoint pas le profil 0.
- Avant de retenter le profil defaillant : 30, 60, 120 puis 240 secondes
  d'attente, avec plafond de 240 s. Les autres profils ne sont pas penalises.
- Une periode de 32 observations propres sur ce profil efface sa penalite.
- Le demarrage et les commandes explicites pause/run restent au profil 0.
  Une panne persistante peut aussi y arriver, mais par descentes successives.
- Le maitre propose les montees; les deux recepteurs peuvent demander
  une baisse. Les controles de qualite et la negociation restent inchanges.

## Modulation

- Half-duplex : A emet, puis B emet. Une seule tonalite a la fois.
- Quatre tonalites, deux bits codes par symbole, mapping Gray.
- Codes du registre RF : 1012, 1020, 4, 12, soit -12, -4, +4, +12 sur 10 bits.
- Mapping dans cet ordre : 00, 01, 11, 10.
- Canal 6, IQ mode 1, AGC, puissance APWR=0/ASK=0 conserves.
- Ces codes sont des positions de registre, PAS des frequences en Hz.
  Leur separation et leur reception effective restent a mesurer.
- Le recepteur apprend quatre centres mesures et accepte un ordre de
  frequences croissant ou decroissant. Il rejette les estimations ambigues.
- Une permutation non monotone due au repliement IQ n'est PAS prise en charge.
  Les quatre centres doivent rester distincts et ordonnes dans la mesure IQ.
- CRC16, Hamming entrelace, acquittements, retransmissions et files conserves.

Le dithering n'est pas implemente dans cette beta : il n'existe aucune
variation volontaire des tonalites ou de la cadence IQ. Aucun besoin
mesure ne le justifie, et il pourrait elargir les groupes de frequences.
La cadence visee d'estimation reste 27 us; le portage de l'adaptation
ne modifie pas la boucle d'acquisition IQ.

## Fenetres Et Calibration

Les fenetres restent fixes a 1000000 us par sens, cycle de 2000000 us.
La garde TX reste de 20000 us et la marge par trame de 2000 us.
Il n'y a pas de remplissage de fin de fenetre.

Chaque salve commence par QUATRE pilotes de 40000 us : 160 ms au total,
contre 80 ms dans la version 2-FSK. Les trames suivantes de la meme
fenetre partagent cette calibration, sans quitter TX entre elles.
Un historique de 128 estimations de calibration, espacees d'au moins
1 ms, sert a rechercher quatre groupes. Les centres d'une reception
CRC-valide peuvent etre reutilises pendant 15 secondes.
Ces choix sont conservateurs, pas une garantie d'acquisition RF.

Une trame de 14 octets utiles conserve 650 bits codes, transmis en
325 symboles. Le preambule et la synchronisation sont inclus.

| Profil | us/symbole | bit/s bruts en TX | Trames/fenetre max.* | Octets/s utiles/sens max.* |
| --- | ---: | ---: | ---: | ---: |
| 0 | 1000 | 2000 | 2 | 14 |
| 1 | 800 | 2500 | 3 | 21 |
| 2 | 600 | 3333 | 4 | 28 |
| 3 | 500 | 4000 | 5 | 35 |
| 4 | 400 | 5000 | 6 | 42 |
| 5 | 300 | 6667 | 8 | 56 |
| 6 | 200 | 10000 | 12 | 84 |
| 7 | 140 | 14286 | 17 | 119 |

* Calculs avec buffers pleins, sans perte, sans retransmission et hors
petits delais logiciels. Ce ne sont PAS des mesures sur ESP.
Les exemples plafonnent au profil 5 (300 us/symbole). Le demarrage utilise
le profil 0; la recuperation automatique descend d'un palier a la fois.
Tous ces profils restent 4-FSK.
Il n'y a pas de bascule negociee vers la 2-FSK.

## Installation Arduino

Dans Arduino IDE : Croquis > Inclure une bibliotheque > Ajouter la
bibliotheque .ZIP. Carte : LOLIN(WEMOS) D1 R2 & mini, CPU 160 MHz,
flash 4 MB, coeur ESP8266 3.1.2.

Ne pas installer simultanement plusieurs variantes de cette bibliotheque dans le meme
sketchbook : elles exposent les memes noms d'en-tetes et de classes.
Conserver l'archive 0.5.1 pour revenir a cette version.
Installer cette mise a jour sur les deux roles pour partager la meme adaptation.
Cette beta ne communique pas avec le firmware 0.5.1 2-FSK.
L'identifiant de protocole passe de 0xd5 a 0xd6.

Exemples :

- examples/TDM_A/TDM_A.ino : maitre, role 1.
- examples/TDM_B/TDM_B.ino : suiveur, role 2.

Les exemples emettent automatiquement des messages de demonstration
apres leur demarrage. Les binaires des deux roles sont fournis; les journaux
de construction et de flash sont conserves dans validation/.

## API

L'interface ESP8266TDM garde begin(), poll(), send(), write(), recv(),
setAdaptive(), setRateLimit(), pause(), statistics() et les files de
32 messages de 14 octets (448 octets par file).
write() peut accepter seulement une partie du buffer : conserver le reste
et reessayer apres poll(). recv() fournit des messages, pas un flux reassemble.
Un send() reussi confirme la mise en file, pas la livraison distante.
Les files et l'antidoublon ne persistent pas apres redemarrage.

- symbolUs() : duree d'un symbole 4-FSK.
- rawBitRate() : debit brut en phase TX, deux bits par symbole.
- bitUs() : ancien nom conserve pour compilation, renvoie AUSSI la duree
  d'un symbole dans cette branche; ne pas l'interpreter comme le temps d'un bit.
- cycleUs() : 2000000 apres begin().

Ne pas appeler directement setBitUs(), setBitRate(), setFourFsk() ou
setModemConfig() sur le pilote pendant une session TDM : les periodes sont
negociees par ESP8266TDM. setTones() du pilote ne concerne que son mode 2-FSK.
Les quatre codes de cette beta sont fixes dans Fsk4.h et dans la boucle TX.

## Diagnostics

Serie 115200 bauds, commandes terminees par LF. Ouvrir un port serie peut
reinitialiser une carte. Les commandes de demonstration restent :
/stats, /auto on, /auto off, /send texte, /limit 0..7, /pause, /run,
/recal, /iq 0, /iq 1, /gain agc, /gain RF BB et /reset.
/auto off arrete le producteur de messages, pas l'adaptation du debit.
/limit fixe un plafond, pas un profil force.

La ligne TDM affiche symbolUs au lieu de bitUs. FSK4 affiche les quatre
centres appris, le debit brut et dither=off. IQ35 compte les estimations,
pas les bits utiles. Les anciens scripts de banc 0.5.1 ne sont pas inclus
car leurs attentes de modulation et de nombre de trames ne sont plus valides.
Le journal serie est herite de la 0.5.1; son ancien risque de ligne
tronquee n'est pas declare corrige par cette evolution de modulation.

## Construction Et Limites

sh build.sh lance les controles logiciels sur hote puis compile A et B.
Il n'ouvre aucun port serie et ne televerse aucun firmware.
Voir VALIDATION.txt pour les resultats et les limites de cette livraison.

Le decodage synthetique ne modele ni le front-end RF, ni l'AGC, ni le
repliement reel, ni les interruptions et delais de l'ESP8266. Une compilation
reussie ne prouve pas que les quatre tonalites seront separees en reception.
Pas de promesse de debit, de portee ou de fiabilite. Pas de chiffrement
ni d'authentification. Utiliser uniquement un environnement radio autorise.
