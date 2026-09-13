# ESP8266 — Générateur FSK / M-FSK autonome
## Référence technique officielle du projet de reverse-engineering

**Édition unique : v1.0**  
**Date : 2026-09-13**  
**Statut : FIGÉE — fermeture à 100 % du périmètre logiciel/statique TX FSK**  
**Portée : ESP8266 / mask-ROM + PHY v6 du corpus analysé**

> **Important — statut du mot “officiel”.** Ce document est la référence officielle **du projet de reverse-engineering mené sur le corpus analysé**. Il ne s’agit pas d’un document officiel d’Espressif et il ne doit pas être présenté comme tel.

> **Règle de maintenance.** Cette édition est volontairement figée. Les découvertes ultérieures, mesures RF, lois `tone_control→Hz`, optimisations de débit ou caractérisations de phase/settling ne modifieront pas cette édition ; elles continuent dans le document maître vivant `ESP8266_WIFI_PHY_REVERSE_ENGINEERING.md`.

---

## 1. Objet

Ce document décrit le **chemin logiciel/statique canonique** permettant d’utiliser le générateur de tone de l’ESP8266 comme actionneur de **2-FSK et M-FSK**, après une initialisation RF/PHY unique et sans revenir au trafic Wi‑Fi normal entre les symboles.

La cible n’est pas :

- de transmettre des trames 802.11 ;
- d’effectuer un changement complet de canal pour chaque symbole ;
- d’utiliser `set_rf_freq_offset()` comme modulateur rapide ;
- de relocker/recalibrer la RFPLL à chaque symbole ;
- de transformer l’ESP8266 en SDR I/Q généraliste ;
- de prétendre connaître statiquement la relation physique `tone_control → Hz`.

La cible logicielle est :

```text
BOOT
  ↓
initialisation / calibration RF-PLL-PHY une fois
  ↓
abandon du trafic Wi-Fi normal
  ↓
canal / RFPLL fixes
  ↓
RF TX + TX clock maintenues
  ↓
normalisation du tone slot 1
  ↓
gate bit18 maintenu actif
scale bits17:10 maintenu fixe
  ↓
┌───────────────────────────────────────────────┐
│ 2-FSK  : tone_control K0 ↔ K1               │
│ M-FSK  : tone_control K0 ... K(M-1)          │
└───────────────────────────────────────────────┘
  ↓
RMW ciblé du champ bas du slot
```

La réponse physique exacte du générateur après un changement `K0→K1` reste une caractérisation silicium/RF.

---

## 2. Définition du « 100 % » dans cette référence

Le statut :

```text
TX FSK logiciel/statique = 100 %
```

signifie :

> **100 % du chemin de commande logiciel canonique inclus dans cette référence est directement démontré ou fermé par la mask-ROM, `libphy.a`, les relocations, les accès MMIO, le packing du tone generator et la séparation des chemins RFPLL du corpus analysé.**

Les grandeurs suivantes ne sont pas classées comme lacunes du reverse-engineering logiciel :

```text
tone_control → fréquence RF exacte
Δf entre deux codes K
latence write MMIO → nouveau tone
settling K0→K1
continuité de phase
jitter du changement de fréquence
transitoires spectraux
wrap/modulo interne du générateur
clock exacte du phase accumulator / NCO
largeur exacte de l’accumulateur
débit FSK maximal réel
puissance absolue en dBm
```

Ces grandeurs dépendent du silicium et doivent être mesurées expérimentalement.

### 2.1 Tableau final de fermeture logicielle

