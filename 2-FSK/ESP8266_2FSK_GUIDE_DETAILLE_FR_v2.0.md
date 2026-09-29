# ESP8266 2‑FSK avec IQ_EST — Guide détaillé de mise en œuvre

Version : **2.0 — synthèse expérimentale**

Ce document décrit la méthode 2‑FSK qui a réellement fonctionné sur deux ESP8266 en utilisant le générateur de tonalité du PHY côté TX et le bloc IQ_EST côté RX.

Il distingue volontairement :

- les faits confirmés expérimentalement ;
- les choix de notre implémentation ;
- les éléments encore à caractériser.

---

# 1. Vue d’ensemble

Le principe final est simple :

```text
ESP8266 TX
  |
  |  TONE = 1016 ou 0
  v
canal RF Wi‑Fi 6
  |
  v
ESP8266 RX
  |
  |  IQ_EST, N=4
  v
suite I,Q,I,Q,...
  |
  v
rotation de phase entre acquisitions
  |
  v
f_eff ou dphi
  |
  v
seuil
  |
  v
bit 0 / bit 1
```

L’idée importante est que le bloc IQ_EST n’est pas seulement un bloc d’énergie.

À très faible N, notamment **N=4**, ses sorties I et Q changent à chaque acquisition et suivent une rotation cohérente. Cette rotation encode suffisamment d’information de fréquence pour séparer les deux tons FSK.

---

# 2. Pourquoi les premières approches ont échoué

## 2.1. Énergie E4 seule

Le registre :

`0x600005E4`

est bien un résultat d’énergie latched mis à jour après IQ_EST.

En statique, il pouvait séparer fortement deux tons.

Mais lorsque le TX alternait rapidement entre deux fréquences, la métrique dynamique s’effondrait et la BER restait proche du hasard.

Conclusion :

> E4 est utile comme indicateur de puissance / diagnostic, mais ce n’est pas la méthode qui a donné notre modem FSK rapide fiable.

## 2.2. CFO natif

Le registre CFO utilisé par certaines voies Wi‑Fi packet n’a pas fourni un signal frais, monotone et exploitable sur notre CW/FSK passif.

## 2.3. Corrélateurs R0..R3

Les registres autour de :

`0x60000580..0x6000058C`

ne doivent pas être traités comme I/Q bruts.

Notre configuration n’a pas donné directement la paire complexe souhaitée via cette voie.

---

# 3. Génération FSK côté TX

## 3.1. Registre principal

Le générateur de tonalité principal utilisé est :

`0x600005B8`

Format empirique confirmé :

| Bits | Fonction |
|---|---|
| 9:0 | `TONE` |
| 17:10 | `ASK` |
| 18 | `GATE` |

La relation observée pour TONE est :

`Δf_RF ≈ TONE × 78.125 kHz`

avec repliement modulo environ 80 MHz autour de la fréquence de canal.

Exemples :

```text
TONE=0      -> centre
TONE=64     -> ~ +5 MHz
TONE=128    -> ~ +10 MHz
TONE=256    -> ~ +20 MHz
TONE=704    -> ~ -25 MHz
TONE=1008   -> ~ -1.25 MHz
TONE=1016   -> ~ -625 kHz
```

## 3.2. Paire FSK retenue

La paire qui a très bien fonctionné :

```cpp
static constexpr uint16_t TONE_A = 1016; // bit 0
static constexpr uint16_t TONE_B = 0;    // bit 1

static constexpr uint8_t ASK  = 32;
static constexpr uint8_t APWR = 64;
```

L’écart RF nominal entre ces deux codes est d’environ :

`8 × 78.125 kHz = 625 kHz`

## 3.3. Commutation rapide

Le point critique est de ne modifier que les bits `TONE`.

Exemple :

```cpp
static inline void setTone(uint16_t tone) {
    uint32_t r = REG32(0x600005B8);
    r = (r & ~0x3FFu) | (tone & 0x3FFu);
    REG32(0x600005B8) = r;
    memw();
}
```

