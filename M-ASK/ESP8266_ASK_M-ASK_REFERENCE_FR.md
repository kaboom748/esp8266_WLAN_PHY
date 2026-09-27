# ESP8266 PHY — Référence détaillée ASK / M-ASK

> **Statut** : référence empirique de programmation issue des mesures RF, des essais IQ_EST/E4 et des bancs TX/RX réalisés sur ESP8266.  
> **Important** : ce document n'est pas une spécification officielle Espressif. Il décrit ce qui a été observé et validé sur le banc de test.

---

## 1. Résumé pratique

| Élément | Référence actuelle |
|---|---|
| Bande RF utilisée | 2,4 GHz |
| Canal de référence | Canal 6, 2,437 GHz |
| Registre TX principal observé | `0x600005B8` |
| GATE TX | bit 18 |
| ASK / digital scale | bits 17:10 |
| tone_control | bits 9:0 |
| Codage ASK appliqué | `askCode = (-ASK) mod 256` |
| Zone ASK propre / monotone | principalement `ASK = 0..128` |
| Rupture observée | `ASK 128 -> 129` |
| 8-ASK retenu | `{70, 83, 94, 102, 108, 114, 120, 127}` |
| Bits par symbole | 3 |
| Sous-porteuse validée | jusqu'à **8 kHz** |
| 10 kHz | échec de synchronisation dans le banc actuel |
| Débit brut conservateur | **100 bit/s** |
| Plafond démontré actuel | **150 bit/s**, encore avec erreurs |
| 200 bit/s | échec de classification dans l'implémentation actuelle |
| Métrique RX | corrélation / amplitude `amp1k` ou équivalent à la fréquence cible |
| Bloc RX utilisé | IQ_EST / lecture E4 |
| Gain RX | fixe recommandé pour mesures reproductibles |

---

## 2. Architecture TX ASK

### Registre `0x600005B8`

| Bits | Nom | Fonction |
|---|---|---|
| 18 | `GATE` | Ouvre / ferme rapidement l'émission du tone |
| 17:10 | `ASK / digital scale` | Échelle numérique d'amplitude |
| 9:0 | `tone_control` | Contrôle du tone, utilisé notamment pour FSK / génération du tone |

Le champ ASK est traité comme un **code numérique d'échelle d'amplitude**, et non comme une valeur directement exprimée en dB ou en pourcentage.

### Encodage logiciel validé

```cpp
uint8_t askCode = (uint8_t)(0u - ask);
```

Puis :

```cpp
toneShadow &= ~SCALE_MASK;
toneShadow |= ((uint32_t)askCode << SCALE_SHIFT) & SCALE_MASK;
wr32(0x600005B8, toneShadow);
```

avec :

```cpp
SCALE_SHIFT = 10;
SCALE_MASK  = 0x0003FC00;
GATE_MASK   = 0x00040000;
```

---

## 3. APWR — comportement empirique

APWR n'est pas linéaire sur `0..255`.

Modèle empirique :

```text
APWR = [bits 7:6][bits 5:0]
```

Les bits 7:6 sélectionnent un régime ; les bits 5:0 assurent un réglage fin à l'intérieur du régime.

| Bits 7:6 | Plage | Comportement observé |
|---|---|---|
| `00xxxxxx` | `0x00..0x3F` | environ -31,6 dBm, quasi constant |
| `01xxxxxx` | `0x40..0x7F` | atténuation progressive |
| `10xxxxxx` | `0x80..0xBF` | régime atténué |
| `11xxxxxx` | `0xC0..0xFF` | retour à un régime fort |

### Mesures APWR de référence, ASK = 0

| APWR | Hex | Puissance observée |
|---:|---:|---:|
| 0 | 0x00 | -31,6 dBm |
| 63 | 0x3F | -31,6 dBm |
| 64 | 0x40 | -30,7 dBm |
| 65 | 0x41 | -31,1 dBm |
| 72 | 0x48 | -32,1 dBm |
| 80 | 0x50 | -33,1 dBm |
| 96 | 0x60 | -36,1 dBm |
| 127 | 0x7F | -43,1 à -44,6 dBm |
| 128 | 0x80 | -43,6 dBm |
| 192 | 0xC0 | -31,7 dBm |
| 193 | 0xC1 | -30,7 dBm |
| 224 | 0xE0 | -31,2 dBm |

### Approximation APWR dans le régime `0x40..0x7F`

