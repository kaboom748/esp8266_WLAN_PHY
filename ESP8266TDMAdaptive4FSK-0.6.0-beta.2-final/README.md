# ESP8266TDMAdaptive4FSK

## Livraison Finalisee 0.6.0-beta.2

Bibliotheque Arduino pour une liaison 4-FSK half-duplex entre deux ESP8266.
Cette livraison fige la version deja compilee et flashee sur les deux modules,
avec une documentation complete et des empreintes de controle, sur le modele
de la livraison ESP8266TDMAdaptive-0.5.1.

**Aucun changement du firmware, des exemples, des tests ou des reglages.**
La finalisation ne recompile rien, n'ouvre aucun port serie, ne reflashe rien
et ne relance aucun essai radio. Le suffixe `-final` identifie le conditionnement
de l'archive, pas une nouvelle version technique. Le numero reste
`0.6.0-beta.2`, y compris dans les binaires et `library.properties`.

La livraison est complete ; la qualification RF longue duree ne l'est pas.
Le controle materiel existant confirme une liaison bidirectionnelle lors d'un
essai court. Il ne constitue pas une certification de fiabilite dans toutes
les conditions. Voir [les resultats et limites](VALIDATION.md).

## Installation

1. Utiliser deux ESP8266 de type D1 mini, avec 4 Mo de flash.
2. Dans Arduino IDE, installer le coeur ESP8266 **3.1.2**, selectionner
   **LOLIN(WEMOS) D1 R2 & mini**, CPU **160 MHz**, flash **4 MB**.
   La cible utilisee pour les binaires fournis est
   `esp8266:esp8266:d1_mini:xtal=160,eesz=4M`.
3. Installer cette archive avec **Croquis > Inclure une bibliotheque >
   Ajouter la bibliotheque .ZIP**. Ne pas garder plusieurs copies concurrentes
   de cette bibliotheque dans le dossier Arduino `libraries`.
4. L'exemple [TDM_A](examples/TDM_A/TDM_A.ino) est le maitre ;
   [TDM_B](examples/TDM_B/TDM_B.ino) est le suiveur. Un exemplaire de chaque
   role est necessaire. Les deux firmwares doivent utiliser ce meme protocole.
5. Pour consulter les diagnostics, utiliser un terminal serie **115200 bauds**,
   fin de ligne **LF**. Ouvrir un terminal peut reinitialiser la carte.

Les deux ESP du dernier essai possedent deja les binaires de cette livraison :
**aucun reflash n'est necessaire pour beneficier de cette finalisation**.
Les fichiers [A](firmware/TDM_A.ino.bin) et [B](firmware/TDM_B.ino.bin)
sont fournis pour archivage/restauration. Lors du flash precedent, A etait
`/dev/ttyUSB1` et B `/dev/ttyUSB0` ; ces noms peuvent changer apres reconnexion.
Verifier l'identite de chaque carte avant tout futur flash.

La liaison demarre seule a l'alimentation. Les exemples activent un generateur
de donnees de demonstration ; ils ne sont pas un pont UART transparent.

## Fonctionnement

- **4-FSK, deux bits codes par symbole**, mapping Gray `00, 01, 11, 10`.
  Une seule tonalite est emise a un instant donne, pas quatre canaux paralleles.
- Codes du registre RF : `1012, 1020, 4, 12`, soit `-12, -4, +4, +12`
  sur 10 bits. Ce sont des codes de registre, pas des frequences en Hz.
- Canal 6, IQ mode 1, AGC, APWR=0 et ASK=0. **Pas de dithering**.
- Fenetres fixes de **1 000 ms par sens**, cycle de **2 000 ms**.
  Le debit change le nombre de symboles dans la fenetre, pas sa duree.
- Garde TX de 20 ms, marge de fin de trame de 2 ms ; quatre pilotes de
  calibration de 40 ms, soit 160 ms par salve.
- Trames de 32 octets sur l'air, dont **14 octets utiles maximum**.
  CRC16, correction Hamming et entrelacement ; 325 symboles par trame TDM.
- Files TX et RX de **32 messages chacune**, jusqu'a 448 octets utiles chacune.
- Le TX enchaine les messages disponibles tant qu'une trame tient dans sa
  fenetre, puis conserve le reste pour les suivantes. Pas de quota artificiel
  d'une seule trame, pas de recopie du dernier message pour remplir la fin.
- Une file vide peut produire une trame de controle. Les champs fixes et le
  bourrage interne du codage restent necessaires au protocole.
- Acquittements cumulatifs, retransmissions et suppression des doublons dans
  la session. Les donnees restent en file jusqu'a leur acquittement.
- Les annonces de temps restant recalent les **echeances logicielles TDM**.
  Elles ne reglent pas physiquement les quartz. La calibration IQ apprend
  les quatre centres de reception, ce qui est une operation distincte.

