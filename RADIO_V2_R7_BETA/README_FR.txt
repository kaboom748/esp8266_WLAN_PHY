MODEM RADIO ESP8266 — GUIDE FRANÇAIS
Protocole radio v2 / première version du modem adaptatif

VERSION : RADIO_V2_R7. Les résultats exacts sont dans RAPPORT_TESTS.txt.
Pour le montage VM/VirtualHere actuel, utiliser le chemin USBFS avec 64 lectures
préparées et XON/XOFF logiciel. GUIDE_PONT_VM.txt explique son utilisation avec
une application série normale. Le chemin ttyUSB ordinaire ne bénéficie pas
de cette correction. Les essais et échecs historiques restent conservés.

Ce projet relie deux ports série par une modulation 4-FSK native à 2,4 GHz.
Les ESP transportent les octets de l'application avec le contrôle de flux
standard XON/XOFF. La compression, la correction d'erreurs et les acquittements
sont internes à la liaison radio.

Les deux appareils doivent utiliser les firmwares A et B issus de la même
version. Le format radio v2 n'est pas compatible avec le format v1 de T115_r1.
Ne pas laisser un appareil en v1 et l'autre en v2.

Les résultats matériels, les limites constatées et les versions effectivement
testées sont à consulter dans RAPPORT_TESTS.txt. Ce guide ne constitue pas une
validation de débit, de portée ni de fonctionnement dans tout environnement.


1. Organisation de la liaison

A est le maître, B est l'esclave. Le calendrier TDD conserve deux demi-créneaux
de 15 ms, soit un cycle de 30 ms : un créneau A vers B puis un créneau B vers A.
Le maître émet à chaque tour, même sans données utilisateur, afin de maintenir
la référence temporelle et les échanges de contrôle. Les protections contre
une préparation tardive ou une erreur matérielle peuvent faire sauter une
émission plutôt que l'envoyer hors créneau.

L'esclave acquiert le début d'une trame du maître et en déduit son époque,
c'est-à-dire l'origine du cycle TDD. Il recale cette époque sur les réceptions
valides. Après 12 réceptions attendues manquées, il repasse en recherche de
synchronisation. Une coupure radio conserve les données en attente; un
changement de session après un redémarrage est traité séparément pour éviter
de mélanger deux transferts.

Le profil livré utilise :
  - CPU ESP8266 à 160 MHz;
  - capture IQ à 625 000 échantillons/s;
  - 2 échantillons par symbole, 4-FSK;
  - tons natifs -234,375 / -78,125 / +78,125 / +234,375 kHz;
  - séquence de synchronisation de 24 symboles;
  - A : réception canal 11, émission canal 3; B : réception 3, émission 11.

Il s'agit d'une liaison radio propriétaire, pas d'une connexion Wi-Fi/IP.
Le mécanisme temporel et la modulation de référence sont conservés; cette
version adapte la protection FEC et la quantité de données utiles.

Les profils adaptatifs 2-FSK, 8-FSK et 16-FSK ne sont pas implémentés dans cette
première version. Des constantes ou du code expérimental ancien peuvent rester
dans legacy.h; ils ne constituent pas des modes disponibles ou validés. QAM
n'est pas utilisée. Conserver les options PHY par défaut pour cette version.


2. Correction d'erreurs, adaptation et retransmissions

Les deux profils occupent toujours 244 octets sur l'air. Le nombre de symboles,
les durées de capture et le préambule ne changent donc pas lors de l'adaptation.

  Profil robuste : en-tête 16 + charge utile 192 + CRC32 4 + parité RS 32.
  Profil rapide  : en-tête 16 + charge utile 208 + CRC32 4 + parité RS 16.

Le profil robuste corrige jusqu'à 16 erreurs d'octets par codeword; le rapide
jusqu'à 8. Le CRC, la version, le profil, les métadonnées et le padding sont
vérifiés avant admission. Le bit de profil reçu sert seulement à ordonner les
deux essais de décodage; il est vérifié de nouveau après correction.

L'émetteur commence en mode robuste. Par défaut, 64 blocs consécutifs acquittés
après une seule tentative permettent de passer au profil rapide. Ce seuil est
réglable à la compilation. Chaque sens suit ses propres acquittements.

