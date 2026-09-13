# ESP8266 — Récepteur FSK / M-FSK
## Référence technique officielle du projet de reverse-engineering — périmètre logiciel/statique

**Édition unique : v1.0**  
**Date : 2026-09-13**  
**Statut : FIGÉE — fermeture à 100 % du périmètre logiciel/statique RX FSK**  
**Portée : ESP8266 / mask-ROM + PHY v6 du corpus analysé**

> **Important — statut du mot “officiel”.** Ce document est la référence officielle **du projet de reverse-engineering mené sur le corpus analysé**. Il ne s’agit pas d’un document officiel d’Espressif et il ne doit pas être présenté comme tel.

> **Important — portée du mot “récepteur FSK”.** Le chemin logiciel de lecture/discrimination CFO est fermé à 100 %. La capacité du baseband à produire une **nouvelle mesure CFO valide sur un tone/FSK non‑802.11** reste une validation silicium/baseband séparée. Cette validation physique n’est pas incluse dans le score logiciel.

> **Règle de maintenance.** Cette édition est figée pour le périmètre logiciel/statique. Les validations sur silicium, mesures de cadence, sensibilité, comportement sur tone non‑802.11 et optimisations de démodulation continuent dans le document maître vivant `ESP8266_WIFI_PHY_REVERSE_ENGINEERING.md`.

---

## 1. Objet

Ce document décrit le **chemin logiciel/statique canonique** permettant d’exploiter la mesure de fréquence reçue (CFO) de l’ESP8266 comme métrique de discrimination pour **2-FSK et M-FSK**.

La cible logicielle est :

```text
RF RX / baseband
    ↓
résultat CFO hardware
0x60009800
    ↓
gate contexte WDEV
    ↓
bit0 valid ?
    ↓
raw signé [15:8]
    ↓
(raw × 107) >> 6
    ↓
métrique CFO
    ↓
classification FSK / M-FSK
    ↓
handshake final
0x600098DC |= 0xF
```

La cible n’est pas :

- d’utiliser le RSSI d’un paquet ;
- d’utiliser la métrique E4 d’IQ_EST comme discriminateur de fréquence général ;
- de confondre mesure CFO et correction RFPLL ;
- d’utiliser `phy_freq_offset` comme si c’était la mesure reçue ;
- de dépendre d’une conversion absolue en Hz pour classer des symboles ;
- de prétendre que le corpus statique prouve déjà un CFO frais sur n’importe quel tone non‑802.11.

---

## 2. Définition du « 100 % » dans cette référence

Le statut :

```text
RX FSK logiciel/statique = 100 %
```

signifie :

> **100 % du chemin CPU/logiciel de lecture CFO, de validation, de conversion, de finalisation et de séparation mesure/correction inclus dans cette référence est directement démontré par `libphy.a`, `libpp.a`, `libnet80211.a`, le DWARF, les relocations, les accès MMIO et la mask-ROM du corpus analysé.**

Les points suivants sont **hors score logiciel** :

```text
instant physique exact où 0x60009800.bit0 monte
cause baseband exacte de cette montée
CFO frais sur tone non-802.11
cadence de renouvellement de la mesure
latence / jitter hardware
sensibilité RF FSK
BER selon déviation / SNR
déviation minimale détectable
```

### 2.1 Tableau final de fermeture logicielle

| Élément RX FSK canonique | Statut |
|---|---:|
| registre résultat CFO `0x60009800` | **100 % logiciel** |
| validité utilisée `bit0` | **100 % logiciel** |
| champ CFO brut `[15:8]` | **100 % logiciel** |
| interprétation signée 8 bits | **100 % logiciel** |
| conversion `(raw*107)>>6` | **100 % logiciel** |
| sentinelle invalide `0x7FFF` | **100 % logiciel** |
| gate WDEV `0x3FF2003C[19:16] < 8` | **100 % logiciel** |
| stockage `phy_meas_freq_offset` | **100 % logiciel** |
| séparation `phy_meas_freq_offset` / `phy_freq_offset` | **100 % logiciel** |
| registre handshake `0x600098DC` | **100 % logiciel** |
| masque handshake `[3:0] = 0xF` | **100 % logiciel** |
| RMW + préservation des autres bits | **100 % logiciel** |
| handshake exécuté aussi sur lecture invalide | **100 % logiciel** |
| absence de deuxième arm/clear CPU dans le corpus | **100 % corpus analysé** |
| consommation standard post-RX-success | **100 % logiciel** |
| bit8 WDEV = gate du chemin RX principal | **100 % structurel logiciel** |
| rôle électrique individuel ACK/CLEAR/REARM | **hors score logiciel** |
| CFO frais sur tone non-802.11 | **validation silicium/baseband** |

