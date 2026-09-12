# ESP8266 — Générateur autonome OOK / ASK
## Référence technique officielle du projet de reverse-engineering

**Édition unique : v1.0**  
**Date : 2026-09-12**  
**Statut : FIGÉE — émission unique sur demande**  
**Portée : ESP8266 / mask-ROM + PHY v6 du corpus analysé**

> **Important — statut du mot “officiel”.** Ce document est la référence officielle **du projet de reverse-engineering mené sur le corpus analysé**. Il ne s’agit pas d’un document officiel d’Espressif et il ne doit pas être présenté comme tel.

> **Règle de maintenance.** Cette édition est volontairement figée. Les découvertes ultérieures ne modifieront pas ce document. Elles continueront d’être intégrées au document maître vivant `ESP8266_WIFI_PHY_REVERSE_ENGINEERING.md`.

---

## 1. Objet

Ce document décrit comment utiliser l’ESP8266 comme **générateur RF autonome OOK et ASK/M-ASK**, après une initialisation RF/PHY unique, **sans revenir au fonctionnement Wi‑Fi**.

La cible n’est pas :

- de transmettre des trames 802.11 ;
- de faire coexister OOK/ASK et le Wi‑Fi normal ;
- de restaurer PP/LMAC/WDEV après la modulation ;
- de transformer l’ESP8266 en SDR I/Q généraliste.

La cible est :

```text
BOOT
  ↓
initialisation / calibration RF-PLL-PHY une fois
  ↓
arrêt / abandon du trafic Wi‑Fi normal
  ↓
maintien RF + PLL + TX clock
  ↓
configuration du générateur de tone slot 1
  ↓
┌────────────────────────────────────────────┐
│ OOK   : bit18 ON/OFF                       │
│ ASK   : bits17:10 modifiés à chaud         │
│ M-ASK : plusieurs valeurs digital_scale    │
└────────────────────────────────────────────┘
  ↓
reste en mode générateur RF
```

---

## 2. Niveau de certitude

### 2.1 Ce qui est considéré comme complètement décodé côté commande logicielle

| Élément | Statut |
|---|---:|
| adresse du tone slot 1 | **100 %** |
| gate OOK bit18 | **100 %** |
| masque ASK bits17:10 | **100 %** |
| encodage `(-digital_scale)&0xFF` | **100 %** |
| mise à jour ASK à chaud | **100 % démontré** |
| distinction scale numérique / scale analogique | **100 % logiciel** |
| comportement `start_tx_tone()` / `stop_tx_tone()` pertinent à OOK | **100 %** |
| nécessité de garder TX clock active pendant les symboles | **100 %** |
| nécessité de normaliser le slot avant modulation | **100 %** |

### 2.2 Ce qui n’est pas une lacune logicielle mais une caractérisation matérielle

Les points suivants doivent être mesurés sur le silicium/RF :

- extinction RF réelle lorsque bit18 = 0 ;
- temps entre write MMIO et changement RF ;
- jitter temporel du front RF ;
- continuité de phase OFF→ON ;
- relation `digital_scale → amplitude / puissance RF` ;
- nombre de niveaux ASK réellement séparables ;
- transitoires spectraux ;
- puissance absolue en dBm.

Le désassemblage LX106 ne peut pas fournir ces valeurs physiques.

---

## 3. Corpus et fonctions utilisées

Le modèle repose sur :

- mask-ROM ESP8266 analysée ;
- `libphy.a` / PHY v6 analysé ;
- chemins ROM `rfcal_pwrctrl`, `rfcal_rxiq`, `rfcal_txcap`, TXIQ ;
- accès directs au bloc tone ;
- instrumentation interne IQ_EST et SAR.

Fonctions ROM utiles identifiées :

```text
0x40006B08  phy_get_romfuncs
0x40006C50  rom_set_channel_freq
0x40007268  rom_i2c_readReg
0x400072D8  rom_i2c_writeReg
0x4000754C  rom_pbus_set_rxgain
0x40007610  rom_pbus_set_txgain
0x400076FC  rom_pbus_xpd_tx_off
0x40007740  rom_pbus_xpd_tx_on
0x400077A0  rom_pbus_xpd_tx_on__low_gain
0x40007968  rom_rfpll_set_freq
0x40007EB4  rom_rfcal_pwrctrl
0x4000804C  rom_rfcal_rxiq
0x40008388  rom_rfcal_txcap
0x40008610  rom_rfcal_txiq
```