Pendant une trame :

- RF reste ON ;
- clock TX reste ON ;
- GATE reste ON ;
- ASK reste constant ;
- APWR reste constant ;
- seul TONE change.

Cela évite de refaire toute la mise sous tension RF à chaque bit.

---

# 4. Bloc IQ_EST côté RX

## 4.1. Registre de contrôle

Adresse :

`0x6000057C`

Format utile :

| Bit(s) | Nom | Rôle |
|---|---|---|
| 0 | ENABLE | active IQ_EST |
| 1 | START | lance l’acquisition |
| 2..16 | N / FIELD | longueur d’intégration |
| 18 | MODE | mode |
| 31 | DONE | acquisition terminée |

Dans notre modem :

```cpp
N = 4;
MODE = 0;
```

## 4.2. Résultats utiles

Adresses importantes :

```text
0x600005DC : I signé
0x600005E0 : Q signé
0x600005E4 : énergie latched
```

Pour notre discriminateur de fréquence, on utilise principalement **I et Q**.

---

# 5. Pourquoi N=4 a changé le projet

Au début, des fenêtres plus longues semblaient naturelles, car elles améliorent souvent le SNR d’une mesure d’énergie.

Mais pour une démodulation FSK rapide, on veut surtout :

- beaucoup de mesures ;
- très peu de délai ;
- conserver la phase instantanée suffisamment longtemps pour observer la rotation.

Avec **N=4**, le bloc renouvelle bien I et Q.

Dans un log réel, on observait par exemple des suites du genre :

```text
I,Q
-10112,-16384
-14208,  8512
  4288, 13760
 12672, -9728
-10688,-12544
-12096, 10944
```

Ce n’est pas une suite de valeurs aléatoires indépendantes : le vecteur complexe tourne.

Autre exemple :

```text
  448, 16192
15552,  -640
-2048,-15744
-18752, 1216
 2624, 18304
17088, -2688
-5056,-18624
```

Cela a permis de passer d’un discriminateur d’énergie peu fiable à un discriminateur de fréquence par rotation de phase.

---

# 6. Handshake IQ_EST correct

Une erreur importante rencontrée était la possibilité de relire un `DONE=1` appartenant à la mesure précédente.

La séquence robuste est :

```text
START = 0
attendre DONE = 0

START = 1
attendre DONE = 1

lire I
lire Q
lire éventuellement E4
```

Puis recommencer.

Pseudo-code :

```cpp
bool acquireIQ(int32_t &I, int32_t &Q) {
    uint32_t cfg = makeCfg();

    REG32(IQ_CTRL) = cfg;
    memw();

    REG32(IQ_CTRL) = cfg | ENABLE;
    memw();

    REG32(IQ_CTRL) = cfg | ENABLE | START;
    memw();

    while (!(REG32(IQ_CTRL) & DONE)) {
        // timeout conseillé
    }

    I = (int32_t)REG32(0x600005DC);
    Q = (int32_t)REG32(0x600005E0);

    REG32(IQ_CTRL) = cfg | ENABLE;
    memw();

    REG32(IQ_CTRL) = cfg;
    memw();

    return true;
}
```

---

# 7. Calcul de phase entre deux acquisitions

Soit :

```text
z0 = I0 + jQ0
z1 = I1 + jQ1
```

Le produit complexe :

```text
conj(z0) * z1
```

contient directement la rotation de phase entre les deux échantillons.

Sans calcul complexe explicite :

```text
dot   = I0*I1 + Q0*Q1
cross = I0*Q1 - Q0*I1
```

Puis :

```text
dphi = atan2(cross, dot)
```

En C/C++ :

```cpp
double cross =
    (double)I0 * (double)Q1 -
    (double)Q0 * (double)I1;

double dot =
    (double)I0 * (double)I1 +
    (double)Q0 * (double)Q1;

float dphi = atan2f((float)cross, (float)dot);
```