---

## 3. Corpus et fonctions utilisées

Le modèle repose sur :

- `libphy.a` exact ;
- `libpp.a` exact ;
- `libnet80211.a` exact ;
- mask-ROM ESP8266 64 KiB ;
- DWARF du `wdev.o` et des objets PHY ;
- relocations exactes ;
- scans des sections exécutables et des accès MMIO reconstruits par base+offset.

Fonctions / symboles principaux :

```text
phy_get_bb_freqoffset
phy_get_bb_evm
phy_get_freq_param
wDev_ProcessRxSucData
wDev_ProcessFiq
HdlChlFreqCal
chip_v6_set_chan_offset
set_rf_freq_offset
```

États principaux :

```text
phy_meas_freq_offset
phy_freq_offset
```

---

# PARTIE I — RÉSULTAT CFO

## 4. Registre principal

Le résultat CFO est lu à :

```text
0x60009800
```

Le getter utilise :

```text
bit0      → condition de validité CFO
bits15:8  → CFO brut 8 bits
```

Le registre contient aussi d’autres métriques, notamment une zone EVM, mais le chemin CFO utilise spécifiquement les champs ci-dessus.

---

## 5. Gate de contexte WDEV

Avant de lire le CFO, le getter lit :

```text
0x3FF2003C
```

et extrait :

```text
bits19:16
```

Condition :

```text
si champ >= 8
    → CFO invalide
```

Donc la condition canonique est :

```text
((REG32(0x3FF2003C) >> 16) & 0xF) < 8
```

Le nom électrique exact de ce champ reste hors périmètre.

Le terme recommandé est :

```text
WDEV CFO context/state gate
```

---

## 6. Sentinelle invalide

La valeur :

```text
0x7FFF
```

est utilisée lorsque :

```text
gate WDEV invalide
ou
0x60009800.bit0 == 0
```

Constante :

```c
#define CFO_INVALID ((int16_t)0x7FFF)
```

---

## 7. CFO brut signé

Sur le chemin valide :

```text
raw_u8 = 0x60009800[15:8]
```

puis :

```text
raw_s8 = sign_extend(raw_u8)
```

Le champ est donc une valeur signée sur 8 bits.

---

## 8. Conversion exacte

La formule reconstruite est :

```text
CFO = (raw_s8 × 107) >> 6
```

soit :

```text
≈ raw_s8 × 1,671875
```

en arithmétique entière signée.

La sortie publique historique est classée en kHz à très haute confiance, mais un démodulateur relatif n’a pas besoin d’une calibration absolue de l’unité.

---

# PARTIE II — HANDSHAKE CFO

## 9. Registre de finalisation

Après chaque tentative CFO, le getter utilise :

```text
0x600098DC
```

Le comportement exact est :

```c
r = REG32(0x600098DC);
r |= 0x0000000F;
REG32(0x600098DC) = r;
```

avec barrières `MEMW`.

---

## 10. Le handshake est inconditionnel

Le getter possède un épilogue commun.

Les chemins :

```text
CFO valide
bit0 invalide
gate WDEV invalide
```

convergent tous vers :

```text
MEMW
read 0x600098DC
OR 0xF
MEMW
write 0x600098DC
store phy_meas_freq_offset
return
```

Donc :

> **le handshake doit être exécuté après toute tentative, y compris lorsque la valeur retournée est `0x7FFF`.**

---

## 11. Rôle logiciel final de `0x600098DC[3:0]`

Le comportement logiciel est fermé comme :

```text
finalisation / handshake du cycle CFO
```

Le sous-rôle électrique exact :

```text
ACK
CLEAR
REARM
W1C
strobe
```

n’est pas requis pour reproduire le logiciel.

---

## 12. Aucun second re-arm CPU

Le scan exhaustif du corpus ne retrouve aucune séquence CPU séparée :

```text
clear CFO
arm CFO
start CFO
```

Le chemin standard est :

```text
hardware produit ou non le CFO
    ↓
getter CFO
    ↓
lecture / conversion / sentinelle
    ↓
0x600098DC |= 0xF
    ↓
retour
```

---