Le générateur de tone est également exposé via `g_phyFuns` :

```text
+0x03C  rom_set_txclk_en
+0x050  rom_set_ana_inf_tx_scale
+0x068  rom_start_tx_tone
+0x06C  rom_stop_tx_tone
+0x070  rom_txtone_linear_pwr
```

Dans le dump étudié :

```text
rom_start_tx_tone ≈ 0x400068B4
rom_stop_tx_tone  ≈ 0x4000698C
```

---

## 4. Registres du générateur de tone

Trois slots existent :

```text
0x600005B8   TONE SLOT 1
0x600005BC   TONE SLOT 2
0x600005C4   TONE SLOT 3
```

Pour OOK/ASK, **le slot 1 suffit**.

Les trois usages ROM directs retrouvés (`rfcal_pwrctrl`, `rfcal_rxiq`, `rfcal_txcap`) utilisent le slot 1 et laissent slots 2/3 désactivés.

---

## 5. Packing du slot 1

Le comportement logiciel reconstruit de `rom_start_tx_tone()` est :

```c
r = REG32(slot);
r &= 0xF0000000;
r |= raw_control;
r |= ((uint32_t)((0x100 - digital_scale) & 0xff) << 10);
r |= ((uint32_t)mode_code << 18);
REG32(slot) = r;
```

Représentation pratique :

```text
31        28 27                  18 17          10 9                    0
┌───────────┬──────────────────────┬──────────────┬──────────────────────┐
│ préservés │ zone mode/test       │ scale code   │ raw tone_control     │
│           │ bit18 = gate normal  │ 8 bits       │ valeurs 8/64 vues   │
└───────────┴──────────────────────┴──────────────┴──────────────────────┘
```

### 5.1 Important : largeur des champs

La ROM **ne masque pas** explicitement :

```c
raw_control &= 0x3ff;
mode_code   &= 0x3ff;
```

Ces masques n’existent pas dans `rom_start_tx_tone()`.

Pour un firmware autonome, il est néanmoins prudent d’utiliser :

```c
control_safe = tone_control & 0x3ff;
```

afin de garantir qu’un `tone_control` incorrect ne chevauche pas le champ ASK. Ce masque est une **protection de notre firmware**, pas un comportement de la ROM.

---

## 6. Mode normal du tone

Pour un tone normal :

```text
mode_code = 1
```

Ainsi :

```text
1 << 18 = 0x00040000
```

Le bit18 est donc le bit actif du mode normal.

`rom_stop_tx_tone()` efface précisément :

```text
0x00040000
```

et ne nettoie pas les autres bits de mode/test.

C’est la preuve logicielle principale que **bit18 est le gate minimal du tone dans le mode normal**.

---

# PARTIE I — OOK

## 7. Principe OOK

OOK = **On-Off Keying**.

La stratégie correcte n’est pas :

```text
symbole 1 : start_tx_tone()
symbole 0 : stop_tx_tone()
```

car `rom_stop_tx_tone()` :

1. efface bit18 ;
2. coupe ensuite la TX clock globale.

Cela impose une réinitialisation inutile du chemin TX entre symboles.

La stratégie correcte est :

```text
PLL       reste actif
RF TX     reste préparé
TX clock  reste active
slot1     reste configuré

bit18 = 1 → ON
bit18 = 0 → OFF
```

---

## 8. Constantes OOK canoniques

```c
#define TONE1_ADDR       0x600005B8u
#define TONE_GATE_BIT    18u
#define TONE_GATE_MASK   0x00040000u
```

### ON

```c
slot |= TONE_GATE_MASK;
```

### OFF

```c
slot &= ~TONE_GATE_MASK;
```

Aucun autre champ ne doit être modifié pendant un symbole OOK.

---

## 9. Primitive OOK canonique

Pseudo-C :

