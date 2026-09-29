# ESP8266 — Référence pratique 2‑FSK avec IQ_EST (v2.0)

## Ce qui fonctionne réellement

Cette méthode utilise deux blocs différents du PHY ESP8266 :

- **TX** : le générateur de tonalité interne autour de `0x600005B8`.
- **RX** : le bloc **IQ_EST** autour de `0x6000057C`, utilisé à très petite fenêtre `N=4`.

Le point essentiel découvert expérimentalement est que **N=4 renouvelle bien I et Q à chaque acquisition**, et que la **rotation de phase entre deux acquisitions successives** est un excellent discriminateur de fréquence.

> Ce n’est pas un fréquencemètre RF absolu. On mesure une **fréquence de rotation baseband effective / aliasée**, mais elle sépare très bien les deux tons FSK.

---

## 1. TX — registre de tonalité

Registre principal testé :

`0x600005B8`

| Bits | Nom | Fonction |
|---|---|---|
| 9:0 | `TONE` | tonalité / offset |
| 17:10 | `ASK` | amplitude |
| 18 | `GATE` | active la tonalité |

Relation empirique :

`Δf_RF ≈ TONE × 78.125 kHz`, modulo ~80 MHz autour du canal.

Exemples :

- `TONE=0` : centre
- `TONE=64` : environ +5 MHz
- `TONE=128` : environ +10 MHz
- `TONE=256` : environ +20 MHz
- `TONE=1016` : environ −625 kHz

### Paire 2‑FSK actuellement validée

- **bit 0** → `TONE_A = 1016`
- **bit 1** → `TONE_B = 0`
- `ASK = 32`
- `APWR = 64`
- canal Wi‑Fi 6

Ne modifier que `TONE` pendant la trame. Garder RF, clock, gate et amplitude actifs.

---

## 2. RX — IQ_EST

Registre de contrôle :

`0x6000057C`

| Bit(s) | Nom | Fonction |
|---|---|---|
| 0 | ENABLE | active IQ_EST |
| 1 | START | démarre l’acquisition |
| 2..16 | FIELD / N | longueur d’intégration |
| 18 | MODE | mode 0/1 |
| 31 | DONE | résultat prêt |

Dans notre modem rapide :

- `N = 4`
- `MODE = 0`

Résultats utilisés :

| Adresse | Valeur |
|---|---|
| `0x600005DC` | I signé |
| `0x600005E0` | Q signé |
| `0x600005E4` | énergie latched, utile pour diagnostic |

`0x600005EC` et `0x600005F0` sont des statistiques/accumulateurs, **pas les I/Q bruts**.

---

## 3. Pourquoi N=4 est important

Avec `N=4`, chaque acquisition est extrêmement courte.

Dans nos captures, les acquisitions successives utilisées pour la phase étaient typiquement séparées d’environ **3 µs** au niveau du code mesuré avec `micros()`.

N=4 permet donc de suivre la rotation complexe :

`z[n] = I[n] + j Q[n]`

Au lieu d’utiliser seulement l’énergie, on regarde la rotation entre deux points I/Q.

---

## 4. Handshake correct

Pour éviter les résultats `DONE` périmés :

1. écrire `START=0`
2. attendre `DONE=0`
3. écrire `START=1`
4. attendre `DONE=1`
5. lire I et Q
6. recommencer

Ne pas simplement repulser START en supposant que DONE représente toujours une nouvelle mesure.

---

## 5. Discriminateur de fréquence par phase

Pour deux acquisitions successives :

```text
cross = Iprev*Qnow - Qprev*Inow
dot   = Iprev*Inow + Qprev*Qnow

dphi = atan2(cross, dot)
```

Puis :

```text
f_eff = dphi / (2*pi*dt)
```

`f_eff` est une fréquence effective du chemin RX, pas l’offset RF absolu.

Pour réduire le bruit, notre code prend **8 acquisitions**, donc jusqu’à **7 différences de phase**, puis calcule une moyenne circulaire.

---

## 6. Calibration réellement observée

Avec :

- canal 6
- `TONE_A=1016`
- `TONE_B=0`
- `ASK=32`
- `APWR=64`
- `N=4`
- `MODE=0`

le RX voit typiquement :

- **bit 0 / TONE_A=1016** → `f_eff ≈ +19 à +21 kHz`
- **bit 1 / TONE_B=0** → `f_eff ≈ −26 à −28 kHz`

Seuil actuellement utilisé :

```cpp
static constexpr int32_t FREQ_THRESHOLD_HZ = -3000;
```

Décision :

```cpp
bit = (f_eff < FREQ_THRESHOLD_HZ) ? 1 : 0;
```

Le signe du discriminateur est **empirique** : il faut toujours vérifier la calibration réelle avant de figer le mapping.

---

## 7. Format de trame qui fonctionne

TX validé :

```text
20 ms TONE_A
20 ms TONE_B
32 bits de données
20 ms TONE_A
```

Mot de test :

`0xD3A5C69B`, MSB en premier.

À `SYMBOL_US = 1500`, le RX a obtenu de très longues séries de :

```text
RX=D3A5C69B  ERR=0/32
```

À `SYMBOL_US = 750`, le discriminateur reste très propre, mais une erreur de synchronisation d’un symbole apparaît parfois.

---

## 8. Résultats importants

### 1500 µs / symbole

Très robuste :

- groupes F0/F1 très séparés
- nombreuses trames à `0/32`
- erreurs rares

### 750 µs / symbole

Le PHY suit toujours parfaitement les deux tons :

- F0 ≈ +19…+21 kHz
- F1 ≈ −25…−28 kHz

Mais certaines trames deviennent :

`A74B8D36`

ce qui correspond à :

`D3A5C69B << 1`

Donc le problème à 750 µs est surtout **la synchronisation de début de payload**, pas la démodulation FSK.

---

## 9. Ce qui n’a pas marché

### E4 comme démodulateur rapide

E4 séparait très bien les deux tons en statique, mais lors des séquences FSK rapides, les valeurs dynamiques se sont écrasées et la BER est restée proche du hasard.

### Registre CFO packet-Wi‑Fi

`0x60009800` n’a pas fourni une mesure fraîche/monotone exploitable pour un CW/FSK passif.

### R0..R3

Les registres autour de `0x60000580..0x6000058C` n’ont pas donné directement une paire I/Q brute exploitable dans notre configuration.

---

## 10. Recette minimale

### TX

```cpp
setTone(1016);   // bit 0
setTone(0);      // bit 1
```

Garder la porte et l’amplitude actives pendant toute la trame.

### RX

```cpp
N = 4;
MODE = 0;

acquire I,Q;
acquire I,Q;
compute dphi;
repeat several times;
circular-average dphi;
convert to f_eff;
bit = (f_eff < -3000) ? 1 : 0;
```

### Timing

Commencer par :

```cpp
SYMBOL_US = 1500;
```

Puis tester :

```cpp
SYMBOL_US = 750;
```

une fois la synchro parfaitement contrôlée.

---

## 11. Règle d’or

Pour ce montage, la méthode fiable est :

**TONE TX → IQ_EST N=4 → rotation I/Q → seuil de phase/fréquence → bit**

et non :

**TONE TX → énergie E4 seule → bit**