Avec :

```text
F = APWR - 64
```

approximation empirique :

```text
P(dBm) ≈ -31,04 - 0,1125·F - 0,001441·F²
```

Cette formule est une approximation de banc, pas une loi garantie du silicium.

---

## 4. ASK — comportement empirique

### Mesures RF à APWR = 64

| ASK | askCode | Puissance mesurée | Spectre |
|---:|---:|---:|---|
| 0 | 0x00 | -30,7 dBm | 1 pic |
| 32 | 0xE0 | -39,3 dBm | 1 pic |
| 64 | 0xC0 | -46,3 dBm | 1 pic |
| 127 | 0x81 | -62,2 dBm | creux |
| 128 | 0x80 | -61,7 dBm | creux |
| 160 | 0x60 | -33,7 dBm | ~8 pics |
| 192 | 0x40 | -31,7 dBm | plusieurs pics |
| 224 | 0x20 | -32,1 dBm | plusieurs pics |
| 255 | 0x01 | -32,1 dBm | plusieurs pics |

### Loi empirique dans la zone utile

Pour `0 <= ASK <= 128`, avec APWR = 64 :

```text
P(dBm) ≈ -30,99 - 0,2429·ASK
```

soit environ :

```text
4,1 pas ASK ≈ 1 dB
```

### Rupture exacte observée

Le sweep détaillé a montré :

```text
ASK 128 -> régime faible
ASK 129 -> retour brutal au régime fort
```

Mesure corrélée RX :

```text
ASK 128 ≈ 148,7k
ASK 129 ≈ 476,1k
```

soit un saut d'environ `x3,2` sur la métrique de corrélation RX.

Cela correspond au passage :

```text
ASK 128 -> askCode 0x80
ASK 129 -> askCode 0x7F
```

La frontière est donc fortement corrélée au passage du bit de poids fort du code matériel.

---

## 5. Sweep ASK 0..128 — comportement RX

La métrique RX utilisée est une amplitude de corrélation de la sous-porteuse.  
**Elle n'est pas une puissance RF en dBm.**

Observations générales :

- `ASK 0..~64` : zone relativement comprimée côté RX.
- `ASK ~70..128` : pente bien plus exploitable pour M-ASK.
- `ASK 127` et `128` sont proches.
- `ASK >=129` : retour au régime fort ; non monotone globalement.

Quelques points mesurés :

| ASK | amplitude RX approx. |
|---:|---:|
| 0 | 491659 |
| 32 | 484999 |
| 64 | 476513 |
| 70 | 456144 |
| 80 | 422497 |
| 96 | 357401 |
| 104 | 307520 |
| 112 | 245819 |
| 120 | 187662 |
| 127 | 146612 |
| 128 | 147767 |

---

## 6. Jeu de niveaux 8-ASK retenu

Les niveaux ont été choisis pour obtenir des centres RX approximativement espacés.

| Symbole | Bits | ASK |
|---:|---|---:|
| 0 | `000` | 70 |
| 1 | `001` | 83 |
| 2 | `010` | 94 |
| 3 | `011` | 102 |
| 4 | `100` | 108 |
| 5 | `101` | 114 |
| 6 | `110` | 120 |
| 7 | `111` | 127 |

Exemple de centres calibrés sur un run autonome :

| Bits | ASK | Centre RX |
|---|---:|---:|
| 000 | 70 | 404493 |
| 001 | 83 | 371364 |
| 010 | 94 | 341467 |
| 011 | 102 | 304014 |
| 100 | 108 | 268425 |
| 101 | 114 | 225059 |
| 110 | 120 | 184963 |
| 111 | 127 | 149032 |

Ces valeurs absolues ne doivent pas être codées comme constantes universelles.  
Elles dépendent du montage, du gain RX, du canal RF, de la distance, de l'orientation et de la sous-porteuse.

### Décision recommandée

```text
symbole = argmin_i |A_mesuré - centre_i|
```

où `centre_i` est appris lors d'une phase de calibration.

---

## 7. RX — IQ_EST / E4

### Registres observés

| Élément | Adresse |
|---|---:|
| Contrôleur IQ_EST | `0x6000057C` |
| Résultat E4 | `0x600005E4` |
| RX CTRL | `0x60009B08` |

### Champs principaux IQ_EST

| Bit(s) | Fonction |
|---|---|
| 31 | DONE |
| 18 | MODE |
| 16:2 | N |
| 1 | START |
| 0 | ENABLE |