```c
static inline void xtensa_memw(void)
{
    __asm__ volatile ("memw" ::: "memory");
}

static inline uint32_t reg32_read(uint32_t addr)
{
    return *(volatile uint32_t *)addr;
}

static inline void reg32_write(uint32_t addr, uint32_t value)
{
    *(volatile uint32_t *)addr = value;
}

static inline void ook_set(bool on)
{
    uint32_t r;

    xtensa_memw();
    r = reg32_read(TONE1_ADDR);

    if (on)
        r |= TONE_GATE_MASK;
    else
        r &= ~TONE_GATE_MASK;

    xtensa_memw();
    reg32_write(TONE1_ADDR, r);
}
```

Les séquences ROM pertinentes utilisent des barrières `MEMW` autour des accès MMIO du bloc tone. La primitive autonome doit conserver cette discipline.

---

## 10. Pourquoi il faut un Read-Modify-Write

Il ne faut pas écrire :

```c
REG32(TONE1_ADDR) = TONE_GATE_MASK;
```

Cela détruirait :

- `tone_control` ;
- le scale ASK ;
- les bits de mode/test supérieurs ;
- éventuellement des bits préservés.

Toujours utiliser un **RMW ciblé**.

---

## 11. Normalisation obligatoire du slot avant OOK

Après une calibration TXIQ, le slot peut conserver des bits de test/mode alors que bit18 a été effacé.

Il est donc incorrect de commencer par :

```c
REG32(TONE1_ADDR) |= TONE_GATE_MASK;
```

sur un état inconnu.

Avant le premier symbole, il faut reconstruire un mot de slot normal :

```c
static inline uint32_t tone1_build_normal(
    uint32_t old_slot,
    uint16_t tone_control,
    uint8_t digital_scale,
    bool enabled)
{
    uint32_t r;
    uint8_t code = (uint8_t)(0u - digital_scale);

    r  = old_slot & 0xF0000000u;
    r |= ((uint32_t)tone_control & 0x3FFu);
    r |= ((uint32_t)code << 10);
    r |= ((uint32_t)(enabled ? 1u : 0u) << 18);

    return r;
}
```

Ici, `&0x3FF` est la protection autonome recommandée, pas un masque ROM observé.

---

# PARTIE II — ASK / M-ASK

## 12. Principe ASK

ASK = **Amplitude Shift Keying**.

Le tone reste actif et sa composante numérique est modifiée par :

```text
bits17:10
```

Le chemin TXIQ démontre que ce champ peut être **réécrit à chaud pendant que la TX clock reste active**.

C’est donc le mécanisme canonique pour :

- 2-ASK ;
- 4-ASK ;
- 8-ASK ;
- M-ASK en général, dans les limites RF mesurées.

---

## 13. Encodage exact du scale numérique

Le champ n’est pas un entier d’amplitude positif direct.

L’encodage logiciel exact est :

```text
digital_field = (-digital_scale) mod 256
slot[17:10]   = digital_field
```

Pour le chemin canonique :

```text
digital_scale = 0..63
```

Exemples :

| `digital_scale` | champ 8 bits |
|---:|---:|
| 0 | `0x00` |
| 1 | `0xFF` |
| 2 | `0xFE` |
| 3 | `0xFD` |
| ... | ... |
| 63 | `0xC1` |

Il ne faut donc **jamais** interpréter directement le champ brut comme une amplitude linéaire.

---

## 14. Masque ASK canonique

```c
#define TONE_SCALE_SHIFT  10u
#define TONE_SCALE_MASK   0x0003FC00u
```

Primitive :

```c
static inline void ask_set_digital_scale(uint8_t digital_scale)
{
    uint32_t r;
    uint8_t code;

    /* chemin canonique décodé */
    if (digital_scale > 63)
        digital_scale = 63;

    code = (uint8_t)(0u - digital_scale);

    xtensa_memw();
    r = reg32_read(TONE1_ADDR);

    r &= ~TONE_SCALE_MASK;
    r |= ((uint32_t)code << TONE_SCALE_SHIFT);

    xtensa_memw();
    reg32_write(TONE1_ADDR, r);
}
```

Cette écriture préserve :

- bit18 ;
- `tone_control` ;
- les modes supérieurs ;
- les bits 31:28.

---

## 15. M-ASK

Une constellation M-ASK logicielle peut être représentée par une table :