Le protocole `0xd6` n'est pas compatible sur l'air avec les versions
2-FSK, 8-FSK ou 16-FSK. Il ne transporte pas de paquets Wi-Fi/IP.

## Adaptation Corrigee

Le controleur provient de la 16-FSK 0.8.0-beta.4 ; seule son adaptation a ete
portee dans la 4-FSK. La modulation et le recepteur 4-FSK sont conserves.

- Apres trois fenetres RX consecutives manquees, descente **d'un profil** :
  `5 -> 4`, puis `4 -> 3` si trois nouvelles occasions echouent, etc.
- Une mauvaise qualite peut aussi declencher une baisse negociee d'un profil.
- Une calibration CRC-valide recente peut etre reutilisee pendant 15 s,
  sauf retour au profil de base ou remise a zero explicite.
- Un profil ayant echoue attend **30, 60, 120 puis 240 secondes** avant un
  nouveau probe, plafond 240 s. Les autres profils ne sont pas penalises.
- Trente-deux observations propres sur le profil effacent sa penalite.
- Le maitre propose les montees ; les deux recepteurs peuvent demander une
  baisse. La fin du delai n'impose pas de montee : il faut aussi une qualite
  suffisante, le plafond autorise et une negociation reussie.

Le demarrage et une transition explicite pause/reprise repartent au profil 0.
Une panne persistante peut atteindre 0 par descentes successives. Abaisser
manuellement le plafond peut sauter plusieurs profils. Ce sont des cas
distincts du repli automatique corrige. Les penalites ne persistent pas
apres une coupure d'alimentation.

## Debit Et Latence

Les exemples plafonnent par defaut au **profil 5 : 300 us/symbole**.
La classe seule autorise par defaut le profil 7 ; l'exemple fixe explicitement
le plafond 5. Un plafond ne force pas le debit courant.

| Profil | us/symbole | Debit brut approx. bit/s | Trames/fenetre TX | Utile ideal octets/s par sens |
| --- | ---: | ---: | ---: | ---: |
| 0 | 1000 | 2000 | 2 | 14 |
| 1 | 800 | 2500 | 3 | 21 |
| 2 | 600 | 3333 | 4 | 28 |
| 3 | 500 | 4000 | 5 | 35 |
| 4 | 400 | 5000 | 6 | 42 |
| 5 | 300 | 6667 | 8 | 56 |
| 6 | 200 | 10000 | 12 | 84 |
| 7 | 140 | 14286 | 17 | 119 |

Ce tableau donne une capacite theorique, avec messages pleins de 14 octets,
pilotes/gardes/trames inclus, sans retransmission ni cout logiciel additionnel.
Chaque sens n'emet qu'une seconde sur deux. **6,67 kbit/s brut ne signifie pas
6,67 kbit/s utile** : au profil 5, le maximum ideal est 56 octets/s par sens,
soit 448 bit/s par sens. Les profils 6 et 7 ne sont pas qualifies sur materiel.

Le dernier essai a observe le profil 5 sur les deux cartes en fin de capture,
avec 8 trames/112 octets dans les derniers diagnostics de fenetre. Il n'a pas
mesure un plateau prolonge a ce debit. Les mesures IQ35, environ 35,6 a
35,7 milliers d'estimations/s en RX actif, ne sont pas un debit utile.

La latence depend du moment d'arrivee dans le cycle, de la file et des reprises.
Elle n'est pas garantie inferieure a une seconde. Il n'y a pas de reglage
public de la duree des fenetres dans cette version.

## Envoyer Ses Donnees

Pour un message manuel, saisir `/auto off` sur **les deux cartes**. Cela arrete
le generateur de demonstration, pas l'adaptation de debit. Les messages deja
en file doivent encore s'ecouler ; consulter `/stats` jusqu'a `queue=0`.

Sur A, saisir `/send Bonjour B`, puis sur B `/send Bonjour A`.
La commande accepte de 1 a 14 **octets**, pas necessairement 14 caracteres UTF-8.
`QUEUED` confirme la mise en file locale, pas la livraison distante.
Un ACK confirme l'acceptation par la bibliotheque distante, pas l'execution
d'une action par son application. Aucun chiffrement/authentification n'est fourni.

Pour une application, utiliser `send()` ou `write()`, appeler `poll()` regulierement
et vider `recv()`. `write()` peut accepter partiellement le buffer : conserver
le reste et reessayer plus tard. Voir [la reference API](API.md).

## Commandes Des Exemples