| Élément TX FSK canonique | Statut |
|---|---:|
| adresse du tone slot 1 `0x600005B8` | **100 % logiciel** |
| injection brute de `tone_control` dans le champ bas | **100 % logiciel** |
| séparation `tone_control` / scale `bits17:10` | **100 % logiciel** |
| gate normal bit18 | **100 % logiciel** |
| conservation du scale pendant la FSK | **100 % architecture logicielle** |
| maintien de la TX clock entre symboles | **100 % logiciel** |
| normalisation du slot avant modulation | **100 % logiciel** |
| primitive RMW ciblée du champ `tone_control` | **100 % définie** |
| discipline `MEMW` autour du MMIO | **100 % logiciel** |
| séparation tone-step / `set_rf_freq_offset()` | **100 % architecture logicielle** |
| `set_rf_freq_offset()` = voie RFPLL/recalibration | **100 % fonctionnellement classé** |
| absence de conversion logicielle `tone_control→Hz` dans le corpus | **100 % démontré** |
| distinction logiciel / caractérisation RF | **100 % définie** |

Aucun élément logiciel inconnu n’est requis pour décrire le chemin canonique de commande TX FSK de cette référence.

---

## 3. Corpus et fonctions utilisées

Le modèle repose sur :

- mask-ROM ESP8266 analysée ;
- `libphy.a` / PHY v6 analysé ;
- `libpp.a` et `libnet80211.a` audités pour les interactions concurrentes ;
- `g_phyFuns` / `phy_func_tab` ;
- chemins `rfcal_pwrctrl`, `rfcal_rxiq`, `rfcal_txcap`, TXIQ ;
- accès directs au bloc tone ;
- analyse de `set_rf_freq_offset()`, `ram_rfpll_set_freq()` et de la construction SDM RFPLL.

Fonctions ROM / PHY directement pertinentes :

```text
rom_set_txclk_en
rom_set_ana_inf_tx_scale
rom_start_tx_tone
rom_stop_tx_tone
rom_txtone_linear_pwr
rom_pbus_xpd_tx_off
rom_pbus_xpd_tx_on
rom_pbus_set_txgain
ram_rfpll_set_freq
set_rf_freq_offset
wait_rfpll_cal_end
```

Adresses ROM démontrées pour les primitives tone :

```text
rom_start_tx_tone       ≈ 0x400068B4
rom_stop_tx_tone        ≈ 0x4000698C
rom_txtone_linear_pwr   ≈ 0x40006A1C
```

---

## 4. Registres du générateur de tone

Trois slots sont présents :

```text
0x600005B8   TONE SLOT 1
0x600005BC   TONE SLOT 2
0x600005C4   TONE SLOT 3
```

Pour la référence FSK canonique, le **slot 1** est utilisé.

Les usages ROM directs retrouvés pour les calibrations principales utilisent le slot 1.

---

## 5. Packing du slot 1

Le comportement reconstruit de `rom_start_tx_tone()` est :

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
│           │ bit18 = gate normal  │ 8 bits       │ stimulus / step      │
└───────────┴──────────────────────┴──────────────┴──────────────────────┘
```

Le point essentiel pour le FSK est :

```text
tone_control
    ≠ digital_scale
    ≠ gate bit18
    ≠ RFPLL
