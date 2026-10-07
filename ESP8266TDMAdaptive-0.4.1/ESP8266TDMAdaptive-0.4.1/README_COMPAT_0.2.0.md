# ESP8266FSKRadioHeadCompat 0.2.0

Bibliotheque Arduino independante pour une liaison 2-FSK half-duplex entre ESP8266.
Interfaces principales proches de RadioHead : RH_ESP8266FSK, RHDatagram,
RHReliableDatagram, sendtoWait(), recvfromAck(), setTimeout(), setRetries().
Aucune dependance RadioHead. Le format radio est propre a cette bibliotheque.
Ne pas inclure la bibliotheque RadioHead originale dans le meme sketch :
les noms de classes de compatibilite sont identiques. La migration est au niveau
API, pas une interoperabilite radio avec les pilotes RadioHead existants.

## Installation et configuration

Installer ce dossier dans Arduino/libraries, ou installer l'archive ZIP dans Arduino.
Core utilise : ESP8266 Arduino 3.1.2, carte Wemos D1 Mini, flash 4 Mo.
Le profil principal est valide a 160 MHz. Ne pas supposer que les memes
performances sont obtenues avec le reglage Arduino par defaut a 80 MHz.
Les resultats mesures et limites sont dans VALIDATION.md.

```cpp
#include <RH_ESP8266FSK.h>
#include <RHReliableDatagram.h>
RH_ESP8266FSK driver(6);
RHReliableDatagram manager(driver, 1); // adresse 2 sur l'autre carte

void setup() {
  driver.setModemConfig(RH_ESP8266FSK::FSK_Rb333Reliable);
  manager.init();
  manager.setTimeout(driver.frameDurationUs(1) / 1000 + 500);
  manager.setRetries(5);
}
```

Les deux cartes doivent employer le meme canal, les memes codes TONE et la meme
duree de bit. Leurs seuils de reception sont calibres individuellement.
setBitUs(3000) donne exactement 3000 us par bit. setBitRate(333) arrondit la
periode a 3003 us : utiliser la meme methode des deux cotes.
Les anciens noms de profils sont conserves comme alias, mais n'indiquaient pas
leur debit reel. Preferer les nouveaux noms.

| Profil | Duree du bit | Debit de symboles |
| --- | ---: | ---: |
| FSK_Rb250Reliable | 4000 us | 250/s |
| FSK_Rb333Reliable | 3000 us | 333,33/s |
| FSK_Rb500Experimental | 2000 us | 500/s |
| FSK_Rb667Experimental | 1500 us | 666,67/s |
| FSK_Rb1000Experimental | 1000 us | 1000/s |

Les noms ne certifient pas les debits. Seuls les essais enumeres dans
VALIDATION.md ont ete realises sur cette version.

## Trame et fiabilite

Canal 6; TONE 16 et 1016; APWR=0; ASK=0 par defaut.
L'estimateur I/Q existant reste en MODE=0, champ N=4, avec huit acquisitions
courtes par estimation de frequence. E4 ne sert pas de decodeur FSK.
Les centres de frequence sont appris a chaque reception : aucun seuil absolu
propre a une carte n'est impose. La synchronisation determine aussi la polarite.

Une trame comprend deux tonalites de calibration de 40 ms, 36 bits alternes,
un marqueur de 12 bits, la longueur codee Hamming(7,4), puis les en-tetes,
la charge utile et un CRC16-CCITT-FALSE. Les donnees sont entrelacees par blocs
de quatre octets (huit mots Hamming), en-tetes et CRC compris.
Une rafale de huit erreurs consecutives dans un bloc entrelace est corrigeable
si chaque mot Hamming ne contient pas d'autre erreur.
Le CRC reste obligatoire; la correction Hamming seule ne valide jamais un paquet.