Après trois tentatives rapides sans acquittement, le bloc en attente repasse
en protection robuste. Si son contenu encodé dépasse 192 octets, il est envoyé
en deux fragments robustes : 186 octets de contenu dans le premier, puis le
reste, chacun précédé d'un en-tête de fragment de 6 octets. Ils gardent le même
numéro de bloc. L'acquittement est global : il n'est envoyé qu'après réception
complète, vérification, éventuelle décompression et admission dans la file UART.
Une répétition du bloc n'entraîne pas une deuxième livraison à l'application.

Les retransmissions réutilisent le contenu déjà préparé. Les données ne sont
pas recompressées différemment à chaque tentative.


3. Compression

Heatshrink fonctionne avec une fenêtre W8 de 256 octets et un paramètre L4,
sans allocation dynamique. Chaque bloc source contient au maximum 512 octets.
L'état du compresseur et du décompresseur est réinitialisé pour chaque bloc :
aucun dictionnaire persistant n'est partagé entre les ESP.

Le transport essaie de compresser lorsque davantage d'octets source pourraient
tenir dans une seule trame. Il garde le résultat uniquement s'il tient dans le
profil choisi et réduit la taille; sinon il transmet des octets bruts. Les
petits échanges interactifs ne sont donc pas systématiquement compressés.

Le calcul a lieu après une tentative d'émission radio, uniquement si plus de
8 ms restent avant la préparation du prochain créneau. Il peut préparer le
bloc suivant pendant que le bloc courant attend son acquittement. Le résultat
est conservé dans un cache distinct : les octets source correspondants restent
en attente, dans leur ordre initial, jusqu'à l'adoption de ce cache. Une
retransmission du bloc courant n'écrase pas le bloc préparé à l'avance.
Si la liaison est revenue au profil robuste depuis le calcul du cache, son
contenu est conservé et envoyé avec la protection robuste, en deux fragments
si nécessaire.

Après une réception ou un ACK, la préparation ordinaire ne lance pas le
compresseur : elle adopte le cache disponible ou copie un préfixe brut. Elle
demande une marge supérieure à 500 us. Si un essai de compression ne donne pas
un résultat utilisable, les 16 prochains nouveaux blocs bruts sont préparés
sans nouvel essai; les retransmissions ne consomment pas ce compteur. Cela
évite de répéter le calcul à chaque bloc d'un flux incompressible.

Un petit flux neuf devient admissible à la préparation après 5 ms de
regroupement; il n'attend pas d'atteindre 512 octets.
Ce délai s'ajoute à l'attente du créneau radio et aux éventuelles reprises.
Les marges de 8 ms et 500 us ne sont pas une preuve de respect de toutes les
échéances sur ESP8266 : les durées doivent être vérifiées sur le build testé.

L'enveloppe comprimée indique le format, la longueur originale et un CRC32 des
octets décompressés. Une erreur de format, de longueur ou de CRC interdit la
livraison et l'acquittement. La compression ne garantit aucun gain pour un
fichier déjà comprimé ou des données aléatoires.


4. Utilisation du port série

Réglage de référence : 115 200 bauds, 8 bits, sans parité, 1 bit d'arrêt,
contrôle de flux logiciel XON/XOFF. Ne pas activer RTS/CTS en plus.

Le firmware n'ajoute aucun protocole d'adaptation côté PC : pas d'en-tête privé,
de CRC série, de commande AT, de bannière applicative ni de commande de
diagnostic dans les données. Les chaînes ~S~ et ~R~, les octets 0x7d et 0x7e,
ainsi que NUL, sont des données ordinaires pour les ESP.

Deux valeurs sont réservées au contrôle de flux :
  0x11 = XON, autorisation de reprendre;
  0x13 = XOFF, demande de pause.

Pour du texte ordinaire, aucun échappement supplémentaire n'est nécessaire.
Pour transporter les 256 valeurs d'octet d'un fichier binaire, les logiciels
des deux PC doivent utiliser leur propre convention d'échappement des valeurs
réservées. L'ESP transmet ces octets échappés tels quels; il ne les décode pas.

Le banc propose une convention optionnelle avec --escaped-binary : il échappe
0x11, 0x13 et son caractère d'échappement 0x7d, puis vérifie les octets originaux
à l'arrivée. C'est une option de l'application de test, pas une obligation pour
un terminal texte ni un nouveau protocole imposé par le firmware.

