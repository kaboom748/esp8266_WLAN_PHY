# Validation 0.3.0 - 2026-10-09

## Firmware et cartes

Compilation avec Arduino ESP8266 core 3.1.2, d1_mini, CPU 160 MHz, flash 4M.
Deux images de 296416 octets. Ecriture et hash verifies par esptool lors du flash.

| Role radio | MAC ESP verifiee avant flash | Port VM lors des essais |
| --- | --- | --- |
| A | b4:e6:2d:23:0e:f4 | /dev/ttyUSB0 |
| B | 68:c6:3a:d6:0e:e8 | /dev/ttyUSB1 |

Ces numeros de port ne sont pas des identifiants permanents. Verifier les cartes
avant un prochain flash. Les empreintes des binaires livres correspondent aux
images de construction sur la VM, qui ont ete utilisees pour le flash.

- A : 21ad1313c3641cb08226a1ad79dd9ef78f5921b2d903514d337cff798b1322ca
- B : e019988ff50ceb775b35a053bff476f7646f8702ef801a6f60fdeeaeebe58a0d

Sources des preuves : validation/flash-A.log, flash-B.log, flash.json,
devices-before-flash.json et firmware-sha256.json.

RAM globale : 61304/80192 octets. IRAM + cache : 61431/65536 octets.
Cette marge IRAM est faible ; ne pas ajouter du code radio sans recompiler.

## Tests unitaires et PHY

Execution sur la VM, compilateur g++, ASan/UBSan, -Wall -Wextra -Werror :

- Connexion radio automatique sans octet serie ni commande AT.
- AT et +++ transportes comme donnees ; absence de messages locaux.
- 12 dialogues CLIENT/CLIENTSERVER dans une seule session radio.
- XON/XOFF intercales dans CLIENT, pauses simultanees, reprise au milieu du mot.
- Correspondant absent pendant 150 secondes : conservation du CLIENT initial
  a travers plusieurs echeances de connexion, pas de XOFF premature.
- Tampon hors connexion, seuils de flux et octets deja en transit.
- Pertes, corruptions, duplications et pause de 25 secondes :
  20180 et 16139 octets echappes restitues exactement.
- Rupture radio, reconnexion, reset du correspondant, pause conservee jusqu'a XON,
  debordement, erreur UART et rejet d'un ancien appel radio de type AT.
- CRC, rebouclage des numeros de sequence et de l'horloge 32 bits.
- 54 cas IQ de PHY, decalage frequentiel, inversion, bruit et synchronisation.
- 5 tests Python du controle de flux en espace utilisateur.

Resultats complets : validation/build.log.

## Essais physiques USB/UART et radio

Aucun raccourci logiciel ne transfere les donnees entre les deux ports.
Chemin : PC Linux -> USB via VirtualHere -> ESP A -> radio -> ESP B -> USB via
VirtualHere -> PC Linux. Le programme de test joue les deux applications PC.

Resultat final : PASS, validation/hardware-transparent.json.

- Echange des premiers messages sans CLIENT ni AT prealable.
- AT/ATH/ATZ/ATDT/+++ transportes, sans execution ni echo local.
- 6 dialogues alternant les roles PC client/serveur, puis 2 apres reouverture.
- Aucune reponse CLIENTSERVER tant que l'application serveur ne l'a pas emise.
- XOFF au milieu de CLIENTSERVER : CLIE recu, pause de 4 secondes, puis
  NTSERVER recu exactement apres XON.
- XON/XOFF intercales dans CLIENT : le correspondant recoit exactement CLIENT.
- Transfert simultane : 4152 octets A -> B et 4151 B -> A, identiques aux sources,
  hashes SHA-256 verifies, avec pause de reception de 8 secondes sur B.
- Duree du transfert : 25.321 secondes, pause incluse.

## PPP reel sur la radio

Resultat : PASS, validation/hardware-ppp.json. Deux instances reelles de pppd
dans deux espaces reseau Linux isoles ; aucune liaison IP de secours entre eux.

- Dialogue CLIENT/CLIENTSERVER genere uniquement par les applications PC.
- IPCP : 192.0.2.1 et 192.0.2.2, MTU 296.
- ICMP : 3 paquets emis, 3 recus, 0 % de perte sur cet essai.
- RTT observe : environ 1.60 a 1.98 seconde.
- TCP : 4096 octets verifies par SHA-256 dans chaque sens.
- Controle logiciel utilise : espace utilisateur Linux, avec XOFF/XON
  effectivement observes dans les deux directions.
- Duree totale : environ 89.75 secondes.

Ceci valide ce petit essai Linux, pas un debit TCP soutenu garanti ni Windows RAS.

## Reset USB observe et premier essai

Le premier test physique a echoue uniquement sur l'hypothese qu'une reouverture
de COM n'emettrait aucun octet. Les tests radio, de flux et d'integrite de cette
premiere execution avaient deja reussi. Preuves conservees sans les remplacer :
validation/hardware-first-run.json et hardware-first-run.log.

Une capture distincte a 74880 bauds a ensuite montre sur les deux cartes :
message ROM de demarrage, rst cause:2. Voir validation/reopen-boot-probe.log.
L'ouverture/fermeture via le CH340/VirtualHere declenche donc un redemarrage dans
ce banc. Le reglage RTS/DTR a false dans pyserial n'a pas empeche ce comportement.

Le test final attend le demarrage et journalise les octets recus avant de tester
l'application. Il ne revendique pas une reouverture sans reset. Apres ce demarrage,
les deux dialogues de reconnexion passent. Le bruit de boot n'est pas un resultat AT
et ne peut pas etre supprime par le retrait du parseur AT de l'application.

## Outils Linux et radio simulee

Resultat : PASS, validation/cable-ppp-simulation.json. Cette fois les vrais outils
tools/modem_port.py jouent le client et le serveur avec deux pppd, deux PTY et les
coeurs C++ du firmware. La radio est simulee, avec des defauts injectes.

IPCP, 3 pings sur 3 et 4096 octets TCP exacts dans chaque sens ont reussi.
27 retransmissions par role, aucun debordement. Duree : environ 86.38 secondes.
Ce test est complementaire a l'essai RF physique et ne le remplace pas.

## Windows

validation/windows-inf.json : 77 assertions PASS via Windows SetupAPI et
BuildCommDCBW. Verification du DCB 4800/8N1, XON/XOFF, absence de controle
materiel, limites de vitesse, commandes de cable et absence de commandes AT.

**Non teste** : installation du nouveau profil, acceptation de sa signature
(il est non signe), sequence executee par Unimodem, negotiation PPP Windows,
rearmement des Connexions entrantes et absence de blocage RasMan/RemoteAccess.

Les tests d'API Windows sont en lecture seule. Aucun service ni peripherique du
PC Windows n'a ete modifie. Le nouveau profil doit remplacer l'ancien AT sur
les connexions concernees ; voir windows/README.md.

## Fin des essais

Les programmes serie et pppd ont ete arretes ; les espaces reseau temporaires ont
ete supprimes. Aucun processus ne tenait les deux ports serie a la verification.
Les deux appareils ont ete relaches par STOP USING dans VirtualHere ; ils etaient
disponibles pour les PC Windows. La webcam n'a pas ete touchee.
Preuve : validation/virtualhere-release.json.