### Séquence de mesure validée

1. RX RF ON.
2. RX clock ON.
3. Mode PBUS manuel/debug.
4. Gain RX fixe.
5. Configurer IQ_EST.
6. `START=0`, attendre `DONE=0`.
7. `START=1`, attendre `DONE=1`.
8. Lire E4.
9. Désactiver / réarmer proprement avant la mesure suivante.

E4 est une valeur de mesure **ponctuelle** et non un RSSI continu.

---

## 8. Détection de sous-porteuse

La détection robuste utilisée repose sur une corrélation sélective en fréquence :

- fréquence cible `f0`,
- deux fréquences de contrôle autour,
- moyenne glissante locale retirée,
- corrélation carrée I/Q,
- amplitude approximée par `|I| + |Q|`.

Exemple initial à 1 kHz :

```text
target = 1000 Hz
controls = 700 / 1300 Hz
```

Puis adaptation proportionnelle pour les autres sous-porteuses.

### Fenêtre RX

Pour comparer les fréquences, la règle qui a bien fonctionné est :

```text
fenêtre RX ≈ 10 périodes de sous-porteuse
```

Exemples :

| Sous-porteuse | Fenêtre RX |
|---:|---:|
| 1 kHz | 10 ms |
| 1,5 kHz | 6,666 ms |
| 2 kHz | 5 ms |
| 2,5 kHz | 4 ms |
| 3 kHz | 3,333 ms |
| 4 kHz | 2,5 ms |
| 5 kHz | 2 ms |
| 6 kHz | 1,666 ms |
| 8 kHz | 1,25 ms |

---

## 9. Résultats sous-porteuse — 8-ASK à 100 bit/s brut

Débit fixé à `30 ms/symbole = 100 bit/s brut`.

| Sous-porteuse | Fenêtre RX | Envoyés | Corrects | Couverture | Exactitude | Sync |
|---:|---:|---:|---:|---:|---:|---:|
| 1 kHz | 10000 µs | 320 | 305 | 100 % | 95,3 % | 89,3 % |
| 1,5 kHz | 6666 µs | 320 | 314 | 100 % | 98,1 % | 94,3 % |
| 2 kHz | 5000 µs | 320 | 320 | 100 % | 100,0 % | 96,7 % |
| 2,5 kHz | 4000 µs | 320 | 315 | 100 % | 98,4 % | 94,0 % |
| 3 kHz | 3333 µs | 320 | 319 | 100 % | 99,7 % | 98,9 % |
| 4 kHz | 2500 µs | 320 | 313 | 100 % | 97,8 % | 99,7 % |
| 5 kHz | 2000 µs | 320 | 320 | 100 % | 100,0 % | 98,9 % |
| 6 kHz | 1666 µs | 320 | 320 | 100 % | 100,0 % | 99,2 % |
| 8 kHz | 1250 µs | 320 | 319 | 100 % | 99,7 % | 99,4 % |

À `10 kHz`, le banc actuel a échoué en synchronisation.  
La limite physique réelle n'est pas prouvée ; seule la limite du banc actuel est documentée.

### Conclusion sous-porteuse

- **8 kHz validé proprement.**
- **10 kHz non validé dans l'implémentation actuelle.**
- Zone particulièrement solide observée : `2..8 kHz`.

---

## 10. Débit 8-ASK à sous-porteuse fixe 8 kHz

Fenêtre RX corrigée à `1250 µs` ≈ 10 périodes.

| Durée symbole | Débit brut | Envoyés | Décisions | Corrects | Couverture | Exactitude | Sync |
|---:|---:|---:|---:|---:|---:|---:|---:|
| 30 ms | 100 bit/s | 320 | 320 | 298 | 100 % | 93,1 % | 98,0 % |
| 25 ms | 120 bit/s | 320 | 320 | 265 | 100 % | 82,8 % | 99,6 % |
| 20 ms | 150 bit/s | 320 | 320 | 299 | 100 % | 93,4 % | 98,5 % |
| 15 ms | 200 bit/s | 320 | 320 | 16 | 100 % | 5,0 % | 94,9 % |

### Interprétation