Charge utile : 0 a 32 octets. Une seule trame en attente en reception.
Les ACK sont de vrais paquets : adresse source, destination, identifiant,
drapeau ACK, contenu et CRC verifies. Un timeout donne false. Un retry
deja livre provoque un nouvel ACK sans nouvelle livraison au sketch.
Un broadcast n'est pas acquitte. La deduplication utilise des identifiants de
8 bits et un etat en RAM; elle ne constitue pas une garantie transactionnelle
apres redemarrage ou rebouclage des identifiants. Conserver setTxRepeats(1) avec le gestionnaire
fiable : ses retries gerent les repetitions et les fenetres half-duplex.

Le chemin RF est restaure apres chaque emission : gate coupee, horloge TX
desactivee et registres PBUS sauvegardes/restaures. Une garde RX de 120 ms
evite les mesures transitoires de sa propre emission. Elle ne coupe pas
les bits d'une trame. Les exemples et le banc espacent les essais de 1500 ms.

## CPU et integration

driver.send() prepare le paquet et demarre une emission cooperative.
Appeler driver.poll() regulierement dans loop(), y compris pendant TX.
Le reglage initial du PHY lors de send() prend encore environ 80 ms.
Le reste de l'emission ne fait pas d'attente active de plusieurs secondes.
Un retard TX excessif detecte au prochain poll() coupe l'emission;
statistics().txAborts l'indique. Un loop bloque ne peut pas assurer cet arret
cooperatif immediatement.
setMode(RHModeIdle) interrompt aussi un TX et ferme sa gate.
sleep() interrompt le TX mais retourne false : l'economie d'energie materielle
n'est pas implementee. Une emission n'est lancee que par send(), jamais par
setMode(RHModeTx).

En reception, available()/recv() effectuent au plus une estimation I/Q a la fois.
La veille echantillonne toutes les 2 ms; le rythme augmente apres detection
d'une paire de tonalites. A 3000 us/bit, viser un passage dans poll()/recv()
au moins toutes les 500 us pendant une trame. Une operation longue du sketch
peut faire perdre une trame; les retries ne remplacent pas un loop reactif.

sendtoWait(), waitPacketSent() et l'emission de l'ACK par recvfromAck() restent
synchrones. Elles rendent la main au SDK, mais n'executent pas le loop utilisateur
pendant leur attente. Pour le travail utilisateur pendant une emission brute,
voir examples/raw_tx. Le gestionnaire fiable n'est pas entierement asynchrone.

Le PHY est reserve au modem : le Wi-Fi ordinaire n'est pas utilisable
simultanement. Aucun timer materiel supplementaire n'est reserve par le pilote.
Les statistiques mesurent le temps passe dans les appels RX du pilote,
pas toute la charge CPU du SDK. setPollBudgetUs() est conserve pour compatibilite;
le recepteur traite desormais une seule estimation par appel.

Les anciens ACK par simple tonalite sont desactives.
setUseAckBeacon() ne reactive pas ce chemin non adresse.
Cette version n'implemente pas encore un protocole separe de reveil/CTS :
la calibration et le preambule sont inclus dans chaque paquet.

## Tests reproductibles

Compiler puis televerser examples/link_bench sur les deux cartes. Le sketch
ne transmet rien spontanement. Le banc utilise Serial a 115200.

```sh
arduino-cli compile --fqbn esp8266:esp8266:d1_mini:xtal=160,eesz=4M examples/link_bench
arduino-cli upload -p /dev/ttyUSB0 --fqbn esp8266:esp8266:d1_mini:xtal=160,eesz=4M examples/link_bench
arduino-cli upload -p /dev/ttyUSB1 --fqbn esp8266:esp8266:d1_mini:xtal=160,eesz=4M examples/link_bench
python3 tests/hardware_bench.py --cpu 160 --bit-us 3000 --rounds 10 --output results.json
sh tests/run_host_tests.sh
```

Le banc exige un message acquitte ET une reception correspondante verifiee.
Il conserve les echecs, retries, timings, doublons et resets.
Les tests logiciels verifient les ACK errones, les limites de retries, les
debordements temporels, les doublons, le CRC et les rafales entrelacees.
Ils ne remplacent pas les essais RF de portee ou de coexistence.