---

# 8. Conversion en fréquence effective

Si `dt` est le temps entre les deux acquisitions :

```text
f_eff = dphi / (2*pi*dt)
```

Exemple :

```cpp
float f_eff =
    dphi /
    (2.0f * PI * dt_seconds);
```

Important :

> `f_eff` n’est pas l’écart RF réel de 625 kHz.

Le chemin RX, l’aliasing, les horloges internes et la façon dont IQ_EST expose sa phase produisent une fréquence baseband effective.

On l’utilise comme **discriminateur relatif**, pas comme fréquencemètre absolu.

---

# 9. Moyenne circulaire

Une simple moyenne arithmétique de phases peut être mauvaise près de `-pi/+pi`.

Notre estimateur prend 8 acquisitions.

Cela donne jusqu’à 7 valeurs `dphi`.

On calcule :

```cpp
sumSin += sinf(dphi);
sumCos += cosf(dphi);
```

Puis :

```cpp
meanPhase = atan2f(sumSin, sumCos);
```

C’est une moyenne circulaire.

Ensuite :

```cpp
meanDt = sumDt / pairs;

f_eff =
    meanPhase /
    (2.0f * PI * meanDt * 1e-6f);
```

---

# 10. Calibration réelle des deux tons

Avec notre paire :

```text
bit 0 -> TONE 1016
bit 1 -> TONE 0
```

et :

```text
canal 6
ASK 32
APWR 64
N 4
MODE 0
```

le RX a montré de façon répétée :

```text
bit 0 / TONE 1016 : environ +19 à +21 kHz
bit 1 / TONE 0    : environ -26 à -28 kHz
```

Le seuil utilisé dans le modem :

```cpp
static constexpr int32_t FREQ_THRESHOLD_HZ = -3000;
```

Décision correcte :

```cpp
uint8_t bit =
    (f_eff < FREQ_THRESHOLD_HZ) ? 1 : 0;
```

Cette inversion de signe a été une découverte importante.

Une version précédente supposait l’inverse et produisait des mots complètement faux malgré une excellente séparation physique des deux tons.

---

# 11. Pourquoi le seuil -3000 Hz fonctionne

Si les deux centres sont approximativement :

```text
F0 ~ +20 kHz
F1 ~ -27 kHz
```

le milieu est vers :

```text
(+20 - 27) / 2 ≈ -3.5 kHz
```

Donc `-3000 Hz` est très proche du milieu naturel.

Lors des trames à 1500 µs, les marges observées étaient énormes.

Exemples typiques :

```text
F0=[+13 kHz, +27 kHz]
F1=[-28 kHz, -26 kHz]
```

---

# 12. Format de synchronisation utilisé

Notre TX envoie :

```text
20 ms A
20 ms B
32 bits payload
20 ms A
```

où :

```text
A = TONE 1016 = bit 0 = f_eff positif
B = TONE 0    = bit 1 = f_eff négatif
```

Le RX cherche :

1. A stable pendant environ 12 ms ;
2. transition vers B ;
3. B stable pendant environ 12 ms ;
4. estime que le payload commence 20 ms après le début de B.

---

# 13. Mot de test

Mot :

```text
0xD3A5C69B
```

MSB en premier.

À 1500 µs/symbole, le modem a produit de longues séries :

```text
RX=D3A5C69B
EXP=D3A5C69B
ERR=0/32
VALID=32
```

---

# 14. Test alterné 0101

Pour séparer le problème de démodulation du problème de synchronisation, un TX spécial a envoyé :

```text
010101010101...
```

à 1500 µs/symbole.

Après correction du mapping :

```text
bit 0 -> positif
bit 1 -> négatif
```

le RX a produit de nombreuses trames :

```text
ERR=0/64
POS=32
NEG=32
```

C’est la preuve la plus claire que :

- le générateur TONE commute correctement ;
- le chemin RF suit ;
- IQ_EST N=4 suit ;
- le discriminateur de phase suit ;
- 1500 µs/symbole est très confortable.