Activer le contrôle de flux sur les deux PC et continuer à lire le port même
pendant l'émission. Ne pas ouvrir simultanément un terminal et le banc sur le
même port. Le firmware peut mettre XOFF lorsque la liaison ou les files ne
permettent plus une livraison sûre.

Par défaut, les files d'entrée et de sortie contiennent chacune 2048 octets.
L'entrée demande XOFF lorsqu'il reste au plus 512 octets libres et autorise
XON lorsqu'il en reste au moins 1024, si la liaison est prête et sans défaut.

Une option expérimentale agrandit seulement l'entrée à 8192 octets, avec XOFF
à 4096 octets libres et XON à 6144. Elle réserve davantage de place aux octets
déjà en vol dans le PC ou le pont USB après XOFF. La sortie reste à 2048 octets;
le surcoût statique est de 6144 octets de RAM. Cette option n'a pas résolu le
problème observé avec le XON/XOFF du système dans la
VM : sur R4, l'essai à 8192 octets est passé, mais celui à 65536 a saturé les
8192 octets d'entrée et déclenché un défaut de débordement sur les deux ESP.
Les compteurs UART ont enregistré 66294/66320 octets lus sur A/B malgré
165/166 XOFF émis. Cette réserve ne doit donc pas être présentée comme un
correctif du contrôle de flux de la VM. Ces observations n'identifient pas
à elles seules le composant fautif et ne valident ni n'invalident PuTTY Windows.
Les options de compilation correspondantes figurent à la section 6.

En cas de défaut UART permanent ou de changement de session pendant un
transfert, le firmware bloque le flux plutôt que de poursuivre avec une suite
d'octets potentiellement endommagée. Reprendre alors depuis un état connu des
deux appareils et de l'application; un redémarrage n'est pas une reprise de
fichier persistante.

Le firmware applicatif reste silencieux. Des octets du démarrage ROM peuvent
toutefois apparaître lors d'un reset. Le banc journalise séparément une fenêtre
initiale d'au moins trois secondes avant d'envoyer les données de test; il ne
filtre pas arbitrairement le flux pendant le transfert.


5. Répertoire et identification des appareils

Dans la VM, utiliser :

  cd /home/kaboom/modemradio_20261016/modem_radio

Identités des deux ESP :
  A / maître  : b4:e6:2d:23:0e:f4
  B / esclave : 68:c6:3a:d6:0e:e8

Les noms /dev/ttyUSB0 et /dev/ttyUSB1 peuvent être inversés après une reconnexion.
Identifier les appareils avant d'attribuer A et B. Les liens /dev/serial/by-id
décrivent généralement le convertisseur USB-série, pas directement la MAC ESP.

Lister les ports Linux :

  python3 -m serial.tools.list_ports -v
  ls -l /dev/serial/by-id/

Les variables ci-dessous sont des exemples à remplacer par les ports dont
l'identité A/B a été vérifiée :

  PORT_A=/dev/ttyUSB0
  PORT_B=/dev/ttyUSB1

Vérifier aussi que les deux ports sont bien accessibles dans la VM. Un port
encore réservé par un autre programme ou une connexion VirtualHere absente ne
peut pas servir au banc.


6. Tests logiciels et compilation

Prérequis : Python 3, gcc, g++, pyserial pour le banc série, arduino-cli et le
core ESP8266 pour la compilation embarquée. La cible utilisée par build.py est
esp8266:esp8266:d1_mini:xtal=160,eesz=4M. Les sources Heatshrink sont incluses
dans vendor/heatshrink; aucune récupération réseau n'est nécessaire pour elles.

Exécuter les tests logiciels sans toucher aux appareils :

  python3 run_tests.py
  python3 run_tests.py --sanitize

Le second passage active AddressSanitizer et UndefinedBehaviorSanitizer.
Les sorties et results.json se trouvent dans test_results/release et
test_results/sanitized. Ces tests couvrent notamment les deux codecs RS, les
conversions de phase, la compression, le transport, l'UART et le banc Python.
Ils ne remplacent pas une mesure des échéances sur ESP8266.

Compiler les deux firmwares, sans flasher :

  python3 build.py --name RADIO_V2