```c
static const uint8_t ask_levels_4[4] = {
    LEVEL0,
    LEVEL1,
    LEVEL2,
    LEVEL3
};
```

Puis :

```c
ask_set_digital_scale(ask_levels_4[symbol & 3]);
```

**Attention :** le logiciel définit les codes mais pas les amplitudes RF absolues. Les valeurs `LEVEL0...LEVEL3` doivent être choisies après caractérisation instrumentée afin d’obtenir des niveaux RF correctement espacés.

---

## 16. ASK versus OOK

### OOK

```text
bit18 change
scale reste fixe
```

### ASK

```text
bit18 reste à 1
bits17:10 changent
```

### Combinaison possible

Un firmware peut aussi utiliser :

```text
OFF absolu logique : bit18 = 0
niveaux actifs      : bit18 = 1 + plusieurs digital_scale
```

Cela permet d’avoir un symbole réellement “gated” et plusieurs niveaux actifs, mais la séparation RF réelle entre niveaux doit être mesurée.

---

# PARTIE III — SCALE ANALOGIQUE

## 17. `rom_set_ana_inf_tx_scale()`

Cette fonction sépare une valeur `x` entre :

- composante numérique ;
- composante analogique interne.

Pseudo-code reconstruit :

```c
uint8_t set_ana_inf_tx_scale(uint8_t x)
{
    uint8_t analog_scale;
    uint8_t digital_scale;

    if (x < 64) {
        analog_scale  = 0;
        digital_scale = x;
    } else {
        analog_scale  = (uint8_t)(63 - x);
        digital_scale = 63;
    }

    /* bloc I2C interne 0x77, host 0, reg 9, bits7:0 */
    i2c_write_mask(0x77, 0, 9, 7, 0, analog_scale);

    return digital_scale;
}
```

Le branchement Xtensa observé est un test non signé contre `64`.

---

## 18. Rôle du scale analogique dans un modulateur autonome

Le scale analogique ne doit pas être utilisé symbole-par-symbole pour une ASK rapide.

Architecture recommandée :

```text
scale analogique
      ↓
réglage de plage / niveau de base
      ↓
fixé pendant la trame

scale numérique bits17:10
      ↓
modulation ASK rapide
```

La partie analogique passe par un bus I²C interne au PHY ; elle est plus transactionnelle et son objectif est différent du RMW direct du slot.

---

# PARTIE IV — RF / PBUS / TX CLOCK

## 19. `start_tx_tone()` n’active pas toute la chaîne RF

Le générateur numérique et la chaîne RF sont séparés.

Les calibrations ROM montrent :

```text
préparation RF/PBUS
        ↓
RX RF OFF
        ↓
TX XPD ON
        ↓
réglage scale / détecteur
        ↓
start_tx_tone()
```

Donc appeler uniquement `rom_start_tx_tone()` sur une RF froide n’est pas une procédure d’initialisation complète démontrée.

---

## 20. PBUS

Registres principaux :

```text
0x60000594   commande PBUS
0x600005A0   statut PBUS
```

Format de commande reconstruit :

```text
bits15:14   bank
bits13:5    value (9 bits)
bits4:2     selector
bit1        START
bit0        préservé
```

Pseudo-code :

```c
cmd = REG32(0x60000594);
cmd &= 0xFFFF0001;
cmd |= (bank << 14);
cmd |= (value << 5);
cmd |= (selector << 2);
cmd |= 0x2;
REG32(0x60000594) = cmd;

while (REG32(0x600005A0) & 0x80000000)
    ;

REG32(0x60000594) &= ~0x2;
```

---

## 21. Activation TX RF observée

### TX OFF

`rom_pbus_xpd_tx_off()` :

```text
PBUS(6, 1,   0)
PBUS(1, 1,  12)
PBUS(2, 1,   0)
```

### TX ON

`rom_pbus_xpd_tx_on()` :

```text
PBUS(2, 1,   1)
PBUS(7, 1,  95)
PBUS(0, 1,   x)
PBUS(1, 1, 127)
PBUS(6, 1, 127)
```

La variante low-gain utilise notamment :

```text
PBUS(7, 1, 0)
```

au lieu de `95`.

Le sélecteur `4 / bank 1` est utilisé par `rom_pbus_set_txgain()`.

---

## 22. TX clock