---

# 15. Passage à 750 µs/symbole

La seule modification principale :

```cpp
static constexpr uint32_t SYMBOL_US = 750;
```

Le discriminateur continue de montrer des groupes très propres :

```text
F0 ~ +19...+21 kHz
F1 ~ -25...-28 kHz
```

La plupart des trames restent :

```text
D3A5C69B
ERR=0/32
```

Mais certaines deviennent :

```text
A74B8D36
ERR=19/32
```

Or :

```text
D3A5C69B << 1 = A74B8D36
```

Cela montre que le RX s’est parfois décalé d’un symbole entier.

C’est donc un problème de **synchronisation temporelle**, pas un effondrement du discriminateur de fréquence.

---

# 16. Synchronisation plus robuste

Pour aller plus vite, le RX devrait idéalement ne pas déduire le début du payload uniquement de :

```text
premier instant B observé + 20 ms
```

car le premier B observé contient une erreur temporelle liée :

- au temps d’une estimation ;
- à `yield()` ;
- au moment exact du front ;
- à la quantification de `micros()`.

Plusieurs améliorations possibles :

## Option A — préambule alterné

Par exemple :

```text
AAAA
BBBB
10101010
payload
```

Le RX peut verrouiller précisément la phase symbole sur les transitions `10/01`.

## Option B — mot de sync connu

Exemple :

```text
0xA55A
```

avant le payload.

Le RX peut tester plusieurs décalages de phase symbole et choisir celui qui reproduit le mot de sync.

## Option C — suréchantillonnage

Prendre plusieurs estimations par symbole et chercher le centre temporel qui maximise la stabilité.

---

# 17. Pourquoi lire au centre du symbole

Les transitions de TONE ne sont pas forcément instantanément propres au niveau du discriminateur.

Lire au centre évite :

- transitoires ;
- changements partiels ;
- erreur de front.

À `SYMBOL_US=1500`, le centre est :

```text
+750 µs
```

À `SYMBOL_US=750` :

```text
+375 µs
```

---

# 18. E4 reste utile

Même si E4 n’est pas notre discriminateur final, il peut servir pour :

- vérifier qu’un signal est présent ;
- détecter une perte de lien ;
- rejeter une mesure très faible ;
- observer les changements de niveau RX.

Dans nos tests, le ton donnant `f_eff` négatif avait souvent une E4 plus grande que l’autre.

Mais il ne faut pas coder le bit uniquement à partir de ce niveau.

---

# 19. Éviter les pièges logiciels

## 19.1. Pas de Serial pendant le payload

Les `Serial.printf()` perturbent fortement le timing.

Capturer d’abord, imprimer après.

## 19.2. WDT

Les longues recherches de synchro doivent appeler `yield()`.

En revanche, une capture courte de quelques dizaines de millisecondes peut rester en boucle serrée pour garder le timing.

## 19.3. micros() est grossier

À quelques microsecondes, `micros()` est quantifié.

Pour la décision binaire, on pourrait même éviter la conversion en Hz et classifier directement `dphi`.

Exemple empirique précédent :

```text
un ton : dphi ~ -0.52 rad
autre  : dphi ~ -0.28...-0.37 rad
```

Un seuil direct de phase pourrait réduire l’erreur liée à `dt`.

Dans la calibration finale actuelle, il faudra refaire ce seuil avec le mapping corrigé avant de le figer.

---

# 20. Architecture logicielle recommandée

## TX

```text
setup RF
enable TX clock
set APWR
set ASK
set GATE
set TONE A

send sync A
send sync B

for each bit:
    if bit == 0:
        setTone(1016)
    else:
        setTone(0)
    attendre SYMBOL_US
```

## RX

```text
setup canal 6

while recherche synchro:
    estimateFreq()
    yield()

calculer dataStart

for each bit:
    attendre centre symbole
    estimateFreq()
    classifier
    stocker bit

imprimer après la trame
```