| Commande | Effet |
| --- | --- |
| `/stats` | Demande les diagnostics ; leur impression peut etre differee. |
| `/auto on` ou `/auto off` | Active/arrete uniquement la production des donnees de demonstration. |
| `/send texte` | Met 1 a 14 octets en file TX, sous reserve de place. |
| `/limit N` | Plafond de profil 0 a 7 ; valeur initiale 5, pas une consigne de debit force. |
| `/pause` | Suspend l'emission tout en conservant les messages en file. |
| `/run` | Reprend apres une pause au profil 0 ; sans effet de reprise si deja actif. |
| `/recal` | Oublie la calibration RX ; ne selectionne pas un nouveau profil. |
| `/reset` | Redemarre la carte ; perd les donnees et l'etat conserves en RAM. |
| `/gain agc` | Restaure le controle automatique de gain RX. |
| `/gain RF VGA` | Diagnostic de gain RX manuel : RF 0..6 et VGA 0..7, pas la puissance TX. |
| `/iq 0` ou `/iq 1` | Change le mode IQ et invalide la calibration ; mode 1 par defaut. |

Les reglages de ces commandes ne sont pas sauvegardes en flash. Ne pas
modifier gain/IQ au hasard pendant une mesure de debit. La file n'est pas
videe par `/auto off` ni par `/pause`. La lecture serie et les impressions
peuvent attendre une plage de maintenance radio.

## Integration Et Diagnostic

Cette bibliotheque utilise des fonctions PHY ESP8266 specifiques. Elle n'est
pas prevue pour ESP32 ni pour une utilisation simultanee du Wi-Fi normal.
Ne pas modifier directement les parametres du pilote radio pendant que TDM
en a la responsabilite. Les classes RH incluses ne garantissent pas la
compatibilite avec toute la bibliotheque RadioHead.

| Diagnostic | Interpretation |
| --- | --- |
| `TDM state=SEARCH` / `SYNC` / `LOCKED` | Recherche, synchronisation ou liaison verrouillee ; LOCKED seul n'est pas une preuve de livraison. |
| `ADAPT waitMs` | Attente avant un nouveau probe du profil suivant, pas une panne du buffer. |
| `WINDOW queuedBytes` | Donnees encore en attente d'ACK ; une file pleine est normale avec le generateur automatique. |
| `WINDOW frames`, `bytes` | Tentatives de la derniere fenetre TX, pas un compteur de livraison garantie. |
| `FSK4` | Quatre centres IQ, periode symbole et debit brut ; dithering desactive. |
| `IQ35 estimatesPerSec` | Estimations pendant les periodes RX actives, pas bit/s applicatifs. |
| `PHY crc` / rejets | Diagnostic de tentatives de decodage ; pas directement un taux de perte de paquets. |
| `logDrop` | Perte de diagnostics serie ; les captures ne sont alors pas exhaustives. |

Les lignes de diagnostic arrivent a des instants differents. Ne pas soustraire
aveuglement des compteurs A et B pour en deduire des pertes radio. Mesurer les
octets **acquittes** sur une meme carte et un intervalle connu pour un debit
utile TX ; les journaux de reception permettent un controle independant des
payloads lorsqu'ils sont complets.

Le RX demande quatre centres IQ distincts et monotones, dans un ordre croissant
ou decroissant. Un repliement IQ qui produit une permutation non monotone
n'est pas pris en charge. Un spectre clair au SDR ne prouve pas a lui seul
que l'ESP decode sans erreur ou sans saturation. La distance, le gain et
l'environnement radio restent des conditions de fonctionnement a mesurer.

## Contenu Et Tracabilite

| Element | Contenu |
| --- | --- |
| [API.md](API.md) | Contrats publics, buffers, adaptation et compteurs. |
| [VALIDATION.md](VALIDATION.md) | Resultats existants, controle de finalisation et limites connues. |
| [CHANGELOG.md](CHANGELOG.md) | Historique et perimetre de cette finalisation. |
| `src/`, `examples/`, `firmware/` | Code et binaires originaux beta.2, inchanges. |
| `tests/`, [build.sh](build.sh) | Tests sur hote et construction de reference. |
| `validation/` | Journaux de construction, flash et essai court precedents. |
| [VALIDATION.txt](VALIDATION.txt) | Rapport bref original, conserve sans modification. |
| `validation/provenance-beta2/` | README, metadonnees, manifeste et rapport originaux. |
| [RELEASE.json](RELEASE.json) | Identite et politique de livraison machine-readable. |
| [SHA256SUMS.txt](SHA256SUMS.txt) | Empreinte de chaque fichier livre, hors le manifeste lui-meme. |
| [LICENSE](LICENSE) | Licence fournie avec le projet. |

Les tests Linux de reference et leurs prerequis figurent dans
[tests/README.md](tests/README.md). Ils n'ont pas ete relances pour cette
finalisation documentaire. Une reconstruction dans un autre environnement
doit etre revalidee avant d'etre assimilee aux binaires archives.