`rom_start_tx_tone()` commence par :

```text
rom_set_txclk_en(1)
```

`rom_stop_tx_tone()` :

```text
clear bit18
puis
rom_set_txclk_en(0)
```

Conséquence fondamentale :

> **OOK et ASK rapides doivent maintenir la TX clock active.**

---

# PARTIE V — DÉMARRAGE AUTONOME SANS RETOUR WI‑FI

## 23. Définition correcte de « désactiver le Wi‑Fi »

Dans ce projet, « désactiver le Wi‑Fi » signifie :

- ne plus produire de trafic 802.11 ;
- ne plus laisser PP/LMAC/WDEV reprendre possession du TX ;
- empêcher sleep/wakeup et reconfiguration RF pendant la session ;
- **ne pas éteindre le PHY/RF dont le générateur a besoin**.

Un appel de haut niveau qui coupe totalement la radio n’est donc pas équivalent à l’objectif recherché.

---

## 24. Procédure robuste recommandée

### Phase A — boot / calibration

1. démarrer normalement le silicium ;
2. laisser le PHY effectuer l’initialisation/calibration nécessaire ;
3. fixer le canal/fréquence RF ;
4. préparer le mode RF de test / chaîne TX ;
5. empêcher les mécanismes susceptibles de remettre la RF en sleep ou de la réinitialiser ;
6. abandonner définitivement le trafic Wi‑Fi.

### Phase B — préparation RF TX

À partir d’un état RF initialisé :

```text
PBUS debug/test
RX RF off
TX XPD on
choix gain / scale de base
TX clock on
```

Les calibrations `rfcal_pwrctrl` et `rfcal_txcap` démontrent ce squelette.

### Phase C — normalisation du tone

1. choisir `tone_control` ;
2. choisir le scale de base ;
3. réécrire complètement le slot 1 en `mode_code=1` ;
4. ne plus utiliser les anciens bits TXIQ laissés par une calibration.

### Phase D — modulation

```text
OOK   → bit18 uniquement
ASK   → bits17:10 uniquement
M-ASK → bits17:10 uniquement
```

### Phase E — aucune restauration Wi‑Fi

Le firmware reste dans l’état générateur.

---

## 25. Limite importante : cold-start totalement bare-metal

Le corpus permet de décoder complètement les **primitives OOK/ASK**, mais ne démontre pas une séquence minimale unique et universelle depuis un reset totalement froid qui remplacerait toute l’initialisation PHY/RF/calibration.

La procédure robuste est donc :

> **initialiser/calibrer la RF une fois avec le chemin PHY/test existant, puis prendre définitivement le contrôle du générateur.**

Ne pas présenter une séquence PBUS isolée comme substitut garanti à toute l’initialisation analogique du silicium.

---

# PARTIE VI — SQUELETTE DE FIRMWARE

## 26. Registres et masques

```c
#include <stdint.h>
#include <stdbool.h>

#define TONE1_ADDR        0x600005B8u
#define TONE_GATE_MASK    0x00040000u
#define TONE_SCALE_MASK   0x0003FC00u
#define TONE_SCALE_SHIFT  10u
#define TONE_CONTROL_MASK 0x000003FFu  /* protection locale */
```

---

## 27. Accès MMIO

```c
static inline void memw(void)
{
    __asm__ volatile ("memw" ::: "memory");
}

static inline uint32_t rd32(uint32_t addr)
{
    return *(volatile uint32_t *)addr;
}

static inline void wr32(uint32_t addr, uint32_t value)
{
    *(volatile uint32_t *)addr = value;
}
```

---

## 28. Construction propre du slot 1

```c
static inline uint32_t build_tone1_normal(
    uint32_t previous,
    uint16_t tone_control,
    uint8_t digital_scale,
    bool gate)
{
    uint8_t scale_code;
    uint32_t r;

    if (digital_scale > 63)
        digital_scale = 63;

    scale_code = (uint8_t)(0u - digital_scale);

    r  = previous & 0xF0000000u;
    r |= ((uint32_t)tone_control & TONE_CONTROL_MASK);
    r |= ((uint32_t)scale_code << TONE_SCALE_SHIFT);
    r |= gate ? TONE_GATE_MASK : 0u;

    return r;
}
```