# PARTIE III — PRIMITIVE CFO CANONIQUE

## 13. Registres et constantes

```c
#include <stdint.h>
#include <stdbool.h>

#define WDEV_CFO_CTX_ADDR   0x3FF2003Cu
#define BB_CFO_RESULT_ADDR  0x60009800u
#define BB_CFO_HS_ADDR      0x600098DCu

#define CFO_INVALID         ((int16_t)0x7FFF)
```

---

## 14. Discipline MMIO

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

## 15. Primitive canonique reconstruite

```c
static inline int16_t fsk_cfo_read(void)
{
    int16_t out = CFO_INVALID;

    uint32_t ctx = rd32(WDEV_CFO_CTX_ADDR);

    if (((ctx >> 16) & 0x0Fu) < 8u) {
        uint32_t bb = rd32(BB_CFO_RESULT_ADDR);

        if (bb & 1u) {
            int8_t raw = (int8_t)((bb >> 8) & 0xFFu);
            out = (int16_t)(((int32_t)raw * 107) >> 6);
        }
    }

    memw();
    uint32_t h = rd32(BB_CFO_HS_ADDR);
    h |= 0x0Fu;
    memw();
    wr32(BB_CFO_HS_ADDR, h);

    return out;
}
```

Cette primitive reproduit le **contrat CPU observé**.

---

## 16. Variante avec statut explicite

```c
static inline bool fsk_cfo_try_read(int16_t *out)
{
    int16_t v = fsk_cfo_read();

    if (v == CFO_INVALID)
        return false;

    *out = v;
    return true;
}
```

Le caller ne doit jamais classifier un symbole à partir de `0x7FFF`.

---

# PARTIE IV — MESURE VS CORRECTION

## 17. `phy_meas_freq_offset`

Le getter CFO stocke son résultat dans :

```text
phy_meas_freq_offset
```

Cette variable représente la dernière mesure CFO calculée/lue.

---

## 18. `phy_freq_offset`

Une variable distincte :

```text
phy_freq_offset
```

est utilisée par les chemins de correction / canal / retuning.

Il est incorrect de les confondre.

---

## 19. `phy_get_freq_param()`

Le chemin reconstruit sépare explicitement :

```text
*corr_out = phy_freq_offset
*meas_out = phy_meas_freq_offset
```

La distinction mesure/correction est donc fermée.

---

# PARTIE V — CONSOMMATION STANDARD WDEV / PP

## 20. Consommation CFO après RX-success

Dans le SDK standard :

```text
wDev_ProcessFiq()
    ↓
traitement RX
    ↓
discard ou success
    ↓ success
wDev_ProcessRxSucData()
    ↓
phy_get_bb_freqoffset()
```

Le getter CFO standard n’est pas appelé sur la branche discard.

---

## 21. Événement WDEV bit8

Le dispatcher lit :

```text
0x3FF20C20
```

comme mot d’événements WDEV.

Le bit :

```text
bit8
```

gate le grand chemin de traitement RX principal.

Il est observé **avant** la décision finale :

```text
discard
ou
RX-success
```

Cela fournit un repère chronologique matériel, mais ne prouve pas que le CFO est déjà valide à cet instant.

---

## 22. Pourquoi cela ne limite pas la fermeture logicielle

Le logiciel standard choisit de consommer le CFO après RX-success.

La question :

```text
le baseband avait-il déjà produit le CFO avant ?
```

est une propriété de timing matériel.

Elle est donc séparée du contrat CPU du getter.

---

# PARTIE VI — CLASSIFICATION FSK

## 23. Principe 2-FSK

Si deux états fréquentiels produisent des centres CFO stables :

```text
μ0
μ1
```

on peut définir :

```text
T = (μ0 + μ1) / 2
```

Puis classifier selon l’ordre observé :

```text
si μ1 > μ0 :
    symbole 1 ⇔ CFO > T
sinon :
    symbole 1 ⇔ CFO < T
```

Cette classification est une architecture logicielle de projet dérivée de la métrique CFO.

---

## 24. Pourquoi l’unité absolue n’est pas obligatoire

Comme pour l’ASK en unités E4, un discriminateur relatif peut travailler directement dans l’espace de sortie CFO.

La seule exigence est :

```text
classes séparables
```

Il n’est donc pas nécessaire de convertir la mesure en fréquence RF absolue pour classer deux états.

---

## 25. M-FSK

Pour M niveaux :