```

---

## 6. Largeur pratique du champ `tone_control`

La ROM ne réalise pas explicitement :

```c
raw_control &= 0x3ff;
```

avant l’OR.

Le modèle de packing donne néanmoins un espace naturel de 10 bits pour la partie basse.

Pour un firmware autonome, la protection locale recommandée est :

```c
#define TONE_CONTROL_MASK 0x000003FFu
```

puis :

```c
control_safe = tone_control & TONE_CONTROL_MASK;
```

Ce masque est une **discipline du firmware autonome**, pas un masque observé dans `rom_start_tx_tone()`.

---

# PARTIE I — PRINCIPE FSK

## 7. Principe 2-FSK

La 2-FSK encode deux symboles par deux états de commande du générateur :

```text
symbole 0 → K0
symbole 1 → K1
```

Architecture :

```text
RFPLL      fixe
canal      fixe
TX RF      actif
TX clock   active
gate       actif
scale      fixe
tone_control change
```

Le logiciel ne doit pas réinitialiser le générateur à chaque symbole.

---

## 8. Principe M-FSK

Une constellation M-FSK logicielle est représentée par :

```text
K0, K1, ... K(M-1)
```

Chaque code est écrit dans le même champ bas du slot, sans modifier :

```text
bit18
bits17:10
bits31:28
```

La correspondance physique :

```text
Ki → fi
```

doit être mesurée sur le silicium.

La référence ne suppose ni linéarité, ni symétrie, ni pas constant.

---

## 9. Valeurs réellement observées dans le corpus

Les valeurs distinctes directement démontrées dans les chemins analysés sont notamment :

```text
tone_control = 8
tone_control = 64
```

Leur rôle fonctionnel est lié à différents stimuli/calibrations :

```text
8   → chemin RXIQ
64  → TXIQ / power-control / TX-cap / mesure de puissance tone
```

Ces deux valeurs prouvent l’existence de codes distincts utilisés par le générateur.

Elles ne prouvent pas à elles seules :

```text
8  = fréquence F0
64 = fréquence F1
```

ni une loi d’échelle.

---

# PARTIE II — PRIMITIVE TX FSK CANONIQUE

## 10. Masque du champ bas

Pour le firmware autonome :

```c
#define TONE1_ADDR          0x600005B8u
#define TONE_CONTROL_MASK   0x000003FFu
```

---

## 11. Primitive RMW

```c
#include <stdint.h>

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

static inline void fsk_set_control(uint16_t tone_control)
{
    uint32_t r;

    memw();
    r = rd32(TONE1_ADDR);

    r &= ~TONE_CONTROL_MASK;
    r |= ((uint32_t)tone_control & TONE_CONTROL_MASK);

    memw();
    wr32(TONE1_ADDR, r);
}
```

Cette primitive préserve :

```text
scale ASK bits17:10
gate bit18
modes supérieurs
bits31:28
```

---

## 12. Pourquoi le RMW est obligatoire

Il ne faut pas écrire :

```c
REG32(TONE1_ADDR) = K;
```

car cela détruirait :

```text
scale
gate
mode/test
bits préservés
```

La modulation canonique agit uniquement sur le champ ciblé.

---

## 13. Normalisation du slot avant la FSK

Un slot provenant d’une calibration TXIQ peut contenir des bits de test/mode résiduels.

Avant la session FSK, construire explicitement un état normal :

```c
#define TONE_GATE_MASK    0x00040000u
#define TONE_SCALE_SHIFT  10u
#define TONE_SCALE_MASK   0x0003FC00u