---

# 21. Pseudo-code RX complet

```cpp
int32_t estimateFreq() {
    IQSample s[8];

    for (int i = 0; i < 8; ++i)
        acquireIQ(s[i]);

    float sumSin = 0;
    float sumCos = 0;
    uint32_t sumDt = 0;
    int pairs = 0;

    for (int i = 1; i < 8; ++i) {
        uint32_t dt = s[i].t - s[i-1].t;

        double cross =
            (double)s[i-1].i * s[i].q -
            (double)s[i-1].q * s[i].i;

        double dot =
            (double)s[i-1].i * s[i].i +
            (double)s[i-1].q * s[i].q;

        float p = atan2f(cross, dot);

        sumSin += sinf(p);
        sumCos += cosf(p);
        sumDt += dt;
        ++pairs;
    }

    float phase =
        atan2f(sumSin, sumCos);

    float dt_us =
        (float)sumDt / pairs;

    return phase /
        (2 * PI * dt_us * 1e-6f);
}
```

Décision :

```cpp
bit = estimateFreq() < -3000 ? 1 : 0;
```

---

# 22. Registres à retenir

## TX

```text
0x600005B8
  bits 9:0   TONE
  bits 17:10 ASK
  bit 18     GATE
```

## RX IQ_EST

```text
0x6000057C  contrôle
0x600005DC  I
0x600005E0  Q
0x600005E4  énergie latched
```

Ne pas confondre :

```text
0x600005EC
0x600005F0
```

avec I et Q.

---

# 23. Paramètres de référence actuels

```text
Canal               6
Fréquence canal     2.437 GHz
TONE bit 0          1016
TONE bit 1          0
ASK                  32
APWR                 64
IQ_EST N             4
IQ_EST MODE          0
Acquisitions/est.    8
Seuil f_eff          -3000 Hz
Payload test         D3A5C69B
Ordre                MSB first
Sync A               20 ms
Sync B               20 ms
Symbol robuste       1500 µs
Symbol testé rapide  750 µs
```

---

# 24. Résumé des découvertes essentielles

1. **Le registre TX de tonalité est bien utilisable pour commuter rapidement deux fréquences.**
2. **E4 est excellent en statique, mais mauvais comme discriminateur FSK rapide dans notre configuration.**
3. **N=4 renouvelle I et Q très rapidement.**
4. **I/Q tournent de façon cohérente.**
5. **La différence de phase entre acquisitions sépare fortement les deux tons.**
6. **Le mapping réel doit être calibré : ici bit0=positif, bit1=négatif.**
7. **1500 µs/symbole est très robuste.**
8. **750 µs/symbole fonctionne physiquement, mais la synchro doit être améliorée.**
9. **Le prochain gain de performance vient surtout de la synchronisation, pas du discriminateur RF.**

---

# 25. Point de départ conseillé pour un nouveau projet

Commencer exactement par :

```text
TONE_A=1016
TONE_B=0
ASK=32
APWR=64

N=4
MODE=0

8 acquisitions par estimation
seuil f_eff=-3000 Hz

20 ms A
20 ms B
32 bits
1500 µs/symbole
```

Valider d’abord avec :

```text
010101...
```

Puis avec :

```text
D3A5C69B
```

Une fois `ERR=0` stable, diminuer progressivement `SYMBOL_US`.

---

# 26. Nature de cette référence

Cette référence est **empirique**.

Elle documente ce qui a réellement fonctionné sur notre montage ESP8266.

Les registres PHY utilisés ne constituent pas une API Arduino officielle et certains comportements peuvent varier selon :

- révision de puce ;
- SDK/core ESP8266 ;
- canal ;
- gain RX ;
- niveau RF ;
- environnement ;
- timing logiciel.

La bonne pratique est donc toujours :

**mesurer → calibrer → verrouiller les paramètres → seulement ensuite optimiser le débit.**