Le résultat est enregistré dans builds/RADIO_V2 : config.json, sources ModemA
et ModemB, journaux build_A.log/build_B.log et binaires bin_A/bin_B. Le nom du
build identifie le dossier; il ne change pas le protocole.

Compiler un témoin sans compression ni promotion rapide :

  python3 build.py --name RADIO_V2_REFERENCE --compression 0 --adaptive-fec 0

Compiler l'option expérimentale avec une réserve d'entrée UART plus grande,
sans programmer les appareils :

  python3 build.py --name RADIO_V2_INPUT8192 --uart-input-ring 8192 \
    --uart-stop-free 4096 --uart-resume-free 6144

Les valeurs par défaut sont respectivement 2048, 512 et 1024. La taille
d'entrée doit être une puissance de deux, au plus 32768, avec des seuils
respectant 0 < stop < resume <= taille d'entrée. Ces options ne changent ni
la taille de sortie, ni les horloges, ni les paramètres radio. Ne pas comparer
deux résultats en omettant ce changement de configuration.

L'option --locked-hypotheses accepte 1, 2 ou 3; la valeur par défaut est 1.
Elle limite le nombre de candidats de synchronisation soumis au décodage
après verrouillage de la liaison. L'acquisition initiale et la recherche après
perte de synchronisation conservent toujours la limite de trois candidats.
Un candidat peut demander deux essais RS, un par profil. Réduire cette option
peut diminuer le temps de décodage des candidats rejetés, mais supprime aussi
des possibilités de récupération. La capture, la démodulation, le filtrage et
la recherche des candidats restent présents : leur coût n'est pas supprimé.

Exemple de compilation pour mesurer ce compromis, sans flasher :

  python3 build.py --name RADIO_V2_HYP1 --locked-hypotheses 1

Ce réglage expérimental ne constitue pas un gain de débit validé. Comparer
les temps DSP, les pertes et les retransmissions à configuration égale.

--raw-candidate-order 1, activé par défaut, classe les candidats retenus après
réajustement sur les mêmes échantillons bruts que le décodeur de symboles.
Le budget d'essais FEC est ensuite appliqué à ce classement. Les syndromes RS
communs aux deux profils sont calculés une seule fois; les vérifications
après correction et les CRC restent obligatoires.

Les options --payload 192 et --parity 32 décrivent la référence et doivent rester
ainsi. Le passage à 208/16 est décidé à l'exécution, pas en recompilant ces deux
options. Conserver également les réglages PHY, TDD, UART et les gardes par
défaut pour comparer des versions sur la même base.

build.py ne programme les appareils que si --flash est explicitement ajouté
avec deux ports distincts --port-a et --port-b. Une compilation seule ne
modifie donc pas les firmwares installés. Programmer les deux rôles issus du
même build avant un test v2, en conservant leur correspondance MAC/port.


7. Banc des deux appareils

Les commandes suivantes ouvrent les deux ports et transfèrent des données;
elles sont à exécuter une fois les deux firmwares correspondants installés.
Le programme utilise le XON/XOFF du système par défaut. Pour le banc dans la
VM de ce projet, utiliser --software-flow --usb-prefetch 64 avec sudo.
Le backend Linux x86-64 prend temporairement l'interface CH340 vérifiée,
prépare 64 lectures USB de 32 octets, puis rétablit ch341 à la fermeture.
Le lecteur traite les contrôles standard XON/XOFF; chaque écriture contient
au plus 64 octets. L'attente USB est bornée à 5 secondes ou au délai plus court
de l'appelant. Un timeout reste un échec, sans renvoi de données incertaines.
Fermer les ponts PTY avant ces commandes. Aucun protocole privé n'est ajouté.

Test courant : les deux sens, duplex, pause du destinataire et interaction :

  sudo python3 bench_transparent.py --port-a "$PORT_A" --port-b "$PORT_B" \
    --software-flow --usb-prefetch 64 --baud 115200 --bytes 65536 --timeout 180 \
    --output tests/radio_v2.json

Test optionnel des 256 valeurs binaires, avec échappement dans l'application :

  sudo python3 bench_transparent.py --port-a "$PORT_A" --port-b "$PORT_B" \
    --software-flow --usb-prefetch 64 --baud 115200 --escaped-binary --pattern random --bytes 65536 \
    --timeout 180 --output tests/radio_v2_binary.json

