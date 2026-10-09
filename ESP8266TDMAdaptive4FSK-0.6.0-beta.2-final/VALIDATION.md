# Validation De La Livraison 4-FSK 0.6.0-beta.2

## Perimetre

Cette finalisation porte sur la documentation, la provenance et l'integrite
du paquet. **Aucune nouvelle compilation, aucun nouveau flash, aucune
ouverture serie et aucun nouvel essai RF** n'ont ete effectues pour elle.
Les resultats ci-dessous proviennent de la construction et de l'essai beta.2
deja enregistres le 8 octobre 2026. Les nouvelles verifications sont uniquement
des controles locaux de fichiers et une relecture des payloads enregistres.

Les sources, exemples, binaires, scripts de construction et tests sont
conserves octet pour octet. Le code de modulation reste 4-FSK, sans dithering,
avec des fenetres fixes d'une seconde. La version technique reste beta.2.

## Construction Et Tests Existants

Source : [build.log](validation/build.log), suite [tests/](tests/README.md).

| Verification sur hote | Resultat enregistre |
| --- | --- |
| CRC16, Hamming, entrelacement | PASS |
| 4-FSK : 25 292 decodages, deux orientations, tailles et bourrage | PASS |
| Signaux synthetiques : huit periodes, phases, bruit et jitter | PASS |
| ACK, identites de session, retransmissions et doublons | PASS |
| Temporisations du controleur, plafonds et debordement de l'horloge | PASS |
| 54 cas de repli adjacent du TDM | PASS |
| 18 sequences simulees de probes repetes de 300 s | PASS |
| Panne totale, reprise, files, calibration, pause/reprise | PASS |
| Confirmations perdues et redemarrage ; absence de chevauchement TX simule | PASS |
| Approximation de phase entiere | PASS, erreur maximale environ 4,2 degres |
| Journalisation sur hote | PASS ; ne prouve pas l'absence de perte UART sur carte |
| AddressSanitizer / UndefinedBehaviorSanitizer des nouveaux tests rate/TDM | PASS |
| Compilation des roles A et B | PASS |

Cible : `esp8266:esp8266:d1_mini:xtal=160,eesz=4M`, coeur ESP8266 **3.1.2**.
Empreinte de construction : RAM 37 160 / 80 192 octets ; IRAM cache inclus
60 431 / 65 536 octets ; code flash 260 944 / 1 048 576 octets.
Chaque fichier binaire occupe **296 832 octets**. L'occupation IRAM n'est pas
un pourcentage d'utilisation CPU.

## Flash Precedent

| Carte | Role | Port observe | MAC | Verification du flash |
| --- | --- | --- | --- | --- |
| A | Maitre | `/dev/ttyUSB1` | `b4:e6:2d:23:0e:f4` | Hash of data verified |
| B | Suiveur | `/dev/ttyUSB0` | `68:c6:3a:d6:0e:e8` | Hash of data verified |

Journaux : [A](validation/flash-A.log), [B](validation/flash-B.log).

SHA-256 de `firmware/TDM_A.ino.bin` :

```text
9c4198cbec8e29e5513c4df5b0b8441774916c2eb0ce61d2062c9b39a70bb6e7
```

SHA-256 de `firmware/TDM_B.ino.bin` :

```text
85bac7f70f98271be1601dc2646f6694b9bb761b90b766061bde828ee9bcf916
```

Ces memes fichiers sont livres sans reconstruction. Cette constatation decrit
le dernier flash enregistre, pas une nouvelle lecture de la memoire des ESP.

## Essai Court Existant

Sources : [smoke.json](validation/smoke.json), [smoke.log](validation/smoke.log).
Capture du **2026-10-08 a 20:42:32 -04:00**, duree totale **63,621 s** :
environ 1,617 s d'identification, 60 s de collecte et 2 s de collecte finale.
Resultat du script : `passed=true`, aucune erreur bloquante.