- La couverture restant à 100 % jusqu'à 200 bit/s montre que le RX continue à produire des décisions.
- L'effondrement à 200 bit/s concerne principalement la **classification des amplitudes**.
- Le comportement non monotone entre 100, 120 et 150 bit/s suggère une dépendance forte au **timing / placement des fenêtres RX**.
- Le plafond démontré actuel est **150 bit/s brut**, mais avec environ 6,6 % d'erreurs symbole sur ce run.
- Pour un modem conservateur, **100 bit/s brut** reste le point de fonctionnement recommandé actuellement.

---

## 11. Synchronisation RF

Le banc autonome a montré qu'une synchronisation RF dédiée est indispensable.

Architecture utilisée :

1. période OFF longue,
2. marqueur ON fort,
3. calibration ASK,
4. train alterné ON/OFF,
5. récupération de phase,
6. payload connu,
7. comparaison locale dans l'ESP RX.

Le comptage d'erreurs doit être fait **dans le RX**, pas dans le navigateur, pour éviter les erreurs de synchronisation USB/WebSerial.

---

## 12. Bonnes pratiques de programmation

### TX

- Fixer le canal RF.
- Fixer APWR.
- Utiliser ASK uniquement dans la zone monotone choisie.
- Modifier uniquement les bits ASK quand on change de niveau.
- Préserver GATE et tone_control.
- Éviter de réécrire l'ensemble du registre sans masque.
- Pour M-ASK, garder un mapping stable entre symbole et ASK.
- Prévoir une calibration au démarrage.

### RX

- Utiliser un gain RX fixe pour les expériences reproductibles.
- Réaliser les décisions localement sur l'ESP.
- Garder les impressions série hors de la boucle critique.
- Mesurer plusieurs fenêtres par symbole quand le temps le permet.
- Éviter les fenêtres qui chevauchent une transition de symbole.
- Utiliser la médiane ou la moyenne robuste pour rejeter les fenêtres aberrantes.
- Recalibrer si le canal, la distance, l'orientation ou le gain changent.

---

## 13. Ce qu'il ne faut pas supposer

- `ASK = x` n'est pas `x %` de puissance.
- APWR n'est pas linéaire sur 0..255.
- `amp1k` / `amp_f` n'est pas un dBm.
- ASK > 128 n'est pas une continuation monotone.
- Les centres RX absolus ne sont pas universels.
- Une sous-porteuse plus élevée ne garantit pas automatiquement un débit symbole plus élevé.
- Une bonne synchro ne garantit pas une bonne classification ASK.
- Un taux d'erreur mesuré sur quelques dizaines de symboles n'est pas suffisant pour caractériser un modem robuste.

---

## 14. Profil de référence recommandé

### Profil conservateur actuel

```text
RF channel      : 6 / 2.437 GHz
Modulation      : 8-ASK
ASK levels      : 70,83,94,102,108,114,120,127
Subcarrier      : 8 kHz
RX window       : 1250 us
Symbol duration : 30 ms
Raw bit rate    : 100 bit/s
Bits/symbol     : 3
Calibration     : mandatory
RX gain         : fixed
Decision        : nearest calibrated center
```

### Profil expérimental rapide

```text
Subcarrier      : 8 kHz
Symbol duration : 20 ms
Raw bit rate    : 150 bit/s
Observed run    : 299 / 320 correct
Accuracy        : 93.4 %
Status          : demonstrated, not yet robust
```

---

## 15. Statut de validation

| Élément | Statut |
|---|---|
| GATE bit 18 de `0x600005B8` | validé |
| ASK bits 17:10 | validé |
| askCode = -ASK mod 256 | validé |
| zone ASK 0..128 | validée empiriquement |
| rupture 128 -> 129 | validée empiriquement |
| 8-ASK avec 8 niveaux sélectionnés | validé |
| IQ_EST / E4 pour mesure RX | validé |
| 1..8 kHz de sous-porteuse | validé sur banc |
| 10 kHz | non validé / sync fail |
| 100 bit/s brut | validé comme point conservateur |
| 150 bit/s brut | démontré mais encore avec erreurs |
| 200 bit/s brut | non exploitable actuellement |

---

## 16. Phrase de référence

> Sur ESP8266, le champ ASK du PHY doit être traité comme une échelle numérique d'amplitude, exploitable de façon monotone principalement entre ASK 0 et 128. Pour le M-ASK, une calibration RX par centres est indispensable. Un jeu 8-ASK `{70,83,94,102,108,114,120,127}` a été validé avec une sous-porteuse jusqu'à 8 kHz et un débit brut démontré jusqu'à 150 bit/s dans le banc actuel.