Comparer un flux compressible dans les deux sens :

  sudo python3 bench_transparent.py --port-a "$PORT_A" --port-b "$PORT_B" \
    --software-flow --usb-prefetch 64 --baud 115200 --pattern text --cases duplex --bytes 65536 \
    --timeout 180 --output tests/radio_v2_text.json

--pattern repeat propose un autre flux compressible. Pour comparer des builds,
garder la même taille, le même motif, la même graine et le même environnement.
Noter aussi le mode de contrôle de flux. Un essai --software-flow ne valide
pas le pilote du port physique ordinaire. Pour tester ce dernier séparément,
omettre --software-flow ET --usb-prefetch et conserver un résultat distinct.

Le JSON indique les octets attendus/reçus, les SHA-256, les écarts, les délais
et le résultat de chaque cas. Un échec arrête la série et conserve des éléments
de diagnostic. Le journal détaillé porte le suffixe .jsonl. Vérifier le champ
global ok et le code de sortie; l'absence de message UART n'est pas une preuve
de réussite. Le cas backpressure vérifie une pause XOFF puis une reprise XON.


8. Diagnostics et limites de la version

Les diagnostics sont sélectionnés à la compilation et stockés dans la mémoire
RTC. Ils n'impriment pas de JSON ni de messages sur le port de données.

Exemple de build avec capture RTC et instantané différé :

  python3 build.py --name RADIO_V2_DIAG --rtc-diagnostics 1 --diag-after-ms 60000

La capture différée a lieu lorsque le programme dispose d'une marge; ce n'est
pas une interruption garantie exactement à 60 secondes. Les compteurs couvrent
notamment UART, compression, promotions/replis, fragments, retransmissions,
resynchronisations, échecs et durées maximales. Le mode diagnostic n'est pas une
commande activable en envoyant du texte sur l'UART.
Le premier instantané est conservé : un défaut antérieur empêche la capture
différée, et une capture différée acquise ne sera pas écrasée par un défaut
ultérieur provoqué pendant une lecture ou un redémarrage.

read_rtc.py décode un dump RTC déjà acquis, de 256 ou 384 octets :

  python3 read_rtc.py chemin/rtc_A.bin chemin/rtc_B.bin

Cet outil n'acquiert pas lui-même la mémoire de l'ESP. Une lecture matérielle
du RTC appartient à la procédure de banc et peut interrompre la session : ne
pas la lancer pendant un transfert à mesurer.

Les options --drop-every, --rx-blackout-at-ms et --rx-blackout-ms servent à
l'injection de pertes pour les essais. Elles doivent rester à zéro dans un
build d'usage normal. --uart-echo est un firmware de diagnostic local UART,
pas le modem radio à utiliser pour une liaison A/B.

La consommation de RAM et d'IRAM dépend du build, du cache de compression et
de la taille d'entrée choisie. Vérifier les journaux de la compilation utilisée;
les valeurs d'un ancien build ne décrivent pas nécessairement le suivant.
La taille statique ne mesure ni le heap libre ni la pile au pire instant
d'exécution. L'option d'entrée 8192 ajoute notamment 6144 octets statiques.

L'UART est volontairement mis au repos pendant certaines opérations RF. Les
temps de traitement, les retransmissions et ces périodes de silence limitent
le débit livré au PC. Plus d'octets radio ou un débit de modulation supérieur
ne suffisent pas à garantir un débit utilisateur supérieur.

Les pertes précédentes ont été reproduites sans radio par un écho UART, puis
observées dès les complétions USB dans la VM. Les lectures USB préparées ont
permis les essais longs exacts décrits dans RAPPORT_TESTS.txt. Cela ne change
pas le petit buffer physique du CH340 ni tous les pilotes des autres systèmes.
Un ACK radio confirme l'admission dans la file UART, pas la lecture par le PC.
Des retards réseau arbitraires ne peuvent donc pas être garantis sans limite.

Pour les conclusions de validation, consulter RAPPORT_TESTS.txt et les JSON
auxquels il renvoie. Cette version n'annonce pas encore de profils 8/16-FSK,
de duplex simultané, de changement automatique de canal ni de débit garanti.