| Observation | A | B |
| --- | ---: | ---: |
| Payloads verifies independamment dans le journal | 74 recus de B | 99 recus de A |
| Dernier etat TDM | LOCKED | LOCKED |
| Dernier profil TDM | 5 | 5 |
| Periode symbole | 300 us | 300 us |
| Compteur local delivered | 80 | 99 |
| Compteur local acknowledged | 98 | 88 |
| Retransmissions | 4 | 20 |
| Fenetres manquees | 1 | 0 |
| Echecs TX | 0 | 0 |
| Montees / descentes / reprises | 5 / 0 / 0 | 5 / 0 / 0 |
| Derniere fenetre : trames / octets | 8 / 112 | 8 / 112 |
| Dernier logDrop | 3 | 0 |
| Dernier IQ35 estimatesPerSec | 35 648 | 35 729 |

Les compteurs TDM ont ete imprimes aux temps locaux 55 389 ms et 57 398 ms ;
les autres diagnostics ont leurs propres instants. Ils ne constituent pas
une mesure simultanee A/B. L'ecart entre ACK d'un cote et RX de l'autre ne
doit donc pas etre interprete directement comme un nombre de paquets perdus.

**173 payloads de 14 octets, soit 2 422 octets**, ont ete verifies contre le
motif attendu et leur identifiant de sequence. Cette verification est
refaite hors ligne lors de la finalisation. A annonce 80 livraisons mais
seulement 74 lignes exploitables sont disponibles : six livraisons ne sont
pas controlees independamment dans cette capture. Une ligne serie fusionnee
est signalee par le collecteur, et `logDrop=3` sur A confirme que les logs
ne sont pas exhaustifs. Le compteur de pertes de logs ne se convertit pas
directement en nombre de payloads absents.

Les criteres de l'essai court ont reussi : les deux roles sont identifies,
les deux liens sont LOCKED, au moins cinq payloads et cinq ACK sont observes
par sens, les fenetres restent a 1 s sans remplissage final, aucun echec TX
ni redemarrage supplementaire n'est observe. Deux messages de boot par carte
etaient attendus a cause de l'ouverture serie et du reset explicite.

**Le profil 5 n'a pas ete tenu pendant 60 s de plateau dans cet essai.**
Au controle des criteres, les deux cartes etaient encore au profil 4 ;
le profil 5 apparait dans la collecte finale. On confirme qu'il a ete atteint,
pas qu'un debit utile constant de 56 octets/s par sens a ete mesure.

## Ce Qui Reste Non Qualifie

- Endurance RF prolongee au profil 5 et aux profils 6/7.
- Replis et temporisations en presence de pertes radio provoquees sur les
  deux vrais modules : les compteurs down/recovery sont restes a zero dans
  l'essai court. Les preuves de ces scenarios sont actuellement simulees.
- Fiabilite a toutes distances, saturation a courte distance, variations
  de gain, temperature, alimentation et environnement radio.
- Bande occupee, exactitude des frequences RF, conformite reglementaire et
  immunite aux interferences. Un spectre visuellement clair ne les certifie pas.
- Observabilite serie exhaustive sous charge.
- Garantie de latence, de debit applicatif soutenu ou d'execution persistante
  exactement une fois apres redemarrage.

La requalification anterieure de la beta.1 comportait des limites de plateau
rapide, d'ecoulement des files et de capture serie. Elle concernait un autre
firmware ; elle n'est ni une preuve de succes ni une mesure equivalente de
cette beta.2. De meme, les resultats de la 0.5.1 ne certifient pas la 4-FSK.

## Integrite De La Finalisation

[finalization-audit.json](validation/finalization-audit.json) contient le
controle des fichiers conserves, des binaires et des payloads deja captures.
Il ne contient aucun nouvel essai radio. [RELEASE.json](RELEASE.json) decrit
la politique de livraison et les empreintes de provenance.

Le manifeste [SHA256SUMS.txt](SHA256SUMS.txt) couvre tous les fichiers du
nouveau paquet, hors lui-meme. L'archive ZIP possede aussi une empreinte
exterieure. Ces sommes detectent une modification accidentelle ; ce ne sont
pas des signatures cryptographiques d'un editeur.

Le README, le manifeste, les metadonnees et le rapport bref originaux sont
conserves dans `validation/provenance-beta2/`. Leurs chemins relatifs et
empreintes concernent **l'ancien paquet beta.2**, pas le nouveau document
README ni le nouveau manifeste. Les anciens paquets ne sont pas modifies.