---

## 29. Initialisation du slot

```c
static void tone1_init(uint16_t tone_control,
                       uint8_t digital_scale,
                       bool start_on)
{
    uint32_t old;
    uint32_t fresh;

    memw();
    old = rd32(TONE1_ADDR);

    fresh = build_tone1_normal(
        old,
        tone_control,
        digital_scale,
        start_on
    );

    memw();
    wr32(TONE1_ADDR, fresh);
}
```

Avant cet appel, la RF TX et la TX clock doivent être prêtes.

---

## 30. OOK

```c
static inline void tone_ook(bool on)
{
    uint32_t r;

    memw();
    r = rd32(TONE1_ADDR);

    if (on)
        r |= TONE_GATE_MASK;
    else
        r &= ~TONE_GATE_MASK;

    memw();
    wr32(TONE1_ADDR, r);
}
```

Exemple logique :

```c
for (;;) {
    tone_ook(true);
    symbol_delay();

    tone_ook(false);
    symbol_delay();
}
```

`symbol_delay()` dépend du débit recherché et doit être remplacé par un mécanisme temporel déterministe adapté au firmware final.

---

## 31. ASK

```c
static inline void tone_ask(uint8_t digital_scale)
{
    uint32_t r;
    uint8_t code;

    if (digital_scale > 63)
        digital_scale = 63;

    code = (uint8_t)(0u - digital_scale);

    memw();
    r = rd32(TONE1_ADDR);

    r &= ~TONE_SCALE_MASK;
    r |= ((uint32_t)code << TONE_SCALE_SHIFT);

    memw();
    wr32(TONE1_ADDR, r);
}
```

Exemple 4-ASK :

```c
static const uint8_t level[4] = {
    L0, L1, L2, L3
};

for (;;) {
    uint8_t sym = next_2_bits();
    tone_ask(level[sym]);
    symbol_delay();
}
```

Les valeurs `L0...L3` doivent être obtenues par mesure.

---

## 32. OOK + niveaux ASK

Une extension utile :

```c
static inline void send_symbol(bool active, uint8_t level)
{
    if (!active) {
        tone_ook(false);
        return;
    }

    tone_ask(level);
    tone_ook(true);
}
```

Cela permet d’utiliser :

- OFF par gate ;
- plusieurs niveaux actifs par scale numérique.

---

# PARTIE VII — CONCURRENCE ET TIMING

## 33. Interruptions

Pour une modulation temporellement propre, un changement de symbole ne doit pas être retardé de façon arbitraire par :

- ISR Wi‑Fi ;
- timers PHY de maintenance ;
- sleep/wakeup ;
- tâches réseau ;
- reconfiguration canal/PLL.

Puisque la cible n’a pas besoin de revenir au Wi‑Fi, il est préférable de supprimer/neutraliser ces sources plutôt que de tenter une coexistence.

---

## 34. Atomicité

Le RMW :

```text
read slot
modify field
write slot
```

n’est pas atomique vis-à-vis d’un autre écrivain du même registre.

Dans le firmware final, il doit exister **un seul propriétaire du slot tone**.

Après abandon du Wi‑Fi et des calibrations concurrentes, ce propriétaire doit être la boucle de modulation.

---

## 35. `MEMW`

Les routines ROM encadrent les accès tone avec des barrières Xtensa `MEMW`.

Cela signifie que le timing réel inclut :

```text
LX106
 ↓
MEMW
 ↓
MMIO
 ↓
bus périphérique
 ↓
bloc tone
 ↓
DAC/RF
```

La durée d’une instruction CPU n’est donc pas égale au délai RF réel.

---

# PARTIE VIII — INSTRUMENTATION INTERNE

## 36. Voie IQ estimator

Le bloc IQ_EST utilise notamment :

```text
0x6000057C  contrôle
0x60000580
0x60000584
0x60000588
0x6000058C  corrélations
0x600005E4  puissance/énergie utilisée par RXIQ
```

`M=1` est utilisé par les chemins RXIQ de corrélation/puissance.

Cette voie peut comparer des états stables du tone via loopback interne.

---

## 37. Voie TX detector / SAR

`rom_txtone_linear_pwr(n,q)` accumule :

