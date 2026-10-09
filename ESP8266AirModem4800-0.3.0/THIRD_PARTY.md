# Provenance

Distribution GPL-3.0-or-later (`LICENSE`). Les fichiers repris conservent
leurs licences. Ce nouveau projet ne modifie pas les anciens projets sur la VM.

- Base utilisateur : ESP8266TDMAdaptive4FSK 0.6.0-beta.2-final,
  `/home/kaboom/ESP8266TDMAdaptive4FSK-0.6.0-beta.2`, licence MIT dans
  `licenses/BASE-MIT.txt`. Les pilotes `RH*` et helpers `Fsk*.h` en proviennent,
  via la revision locale ESP8266DMR24 0.1.0 rf-fix.
- Radio : adaptation du pilote et du traitement IQ de cette revision locale.
  La generation fractionnaire reprend le principe `ditherBurst` du projet
  [SP8ESA](https://github.com/SP8ESA/ESP8266_2.4GHz_SSB_TRX), revision
  `a5bc0f3dcf3f2ef9fabe61e0bdf4ad8158166b3e`, licence MIT conservee dans
  `licenses/SP8ESA-MIT.txt`.
- Aucun codec DMR MMDVM ni codec voix n'est utilise dans le nouveau firmware.
  `licenses/MMDVM-GPL2.txt` accompagne les binaires historiques de recuperation,
  pas une dependance du modem courant. Leur source complete est dans
  l'archive ESP8266DMR24 rf-fix conservee a cote de cette livraison.
- Arduino core ESP8266 3.1.2 et son SDK restent des dependances de compilation.
  Python pyserial et Linux pppd sont des dependances externes, non vendues
  comme composants originaux de ce projet.

Specifications consultees, non redistribuees :

- [PPP async HDLC, RFC 1662](https://www.rfc-editor.org/rfc/rfc1662.html).
- [pppd upstream](https://github.com/ppp-project/ppp/blob/master/pppd/pppd.8).
- [DCB Windows](https://learn.microsoft.com/en-us/windows/win32/api/winbase/ns-winbase-dcb).
- [Reset ESP8266](https://docs.espressif.com/projects/esptool/en/latest/esp8266/advanced-topics/boot-mode-selection.html).

Ce modem n'implante pas les normes telephoniques V.22bis/V.42 ou le protocole
DMR ETSI. Les commandes AT sont un sous-ensemble compatible dans leur syntaxe.