```text
μ0, μ1, ... μ(M-1)
```

trier les centres par valeur CFO.

Seuils :

```text
Ti = (μi + μ(i+1)) / 2
```

Puis classer la mesure dans l’intervalle correspondant.

---

## 26. Valeurs invalides

Lorsqu’une acquisition retourne :

```text
0x7FFF
```

elle doit être traitée comme :

```text
aucune décision valide
```

et non comme un niveau M-FSK.

Le firmware doit prévoir :

```text
erasure
retry
resynchronisation
ou perte de symbole
```

selon son protocole.

---

# PARTIE VII — ARCHITECTURE AUTONOME CANDIDATE

## 27. Architecture logicielle

La cible autonome candidate est :

```text
initialisation RF/PHY une fois
    ↓
canal / PLL fixes
    ↓
RX/baseband maintenus
    ↓
lecture CFO
    ↓
validation
    ↓
classification FSK
    ↓
handshake
    ↓
répéter
```

---

## 28. Point non encore démontré physiquement

Le corpus ne démontre pas à lui seul :

```text
tone non-802.11
    ↓
bit0 = 1
    ↓
raw CFO frais
```

Cette propriété doit être vérifiée sur silicium.

---

## 29. Pourquoi cette validation n’annule pas le 100 % logiciel

Le logiciel est complètement défini pour les deux cas :

```text
CFO valide
CFO invalide
```

Ce qui reste à déterminer est si le **hardware** produit une valeur valide pour le signal cible.

C’est une qualification fonctionnelle du bloc baseband, pas une fonction CPU inconnue.

---

# PARTIE VIII — PROCÉDURE DE VALIDATION SILICIUM

## 30. Test A — trame Wi-Fi valide

Établir la référence :

```text
signal avec CFO connu
    ↓
0x60009800.bit0
[15:8]
sortie getter
```

Vérifier la cohérence de signe et de variation.

---

## 31. Test B — entrée du chemin WDEV bit8

Au moment où :

```text
event.bit8 = 1
```

capturer passivement :

```text
0x60009800
0x600098DC
0x3FF2003C
CCOUNT
```

sans appeler le getter avant la capture.

---

## 32. Test C — trame finalement rejetée

Capturer le CFO avant la décision discard.

Si :

```text
bit0 = 1
```

sur une frame ensuite rejetée, cela prouve que le CFO peut être produit avant RX-success.

---

## 33. Test D — tone / FSK non-802.11

Injecter un signal à fréquence décalée connue.

Observer si :

```text
0x60009800.bit0
```

revient à 1 et si :

```text
[15:8]
```

suit le décalage.

Résultat positif :

```text
RX FSK autonome matériellement validé
```

Résultat négatif :

```text
estimateur dépendant d’une synchronisation PHY
```

et une investigation supplémentaire du synchroniseur serait nécessaire.

---

# PARTIE IX — TIMING ET ACQUISITION

## 34. Ne pas confondre cadence logicielle et cadence hardware

La fonction CPU peut être appelée rapidement.

Mais la cadence de nouvelles valeurs dépend de :

```text
baseband
synchronisation
estimateur CFO
cycle de handshake
```

La cadence symbolique maximale ne peut donc pas être dérivée du nombre d’instructions CPU seul.

---

## 35. Handshake après chaque tentative

Même lorsqu’une lecture est invalide :

```text
result = 0x7FFF
```

il faut quand même reproduire :

```text
0x600098DC |= 0xF
```

si l’on remplace le getter standard par une primitive directe.

---

# PARTIE X — CE QU’IL NE FAUT PAS FAIRE

## 36. Ne pas utiliser `phy_freq_offset` comme mesure RX

C’est l’état de correction, pas la mesure brute reçue.

---

## 37. Ne pas ignorer la sentinelle `0x7FFF`

Une valeur invalide ne doit jamais être classée comme symbole.

---

## 38. Ne pas omettre le handshake sur erreur

Le handshake est inconditionnel dans le getter exact.

---

## 39. Ne pas appeler précocement le getter comme simple sonde passive

Le getter modifie :

```text
0x600098DC
```

Une instrumentation précoce doit lire directement le résultat sans provoquer le handshake avant la capture.

---

## 40. Ne pas assimiler `0x3FF2003C[19:16]` à `rxend_state`

Le premier est un champ MMIO WDEV de 4 bits utilisé comme gate CFO.

`RxControl.rxend_state` est un champ de descripteur distinct.