static inline uint32_t build_tone1_fsk_normal(
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

Dans le mode FSK :

```text
gate = 1
digital_scale = constant
tone_control = variable par symbole
```

---

# PARTIE III — POURQUOI LA RFPLL N’EST PAS LE MODULATEUR RAPIDE

## 14. `set_rf_freq_offset()` n’est pas un simple offset numérique

Dans le `phy_chip_v6_ana.o` exact :

```text
set_rf_freq_offset
    taille = 0x6D octets
```

Les relocations démontrent :

```text
set_rf_freq_offset()
        ↓
ram_rfpll_set_freq()
        ↓
wait_rfpll_cal_end()
```

La primitive reprogramme donc la RFPLL et attend sa calibration.

---

## 15. Conséquence pour le fast-FSK

La voie :

```text
set_rf_freq_offset()
```

est classée comme :

```text
correction / retuning RFPLL
```

et non comme :

```text
modulation symbole-par-symbole rapide
```

Le fast-FSK canonique utilise donc le tone generator.

---

## 16. `chip_v6_set_chan_offset()` est encore plus lourd

Le chemin de canal/offset implique notamment :

```text
stop RX
changement de canal
BBPLL / RF
recalibration
restart RX
```

Il ne doit pas être utilisé dans la boucle FSK rapide.

---

## 17. Construction SDM RFPLL

Le reverse-engineering ferme aussi la branche lente :

```text
fréquence RF
    ↓
ram_rfpll_set_freq()
    ↓
calcul mot SDM 24 bits
    ↓
rom_write_rfpll_sdm()
    ↓
RFPLL
```

Cette fermeture confirme la séparation architecturale :

```text
tone_control
    ≠
mot SDM RFPLL
```

---

# PARTIE IV — RF / PBUS / TX CLOCK

## 18. Le tone generator n’initialise pas toute la RF

`rom_start_tx_tone()` ne remplace pas une initialisation RF complète.

Les calibrations démontrent le squelette :

```text
RF/PBUS préparés
    ↓
RX RF off
    ↓
TX XPD on
    ↓
gain / scale de base
    ↓
TX clock on
    ↓
tone generator
```

---

## 19. TX clock

`rom_start_tx_tone()` active la TX clock.

`rom_stop_tx_tone()` :

```text
clear bit18
puis
TX clock off
```

La FSK rapide doit donc maintenir la TX clock active entre les symboles.

---

## 20. Ne pas utiliser `stop_tx_tone()` entre symboles

La stratégie incorrecte :

```text
K0 → start
transition
stop
K1 → start
```

réinitialise inutilement le chemin.

La stratégie canonique :

```text
start / préparation une fois
    ↓
TX clock reste active
gate reste actif
    ↓
K0 ↔ K1 par RMW
```

---

# PARTIE V — PROCÉDURE AUTONOME COMPLÈTE

## 21. Phase A — boot / calibration

1. démarrer normalement le silicium ;
2. laisser le PHY effectuer ses initialisations/calibrations ;
3. fixer le canal / RFPLL ;
4. empêcher sleep/wakeup pendant la session ;
5. abandonner le trafic Wi‑Fi normal.

Cette référence ne prétend pas remplacer tout le cold-start RF par quelques écritures isolées.

---

## 22. Phase B — préparation RF TX

À partir d’un état RF initialisé :

```text
PBUS debug/test
RX RF off
TX XPD on
gain TX déterministe
scale de base déterministe
TX clock on
```

---

## 23. Phase C — normalisation du tone

Choisir :

```text
K_INITIAL
digital_scale fixe
mode normal
gate actif
```

Réécrire complètement le slot normal une fois.

---

## 24. Phase D — caractérisation des codes FSK

Avant d’utiliser une constellation :

```text
mesurer f(K0)
mesurer f(K1)
...
mesurer f(KM-1)
```

Puis définir la table :

```c
typedef struct {
    uint16_t control;
    /* fréquence mesurée éventuellement stockée hors boucle */
} fsk_level_t;
```

Cette étape est une caractérisation RF, pas une lacune logicielle.

---

## 25. Phase E — modulation

2-FSK :

```c
static const uint16_t fsk2[2] = {
    K0,
    K1
};

for (;;) {
    uint8_t bit = next_bit();
    fsk_set_control(fsk2[bit & 1u]);
    symbol_delay();
}
```

M-FSK :

```c
static const uint16_t mfsk[4] = {
    K0, K1, K2, K3
};

for (;;) {
    uint8_t sym = next_2_bits();
    fsk_set_control(mfsk[sym & 3u]);
    symbol_delay();
}
```

---

# PARTIE VI — TIMING ET CONCURRENCE

## 26. Propriétaire unique du slot

Le RMW n’est pas atomique vis-à-vis d’un autre écrivain.

Après abandon du Wi‑Fi :

```text
un seul propriétaire du tone slot
    =
boucle de modulation FSK
```

---

## 27. Interruptions

Pour une modulation temporellement propre, éviter les perturbations par :

```text
ISR Wi-Fi
timers PHY de maintenance
sleep/wakeup
reconfiguration canal/PLL
calibrations concurrentes
```

---

## 28. `MEMW`

Les chemins ROM utilisent des barrières Xtensa `MEMW`.

Le chemin réel est :

```text
LX106
  ↓
MEMW
  ↓
MMIO
  ↓
bus périphérique
  ↓
tone generator
  ↓
DAC/RF
```

Le temps CPU seul ne donne donc pas la latence RF.

---

# PARTIE VII — ÉLÉMENTS DÉLIBÉRÉMENT HORS PÉRIMÈTRE LOGICIEL

## 29. Loi `tone_control → Hz`

Aucune formule logicielle démontrée n’existe dans le corpus exact pour convertir :

```text
tone_control
```

en :

```text
Hz
kHz
MHz
```

Le code est injecté brut dans le matériel.

---

## 30. `app_tone_offset_khz` ne ferme pas la loi

Le DWARF expose historiquement :

```text
app_tx_tone
app_tone_offset_khz
```

mais le global interne exact :

```text
chip6_phy_init_ctrl
```

fait seulement 80 octets, plaçant les offsets correspondants hors de l’objet réellement alloué dans ce build.

De plus :

```text
app_test_code()
```

est un stub `RET.N`.

Conclusion :

```text
app_tone_offset_khz
    ≠ preuve de
tone_control → kHz
```

---

## 31. Phase accumulator / NCO

Une loi générique de type :

```text
f = step × Fclk / 2^N
```

serait compatible avec un générateur numérique, mais aucune constante :

```text
Fclk
N
```

n’est démontrée pour ce tone generator ESP8266.

Elle ne doit pas être utilisée comme résultat officiel.

---

## 32. Hot-update : frontière exacte

Le logiciel peut écrire :

```text
K0 → K1
```

par RMW sans appeler `stop_tx_tone()`.

Cela ferme la **capacité logicielle d’écriture à chaud**.

Ce qui reste matériel :

```text
le hardware applique-t-il immédiatement K1 ?
quelle latence ?
quelle phase ?
quel settling ?
```

---

# PARTIE VIII — VALIDATION PHYSIQUE

## 33. Pourquoi une validation silicium reste nécessaire

Le reverse-engineering logiciel répond à :

```text
quel registre écrire
quel champ modifier
quels autres champs préserver
comment garder gate/clock actifs
quelle voie ne pas utiliser
comment séparer tone-step et RFPLL
```

Il ne peut pas déterminer les performances internes du générateur.

---

## 34. Mesures TX à réaliser

Dans un environnement RF contrôlé :

```text
f(K) pour plusieurs K
Δf K0↔K1
latence write→nouvelle fréquence
settling
jitter
phase K0→K1
spectre / transitoires
puissance pour chaque K
débit maximal
```

---

## 35. Validation 2-FSK

Procédure :

1. préparer RF/PLL/TX ;
2. normaliser le slot ;
3. fixer gate et scale ;
4. mesurer `K0` ;
5. écrire `K1` par RMW uniquement ;
6. mesurer fréquence et timing ;
7. revenir à `K0` ;
8. répéter.

Validation fonctionnelle :

```text
K0 et K1 produisent deux états fréquentiels suffisamment séparés
et stables pour le débit choisi
```

---

## 36. Validation M-FSK

Pour chaque `Ki` :

```text
mesurer fi
mesurer variance / settling
```

Puis sélectionner une constellation présentant :

```text
séparation suffisante
puissance comparable
transitoires acceptables
```

---

# PARTIE IX — CE QU’IL NE FAUT PAS FAIRE

## 37. Ne pas utiliser `stop_tx_tone()` par symbole

Il coupe la TX clock.

---

## 38. Ne pas utiliser `set_rf_freq_offset()` par symbole

Il passe par RFPLL + attente de calibration.

---

## 39. Ne pas supposer `tone_control = fréquence en kHz`

Aucune telle identité n’est démontrée.

---

## 40. Ne pas supposer une loi linéaire

Le corpus ne démontre pas :

```text
f ∝ K
```

sur toute la plage.

---

## 41. Ne pas écraser tout le slot

Toujours préserver :

```text
scale
gate
modes
bits supérieurs
```

par RMW ciblé.

---

## 42. Ne pas laisser le Wi‑Fi reprendre le slot

La référence autonome suppose un seul propriétaire du générateur pendant la session.

---

# PARTIE X — ÉTAT FINAL DU MODÈLE

## 43. 2-FSK canonique

```text
RF/PLL préparés
TX clock active
slot normalisé
gate = 1
scale fixe
    ↓
K0 ↔ K1
par RMW du champ bas
```

---

## 44. M-FSK canonique

```text
RF/PLL préparés
TX clock active
slot normalisé
gate = 1
scale fixe
    ↓
K0, K1, ... K(M-1)
par RMW du champ bas
```

---

## 45. Frontière exacte entre « décodé » et « à mesurer »

### Décodé à 100 % côté logiciel/statique

```text
registre tone
packing
gate
scale
champ tone_control
normalisation
RMW
MEMW
TX clock
séparation RFPLL
voie set_rf_freq_offset lente
absence de conversion logicielle K→Hz
```

### À mesurer sur silicium

```text
K→f
latence
settling
phase
jitter
spectre
débit maximal
```

---

# PARTIE XI — NOTE RF / CONFORMITÉ

## 46. Utilisation en laboratoire

La modulation autonome du générateur de tone sort du chemin Wi‑Fi normal.

Les essais doivent être réalisés dans un environnement RF contrôlé et conforme à la réglementation applicable, avec atténuation/charge ou instrumentation adaptée lorsque nécessaire.

Cette note ne modifie pas le modèle logiciel.

---

# PARTIE XII — RÉFÉRENCE RAPIDE

## 47. Cheat-sheet

```text
TONE SLOT 1:
    0x600005B8

FIELD:
    tone_control → partie basse
    protection firmware : & 0x3FF

GATE:
    bit18 = 1 pendant FSK active

SCALE:
    bits17:10 fixes pendant FSK

FAST FSK:
    RMW du champ tone_control
    K0 ↔ K1

NE PAS UTILISER PAR SYMBOLE:
    rom_stop_tx_tone()
    set_rf_freq_offset()
    chip_v6_set_chan_offset()
    changement de canal

TX CLOCK:
    reste active

MEMW:
    conserver autour des MMIO critiques

PHYSICAL CALIBRATION:
    mesurer K → fréquence
```

---

## 48. Pseudo-code final minimal

```c
#define TONE1_ADDR        0x600005B8u
#define TONE_CONTROL_MASK 0x000003FFu

static inline void fsk_write(uint16_t k)
{
    uint32_t r;

    memw();
    r = rd32(TONE1_ADDR);

    r &= ~TONE_CONTROL_MASK;
    r |= ((uint32_t)k & TONE_CONTROL_MASK);

    memw();
    wr32(TONE1_ADDR, r);
}

void send_2fsk_bit(uint8_t bit)
{
    static const uint16_t K[2] = { K0, K1 };
    fsk_write(K[bit & 1u]);
}
```

Les valeurs `K0/K1` sont obtenues après caractérisation RF.

---

## 49. Conclusion officielle du projet pour TX FSK/M-FSK

Pour le corpus analysé, le projet considère désormais fermé à **100 % au périmètre logiciel/statique** le chemin de commande TX FSK :

```text
initialisation RF
    ↓
tone generator
    ↓
slot 1
    ↓
gate / scale fixes
    ↓
RMW tone_control
    ↓
K0 ↔ K1 ↔ ...
```

Le reverse-engineering ne revendique pas comme résultat logiciel une relation physique non observable :

```text
tone_control → Hz
```

La détermination des fréquences réelles, du settling, de la phase, du jitter et des performances spectrales appartient à la **validation silicium/RF**.

Cette séparation est volontaire et constitue la définition officielle du « 100 % » de cette référence.

---

## 50. Statut documentaire

```text
Document : référence TX FSK/M-FSK du projet
Version  : 1.0
Statut   : FIGÉE
Base     : mask-ROM ESP8266 + PHY v6 + corpus analysé
Suite    : caractérisation RF et optimisations dans le maître vivant
```
