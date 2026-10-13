# ESP8266 Modem300 FEC-ARQ

**Version de reference : `v1.0.0-reference` du 13 octobre 2026.**

Modem serie binaire nominal **300 bauds, 8N1, sans controle de flux**, sur deux ESP8266 a 160 MHz. La liaison radio ajoute Reed-Solomon, CRC32, calibration numerique continue et au plus trois retransmissions. Une echeance depassee provoque un abandon, pas un allongement de la latence.

Cette livraison fige et documente le firmware teste `release_uart_wait`. Elle **ne modifie pas les binaires**, ne les reflashe pas et ne reprend aucun peripherique VirtualHere. Il s'agit d'une reference validee sur le banc, **pas d'une certification electrique, de portee ou reglementaire**.

## Documentation

- [Note technique : fonctionnement, canaux, protocole et timings](NOTE_TECHNIQUE.md)
- [Schema PNG des trames et des timings](TRAMES_ET_TIMINGS.png)
- [Rapport des essais](RAPPORT.txt)
- [Parametres et identite de la livraison](RELEASE.json)
- [Mesures brutes et index des essais](mesures/INDEX.txt)
- [Provenance et contraintes de reconstruction](SOURCE.txt)

![Trames et timings du modem](TRAMES_ET_TIMINGS.png)

## Deux canaux, mais une seule emission a la fois

| Sens | Canal | Frequence centrale nominale |
|---|---:|---:|
| A vers B | 3 | 2422 MHz |
| B vers A | 11 | 2462 MHz |

Les emissions alternent a 4,5 ms d'intervalle. Un cycle complet dure 9 ms. Ce n'est ni du Wi-Fi 802.11 transporte, ni deux emissions simultanees, ni un changement de canal a chaque retry. Les deux flux UART peuvent cependant fonctionner en meme temps.

## Resultat de reference

Le dernier essai des binaires persistants a transmis **12 870 octets**, avec identite complete des quatre flux : A vers B, B vers A, et les deux sens simultanes. Aucune perte, modification, duplication ou inversion d'ordre sur cet essai d'environ six minutes.

L'essai instrumente precedent a conserve une perte sur 12 870 octets. Les deux resultats sont livres, sans selection ni addition trompeuse. La borne demandee de **66,66 ms du premier bit d'entree au premier bit de sortie reste a mesurer sur les fils**.

## Contenu

| Chemin | Contenu |
|---|---|
| `firmware/FastA`, `firmware/FastB` | Sketches autonomes exacts des deux roles |
| `firmware/build_A`, `firmware/build_B` | Binaires testes et verifies au flash |
| `firmware/*.log` | Logs de compilation et de flash |
| `fast.h`, `backend_seed.ino`, `generate.py` | Implementation actuelle et generation |
| `build_flash.py` | Construction et flash optionnel des deux roles |
| `binary_test.py`, `test_*.py`, `test_codec.cpp` | Banc et tests logiciels |
| `docs/render_timing.py` | Generation reproductible du PNG, avec controles numeriques |
| `esp8266_WLAN_PHY_original_89172ce.zip` | Depot original complet, conserve intact |
| `MANIFEST.sha256` | Empreintes de tous les fichiers livres, sauf le manifeste lui-meme |

## Construire sans toucher aux cartes

Dans la VM, depuis ce dossier, avec Arduino CLI, le core ESP8266 **3.1.2**, Python 3 et g++ :

```sh
python3 build_flash.py --name rebuild_reference
```

Les valeurs par defaut sont celles de `RELEASE.json`. Le FQBN utilise est `esp8266:esp8266:d1_mini:xtal=160,eesz=4M`. Le firmware A doit etre associe au firmware B de cette meme livraison ; le format radio n'est pas compatible avec le REV7 original.

**Flash uniquement apres avoir rattache volontairement les cartes et verifie leurs ports :**

```sh
python3 build_flash.py --name rebuild_reference --flash
```

Cette commande remplace les deux images. Le script suppose A sur `/dev/ttyUSB1` et B sur `/dev/ttyUSB0`. Ces noms etaient ceux du banc ; une reconnexion USB peut les changer. Ne pas reprendre les ports VirtualHere utilises ailleurs pour simplement consulter ou reconstruire ce projet.

## Tests et schema

```sh
python3 test_binary.py
python3 test_phy.py
python3 binary_test.py --frames 128 --firmware rebuild_reference --output essai.json
python3 docs/render_timing.py
```

`test_binary.py` et le banc demandent `pyserial` ; `test_phy.py` demande `numpy`. Le generateur du schema demande `matplotlib`. Seul `binary_test.py` ouvre les deux ports. Il laisse six secondes au demarrage et n'impose aucun controle de flux. Les builds `--end-report 1` sont des diagnostics temporaires qui s'arretent et passent a 115200 bauds ; ne pas les confondre avec le modem persistant.

## Provenance

Projet d'origine : [kaboom748/esp8266_WLAN_PHY](https://github.com/kaboom748/esp8266_WLAN_PHY), dossier `300bps - FEC MODEM`, commit `89172cebb4bef19d8a1f37cc3e491dbed64c40eb`. Licence MIT conservee dans `LICENSE`. Le code original et les essais precedents restent disponibles pour comparaison.