Aucune identité binaire directe n’est démontrée.

---

## 41. Ne pas utiliser IQ_EST E4 comme discriminateur FSK général

E4 est une métrique d’énergie/puissance adaptée à OOK/ASK.

Elle ne remplace pas le CFO pour une FSK générale.

---

## 42. Ne pas présenter le CFO non-802.11 comme déjà validé

Le logiciel est fermé ; la disponibilité autonome de la métrique reste une validation matérielle.

---

# PARTIE XI — FRONTIÈRE EXACTE DU MODÈLE

## 43. Décodé à 100 % côté logiciel/statique

```text
adresse résultat CFO
valid bit
champ raw
signe
conversion
sentinelle
gate WDEV
stockage mesure
séparation mesure/correction
handshake
masque handshake
RMW
MEMW
handshake sur succès et échec
absence de second re-arm CPU
moment de consommation standard
repère WDEV bit8
```

---

## 44. À mesurer sur silicium/baseband

```text
bit0 valide à event.bit8 ?
CFO sur frame rejetée ?
CFO sur tone non-802.11 ?
cadence de renouvellement ?
latence ?
déviation minimale détectable ?
sensibilité ?
BER ?
```

---

# PARTIE XII — RÉFÉRENCE RAPIDE

## 45. Cheat-sheet

```text
CFO RESULT:
    0x60009800

VALID:
    bit0

RAW CFO:
    bits15:8
    signed int8

CONVERSION:
    cfo = (raw * 107) >> 6

INVALID:
    0x7FFF

WDEV CONTEXT GATE:
    0x3FF2003C[19:16] < 8

HANDSHAKE:
    0x600098DC
    RMW |= 0xF
    exécuté même sur invalid

MEASUREMENT STATE:
    phy_meas_freq_offset

CORRECTION STATE:
    phy_freq_offset

STANDARD CONSUMPTION:
    post-RX-success

WDEV RX GATE:
    event bit8

AUTONOMOUS NON-WIFI CFO:
    validation silicium requise
```

---

## 46. Pseudo-code final minimal

```c
bool rx_fsk_sample(int16_t *cfo)
{
    uint32_t ctx = rd32(0x3FF2003Cu);
    int16_t v = CFO_INVALID;

    if (((ctx >> 16) & 0x0Fu) < 8u) {
        uint32_t r = rd32(0x60009800u);

        if (r & 1u) {
            int8_t raw = (int8_t)((r >> 8) & 0xFFu);
            v = (int16_t)(((int32_t)raw * 107) >> 6);
        }
    }

    memw();
    uint32_t h = rd32(0x600098DCu);
    h |= 0x0Fu;
    memw();
    wr32(0x600098DCu, h);

    if (v == CFO_INVALID)
        return false;

    *cfo = v;
    return true;
}
```

---

## 47. Exemple conceptuel 2-FSK

```c
bool demod_2fsk(int16_t cfo,
                int16_t threshold,
                bool one_is_high)
{
    return one_is_high
        ? (cfo > threshold)
        : (cfo < threshold);
}
```

Le seuil doit être appris ou calibré à partir des deux centres reçus.

---

## 48. Conclusion officielle du projet pour RX FSK/M-FSK

Pour le corpus analysé, le projet considère désormais **fermé à 100 % au périmètre logiciel/statique** le chemin CPU CFO nécessaire à un discriminateur FSK :

```text
0x60009800
    ↓
gate + valid
    ↓
raw CFO signé
    ↓
conversion
    ↓
classification logicielle
    ↓
0x600098DC |= 0xF
```

La séparation avec la correction RF est fermée :

```text
phy_meas_freq_offset
    ≠
phy_freq_offset
```

Le protocole CPU ne contient aucun second re-arm caché dans le corpus.

La seule frontière restante est physique :

> **le baseband ESP8266 produit-il une nouvelle mesure CFO valide pour un signal FSK/tone non‑802.11 dans l’état autonome visé ?**

Cette question relève de la validation silicium/baseband et n’est pas une lacune du reverse-engineering logiciel.

---

## 49. Statut documentaire

```text
Document : référence RX FSK/M-FSK du projet
Version  : 1.0
Statut   : FIGÉE — périmètre logiciel/statique
Base     : mask-ROM ESP8266 + PHY v6 + PP/WDEV du corpus analysé
Suite    : validation CFO autonome sur silicium dans le maître vivant
```