```text
Σ ((out0 << q) / max(out1,1))
```

TXIQ utilise :

```text
rom_txtone_linear_pwr(4, 10)
```

soit une métrique Q10 sur quatre acquisitions.

Les sorties SAR sont reconstruites comme :

```text
out0 = max(2×(s1+s2+s3) - 3×(s6+s7), 0)
out1 = max(3×(s4+s5)    - 3×(s6+s7), 0)
```

Chaque acquisition contient au moins :

```text
ets_delay_us(25)
```

Donc :

```text
txtone_linear_pwr(4,10) ≥ 100 µs de délais explicites
```

Cette voie est utile pour :

- comparer ON/OFF steady-state ;
- comparer plusieurs niveaux ASK ;
- calibrer une table M-ASK relative.

Elle n’est pas assez rapide pour mesurer directement un front OOK très court.

---

# PARTIE IX — PROCÉDURE DE VALIDATION

## 38. Validation OOK

Dans un environnement RF contrôlé :

1. préparer RF/PLL/TX ;
2. initialiser le slot normal ;
3. mesurer l’état bit18=1 ;
4. mettre bit18=0 sans couper TX clock ;
5. mesurer l’état OFF ;
6. répéter pour vérifier stabilité ;
7. mesurer le front avec instrumentation externe rapide.

À extraire :

```text
P_ON
P_OFF
extinction = P_ON - P_OFF
t_on
t_off
jitter
phase OFF→ON si instrument disponible
```

---

## 39. Validation ASK

Pour chaque `digital_scale` choisi :

1. garder RF, PLL, TX clock et bit18 constants ;
2. écrire uniquement bits17:10 ;
3. attendre l’état stable ;
4. mesurer niveau interne SAR et niveau RF externe ;
5. construire une table :

```text
digital_scale → niveau relatif → dBm mesuré
```

Puis choisir les symboles ASK de manière à obtenir des niveaux réellement séparables.

---

## 40. Ce qu’il ne faut pas faire

### Ne pas faire OOK avec `stop_tx_tone()` par symbole

Mauvais :

```text
start / stop / start / stop
```

car `stop_tx_tone()` coupe la TX clock.

### Ne pas utiliser le scale analogique comme modulateur rapide

Il passe par I²C interne ; le RMW numérique est le chemin rapide démontré.

### Ne pas réactiver simplement bit18 sur un slot inconnu

Toujours normaliser d’abord le slot 1.

### Ne pas écraser le registre entier pour un symbole

Toujours modifier uniquement le champ concerné.

### Ne pas laisser sleep/wakeup réinitialiser la RF

Le générateur suppose un état RF/PLL stable.

### Ne pas supposer que `digital_scale=0` signifie RF=0

Le code numérique est connu ; l’effet RF doit être mesuré.

---

# PARTIE X — ÉTAT FINAL DU MODÈLE

## 41. OOK canonique

```text
registre : 0x600005B8
champ    : bit18

OFF = clear bit18
ON  = set bit18

PLL       : stable
TX clock  : ON
RF/XPD    : préparé
scale     : stable
control   : stable
```

---

## 42. ASK canonique

```text
registre : 0x600005B8
champ    : bits17:10

code = ((-digital_scale) & 0xff) << 10
```

Mise à jour :

```text
slot = (slot & ~0x0003FC00) | code
```

Pendant M-ASK :

```text
bit18 = 1
TX clock = ON
RF/PLL = stables
analog scale = fixe
```

---

## 43. Architecture complète recommandée

```text
                   ESP8266
                      │
                      ▼
             boot + calibration
                      │
                      ▼
               canal / RF PLL
                      │
                      ▼
              abandon du Wi-Fi
                      │
                      ▼
        PBUS / RX off / TX XPD on
                      │
                      ▼
                TX clock ON
                      │
                      ▼
      slot1 normalisé : mode_code=1
                      │
        ┌─────────────┴─────────────┐
        │                           │
        ▼                           ▼
       OOK                         ASK
    bit18 RMW                  bits17:10 RMW
        │                           │
        └─────────────┬─────────────┘
                      ▼
                  DAC / RF
```

---

## 44. Frontière exacte entre “décodé” et “à mesurer”

### Décodé

