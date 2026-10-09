# ESP8266AirModem4800 0.3.0 - cable radio transparent

Version du 2026-10-09. Les deux ESP ont ete flashes et testes par radio via
VirtualHere. La version 0.2.0 AT + cable est conservee separement.

## Ce qui change

- Aucun interpreteur AT, echo, resultat OK/CONNECT/RING/NO CARRIER ou echappement +++.
- Transport serie actif des le demarrage de l'application, sans CLIENT ni AT requis.
- Liaison radio et tentatives de reconnexion automatiques, meme sans donnees serie.
- CLIENT et CLIENTSERVER sont transportes par radio, pas reconnus ni generes par les ESP.
- AT, ATH et +++ sont maintenant des donnees ordinaires. Ne pas les utiliser pour configurer le firmware.
- Meme logique sur les deux cartes ; A et B restent deux roles de synchronisation radio.
  N'importe quel PC peut etre client ou serveur, independamment du role A/B.

L'UART reste a **4800 bit/s, 8N1, XON/XOFF**, sans controle RTS/CTS.
Les binaires A/B ne doivent pas etre intervertis avec deux images du meme role.

## Windows : changer de profil

L'ancien profil **AirModem4800 AT - 4800 bps XON-XOFF** ne fonctionne plus
avec cette version : les ESP ne repondent plus aux commandes du pilote AT.

Utiliser le profil fourni dans `windows/airmodem-transparent.inf` :

**AirModem4800 Transparent 0.3 - 4800 bps XON-XOFF**

C'est un profil de cable direct, pas un nouveau pilote USB. Le PC client envoie
CLIENT ; le PC serveur envoie CLIENTSERVER. Voir `windows/README.md` pour les
precautions d'installation, de signature et de validation native RAS.

Ne pas employer le profil generique cable a 19200 bauds : le firmware reste a 4800.
Fermer PuTTY et les moniteurs qui occupent le COM avant de lancer RAS.
Les deux appareils ont ete relaches dans VirtualHere a la fin des essais.

## XON/XOFF et transparence

CLIENT contient `43 4c 49 45 4e 54` ; CLIENTSERVER ajoute `53 45 52 56 45 52`.
Ces mots ne contiennent ni XON (11 hexadecimal) ni XOFF (13 hexadecimal).
Il n'y a donc pas de collision entre leurs caracteres et le controle de flux.
Une pause peut retarder le mot, mais ne doit pas en supprimer des lettres.

**Exception a la transparence : les octets bruts 0x11 et 0x13 sont des controles
locaux, pas des donnees radio.** Pour transporter leurs valeurs dans PPP, les
PC doivent les echapper dans les deux sens : 0x11 -> 7d 31, 0x13 -> 7d 33.
Les ESP ne font pas le decodage PPP. Cela suit la
[RFC 1662, section 4.2](https://www.rfc-editor.org/rfc/rfc1662.html#section-4.2).

Un XOFF envoye par le PC bloque les donnees ESP -> PC jusqu'au prochain XON.
Il n'existe pas de minuterie qui force une reprise contre la demande du PC.
Les controles locaux peuvent toujours passer pendant cette pause.
Le firmware repete son propre etat XON/XOFF chaque seconde.

Le seuil de pause des donnees PC -> radio est 512 octets en attente ; la reprise
se fait a 256. Le tampon avant connexion contient 1024 octets. Une petite requete
CLIENT reste acceptee si le correspondant radio est encore absent. Les octets
avant la premiere connexion survivent aux nouvelles tentatives radio.

## Limites a connaitre

- Le CRC32, les acquittements, les retransmissions et les credits radio sont conserves.
  Ils ne garantissent pas l'absence de pertes sur un pilote USB ou un PC qui ignore XOFF.
- Une rupture definitive de session radio, un debordement ou une erreur UART peuvent
  interrompre le flux et purger des donnees. PPP/TCP doivent detecter et recuperer
  l'erreur ; le firmware n'injecte plus de message textuel d'erreur dans le flux.
- Fermer le COM n'est pas un signal de raccrochage pour le firmware. Sans reset ni
  rupture radio, les tampons ne sont pas purges simplement parce que le PC ferme
  son application. Un PC qui avait envoye XOFF doit envoyer XON a sa reprise.
- **Reset a l'ouverture observe sur les deux CH340 via VirtualHere** : la ROM emet
  son message a 74880 bauds, illisible a 4800. Ce message precede l'application ;
  supprimer AT ne le supprime pas. Les essais attendent 3 secondes apres ouverture
  et consignent ces octets avant de tester les donnees. Windows peut avoir besoin
  d'une nouvelle tentative apres le demarrage. Voir le rapport de validation.
- 4800 bit/s est le debit UART, pas le debit utile garanti. La PHY 4-FSK reste a
  9600 bit/s bruts. Avec 52 octets utiles par cycle radio de 280 ms dans chaque sens,
  le plafond est environ 186 octets/s/sens avant PPP, pertes et retransmissions.
- Ce transport radio experimental n'est ni du DMR certifie ni un modem telephonique V.22bis.
- Les tests Linux ne constituent pas une preuve de fonctionnement de Windows RAS
  ni de disparition du blocage RasMan/RemoteAccess.

## Linux

Dependances : Python 3, pyserial, pppd. `tools/requirements.txt` indique la dependance
Python. Le helper applique XON/XOFF en espace utilisateur et limite ses ecritures ;
il evite de dependre uniquement du controle de flux du pilote USB.

Exemple de laboratoire isole, sans authentification, une commande sur chaque PC :

```sh
# PC serveur : choisir son propre port.
sudo python3 tools/modem_port.py --port /dev/ttyUSB0 --cable-server --ppp noauth nodefaultroute noipv6 noccp novj 192.0.2.2:192.0.2.1
# PC client : choisir son propre port.
sudo python3 tools/modem_port.py --port /dev/ttyUSB0 --cable-client --ppp noauth nodefaultroute noipv6 noccp novj 192.0.2.1:192.0.2.2
```

Ces options noauth sont reservees au laboratoire ; ne pas les deployer sur une
liaison donnant acces a un reseau sans configurer authentification et filtrage.
Le helper ajoute `asyncmap a0000 escape 11,13 mtu 296 mru 296` et les echos LCP.
Le serveur du helper gere **une session** ; relancer le programme apres sa fin.
Il ne faut pas confondre le service d'ecoute du PC avec la radio toujours active.

## Compilation et preuves

Outil utilise : Arduino ESP8266 core 3.1.2, d1_mini, CPU 160 MHz, flash 4M.

```sh
sh build.sh
# Tests C++ avec ASan/UBSan, PHY et controle de flux Python :
sh tests/run_host_tests.sh
```

Binaires effectivement flashes : `firmware/Modem_A.ino.bin` et `firmware/Modem_B.ino.bin`.
`VALIDATION.md` distingue les essais physiques, simules et les points non verifies.
Les scripts `tests/hardware_transparent.py` et `tests/hardware_ppp.py` ouvrent les
vrais ports ; ne les lancer que lorsque les deux ESP sont disponibles.
Les journaux et SHA-256 sont dans `validation/`. Licences : `LICENSE` et `THIRD_PARTY.md`.