```text
quel registre écrire
quel bit pour OOK
quel masque pour ASK
comment encoder le scale
comment modifier à chaud
pourquoi garder TX clock active
comment ne pas détruire tone_control
comment normaliser le slot
comment séparer scale numérique et analogique
quels instruments internes utiliser
```

### À mesurer

```text
combien de dB donne chaque scale
combien de dB d'extinction donne bit18=0
combien de ns/µs prend un front RF
combien de jitter existe
si la phase continue pendant OFF
combien de niveaux M-ASK sont réellement utilisables
```

---

# PARTIE XI — NOTE RF / CONFORMITÉ

## 45. Utilisation en laboratoire

Le générateur agit dans la chaîne RF 2,4 GHz de l’ESP8266. Les essais doivent être réalisés de manière à ne pas perturber d’autres systèmes radio.

Bonnes pratiques :

- environnement blindé ou fortement atténué ;
- puissance minimale nécessaire ;
- charge/atténuation et instrumentation adaptées lorsqu’une sortie conduite est possible ;
- respect des règles locales applicables aux émissions RF ;
- ne pas utiliser cette technique pour brouiller ou perturber des réseaux tiers.

---

# PARTIE XII — RÉFÉRENCE RAPIDE

## 46. Cheat-sheet

```text
TONE SLOT 1       = 0x600005B8
TONE SLOT 2       = 0x600005BC
TONE SLOT 3       = 0x600005C4

OOK GATE           = bit18
OOK MASK           = 0x00040000

ASK FIELD          = bits17:10
ASK MASK           = 0x0003FC00
ASK SHIFT          = 10
ASK ENCODING       = (-digital_scale) & 0xff
DIGITAL SCALE      = 0..63 (chemin canonique)

TONE CONTROL       = partie basse observée
SAFE LOCAL MASK    = 0x000003ff

MODE NORMAL        = 1

OOK SYMBOL:
    RMW bit18 only

ASK SYMBOL:
    RMW bits17:10 only

NE PAS FAIRE:
    stop_tx_tone() entre symboles
    analog I2C entre symboles ASK rapides
    écrire tout le slot à chaque symbole
    réutiliser un slot post-TXIQ sans normalisation
```

---

## 47. Pseudo-code final minimal

```c
rf_phy_boot_and_calibrate_once();
set_channel_and_lock_rf();
disable_wifi_protocol_forever_but_keep_rf_awake();
prepare_pbus_tx_path();
set_base_analog_power_once();
set_tx_clock(true);

tone1_init(TONE_CONTROL, INITIAL_DIGITAL_SCALE, false);

for (;;) {
    switch (next_mode()) {
    case MOD_OOK:
        tone_ook(next_bit());
        break;

    case MOD_ASK:
        tone_ook(true);
        tone_ask(next_ask_level());
        break;
    }

    wait_symbol_boundary();
}
```

Les fonctions de haut niveau `rf_phy_boot_and_calibrate_once()`, `disable_wifi_protocol_forever_but_keep_rf_awake()` et `prepare_pbus_tx_path()` représentent l’intégration système. Le mécanisme OOK/ASK lui-même est entièrement défini par les primitives du présent document.

---

# 48. Conclusion officielle du projet pour OOK/ASK

Pour le corpus ESP8266 étudié, la commande logicielle du générateur OOK/ASK est considérée **fermée** :

```text
OOK = gate bit18 de 0x600005B8
ASK = modification à chaud de bits17:10 de 0x600005B8
```

Le chemin rapide ne doit pas couper TX clock ni RF entre symboles.

Le scale analogique sert de réglage de plage ; le scale numérique sert à la modulation ASK rapide.

Le slot doit être normalisé une fois en mode tone normal avant toute modulation.

Les valeurs RF absolues, latences et phénomènes de phase sont volontairement exclus du terme « décodage logiciel » et doivent être obtenus expérimentalement.

---

## 49. Statut documentaire

**Document :** `ESP8266_ASK_OOK_REFERENCE_OFFICIELLE_PROJET_v1.0.md`  
**Édition :** 1.0  
**Émission :** unique  
**Maintenance future :** aucune — document figé  
**Suite du projet :** retour au document maître vivant et poursuite du reverse-engineering, notamment FSK/M-FSK.

