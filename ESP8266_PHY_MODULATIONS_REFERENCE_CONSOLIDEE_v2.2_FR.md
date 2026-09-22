# ESP8266 — PHY RF autonome et modulations OOK / ASK / FSK / M-FSK / QPSK / QAM
## Référence consolidée du projet de reverse-engineering — v2.2

**Date de consolidation initiale : 2026-09-13 — révision v2.1 : 2026-09-18 — révision v2.2 validation silicium : 2026-09-22**  
**Statut : RÉFÉRENCE CONSOLIDÉE — logiciel/ROM + frontières hardware + validations silicium mesurées**  
**Portée : ESP8266 / LX106 / mask-ROM / PHY v6 / libphy / libpp / libnet80211 / librftest.a du corpus analysé**

**Révision v2.1 — corpus supplémentaire audité :** mask-ROM 64 KiB + `libphy.a`, `libpp.a`, `libnet80211.a` fournis localement. La v2.1 intègre la décompilation ciblée des chemins ASK/M-ASK, gain RX, noise-floor, IQ_EST, calibration DC/IQ et PWDET/SAR.

**Révision v2.2 — validation banc/silicium :** des mesures contrôlées sur ESP8266 ajoutent des preuves directes sur la fraîcheur IQ_EST/E4, le handshake de réarmement START/DONE, le suivi d’une enveloppe OOK à 10 kHz avec `IQ_N=16`, l’exclusion de `0x60009824` comme substitut OOK rapide dans la configuration testée, et les coûts d’acquisition ROM/EDGE/SOFT. Il s’agit de résultats de banc du projet, pas de spécifications officielles Espressif.

> **Important — sens du mot « référence ».** Ce document est la référence consolidée **du projet de reverse-engineering**. Il ne s'agit pas d'un document officiel d'Espressif et il ne doit pas être présenté comme tel.

> **Objectif.** Fusionner sans perte les quatre références projet OOK/ASK/FSK, corriger les ambiguïtés ou erreurs identifiées au cours des passes ultérieures, puis ajouter le dossier QPSK/QAM et l'analyse directe de `librftest.a`.

> **Règle normative.** Les parties I à XII de ce document constituent la synthèse normative v2.2. Les annexes conservent les quatre références v1.0 complètes et l'historique QAM v0.60→v0.83 afin qu'aucune information source ne soit perdue. Lorsqu'une formulation historique diverge de la synthèse v2.2, la synthèse v2.2 prévaut.

---

# TABLE DES MATIÈRES

1. Convention de preuve et définition des pourcentages
2. Résumé exécutif et état des quatre familles de modulation
3. Architecture PHY commune et carte des blocs
4. TX OOK / ASK / M-ASK
5. RX OOK / ASK / M-ASK
6. TX FSK / M-FSK
7. RX FSK / M-FSK
8. TX QPSK / 16-QAM / 64-QAM
9. RX QPSK / 16-QAM / 64-QAM
10. Analyse directe de `librftest.a`
11. Carte consolidée des registres, fonctions et frontières
12. Corrections, supersessions, inconnues restantes et plan expérimental
13. Annexes — quatre références v1.0 complètes + historique QAM détaillé

---

# PARTIE I — CONVENTION DE PREUVE

## 1. Trois niveaux qu'il ne faut plus confondre

Le projet distingue désormais explicitement trois niveaux.

### Niveau A — logiciel / ROM / ABI / MMIO démontré

Ce niveau couvre ce que l'on peut fermer statiquement à partir du code réel :

```text
mask-ROM
libphy.a
libpp.a
libnet80211.a
librftest.a
DWARF
relocations
symboles
strings
accès MMIO / PBUS / I²C
structures et ABI
```

Un « 100 % logiciel/statique » signifie que le **contrat CPU et la séquence de commande** sont fermés pour le périmètre déclaré. Il ne signifie pas que la réponse analogique du silicium est connue.

### Niveau B — architecture matérielle fortement inférée

Ce niveau regroupe les conclusions imposées par le flot logiciel mais dont le nom électrique interne n'est pas exposé : mapper QAM derrière `RATE`, corrélateur IQ_EST, NCO/phase accumulator derrière `tone_control`, rôle exact de certains bits de handshake, etc.

### Niveau C — validation silicium / RF

Ce niveau exige une mesure réelle : dBm, délai MMIO→RF, settling, phase, jitter, sensibilité, BER, gain en dB, loi `tone_control→Hz`, fraîcheur d'un CFO sur tone non-802.11, transfert absolu `E4→dBm`, limite haute de bande passante OOK, etc.

## 2. Convention de statut

| Marque | Sens |
|---|---|
| **100 % logiciel** | séquence CPU/ROM/ABI fermée dans le corpus |
| **~99 % structurel** | architecture imposée à très haute confiance, nom électrique parfois ouvert |
| **candidat** | piste techniquement plausible mais non démontrée |
| **écarté** | hypothèse incompatible avec le corpus analysé |
| **validation silicium** | question physique non tranchable statiquement |

## 3. Résumé global de compréhension

Les pourcentages ci-dessous décrivent la **compréhension logiciel/ROM**, pas la garantie d'un modem RF déjà validé sur banc.

| Famille | TX logiciel/ROM | RX logiciel/ROM | Commentaire |
|---|---:|---:|---|
| OOK | **100 %** | **100 % logiciel pour le chemin canonique à gain fixe** | génération + IQ_EST + gain/PBUS/noise-floor fermés ; détection OOK par E4 et handshake START/DONE frais validés sur silicium jusqu’à au moins 10 kHz dans le banc testé ; calibration RF absolue encore ouverte |
| ASK / M-ASK | **100 %** | **100 % logiciel pour le chemin canonique à gain fixe** | génération multi-niveaux + gain RX + métriques E/Corr²/DC² fermés ; dynamique réelle à mesurer |
| FSK / M-FSK | **100 %** | **100 %** | commande TX et contrat CFO RX fermés ; réponse non-802.11 à valider |
| QPSK / QAM standard Wi-Fi | **~95–99 % côté interface CPU** | **~90–95 % côté interface CPU** | mapper/demapper natifs derrière frontière hardware |
| QPSK/QAM propriétaire arbitraire | **non fermé** | **non fermé** | aucun port symbole/IQ/LLR direct trouvé |

La différence entre « QAM standard compris » et « QAM propriétaire faisable » est fondamentale. Le CPU sait sélectionner la modulation native, mais le corpus n'expose pas un port arbitraire de points de constellation.

---

# PARTIE II — ARCHITECTURE PHY COMMUNE

## 4. Modèle global

Le meilleur modèle consolidé du projet est :

```text
LX106 / firmware
   │
   ├── WDEV / PP / LMAC / DMA
   │
   ├── registres PHY 0x6000xxxx
   │
   ├── PBUS / I²C interne RF
   │
   ▼
digital baseband matériel
   │
   ├── générateur tone / test
   ├── OFDM TX : scrambler → FEC → interleaver → mapper
   ├── OFDM RX : FFT/égalisation → demapper → deinterleave/FEC
   ├── IQ_EST / corrélations / énergie
   ├── CFO / EVM / noise-floor
   ▼
chaîne analogique / PLL / mixer / PA / LNA / ADC-DAC
```

L'ESP8266 n'est donc pas un SDR généraliste du point de vue CPU : plusieurs blocs vectoriels existent, mais leur interface normale expose surtout des **commandes, statistiques et paquets**, pas un flux continu `I[n],Q[n]`.

## 5. Carte consolidée de la région tone / IQ_EST

```text
0x60000504..0x60000560  table TX BB attenuation / power
0x6000057C              IQ_EST_CTRL
0x60000580              corrélation R0
0x60000584              corrélation R1
0x60000588              corrélation R2
0x6000058C              corrélation R3
0x60000590 bit4         RXMAX_EXT_DIG (contrôle d'extension RX digitale, pas le code de gain principal)
0x60000594              PBUS / test command
0x60000598              continuous/test control
0x6000059C              continuous/test control
0x600005A0              PBUS status
0x600005B8              tone slot 1
0x600005BC              tone slot 2
0x600005C4              tone slot 3
0x600005DC              DC / mean I
0x600005E0              DC / mean Q
0x600005E4              énergie / puissance latchée de la dernière mesure IQ_EST terminée
0x600005E8              utilisé par rftest/PBUS TX ; sous-rôle à classifier finement
```

## 6. Carte consolidée de résultats RX / TXIQ

```text
0x60009800              résultat partagé CFO/EVM
  bit0                  validité exigée par le getter CFO
  bits15:8              CFO brut signed8
  bits28:16             métrique lue par phy_get_bb_evm()

0x60009804              observé dans do_rx_poll() de librftest.a ; rôle exact ouvert
0x60009824              readout noise-floor hardware ; la lecture directe OOK est restée invariante dans le test OFF/ON
0x60009860              configuration/correction digitale TXIQ
0x600098DC              handshake/finalisation du cycle CFO
0x60009B4C              observé dans do_rx_poll() de librftest.a ; rôle exact ouvert
0x60009B00 bit28        contrôle CCA utilisé derrière phy_enable_agc()/phy_disable_agc()
0x60009B60              contrôle/lancement du sous-système noise-floor
0x60009B64[31:20]       représentation noise-floor traitée lue par ram_get_noisefloor()
0x60009B64[11:0]        champs de configuration/lancement noise-floor
```

### 6A. Carte PWDET / SAR / power-control TX ajoutée en v2.1

```text
0x60000D50 bit0         enable PWDET
0x60000D50 bit1         trigger acquisition SAR/FM
0x60000D5C bits21/23    configuration PWDET nettoyée par rom_en_pwdet()/tx_pwctrl_bg_init()
0x60000D60              contrôle de power-control TX de fond
0x60000D80..0x60000D9C  banque de huit résultats SAR
```

Le chemin logiciel est fermé comme **mesure/calibration de puissance TX**. Il ne doit plus être présenté comme un détecteur RX OOK démontré. La conversion absolue de ces codes vers dBm reste une question de silicium/RF.

## 7. Carte WDEV utile

```text
0x3FF20004 bit31        gate RX WDEV haut niveau
0x3FF20038             observé dans factory RX ; rôle exact ouvert
0x3FF2003C[19:16]      gate de contexte utilisé par phy_get_bb_freqoffset()
0x3FF20040             observé dans factory RX ; rôle exact ouvert

0x3FF20C18             interrupt enable WDEV
0x3FF20C20             événements WDEV latched
0x3FF20C24             clear/acquittement événements
0x3FF20C20.bit8        entrée du grand chemin RX avant discard/success

0x3FF20CDC             TX DMA descriptor/control
0x3FF20CE0             TX PPDU/PLCP control : LENGTH/RATE/KID/HT
0x3FF20CE4             HT-SIG low 32 bits
0x3FF20CE8             Duration/ID control
```

---

# PARTIE III — TX OOK / ASK / M-ASK

## 8. Primitive matérielle commune

Le générateur autonome repose sur le tone slot 1 :

```text
TONE1 = 0x600005B8
```

Le packing canonique reconstruit de `rom_start_tx_tone()` est :

```text
31        28 27                  18 17          10 9                    0
┌───────────┬──────────────────────┬──────────────┬──────────────────────┐
│ préservés │ zone mode/test       │ scale code   │ raw tone_control     │
│           │ bit18 = gate normal  │ 8 bits       │ stimulus / step      │
└───────────┴──────────────────────┴──────────────┴──────────────────────┘
```

La ROM ne masque pas explicitement `tone_control` à 10 bits. Le masque `0x3FF` utilisé dans les firmwares autonomes est une **mesure de sûreté du projet**, pas une instruction observée dans la ROM.

## 9. OOK

OOK canonique :

```text
bit18 = 1 → tone actif
bit18 = 0 → tone coupé dans le slot
```

La TX clock reste active entre symboles. Il ne faut pas appeler `rom_stop_tx_tone()` à chaque symbole, car cette routine coupe aussi la TX clock globale.

Masque :

```c
#define TONE_GATE_MASK 0x00040000u
```

La modulation doit être un RMW ciblé du slot afin de préserver `tone_control`, scale et modes supérieurs.

## 10. ASK / M-ASK

Le champ numérique est :

```text
bits17:10
```

avec l'encodage exact :

```text
digital_field = (-digital_scale) mod 256
```

et le chemin canonique utilise `digital_scale = 0..63`.

Le TXIQ démontre que le champ peut être réécrit alors que la TX clock reste active. Cela ferme le mécanisme logiciel de modulation d'amplitude, mais pas la loi RF `code→dBm`.

## 11. Ce qui est fermé / ce qui reste physique

Fermé logiciellement : adresse, gate, scale, RMW, initialisation du slot, discipline `MEMW`, séparation scale numérique/analogique, maintien de la TX clock.

À mesurer : extinction réelle, pente amplitude/power, phase pendant hot-update, latence, jitter, nombre de niveaux M-ASK séparables, puissance absolue.

### 11A. PWDET/SAR : calibration de puissance TX, pas modulateur

La décompilation de la ROM et du PHY v6 ferme le chemin :

```text
rom_en_pwdet()
  ↓
SAR init
  ↓
0x60000D5C configuration
  ↓
0x60000D50[0] enable
```

`ram_get_fm_sar_dout()` déclenche l'acquisition via `0x60000D50[1]`, puis `read_sar_dout()` lit huit résultats dans `0x60000D80..0x60000D9C`.

`rom_get_power_db()` construit ensuite une mesure logarithmique relative à partir des résultats SAR, et `meas_tone_pwr_db()` mesure un tone TX connu par deux acquisitions successives. Les fonctions `tx_pwctrl_cal()`, `tx_pwctrl_background()` et `ram_rfcal_pwrctrl()` utilisent cette chaîne pour corriger le gain/attenuation TX.

**Conclusion normative v2.1 :** PWDET/SAR est une excellente voie de **calibration de la puissance de base TX** et de caractérisation d'une LUT M-ASK, mais ce n'est ni le modulateur symbole-par-symbole ni un détecteur RX OOK démontré.

---

# PARTIE IV — RX OOK / ASK / M-ASK

## 12. Architecture canonique

```text
initialisation RF/PHY
   ↓
rom_pbus_xpd_rx_on()
   ↓
RX clock ON
   ↓
PBUS debug : packet RX retiré, RF RX conservée
   ↓
gain RX déterministe
   ↓
IQ_EST(mode=1,N)
   ↓
DONE
   ↓
0x600005E4 = énergie/power latchée de la mesure terminée
   ↓
1 seuil : OOK
plusieurs seuils : ASK / M-ASK
```

## 13. IQ_EST_CTRL

```text
0x6000057C
bit0       ENABLE
bit1       START
bits16:2   N[14:0]
bit18      MODE
bit31      DONE
```

La valeur démontrée dans les calibrations est notamment `MODE=1, N=1024`.

Les sorties du même bloc sont :

```text
R0..R3              statistiques/corrélations
DC_I / DC_Q          moyennes signées
E4                   énergie / puissance
```

### 13A. Validation silicium v2.2 — fraîcheur E4 et réarmement direct IQ_EST

Les mesures de banc du 2026-09-22 précisent le comportement visible CPU de ce bloc. Dans la configuration testée, `0x600005E4` se comporte comme le **résultat latché de la dernière mesure IQ_EST terminée**, et non comme un RSSI rafraîchi en continu ni comme une valeur calibrée en dBm. Un essai de lecture libre/sans nouveau trigger répète la même valeur E4 capturée jusqu’au lancement d’une nouvelle mesure.

Le projet a testé quatre stratégies d’acquisition :

```text
ROM    : chemin canonique enable/start/wait/read
PULSE  : ancien pulse direct de START ; ne prouve pas que DONE est redescendu
EDGE   : START=0 → attendre DONE=0 → START=1 → attendre DONE=1 → lire E4
SOFT   : reset logiciel EN/START + même handshake DONE bas/DONE haut
```

L’ancien mode `PULSE`/ancien-`rearm` **ne garantit pas la fraîcheur** : DONE peut déjà être haut depuis la mesure précédente, donc une attente de DONE=1 peut se terminer immédiatement. Le mode `EDGE` observe explicitement une transition complète de DONE et constitue donc l’expérience de réarmement direct de référence.

Sur une capture de 256 mesures, `EDGE` et `SOFT` ont tous deux observé DONE haut à l’entrée, puis bas, puis haut à nouveau sur **256/256 mesures**, avec `low_to=0`, `high_to=0` et aucun timeout d’acquisition. Le handshake direct suivant est donc validé sur le silicium testé :

```text
START = 0
attendre DONE = 0
START = 1
attendre DONE = 1
lire E4
```

La même série de banc démontre le suivi d’une enveloppe OOK avec `IQ_N=16` à **10 kHz**. Sur 256 échantillons nominalement espacés de 10 µs (2,56 ms au total), en reclassant les valeurs E4 enregistrées avec un seuil de 60 000 unités, le nombre de transitions mesuré est :

| chemin d’acquisition | transitions | attendu pour 10 kHz sur 2,56 ms |
|---|---:|---:|
| ROM | 51 | ~51,2 |
| EDGE | 49 | ~51,2 |
| SOFT | 50 | ~51,2 |

Cela établit un **suivi OOK d’au moins 10 kHz dans la configuration de banc testée** ; cela n’établit pas une limite haute de 10 kHz. Un détecteur pratique doit employer des seuils calibrés ou une hystérésis plutôt que d’interpréter le code E4 brut comme une unité RF absolue.

Le coût d’acquisition mesuré dans le même run normalisé (`F_CPU=80 MHz`, `IQ_N=16`) est approximativement :

```text
ROM   cost_avg ≈ 353 cycles
EDGE  cost_avg ≈ 396 cycles
SOFT  cost_avg ≈ 390 cycles
```

À délai inter-échantillon demandé nul, l’espacement moyen observé est d’environ 382 cycles (ROM), 423 cycles (EDGE) et 419 cycles (SOFT), soit environ 209 kS/s, 189 kS/s et 191 kS/s à 80 MHz. Ces valeurs sont des mesures d’implémentation/banc, pas des limites hardware garanties.

## 14. Correction importante AGC/CCA

Les wrappers historiquement nommés `phy_enable_agc()` / `phy_disable_agc()` ne doivent pas être interprétés comme un contrôle AGC général dans ce corpus.

Le traçage exact de la petite table `phy_ops` du chip v6 donne :

```text
phy_enable_agc()  → rom_chip_v5_enable_cca()
phy_disable_agc() → rom_chip_v5_disable_cca()
```

et les deux routines ROM manipulent :

```text
0x60009B00 bit28

enable  : clear bit28
disable : set bit28
```

**Correction v2.1 :** ce chemin ferme le contrat logiciel CCA correspondant, mais **ne démontre pas** que `0x60009B00[28]` fige à lui seul le gain analogique. Pour ASK/M-ASK, la voie canonique reste donc **un gain explicitement déterministe via PBUS**, sans dépendre d'une hypothèse « AGC freeze ».

### 14A. Gain RX composite 15 bits — mapping PBUS fermé

Pour un mot de gain `g` :

```text
g[2:0]   → PBUS(3,2)[5:3]

g[3]     → PBUS(3,1)[6]
g[4]     → PBUS(3,1)[5]
g[5]     → PBUS(3,1)[4]
g[6]     → PBUS(3,1)[3]
g[7]     → PBUS(3,1)[2]
g[8]     → PBUS(3,1)[1]
g[9]     → PBUS(3,1)[0]

g[10]    → PBUS(2,1)[1]
g[14:11] → PBUS(2,1)[6:3]
```

`PBUS(2,1)` conserve les bits `0,2,7,8` via `old & 0x185`. Le code 15 bits est donc **composite** et ne doit pas être interprété comme une échelle linéaire de dB.

### 14B. Gain baseband — formule coarse/fine exacte

`pbus_set_rxbbgain(gain)` programme :

```text
gain 0..5    → coarse = 0x00
gain 6..11   → coarse = 0x40
gain 12..17  → coarse = 0x60
gain 18..23  → coarse = 0x70
gain >=24    → coarse = 0x78

fine = ((gain % 6) << 3) | 0x06
```

puis :

```text
PBUS force-test(3,1,coarse)
PBUS force-test(3,2,fine)
```

Le protocole `rom_pbus_force_test()` utilise `0x60000594` comme commande et `0x600005A0` comme statut/handshake.

### 14C. Table de gain RX v6

`register_chipv6_phy_init_param()` fait pointer :

```text
rx_gain_swp_step = phy_init_data + 2
```

`gen_rx_gain_table()` utilise **16 step lengths** et **16 codes de base** pour générer **127 codes de gain 15 bits**.

À l'intérieur d'un segment, le motif est structuré en groupes de six :

```c
if (r < 24) {
    q = r / 6;
    coarse = (1 << q) - 1;   // ladder/thermometer
    fine   = r % 6;
} else {
    coarse = 15;
    fine   = min(r - 24, 5);
}

code15 = base_code[stage] + (coarse << 3) + fine;
```

La rampe AGC n'est donc pas totalement figée dans la ROM : elle est partiellement paramétrée par `phy_init_data`.

### 14D. Conversion `code15 → bb_gain`

`set_rx_gain_testchip_50()` reconstruit :

```c
coarse_count = min(popcount((code15 >> 3) & 0x7F), 4);
fine         = code15 & 0x7;
bb_gain      = min(6 * coarse_count + fine, 29);
```

Le PHY travaille donc avec **30 indices BB**, `0..29`.

### 14E. `0x60000590[4]` : RXMAX_EXT_DIG, pas registre principal de gain

Le build fourni accède à `0x60000590` via la base `0x60000200 + 0x390`.

`chip_v6_rxmax_ext_dig()` commande exactement le **bit 4**. `set_rx_gain_cal_iq()` le désactive temporairement pendant la calibration, puis le réactive.

**Correction normative :** `0x60000590[4]` appartient au sous-système **RXMAX_EXT_DIG**. Le gain principal reste programmé via PBUS.

### 14F. Noise-floor : deux représentations différentes

Les deux adresses précédemment observées sont toutes deux valides :

```text
0x60009824[11:0]   mesure hardware brute
0x60009B64[31:20]  représentation traitée lue par ram_get_noisefloor()
```

Formules reconstruites :

```c
raw = REG32(0x60009824) & 0xFFF;
nf_hw = (int16_t)((raw - 4095) >> 1);
```

et :

```c
x = (REG32(0x60009B64) >> 20) & 0xFFF;
nf = (int16_t)(((x + 1) >> 1) - 2048);
```

`0x60009B60` et les bits bas de `0x60009B64` gèrent le lancement/configuration du sous-système. `get_noisefloor_sat()` borne la valeur dans `[-392,-340]`; l'interprétation en quart de dB est cohérente avec la nomenclature Espressif mais reste distincte de la calibration RF absolue.

**Qualification banc v2.2 de `0x60009824` :** une sonde de lecture directe rapide de `0x60009824[11:0]` est restée invariante entre les conditions OOK TX OFF/ON testées (valeur décodée `-684` dans ce run), tandis que E4 déclenché variait de plusieurs ordres de grandeur. `0x60009824` reste donc un readout lié au noise-floor cohérent avec l’analyse statique, mais ce n’est **pas un détecteur rapide d’enveloppe OOK démontré** et il ne doit pas remplacer une mesure IQ_EST/E4 fraîche sur le seul critère de vitesse de lecture.

### 14G. `rom_get_corr_power()` — trois métriques, pas une seule

`rom_get_corr_power()` lit :

```text
0x60000580 / 584 / 588 / 58C
0x600005DC / 5E0 / 5E4
```

et produit trois sorties :

```text
out[0] = énergie E4 normalisée

corr_re = R0 + R3
corr_im = R1 - R2
out[1]  = |corr|² normalisé

out[2]  = |DC|² normalisé
```

Donc un RX M-ASK peut exploiter `E`, `Corr²` et `DC²`, même si **E4 seul reste la métrique canonique la plus simple**.

### 14H. Calibration DC/IQ par niveau de gain

La table RX contient d'abord les **127 codes de gain** puis, à partir d'environ `+0x100`, **30 records de 8 octets** associés aux indices BB `0..29`.

Chaque record contient quatre valeurs de calibration DC sur 9 bits. `set_cal_rxdc()` les applique aux endpoints :

```text
PBUS(4,1)
PBUS(5,1)
PBUS(4,2)
PBUS(5,2)
```

`set_rx_gain_cal_iq()` orchestre ensuite la calibration IQ : désactivation temporaire de RXMAX_EXT_DIG, configuration I²C/loopback, **4 acquisitions**, moyenne, clamp des deux coefficients dans `[-15,+15]` et `[-31,+31]`, puis stockage compact.

`ram_rxiq_get_mis()` confirme que `0x60000580..0x6000058C` sont des **statistiques complexes de mismatch/corrélation**, pas des échantillons I/Q bruts.

## 15. Limite physique

Le chemin externe→IQ_EST est désormais à la fois fortement reconstruit et partiellement validé sur silicium. L’acquisition E4 fraîche via le chemin ROM canonique et via le handshake direct sur front START/DONE est démontrée, et le suivi OOK est démontré jusqu’à au moins 10 kHz avec `IQ_N=16` dans le banc testé. Restent à mesurer : `E4→dBm` absolu, loi complète `N→temps`, sensibilité, saturation, gain-code→dB, limite haute OOK, dispersion température/composant et nombre réel de niveaux ASK séparables.

---

# PARTIE V — TX FSK / M-FSK

## 16. Principe

Le fast-FSK du projet conserve :

```text
RFPLL fixe
canal fixe
TX RF actif
TX clock active
gate bit18 actif
scale constant
```

et modifie uniquement :

```text
tone_control = K0, K1, ... K(M-1)
```

via RMW ciblé du bas de `0x600005B8`.

## 17. Frontière logiciel / RF

Le chemin logiciel est fermé à 100 % : le code peut modifier le champ, le préserver par RMW, maintenir la TX clock et éviter la voie RFPLL lente.

En revanche, le silicium reste seul à définir :

```text
Ki → fréquence fi
temps de settling
continuité de phase
jitter
transitoires spectraux
```

## 18. Pourquoi `set_rf_freq_offset()` n'est pas le modulateur rapide

Cette fonction suit une séquence RFPLL :

```text
set_rf_freq_offset()
  ↓
ram_rfpll_set_freq()
  ↓
write RFPLL SDM
  ↓
restart calibration
  ↓
wait_rfpll_cal_end()
```

Elle est donc un mécanisme de retuning/correction, pas le chemin symbole-par-symbole canonique.

---

# PARTIE VI — RX FSK / M-FSK

## 19. Résultat CFO exact

```text
BB_CFO_RESULT = 0x60009800
bit0          = condition de validité CFO
bits15:8      = raw CFO signed8
```

Le getter applique :

```text
WDEV gate : ((0x3FF2003C >> 16) & 0xF) < 8
```

puis :

```text
CFO = (raw_signed8 * 107) >> 6
```

Sinon, la sentinelle est :

```text
0x7FFF
```

## 20. Handshake exact

Après toute tentative, valide ou invalide :

```c
r = REG32(0x600098DC);
r |= 0x0F;
REG32(0x600098DC) = r;
```

Le rôle logiciel est fermé comme **finalisation/handshake de tentative CFO**. Le nom électrique détaillé de chaque bit (`ACK`, `CLEAR`, `REARM`, `W1C`, strobe...) n'est pas requis pour reproduire le contrat CPU.

## 21. Séparation mesure / correction

```text
phy_meas_freq_offset  = dernière mesure CFO
phy_freq_offset       = état/correction appliquée
```

`phy_get_freq_param(corr_out,meas_out)` expose explicitement ces deux états dans cet ordre.

## 22. Limite physique

Le RX FSK est fermé à 100 % **au périmètre CPU/statique**, mais il reste à démontrer sur silicium que le baseband produit un CFO frais sur un tone/FSK arbitraire non-802.11 et à quelle cadence.

---

# PARTIE VII — TX QPSK / QAM

## 23. Première conclusion : le mapper QAM natif existe et est sélectionné par RATE

Le champ :

```text
esf_tx_desc_s.rate  @ offset +8, 8 bits
```

est chargé par `lmacSetTxFrame()`. Pour le legacy OFDM, le nibble bas est écrit dans :

```text
0x3FF20CE0[15:12]
```

et la longueur dans :

```text
0x3FF20CE0[11:0]
```

Le layout consolidé du mot est :

```text
31                             25 24 23               16 15         12 11             0
┌────────────────────────────────┬──┬───────────────────┬─────────────┬────────────────┐
│             0                  │HT│       KID         │    RATE     │     LENGTH     │
│                                │  │      8 bits       │    4 bit    │     12 bit     │
└────────────────────────────────┴──┴───────────────────┴─────────────┴────────────────┘
```

Pseudo-code :

```c
plcp1 =
      (length & 0x0FFF)
    | ((rate & 0x0F) << 12)
    | ((uint32_t)kid << 16)
    | (is_ht ? 0x01000000u : 0);
```

## 24. Table legacy rate → modulation/coding

| Code CPU/PHY | Débit | Modulation | Code rate |
|---:|---:|---|---:|
| `0x0B` | 6 Mb/s | BPSK | 1/2 |
| `0x0F` | 9 Mb/s | BPSK | 3/4 |
| `0x0A` | 12 Mb/s | QPSK | 1/2 |
| `0x0E` | 18 Mb/s | QPSK | 3/4 |
| `0x09` | 24 Mb/s | 16-QAM | 1/2 |
| `0x0D` | 36 Mb/s | 16-QAM | 3/4 |
| `0x08` | 48 Mb/s | 64-QAM | 2/3 |
| `0x0C` | 54 Mb/s | 64-QAM | 3/4 |

Le groupe `0x0F / 0x0E / 0x0D / 0x0C` est particulièrement utile expérimentalement : **même code rate 3/4, constellation différente**.

## 25. Frontière exacte du mapper

Pour les rates legacy, aucun second registre CPU séparé n'est programmé pour :

```text
MODULATION
FEC_RATE
PUNCTURE_MODE
INTERLEAVER_MODE
CONSTELLATION_INDEX
```

Le modèle est donc :

```text
CE0.RATE
   ↓
decodeur PHY interne
   ├─ famille de modulation
   ├─ coding rate
   ├─ puncturing
   ├─ paramètres d'interleaver
   └─ mode du mapper constellation
```

La banque TX WDEV est fonctionnellement classée :

```text
0x3FF20CDC  DMA/control
0x3FF20CE0  PPDU/PLCP control
0x3FF20CE4  HT-SIG
0x3FF20CE8  Duration/ID
```

Aucun de ces mots n'est un port `I`, `Q`, index constellation ou coded-bit direct.

## 26. Fermeture aval vers WDEV

`lmacTxFrame()` appelle `lmacSetTxFrame()`, puis `wDev_EnableTransmit(index,aifs,backoff)`. L'ABI de `wDev_EnableTransmit()` ne transporte ni `rate`, ni `I/Q`, ni constellation. Le `rate` n'est pas relu après la préparation CE0 dans le chemin legacy.

Cela renforce que la frontière CPU de modulation standard est **CE0.RATE**.

## 27. Raw freedom et fixed-rate

`wifi_send_pkt_freedom` / `ieee80211_freedom_output()` permettent de choisir une trame MAC brute, mais le chemin converge vers `ppTxPkt → ppProcessTxQ → lmacTxFrame`.

Donc :

```text
raw MAC ≠ raw PHY
```

Le fixed-rate permet de sélectionner la constellation native, mais ne fournit ni coded bits, ni symboles, ni I/Q arbitraires.

## 28. `tx_cont_*` et « iqview »

Le continuous-TX manipule principalement :

```text
0x60000594
0x60000598
0x6000059C
```

avec sauvegarde/restauration de l'état de test. Il ne reçoit aucun payload, rate, I/Q ou constellation index.

Le terme « iqview » du factory-test désigne le mode de test destiné à un instrument externe IQView ; il ne constitue pas un port I/Q interne CPU.

## 29. Audit des APIs nommées

L'audit des symboles de `libphy`, `libpp`, `libnet80211`, ROM et maintenant `librftest.a` ne révèle aucun API nommé de type :

```text
scrambler bypass
FEC bypass
interleaver bypass
coded-bit input
QAM mapper input
constellation index
raw IQ TX FIFO
```

Ce résultat ne prouve pas qu'aucun MMIO anonyme n'existe physiquement, mais il ferme fortement la surface logicielle exposée.

## 30. TXIQ : ce que le CPU contrôle réellement

Le registre :

```text
0x60009860
```

contient des champs de correction TXIQ :

```text
bits28:24  gain_code signé/encodé
bits23:18  phase_code signé/encodé
bits17:16  forcés à 11 dans le chemin observé
bit0       activé/latché par phy_bb_rx_cfg(), rôle électrique exact non nommé
```

Les coefficients sont des **corrections de mismatch**, pas un rotateur arbitraire. La formule de phase est une erreur normalisée, ce qui déclassifie l'idée de produire directement 0/90/180/270° par simple écriture du trim.

## 31. Stimuli vectoriels TXIQ

Les états observés :

```text
0x10B ↔ 0x20B   paire de gain : deux rails orthogonaux séparés à très haute confiance
0x00B ↔ 0x04B   paire de phase : combinaisons croisées type A+B / A-B à forte confiance
```

Ils démontrent un **mini-générateur vectoriel de calibration**. En revanche, les quatre signes nécessaires à un QPSK complet ne sont pas exposés dans le chemin standard observé.

Le bit physique 25 du nibble supérieur n'est pas un candidat de signe dans le chemin standard : il est explicitement maintenu à zéro par les séquences TXIQ analysées.

## 32. Verdict TX QAM propriétaire

État actuel :

```text
QAM Wi-Fi standard via mapper natif         : démontré
sélection QPSK/16QAM/64QAM par rate         : démontrée
injection directe de symboles QAM arbitraires: non trouvée
FIFO I/Q TX CPU                             : non trouvé
mapper bypass factory                       : non trouvé
TXIQ comme synthèse QPSK/QAM                : piste partielle, non fermée
```

La piste la plus crédible sans nouveau port hardware est désormais de **détourner mathématiquement le pipeline 802.11** : calculer les données d'entrée afin d'obtenir des suites de coded bits / symboles compatibles avec les contraintes scrambler+FEC+puncturing+interleaver. Cette possibilité n'est pas encore démontrée de bout en bout.

---

# PARTIE VIII — RX QPSK / QAM

## 33. IQ_EST n'est pas un flux I/Q brut

`rom_dc_iq_est(mode,N,out)` :

```text
configure IQ_EST
lit 0x600005DC / 0x600005E0
shift arithmétique
division signée par N+1
retourne moyenne I, moyenne Q
```

`N=0` est accepté par le logiciel, mais cela signifie **fenêtre minimale**, pas preuve d'un sample ADC I/Q instantané.

Les registres :

```text
0x60000580..0x6000058C
```

sont lus comme statistiques de corrélation/matrice. Le code forme notamment :

```text
R0 ± R3
R1 ± R2
```

et effectue des calculs 64 bits de mismatch gain/phase. Ils ne constituent pas une FIFO `I_symbol/Q_symbol` démontrée.

## 34. `phy_adc_read_fast()` n'est pas l'ADC RF I/Q

La fonction utilise le sous-système SAR/TOUT. Elle est donc écartée comme voie de capture I/Q RF brute.

## 35. Interface RX standard après le demapper

Le `RxControl` exact fait 12 octets et expose des métadonnées de paquet : RSSI, rate/sig_mode, length, MCS, CWB, HT length, smoothing, sounding, aggregation, STBC, FEC, SGI, rxend_state, ampdu count, channel, noise floor.

Il n'expose pas :

```text
I/Q
LLR
soft bits
EVM par symbole
index constellation
```

`esf_buf_s` fait 40 octets et contient des pointeurs/buffers, longueurs, `chl_freq_offset`, bookkeeping et descriptor. Là encore, aucun soft-symbol buffer n'est présent.

Le modèle est :

```text
digital baseband matériel
  FFT / égalisation
  dérotation
  QPSK/QAM demapper
  deinterleaver
  FEC decode
  CRC / RX state
        ↓
bytes + RxControl + metadata
        ↓
LX106
```

## 36. CFO / EVM

`0x60009800` transporte au moins :

```text
bit0       validité exigée par le CFO
bits15:8   raw CFO signé
bits28:16  métrique lue par phy_get_bb_evm()
```

`phy_get_bb_evm()` n'est pas un export normal du RX packet : dans le corpus standard, son appel direct retrouvé est dans `fix_cache_bug()`, séquence d'initialisation/cache. Il ne faut donc pas interpréter cet appel comme une mesure EVM continue par paquet.

## 37. Conséquence RX QAM propriétaire

Le chemin standard ne permet pas de récupérer un symbole complexe avant FEC. Les scénarios encore ouverts sont :

```text
A. tap pré-FEC / pré-demapper caché
B. SRAM/FIFO interne non référencé par le SDK
C. test-mode digital baseband
D. pipeline Wi-Fi natif, protocole propriétaire au-dessus
E. détection statistique assistée par IQ_EST plutôt qu'un QAM classique
```

Le QAM RX propriétaire classique reste donc plus contraint que le TX.

---

# PARTIE IX — ANALYSE DIRECTE DE `librftest.a`

## 38. Identité de l'archive analysée

Le fichier réellement fourni au projet est :

```text
nom       : librftest.a
taille    : 83 626 octets
SHA-256   : 01c9b9712cd5772823b6b647fa2181c8b594162b5b7091663b8b3bd482400b8e
```

**Correction importante :** cette taille locale exacte supersède les estimations antérieures basées sur l'affichage web GitHub (`60,8 kB`, `81,7 kB`, etc.). La taille normative du fichier analysé est **83 626 octets**.

Membres de l'archive :

```text
bb_common.o
crc.o
mac_common.o
rftest_func.o
```

L'archive n'est pas stripée et conserve de nombreux noms de fonctions.

## 39. Symboles importants

Parmi les symboles définis :

```text
ate_txframe_dut
do_rx_poll
set_tone_freq_step
get_rx_tone_pwr
iqmis_set
get_iqmis_cal
settxframe_rate
test_tx_frame
tx_data_frame
FillTxPacket
WifiTxStart
WifiRxStart
esp_tx_func
esp_tx_func_org
esp_rx_func
tx_pocket_test_enable
tx_pocket_test_func
wifitxout_func
set_tx_pbus_on
```

Aucun symbole nommé `qam`, `qpsk`, `mapper`, `interleaver`, `coded-bit`, `constellation`, `llr` ou FIFO I/Q n'a été trouvé.

## 40. RX factory : `freq_offset` fermé positivement

`do_rx_poll()` fait 0x2B3 octets et contient une relocation directe vers :

```text
phy_get_bb_freqoffset
```

La même fonction contient la chaîne :

```text
Correct: %d Desired: %d RSSI: %d noise: %d gain: %d err: %d err_fcs: %d freq_offset: %d
```

Ainsi, le champ factory `freq_offset` est bien issu du **même getter CFO** que le chemin RX analysé dans `libphy/libpp`. Ce point n'est plus une hypothèse numérique.

## 41. Nouveaux candidats RX observés dans `do_rx_poll()`

Le literal pool / les accès de la fonction font apparaître notamment :

```text
0x60009800   connu : CFO/EVM
0x60009804   nouveau / rôle exact ouvert
0x60009824   connu : noise-floor
0x60009B4C   nouveau / rôle exact ouvert
0x3FF20038   nouveau / rôle exact ouvert
0x3FF2003C   connu : gate contexte CFO
0x3FF20040   nouveau / rôle exact ouvert
```

Ces adresses sont désormais les meilleures cibles pour une cartographie plus profonde du RX factory.

## 42. TX factory : graphe de paquet

Le chemin public se décompose en grandes lignes comme :

```text
esp_tx_func()
  ↓
esp_tx_func_org()
  ↓
tx_pocket_test_enable()
  ↓
FillTxPacket()
  ↓
WifiTxStart()
  ↓
test_tx_frame()
```

La voie ATE plus basse contient :

```text
tx_pocket_test_func()
  ↓
ate_txframe_dut()
  ↓
fill_txdataframe()
  ↓
fill_tx_frame()
  ↓
tx_data_frame()
  ↓
PLCP/WDEV
```

`ate_txframe_dut()` fait 0x83B octets. Malgré sa taille, son graphe demeure celui d'un générateur de trames/PLCP et d'une infrastructure de test/calibration, pas d'un port direct de constellation.

## 43. Registres TX factory

`ate_txframe_dut()` fait apparaître notamment :

```text
0x60000504
0x60000524
0x3FF2006C
0x3FF20C48
0x3FF20C4C
0x3FF20C94
0x3FF20004
0x3FF20000
0x3FF20C68
0x3FF2001C
0x3FF20C00
0x3FF20E44
```

`0x60000504` et `0x60000524` tombent dans la table déjà classée `0x60000504..0x60000560` d'atténuation/power TX BB. Ils ne constituent donc pas un nouveau port QAM évident.

## 44. Calibration IQ factory

L'archive contient également :

```text
iqmis_set()
get_iqmis_cal()
get_rx_tone_pwr()
set_tone_freq_step()
```

Les chemins IQ observés convergent vers la famille des tone slots / calibrations déjà cartographiée, notamment `0x600005C4` pour le slot 3 dans les séquences concernées. Cela enrichit l'instrumentation factory, sans fournir un stream de symboles.

## 45. Conclusion `librftest.a`

Pour le QAM :

```text
bypass mapper nommé                     : non trouvé
FIFO I/Q TX                             : non trouvé
soft-symbol / LLR RX                    : non trouvé
factory CFO via phy_get_bb_freqoffset   : démontré
métriques factory supplémentaires       : démontrées
MMIO RX supplémentaires à classifier    : oui
```

Cette archive **affaiblit la piste TX factory-bypass** et **renforce la piste RX instrumentation/debug**.

---

# PARTIE X — CARTE CONSOLIDÉE DES PRIMITIVES

## 46. TX tone / RF

```text
rom_set_txclk_en
rom_set_ana_inf_tx_scale
rom_start_tx_tone
rom_stop_tx_tone
rom_txtone_linear_pwr
rom_pbus_xpd_tx_on/off
rom_pbus_set_txgain
set_rf_freq_offset
ram_rfpll_set_freq
wait_rfpll_cal_end
```

## 47. RX énergie / gain / calibration

```text
rom_iq_est_enable
rom_iq_est_disable
rom_get_corr_power
ram_rxiq_get_mis
ram_rxiq_cover_mg_mp
ram_rfcal_rxiq
set_rx_gain_cal_iq
set_rx_gain_testchip_50
set_cal_rxdc
gen_rx_gain_table
rom_pbus_set_rxgain / ram_pbus_set_rxgain
pbus_set_rxbbgain
rom_pbus_force_test
rom_pbus_xpd_rx_on/off
ram_pbus_debugmode
pbus_workmode
read_hw_noisefloor
ram_get_noisefloor
ram_set_noise_floor
ram_start_noisefloor
get_noisefloor_sat
chip_v6_rxmax_ext_dig
```

### 47A. TX power measurement / calibration

```text
rom_en_pwdet
ram_get_fm_sar_dout
read_sar_dout
rom_get_power_db
meas_tone_pwr_db
tx_pwctrl_bg_init
tx_pwctrl_cal
tx_pwctrl_background
ram_rfcal_pwrctrl
```

## 48. CFO / QAM RX

```text
phy_get_bb_freqoffset
phy_get_bb_evm
phy_get_freq_param
wDev_ProcessFiq
wDev_ProcessRxSucData
HdlChlFreqCal
rom_dc_iq_est
ram_rxiq_get_mis
```

## 49. QAM TX standard

```text
ieee80211_freedom_output
ppTxPkt
ppProcessTxQ
lmacTxFrame
lmacSetTxFrame
wDev_EnableTransmit
```

---

# PARTIE XI — CORRECTIONS ET SUPERSESSIONS

## 50. Corrections consolidées

### 50.1 « Officiel »

Toutes les mentions « référence officielle » désignent **la référence officielle du projet**, jamais une publication officielle Espressif.

### 50.2 Pourcentages à 100 %

Un « 100 % » est toujours borné par le **périmètre logiciel/statique explicitement défini**. Il n'inclut pas automatiquement la réponse RF du silicium.

### 50.3 `tone_control` 10 bits

Le masque `0x3FF` est une protection recommandée du firmware autonome. Il n'est pas un masque explicitement exécuté par `rom_start_tx_tone()`.

### 50.4 AGC / CCA

Les wrappers `phy_enable_agc/phy_disable_agc` ne doivent pas être utilisés comme preuve d'un contrôle AGC complet. La table d'opérations concernée les relie au CCA dans ce corpus.

### 50.5 `phy_adc_read_fast`

Écarté comme accès RF I/Q : il appartient au SAR/TOUT ADC.

### 50.6 TXIQ phase

`txiq_phase` est une métrique de mismatch normalisée, pas un angle en degrés. Le trim n'est pas un rotateur arbitraire QPSK.

### 50.7 Bit25 des stimuli TXIQ

Le bit25 n'est plus un candidat sérieux « signe/quadrant » dans le chemin standard ; les séquences analysées le forcent à zéro.

### 50.8 `tx_cont` / iqview

Le mode continuous/test ne fournit aucun payload/IQ/constellation input. « iqview » décrit l'usage avec un instrument externe, pas un port I/Q interne.

### 50.9 Raw freedom

`wifi_send_pkt_freedom` fournit une trame MAC brute mais rejoint le pipeline PHY standard. Raw MAC n'est pas raw PHY.

### 50.10 IQ_EST en QAM RX

IQ_EST est un moteur de statistiques de fenêtre : moyenne/DC, corrélations, puissance. Il ne doit pas être présenté comme FIFO de symboles.

### 50.11 `phy_get_bb_evm()`

Son appel depuis `fix_cache_bug()` est un chemin init/cache ; ce n'est pas un export EVM normal par paquet.

### 50.12 Factory `freq_offset`

Désormais fermé par `librftest.a` : `do_rx_poll()` appelle directement `phy_get_bb_freqoffset()`.

### 50.13 Taille de `librftest.a`

Le fichier analysé fait exactement **83 626 octets** ; les tailles web précédemment évoquées ne sont plus normatives.

### 50.14 AGC « freeze »

Une formulation antérieure de la recherche pouvait laisser entendre que `phy_disable_agc()` constituait un gel de gain démontré. **Supersédé :** le chemin exact aboutit aux primitives ROM CCA et à `0x60009B00[28]`. Le gel de gain analogique n'est pas démontré par ce bit seul. Le RX ASK/M-ASK canonique utilise un gain PBUS explicitement fixé.

### 50.15 `0x60000590`

**Supersédé :** `0x60000590` n'est pas le registre principal du code de gain. Le bit 4 est utilisé par `RXMAX_EXT_DIG`. Le gain composite principal est programmé via PBUS.

### 50.16 Noise-floor `0x60009824` vs `0x60009B64`

Les deux adresses sont correctes mais représentent des niveaux différents du sous-système : `0x60009824[11:0]` est la mesure hardware brute ; `0x60009B64[31:20]` est la représentation traitée lue par `ram_get_noisefloor()`.

### 50.17 PWDET côté RX

L'hypothèse « PWDET = détecteur RX OOK rapide » n'est pas démontrée et ne doit pas être normative. La décompilation le rattache solidement à la chaîne **SAR / mesure / calibration de puissance TX**.

### 50.18 `ram_get_corr_power`

Dans le `libphy.a` 2020 exact fourni au projet, aucun symbole exporté `ram_get_corr_power` n'est présent. La référence normative de ce corpus reste `rom_get_corr_power()` (éventuellement appelée via table). Les anciennes mentions d'une implémentation RAM appartiennent à d'autres builds/historiques.

### 50.19 Portée du « 100 % logiciel » ASK/M-ASK

La v2.1 ferme à 100 % **le chemin logiciel fonctionnel canonique à gain fixe** : modulation TX, PBUS/gain RX, IQ_EST, `get_corr_power`, noise-floor et calibrations DC/IQ nécessaires. Elle ne ferme pas la loi analogique code→dB, les dBm, la saturation, la sensibilité, le BER ou le nombre réellement séparable de niveaux M-ASK.

---

# PARTIE XII — INCONNUES RESTANTES ET PLAN DE RECHERCHE

## 51. OOK/ASK/M-ASK

Le chemin logiciel canonique est fermé à **100 % dans le corpus analysé**, et la v2.2 ajoute des validations silicium ciblées. Démontré sur le banc testé :

```text
E4 est latché par mesure IQ_EST terminée
le chemin ROM IQ_EST fournit des mesures E4 fraîches
réarmement direct EDGE : START↓ / DONE↓ / START↑ / DONE↑ fonctionnel
SOFT fournit également des mesures fraîches
l’ancien PULSE/ancien-rearm ne garantit pas la fraîcheur
suivi de l’enveloppe OOK démontré jusqu’à au moins 10 kHz avec IQ_N=16
0x60009824 n’a pas réagi comme détecteur OOK rapide dans le test OFF/ON
```

Les questions restantes sont physiques/statistiques :

```text
digital_scale→amplitude RF / dBm
PWDET/SAR→dBm absolu et dérive
E4/Corr²/DC²→niveau RF absolu
loi complète N→temps matériel
code gain→gain réel en dB
extinction bit18
latence/jitter et limite haute OOK
phase hot-update
saturation
sensibilité/dynamique
dispersion température/composant
BER vs SNR/débit
nombre M-ASK réellement séparable
```

Plan de validation prioritaire :

1. sweep `digital_scale=0..63` et construire la courbe amplitude/power RF ;
2. comparer cette courbe aux métriques PWDET/SAR internes ;
3. cartographier `code15` / `bb_gain` contre un niveau RF connu ;
4. mesurer les clusters `(E, Corr², DC²)` pour A0/A1/A2/A3 à gain fixe ;
5. déterminer la limite haute de suivi OOK et optimiser le chemin EDGE frais (par ex. timeout/polling basé sur CCOUNT) ;
6. calculer `Dmin` et BER pour décider objectivement entre OOK, 2-ASK, 4-ASK et éventuellement 8-ASK.

## 52. FSK

TX : mesurer `tone_control→Hz`, settling et phase.  
RX : démontrer CFO frais sur tone/FSK non-802.11 et mesurer cadence/latence.

## 53. QAM TX

Priorités :

1. formaliser l'inversion/contrainte de la chaîne scrambler→FEC→puncturing→interleaver→mapper ;
2. déterminer quelles suites de points QPSK/16-QAM/64-QAM sont atteignables depuis les bytes d'entrée ;
3. tester les quatre rates 3/4 `0x0F/0x0E/0x0D/0x0C` pour isoler la constellation ;
4. poursuivre uniquement les MMIO/test-modes anonymes si de nouvelles preuves apparaissent ;
5. valider sur récepteur IQ externe.

## 54. QAM RX

Priorités :

1. désassembler plus finement `do_rx_poll()` ;
2. classifier `0x60009804`, `0x60009B4C`, `0x3FF20038`, `0x3FF20040` ;
3. snapshot passif de `0x60009800` au point WDEV `event.bit8` ;
4. rechercher une SRAM/FIFO/test-mode pré-FEC ;
5. si aucun tap n'existe, accepter la frontière bytes+metadata et concevoir le protocole autour du demapper Wi-Fi natif ou des statistiques IQ_EST.

## 55. Verdict consolidé

```text
OOK/ASK/M-ASK : chemin logiciel fermé ; comportements IQ_EST/E4/OOK ciblés validés sur silicium
FSK           : couche logicielle/ROM fermée au périmètre déclaré
QAM     : interface CPU standard très bien comprise,
          mais accès arbitraire aux symboles toujours non exposé
```

Le principal territoire inconnu n'est plus le firmware périphérique. Il est désormais **dans le digital baseband interne après les registres CPU**, particulièrement autour du mapper/demapper QAM.

---

# ANNEXES — CONSERVATION INTÉGRALE DES RÉFÉRENCES SOURCES

> Les annexes suivantes sont incluses pour satisfaire l'exigence **sans perte d'information**. Elles conservent le contenu des quatre références v1.0, avec uniquement une démotion des niveaux de titres pour intégration Markdown. Leur contenu historique peut employer des formulations ensuite précisées par la synthèse normative v2.1 ci-dessus.


---

# ANNEXE A — TX OOK / ASK — référence v1.0 complète

### ESP8266 — Générateur autonome OOK / ASK
#### Référence technique officielle du projet de reverse-engineering

**Édition unique : v1.0**  
**Date : 2026-09-12**  
**Statut : FIGÉE — émission unique sur demande**  
**Portée : ESP8266 / mask-ROM + PHY v6 du corpus analysé**

> **Important — statut du mot “officiel”.** Ce document est la référence officielle **du projet de reverse-engineering mené sur le corpus analysé**. Il ne s’agit pas d’un document officiel d’Espressif et il ne doit pas être présenté comme tel.

> **Règle de maintenance.** Cette édition est volontairement figée. Les découvertes ultérieures ne modifieront pas ce document. Elles continueront d’être intégrées au document maître vivant `ESP8266_WIFI_PHY_REVERSE_ENGINEERING.md`.

---

#### 1. Objet

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

#### 2. Niveau de certitude

##### 2.1 Ce qui est considéré comme complètement décodé côté commande logicielle

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

##### 2.2 Ce qui n’est pas une lacune logicielle mais une caractérisation matérielle

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

#### 3. Corpus et fonctions utilisées

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

#### 4. Registres du générateur de tone

Trois slots existent :

```text
0x600005B8   TONE SLOT 1
0x600005BC   TONE SLOT 2
0x600005C4   TONE SLOT 3
```

Pour OOK/ASK, **le slot 1 suffit**.

Les trois usages ROM directs retrouvés (`rfcal_pwrctrl`, `rfcal_rxiq`, `rfcal_txcap`) utilisent le slot 1 et laissent slots 2/3 désactivés.

---

#### 5. Packing du slot 1

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

##### 5.1 Important : largeur des champs

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

#### 6. Mode normal du tone

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

### PARTIE I — OOK

#### 7. Principe OOK

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

#### 8. Constantes OOK canoniques

```c
#define TONE1_ADDR       0x600005B8u
#define TONE_GATE_BIT    18u
#define TONE_GATE_MASK   0x00040000u
```

##### ON

```c
slot |= TONE_GATE_MASK;
```

##### OFF

```c
slot &= ~TONE_GATE_MASK;
```

Aucun autre champ ne doit être modifié pendant un symbole OOK.

---

#### 9. Primitive OOK canonique

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

#### 10. Pourquoi il faut un Read-Modify-Write

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

#### 11. Normalisation obligatoire du slot avant OOK

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

### PARTIE II — ASK / M-ASK

#### 12. Principe ASK

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

#### 13. Encodage exact du scale numérique

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

#### 14. Masque ASK canonique

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

#### 15. M-ASK

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

#### 16. ASK versus OOK

##### OOK

```text
bit18 change
scale reste fixe
```

##### ASK

```text
bit18 reste à 1
bits17:10 changent
```

##### Combinaison possible

Un firmware peut aussi utiliser :

```text
OFF absolu logique : bit18 = 0
niveaux actifs      : bit18 = 1 + plusieurs digital_scale
```

Cela permet d’avoir un symbole réellement “gated” et plusieurs niveaux actifs, mais la séparation RF réelle entre niveaux doit être mesurée.

---

### PARTIE III — SCALE ANALOGIQUE

#### 17. `rom_set_ana_inf_tx_scale()`

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

#### 18. Rôle du scale analogique dans un modulateur autonome

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

### PARTIE IV — RF / PBUS / TX CLOCK

#### 19. `start_tx_tone()` n’active pas toute la chaîne RF

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

#### 20. PBUS

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

#### 21. Activation TX RF observée

##### TX OFF

`rom_pbus_xpd_tx_off()` :

```text
PBUS(6, 1,   0)
PBUS(1, 1,  12)
PBUS(2, 1,   0)
```

##### TX ON

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

#### 22. TX clock

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

### PARTIE V — DÉMARRAGE AUTONOME SANS RETOUR WI‑FI

#### 23. Définition correcte de « désactiver le Wi‑Fi »

Dans ce projet, « désactiver le Wi‑Fi » signifie :

- ne plus produire de trafic 802.11 ;
- ne plus laisser PP/LMAC/WDEV reprendre possession du TX ;
- empêcher sleep/wakeup et reconfiguration RF pendant la session ;
- **ne pas éteindre le PHY/RF dont le générateur a besoin**.

Un appel de haut niveau qui coupe totalement la radio n’est donc pas équivalent à l’objectif recherché.

---

#### 24. Procédure robuste recommandée

##### Phase A — boot / calibration

1. démarrer normalement le silicium ;
2. laisser le PHY effectuer l’initialisation/calibration nécessaire ;
3. fixer le canal/fréquence RF ;
4. préparer le mode RF de test / chaîne TX ;
5. empêcher les mécanismes susceptibles de remettre la RF en sleep ou de la réinitialiser ;
6. abandonner définitivement le trafic Wi‑Fi.

##### Phase B — préparation RF TX

À partir d’un état RF initialisé :

```text
PBUS debug/test
RX RF off
TX XPD on
choix gain / scale de base
TX clock on
```

Les calibrations `rfcal_pwrctrl` et `rfcal_txcap` démontrent ce squelette.

##### Phase C — normalisation du tone

1. choisir `tone_control` ;
2. choisir le scale de base ;
3. réécrire complètement le slot 1 en `mode_code=1` ;
4. ne plus utiliser les anciens bits TXIQ laissés par une calibration.

##### Phase D — modulation

```text
OOK   → bit18 uniquement
ASK   → bits17:10 uniquement
M-ASK → bits17:10 uniquement
```

##### Phase E — aucune restauration Wi‑Fi

Le firmware reste dans l’état générateur.

---

#### 25. Limite importante : cold-start totalement bare-metal

Le corpus permet de décoder complètement les **primitives OOK/ASK**, mais ne démontre pas une séquence minimale unique et universelle depuis un reset totalement froid qui remplacerait toute l’initialisation PHY/RF/calibration.

La procédure robuste est donc :

> **initialiser/calibrer la RF une fois avec le chemin PHY/test existant, puis prendre définitivement le contrôle du générateur.**

Ne pas présenter une séquence PBUS isolée comme substitut garanti à toute l’initialisation analogique du silicium.

---

### PARTIE VI — SQUELETTE DE FIRMWARE

#### 26. Registres et masques

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

#### 27. Accès MMIO

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

#### 28. Construction propre du slot 1

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

#### 29. Initialisation du slot

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

#### 30. OOK

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

#### 31. ASK

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

#### 32. OOK + niveaux ASK

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

### PARTIE VII — CONCURRENCE ET TIMING

#### 33. Interruptions

Pour une modulation temporellement propre, un changement de symbole ne doit pas être retardé de façon arbitraire par :

- ISR Wi‑Fi ;
- timers PHY de maintenance ;
- sleep/wakeup ;
- tâches réseau ;
- reconfiguration canal/PLL.

Puisque la cible n’a pas besoin de revenir au Wi‑Fi, il est préférable de supprimer/neutraliser ces sources plutôt que de tenter une coexistence.

---

#### 34. Atomicité

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

#### 35. `MEMW`

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

### PARTIE VIII — INSTRUMENTATION INTERNE

#### 36. Voie IQ estimator

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

#### 37. Voie TX detector / SAR

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

### PARTIE IX — PROCÉDURE DE VALIDATION

#### 38. Validation OOK

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

#### 39. Validation ASK

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

#### 40. Ce qu’il ne faut pas faire

##### Ne pas faire OOK avec `stop_tx_tone()` par symbole

Mauvais :

```text
start / stop / start / stop
```

car `stop_tx_tone()` coupe la TX clock.

##### Ne pas utiliser le scale analogique comme modulateur rapide

Il passe par I²C interne ; le RMW numérique est le chemin rapide démontré.

##### Ne pas réactiver simplement bit18 sur un slot inconnu

Toujours normaliser d’abord le slot 1.

##### Ne pas écraser le registre entier pour un symbole

Toujours modifier uniquement le champ concerné.

##### Ne pas laisser sleep/wakeup réinitialiser la RF

Le générateur suppose un état RF/PLL stable.

##### Ne pas supposer que `digital_scale=0` signifie RF=0

Le code numérique est connu ; l’effet RF doit être mesuré.

---

### PARTIE X — ÉTAT FINAL DU MODÈLE

#### 41. OOK canonique

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

#### 42. ASK canonique

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

#### 43. Architecture complète recommandée

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

#### 44. Frontière exacte entre “décodé” et “à mesurer”

##### Décodé

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

##### À mesurer

```text
combien de dB donne chaque scale
combien de dB d'extinction donne bit18=0
combien de ns/µs prend un front RF
combien de jitter existe
si la phase continue pendant OFF
combien de niveaux M-ASK sont réellement utilisables
```

---

### PARTIE XI — NOTE RF / CONFORMITÉ

#### 45. Utilisation en laboratoire

Le générateur agit dans la chaîne RF 2,4 GHz de l’ESP8266. Les essais doivent être réalisés de manière à ne pas perturber d’autres systèmes radio.

Bonnes pratiques :

- environnement blindé ou fortement atténué ;
- puissance minimale nécessaire ;
- charge/atténuation et instrumentation adaptées lorsqu’une sortie conduite est possible ;
- respect des règles locales applicables aux émissions RF ;
- ne pas utiliser cette technique pour brouiller ou perturber des réseaux tiers.

---

### PARTIE XII — RÉFÉRENCE RAPIDE

#### 46. Cheat-sheet

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

#### 47. Pseudo-code final minimal

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

### 48. Conclusion officielle du projet pour OOK/ASK

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

#### 49. Statut documentaire

**Document :** `ESP8266_ASK_OOK_REFERENCE_OFFICIELLE_PROJET_v1.0.md`  
**Édition :** 1.0  
**Émission :** unique  
**Maintenance future :** aucune — document figé  
**Suite du projet :** retour au document maître vivant et poursuite du reverse-engineering, notamment FSK/M-FSK.


---

# ANNEXE B — RX OOK / ASK — référence v1.0 complète

### ESP8266 — Récepteur autonome OOK / ASK
#### Référence technique officielle du projet de reverse-engineering

**Édition unique : v1.0**  
**Date : 2026-09-12**  
**Statut : FIGÉE — émission après fermeture à 100 % du périmètre logiciel RX canonique**  
**Portée : ESP8266 / mask-ROM + PHY v6 du corpus analysé**

> **Important — statut du mot “officiel”.** Ce document est la référence officielle **du projet de reverse-engineering mené sur le corpus analysé**. Il ne s’agit pas d’un document officiel d’Espressif et il ne doit pas être présenté comme tel.

> **Règle de maintenance.** Cette édition est volontairement figée. Les découvertes ultérieures, optimisations de vitesse ou caractérisations RF ne modifieront pas cette édition ; elles continuent dans le document maître vivant `ESP8266_WIFI_PHY_REVERSE_ENGINEERING.md`.

---

#### 1. Objet

Ce document décrit comment utiliser l’ESP8266 comme **récepteur autonome OOK et ASK/M-ASK**, après une initialisation RF/PHY unique, **sans décoder de paquets 802.11** et sans dépendre de PP/net80211 pour obtenir la métrique de réception.

La cible n’est pas :

- de recevoir des trames Wi‑Fi ;
- d’utiliser le RSSI d’un paquet décodé ;
- de faire dépendre l’OOK d’un hypothétique bit `CCA busy` ;
- de transformer l’ESP8266 en SDR I/Q brut ;
- de convertir obligatoirement la mesure interne en dBm ;
- de restaurer le fonctionnement Wi‑Fi entre les symboles.

La cible est :

```text
BOOT
  ↓
initialisation / calibration RF-PLL-PHY une fois
  ↓
fixer le canal / PLL
  ↓
activer le RX RF normal
  ↓
maintenir la RX clock
  ↓
retirer le packet RX Wi‑Fi et prendre le contrôle PBUS
  ↓
fixer un gain RX déterministe
  ↓
┌───────────────────────────────────────────────┐
│ IQ_EST M=1                                   │
│ attendre DONE                                │
│ lire 0x600005E4                              │
│ seuil unique      → OOK                      │
│ plusieurs seuils → ASK / M-ASK               │
└───────────────────────────────────────────────┘
  ↓
répéter la séquence d’acquisition
```

---

#### 2. Définition du « 100 % » dans cette référence

Le statut 100 % de ce document signifie :

> **100 % du chemin logiciel canonique inclus dans la procédure est directement démontré par la mask-ROM, `libphy.a`, les relocations, les accès MMIO/PBUS et les chemins de calibration du corpus analysé.**

Comme pour la référence TX, les grandeurs physiques suivantes ne sont pas classées comme lacunes du reverse-engineering logiciel :

```text
E4 → dBm
code de gain → dB
sensibilité absolue
BER
latence réelle START→DONE
cadence maximale OOK
nombre réel de niveaux ASK séparables
saturation / dynamique
fading / sélectivité / interférences
```

Ces grandeurs dépendent du silicium et doivent être mesurées expérimentalement.

##### 2.1 Tableau final de fermeture logicielle

| Élément RX canonique | Statut |
|---|---:|
| établissement de l’état RX RF normal | **100 % logiciel** |
| commande RX clock | **100 % logiciel** |
| retrait du packet RX Wi‑Fi | **100 % logiciel** |
| passage en PBUS debug / contrôle manuel | **100 % logiciel** |
| programmation d’un code de gain RX fixe | **100 % logiciel** |
| préservation des bits de contrôle pendant le changement de gain | **100 % logiciel** |
| séparation du mode spécial RXIQ/loopback et d’IQ_EST | **100 % logiciel** |
| registre `IQ_EST_CTRL` et protocole enable/start/N/mode/DONE | **100 % logiciel** |
| acquisition `M=1, N=1024` | **100 % démontré** |
| lecture de la métrique `0x600005E4` | **100 % logiciel** |
| re-arm sûr par séquence ROM complète | **100 % logiciel** |
| classification OOK en unités E4 calibrées | **100 % architecture logicielle** |
| classification ASK/M-ASK à gain fixe | **100 % architecture logicielle** |
| indépendance de PP/net80211 pour IQ_EST | **100 % démontré** |

Aucun élément inférieur à 100 % n’est requis dans le chemin canonique de cette référence.

---

#### 3. Corpus et fonctions utilisées

Le modèle repose sur :

- mask-ROM ESP8266 analysée ;
- `libphy.a` / PHY v6 analysé ;
- `libpp.a` et `libnet80211.a` audités pour éliminer une dépendance cachée ;
- `g_phyFuns` / `phy_func_tab` ;
- chemins de calibration RXIQ et de gain RX ;
- accès PBUS ;
- instrumentation interne IQ estimator.

Fonctions ROM directement pertinentes :

```text
0x40006260  rom_get_corr_power
0x40006400  rom_iq_est_disable
0x40006430  rom_iq_est_enable
0x4000754C  rom_pbus_set_rxgain
0x40007688  rom_pbus_xpd_rx_off
0x400076CC  rom_pbus_xpd_rx_on
0x4000804C  rom_rfcal_rxiq
```

Entrées `g_phyFuns` pertinentes :

```text
+0x020  rom_get_corr_power
+0x030  rom_iq_est_disable
+0x034  rom_iq_est_enable
+0x040  rom_set_rxclk_en
+0x054  rom_set_loopback_gain
+0x0A0  PBUS debug mode / patch RAM v6
+0x0B0  rom_pbus_rd
+0x0B4  rom_pbus_set_rxgain
+0x0BC  rom_pbus_workmode
+0x0C0  rom_pbus_xpd_rx_off
+0x0C4  rom_pbus_xpd_rx_on
+0x0F4  rom_rfcal_rxiq
```

---

### PARTIE I — ÉTAT RX NORMAL

#### 4. Activation RF RX normale

`rom_pbus_xpd_rx_on()` établit directement l’état PBUS RX normal observé :

```text
PBUS(2,1) = 0x184
PBUS(3,2) = 0x006
```

Cette paire constitue l’état RX RF normal de référence utilisé par le corpus.

La primitive canonique commence donc par :

```text
rom_pbus_xpd_rx_on()
```

après que la RF/PLL a été initialisée normalement.

---

#### 5. RX OFF n’est pas RX normal

`rom_pbus_xpd_rx_off(x)` écrit le paramètre `x` dans `PBUS(2,1)` puis met les autres étages RX concernés à zéro.

Dans plusieurs calibrations, l’appel observé est :

```text
rom_pbus_xpd_rx_off(1)
```

ce qui produit l’état :

```text
PBUS(2,1) = 0x001
```

Pour la réception autonome, cet état OFF ne doit donc pas être utilisé dans la boucle de symboles.

---

#### 6. RX clock

Le RX RF et la logique de mesure nécessitent la RX clock.

La commande canonique est :

```text
rom_set_rxclk_en(1)
```

La calibration RXIQ officielle utilise elle-même cette primitive avant les opérations de mesure puis la coupe en sortie.

Dans la cible autonome, la RX clock reste active pendant la session de réception.

---

### PARTIE II — RETIRER LE WI‑FI SANS COUPER LE RX

#### 7. PBUS debug mode

Le patch v6 `ram_pbus_debugmode()` a deux effets structurants :

1. il retire le RX numérique de paquets Wi‑Fi ;
2. il met le PBUS sous contrôle manuel/forcé.

Le même bit matériel de contrôle du RX numérique est utilisé par les primitives start/stop du RX digital et par PBUS debug.

Le mode debug active également le latch PBUS manuel autour de :

```text
0x60000594 bit0
```

L’intérêt est précisément de conserver la chaîne analogique RX tout en retirant le décodeur packet Wi‑Fi.

La séquence canonique est donc :

```text
RX RF normal ON
RX clock ON
PBUS debug mode
```

et non :

```text
RX RF OFF
```

---

#### 8. `pbus_workmode()` n’est pas nécessaire entre symboles

`pbus_workmode()` remet le contrôle PBUS dans le fonctionnement normal Wi‑Fi.

Pour un récepteur autonome qui ne revient pas au Wi‑Fi entre les symboles, la bonne architecture est de **rester en PBUS debug** pendant toute la session de démodulation.

La restauration en workmode n’est donc pas dans la boucle de symboles.

---

### PARTIE III — GAIN RX FIXE

#### 9. Pourquoi le gain doit être fixe pour ASK

Une modulation ASK encode l’information dans des différences d’amplitude.

Si un mécanisme automatique modifie le gain pendant les symboles, il peut réduire ou effacer ces écarts.

Le chemin de calibration Espressif démontre directement la combinaison :

```text
PBUS debug
   ↓
pbus_set_rxgain(...)
   ↓
IQ_EST
```

Le contrôle de gain fixe n’est donc pas une architecture inventée : c’est un mode réellement utilisé par le PHY pour ses propres mesures.

---

#### 10. Packing logiciel du gain RX forcé

Pour un mot de gain `g`, le mapping observé est :

```text
g[2:0]   → PBUS(3,2)[5:3]

g[3]     → PBUS(3,1)[6]
g[4]     → PBUS(3,1)[5]
g[5]     → PBUS(3,1)[4]
g[6]     → PBUS(3,1)[3]
g[7]     → PBUS(3,1)[2]
g[8]     → PBUS(3,1)[1]
g[9]     → PBUS(3,1)[0]

g[10]    → PBUS(2,1)[1]
g[14:11] → PBUS(2,1)[6:3]
```

Lors de l’écriture de `PBUS(2,1)`, le setter conserve :

```text
old & 0x185
```

soit les bits de contrôle :

```text
0, 2, 7, 8
```

Le code de gain est donc modifié sans détruire l’état de chemin conservé.

La conversion de ces codes en dB n’est pas nécessaire à la démodulation canonique ; elle relève de la caractérisation analogique.

---

#### 11. Gain baseband

Le PHY expose également `pbus_set_rxbbgain(...)`.

Son packing coarse/fine est distinct du gain RF. Pour la référence RX, il suffit que le choix soit :

- déterministe ;
- fixé avant la classification ASK ;
- inchangé pendant la trame.

La loi exacte index→dB n’est pas requise pour une classification relative en unités E4.

---

### PARTIE IV — SÉPARATION RX NORMAL / RXIQ LOOPBACK

#### 12. Le point critique : IQ_EST n’active pas le loopback

Le désassemblage de `rom_rfcal_rxiq()` montre que le mode spécial RXIQ est configuré explicitement par des opérations séparées autour de la calibration.

Avant la calibration RXIQ, le chemin observé contient notamment :

```text
rom_set_rxclk_en(1)
I2C block 0x77, host 0, reg16 bit2 = 1
I2C block 0x77, host 0, reg24 bit7 = 1
set_ana_inf_tx_scale(...)
start_tx_tone(...)
rxiq_cover(...)
stop_tx_tone(...)
```

À la sortie commune de `rom_rfcal_rxiq()`, ces champs spéciaux sont remis à zéro :

```text
reg24 bit7 = 0
reg16 bit2 = 0
rom_set_rxclk_en(0)
```

Cette séquence démontre que l’état spécial RXIQ est **une configuration explicite externe à IQ_EST**.

---

#### 13. `rom_iq_est_enable()` ne configure aucun loopback

`rom_iq_est_enable()` ne programme :

- ni le tone TX ;
- ni `set_loopback_gain()` ;
- ni les bits I²C RXIQ ci-dessus ;
- ni un mux RXIQ spécifique.

Il ne pilote que le bloc `IQ_EST_CTRL`.

Conclusion canonique :

> **Si l’on part du RX normal et que l’on n’exécute pas la séquence spéciale RXIQ, IQ_EST mesure le datapath RX dans son état normal courant.**

---

#### 14. Ce qu’il ne faut pas appeler dans le RX externe canonique

Pendant la préparation du récepteur externe :

```text
NE PAS appeler set_loopback_gain()
NE PAS activer les bits I2C RXIQ spéciaux
NE PAS démarrer de tone TX
```

Ces primitives appartiennent à la calibration interne et ne sont pas nécessaires au détecteur RX externe.

---

### PARTIE V — IQ ESTIMATOR

#### 15. Registre de contrôle

Le contrôle principal est :

```text
0x6000057C  IQ_EST_CTRL
```

Bitfields reconstruits :

```text
bit 0       ENABLE
bit 1       START / trigger
bits 16:2   N[14:0]
bit 18      MODE
bit 31      DONE / ready
```

Le mode démontré pour la mesure RX de puissance/corrélation est :

```text
MODE = 1
```

---

#### 16. Protocole de `rom_iq_est_enable(mode,N)`

La routine :

1. active le bloc ;
2. programme `MODE` ;
3. programme `N[14:0]` ;
4. positionne `START` ;
5. attend matériellement `DONE=1` ;
6. retourne seulement lorsque la mesure est terminée.

Aucune temporisation logicielle arbitraire n’est utilisée pour attendre la fin de la mesure.

---

#### 17. Valeur canonique de N

Le PHY v6 utilise directement :

```text
M = 1
N = 1024
```

pour une calibration RX basée sur IQ_EST.

Cette référence utilise donc :

```text
N_CANONICAL = 1024
```

comme valeur démontrée.

D’autres valeurs sont programmables, mais la relation `N→temps` est une caractérisation de performance et non une dépendance fonctionnelle du récepteur canonique.

---

#### 18. Désactivation / re-arm sûr

`rom_iq_est_disable()` effectue une remise au repos en deux phases :

```text
START = 0
puis
ENABLE = 0
```

La séquence canonique de répétition est donc volontairement la séquence ROM complète :

```text
rom_iq_est_enable(1,N)
attendre son retour
lire le résultat
rom_iq_est_disable()
```

puis recommencer.

Cette procédure est fermée à 100 % côté logiciel.

L’optimisation :

```text
ENABLE=1 permanent
START bas→haut seulement
```

n’est **pas utilisée** dans cette référence et n’est donc pas une dépendance du statut 100 %.

---

### PARTIE VI — MÉTRIQUE RX

#### 19. Registre de puissance / énergie

La métrique canonique est :

```text
0x600005E4
```

Elle appartient au bloc de résultats IQ estimator utilisé par les routines de calibration RX.

`rom_get_corr_power()` lit le même ensemble de résultats autour de :

```text
0x60000580
0x60000584
0x60000588
0x6000058C
0x600005DC
0x600005E0
0x600005E4
```

---

#### 20. Espressif utilise E4 comme valeur de décision

Dans `set_rx_gain_cal_iq()`, le PHY :

```text
IQ_EST(mode=1, N=1024)
   ↓
lecture 0x600005E4
   ↓
traitement / comparaison
   ↓
ajustement de gain
```

`0x600005E4` n’est donc pas seulement un registre observé : c’est une métrique réellement utilisée par le PHY pour prendre une décision de calibration RX.

---

#### 21. Pourquoi aucune conversion dBm n’est nécessaire

Pour OOK et ASK, on peut travailler directement dans l’espace numérique E4.

La seule exigence est que les classes apprises soient séparées dans cette métrique.

La conversion :

```text
E4 → dBm
```

est donc optionnelle et n’entre pas dans le chemin canonique.

---

### PARTIE VII — OOK

#### 22. Principe OOK RX

OOK comporte deux classes :

```text
OFF
ON
```

Le récepteur mesure E4 sur un préambule connu et détermine les centres :

```text
μ_OFF
μ_ON
```

Seuil canonique :

```text
T = (μ_OFF + μ_ON) / 2
```

Pour ne faire aucune hypothèse sur le sens numérique de E4 :

```text
si μ_ON > μ_OFF : ON ⇔ E > T
sinon            : ON ⇔ E < T
```

Cette règle rend la classification indépendante d’une unité physique absolue.

---

#### 23. Primitive OOK canonique

Pseudo-C :

```c
#include <stdint.h>
#include <stdbool.h>

#define IQ_POWER_ADDR 0x600005E4u

typedef void (*iq_est_enable_fn)(uint32_t mode, uint32_t n);
typedef void (*iq_est_disable_fn)(void);

#define ROM_IQ_EST_ENABLE  ((iq_est_enable_fn)0x40006430u)
#define ROM_IQ_EST_DISABLE ((iq_est_disable_fn)0x40006400u)

static inline uint32_t rd32(uint32_t addr)
{
    return *(volatile uint32_t *)addr;
}

static inline uint32_t rx_measure_e4(void)
{
    ROM_IQ_EST_ENABLE(1u, 1024u);
    uint32_t e = rd32(IQ_POWER_ADDR);
    ROM_IQ_EST_DISABLE();
    return e;
}
```

Classification :

```c
static inline bool ook_classify(uint32_t e,
                                uint32_t threshold,
                                bool on_is_high)
{
    return on_is_high ? (e > threshold) : (e < threshold);
}
```

---

### PARTIE VIII — ASK / M-ASK

#### 24. Principe ASK RX

Pour M-ASK, le récepteur fixe le gain puis apprend les centres E4 :

```text
μ0, μ1, ... μ(M-1)
```

Les centres sont triés par valeur numérique E4.

Pour deux centres voisins :

```text
Ti = (μi + μ(i+1)) / 2
```

La mesure reçue est ensuite classée dans l’intervalle correspondant.

---

#### 25. Pourquoi le gain doit rester identique entre calibration et données

Les seuils ASK sont définis dans l’espace E4 sous un état de gain donné.

Il faut donc conserver pendant la trame :

```text
même code de gain RF
même gain baseband
même chemin PBUS
même canal / PLL
```

Le changement de gain doit se faire entre phases de calibration/adaptation, pas au milieu des symboles ASK utilisés pour la décision.

---

#### 26. Classificateur M-ASK conceptuel

```c
uint32_t e = rx_measure_e4();

if (e < T0)
    symbol = 0;
else if (e < T1)
    symbol = 1;
else if (e < T2)
    symbol = 2;
else
    symbol = 3;
```

Pour une constellation dont l’ordre E4 est inversé par rapport à l’ordre logique, la table d’association des classes est simplement inversée après calibration.

---

### PARTIE IX — POURQUOI LE RSSI DE PAQUET EST REJETÉ

#### 27. `RxControl` / packet RSSI

Le RX Wi‑Fi expose des métadonnées par paquet, notamment RSSI/noise-floor.

Ces informations n’existent qu’après le traitement d’un paquet reconnu par le PHY/MAC.

Elles ne constituent donc pas une primitive générale pour :

```text
porteuse arbitraire OOK
ASK non 802.11
M-ASK propriétaire
```

La référence RX canonique n’utilise aucun RSSI de paquet.

---

### PARTIE X — POURQUOI LE CCA N’EST PAS NÉCESSAIRE

#### 28. CCA

Le corpus permet de configurer le CCA et son seuil, mais aucun readout CPU continu `CCA busy` n’est requis par le chemin de référence.

Même si un tel bit était identifié plus tard, il fournirait principalement une classification binaire.

IQ_EST fournit directement une métrique multi-niveaux compatible avec :

```text
OOK
ASK
M-ASK
```

Le CCA est donc volontairement exclu des dépendances de cette référence.

---

### PARTIE XI — INDÉPENDANCE DE PP / NET80211

#### 29. Aucun pilote caché nécessaire

L’audit de `libpp.a` et `libnet80211.a` n’a révélé aucun second pilote nécessaire de :

```text
0x6000057C
```

Les occurrences apparentes d’offset `+0x37C` inspectées dans ces bibliothèques étaient relatives à d’autres bases ou des faux décodages.

La primitive IQ_EST canonique reste donc encapsulée par le PHY/ROM et peut être utilisée sans logique packet PP/net80211.

---

### PARTIE XII — PROCÉDURE AUTONOME COMPLÈTE

#### 30. Phase A — boot et calibration initiale

1. démarrer normalement le silicium ;
2. laisser le PHY réaliser l’initialisation et les calibrations requises ;
3. laisser `rom_rfcal_rxiq()` terminer et restaurer ses bits spéciaux ;
4. fixer le canal / PLL ;
5. empêcher une mise en sleep ou une reprise Wi‑Fi pendant la session autonome.

Cette référence ne prétend pas remplacer toute l’initialisation analogique depuis un reset totalement froid par quelques écritures PBUS isolées.

---

#### 31. Phase B — établir le RX externe

```text
rom_pbus_xpd_rx_on()
rom_set_rxclk_en(1)
PBUS debug mode
programmer gain RX fixe
```

Puis vérifier qu’aucune primitive de calibration RXIQ/loopback n’est appelée pendant la session.

---

#### 32. Phase C — calibration des classes

##### OOK

Mesurer plusieurs fenêtres connues OFF et ON :

```text
μ_OFF = moyenne / centre robuste des mesures OFF
μ_ON  = moyenne / centre robuste des mesures ON
T     = milieu des deux centres
```

##### M-ASK

Pour chaque niveau connu du préambule :

```text
μ0 ... μM-1
```

Puis trier et placer les seuils entre centres voisins.

---

#### 33. Phase D — boucle de réception

```text
LOOP:
    rom_iq_est_enable(1, 1024)
    E = REG32(0x600005E4)
    rom_iq_est_disable()

    si OOK:
        classer avec un seuil

    si M-ASK:
        classer avec plusieurs seuils
```

Aucune restauration PBUS workmode n’est requise entre acquisitions dans la cible autonome.

---

### PARTIE XIII — SQUELETTE DE FIRMWARE

#### 34. API abstraite recommandée

```c
void rx_phy_boot_and_calibrate_once(void);
void set_channel_and_lock_rf(void);
void keep_rf_awake_and_take_exclusive_control(void);
void rx_rf_normal_on(void);
void rx_clock_on(void);
void pbus_enter_manual_rx_mode(void);
void set_fixed_rx_gain(uint16_t gain_code);
uint32_t rx_measure_e4(void);
```

Boucle :

```c
rx_phy_boot_and_calibrate_once();
set_channel_and_lock_rf();
keep_rf_awake_and_take_exclusive_control();

rx_rf_normal_on();
rx_clock_on();
pbus_enter_manual_rx_mode();
set_fixed_rx_gain(RX_GAIN_CODE);

calibrate_symbol_centres();

for (;;) {
    uint32_t e = rx_measure_e4();
    consume_symbol(classify_e4(e));
}
```

---

#### 35. Discipline MMIO

Les chemins ROM utilisent des barrières Xtensa `MEMW` autour des accès MMIO critiques.

Les implémentations qui remplacent les wrappers ROM par des accès directs doivent conserver cette discipline :

```c
static inline void memw(void)
{
    __asm__ volatile ("memw" ::: "memory");
}
```

La référence canonique préfère néanmoins les primitives ROM/PHY déjà démontrées pour IQ_EST.

---

### PARTIE XIV — ÉLÉMENTS DÉLIBÉRÉMENT NON NÉCESSAIRES

#### 36. Re-arm START-only

La possibilité d’un streaming plus rapide :

```text
ENABLE=1 permanent
START=0
START=1
```

reste une optimisation de performance.

Elle n’est pas utilisée dans cette référence.

Le chemin canonique utilise toujours :

```text
enable/acquire
read
disable
```

et est donc indépendant de cette optimisation.

---

#### 37. Nom électrique exact des bits PBUS conservés

`pbus_set_rxgain()` préserve explicitement les bits `0,2,7,8` du mot `PBUS(2,1)`.

Leur comportement logiciel pertinent est connu et préservé automatiquement par la primitive de gain.

Leur nom électrique détaillé n’est pas nécessaire au chemin canonique et n’est donc pas revendiqué dans cette référence.

---

#### 38. Mode IQ_EST M=0

Le corpus démontre l’usage RX pertinent de :

```text
M=1
```

Le sens physique détaillé de `M=0` n’est pas requis pour OOK/ASK et est exclu de cette référence.

---

### PARTIE XV — VALIDATION PHYSIQUE

#### 39. Pourquoi une validation silicium reste nécessaire

Le reverse-engineering logiciel répond à :

```text
quoi activer
quoi désactiver
quel chemin conserver
comment figer le gain
comment déclencher la mesure
quel registre lire
comment répéter l’acquisition
comment classer OOK/ASK sans dBm
```

Il ne peut pas, à lui seul, déterminer les performances analogiques exactes d’un exemplaire réel.

---

#### 40. Mesures physiques à réaliser

Pour caractériser le récepteur réel :

```text
E4 bruit seul
E4 pour plusieurs puissances d’entrée
E4 pour plusieurs codes de gain
START→DONE en cycles / µs
sensibilité OOK
BER selon SNR
saturation
nombre maximal de niveaux ASK séparables
robustesse au fading / interférences
```

Ces mesures ne remettent pas en cause le statut 100 % du chemin logiciel canonique.

---

### PARTIE XVI — CE QU’IL NE FAUT PAS FAIRE

#### 41. Ne pas utiliser le packet RSSI comme détecteur OOK

Il dépend d’un paquet Wi‑Fi décodé.

---

#### 42. Ne pas appeler `set_loopback_gain()` pour le RX externe

Cette primitive appartient à l’état spécial de calibration interne.

---

#### 43. Ne pas lancer un tone TX dans le chemin RX externe

Le tone TX est utilisé dans RXIQ interne, pas dans la réception externe canonique.

---

#### 44. Ne pas laisser un AGC implicite reprendre le contrôle

Rester dans le contexte PBUS manuel et conserver un gain déterministe pendant la classification ASK.

---

#### 45. Ne pas changer le gain pendant un symbole ASK

Les centres et seuils E4 doivent être valables pour un état de gain stable.

---

#### 46. Ne pas dépendre du CCA pour fonctionner

CCA peut être étudié comme accélérateur OOK, mais il ne fait pas partie du chemin de référence.

---

#### 47. Ne pas présenter E4 comme des dBm sans calibration

`E4` est une métrique interne exploitable directement ; sa conversion physique exige une caractérisation.

---

### PARTIE XVII — RÉFÉRENCE RAPIDE

#### 48. Cheat-sheet

```text
RX RF NORMAL:
    rom_pbus_xpd_rx_on()
    PBUS(2,1) = 0x184
    PBUS(3,2) = 0x006

RX CLOCK:
    rom_set_rxclk_en(1)

MANUAL RX:
    PBUS debug mode
    packet RX Wi-Fi retiré

FIXED GAIN:
    pbus_set_rxgain(code)
    conserver le même état pendant la trame

IQ_EST CTRL:
    0x6000057C
    bit0     ENABLE
    bit1     START
    16:2     N
    bit18    MODE
    bit31    DONE

CANONICAL ACQUISITION:
    mode = 1
    N    = 1024
    rom_iq_est_enable(1,1024)
    E = REG32(0x600005E4)
    rom_iq_est_disable()

OOK:
    calibrer μ_OFF et μ_ON
    seuil au milieu
    détecter le sens automatiquement

M-ASK:
    calibrer μ0...μM-1
    trier les centres
    seuils entre centres voisins

NE PAS UTILISER COMME DÉPENDANCE:
    packet RSSI
    CCA busy
    set_loopback_gain
    TX tone
    START-only re-arm
    E4→dBm
```

---

#### 49. Architecture complète recommandée

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
                    RX RF normal ON
                            │
                            ▼
                      RX clock ON
                            │
                            ▼
                    PBUS debug mode
                            │
              packet RX Wi-Fi retiré
                            │
                            ▼
                     gain RX fixe
                            │
                            ▼
                   IQ_EST M=1,N=1024
                            │
                            ▼
                      DONE matériel
                            │
                            ▼
                       lire E4
                            │
              ┌─────────────┴─────────────┐
              ▼                           ▼
             OOK                         ASK
         seuil unique               seuils multiples
              │                           │
              └─────────────┬─────────────┘
                            ▼
                         symbole
```

---

#### 50. Frontière exacte entre « décodé » et « à mesurer »

##### Décodé à 100 % dans cette référence

```text
comment établir le RX normal
comment garder la RX clock
comment retirer le packet RX
comment prendre le contrôle PBUS manuel
comment imposer un code de gain fixe
comment préserver les bits de chemin
comment éviter le loopback RXIQ
comment déclencher IQ_EST
comment attendre DONE
quel registre de puissance lire
comment réarmer de façon sûre
comment construire un slicer OOK relatif
comment construire un classificateur M-ASK relatif
```

##### À mesurer sur silicium

```text
relation E4→dBm
relation gain-code→dB
sensibilité
dynamique
latence
cadence maximale
BER
nombre de niveaux ASK utilisables
sélectivité physique
```

---

#### 51. Conclusion officielle du projet pour RX OOK/ASK

Pour le corpus ESP8266 étudié, le chemin logiciel canonique du récepteur autonome OOK/ASK est considéré **fermé à 100 %** :

```text
RX normal
→ RX clock
→ PBUS debug / gain fixe
→ IQ_EST M=1,N=1024
→ DONE
→ 0x600005E4
→ disable
→ classification relative
```

Le récepteur ne dépend :

- ni d’un paquet Wi‑Fi ;
- ni de PP/net80211 ;
- ni d’un RSSI de paquet ;
- ni d’un bit CCA direct ;
- ni du loopback RXIQ ;
- ni d’une conversion dBm ;
- ni de l’optimisation START-only.

OOK est obtenu par deux classes E4 calibrées. ASK/M-ASK est obtenu par plusieurs classes E4 calibrées sous gain fixe.

Les performances absolues sont volontairement classées comme **caractérisation matérielle**, exactement séparées du décodage logiciel de la procédure.

---

#### 52. Statut documentaire

**Document :** `ESP8266_RX_OOK_ASK_REFERENCE_OFFICIELLE_PROJET_v1.0.md`  
**Édition :** 1.0  
**Émission :** unique  
**Statut logiciel :** **100 % fermé dans le périmètre canonique décrit**  
**Maintenance future :** aucune — document figé  
**Suite du projet :** caractérisation silicium/RF et optimisations optionnelles dans le document maître vivant.

---

# ANNEXE C — TX FSK / M-FSK — référence v1.0 complète

### ESP8266 — Générateur FSK / M-FSK autonome
#### Référence technique officielle du projet de reverse-engineering

**Édition unique : v1.0**  
**Date : 2026-09-13**  
**Statut : FIGÉE — fermeture à 100 % du périmètre logiciel/statique TX FSK**  
**Portée : ESP8266 / mask-ROM + PHY v6 du corpus analysé**

> **Important — statut du mot “officiel”.** Ce document est la référence officielle **du projet de reverse-engineering mené sur le corpus analysé**. Il ne s’agit pas d’un document officiel d’Espressif et il ne doit pas être présenté comme tel.

> **Règle de maintenance.** Cette édition est volontairement figée. Les découvertes ultérieures, mesures RF, lois `tone_control→Hz`, optimisations de débit ou caractérisations de phase/settling ne modifieront pas cette édition ; elles continuent dans le document maître vivant `ESP8266_WIFI_PHY_REVERSE_ENGINEERING.md`.

---

#### 1. Objet

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

#### 2. Définition du « 100 % » dans cette référence

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

##### 2.1 Tableau final de fermeture logicielle

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

#### 3. Corpus et fonctions utilisées

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

#### 4. Registres du générateur de tone

Trois slots sont présents :

```text
0x600005B8   TONE SLOT 1
0x600005BC   TONE SLOT 2
0x600005C4   TONE SLOT 3
```

Pour la référence FSK canonique, le **slot 1** est utilisé.

Les usages ROM directs retrouvés pour les calibrations principales utilisent le slot 1.

---

#### 5. Packing du slot 1

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

#### 6. Largeur pratique du champ `tone_control`

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

### PARTIE I — PRINCIPE FSK

#### 7. Principe 2-FSK

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

#### 8. Principe M-FSK

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

#### 9. Valeurs réellement observées dans le corpus

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

### PARTIE II — PRIMITIVE TX FSK CANONIQUE

#### 10. Masque du champ bas

Pour le firmware autonome :

```c
#define TONE1_ADDR          0x600005B8u
#define TONE_CONTROL_MASK   0x000003FFu
```

---

#### 11. Primitive RMW

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

#### 12. Pourquoi le RMW est obligatoire

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

#### 13. Normalisation du slot avant la FSK

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

### PARTIE III — POURQUOI LA RFPLL N’EST PAS LE MODULATEUR RAPIDE

#### 14. `set_rf_freq_offset()` n’est pas un simple offset numérique

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

#### 15. Conséquence pour le fast-FSK

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

#### 16. `chip_v6_set_chan_offset()` est encore plus lourd

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

#### 17. Construction SDM RFPLL

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

### PARTIE IV — RF / PBUS / TX CLOCK

#### 18. Le tone generator n’initialise pas toute la RF

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

#### 19. TX clock

`rom_start_tx_tone()` active la TX clock.

`rom_stop_tx_tone()` :

```text
clear bit18
puis
TX clock off
```

La FSK rapide doit donc maintenir la TX clock active entre les symboles.

---

#### 20. Ne pas utiliser `stop_tx_tone()` entre symboles

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

### PARTIE V — PROCÉDURE AUTONOME COMPLÈTE

#### 21. Phase A — boot / calibration

1. démarrer normalement le silicium ;
2. laisser le PHY effectuer ses initialisations/calibrations ;
3. fixer le canal / RFPLL ;
4. empêcher sleep/wakeup pendant la session ;
5. abandonner le trafic Wi‑Fi normal.

Cette référence ne prétend pas remplacer tout le cold-start RF par quelques écritures isolées.

---

#### 22. Phase B — préparation RF TX

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

#### 23. Phase C — normalisation du tone

Choisir :

```text
K_INITIAL
digital_scale fixe
mode normal
gate actif
```

Réécrire complètement le slot normal une fois.

---

#### 24. Phase D — caractérisation des codes FSK

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

#### 25. Phase E — modulation

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

### PARTIE VI — TIMING ET CONCURRENCE

#### 26. Propriétaire unique du slot

Le RMW n’est pas atomique vis-à-vis d’un autre écrivain.

Après abandon du Wi‑Fi :

```text
un seul propriétaire du tone slot
    =
boucle de modulation FSK
```

---

#### 27. Interruptions

Pour une modulation temporellement propre, éviter les perturbations par :

```text
ISR Wi-Fi
timers PHY de maintenance
sleep/wakeup
reconfiguration canal/PLL
calibrations concurrentes
```

---

#### 28. `MEMW`

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

### PARTIE VII — ÉLÉMENTS DÉLIBÉRÉMENT HORS PÉRIMÈTRE LOGICIEL

#### 29. Loi `tone_control → Hz`

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

#### 30. `app_tone_offset_khz` ne ferme pas la loi

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

#### 31. Phase accumulator / NCO

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

#### 32. Hot-update : frontière exacte

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

### PARTIE VIII — VALIDATION PHYSIQUE

#### 33. Pourquoi une validation silicium reste nécessaire

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

#### 34. Mesures TX à réaliser

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

#### 35. Validation 2-FSK

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

#### 36. Validation M-FSK

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

### PARTIE IX — CE QU’IL NE FAUT PAS FAIRE

#### 37. Ne pas utiliser `stop_tx_tone()` par symbole

Il coupe la TX clock.

---

#### 38. Ne pas utiliser `set_rf_freq_offset()` par symbole

Il passe par RFPLL + attente de calibration.

---

#### 39. Ne pas supposer `tone_control = fréquence en kHz`

Aucune telle identité n’est démontrée.

---

#### 40. Ne pas supposer une loi linéaire

Le corpus ne démontre pas :

```text
f ∝ K
```

sur toute la plage.

---

#### 41. Ne pas écraser tout le slot

Toujours préserver :

```text
scale
gate
modes
bits supérieurs
```

par RMW ciblé.

---

#### 42. Ne pas laisser le Wi‑Fi reprendre le slot

La référence autonome suppose un seul propriétaire du générateur pendant la session.

---

### PARTIE X — ÉTAT FINAL DU MODÈLE

#### 43. 2-FSK canonique

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

#### 44. M-FSK canonique

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

#### 45. Frontière exacte entre « décodé » et « à mesurer »

##### Décodé à 100 % côté logiciel/statique

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

##### À mesurer sur silicium

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

### PARTIE XI — NOTE RF / CONFORMITÉ

#### 46. Utilisation en laboratoire

La modulation autonome du générateur de tone sort du chemin Wi‑Fi normal.

Les essais doivent être réalisés dans un environnement RF contrôlé et conforme à la réglementation applicable, avec atténuation/charge ou instrumentation adaptée lorsque nécessaire.

Cette note ne modifie pas le modèle logiciel.

---

### PARTIE XII — RÉFÉRENCE RAPIDE

#### 47. Cheat-sheet

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

#### 48. Pseudo-code final minimal

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

#### 49. Conclusion officielle du projet pour TX FSK/M-FSK

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

#### 50. Statut documentaire

```text
Document : référence TX FSK/M-FSK du projet
Version  : 1.0
Statut   : FIGÉE
Base     : mask-ROM ESP8266 + PHY v6 + corpus analysé
Suite    : caractérisation RF et optimisations dans le maître vivant
```

---

# ANNEXE D — RX FSK / M-FSK — référence v1.0 complète

### ESP8266 — Récepteur FSK / M-FSK
#### Référence technique officielle du projet de reverse-engineering — périmètre logiciel/statique

**Édition unique : v1.0**  
**Date : 2026-09-13**  
**Statut : FIGÉE — fermeture à 100 % du périmètre logiciel/statique RX FSK**  
**Portée : ESP8266 / mask-ROM + PHY v6 du corpus analysé**

> **Important — statut du mot “officiel”.** Ce document est la référence officielle **du projet de reverse-engineering mené sur le corpus analysé**. Il ne s’agit pas d’un document officiel d’Espressif et il ne doit pas être présenté comme tel.

> **Important — portée du mot “récepteur FSK”.** Le chemin logiciel de lecture/discrimination CFO est fermé à 100 %. La capacité du baseband à produire une **nouvelle mesure CFO valide sur un tone/FSK non‑802.11** reste une validation silicium/baseband séparée. Cette validation physique n’est pas incluse dans le score logiciel.

> **Règle de maintenance.** Cette édition est figée pour le périmètre logiciel/statique. Les validations sur silicium, mesures de cadence, sensibilité, comportement sur tone non‑802.11 et optimisations de démodulation continuent dans le document maître vivant `ESP8266_WIFI_PHY_REVERSE_ENGINEERING.md`.

---

#### 1. Objet

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

#### 2. Définition du « 100 % » dans cette référence

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

##### 2.1 Tableau final de fermeture logicielle

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

#### 3. Corpus et fonctions utilisées

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

### PARTIE I — RÉSULTAT CFO

#### 4. Registre principal

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

#### 5. Gate de contexte WDEV

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

#### 6. Sentinelle invalide

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

#### 7. CFO brut signé

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

#### 8. Conversion exacte

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

### PARTIE II — HANDSHAKE CFO

#### 9. Registre de finalisation

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

#### 10. Le handshake est inconditionnel

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

#### 11. Rôle logiciel final de `0x600098DC[3:0]`

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

#### 12. Aucun second re-arm CPU

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

### PARTIE III — PRIMITIVE CFO CANONIQUE

#### 13. Registres et constantes

```c
#include <stdint.h>
#include <stdbool.h>

#define WDEV_CFO_CTX_ADDR   0x3FF2003Cu
#define BB_CFO_RESULT_ADDR  0x60009800u
#define BB_CFO_HS_ADDR      0x600098DCu

#define CFO_INVALID         ((int16_t)0x7FFF)
```

---

#### 14. Discipline MMIO

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

#### 15. Primitive canonique reconstruite

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

#### 16. Variante avec statut explicite

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

### PARTIE IV — MESURE VS CORRECTION

#### 17. `phy_meas_freq_offset`

Le getter CFO stocke son résultat dans :

```text
phy_meas_freq_offset
```

Cette variable représente la dernière mesure CFO calculée/lue.

---

#### 18. `phy_freq_offset`

Une variable distincte :

```text
phy_freq_offset
```

est utilisée par les chemins de correction / canal / retuning.

Il est incorrect de les confondre.

---

#### 19. `phy_get_freq_param()`

Le chemin reconstruit sépare explicitement :

```text
*corr_out = phy_freq_offset
*meas_out = phy_meas_freq_offset
```

La distinction mesure/correction est donc fermée.

---

### PARTIE V — CONSOMMATION STANDARD WDEV / PP

#### 20. Consommation CFO après RX-success

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

#### 21. Événement WDEV bit8

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

#### 22. Pourquoi cela ne limite pas la fermeture logicielle

Le logiciel standard choisit de consommer le CFO après RX-success.

La question :

```text
le baseband avait-il déjà produit le CFO avant ?
```

est une propriété de timing matériel.

Elle est donc séparée du contrat CPU du getter.

---

### PARTIE VI — CLASSIFICATION FSK

#### 23. Principe 2-FSK

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

#### 24. Pourquoi l’unité absolue n’est pas obligatoire

Comme pour l’ASK en unités E4, un discriminateur relatif peut travailler directement dans l’espace de sortie CFO.

La seule exigence est :

```text
classes séparables
```

Il n’est donc pas nécessaire de convertir la mesure en fréquence RF absolue pour classer deux états.

---

#### 25. M-FSK

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

#### 26. Valeurs invalides

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

### PARTIE VII — ARCHITECTURE AUTONOME CANDIDATE

#### 27. Architecture logicielle

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

#### 28. Point non encore démontré physiquement

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

#### 29. Pourquoi cette validation n’annule pas le 100 % logiciel

Le logiciel est complètement défini pour les deux cas :

```text
CFO valide
CFO invalide
```

Ce qui reste à déterminer est si le **hardware** produit une valeur valide pour le signal cible.

C’est une qualification fonctionnelle du bloc baseband, pas une fonction CPU inconnue.

---

### PARTIE VIII — PROCÉDURE DE VALIDATION SILICIUM

#### 30. Test A — trame Wi-Fi valide

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

#### 31. Test B — entrée du chemin WDEV bit8

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

#### 32. Test C — trame finalement rejetée

Capturer le CFO avant la décision discard.

Si :

```text
bit0 = 1
```

sur une frame ensuite rejetée, cela prouve que le CFO peut être produit avant RX-success.

---

#### 33. Test D — tone / FSK non-802.11

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

### PARTIE IX — TIMING ET ACQUISITION

#### 34. Ne pas confondre cadence logicielle et cadence hardware

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

#### 35. Handshake après chaque tentative

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

### PARTIE X — CE QU’IL NE FAUT PAS FAIRE

#### 36. Ne pas utiliser `phy_freq_offset` comme mesure RX

C’est l’état de correction, pas la mesure brute reçue.

---

#### 37. Ne pas ignorer la sentinelle `0x7FFF`

Une valeur invalide ne doit jamais être classée comme symbole.

---

#### 38. Ne pas omettre le handshake sur erreur

Le handshake est inconditionnel dans le getter exact.

---

#### 39. Ne pas appeler précocement le getter comme simple sonde passive

Le getter modifie :

```text
0x600098DC
```

Une instrumentation précoce doit lire directement le résultat sans provoquer le handshake avant la capture.

---

#### 40. Ne pas assimiler `0x3FF2003C[19:16]` à `rxend_state`

Le premier est un champ MMIO WDEV de 4 bits utilisé comme gate CFO.

`RxControl.rxend_state` est un champ de descripteur distinct.

Aucune identité binaire directe n’est démontrée.

---

#### 41. Ne pas utiliser IQ_EST E4 comme discriminateur FSK général

E4 est une métrique d’énergie/puissance adaptée à OOK/ASK.

Elle ne remplace pas le CFO pour une FSK générale.

---

#### 42. Ne pas présenter le CFO non-802.11 comme déjà validé

Le logiciel est fermé ; la disponibilité autonome de la métrique reste une validation matérielle.

---

### PARTIE XI — FRONTIÈRE EXACTE DU MODÈLE

#### 43. Décodé à 100 % côté logiciel/statique

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

#### 44. À mesurer sur silicium/baseband

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

### PARTIE XII — RÉFÉRENCE RAPIDE

#### 45. Cheat-sheet

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

#### 46. Pseudo-code final minimal

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

#### 47. Exemple conceptuel 2-FSK

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

#### 48. Conclusion officielle du projet pour RX FSK/M-FSK

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

#### 49. Statut documentaire

```text
Document : référence RX FSK/M-FSK du projet
Version  : 1.0
Statut   : FIGÉE — périmètre logiciel/statique
Base     : mask-ROM ESP8266 + PHY v6 + PP/WDEV du corpus analysé
Suite    : validation CFO autonome sur silicium dans le maître vivant
```

---

# ANNEXE E — Historique QPSK/QAM complet v0.60 → v0.83

> Cette annexe reprend le segment QAM complet du document maître vivant, depuis l'ouverture QPSK/QAM v0.60 jusqu'à la fermeture de l'interface RX standard en v0.83. Les résultats nouveaux de `librftest.a` figurent dans la synthèse normative v2.0 et supersèdent les hypothèses ATE devenues vérifiables.

### 36BY. QPSK/QAM TX — actionneurs matériels de correction gain/phase I/Q — v0.60

Cette passe ouvre officiellement le chantier **QPSK/QAM** après clôture logicielle OOK/ASK/FSK.
La première question est volontairement plus primitive que « peut-on faire du QAM ? » :

```text
le silicium expose-t-il au CPU des actionneurs séparés
de gain I/Q et de phase I/Q TX ?
```

La réponse est désormais **oui**, dans le contexte des calibrations TXIQ.

---

#### 36BY.1 Fonctions ROM concernées

Le linker ROM Espressif et la table `g_phyFuns` donnent :

```text
rom_rfcal_txiq_set_reg  = 0x40008A70   g_phyFuns + 0x108
rom_set_txiq_cal        = 0x40008D34   g_phyFuns + 0x114
```

Elles sont distinctes de :

```text
rom_rfcal_txiq_cover    = 0x400088B8
rom_start_tx_tone
rom_set_ana_inf_tx_scale
```

La nouvelle analyse porte sur les octets de la mask-ROM exacte du corpus.

---

#### 36BY.2 Rappel fermé : le selector de TXIQ distingue gain et phase

Le flot de `txiq_cover()` déjà reconstruit ferme :

```text
sel = 1
    → états de test 0x4 → 0x8
    → résultat byte[0]
    → txiq_gain

sel = 0
    → états de test 0x0 → 0x1
    → résultat byte[1]
    → txiq_phase
```

Ce mapping est maintenant réutilisé pour nommer les deux branches de
`rom_rfcal_txiq_set_reg()`.

---

#### 36BY.3 Branche `selector=1` : trim de gain I/Q TX

`rom_rfcal_txiq_set_reg()` reçoit un coefficient signé et le selector.

Lorsque :

```text
selector != 0
```

le coefficient est transformé en :

```text
sign
magnitude = min(abs(value), 15)
```

puis écrit par deux opérations `rom_i2c_writeReg_Mask()` :

```text
block = 0x77
host  = 0

reg16 bit0    := sign
reg16 bits7:3 := magnitude
```

Le champ physique réservé à la magnitude fait 5 bits, mais le chemin de calibration observé
borne la valeur utile à :

```text
0..15
```

Le contrat logiciel démontré est donc un **trim signé de gain I/Q TX**.

---

#### 36BY.4 Branche `selector=0` : trim de phase I/Q TX

Lorsque :

```text
selector == 0
```

la même primitive utilise :

```text
sign
magnitude = min(abs(value), 31)
```

puis programme :

```text
block = 0x77
host  = 0

reg15 bit6    := sign
reg17 bits5:0 := magnitude
```

Le champ de magnitude fait 6 bits et la calibration borne l'amplitude utile à :

```text
0..31
```

Le contrat logiciel démontré est donc un **trim signé de phase I/Q TX**.

---

#### 36BY.5 Reconstruction compacte

```text
                        rom_rfcal_txiq_set_reg(value, selector, ...)
                                      │
                    ┌─────────────────┴─────────────────┐
                    │                                   │
               selector=1                          selector=0
                    │                                   │
              TXIQ GAIN                             TXIQ PHASE
                    │                                   │
       abs(value) borné à 15               abs(value) borné à 31
                    │                                   │
       0x77/reg16 bit0 = sign             0x77/reg15 bit6 = sign
       0x77/reg16[7:3] = magnitude        0x77/reg17[5:0] = magnitude
```

Cette séparation n'est plus une simple interprétation des noms : elle est obtenue par combinaison
du flot de `txiq_cover()` et des écritures I²C exactes de la ROM.

---

#### 36BY.6 `rom_set_txiq_cal()` expose deux champs signés dans `0x60009860`

La routine :

```text
rom_set_txiq_cal()
```

lit :

```text
0x60009860
```

sous deux masques exacts :

```text
0x1F000000
0x00FC0000
```

soit :

```text
bits28:24  → champ 5 bits
bits23:18  → champ 6 bits
```

La routine reconstruit ensuite les représentations signées :

```text
champ 5 bits : si valeur >= 16, valeur -= 32
champ 6 bits : si valeur >= 32, valeur -= 64
```

puis propage les informations de signe vers :

```text
0x77/reg16.bit0
0x77/reg15.bit6
```

Les largeurs correspondent exactement aux deux familles gain/phase décrites ci-dessus.

Le classement recommandé devient donc :

```text
0x60009860[28:24] → état/correction TXIQ de largeur 5 bits
0x60009860[23:18] → état/correction TXIQ de largeur 6 bits
```

avec un lien fonctionnel très fort respectivement vers les chemins gain et phase.

---

#### 36BY.7 Ce que cela change pour QPSK/QAM

Avant v0.60, on savait seulement que le PHY :

```text
mesure le mismatch de gain I/Q
mesure le mismatch de phase I/Q
```

La v0.60 ajoute :

```text
le CPU peut réellement appliquer
un trim gain I/Q signé
et un trim phase I/Q signé
via l'I²C interne
```

C'est une nouvelle preuve que le TX contient une chaîne vectorielle I/Q réglable.

---

#### 36BY.8 Ce que cela NE démontre PAS encore

Il serait incorrect de conclure :

```text
phase_trim = 31  → +90°
phase_trim = -31 → -90°
```

Aucune loi :

```text
code → degrés
```

n'est fournie par la ROM.

Surtout, ces contrôles sont des **corrections de mismatch autour du modulateur I/Q nominal**.
Leur plage physique peut être petite.

Ainsi :

```text
actionneur de trim de phase     → démontré
rotation de constellation 90°  → non démontrée
actionneur de trim de gain      → démontré
amplitude I et Q indépendantes → non démontrée
```

Le QPSK exige donc encore un mécanisme de sélection de quadrant / signe I-Q, ou une preuve
que le trim de phase couvre une plage suffisamment grande.

---

#### 36BY.9 Conséquence pour la stratégie de recherche QPSK TX

La priorité TX devient :

```text
1. déterminer l'échelle physique du trim de phase
2. déterminer si le trim est hot-update sans relance calibration
3. identifier un contrôle de signe/quadrant I/Q
4. décoder le rôle physique des états tone_mode TXIQ
5. tester si plusieurs tone slots se combinent dans des chemins orthogonaux
```

La piste :

```text
ASK + FSK = QAM
```

est explicitement rejetée : elle produit amplitude + fréquence, pas amplitude + phase.

---

#### 36BY.10 Statut QPSK/QAM TX après v0.60

| Élément | Statut |
|---|---:|
| existence d'un datapath I/Q TX | **démontrée indirectement très fortement** |
| calibration gain I/Q TX | **100 % fonction logicielle** |
| calibration phase I/Q TX | **100 % fonction logicielle** |
| actionneur trim gain I/Q signé | **~99 % logiciel** |
| actionneur trim phase I/Q signé | **~99 % logiciel** |
| mapping I²C gain | **~99 %** |
| mapping I²C phase | **~99 %** |
| champs TXIQ de `0x60009860` | **~95 % structurel** |
| trim phase → degrés | **ouvert** |
| trim gain → ratio I/Q | **ouvert** |
| hot-update des trims par symbole | **ouvert** |
| sélection de quadrants ±I/±Q | **ouverte** |
| **QPSK TX propriétaire** | **~55–65 % architectural** |
| **QAM TX propriétaire** | **~50–60 % architectural** |

La progression est réelle : le verrou n'est plus « existe-t-il un réglage de phase I/Q ? »,
mais « ce réglage de calibration peut-il devenir un actionneur de modulation, et où se trouve
la sélection de quadrant ? ».

---


### 36BZ. QPSK/QAM RX — `rom_dc_iq_est()` comme primitive vectorielle intégrée — v0.61

Après la découverte v0.60 des trims TX I/Q, cette passe cherche le pendant RX :

```text
le CPU peut-il obtenir deux composantes I et Q séparées
sans accéder au flux ADC/IQ brut ?
```

La réponse logicielle est désormais **oui**, sous forme d'une moyenne intégrée sur une fenêtre
IQ_EST.

---

#### 36BZ.1 ABI reconstruite de `rom_dc_iq_est()`

Adresse officielle et binaire exact :

```text
rom_dc_iq_est = 0x4000615C
```

Les trois premiers arguments Xtensa sont utilisés ainsi :

```text
a2 → mode
a3 → N
a4 → pointeur de sortie
```

Le début de la routine charge :

```text
g_phyFuns + 0x34
    = rom_iq_est_enable
```

puis appelle directement :

```text
iq_est_enable(mode, N)
```

sans transformer les deux paramètres.

---

#### 36BZ.2 Deux sorties séparées I et Q

Après le retour de l'estimateur :

```text
base = 0x60000200
```

la routine lit :

```text
base + 0x3DC = 0x600005DC
base + 0x3E0 = 0x600005E0
```

Pour chacune :

```text
valeur = REG32(...) >> 6
```

où le décalage est arithmétique.

Elle calcule ensuite :

```text
diviseur = N + 1
```

et appelle :

```text
__divsi3 @ 0x4000DC88
```

donc une division **signée**.

Les deux résultats sont écrits dans deux mots 32 bits consécutifs :

```text
out[0] = ((int32_t)REG32(0x600005DC) >> 6) / (N + 1)
out[1] = ((int32_t)REG32(0x600005E0) >> 6) / (N + 1)
```

puis :

```text
rom_iq_est_disable()
```

est appelé avant le retour.

---

#### 36BZ.3 Pseudo-code exact au niveau logiciel

```c
void dc_iq_est(uint32_t mode, uint32_t N, int32_t out[2])
{
    iq_est_enable(mode, N);

    int32_t i_acc = (int32_t)REG32(0x600005DC);
    int32_t q_acc = (int32_t)REG32(0x600005E0);

    i_acc >>= 6;
    q_acc >>= 6;

    int32_t d = (int32_t)N + 1;

    out[0] = i_acc / d;
    out[1] = q_acc / d;

    iq_est_disable();
}
```

Cette reconstruction ne dépend pas d'une interprétation flottante des noms de registres :
les deux lectures, les deux divisions signées et les deux stores sont visibles directement
dans la mask-ROM.

---

#### 36BZ.4 `N=0` est accepté par le chemin CPU

`rom_iq_est_enable()` :

```text
N &= 0x7FFF
N <<= 2
```

puis programme :

```text
0x6000057C[16:2]
```

Aucune vérification :

```text
N > 0
N >= 1
N >= seuil
```

n'est effectuée.

De son côté, `rom_dc_iq_est()` calcule :

```text
N + 1
```

avant les divisions.

Donc pour :

```text
N = 0
```

le logiciel demande bien la fenêtre minimale encodable et utilise :

```text
diviseur = 1
```

Conclusion exacte :

> **`N=0` est un appel logiciel valide de l'API ROM.**

Ce qui reste à mesurer est la durée physique et le nombre exact d'échantillons matériels
effectivement intégrés lorsque `N=0`.

---

#### 36BZ.5 Ce n'est toujours PAS un flux I/Q brut

Il faut distinguer :

```text
SDR classique:
I[0],Q[0], I[1],Q[1], I[2],Q[2] ... à cadence ADC
```

de :

```text
ESP8266 IQ_EST:
déclencher une fenêtre
        ↓
accumuler en hardware
        ↓
retourner moyenne I / moyenne Q
        ↓
désactiver
```

La v0.61 ne découvre donc aucun FIFO de samples ADC.

Elle découvre une primitive plus proche de :

```text
integrate-and-dump I/Q
```

ou :

```text
vector sampler à fenêtre
```

assisté matériellement.

---

#### 36BZ.6 Pourquoi cela intéresse QPSK

Un symbole QPSK idéal peut être représenté :

```text
s = I + jQ
```

avec quatre quadrants.

Si une fenêtre IQ_EST courte et correctement alignée sur un symbole externe restitue un couple
dont le signe/angle suit la constellation :

```text
(+I,+Q)
(-I,+Q)
(-I,-Q)
(+I,-Q)
```

alors le CPU peut classifier le quadrant sans recevoir le flux I/Q complet.

Architecture candidate :

```text
RF RX
  ↓
mixer / baseband hardware
  ↓
IQ_EST fenêtre courte
  ↓
rom_dc_iq_est()
  ↓
(I_mean, Q_mean)
  ↓
correction DC / rotation / gain
  ↓
quadrant
  ↓
2 bits QPSK
```

---

#### 36BZ.7 Extension QAM

Pour une QAM rectangulaire :

```text
I ∈ plusieurs niveaux
Q ∈ plusieurs niveaux
```

Si la relation entre `(I_mean,Q_mean)` et le signal externe est suffisamment monotone et stable,
la même primitive pourrait fournir :

```text
16-QAM → 4 niveaux I × 4 niveaux Q
64-QAM → 8 niveaux I × 8 niveaux Q
```

Le logiciel de décision serait trivial comparé au problème matériel.

Le verrou est la fidélité physique de la primitive, pas l'algorithme de classification.

---

#### 36BZ.8 Limites importantes

Le nom ROM est :

```text
dc_iq_est
```

et non :

```text
symbol_iq_sample
```

La fonction a été conçue pour l'estimation/calibration DC I/Q.

Ainsi, il reste à démontrer :

```text
1. le signal RF externe atteint les accumulateurs I/Q dans l'état RX autonome visé
2. un symbole single-carrier produit un vecteur moyen non nul exploitable
3. le DC cancellation / filtrage interne ne détruit pas cette information
4. N petit termine correctement et assez vite
5. le couple conserve le signe et la phase de manière stable
6. CFO / erreur de phase peuvent être corrigés au rythme requis
```

La v0.61 ne promeut aucun de ces points au rang de fait.

---

#### 36BZ.9 Corrélateurs `0x60000580..58C` comme voie secondaire

Le bloc expose également :

```text
0x60000580
0x60000584
0x60000588
0x6000058C
```

déjà utilisés pour la calibration RXIQ.

Les combinaisons observées incluent :

```text
Re-like = R0 + R3
Im-like = R1 - R2
```

et `rom_get_corr_power()` calcule ensuite :

```text
(Re-like)^2 + (Im-like)^2
```

Cette structure démontre une **corrélation complexe interne**, mais ne ferme pas encore ce qui
sert de référence au corrélateur.

Pour QPSK/QAM, ces registres sont donc classés comme :

```text
voie vectorielle/corrélative candidate
```

mais la priorité reste `dc_iq_est`, dont les deux sorties I et Q sont plus directes.

---

#### 36BZ.10 Statut RX QPSK/QAM v0.61

| Élément | Statut |
|---|---:|
| deux accumulateurs I/Q séparés accessibles CPU | **~99 % logiciel** |
| normalisation signée `/ (N+1)` | **100 % logiciel** |
| API ROM retournant deux composantes | **~99 %** |
| `N` programmable sur 15 bits | **~99 %** |
| `N=0` accepté logiciellement | **100 % logiciel** |
| flux I/Q brut ADC vers LX106 | **non identifié** |
| couple moyen I/Q sur RX externe | **fort candidat, validation silicium** |
| conservation du quadrant QPSK | **ouverte** |
| cadence de fenêtres courtes | **ouverte** |
| correction de rotation/CFO en logiciel | **architecture connue, performance ouverte** |
| **QPSK RX assisté PHY** | **~60–70 % architectural** |
| **QAM RX assisté PHY** | **~55–65 % architectural** |
| **SDR brute I/Q** | **toujours non démontrée** |

---

#### 36BZ.11 Test discriminant suivant

La prochaine validation conceptuelle à préparer est :

```text
entrée RF single-carrier à phase contrôlée
        ↓
quatre phases connues
        ↓
dc_iq_est(mode démontré, N court)
        ↓
tracer (I_mean,Q_mean)
```

Le résultat attendu pour une primitive vectorielle exploitable serait quatre amas distincts
qui tournent avec la phase d'entrée.

Cette validation doit être effectuée en environnement RF contrôlé ; elle n'est pas substituable
par davantage de désassemblage si aucune autre interface vectorielle n'apparaît.

---


### 36CA. QPSK/QAM TX — packing digital TXIQ exact de `0x60009860` — v0.62

La v0.60 avait fermé les deux **trims I²C** gain/phase.
La présente passe cherche où les valeurs calculées par la calibration sont stockées et
appliquées dans le baseband numérique.

Le résultat est plus important pour QPSK/QAM : la ROM programme directement un registre MMIO
avec les deux coefficients.

---

#### 36CA.1 Écriture exacte dans `rom_rfcal_txiq()`

Dans :

```text
rom_rfcal_txiq @ 0x40008610
```

la base :

```text
0x60009600
```

est chargée, puis l'offset :

```text
0x260
```

est utilisé.

Adresse finale :

```text
0x60009860
```

La séquence finale réalise :

```text
old = REG32(0x60009860)
old &= 0xE000FFFF
old |= gain_field
old |= phase_field
old |= 0x00030000
REG32(0x60009860) = old
```

Le masque :

```text
0xE000FFFF
```

préserve :

```text
bits31:29
bits15:0
```

et libère exactement :

```text
bits28:16
```

pour le bloc TXIQ.

---

#### 36CA.2 Saturation exacte du gain

Le premier résultat de `rfcal_txiq_cover()` est celui du chemin :

```text
selector = 1
    → txiq_gain
```

La ROM le sature :

```text
gain < -15 → -15
gain > +15 → +15
```

soit :

```text
gain ∈ [-15,+15]
```

Puis elle forme le code :

```text
gain >= 1:
    gain_code = 32 - gain

gain <= 0:
    gain_code = -gain
```

Ce qui se réduit exactement à :

```c
gain_code = (-gain) & 0x1F;
```

Le code est ensuite placé dans :

```text
0x60009860[28:24]
```

Donc :

```text
bits28:24 = (-txiq_gain_clamped) mod 32
```

---

#### 36CA.3 Saturation exacte de la phase

Le second résultat de `rfcal_txiq_cover()` est :

```text
selector = 0
    → txiq_phase
```

La ROM sature :

```text
phase < -31 → -31
phase > +31 → +31
```

Puis :

```text
phase >= 0:
    phase_code = phase

phase < 0:
    phase_code = phase + 64
```

soit simplement :

```c
phase_code = phase & 0x3F;
```

Le champ est ensuite décalé de 18 bits :

```text
0x60009860[23:18] = phase_code
```

Donc :

```text
bits23:18 = txiq_phase_clamped mod 64
```

---

#### 36CA.4 Bits17:16 forcés à `11`

La ROM OR explicitement :

```text
0x00030000
```

avant le write.

Donc :

```text
0x60009860[17:16] = 0b11
```

dans l'état TXIQ configuré par cette routine.

Leur rôle physique exact n'est pas nommé, mais leur état requis est désormais démontré.

---

#### 36CA.5 Layout TXIQ digital reconstruit

Le sous-champ devient :

```text
31 29 28          24 23               18 17 16 15            0
┌─────┬─────────────┬───────────────────┬─────┬────────────────┐
│keep │ gain_code   │ phase_code        │  11 │ keep           │
│     │ 5 bits      │ 6 bits            │     │                │
└─────┴─────────────┴───────────────────┴─────┴────────────────┘
```

avec :

```text
gain_code  = (-clamp(txiq_gain,-15,+15)) & 0x1F
phase_code = ( clamp(txiq_phase,-31,+31)) & 0x3F
```

---

#### 36CA.6 Format mémoire de calibration

Juste avant l'écriture MMIO, la routine construit également un mot de calibration sauvegardable.

Le packing observé est :

```text
octet bas  = phase_code
octet haut = gain_code
```

soit conceptuellement :

```c
uint16_t txiq_cal_word =
      (uint16_t)phase_code
    | ((uint16_t)gain_code << 8);
```

Ce mot est stocké via un pointeur fourni au chemin de calibration.

---

#### 36CA.7 Chemin de restauration sans recalcul

Un second chemin de `rom_rfcal_txiq()` est pris lorsque l'état d'entrée indique qu'une calibration
préexistante doit être utilisée.

Il lit alors les deux octets du mot sauvegardé :

```text
low byte  → phase_code → <<18
high byte → gain_code  → <<24
```

puis applique exactement le même :

```text
RMW 0x60009860
```

Conclusion :

> `0x60009860` n'est pas seulement un registre de résultat de calibration : il est utilisé
> comme **registre de configuration digitale TXIQ rechargé à partir de coefficients mémorisés**.

---

#### 36CA.8 Relation avec `rom_set_txiq_cal()`

La v0.60 avait montré que :

```text
rom_set_txiq_cal()
```

relit :

```text
0x60009860[28:24]
0x60009860[23:18]
```

comme deux valeurs signées de largeur 5/6 bits, puis ajuste les bits de signe analogiques :

```text
0x77/reg16.bit0
0x77/reg15.bit6
```

La v0.62 ferme donc la boucle :

```text
txiq_cover()
   ↓
gain / phase
   ↓
encodage digital
   ↓
0x60009860
   ↓
rom_set_txiq_cal()
   ↓
synchronisation des signes I²C analogiques
```

---

#### 36CA.9 Pourquoi ce registre est important pour QPSK/QAM

Contrairement à l'I²C interne :

```text
0x60009860
```

est un MMIO direct.

Cela ouvre une nouvelle hypothèse expérimentale :

```text
modifier un coefficient TXIQ
sans relancer toute la calibration
```

Le coût CPU potentiel est donc beaucoup plus faible qu'une paire de transactions I²C.

Mais la v0.62 ne conclut pas encore que cela constitue un modulateur de phase.

---

#### 36CA.10 Limite fondamentale : correction ≠ constellation

Le champ de phase est borné par la calibration à :

```text
[-31,+31]
```

mais aucune correspondance :

```text
1 code = X degrés
```

n'est connue.

Le bloc est conçu pour corriger un **mismatch I/Q**, donc la plage physique peut être seulement
de quelques degrés autour de la quadrature nominale.

Un QPSK nécessite des états séparés de :

```text
90°
```

La question suivante devient donc quantitative :

```text
quelle rotation RF obtient-on en changeant phase_code ?
```

---

#### 36CA.11 Statut TX après v0.62

| Élément | Statut |
|---|---:|
| adresse du registre TXIQ digital | **~99 %** |
| champ gain bits28:24 | **~99 %** |
| champ phase bits23:18 | **~99 %** |
| bits17:16 forcés à 11 | **100 % logiciel** |
| saturation gain ±15 | **100 % logiciel** |
| saturation phase ±31 | **100 % logiciel** |
| encodage gain `(-g)&0x1F` | **100 % logiciel** |
| encodage phase `p&0x3F` | **100 % logiciel** |
| mot sauvegardé `phase | gain<<8` | **~99 %** |
| restauration de coefficients sans recalcul | **~99 %** |
| registre utilisable en hot-update | **plausible, non démontré physiquement** |
| phase_code → degrés | **ouvert** |
| capacité à atteindre plusieurs quadrants | **ouverte** |
| QPSK TX propriétaire | **~65–70 % architectural** |
| QAM TX propriétaire | **~60–65 % architectural** |

---

#### 36CA.12 Prochaine cible TX

Deux tests de recherche restent prioritaires :

```text
A. chercher un writer runtime de 0x60009860 hors calibration
B. chercher une primitive de signe/quadrant I/Q distincte du trim
```

Si aucun n'existe statiquement, le prochain verrou deviendra une caractérisation RF contrôlée du
champ `phase_code`, exactement comme `tone_control→Hz` l'était pour FSK.

---


### 36CB. QPSK/QAM TX — activation et persistance de `0x60009860` — v0.63

La v0.62 a fermé le packing digital :

```text
bits28:24 = gain_code
bits23:18 = phase_code
bits17:16 = 11
```

La question suivante est :

```text
comment cet état est-il activé et conservé dans le PHY ?
```

Le binaire exact apporte deux nouvelles réponses.

---

#### 36CB.1 Ordre exact dans `chip_v6_initialize_bb()`

Les relocations de `phy_chip_v6.o` donnent :

```text
+0x2E88 → ram_rfcal_txiq
+0x2E9B → phy_bb_rx_cfg
```

Le désassemblage autour de cette zone confirme l'ordre :

```text
préparation arguments TXIQ
        ↓
ram_rfcal_txiq(...)
        ↓
branche canal éventuelle
        ↓
phy_bb_rx_cfg()
```

Donc les coefficients TXIQ sont calculés/appliqués **avant** l'étape de configuration BB qui suit.

---

#### 36CB.2 `phy_bb_rx_cfg()` positionne `0x60009860.bit0`

Dans `phy_bb_rx_cfg()` :

```text
+0x28B6  movi.n a13, 1
...
+0x28F5  MEMW
+0x28F8  l32i a2, a14, 608
         a14 = 0x60009600
         → read 0x60009860

+0x28FB  or a2, a2, a13
         → OR 1

+0x28FE  MEMW
+0x2903  s32i a2, a14, 608
         → write 0x60009860
```

Pseudo-code :

```c
REG32(0x60009860) |= 0x00000001u;
```

Ce write arrive après la calibration TXIQ dans la séquence d'initialisation.

---

#### 36CB.3 Interaction avec le packing TXIQ

`rom_rfcal_txiq()` applique :

```c
r  = REG32(0x60009860);
r &= 0xE000FFFFu;
r |= gain_code << 24;
r |= phase_code << 18;
r |= 0x00030000u;
REG32(0x60009860) = r;
```

Le masque :

```text
0xE000FFFF
```

préserve :

```text
bits15:0
```

et donc :

```text
bit0
```

n'est jamais détruit par une mise à jour des coefficients gain/phase.

La structure devient :

```text
0x60009860
    ├── bit0       : état persistant activé par phy_bb_rx_cfg()
    ├── bits17:16  : 11 dans l'état TXIQ programmé
    ├── bits23:18  : phase_code
    └── bits28:24  : gain_code
```

---

#### 36CB.4 Interprétation prudente de bit0

L'ordre :

```text
calibrer / écrire coefficients
        ↓
set bit0
```

est très compatible avec :

```text
enable correction
apply calibration
latch active
```

mais aucun symbole ou branche ne nomme explicitement ce bit.

Le statut correct est donc :

```text
bit0 = enable/latch TXIQ candidate      → très forte inférence
bit0 = "TXIQ_ENABLE" officiel           → non démontré
```

Le document ne transforme pas l'inférence en fait.

---

#### 36CB.5 `register_chipv6_phy()` sauvegarde et restaure `0x60009860`

Un second groupe d'accès apparaît dans :

```text
register_chipv6_phy()
```

La routine lit notamment quatre mots consécutifs/voisins de configuration PHY et utilise
`0x60009860` dans le mécanisme de comparaison avec l'état sauvegardé.

Séquence observée :

```text
read 0x60009854/58/5C-like neighbours
read 0x60009860
read 0x60009864
...
comparer avec backup RTC
...
si restauration nécessaire :
    write état sauvegardé → 0x60009860
    write état sauvegardé → 0x60009864
```

L'accès exact à `0x60009860` apparaît sous forme :

```text
l32i ... offset 608
...
s32i ... offset 608
```

relatif à la même base PHY.

Conclusion :

> **le mot complet `0x60009860` fait partie d'un état de configuration PHY sauvegardable/restaurable.**

Ce comportement serait inhabituel pour un simple registre de résultat éphémère.

---

#### 36CB.6 Conséquence pour un futur modulateur vectoriel

Le bloc TXIQ digital est maintenant mieux décrit comme :

```text
coefficients calibrés
      ↓
0x60009860 gain/phase
      ↓
bit0 d'activation/latch probable
      ↓
configuration persistante
      ↓
restauration après contexte RTC/wakeup
```

Cela renforce la possibilité de détourner ce registre comme actionneur vectoriel.

Mais il manque toujours une preuve fondamentale :

```text
modifier phase_code pendant TX actif
        ↓
rotation RF instantanée et déterministe ?
```

Le fait qu'un registre soit persistant et MMIO ne garantit pas qu'il soit échantillonné
symbole-par-symbole par le modulateur.

---

#### 36CB.7 Statut QPSK/QAM TX v0.63

| Élément | Statut |
|---|---:|
| packing gain/phase `0x60009860` | **~99 %** |
| `bit0` positionné après calibration | **100 % logiciel** |
| conservation de bit0 lors des RMW TXIQ | **100 % logiciel** |
| sauvegarde/restauration du registre | **~99 % structurel** |
| registre = état persistant de configuration PHY | **très haute confiance** |
| bit0 = enable/latch TXIQ | **~85–90 % inférence** |
| write direct MMIO possible par CPU | **100 %** |
| hot-update appliqué immédiatement au modulateur | **ouvert** |
| phase_code → degrés | **ouvert** |
| commande de quadrant ±I/±Q | **toujours ouverte** |
| QPSK TX propriétaire | **~68–72 % architectural** |
| QAM TX propriétaire | **~62–67 % architectural** |

---

#### 36CB.8 Prochaine priorité

Le prochain gain statique doit venir d'un de ces deux résultats :

```text
A. retrouver un writer de 0x60009860 pendant une TX déjà active
B. identifier un champ/mode qui sélectionne explicitement les signes I/Q ou quadrants
```

À défaut, la relation `phase_code → phase RF` deviendra une frontière de caractérisation
matérielle, comme la loi `tone_control → Hz` pour le FSK.

---


### 36CC. QPSK/QAM TX — formules exactes des erreurs gain/phase TXIQ — v0.64

La v0.60/v0.62 avait identifié deux coefficients signés nommés par le flot :

```text
txiq_gain
txiq_phase
```

Il restait une ambiguïté dangereuse pour QPSK :

```text
txiq_phase est-il une phase géométrique exprimée en degrés/codes de rotation,
ou seulement un coefficient de correction de mismatch ?
```

Le corps de `txiq_cover()` permet maintenant de répondre.

---

#### 36CC.1 Mesure de gain : deux puissances indépendantes

Pour :

```text
sel = 1
```

`txiq_get_mis_pwr()` produit deux puissances 16 bits.

Notons :

```text
P0
P1
```

les deux résultats.

La routine construit :

```text
den = min(P0, P1)
if den == 0:
    den = 1

num = (P1 - P0) << 11
q   = num / den
gain = (q + 16) >> 5
```

Donc, à l'arrondi près :

```text
gain ≈ 64 · (P1 - P0) / max(min(P0,P1),1)
```

Une condition de contexte peut ensuite inverser le signe.

Cette forme est typique d'une **erreur relative de gain** : différence de deux puissances
normalisée par une puissance de référence.

---

#### 36CC.2 Mesure de phase : différence normalisée par la somme

Pour :

```text
sel = 0
```

deux autres mesures sont obtenues.

Le code fait :

```text
num = (P0 - P1) << 12

den = P0 + P1
if den == 0:
    den = 1

q = num / den
phase = (q + 16) >> 5
```

Donc :

```text
phase ≈ 128 · (P0 - P1) / max(P0 + P1,1)
```

avec, selon un flag de contexte, une éventuelle inversion finale du signe.

C'est un résultat beaucoup plus fort qu'un simple nom `txiq_phase`.

---

#### 36CC.3 `txiq_phase` n'est pas exprimé en degrés

La formule :

```text
(P0-P1)/(P0+P1)
```

est sans dimension.

Le facteur :

```text
128
```

donne un format d'erreur normalisée de type Q7/échelle 1/128.

Ainsi :

```text
phase_code = 1
```

ne signifie pas :

```text
1°
```

et aucune conversion en degré n'apparaît dans le logiciel.

Conclusion certaine côté logiciel :

> **`txiq_phase` est une coordonnée de correction de mismatch I/Q, pas un angle géométrique.**

---

#### 36CC.4 La saturation ±31 est maintenant interprétable

`rom_rfcal_txiq()` sature ensuite :

```text
txiq_phase ∈ [-31,+31]
```

Cela correspond numériquement à :

```text
|phase_error_coordinate| <= 31/128
                              ≈ 0,2421875
```

dans la coordonnée normalisée produite par `txiq_cover()`.

Cela ne donne pas directement une plage en degrés, car la relation exacte entre ce rapport de
puissances et l'erreur angulaire dépend des vecteurs de test.

Mais cela montre que le bloc applique une **correction locale bornée**, cohérente avec un
compensateur de mismatch et non avec un rotateur complet de constellation.

---

#### 36CC.5 Formule de gain également normalisée

Le gain appliqué est saturé à :

```text
[-15,+15]
```

alors que la métrique calculée est approximativement :

```text
64·ΔP/Pmin
```

Le coefficient gain est donc lui aussi une erreur normalisée, pas une amplitude I ou Q arbitraire.

Conséquence :

```text
gain_code
    ≠ commande indépendante de l'amplitude I
    ≠ commande indépendante de l'amplitude Q
```

au niveau actuellement démontré.

---

#### 36CC.6 Impact direct sur la piste QPSK par `0x60009860`

La piste naïve :

```text
phase_code = K0 → 0°
phase_code = K1 → 90°
phase_code = K2 → 180°
phase_code = K3 → 270°
```

est désormais **fortement déclassée**.

Le registre reste utile pour :

```text
corriger le modulateur I/Q
maintenir une constellation propre
compenser mismatch gain/phase
```

mais pas comme rotateur de quadrant démontré.

---

#### 36CC.7 Les états de stimulus TXIQ deviennent plus intéressants

Le calibrateur de phase ne compare pas un seul tone avec deux petits trims.

Il force deux **états matériels distincts** du `tone_mode` :

```text
0x00B → 0x04B
```

et calcule précisément la différence normalisée de leurs puissances.

Le calibrateur de gain force :

```text
0x10B → 0x20B
```

puis compare les deux puissances.

La structure algorithmique est compatible avec :

```text
gain test  : deux voies orthogonales prises séparément
phase test : deux combinaisons vectorielles de ces voies
```

mais ce mapping physique reste une **forte inférence**, pas encore une preuve.

Pour QPSK, la priorité passe donc de :

```text
modifier phase_code
```

à :

```text
comprendre exactement les vecteurs RF produits par
tone_mode 0x00B / 0x04B / 0x10B / 0x20B
```

---

#### 36CC.8 Hypothèse I±Q — statut précis

En calibration I/Q classique, une comparaison de puissance de deux états combinés peut servir à
estimer l'erreur de quadrature via deux vecteurs de type :

```text
I + Q
I - Q
```

Le rapport :

```text
(Pplus - Pminus)/(Pplus + Pminus)
```

est alors naturellement sensible à l'erreur de phase.

La formule observée dans l'ESP8266 est structurellement compatible avec ce type de méthode.

Cependant le binaire seul ne nomme pas les états :

```text
I+Q
I-Q
```

La v0.64 classe donc :

```text
0x00B / 0x04B = deux stimuli de test de phase I/Q
    → démontré

interprétation exacte I+Q / I-Q
    → hypothèse forte à tester

séparation RF de 90° entre ces deux stimuli
    → non démontrée
```

---

#### 36CC.9 Nouveau classement des pistes TX QPSK

| Piste | Statut |
|---|---:|
| trim phase `0x60009860` comme rotateur 90° | **fortement déclassé** |
| trim gain comme amplitude I/Q indépendante | **fortement déclassé** |
| trims pour corriger une future constellation | **très pertinent / démontré** |
| `tone_mode 0x00B↔0x04B` comme états vectoriels | **très prometteur** |
| `tone_mode 0x10B↔0x20B` comme sélection I/Q | **forte inférence** |
| I+Q / I−Q exact | **ouvert / forte hypothèse** |
| inversion globale 180° | **non identifiée** |
| quatre quadrants QPSK complets | **non fermés** |

---

#### 36CC.10 Statut projet après cette correction

La compréhension du matériel augmente, mais le pourcentage de QPSK TX ne doit pas monter
artificiellement : une piste incorrectement optimiste vient d'être éliminée.

```text
QPSK TX architectural : ~65–70 %
QAM TX architectural  : ~60–65 %
```

La prochaine découverte décisive doit porter sur les **stimuli TXIQ**, pas sur l'amplitude des
petits trims de correction.

---


### 36CD. QPSK/QAM TX — inventaire exhaustif des accès `0x60009860` — v0.65

Cette passe cherche une preuve logicielle directe de :

```text
TX active
    ↓
mise à jour répétée de gain_code / phase_code
    ↓
nouveau vecteur RF
```

Aucun tel utilisateur n'est présent dans le corpus analysé.

---

#### 36CD.1 Objets contenant la base PHY `0x60009600`

Recherche binaire du littéral :

```text
0x60009600
```

dans les archives extraites :

```text
libphy:
    phy_chip_v6_ana.o
    phy_chip_v6_cal.o
    phy_chip_v6.o

libpp:
    aucune occurrence littérale

libnet80211:
    aucune occurrence pertinente
```

La mask-ROM contient la base générale une seule fois dans sa literal pool, partagée par les
routines déjà cartographiées.

---

#### 36CD.2 `phy_chip_v6_cal.o`

Les instructions avec déplacement :

```text
+0x260 = 608
```

relatif à la base `0x60009600` tombent dans :

```text
ram_rfcal_txiq()
```

Séquence :

```text
read  0x60009860
mask / OR gain+phase
write 0x60009860
```

C'est le writer digital de calibration déjà fermé en v0.62.

Aucun second writer vers ce déplacement n'est retrouvé dans cet objet.

---

#### 36CD.3 `phy_chip_v6.o`

Quatre instructions `l32i/s32i` avec déplacement `608` sont retrouvées.

##### Groupe A — `phy_bb_rx_cfg()`

```text
read  0x60009860
OR    1
write 0x60009860
```

Rôle :

```text
activation/latch persistant probable après calibration
```

##### Groupe B — `register_chipv6_phy()`

La fonction :

```text
read  0x60009860
...
compare / backup RTC
...
write état sauvegardé → 0x60009860
```

Rôle :

```text
sauvegarde / restauration de configuration PHY
```

Aucun de ces deux groupes n'est une boucle de modulation TX.

---

#### 36CD.4 `phy_chip_v6_ana.o`

Cet objet contient la base PHY générale mais aucun :

```text
l32i/s32i base+0x260
```

dans sa section de code analysée.

Les chemins RFPLL/analogiques n'ajoutent donc pas de writer caché de `0x60009860`.

---

#### 36CD.5 Mask-ROM

La cartographie ROM exacte déjà effectuée donne :

```text
rom_rfcal_txiq()
    → RMW 0x60009860

rom_set_txiq_cal()
    → lecture des champs TXIQ puis synchronisation de signes I²C
```

Les autres routines ROM utilisant la même base PHY n'apportent aucun writer dynamique
supplémentaire de ce registre.

---

#### 36CD.6 Inventaire fonctionnel final

```text
0x60009860
    │
    ├── WRITE gain/phase
    │      rom_rfcal_txiq
    │      ram_rfcal_txiq
    │
    ├── SET bit0
    │      phy_bb_rx_cfg
    │
    ├── SAVE/RESTORE mot complet
    │      register_chipv6_phy
    │
    └── READ / propagate signs
           rom_set_txiq_cal
```

Absence démontrée dans le corpus :

```text
pas de writer PP
pas de writer net80211
pas de writer LMAC symbole
pas de writer packet TX normal identifié
pas de boucle de modulation basée sur phase_code
```

---

#### 36CD.7 Conséquence QPSK/QAM

La distinction devient nette :

##### Démontré

```text
CPU peut écrire les champs TXIQ
registre persistant
packing connu
coefficients connus
```

##### Non démontré

```text
hardware échantillonne un nouveau coefficient instantanément pendant TX
absence de glitch
continuité de phase
latence write→RF
utilisation à cadence symbole
```

Le simple fait que le registre soit MMIO ne suffit pas à promouvoir le hot-update au rang de fait.

---

#### 36CD.8 Décision de recherche

La voie :

```text
phase_code dans 0x60009860 comme modulateur QPSK rapide
```

atteint maintenant sa **frontière statique dans le corpus standard**.

Deux possibilités restent :

```text
1. firmware usine / ATE ancien contenant un usage dynamique
2. validation silicium contrôlée
```

En parallèle, la voie `tone_mode` reste statiquement attaquable parce qu'elle est déjà modifiée
à chaud par `txiq_get_mis_pwr()` pendant que la TX clock reste active.

C'est donc elle qui devient la priorité QPSK TX n°1.

---

#### 36CD.9 Statut après v0.65

| Piste | Statut |
|---|---:|
| writers `0x60009860` standards inventoriés | **~99 % corpus** |
| hot-update TXIQ coefficient utilisé par SDK | **non retrouvé** |
| possibilité CPU d'écrire le MMIO | **100 %** |
| effet RF d'un hot-update manuel | **validation silicium** |
| `tone_mode` modifié à chaud pendant TXIQ | **100 % logiciel** |
| `tone_mode` comme voie QPSK candidate | **priorité principale** |
| QPSK TX architectural | **~65–70 %** |
| QAM TX architectural | **~60–65 %** |

---


### 36CE. QPSK/QAM TX — modèle vectoriel des stimuli `tone_mode` — v0.66

Cette passe cherche à donner un sens physique cohérent aux quatre états TXIQ déjà démontrés :

```text
phase : 0x00B → 0x04B
gain  : 0x10B → 0x20B
```

Le point nouveau ne vient pas d'un nom symbolique caché, mais du **couplage exact entre chaque
paire d'états et l'équation mathématique que le SDK applique à leurs puissances**.

---

#### 36CE.1 Faits logiciels utilisés

Le corpus exact ferme déjà :

```text
sel = 1
    → tone_mode 0x10B puis 0x20B
    → deux puissances P0/P1
    → correction gain I/Q

sel = 0
    → tone_mode 0x00B puis 0x04B
    → deux puissances P0/P1
    → correction phase/quadrature I/Q
```

Les formules fermées en v0.64 sont :

```text
gain_step  ≈ 64  * (P1-P0) / min(P0,P1)

phase_step ≈ 128 * (P0-P1) / (P0+P1)
```

Ces deux équations ne mesurent pas la même propriété.

---

#### 36CE.2 Interprétation de la paire gain : deux rails séparés

Pour mesurer un déséquilibre de gain entre deux branches quadrature, la mesure la plus directe est :

```text
état G0 → puissance du rail A
état G1 → puissance du rail B
```

puis :

```text
erreur_gain ∝ PB - PA
```

normalisée par une puissance de référence.

C'est exactement la structure du calcul observé :

```text
(P1-P0) / min(P0,P1)
```

Le classement recommandé devient :

```text
tone_mode 0x10B = stimulus rail quadrature A
tone_mode 0x20B = stimulus rail quadrature B
```

avec :

```text
A/B = vraisemblablement I/Q ou Q/I
```

mais **l'ordre I versus Q n'est pas encore démontré**.

Niveau de confiance :

```text
deux rails orthogonaux séparés : très élevé
A=I / B=Q                    : ouvert
A=Q / B=I                    : ouvert
```

---

#### 36CE.3 Interprétation de la paire phase : deux combinaisons croisées

La formule :

```text
(P0-P1)/(P0+P1)
```

est qualitativement différente d'une simple différence de gains.

Pour deux rails sinusoïdaux A et B de même amplitude, les puissances des combinaisons :

```text
A+B
A-B
```

sont :

```text
|A+B|² = |A|² + |B|² + 2 Re(A·B*)
|A-B|² = |A|² + |B|² - 2 Re(A·B*)
```

Donc :

```text
|A+B|² - |A-B|² = 4 Re(A·B*)
```

et :

```text
(|A+B|² - |A-B|²) /
(|A+B|² + |A-B|²)
```

isole directement le terme de corrélation croisée normalisé.

Si A et B sont idéalement en quadrature :

```text
Re(A·B*) = 0
```

les deux puissances deviennent égales.

Une petite erreur de quadrature les déséquilibre.

C'est précisément la propriété nécessaire au calcul :

```text
phase_step ≈ 128*(P0-P1)/(P0+P1)
```

du SDK.

---

#### 36CE.4 Modèle physique promu

Le meilleur modèle devient :

```text
GAIN TEST
    0x10B → rail A seul
    0x20B → rail B seul

PHASE TEST
    0x00B → combinaison A+B
    0x04B → combinaison A-B
```

ou la permutation équivalente :

```text
0x00B ↔ A-B
0x04B ↔ A+B
```

selon la polarité physique et le signe choisi par le chemin de calibration.

Le binaire ne permet pas encore de choisir l'ordre de signe.

Ce qui est désormais très fortement contraint est la **famille de vecteurs**, pas leur étiquette
électrique exacte.

---

#### 36CE.5 Pourquoi ce modèle est plus fort qu'une analogie générique

Le modèle explique simultanément :

```text
1. pourquoi 0x10B/0x20B servent exclusivement à la correction de gain
2. pourquoi leurs puissances sont comparées séparément
3. pourquoi 0x00B/0x04B servent exclusivement à la correction de phase
4. pourquoi le chemin phase utilise une différence normalisée par la somme
5. pourquoi l'erreur phase s'annule lorsque les deux puissances deviennent égales
6. pourquoi le trim final reste petit autour d'un état quadrature nominal
```

Une interprétation de ces quatre états comme simples niveaux d'amplitude arbitraires expliquerait
mal ces six propriétés simultanément.

---

#### 36CE.6 Corroboration externe générale

Les architectures d'émetteurs I/Q conventionnelles corrigent séparément :

```text
gain imbalance
quadrature phase imbalance
```

en mesurant les écarts entre les deux branches I/Q et en appliquant des petites corrections
numériques/analogiques.

La littérature générale sur les modulateurs I/Q décrit précisément le rôle de :

```text
- correction de gain entre I et Q
- correction de phase autour de 90°
- mesure de l'image / terme croisé pour estimer l'erreur de quadrature
```

Cette littérature **corrobore l'architecture**, mais ne constitue pas une preuve du mapping des
bits ESP8266. Le mapping reste fondé sur le corpus binaire exact.

---

#### 36CE.7 Conséquence pour QPSK : nous possédons des vecteurs, pas encore les quatre quadrants

Si le modèle est correct, les états connus correspondent approximativement à :

```text
rail A
rail B
A+B
A-B
```

Ce sont déjà des **stimuli vectoriels**.

Mais un QPSK complet exige quatre états de signe :

```text
+A +B
-A +B
-A -B
+A -B
```

ou une base équivalente tournée.

Le corpus ne démontre pas encore de contrôle qui fournisse explicitement :

```text
-A seul
-B seul
-(A+B)
-(A-B)
```

à cadence symbole.

Donc :

> le `tone_mode` fournit désormais une preuve forte d'accès à la géométrie des deux rails
> quadrature, mais **pas encore à leurs quatre combinaisons de signe**.

---

#### 36CE.8 Bit physique 25 : candidat structurel, aucune promotion

Dans le nibble TXIQ physique `bits27:24`, les états démontrés utilisent :

```text
bit24
bit26
bit27
```

Le bit :

```text
bit25
```

n'apparaît dans aucun des quatre états observés.

Il serait tentant d'en faire un hypothétique :

```text
sign / invert / quadrant
```

mais **aucun usage du corpus ne le démontre**.

Statut :

```text
bit25 = non utilisé dans les stimuli TXIQ observés
rôle = ouvert
```

Il devient une cible de recherche, pas une conclusion.

---

#### 36CE.9 Possibilité d'une constellation partielle

Le corpus démontre déjà des changements à chaud de `tone_mode` dans `txiq_get_mis_pwr()` avec
TX clock maintenue active.

Si les vecteurs physiques correspondent au modèle ci-dessus, le hardware peut donc commuter
au minimum entre plusieurs orientations vectorielles pendant une session tone.

Ce fait est plus fort que la simple existence d'un trim :

```text
TXIQ possède un mini-générateur de vecteurs de test
```

La question devient :

```text
ce mini-générateur expose-t-il les inversions/signatures manquantes
par des combinaisons de bits non utilisées par la calibration ?
```

---

#### 36CE.10 Implication pour QAM

Pour QAM il faut :

```text
signes des deux rails
+
niveaux indépendants des deux rails
```

Les découvertes actuelles donnent :

```text
rails orthogonaux : forte évidence
combinaisons croisées : forte évidence
amplitude globale tone : démontrée
gain I/Q fin : démontré
phase I/Q fin : démontré
signes indépendants : non démontrés
amplitudes I/Q indépendantes rapides : non démontrées
```

Donc le QAM reste derrière le QPSK en difficulté.

---

#### 36CE.11 Nouvelle carte de confiance TX QPSK/QAM

| Élément | Statut |
|---|---:|
| `0x10B/0x20B` = paire utilisée pour gain I/Q | **100 % logiciel** |
| paire gain = deux rails orthogonaux séparés | **~95 % architectural** |
| ordre exact I/Q des deux rails | **ouvert** |
| `0x00B/0x04B` = paire utilisée pour phase I/Q | **100 % logiciel** |
| paire phase = combinaisons croisées somme/différence | **~90–95 % physique/inférentiel** |
| polarité exacte somme versus différence | **ouverte** |
| commutation `tone_mode` à chaud | **100 % logiciel** |
| quatre signes/quadrants QPSK | **non trouvés** |
| bit25 comme signe/quadrant | **hypothèse uniquement** |
| QPSK TX propriétaire | **~65–70 % architectural** |
| QAM TX propriétaire | **~55–60 % architectural** |

---

#### 36CE.12 Prochaine recherche

Les recherches statiques les plus discriminantes deviennent :

```text
1. rechercher tout write qui utilise bit25 physique du tone slot
2. rechercher des valeurs tone_mode autres que :
       0x001
       0x00B
       0x04B
       0x10B
       0x20B
3. chercher dans les firmwares/ROM de test usine des stimuli TXIQ supplémentaires
4. tracer le mapper OFDM natif vers le datapath de modulation
5. côté RX, déterminer si DC_I/DC_Q sont mesurables dans le RX externe normal
   sans loopback et avec N court
```

---


### 36CF. QPSK/QAM RX — séparation `phy_ops` / `phy_func_tab` et statut de `rom_dc_iq_est` — v0.67

La v0.61 avait fermé la primitive :

```text
rom_dc_iq_est(mode,N,out)
    ↓
IQ_EST
    ↓
I_mean, Q_mean
```

La question suivante est :

```text
le PHY v6 l'utilise-t-il déjà dans son chemin RX normal ?
```

Une première recherche automatique a produit un faux positif important. Cette passe le corrige.

---

#### 36CF.1 Le faux positif `phy_enable_agc() +0x10`

Dans `phy.o` :

```text
phy_enable_agc @ 0xB4
```

le désassemblage exact est :

```text
L32R    a0, pointer_slot
L32I.N  a0, a0, 0
L32I.N  a0, a0, 16
CALLX0  a0
```

Pris isolément, ce :

```text
+16 = +0x10
```

semble correspondre à :

```text
phy_func_tab + 0x10 = rom_dc_iq_est
```

Mais cette interprétation est fausse.

---

#### 36CF.2 `register_phy_ops()` installe une table différente

Toujours dans `phy.o` :

```text
register_phy_ops @ .text+0x08
```

fait essentiellement :

```c
local_phy_ops = argument_a2;
```

Le pointeur utilisé ensuite par les wrappers est donc **fourni par l'appelant**.

Il ne provient pas de :

```text
phy_get_romfuncs()
0x3FFFC730
0x3FFFC734
```

Le wrapper `phy_enable_agc()` travaille sur cette table locale enregistrée.

---

#### 36CF.3 Layout reconstruit de la petite table `phy_ops`

Les wrappers de `phy.o` chargent :

```text
rf_init()
    → table +0x00

RFChannelSel()
    → table +0x08

phy_delete_channel()
    → table +0x0C

phy_enable_agc()
    → table +0x10

phy_disable_agc()
    → table +0x14

phy_initialize_bb()
    → table +0x18

phy_set_sense()
    → table +0x1C
```

`bb_init()` utilise lui aussi l'entrée :

```text
+0x18
```

dans son wrapper.

Ce layout prouve fonctionnellement que cette table représente des **opérations PHY de haut niveau**.

Elle est distincte de la table ROM :

```text
phy_func_tab / g_phyFuns
```

qui contient à `+0x10` :

```text
rom_dc_iq_est
```

---

#### 36CF.4 Règle méthodologique nouvelle

À partir de v0.67 :

> **un offset de vtable n'a de sens qu'après identification de l'identité de la table.**

Il est interdit de transformer automatiquement :

```text
indirect_call +0x10
```

en :

```text
rom_dc_iq_est
```

sans avoir prouvé que la base est réellement `g_phyFuns/phy_func_tab`.

Cette correction est importante pour toutes les recherches QPSK/SDR suivantes.

---

#### 36CF.5 Scan des vrais appels `g_phyFuns`

Après séparation des deux tables, les appels indirects via le vrai `g_phyFuns` ont été
réinventoriés dans :

```text
phy_chip_v6.o
phy_chip_v6_cal.o
phy_chip_v6_ana.o
phy_sleep.o
```

Les offsets retrouvés couvrent notamment des primitives connues de :

```text
I2C
PBUS
tone
SAR
RX init
RFPLL
calibration
```

mais aucun appel standard patché v6 n'utilise :

```text
g_phyFuns + 0x10
```

dans les chemins analysés.

Donc :

```text
rom_dc_iq_est existe dans la table ROM
mais
aucun caller v6 standard identifié ne l'utilise
```

---

#### 36CF.6 Ce que cela dit de `rom_dc_iq_est`

##### Démontré

```text
primitive ROM réelle
appelable via la table ROM
contrôle IQ_EST
retourne I_mean/Q_mean signés
N programmable
N=0 accepté par le chemin CPU
```

##### Non démontré

```text
appel automatique pendant réception normale
consommation par WDEV/PP
usage sur chaque symbole Wi-Fi
usage externe QPSK existant dans le SDK
```

Le meilleur classement devient :

```text
instrument vectoriel PHY latent / réutilisable
```

plutôt que :

```text
sortie standard du démodulateur RX
```

---

#### 36CF.7 Conséquence pour QPSK RX

Cette correction ne retire pas l'intérêt de la primitive.

Au contraire, elle précise l'architecture candidate :

```text
RX RF normal
    ↓
datapath baseband actif
    ↓
firmware propriétaire déclenche explicitement IQ_EST
    ↓
rom_dc_iq_est(mode,N,out)
    ↓
(I_mean,Q_mean)
    ↓
classification quadrant
```

Le point matériel restant est maintenant très clair :

> **IQ_EST voit-il un vecteur externe utile dans l'état RX normal lorsque le firmware
> le déclenche explicitement, sans état RXIQ loopback ?**

C'est exactement la validation déjà nécessaire à OOK/ASK pour le chemin externe, mais avec
les deux accumulateurs signés au lieu de la seule énergie E4.

---

#### 36CF.8 Pourquoi l'absence de caller standard est importante

Si `rom_dc_iq_est()` était déjà appelée dans le RX normal, on pourrait rechercher directement :

```text
où vont I_mean/Q_mean ?
```

Ce chemin n'existe pas dans les patchs v6 identifiés.

Il faudra donc construire notre propre consommateur :

```text
trigger
wait DONE
read I/Q
disable/re-arm
classify
```

Cela rapproche l'architecture d'une **SDR assistée par accumulateur vectoriel**, et non d'une
réutilisation d'une sortie de paquet existante.

---

#### 36CF.9 Statut RX QPSK/QAM après correction

| Élément | Statut |
|---|---:|
| existence `rom_dc_iq_est()` | **100 %** |
| ABI `mode,N,out[2]` | **~99 %** |
| I/Q signés normalisés par `N+1` | **~99 %** |
| disponibilité via phy_func_tab | **100 %** |
| `phy_enable_agc()+0x10` = dc_iq_est | **FAUX — corrigé** |
| table `phy_ops` distincte | **100 % logiciel** |
| caller v6 standard de `phy_func_tab+0x10` | **aucun identifié** |
| usage RX externe volontaire | **candidat principal** |
| quadrant externe conservé | **validation silicium** |
| QPSK RX assisté PHY | **~60–65 % architectural** |
| QAM RX assisté PHY | **~50–60 % architectural** |

La légère baisse évite de confondre disponibilité d'une primitive avec utilisation standard.

---

#### 36CF.10 Prochaine cible RX

La prochaine passe doit chercher :

```text
1. emplacement exact d'IQ_EST dans la chaîne par rapport à :
       DC cancellation
       AGC
       channel filter
       dérotation/CFO
2. registres configurant le DC-removal / RX DC calibration
3. comportement de I_mean/Q_mean à N court
4. possibilité de désactiver ou figer un éventuel suppresseur DC
5. corrélateurs 0x60000580..58C comme alternative si DC_I/DC_Q
   annulent un symbole centré
```

---


### 36CG. QPSK/QAM TX — bit25 explicitement annulé dans les stimuli TXIQ — v0.68

La v0.66 avait laissé :

```text
bit25 physique du nibble bits27:24
    → non utilisé
    → rôle ouvert
```

Cette passe ferme plus fortement la question : dans le générateur de stimuli TXIQ du corpus,
bit25 n'est pas simplement absent des constantes observées, il est **explicitement maintenu à zéro**
par la logique de reconstruction du slot.

---

#### 36CG.1 Première écriture TXIQ

Le chemin ROM direct autour de `0x600005B8` commence par préserver uniquement :

```text
old & 0xF0000000
```

Donc :

```text
bits27:0
```

sont reconstruits à neuf.

La logique ajoute ensuite :

```text
tone_control
scale
0x002C0000
```

et le premier état TXIQ :

```text
sel=0 → 0x0 << 24
sel=1 → 0x4 << 24 = bit26
```

Ainsi, dans la première écriture :

```text
bit24 = 0
bit25 = 0
bit26 = sel
bit27 = 0
```

pour les états concernés.

Le bit25 ne peut pas hériter d'un ancien état car `0xF0000000` a supprimé tout `bits27:0`
avant reconstruction.

---

#### 36CG.2 Deuxième écriture TXIQ

Après la première mesure, le code relit le slot puis applique :

```text
old & 0xF0FFFFFF
```

Masque :

```text
1111 0000 1111 1111 1111 1111 1111 1111
     ^^^^
   bits27:24 effacés
```

Puis la valeur de second état est ORée :

```text
sel=0 → 0x1 << 24 = bit24
sel=1 → 0x8 << 24 = bit27
```

Donc :

```text
sel=0:
    bits27:24 = 0001

sel=1:
    bits27:24 = 1000
```

et dans les deux cas :

```text
bit25 = 0
```

par construction.

---

#### 36CG.3 Ensemble exact des états produits

Le mini-générateur TXIQ standard produit donc exactement :

```text
0000
0001
0100
1000
```

sur :

```text
bits27:24
```

soit :

```text
0x0
0x1
0x4
0x8
```

Le code ne produit jamais :

```text
0x2
```

ni aucune valeur contenant :

```text
bit25 = 1
```

dans ces séquences.

---

#### 36CG.4 Recoupement avec l'audit exhaustif des tone slots

La mask-ROM fournie n'a que trois familles réelles d'accès aux slots :

```text
rom_start_tx_tone()
rom_stop_tx_tone()
TXIQ direct
```

Le chemin :

```text
rom_stop_tx_tone()
```

ne fait que clear :

```text
bit18
```

et ne crée aucun état haut.

Les usages normaux démontrés de :

```text
rom_start_tx_tone()
```

programment :

```text
tone_mode = 0x001
```

donc aucun bit25.

Le chemin TXIQ direct vient d'être démontré comme forçant bit25 à zéro.

Conclusion pour le corpus exact :

> **aucun writer standard identifié des tone slots ne produit bit25=1.**

---

#### 36CG.5 Ce que cette conclusion ne signifie pas

Le hardware pourrait techniquement donner un sens à bit25.

La ROM :

```text
rom_start_tx_tone(mode,...)
```

injecte le `mode` fourni sans validation explicite à 10 bits.

Un firmware expérimental pourrait donc écrire un mode non observé.

Mais :

```text
bit25 a un rôle hardware possible
```

n'implique pas :

```text
bit25 = inversion I
bit25 = inversion Q
bit25 = quadrant
```

Aucune de ces significations n'est supportée par le corpus.

---

#### 36CG.6 Conséquence QPSK

Le modèle standard ne contient plus de candidat simple :

```text
bit25 → signe/quadrant
```

Le QPSK complet doit donc être cherché dans :

```text
1. le mapper/modulateur OFDM matériel
2. des états tone_mode non utilisés par le SDK standard
3. la combinaison physique multi-slot
4. un autre registre du datapath I/Q
```

Le mini-générateur TXIQ reste utile parce qu'il démontre les deux rails et leurs combinaisons,
mais il n'expose pas, dans le corpus standard, les quatre signes requis.

---

#### 36CG.7 Statut

| Élément | Statut |
|---|---:|
| états TXIQ `0/1/4/8` | **100 % logiciel** |
| bit25 absent par simple hasard | **non : il est explicitement zéro** |
| première écriture efface l'ancien bit25 | **100 %** |
| deuxième écriture efface l'ancien bit25 | **100 %** |
| bit25 utilisé comme quadrant standard | **écarté** |
| rôle hardware hypothétique de bit25 | **inconnu** |
| QPSK via bit25 standard | **écarté** |

---


### 36CH. SDR/QAM RX — `phy_adc_read_fast()` = SAR/TOUT, pas ADC I/Q RF — v0.69

La recherche d'une interface SDR plus basse que `IQ_EST` a fait apparaître le symbole :

```text
phy_adc_read_fast
```

dans :

```text
phy_chip_v6_ana.o
```

Comme son nom contient `adc_read_fast`, il était indispensable de vérifier s'il pouvait fournir
des échantillons du front-end RF.

La réponse est désormais non.

---

#### 36CH.1 Symbole exact

Le symbole est :

```text
phy_adc_read_fast
offset .irom0.text = 0x16EC
taille             = 627 octets
```

Il se trouve dans le même objet analogique que les fonctions de canal/RF.

Le nom seul était donc insuffisant pour déterminer de quel ADC il s'agissait.

---

#### 36CH.2 ABI reconstruite

Le prologue sauvegarde :

```text
a2 → stack +28
a3 → stack +32
a4 → a14
```

Le corps utilise ensuite :

```text
a2 original → pointeur de sortie
a3 original → nombre d'échantillons
a4 original → paramètre de division/timing
```

La signature reconstruite est donc compatible avec :

```c
void phy_adc_read_fast(uint16_t *adc_addr,
                       uint16_t adc_num,
                       uint8_t adc_clk_div);
```

Cette signature est également celle publiquement associée à cet ancien helper PHY ESP8266.

---

#### 36CH.3 Bloc MMIO utilisé

La fonction charge notamment :

```text
0x60000600
0x60000A00
```

puis travaille sur :

```text
0x60000600 + 0x110 = 0x60000710

0x60000A00 + 0x350 = 0x60000D50
0x60000A00 + 0x354 = 0x60000D54
0x60000A00 + 0x358 = 0x60000D58
0x60000A00 + 0x35C = 0x60000D5C
0x60000A00 + 0x360 = 0x60000D60
0x60000A00 + 0x380 = 0x60000D80
```

La fonction :

```text
- sauvegarde la configuration
- programme le timing / nombre de conversions
- déclenche le bloc
- attend son état
- lit/agrège les résultats
- écrit des valeurs 16 bits dans le buffer
- restaure l'état
```

Le registre :

```text
0x60000D50
```

est publiquement connu dans les implémentations ESP8266 SAR comme registre de configuration/état
du SAR.

---

#### 36CH.4 Recoupement public Espressif

La documentation ESP8266 RTOS SDK décrit :

```text
adc_read_fast()
```

comme :

```text
Measure the input voltage of TOUT(ADC) pin
```

et précise que :

```text
Wi-Fi and interrupts need to be turned off
```

pendant cette mesure rapide.

La structure de configuration associée expose :

```text
clk_div
```

pour l'horloge de collecte ADC.

Cette documentation correspond fonctionnellement au bloc que le binaire v6 manipule.

---

#### 36CH.5 Recoupement historique avec le nom exact

Une documentation historique communautaire du PHY ESP8266 donne explicitement :

```c
void phy_adc_read_fast(uint16 *adc_addr,
                       uint16 adc_num,
                       uint8 adc_clk_div);
```

avec :

```text
adc_addr    = buffer d'échantillons ADC
adc_num     = nombre d'échantillons
adc_clk_div = division de l'horloge de collecte
```

Le nom et l'ABI correspondent au symbole du corpus.

Ce recoupement externe n'est pas nécessaire pour le désassemblage, mais il ferme la nature
du bloc mesuré.

---

#### 36CH.6 Pourquoi ce n'est pas un ADC I/Q RF

Un chemin I/Q RF devrait exposer au minimum deux composantes ou un flux lié au datapath RX :

```text
I[n]
Q[n]
```

ou des buffers associés au baseband RF.

Ici la fonction expose :

```text
un seul flux scalaire 16 bits
```

issu du bloc SAR/TOUT.

Elle est liée aux registres SAR et à la mesure analogique externe/VDD, pas aux registres :

```text
0x600005DC  I accumulator
0x600005E0  Q accumulator
0x60000580..58C corrélations IQ
```

utilisés par `IQ_EST`.

---

#### 36CH.7 Conséquence pour une SDR brute

La piste :

```text
phy_adc_read_fast
    ↓
raw RF ADC samples
    ↓
SDR logicielle
```

est fermée comme :

```text
FAUSSE
```

Le bon classement est :

```text
phy_adc_read_fast
    ↓
SAR ADC / TOUT / VDD analog sampling
```

Ce bloc peut servir à de l'acquisition analogique générale, mais pas comme capture directe du
signal RF Wi-Fi en I/Q.

---

#### 36CH.8 Conséquence pour QPSK/QAM RX

La hiérarchie RX reste donc :

```text
RAW RF I/Q FIFO       → non identifié
        │
        X

IQ_EST vectoriel
    ├── DC_I
    ├── DC_Q
    ├── corrélations complexes
    └── énergie
        ↓
meilleur point d'observation CPU actuellement connu
```

Pour QPSK/QAM propriétaire, l'effort doit rester concentré sur :

```text
IQ_EST
corrélateurs
CFO
état RX/baseband
```

et non sur le SAR ADC.

---

#### 36CH.9 Statut SDR après élimination de cette piste

| Élément | Statut |
|---|---:|
| `phy_adc_read_fast` existe | **100 %** |
| ABI buffer/count/clk_div | **~99 %** |
| utilisation du bloc SAR `0x60000Dxx` | **100 % logiciel** |
| sous-système TOUT/VDD | **~99 % par recoupement** |
| accès aux ADC RF I/Q via cette fonction | **non** |
| flux RF I/Q brut CPU identifié ailleurs | **toujours non** |
| SDR brute type sample-stream | **non supportée par le corpus actuel** |
| SDR assistée via IQ_EST | **reste la piste principale** |

---

#### 36CH.10 Sources publiques de recoupement

- Espressif ESP8266 RTOS SDK — ADC API : `adc_read_fast()` mesure la broche TOUT(ADC).
- Documentation historique ESP8266 du helper `phy_adc_read_fast(buffer,count,clk_div)`.
- Implémentations publiques du SAR ESP8266 utilisant `0x60000D50` comme configuration/état SAR.

---


### 36CI. QPSK/QAM TX — sélection native par `rate`, mapper constellation derrière le hardware — v0.70

La recherche QPSK/QAM doit distinguer deux questions :

```text
1. l'ESP8266 possède-t-il réellement un mapper QPSK/QAM accessible ?
2. peut-on lui fournir des symboles I/Q propriétaires hors 802.11 ?
```

La première réponse est désormais fermée plus bas dans l'interface logiciel/hardware.

---

#### 36CI.1 Structure TX exacte

Le DWARF de :

```text
rate_control.o
```

décrit :

```text
struct esf_tx_desc_s
```

avec :

```text
taille = 32 octets
```

Parmi ses champs :

```text
offset +8 :
    rate : 8 bits
```

Le descriptor contient également :

```text
qid
retry counters
acktime
crypto_type
antenna
status
timestamp
rcSched
```

mais **aucun couple I/Q ni amplitude/phase par symbole**.

---

#### 36CI.2 Codes OFDM retrouvés dans les tables de scheduling

Le fichier :

```text
trc.o
```

contient les tables :

```text
rc11GSchedTbl
rc11NSchedTbl
rcP2P11GSchedTbl
rcP2P11NSchedTbl
BasicOFDMSched
```

Les octets de rate utilisés incluent exactement :

```text
0x08
0x09
0x0A
0x0B
0x0C
0x0D
0x0E
0x0F
```

Ces codes ne sont donc pas des constantes externes théoriques :
ils sont présents dans les tables du corpus exact.

---

#### 36CI.3 Signification officielle Espressif

La documentation Non‑OS ESP8266 donne :

```text
PHY_RATE_48 = 0x08
PHY_RATE_24 = 0x09
PHY_RATE_12 = 0x0A
PHY_RATE_6  = 0x0B
PHY_RATE_54 = 0x0C
PHY_RATE_36 = 0x0D
PHY_RATE_18 = 0x0E
PHY_RATE_9  = 0x0F
```

Ces valeurs correspondent exactement aux octets retrouvés dans `trc.o`.

Le lien :

```text
software rate byte ↔ hardware PHY rate
```

est donc fermé.

---

#### 36CI.4 Mapping rate → modulation OFDM

Pour l'OFDM 802.11a/g :

```text
6  Mb/s → BPSK
9  Mb/s → BPSK

12 Mb/s → QPSK
18 Mb/s → QPSK

24 Mb/s → 16-QAM
36 Mb/s → 16-QAM

48 Mb/s → 64-QAM
54 Mb/s → 64-QAM
```

Ainsi, les codes ESP8266 se regroupent :

```text
0x0B / 0x0F → BPSK

0x0A / 0x0E → QPSK

0x09 / 0x0D → 16-QAM

0x08 / 0x0C → 64-QAM
```

Le second code de chaque paire change principalement le code-rate FEC.

---

#### 36CI.5 Ce que le CPU commande réellement

Le software visible fournit :

```text
payload / descriptor
rate
retries
flags
```

Puis :

```text
rate
  ↓
hardware PHY
  ↓
FEC / interleaving
  ↓
constellation mapper
  ↓
pilotes / OFDM
  ↓
IFFT / datapath I/Q
  ↓
DAC/RF
```

Le choix de modulation existe donc réellement, mais le CPU ne fournit pas directement :

```text
I_symbol
Q_symbol
```

dans ce descriptor.

---

#### 36CI.6 Conséquence pour QPSK propriétaire

Il est possible de demander au PHY standard :

```text
rate = 0x0A ou 0x0E
```

et d'obtenir une transmission QPSK **802.11 OFDM normale**.

Cela ne donne pas encore :

```text
QPSK propriétaire sans framing/FEC/OFDM Wi-Fi
```

Pour cela, il faut trouver un point d'injection après :

```text
descriptor/rate control
```

et avant ou au niveau de :

```text
constellation mapper
```

---

#### 36CI.7 Conséquence pour 16-QAM / 64-QAM

Même conclusion :

```text
rate 0x09/0x0D → mapper 16-QAM natif
rate 0x08/0x0C → mapper 64-QAM natif
```

Le hardware QAM n'est donc plus une hypothèse.

Ce qui reste ouvert est l'accès à ses **coordonnées de constellation** hors pipeline normal.

---

#### 36CI.8 Pourquoi cette frontière est importante

Avant cette passe, deux architectures pouvaient encore être confondues :

```text
A. CPU construit directement des I/Q puis demande émission
B. CPU sélectionne un mode/rate et le PHY construit la constellation
```

Le descriptor exact supporte clairement le modèle :

```text
B
```

au niveau MAC/LMAC connu.

Ainsi :

> **chercher un champ I/Q dans `esf_tx_desc_s` est une fausse piste.**

La recherche doit descendre dans le digital baseband.

---

#### 36CI.9 Relation avec le générateur TXIQ

Nous avons maintenant deux interfaces distinctes :

```text
TXIQ/tone test path
    → stimuli vectoriels de calibration
    → rails / somme-différence
    → instrument de test/calibration

TX normal OFDM path
    → descriptor.rate
    → mapper BPSK/QPSK/QAM hardware
```

Le défi est de trouver :

```text
un pont exploitable vers le mapper normal
```

ou :

```text
un moyen d'étendre le mini-générateur TXIQ aux quatre signes/niveaux
```

---

#### 36CI.10 Statut QAM TX après v0.70

| Élément | Statut |
|---|---:|
| hardware QPSK existe | **100 %** |
| hardware 16-QAM existe | **100 %** |
| hardware 64-QAM existe | **100 %** |
| codes rate ESP8266 0x08..0x0F | **100 %** |
| champ `rate` dans TX descriptor | **100 % DWARF** |
| modulation choisie derrière `rate` | **100 % architecture native** |
| I/Q par symbole dans TX descriptor | **absent** |
| accès propriétaire au mapper | **ouvert** |
| point d'injection pré-mapper | **non identifié** |
| QAM natif Wi-Fi | **OUI** |
| QAM propriétaire hors Wi-Fi | **toujours non démontré** |

---

#### 36CI.11 Prochaine cible TX

La recherche doit maintenant viser les écritures MMIO déclenchées lorsque le code rate change entre :

```text
0x0B → BPSK
0x0A → QPSK
0x09 → 16-QAM
0x08 → 64-QAM
```

Objectif :

```text
identifier le registre hardware qui reçoit
le code modulation/rate
```

puis remonter :

```text
ce registre
    ↓
mapper / encoder / OFDM engine
```

Si ce registre sépare :

```text
modulation
FEC
mode OFDM
```

alors une commande plus basse que le descriptor pourrait devenir accessible.

---


### 36CJ. QPSK/QAM TX — `descriptor.rate` → registre WDEV PHY/PLCP exact — v0.71

La v0.70 avait fermé :

```text
esf_tx_desc_s.rate @ +8
    ↓
codes 0x08..0x0F
    ↓
BPSK / QPSK / 16-QAM / 64-QAM
```

Il manquait le point où cette information quitte les structures CPU et entre dans le modem.

Cette frontière est désormais localisée.

---

#### 36CJ.1 Base WDEV utilisée par `lmacSetTxFrame()`

Le pool de littéraux de `.text.lmacSetTxFrame` contient :

```text
0x3FF20A00
```

La fonction charge cette base dans le registre utilisé pour quatre stores matériels rapprochés.

Les offsets observés sont :

```text
+0x2DC → 0x3FF20CDC
+0x2E0 → 0x3FF20CE0
+0x2E4 → 0x3FF20CE4
+0x2E8 → 0x3FF20CE8
```

Ils appartiennent donc au même bloc WDEV TX.

---

#### 36CJ.2 Chargement exact du rate

Dans le chemin TX :

```text
L8UI a2, a8, 8
```

où :

```text
a8 = pointeur esf_tx_desc_s
```

et le DWARF ferme :

```text
esf_tx_desc_s.rate
    offset +8
    taille 8 bits
```

Le `rate` utilisé pour programmer le hardware est donc bien **le byte du descriptor** décrit en v0.70.

---

#### 36CJ.3 Packing legacy exact dans `0x3FF20CE0`

La séquence reconstruite est :

```text
rate = desc->rate

rate4 = rate & 0x0F
rate4 <<= 12

length12 = frame_length & 0x0FFF
```

Puis ces champs sont combinés dans le mot finalement écrit à :

```text
0x3FF20CE0
```

Pour le chemin legacy :

```text
rate < 16
```

le packing démontré contient au minimum :

```text
bits 11:0   = LENGTH[11:0]
bits 15:12  = RATE[3:0]
```

D'autres champs du même mot proviennent du descriptor et des flags du chemin TX, mais ils sont
orthogonaux au point QAM étudié.

---

#### 36CJ.4 Pourquoi ce packing est typique d'un mot PLCP/PHY

Pour l'OFDM 802.11, le PHY doit recevoir avant émission au minimum :

```text
RATE
LENGTH
```

afin de construire le SIGNAL/PLCP et de configurer le mapper/FEC.

Le mot observé place précisément :

```text
LENGTH 12 bits
RATE    4 bits
```

dans un registre WDEV TX adjacent aux autres mots de préparation PHY.

Le nom neutre recommandé devient donc :

```text
0x3FF20CE0 = TX PHY/PLCP rate-length control word
```

Confiance :

```text
adresse / packing        : 100 % logiciel
fonction rate-length PHY : ~99 % structurel
nom électrique PLCP0     : très probable, non nommé officiellement dans le corpus
```

---

#### 36CJ.5 Mapping QAM direct dans le nibble matériel

Pour les codes legacy fermés en v0.70 :

```text
rate 0x0B / 0x0F → BPSK
rate 0x0A / 0x0E → QPSK
rate 0x09 / 0x0D → 16-QAM
rate 0x08 / 0x0C → 64-QAM
```

le hardware reçoit donc dans :

```text
0x3FF20CE0[15:12]
```

respectivement ces nibbles de sélection PHY.

Exemple conceptuel :

```text
desc.rate = 0x0A
        ↓
0x3FF20CE0[15:12] = 0xA
        ↓
PHY OFDM QPSK
```

Ce n'est plus une simple table de rate-control :
le choix de constellation atteint bien un **champ MMIO matériel concret**.

---

#### 36CJ.6 Chemin HT/MCS

Lorsque :

```text
rate >= 16
```

`lmacSetTxFrame()` traite le descriptor différemment.

Le chemin :

```text
rate >= 16
```

pose notamment un flag :

```text
0x01000000
```

soit :

```text
bit24
```

dans le mot `0x3FF20CE0`.

Puis il construit un second mot écrit à :

```text
0x3FF20CE4
```

dont les bits bas contiennent notamment :

```text
(rate - 16) & 7
```

La séparation devient donc :

```text
legacy OFDM:
    rate nibble dans CE0

HT:
    flag HT dans CE0
    MCS / paramètres HT supplémentaires dans CE4
```

Cette architecture est cohérente avec une séparation PLCP legacy / HT-SIG.

---

#### 36CJ.7 Registres adjacents écrits dans la même préparation

La même fonction écrit également :

```text
0x3FF20CDC
0x3FF20CE8
```

avec des champs de contrôle/longueur/durée provenant du contexte TX.

La v0.71 ne leur attribue pas de noms électriques définitifs, car la découverte utile ici est la
propagation du `rate`.

Le bloc complet se présente cependant comme une petite banque cohérente de **paramètres TX PHY/PLCP**.

---

#### 36CJ.8 Unicité des writes dans `libpp`

Un scan des sections exécutables de tous les objets extraits de `libpp.a` a recherché les stores
32 bits aux offsets :

```text
0x2DC
0x2E0
0x2E4
0x2E8
```

Résultat :

```text
.text.lmacSetTxFrame
    S32I +0x2DC
    S32I +0x2E8
    S32I +0x2E0
    S32I +0x2E4
```

Aucun autre store correspondant n'a été retrouvé dans les autres objets `libpp` analysés.

Donc :

> `lmacSetTxFrame()` est le point CPU canonique de programmation de cette banque TX PHY
> dans le corpus exact.

---

#### 36CJ.9 Corroboration architecturale inter-générations

Dans les ROM publiques de générations Espressif ultérieures apparaissent explicitement les symboles :

```text
mac_tx_set_duration
mac_tx_set_htsig
mac_tx_set_plcp0
mac_tx_set_plcp1
mac_tx_set_plcp2
```

autour de `lmacSetTxFrame`.

Cette observation ne sert **pas** à renommer automatiquement les registres ESP8266.

Elle corrobore seulement l'architecture reconstruite :

```text
lmacSetTxFrame
    ↓
préparation duration / HT-SIG / PLCP words
    ↓
hardware TX PHY
```

---

#### 36CJ.10 Conséquence pour le QAM propriétaire

Nous avons maintenant un actionneur matériel plus bas que le descriptor :

```text
0x3FF20CE0[15:12] = rate / modulation code
```

Mais ce champ choisit seulement :

```text
quelle constellation / quel mode PHY
```

Il ne fournit toujours pas :

```text
I_symbol
Q_symbol
```

ni :

```text
index de point de constellation arbitraire
```

Donc écrire directement :

```text
0xA dans RATE
```

sélectionne QPSK dans le modem OFDM, mais le mapper continue à recevoir ses bits depuis le pipeline
normal FEC/interleaver.

---

#### 36CJ.11 Nouvelle frontière exacte

Le pipeline devient :

```text
esf_tx_desc_s.rate
        ↓
lmacSetTxFrame()
        ↓
0x3FF20CE0[15:12]
        ↓
────────────────────────────────
sélection modulation matérielle
────────────────────────────────
        ↓
FEC / interleaver / constellation mapper
        ↓
I/Q OFDM
        ↓
DAC / RF
```

La prochaine recherche ne doit donc plus chercher :

```text
où rate est écrit ?
```

C'est fermé.

Elle doit chercher :

```text
où les bits codés entrent dans le mapper ?
où le mapper produit/consomme l'index constellation ?
existe-t-il un test-mode contournant FEC/interleaver ?
```

---

#### 36CJ.12 Statut QAM TX après v0.71

| Élément | Statut |
|---|---:|
| `rate` descriptor +8 | **100 %** |
| base WDEV `0x3FF20A00` | **100 %** |
| registre rate/length `0x3FF20CE0` | **100 % logiciel** |
| `RATE` en bits15:12 | **100 % logiciel** |
| `LENGTH` en bits11:0 | **100 % logiciel** |
| flag HT bit24 sur rate>=16 | **100 % logiciel** |
| MCS bas dans `0x3FF20CE4` | **~99 % logiciel** |
| sélection QPSK/16-QAM/64-QAM par MMIO | **100 % architecture native** |
| index constellation arbitraire | **non trouvé** |
| bypass FEC/interleaver | **non trouvé** |
| QAM propriétaire | **toujours ouvert** |

---


### 36CK. QPSK/QAM TX — correspondance exacte avec le champ RATE du SIGNAL OFDM — v0.72

La v0.71 avait fermé le registre :

```text
0x3FF20CE0
```

avec :

```text
bits11:0  = LENGTH
bits15:12 = rate nibble
```

La question restante était :

```text
ce nibble est-il un index Espressif interne,
ou est-il déjà le RATE PLCP OFDM ?
```

Le recoupement bit-à-bit répond maintenant : **c'est le codage RATE OFDM exact**.

---

#### 36CK.1 Table IEEE du champ RATE

Le SIGNAL OFDM 802.11a/g code les huit débits legacy avec les quatre bits :

```text
R1 R2 R3 R4
```

La table standard est :

```text
6  Mb/s → 1101
9  Mb/s → 1111
12 Mb/s → 0101
18 Mb/s → 0111
24 Mb/s → 1001
36 Mb/s → 1011
48 Mb/s → 0001
54 Mb/s → 0011
```

Le champ est transmis dans l'ordre de bits défini par le PHY.

---

#### 36CK.2 Conversion en nibble CPU LSB-first

Dans le registre CPU, les quatre bits occupent :

```text
bits15:12
```

avec :

```text
R1 = bit12
R2 = bit13
R3 = bit14
R4 = bit15
```

La valeur entière du nibble devient donc :

```text
R1 + 2*R2 + 4*R3 + 8*R4
```

Application :

| Débit | R1→R4 | nibble CPU | code ESP8266 |
|---:|:---:|---:|---:|
| 6  | `1101` | `0xB` | `0x0B` |
| 9  | `1111` | `0xF` | `0x0F` |
| 12 | `0101` | `0xA` | `0x0A` |
| 18 | `0111` | `0xE` | `0x0E` |
| 24 | `1001` | `0x9` | `0x09` |
| 36 | `1011` | `0xD` | `0x0D` |
| 48 | `0001` | `0x8` | `0x08` |
| 54 | `0011` | `0xC` | `0x0C` |

La correspondance est parfaite sur les huit valeurs.

---

#### 36CK.3 Conséquence : aucune table de traduction rate legacy n'est requise

Le chemin logiciel observé est simplement :

```text
desc->rate
    ↓
rate & 0x0F
    ↓
<< 12
    ↓
0x3FF20CE0[15:12]
```

et la valeur obtenue est déjà :

```text
PLCP SIGNAL.RATE
```

Le PHY peut donc utiliser directement ces quatre bits pour :

```text
- construire le SIGNAL
- sélectionner la combinaison modulation / code-rate
- configurer le datapath DATA correspondant
```

---

#### 36CK.4 Couplage exact RATE + LENGTH

Le même mot contient :

```text
bits11:0 = LENGTH[11:0]
```

Or le SIGNAL OFDM définit précisément :

```text
RATE   = 4 bits
LENGTH = 12 bits
```

comme paramètres nécessaires au PHY.

Le couple observé dans le registre est donc trop spécifique pour être une simple coïncidence
de scheduling.

Le classement devient :

```text
0x3FF20CE0
    = registre de préparation TX SIGNAL/PLCP legacy
      à très haute confiance
```

Le nom symbolique officiel ESP8266 n'est toujours pas présent dans les headers publics inspectés,
mais la sémantique fonctionnelle est désormais presque entièrement déterminée.

---

#### 36CK.5 Mapping direct modulation/FEC

La table devient :

```text
0xB → BPSK   1/2
0xF → BPSK   3/4

0xA → QPSK   1/2
0xE → QPSK   3/4

0x9 → 16-QAM 1/2
0xD → 16-QAM 3/4

0x8 → 64-QAM 2/3
0xC → 64-QAM 3/4
```

Donc un seul nibble encode simultanément :

```text
famille de constellation
+
coding rate
```

pour les rates legacy OFDM.

---

#### 36CK.6 Ce que cela apporte au projet QAM

Nous avons désormais une primitive matérielle exacte pour sélectionner le mode natif :

```c
legacy_rate_nibble = 0xA; /* QPSK 1/2 */
```

ou :

```c
legacy_rate_nibble = 0x9; /* 16-QAM 1/2 */
```

ou :

```c
legacy_rate_nibble = 0x8; /* 64-QAM 2/3 */
```

dans le contexte normal de préparation TX.

Mais cela ne permet toujours pas de choisir directement :

```text
point QAM n°k
```

Le hardware applique encore derrière :

```text
scrambler
FEC
interleaver
mapper
```

au flux de données.

---

#### 36CK.7 Frontière QAM propriétaire affinée

Avant v0.72 :

```text
rate code
    ↓
hardware inconnu
```

Après v0.72 :

```text
PLCP RATE nibble exact
    ↓
sélection modulation + code-rate
    ↓
FEC/interleaver/mapper hardware
```

La prochaine cible devient donc encore plus précise :

> **trouver si un test-mode permet de fournir au mapper les bits déjà codés/interleavés,
> ou de sélectionner directement un index de constellation.**

---

#### 36CK.8 Corroboration inter-générations

Les ROM Espressif ultérieures exportent autour de `lmacSetTxFrame` les primitives :

```text
mac_tx_set_duration
mac_tx_set_htsig
mac_tx_set_plcp0
mac_tx_set_plcp1
mac_tx_set_plcp2
```

Ce voisinage nominal est cohérent avec la banque de registres reconstruite sur ESP8266.

Il est utilisé uniquement comme corroboration architecturale :
les conclusions v0.72 reposent d'abord sur le binaire ESP8266 exact et la table RATE standard.

---

#### 36CK.9 Statut v0.72

| Élément | Statut |
|---|---:|
| `CE0[15:12]` reçoit `desc.rate & 0xF` | **100 % logiciel** |
| codes 8..F = RATE IEEE bit-à-bit | **100 % correspondance** |
| `CE0[11:0]` = LENGTH | **100 % logiciel** |
| CE0 = préparation SIGNAL/PLCP legacy | **~99 % fonctionnel** |
| sélection modulation + FEC via RATE | **100 % architecture native** |
| table CPU intermédiaire de traduction legacy | **inutile / absente du chemin** |
| point de constellation arbitraire | **non identifié** |
| bypass scrambler/FEC/interleaver | **non identifié** |

---

#### 36CK.10 Sources publiques de recoupement

La table RATE a été recoupée avec :

```text
IEEE 802.11a — SIGNAL field RATE table
```

et des références publiques reproduisant les motifs :

```text
6→1101, 9→1111, 12→0101, 18→0111,
24→1001, 36→1011, 48→0001, 54→0011
```

La conversion en nibble CPU est ensuite purement bit-à-bit et correspond exactement aux codes
présents dans le corpus ESP8266.

---


### 36CL. QPSK/QAM TX — fermeture du mot `0x3FF20CE0` comme PLCP1/PPDU control — v0.73

La v0.71–0.72 avait fermé :

```text
0x3FF20CE0[11:0]  = LENGTH
0x3FF20CE0[15:12] = RATE
```

Cette passe résout les bits supérieurs directement à partir du DWARF et des littéraux du code.

---

#### 36CL.1 `esf_tx_desc_s + 12` = `kid`

Le DWARF exact de `esf_tx_desc_s` décrit le mot à l'offset :

```text
+12
```

avec notamment :

```text
kid         : 8 bits
crypto_type : 4 bits
antenna     : 4 bits
reserved    : 8 bits
status      : 8 bits
```

Compte tenu du layout little-endian généré par le compilateur, le premier octet :

```text
desc + 12
```

correspond à :

```text
kid
```

Dans `lmacSetTxFrame()` :

```text
L8UI a5, a8, 12
SLLI a5, a5, 16
```

donc :

```text
kid → bits23:16
```

du mot finalement écrit à `0x3FF20CE0`.

---

#### 36CL.2 Construction exacte du mot

Le chemin legacy/HT construit :

```text
length12 = data_length & 0x0FFF
rate4    = desc->rate & 0x0F
kid8     = desc->kid
```

puis :

```text
word =
      length12
    | (rate4 << 12)
    | (kid8  << 16)
```

Pour :

```text
rate >= 16
```

le code conserve en plus la constante chargée depuis le pool de littéraux :

```text
0x01000000
```

donc :

```text
word |= BIT(24)
```

avant le store.

---

#### 36CL.3 Littéral HT exact

Le pool de littéraux des 0x50 premiers octets de `.text.lmacSetTxFrame` contient à l'offset :

```text
+0x14 → 0x01000000
```

Le contrôle de flot est :

```text
if (rate < 16)
    ht_flag = 0;
else
    ht_flag = 0x01000000;
```

Le bit :

```text
bit24
```

est donc le marqueur logiciel/hardware du chemin HT dans ce mot pour l'ESP8266 analysé.

Il ne faut pas automatiquement transférer le numéro de bit observé sur d'autres générations :
la sémantique est la même, mais le packing peut varier.

---

#### 36CL.4 Layout final de `0x3FF20CE0`

Pour le chemin analysé :

```text
31                             25 24 23               16 15         12 11             0
┌────────────────────────────────┬──┬───────────────────┬─────────────┬────────────────┐
│             0                  │HT│       KID         │    RATE     │     LENGTH     │
│                                │  │      8 bits       │    4 bit    │     12 bit     │
└────────────────────────────────┴──┴───────────────────┴─────────────┴────────────────┘
```

Soit :

```c
plcp1 =
      (length & 0x0FFF)
    | ((rate & 0x0F) << 12)
    | ((uint32_t)kid << 16)
    | (is_ht ? 0x01000000u : 0);
```

---

#### 36CL.5 Pourquoi `KID` est important pour le classement PLCP1/PPDU

L'index de clé n'appartient pas au champ SIGNAL transmis sur l'air.

Il appartient au **contrôle du TX PPDU** : le hardware doit savoir quelle clé utiliser pendant
le traitement de la trame.

La présence simultanée de :

```text
LENGTH
RATE
KID
HT
```

dans le même mot montre donc que :

```text
0x3FF20CE0
```

est mieux décrit comme :

```text
TX PLCP1 / PPDU control word
```

que comme simple miroir binaire du SIGNAL over-the-air.

Le sous-champ RATE/LENGTH reprend directement les paramètres du PLCP, tandis que KID/HT servent
au séquenceur matériel.

---

#### 36CL.6 Corroboration inter-générations

Le projet ESP32 Open MAC documente sur ESP32 un registre :

```text
PLCP1
```

avec des champs :

```text
LEN
RATE
IS_80211_N
```

et les travaux sur le chiffrement montrent également que l'index de clé crypto est transmis
au hardware dans ce contexte de préparation PLCP/TX.

Ce recoupement ne sert pas à imposer le même offset aux puces.

Il corrobore le rôle architectural identifié indépendamment sur l'ESP8266.

---

#### 36CL.7 Conséquence pour le QAM propriétaire

Ce mot ne transporte pas de point de constellation.

Il dit au PHY :

```text
quel mode/rate utiliser
quelle longueur traiter
quelle clé appliquer
si le PPDU est HT
```

Le mapper reçoit donc ses symboles depuis une étape ultérieure.

La frontière QAM reste :

```text
PLCP1 / PPDU config
        ↓
encoder / interleaver / mapper
        ↓
constellation
```

---

#### 36CL.8 Statut v0.73

| Champ `0x3FF20CE0` | Statut |
|---|---:|
| LENGTH bits11:0 | **100 % logiciel** |
| RATE bits15:12 | **100 % logiciel** |
| KID bits23:16 | **100 % DWARF + flot** |
| HT flag bit24 | **100 % flot + littéral** |
| bits31:25 nuls dans cette construction | **100 % chemin analysé** |
| rôle PLCP1/PPDU control | **~99 % fonctionnel** |
| index constellation direct | **absent** |

---


### 36CM. QPSK/QAM TX — `0x3FF20CE4` = HT-SIG exact sur 32 bits — v0.74

Cette passe ferme le mot écrit uniquement pour :

```text
rate >= 16
```

à :

```text
0x3FF20CE4
```

La correspondance avec le format HT-SIG 802.11n est directe.

---

#### 36CM.1 Construction exacte dans `lmacSetTxFrame()`

Le code calcule :

```text
mcs = (rate - 16) & 7
```

Puis il récupère la longueur sauvegardée plus tôt :

```text
data_length = eb->hdr_len + eb->data_len
```

et effectue :

```text
data_length << 8
```

Le mot commence donc par :

```text
bits2:0   = MCS index 0..7
bits7:3   = 0
bits23:8  = longueur 16 bits
```

---

#### 36CM.2 Pourquoi `data_length` est bien `hdr_len + data_len`

Au début de `lmacSetTxFrame()` :

```text
L16UI eb+20 → hdr_len
L16UI eb+22 → data_len
ADD
EXTUI 16 bits
```

Le résultat est sauvegardé sur la pile puis réutilisé pour CE0/CE4.

Le DWARF de `esf_buf_s` ferme :

```text
+20 hdr_len
+22 data_len
```

Donc le champ écrit dans `CE4[23:8]` est bien la longueur totale PSDU/MPDU préparée par le chemin TX.

---

#### 36CM.3 Codes rate HT Espressif

La famille de codes est :

```text
0x10..0x17 → MCS0..MCS7, Long GI
0x18..0x1F → MCS0..MCS7, Short GI
```

Le code teste :

```text
rate < 24
```

soit :

```text
rate < 0x18
```

pour choisir l'octet haut.

Cela correspond exactement à la frontière :

```text
LGI / SGI
```

de l'enum PHY rate.

---

#### 36CM.4 Octet haut construit par le SDK

Le code initialise :

```text
0x87
```

puis remplace par :

```text
0x07
```

si :

```text
rate < 0x18
```

Il ajoute éventuellement :

```text
0x08
```

selon un bit de `tx_desc.flags`.

Le byte HT-SIG2 initial vaut donc :

```text
LGI, non agrégé : 0x07
LGI, agrégé     : 0x0F

SGI, non agrégé : 0x87
SGI, agrégé     : 0x8F
```

avant décalage de 24 bits.

---

#### 36CM.5 Correspondance bit-à-bit avec HT-SIG1

Le format HT-SIG1 est :

```text
bits 0..6   MCS
bit  7      CBW 20/40
bits 8..23  HT-LENGTH
```

Dans CE4 :

```text
bits0..2    = MCS0..7
bits3..6    = 0
bit7        = 0
bits8..23   = longueur
```

Donc :

```text
MCS[6:0] = valeur 0..7
CBW      = 0
```

ce qui correspond à :

```text
1 spatial stream
20 MHz
MCS0..7
```

pour l'ESP8266.

---

#### 36CM.6 Correspondance bit-à-bit avec le début de HT-SIG2

HT-SIG2 commence après les 24 bits de HT-SIG1.

Ses huit premiers bits sont :

```text
0  Smoothing
1  Not Sounding
2  Reserved
3  Aggregation
4  STBC[0]
5  STBC[1]
6  FEC Coding
7  Short GI
```

Dans le byte produit par le SDK :

```text
bit0 = 1
bit1 = 1
bit2 = 1
bit3 = flag conditionnel
bit4 = 0
bit5 = 0
bit6 = 0
bit7 = SGI
```

soit exactement :

```text
Smoothing    = 1
Not Sounding = 1
Reserved     = 1
Aggregation  = source tx_desc.flags
STBC         = 00
FEC          = BCC / 0
Short GI     = rate >= 0x18
```

La correspondance est complète.

---

#### 36CM.7 Le bit source du descriptor devient `HT-SIG2.Aggregation`

Dans le code :

```text
word0 = tx_desc[0]
flag  = (word0 >> 28) & 1

if (flag)
    high_byte |= 0x08;
```

Puis :

```text
high_byte << 24
```

Donc ce bit devient :

```text
CE4.bit27
```

qui correspond précisément à :

```text
HT-SIG2 bit3 = Aggregation
```

Le nom recommandé pour ce bit source devient donc :

```text
tx_desc aggregate/AMPDU flag
```

à très haute confiance fonctionnelle, même si le sous-champ `flags` du DWARF ne donne pas
de nom individuel à chaque bit.

---

#### 36CM.8 Layout exact de CE4

```text
31 30 29 28 27 26 25 24 23                         8 7 6       3 2      0
┌──┬──┬─────┬──┬──┬──┬──┬───────────────────────────┬─┬─────────┬────────┐
│GI│FEC│STBC │AG│RS│NS│SM│        HT-LENGTH          │0│ MCS[6:3]│MCS[2:0]│
└──┴──┴─────┴──┴──┴──┴──┴───────────────────────────┴─┴─────────┴────────┘

SM = Smoothing        = 1
NS = Not Sounding     = 1
RS = Reserved         = 1
AG = Aggregation flag
STBC                  = 00
FEC                   = 0 / BCC
GI                    = 0 LGI, 1 SGI
CBW bit7              = 0 / 20 MHz
MCS                   = 0..7
```

---

#### 36CM.9 Pseudo-code final

```c
uint32_t htsig_lo32 =
      ((uint32_t)((rate - 0x10) & 7))
    | ((uint32_t)data_length << 8)
    | (0x07u << 24);

if (aggregate)
    htsig_lo32 |= 0x08u << 24;

if (rate >= 0x18)
    htsig_lo32 |= 0x80u << 24;

REG32(0x3FF20CE4) = htsig_lo32;
```

Le pseudo-code reproduit la construction observée pour les champs étudiés.

---

#### 36CM.10 CRC et tail HT-SIG

Le HT-SIG complet fait :

```text
48 bits
```

Les 32 premiers bits sont désormais explicitement fournis par CE4.

Les 16 bits restants contiennent notamment :

```text
extension spatial streams
CRC
tail
```

Dans le chemin `lmacSetTxFrame()` analysé, aucun second mot HT dédié n'est construit à partir de
ces champs.

Comme CRC et tail sont dérivables des premiers bits, le meilleur modèle devient :

> **le moteur PHY génère automatiquement la fin du HT-SIG, notamment CRC/tail,
> à partir du mot de paramètres fourni.**

Confiance :

```text
génération matérielle CRC/tail : forte
emplacement interne exact       : non visible CPU
```

---

#### 36CM.11 Conséquence QAM

Le champ :

```text
MCS
```

sélectionne la modulation/coding HT, mais CE4 reste un mot de **signalisation PHY**.

Il ne transporte pas les symboles de constellation eux-mêmes.

La recherche QAM propriétaire reste donc sous :

```text
HT-SIG / PLCP
        ↓
DATA encoder/interleaver
        ↓
mapper constellation
```

---

#### 36CM.12 Statut v0.74

| Élément CE4 | Statut |
|---|---:|
| MCS bits2:0 | **100 % logiciel** |
| MCS hauts bits3:6 = 0 | **100 % chemin** |
| CBW bit7 = 0 | **100 % chemin / HT20** |
| HT-LENGTH bits23:8 | **100 % logiciel** |
| Smoothing bit24 | **100 % mapping** |
| Not Sounding bit25 | **100 % mapping** |
| Reserved bit26 | **100 % mapping** |
| Aggregation bit27 | **~99 % structurel** |
| STBC bits29:28 = 00 | **100 % chemin** |
| FEC bit30 = 0/BCC | **100 % chemin** |
| Short GI bit31 | **100 % via rate enum** |
| CE4 = HT-SIG[31:0] | **~99–100 %** |
| CRC/tail générés hardware | **forte inférence** |

---


### 36CN. QPSK/QAM TX — fermeture de `CDC` DMA/control et `CE8` Duration/ID — v0.75

Les versions v0.71–v0.74 avaient fermé :

```text
0x3FF20CE0 → PLCP1 / PPDU control
0x3FF20CE4 → HT-SIG[31:0]
```

Il restait deux stores matériels de `lmacSetTxFrame()` :

```text
0x3FF20CDC ← a12
0x3FF20CE8 ← a9
```

Cette passe remonte entièrement la provenance des champs utiles.

---

#### 36CN.1 Les quatre stores exacts de `lmacSetTxFrame()`

Dans la section exacte :

```text
+0x1D7  S32I a12, a0, 0x2DC
+0x1DD  S32I a9,  a0, 0x2E8
+0x200  S32I a13, a0, 0x2E0
+0x23C  S32I a10, a0, 0x2E4
```

avec :

```text
a0 = 0x3FF20A00
```

on obtient :

```text
0x3FF20CDC ← a12
0x3FF20CE8 ← a9
0x3FF20CE0 ← a13
0x3FF20CE4 ← a10
```

Les deux derniers ont déjà été fermés dans les versions précédentes.

---

#### 36CN.2 Provenance de `CDC` : `esf_buf_s.ds_head`

Au début de la fonction, le pointeur de buffer courant est conservé comme :

```text
eb
```

Le DWARF de :

```text
struct esf_buf_s
```

ferme :

```text
offset +4  → ds_head
type       → lldesc_t *
offset +16 → buf_begin
offset +20 → hdr_len
offset +22 → data_len
offset +36 → desc
```

Dans le flot :

```text
L32I.N a0, a0, 4
```

charge donc :

```text
a0 = eb->ds_head
```

Puis :

```text
L32R a6, 0x0003FFFF
AND  a6, a0, a6
```

donne exactement :

```text
dma_low18 = ((uintptr_t)eb->ds_head) & 0x0003FFFF
```

---

#### 36CN.3 Le type `lldesc_t` est explicitement un descripteur DMA

Le DWARF décrit :

```text
struct lldesc_s
taille = 12 octets
```

avec les champs :

```text
size    : 12 bits
length  : 12 bits
offset  : 5 bits
sosf    : 1 bit
eof     : 1 bit
owner   : 1 bit
buf     : pointeur
next/qe : pointeur/queue link
```

Ce layout est celui d'un **linked-list descriptor DMA**.

Le champ bas de `0x3FF20CDC` reçoit donc bien une information de provenance de la chaîne DMA TX.

---

#### 36CN.4 Construction des bits supérieurs de `CDC`

Le mot n'est cependant pas un pointeur brut.

Juste avant le store, le code combine :

```text
dma_low18
```

avec plusieurs champs :

```text
bit22       → conditionnel
bits26:24   → valeur 1 ou 2 selon l'état TX
bit27       → conditionnel
bit28       → conditionnel
```

via une série de :

```text
OR
```

avec les littéraux exacts :

```text
0x00400000
0x08000000
0x10000000
```

et un champ 3 bits décalé de 24.

Le meilleur nom de travail est donc :

```text
0x3FF20CDC = TX DMA descriptor / control word
```

et non :

```text
raw DMA pointer
```

---

#### 36CN.5 Pourquoi cela ressemble fortement à PLCP0/DMA control

Sur des MAC Espressif ultérieurs reverse-engineerés publiquement, le premier mot de préparation TX
nommé `PLCP0` contient précisément les bits bas de l'adresse du `dma_item`.

Ce recoupement est architectural, pas une identité de layout entre puces.

Pour l'ESP8266 exact, nous démontrons indépendamment :

```text
CDC low18 = bits bas de l'adresse lldesc_t*
CDC upper = TX control flags
```

Le classement :

```text
PLCP0-like DMA/control
```

est donc très fortement supporté.

---

#### 36CN.6 Provenance exacte de `CE8`

Le même `eb` fournit :

```text
eb->buf_begin
```

à l'offset :

```text
+16
```

Le code fait ensuite :

```text
L16UI a9, a6, 2
SLLI  a9, a9, 16
...
S32I  a9, WDEV, 0x2E8
```

où :

```text
a6 = eb->buf_begin
```

Donc :

```c
uint16_t v = *(uint16_t *)(eb->buf_begin + 2);
REG32(0x3FF20CE8) = ((uint32_t)v) << 16;
```

---

#### 36CN.7 `buf_begin + 2` = `ieee80211_frame.i_dur`

Le DWARF contient :

```text
struct ieee80211_frame
taille = 24
```

avec :

```text
offset +0 → i_fc[2]
offset +2 → i_dur[2]
offset +4 → i_addr1[6]
...
```

Par conséquent :

```text
*(uint16_t *)(eb->buf_begin + 2)
```

est exactement :

```text
Duration/ID
```

du header 802.11.

Le layout de `CE8` devient :

```text
31                           16 15                         0
┌──────────────────────────────┬────────────────────────────┐
│        Duration / ID         │             0              │
│           16 bits            │          16 bits           │
└──────────────────────────────┴────────────────────────────┘
```

soit :

```text
CE8[31:16] = ieee80211 Duration/ID
CE8[15:0]  = 0
```

dans ce chemin.

---

#### 36CN.8 Corroboration avec les MAC Espressif ultérieurs

Les ROM Espressif ultérieures exportent autour de `lmacSetTxFrame()` :

```text
mac_tx_set_duration
mac_tx_set_htsig
mac_tx_set_plcp0
mac_tx_set_plcp1
mac_tx_set_plcp2
```

et le projet ESP32 Open MAC documente séparément :

```text
PLCP0    → DMA_ADDR
PLCP1    → LEN/RATE/HT
PLCP2    → autre contrôle
DURATION → Duration
```

L'ESP8266 n'a pas forcément les mêmes offsets ni les mêmes largeurs de champs.

Mais la structure générale recoupe remarquablement la banque que nous venons de fermer.

---

#### 36CN.9 Carte fonctionnelle finale de la banque

Le meilleur modèle devient :

```text
0x3FF20CDC
    TX DMA descriptor / control
    low18 = eb->ds_head & 0x3FFFF
    upper = flags TX

0x3FF20CE0
    PLCP1 / PPDU control
    LENGTH | RATE | KID | HT

0x3FF20CE4
    HT-SIG[31:0]
    MCS | HT-LENGTH | AGG | GI | ...

0x3FF20CE8
    Duration/ID control
    Duration/ID en bits31:16
```

C'est la première carte cohérente de cette banque TX WDEV obtenue entièrement depuis le corpus exact.

---

#### 36CN.10 Conséquence décisive pour le QAM propriétaire

Cette banque n'est **pas** un port de constellation.

Elle transporte :

```text
source DMA
configuration PPDU/PLCP
configuration HT
Duration/ID
```

Le flux DATA est donc fourni comme des octets via les descripteurs DMA, puis traité par le PHY :

```text
DMA bytes
   ↓
scrambler
   ↓
FEC
   ↓
interleaver
   ↓
mapper BPSK/QPSK/QAM
   ↓
OFDM/IQ
```

Aucun des quatre mots :

```text
CDC
CE0
CE4
CE8
```

ne fournit :

```text
I
Q
constellation index
coded-bit direct input
```

---

#### 36CN.11 Nouvelle frontière de recherche

La recherche d'un QAM propriétaire doit désormais quitter cette banque.

Les candidats deviennent :

```text
1. registre/test-mode digital baseband
2. bypass scrambler
3. bypass FEC
4. bypass interleaver
5. injection de coded bits
6. mapper-test / constellation-test
7. chemins usine/ATE non utilisés par le SDK standard
```

Une autre possibilité est qu'aucun de ces bypass ne soit exposé au CPU.

Dans ce cas, le QAM propriétaire devra soit :

```text
- détourner le pipeline 802.11 lui-même
```

soit :

```text
- revenir au chemin TXIQ/tone pour synthèse vectorielle
```

---

#### 36CN.12 Statut v0.75

| Élément | Statut |
|---|---:|
| `CDC` low18 = `ds_head` | **100 % DWARF + flot** |
| `ds_head` type `lldesc_t *` | **100 % DWARF** |
| `CDC` = DMA/control word | **~99 % fonctionnel** |
| `CE8[31:16]` = Duration/ID | **100 % DWARF + flot** |
| `CE8[15:0]` = 0 dans ce chemin | **100 % logiciel** |
| banque CDC/CE0/CE4/CE8 fermée fonctionnellement | **~99 %** |
| point de constellation dans cette banque | **non** |
| source DATA = linked DMA descriptors | **100 % architecture logicielle** |
| bypass mapper via cette banque | **non identifié / pas exposé par les champs fermés** |

---


### 36CO. QPSK/QAM TX — audit `tx_cont_*` : mode continu, aucun bypass mapper — v0.76

Après fermeture de la banque :

```text
DMA/control → PLCP1 → HT-SIG → Duration
```

la piste naturelle suivante était le mode :

```text
tx_cont_en
tx_cont_dis
tx_cont_cfg
```

car les modes de test RF sont parfois capables de court-circuiter une partie du modem.

Dans le corpus ESP8266 analysé, ce n'est pas le cas au niveau logiciel observable.

---

#### 36CO.1 `tx_cont_cfg()` ne prend qu'un sélecteur

Le symbole exact est :

```text
tx_cont_cfg
offset  = 0x2D70
taille  = 24 octets
```

Le flot est essentiellement :

```c
void tx_cont_cfg(int enable)
{
    if (enable == 1)
        tx_cont_en();
    else
        tx_cont_dis();
}
```

Aucun autre argument n'est transmis.

Il n'existe donc aucune entrée :

```text
rate
payload
coded bits
constellation index
I
Q
```

dans ce wrapper.

---

#### 36CO.2 `tx_cont_en()` sauvegarde un état de test PHY

Le symbole exact est :

```text
tx_cont_en
offset = 0x2C74
taille = 158 octets
```

Avant de modifier le mode, la fonction sauvegarde les valeurs courantes des registres :

```text
0x60000594
0x60000598
0x6000059C
```

dans des globals de sauvegarde du PHY.

Ces registres sont déjà classés dans le bloc :

```text
PBUS / PHY test / continuous-TX
```

et sont distincts des tone slots :

```text
0x600005B8
0x600005BC
0x600005C4
```

ainsi que de la banque TX WDEV :

```text
0x3FF20CDC
0x3FF20CE0
0x3FF20CE4
0x3FF20CE8
```

---

#### 36CO.3 Transformations exactes du mode continu

Après la préparation RF/PBUS, le code applique :

```c
REG32(0x6000059C) |= 0x0FE03F80u;
REG32(0x60000598) |= 0x0FFFFFFFu;
REG32(0x60000594) &= 0xFFCFFFFFu;
```

La dernière opération efface donc :

```text
0x00300000
```

soit :

```text
bits20 et 21
```

de `0x60000594`.

Les accès sont sérialisés par :

```text
MEMW
```

comme les autres MMIO PHY critiques.

---

#### 36CO.4 `tx_cont_dis()` est la restauration inverse

`tx_cont_dis()` :

```text
offset = 0x2D20
taille = 79 octets
```

restaure les trois valeurs précédemment sauvegardées dans :

```text
0x60000594
0x60000598
0x6000059C
```

puis remet à zéro le flag logiciel d'état continuous-TX.

Cela confirme que le mécanisme est un **overlay de configuration de test** autour d'un état
PHY existant.

Il ne remplace pas le pipeline DATA par un second moteur de symboles programmable.

---

#### 36CO.5 Aucun accès au chemin DMA/PLCP fermé en v0.75

Dans les trois fonctions :

```text
tx_cont_en()
tx_cont_dis()
tx_cont_cfg()
```

aucun accès n'est effectué à :

```text
0x3FF20CDC  DMA/control
0x3FF20CE0  PLCP1/PPDU
0x3FF20CE4  HT-SIG
0x3FF20CE8  Duration/ID
```

Aucun pointeur :

```text
lldesc_t *
```

n'est reçu ou reconstruit.

Aucun `rate` n'est reçu.

Donc le mode continu n'offre pas une seconde source CPU de symboles QAM.

---

#### 36CO.6 Recoupement avec la documentation usine Espressif

La documentation Factory Test ESP8266 expose trois commandes séparées :

```text
tx_contin_en <0|1>
esp_tx <channel> <rate> <attenuation>
wifiscwout <enable> <channel> <attenuation>
```

Elle décrit :

```text
tx_contin_en 1
    → émission de paquets en continu
      avec environ 92 % de duty cycle

tx_contin_en 0
    → mode de test utilisé avec iqview

esp_tx
    → choix séparé du canal, du rate et de l'atténuation

wifiscwout
    → single-carrier
```

Ce découpage public correspond exactement à l'architecture du corpus :

```text
continuous mode   ≠ rate selection
continuous mode   ≠ single-carrier tone
```

La documentation publique ne prouve pas que sa commande appelle directement notre symbole
`tx_cont_cfg`, mais elle corrobore fortement la même séparation fonctionnelle.

---

#### 36CO.7 Conséquence QAM

La piste :

```text
tx_cont mode
    ↓
bypass encoder/interleaver
    ↓
mapper direct
```

n'est pas supportée.

Le mode continu peut modifier :

```text
timing / gating / test state / duty
```

mais la modulation des paquets continue d'être choisie par le chemin :

```text
rate → PLCP/HT-SIG → PHY
```

et les données continuent de provenir de la chaîne DMA standard.

---

#### 36CO.8 Ce que `iqview` ne permet pas de conclure

Le terme public :

```text
iqview test mode
```

est un mode destiné aux instruments RF.

Il ne signifie pas :

```text
CPU access to raw I/Q
```

et ne prouve aucune interface :

```text
I[n], Q[n]
```

ou :

```text
constellation index
```

accessible au LX106.

Aucun tel flux n'est visible dans le code `tx_cont_*`.

---

#### 36CO.9 État des candidats de bypass après v0.76

| Candidat | Résultat |
|---|---|
| banque CDC/CE0/CE4/CE8 | paramètres DMA/PPDU, **pas mapper direct** |
| `tx_cont_*` | mode continu/test, **pas injection symbole** |
| tone TXIQ | stimuli vectoriels, signes complets non exposés |
| single-carrier | tone/test RF, pas QAM |
| mapper OFDM natif | existe, entrée propriétaire toujours cachée |
| coded-bit bypass | **non identifié** |
| FEC/interleaver bypass | **non identifié** |

---

#### 36CO.10 Frontière restante

La recherche doit maintenant viser des fonctions ou registres qui se situent réellement entre :

```text
DMA payload bytes
        ↓
scrambler
        ↓
convolutional encoder / puncturing
        ↓
interleaver
        ↓
QAM mapper
```

et non les modes qui ne font que changer :

```text
duty
RF continuous state
tone
PLCP metadata
```

---

#### 36CO.11 Sources publiques de recoupement

- Espressif, **ESP8266 RTOS SDK — Factory Test** :
  description de `tx_contin_en`, `esp_tx`, `wifiscwout`.
- Espressif, **ESP8266 Wi-Fi Non-Signaling Test** :
  séparation `TX packet`, `TX continues`, `TX tone`.

---


### 36CP. QPSK/QAM TX — raw `freedom` et fixed-rate convergent vers le pipeline standard — v0.77

Après l'audit de :

```text
PLCP/PPDU
HT-SIG
DMA/control
continuous-TX
```

il restait une voie publique particulièrement importante à fermer :

```text
wifi_send_pkt_freedom()
```

Cette API autorise l'envoi de trames 802.11 construites par l'utilisateur.

La question était :

```text
"raw packet" signifie-t-il "raw PHY" ?
```

Pour le corpus analysé, la réponse est clairement **non**.

---

#### 36CP.1 ABI exacte de `ieee80211_freedom_output()`

Le DWARF de `ieee80211_output.o` décrit :

```text
ieee80211_freedom_output
    conn
    outbuf
    buflen
    sys_seq
```

Les quatre paramètres sont :

```text
connexion/contexte
buffer octets
longueur
gestion sequence-number
```

Il n'existe aucun paramètre :

```text
rate
MCS
coded bits
interleaver state
constellation index
I
Q
```

dans l'ABI de cette fonction.

---

#### 36CP.2 Appel direct à `ppTxPkt()`

Les relocations exactes de :

```text
.rela.text.ieee80211_freedom_output
```

contiennent :

```text
+0x219 → ppTxPkt
```

Le même chemin contient auparavant :

```text
ieee80211_getmgtframe
ets_memcpy
```

ce qui correspond à la création d'un `esf_buf_s`, la copie de la trame utilisateur, puis son
injection dans le moteur PP normal.

La primitive freedom ne passe donc pas directement au digital baseband.

---

#### 36CP.3 ABI exacte de `ppTxPkt()`

Le DWARF de `pp.o` ferme :

```text
ppTxPkt(eb)
```

avec un seul argument :

```text
eb : esf_buf_s *
```

Il ne reçoit pas :

```text
raw constellation
QAM point
coded-bit stream
I/Q buffer
```

Le payload est représenté sous la même forme `esf_buf_s` que les transmissions normales.

---

#### 36CP.4 `ppTxPkt()` réutilise le rate-control

Dans la plage exacte de `ppTxPkt()` :

```text
0x8EC .. 0xA61
```

les relocations démontrent un appel à :

```text
rcGetSched()
```

à l'offset fonctionnel :

```text
+0x5A environ / .irom0.text 0x946
```

puis le buffer est mappé/enfilé vers les queues PP.

Le choix du rate n'est donc pas remplacé par une propriété "raw".

---

#### 36CP.5 `ppProcessTxQ()` appelle `lmacTxFrame()`

La relocation de :

```text
.text.ppProcessTxQ
```

démontre :

```text
+0x83 → lmacTxFrame
```

et son DWARF ferme son argument principal comme le buffer TX sélectionné.

La chaîne complète devient :

```text
ieee80211_freedom_output
        ↓
ppTxPkt
        ↓
rcGetSched
        ↓
PP TX queue
        ↓
ppProcessTxQ
        ↓
lmacTxFrame
        ↓
lmacSetTxFrame
        ↓
CDC / CE0 / CE4 / CE8
        ↓
PHY standard
```

---

#### 36CP.6 Recoupement avec l'API publique `wifi_send_pkt_freedom`

La documentation Espressif précise que :

```text
wifi_send_pkt_freedom()
```

envoie une :

```text
user-defined 802.11 packet
```

sans FCS fourni par l'utilisateur.

Elle précise également que le rate d'émission reste celui du mécanisme système/management
dans cette API.

Cela correspond exactement au flot binaire :

```text
octets de trame utilisateur
    ↓
rate-control normal
    ↓
TX pipeline standard
```

---

#### 36CP.7 Fixed rate : contrôle séparé de la constellation native

Le SDK expose :

```c
wifi_set_user_fixed_rate(enable_mask, rate)
```

avec les codes :

```text
0x08  48 Mb/s
0x09  24 Mb/s
0x0A  12 Mb/s
0x0B   6 Mb/s
0x0C  54 Mb/s
0x0D  36 Mb/s
0x0E  18 Mb/s
0x0F   9 Mb/s
```

La v0.72 a fermé leur mapping :

```text
0x0B/0x0F → BPSK
0x0A/0x0E → QPSK
0x09/0x0D → 16-QAM
0x08/0x0C → 64-QAM
```

Donc le SDK permet réellement de sélectionner/fixer une **constellation native** en choisissant
un rate compatible.

---

#### 36CP.8 Corroboration interne du rate-control

Le corpus exact contient :

```text
set_rate_limit()
set_max_fixed_rate()
clean_rate_set()
rc_set_rate_limit_id()
```

et les globals :

```text
max_11b_rate
max_11g_rate
max_11n_rate
```

Les relocations de `set_rate_limit()` démontrent de multiples appels à :

```text
rc_set_rate_limit_id()
```

La notion de limitation/fixation de rate est donc bien implémentée dans le moteur
de rate-control interne, pas comme un mode PHY parallèle.

---

#### 36CP.9 Séparation conceptuelle importante

L'ESP8266 offre deux leviers distincts :

```text
A. choisir les octets de la trame
   → freedom/raw 802.11

B. choisir/fixer le rate
   → QPSK / 16-QAM / 64-QAM natifs
```

Mais le hardware combine ensuite ces deux informations dans le même pipeline :

```text
payload bytes
    +
rate
    ↓
DMA / PLCP
    ↓
scrambler
    ↓
FEC
    ↓
interleaver
    ↓
mapper natif
```

Il n'existe pas de troisième paramètre public ou statique identifié :

```text
"voici mes coded bits"
```

ou :

```text
"voici mon point I/Q"
```

---

#### 36CP.10 Ce que le terme "raw" signifie ici

Le meilleur vocabulaire devient :

```text
raw MAC frame injection
```

et non :

```text
raw PHY symbol injection
```

L'utilisateur choisit la trame 802.11, mais le PHY garde la responsabilité de :

```text
PLCP
scrambling
FEC
interleaving
constellation mapping
OFDM
```

---

#### 36CP.11 Conséquence pratique pour le projet QAM

Pour transmettre avec le QAM natif :

```text
OUI :
    sélectionner rate QPSK/16-QAM/64-QAM
    fournir une trame 802.11
    laisser le PHY mapper les bits
```

Pour transmettre un QAM propriétaire où le logiciel choisit chaque point :

```text
PAS AVEC :
    freedom_output
    fixed_rate
    tx_cont
    PLCP registers
```

Il faut toujours trouver :

```text
coded-bit bypass
mapper test-mode
constellation-index injection
ou synthèse vectorielle TXIQ alternative
```

---

#### 36CP.12 Statut v0.77

| Élément | Statut |
|---|---:|
| freedom ABI sans rate/IQ | **100 % DWARF** |
| freedom → `ppTxPkt` | **100 % relocation** |
| `ppTxPkt` → `rcGetSched` | **100 % relocation** |
| TX queue → `lmacTxFrame` | **100 % relocation** |
| raw frame utilise pipeline standard | **~99 % architecture** |
| fixed-rate API native | **officiel Espressif** |
| fixed-rate = contrôle du rate-control | **très haute confiance** |
| QAM natif fixe sélectionnable | **OUI** |
| raw PHY / coded-bit injection via freedom | **NON** |
| constellation-index via fixed-rate | **NON** |

---


### 36CQ. QPSK/QAM TX — audit exhaustif des APIs nommées du mapper — v0.78

Après fermeture des chemins :

```text
raw frame
fixed rate
PLCP/HT-SIG
DMA
continuous-TX
tone/TXIQ
```

il restait à vérifier si une API plus basse était simplement présente dans le corpus mais n'avait
pas encore été repérée par son nom.

Cette passe réalise un audit symbolique exhaustif.

---

#### 36CQ.1 Périmètre audité

Tous les objets extraits ont été parcourus :

```text
libphy/*.o
libpp/*.o
libnet80211/*.o
```

Les tables de symboles ELF ont été recherchées pour les familles :

```text
scram*
interleav*
fec*
mapper*
qam*
qpsk*
punct*
convolution*
encoder*
coded-bit*
constell*
```

Résultat :

```text
libphy : 0 hit pertinent
libpp  : 0 hit pertinent
libnet : 0 hit pertinent
```

Cette absence porte sur les **noms de symboles** présents dans le corpus exact.

---

#### 36CQ.2 Ce que le corpus nomme effectivement autour du TX PHY

À l'inverse, le corpus expose explicitement des fonctions liées à :

```text
rate control
lmac
PLCP/HT preparation
tone generator
TXIQ calibration
power control
RFPLL
continuous TX
PBUS
MAC enable/disable
```

Cela montre que les symboles de debug et les exports ne sont pas totalement stripés :
plusieurs couches PHY importantes conservent bien des noms fonctionnels.

L'absence totale des mots liés à encoder/interleaver/mapper est donc significative.

---

#### 36CQ.3 Audit du linker ROM officiel

Le linker ROM officiel Espressif :

```text
eagle.rom.addr.v6.ld
```

exporte notamment :

```text
phy_get_romfuncs
rom_start_tx_tone
rom_stop_tx_tone
rom_txtone_linear_pwr
rom_rfcal_txiq
rom_rfcal_txiq_cover
rom_rfcal_txiq_set_reg
rom_set_txiq_cal
rom_tx_mac_disable
rom_tx_mac_enable
rom_set_txclk_en
rom_set_txbb_atten
rom_write_rfpll_sdm
...
```

mais aucune primitive nommée :

```text
scrambler
interleaver
FEC encoder
puncturer
QAM mapper
constellation mapper
coded-bit input
```

Le test lexical sur le fichier officiel ne retrouve aucun :

```text
scram
interleav
fec
mapper
```

---

#### 36CQ.4 Conséquence : le mapper est derrière une frontière hardware non exportée

Le pipeline connu est :

```text
CPU
 ↓
DMA / PPDU metadata
 ↓
MAC/PHY hardware
 ↓
scrambler / FEC / interleaver / mapper
 ↓
OFDM/IQ
```

Les étapes internes :

```text
scrambler
encoder
puncturing
interleaving
constellation mapping
```

ne possèdent donc, dans la surface actuellement connue, aucune API CPU exportée individuellement.

Le hardware peut évidemment les implémenter, puisque le Wi-Fi fonctionne.

La conclusion porte uniquement sur leur **accessibilité logicielle nommée**.

---

#### 36CQ.5 Audit de la surface factory-test publique

La documentation Espressif de factory-test expose :

```text
rftest_init

tx_contin_en(mode)

esp_tx(channel, rate, attenuation)

esp_rx(channel, rate)

wifiscwout(enable, channel, attenuation)

cmdstop
```

Pour `esp_tx`, les seuls paramètres de modulation sont indirectement portés par :

```text
rate
```

Il n'existe aucun paramètre public :

```text
scrambler enable
FEC enable/bypass
interleaver bypass
coded bits
symbol index
I/Q
constellation point
```

---

#### 36CQ.6 `esp_tx` reste une transmission de paquets Wi-Fi

La documentation décrit explicitement :

```text
esp_tx → start transmitting Wi-Fi packets
```

avec :

```text
channel
rate
power attenuation
```

La séparation avec :

```text
wifiscwout → single carrier
```

et :

```text
tx_contin_en → test/continuous mode
```

confirme une nouvelle fois que la factory-test publique n'expose pas de mapper arbitraire.

---

#### 36CQ.7 Ce que cette fermeture exclut

On peut désormais écarter comme piste prioritaire :

```text
"il existe probablement déjà une fonction SDK appelée
  set_qam_symbol() / mapper_bypass() / disable_fec()"
```

Rien de tel n'apparaît dans :

```text
objets SDK exacts
ROM exports officiels
surface factory-test publique
```

---

#### 36CQ.8 Ce que cette fermeture N'exclut PAS

Trois possibilités restent ouvertes :

##### A. Registre MMIO anonyme

Le mapper peut avoir des bits de test dans un registre non nommé :

```text
CPU → MMIO caché → mapper/test path
```

##### B. Fonction non exportée dans une bibliothèque usine différente

Une bibliothèque ATE/factory binaire séparée peut contenir des fonctions absentes du corpus fourni.

##### C. Aucun bypass CPU

Le mapper peut être entièrement câblé derrière le séquenceur PHY, sans entrée arbitraire accessible.

---

#### 36CQ.9 Impact sur la stratégie

Le travail statique doit désormais cesser de chercher des **noms évidents**.

La stratégie utile devient :

```text
1. inventorier les MMIO modifiés par changement de modulation/rate
2. comparer BPSK ↔ QPSK ↔ 16-QAM ↔ 64-QAM
3. identifier les registres du digital baseband non expliqués
4. rechercher les séquences de test dans une vraie librftest/ATE si obtenue
5. sinon caractériser dynamiquement les registres candidats sur silicium
```

---

#### 36CQ.10 Impact sur QPSK/QAM propriétaire

Le verdict logiciel actuel devient plus précis :

```text
QAM natif Wi-Fi
    → OUI, parfaitement sélectionnable

QAM propriétaire via API existante nommée
    → NON trouvé / surface nommée fermée négativement

QAM propriétaire via MMIO caché
    → toujours ouvert

QAM propriétaire via TXIQ vectoriel
    → toujours partiellement ouvert
```

---

#### 36CQ.11 Statut v0.78

| Élément | Statut |
|---|---:|
| symboles mapper/FEC/interleaver dans corpus | **aucun** |
| exports ROM mapper/FEC/interleaver | **aucun** |
| factory API coded-bit/constellation | **aucune documentée** |
| mapper QAM matériel natif | **100 % existe** |
| API nommée de bypass mapper | **fermée négativement** |
| MMIO caché de bypass | **ouvert** |
| bibliothèque ATE non fournie avec bypass | **possible, non démontré** |
| nécessité de recherche par MMIO/différentielle | **forte** |

---


### 36CR. QPSK/QAM TX — le rate legacy est entièrement décodé derrière un seul nibble — v0.79

La v0.72 avait fermé la correspondance :

```text
CE0[15:12] = PLCP RATE
```

mais une question restait ouverte :

```text
le CPU programme-t-il aussi, ailleurs dans lmacSetTxFrame(),
un registre séparé de modulation/FEC selon le rate ?
```

Le désassemblage exact répond maintenant : **non pour le chemin legacy**.

---

#### 36CR.1 Chargement du rate

Dans `.text.lmacSetTxFrame` :

```text
+0x1E3  L8UI  a2, a8, 8
```

avec :

```text
a8 = esf_tx_desc_s *
```

et le DWARF :

```text
esf_tx_desc_s.rate @ +8
```

Donc :

```text
a2 = desc->rate
```

---

#### 36CR.2 Construction du nibble PLCP

Immédiatement après :

```text
+0x1E9  EXTUI a13, a2, 0, 4
+0x1EC  SLLI  a13, a13, 12
```

soit :

```c
rate4 = desc->rate & 0x0F;
rate_field = rate4 << 12;
```

Ce champ est combiné avec :

```text
LENGTH
KID
HT flag
```

puis :

```text
+0x200  S32I a13, WDEV, 0x2E0
```

donc :

```text
REG32(0x3FF20CE0) = plcp1;
```

---

#### 36CR.3 La seule branche structurelle est legacy versus HT

Juste avant le store :

```text
+0x1F2  BGEUI a2, 16, ...
```

Le test est uniquement :

```text
rate >= 16 ?
```

Il sert à ajouter :

```text
HT flag
```

dans `CE0`.

Il ne distingue pas :

```text
BPSK
QPSK
16-QAM
64-QAM
```

entre elles.

---

#### 36CR.4 Deuxième lecture du rate après CE0

Après le store :

```text
+0x203  L8UI a2, a8, 8
+0x209  BGEUI a2, 16, +HT_path
+0x20C  J common_continuation
```

Donc :

```text
si rate < 16:
    sauter directement au chemin commun

si rate >= 16:
    construire CE4 / HT-SIG
```

Le chemin legacy **ne traverse aucune autre logique rate-specific** dans la préparation PHY.

---

#### 36CR.5 Conséquence pour les huit rates OFDM legacy

Les huit valeurs :

```text
0x0B / 0x0F → BPSK
0x0A / 0x0E → QPSK
0x09 / 0x0D → 16-QAM
0x08 / 0x0C → 64-QAM
```

sont donc distinguées par le CPU uniquement via :

```text
CE0[15:12]
```

Le hardware reçoit le code PLCP RATE et en déduit lui-même :

```text
constellation
coding rate
puncturing
interleaver geometry
mapper behavior
```

selon le mode OFDM standard.

---

#### 36CR.6 Aucun `modulation register` CPU séparé dans ce chemin

Le code ne fait pas :

```text
if QPSK:
    write MOD=QPSK

if 16-QAM:
    write MOD=16QAM

if 64-QAM:
    write MOD=64QAM
```

et ne fait pas non plus :

```text
write FEC_RATE
write PUNCTURE_MODE
write INTERLEAVER_MODE
```

séparément pour le legacy.

Le seul sélecteur visible est :

```text
RATE nibble
```

---

#### 36CR.7 Implication hardware

Le meilleur modèle devient :

```text
CE0.RATE
   ↓
decoder PHY interne
   ├─ modulation family
   ├─ coding rate
   ├─ puncturing
   ├─ interleaver parameters
   └─ constellation mapper mode
```

Cette logique est donc **plus profondément enfouie dans le digital baseband** que la banque WDEV
programmé par le LX106.

---

#### 36CR.8 Conséquence QAM propriétaire

Cette découverte réduit encore la probabilité d'un bypass simple par un registre voisin.

Pour obtenir un point QAM arbitraire, il faudrait :

```text
soit
    contourner le decoder RATE interne

soit
    entrer après son choix de mapper

soit
    utiliser un test-path matériel distinct
```

Aucun de ces chemins n'est exposé dans `lmacSetTxFrame()`.

---

#### 36CR.9 Différence avec le chemin HT

Pour :

```text
rate >= 16
```

le CPU doit fournir davantage de paramètres :

```text
MCS
HT-LENGTH
Aggregation
GI
...
```

dans :

```text
0x3FF20CE4
```

Mais même dans ce chemin, il fournit toujours des **paramètres PHY**, pas des symboles QAM.

Le mapper reste interne.

---

#### 36CR.10 Statut v0.79

| Élément | Statut |
|---|---:|
| `desc.rate` chargé à +8 | **100 %** |
| legacy RATE → CE0[15:12] | **100 %** |
| branche rate principale = `<16` / `>=16` | **100 %** |
| second write rate-specific legacy | **aucun dans lmacSetTxFrame** |
| registre modulation séparé legacy | **absent dans ce chemin** |
| decoder RATE→QAM/FEC matériel | **~99 % architecture** |
| mapper derrière decoder hardware | **très haute confiance** |
| bypass CPU visible au même niveau | **non** |

---


### 36CS. QPSK/QAM TX — fermeture aval `lmacSetTxFrame → wDev_EnableTransmit` — v0.80

La v0.79 a démontré qu'au niveau de `lmacSetTxFrame()` le seul contrôle CPU qui distingue
les huit rates OFDM legacy est :

```text
0x3FF20CE0[15:12] = PLCP RATE
```

Il restait une possibilité :

```text
après lmacSetTxFrame(),
une fonction aval programme-t-elle un deuxième registre
de modulation/FEC/mapper ?
```

Cette passe ferme le chemin CPU canonique jusqu'à l'armement de la transmission.

---

#### 36CS.1 Position des deux appels dans `lmacTxFrame()`

Les relocations exactes de :

```text
.text.lmacTxFrame
```

démontrent :

```text
+0x134 → lmacSetTxFrame
+0x161 → wDev_EnableTransmit
```

Le flot est donc :

```text
préparation descriptor
    ↓
lmacSetTxFrame(...)
    ↓
quelques opérations MAC locales
    ↓
wDev_EnableTransmit(...)
```

Il n'existe aucun autre appel externe entre les deux susceptible de traduire le rate en un
second ensemble de paramètres PHY.

---

#### 36CS.2 Après `lmacSetTxFrame()`, le descriptor rate n'est plus relu

Le désassemblage après l'appel à :

```text
lmacSetTxFrame
```

montre des chargements depuis le buffer/contexte pour préparer :

```text
index
AIFS
backoff
```

puis l'appel final WDEV.

Il n'existe pas de lecture du byte :

```text
esf_tx_desc_s.rate @ +8
```

dans cette portion aval.

Le `rate` a donc terminé son chemin CPU au moment où `lmacSetTxFrame()` a écrit `CE0`.

---

#### 36CS.3 ABI exacte de `wDev_EnableTransmit()`

Le DWARF de `wdev.o` ferme :

```c
wDev_EnableTransmit(index, aifs, backoff)
```

avec les trois paramètres :

```text
index
aifs
backoff
```

Leurs registres ABI sont :

```text
a2 = index
a3 = aifs
a4 = backoff
```

Aucun paramètre :

```text
rate
MCS
FEC
modulation
descriptor pointer
payload pointer
constellation index
I
Q
```

n'est présent.

---

#### 36CS.4 Traitement de `backoff`

Le début du désassemblage effectue :

```text
EXTUI a9, a4, 0, 10
```

soit :

```c
backoff10 = backoff & 0x3FF;
```

La fonction prépare ensuite une adresse WDEV dépendant de `index` et programme l'état de
contention/armement associé.

Le rôle de `backoff` est donc explicitement MAC/CSMA, pas PHY modulation.

---

#### 36CS.5 Base WDEV exacte

Le premier littéral de la section est :

```text
0x3FF20A00
```

qui est la même base WDEV/MAC déjà reconstruite dans le projet.

La fonction utilise cette base pour programmer les registres de transmission associés à la queue/index.

Elle n'utilise pas le bloc PHY `0x6000xxxx` pour sélectionner une constellation.

---

#### 36CS.6 Contrôle `0xC0000000`

Un deuxième littéral exact de la fonction vaut :

```text
0xC0000000
```

Le code :

```text
lit un registre WDEV
OR 0xC0000000
réécrit l'état de contrôle
```

dans le chemin d'activation.

Ce pattern est compatible avec un armement/start de queue MAC, pas avec une définition de
QAM/FEC.

Le nom électrique individuel des deux bits hauts n'est pas nécessaire à la conclusion QAM.

---

#### 36CS.7 Aucune transmission du rate vers `wDev_EnableTransmit`

La frontière devient donc :

```text
rate
 ↓
lmacSetTxFrame
 ↓
CE0.RATE
 ↓
retour CPU
 ↓
wDev_EnableTransmit(index,aifs,backoff)
 ↓
armement MAC
```

Il n'existe pas :

```text
wDev_EnableTransmit(..., rate, ...)
```

ni :

```text
wDev_EnableTransmit(..., modulation, ...)
```

---

#### 36CS.8 Conséquence architecturale

Le hardware doit posséder, derrière :

```text
CE0.RATE
```

un décodeur qui sélectionne :

```text
BPSK / QPSK / 16-QAM / 64-QAM
coding rate
puncturing
interleaver geometry
mapper constellation
```

Lorsque le MAC est ensuite armé, il consomme l'état PHY déjà préparé.

Cette architecture sépare proprement :

```text
configuration PHY
        ↓
armement MAC / contention
```

---

#### 36CS.9 Portée de la fermeture

Cette passe ferme le **chemin CPU canonique** :

```text
lmacSetTxFrame
        ↓
wDev_EnableTransmit
```

Elle ne prouve pas qu'il n'existe absolument aucun registre test caché dans tout le silicium.

Elle prouve que :

> **le chemin standard de transmission ESP8266 ne programme aucun second contrôle modulation
> après CE0.RATE avant l'armement WDEV.**

---

#### 36CS.10 Conséquence pour le QAM propriétaire

La recherche d'un point arbitraire de constellation ne doit plus viser :

```text
lmacTxFrame
wDev_EnableTransmit
AIFS/backoff registers
```

Ces couches sont maintenant fermées comme :

```text
configuration/armement MAC
```

et non :

```text
entrée mapper
```

La cible reste :

```text
digital baseband interne
ou
mode test anonyme
ou
TXIQ vectoriel alternatif
```

---

#### 36CS.11 Statut v0.80

| Élément | Statut |
|---|---:|
| `lmacTxFrame → lmacSetTxFrame` | **100 % relocation** |
| `lmacTxFrame → wDev_EnableTransmit` | **100 % relocation** |
| ABI WDEV = index/aifs/backoff | **100 % DWARF** |
| backoff masqué sur 10 bits | **100 % instructionnel** |
| rate transmis à WDEV Enable | **non** |
| second contrôle modulation aval | **aucun dans le chemin CPU canonique** |
| frontière CPU modulation = CE0.RATE | **~99–100 % architecture** |
| mapper interne derrière RATE | **très haute confiance** |

---


### 36CT. QPSK/QAM RX — `DC_I/DC_Q` = statistique moyenne/DC, corrélateur prioritaire — v0.81

La v0.61 avait identifié :

```text
rom_dc_iq_est(mode,N,out)
    ↓
out[0] = moyenne I
out[1] = moyenne Q
```

Cette primitive était un candidat séduisant pour un récepteur QPSK assisté.

La v0.81 précise maintenant **ce que ces deux sorties représentent réellement dans le bloc IQ_EST**.

---

#### 36CT.1 `rom_dc_iq_est()` calcule explicitement une moyenne

Le désassemblage exact de :

```text
rom_dc_iq_est @ 0x4000615C
```

effectue :

```text
iq_est_enable(mode, N)
```

puis :

```text
REG32(0x600005DC)
    ↓
arith_shift_right 6
    ↓
signed_divide by (N+1)
    ↓
out[0]
```

et :

```text
REG32(0x600005E0)
    ↓
arith_shift_right 6
    ↓
signed_divide by (N+1)
    ↓
out[1]
```

avant :

```text
iq_est_disable()
```

Le caractère :

```text
sum / (N+1)
```

est donc démontré directement.

---

#### 36CT.2 Les deux accumulateurs sont signés

Le code utilise :

```text
SRAI
```

puis :

```text
__divsi3
```

et non une division non signée.

Les grandeurs peuvent donc prendre les deux signes :

```text
I_DC < 0 / > 0
Q_DC < 0 / > 0
```

ce qui est cohérent avec des composantes DC/offset vectorielles.

---

#### 36CT.3 `rom_get_corr_power()` lit sept résultats IQ_EST distincts

À :

```text
rom_get_corr_power @ 0x40006260
```

la base MMIO est la région :

```text
0x60000200
```

Les lectures exactes correspondent à :

```text
+0x380 → 0x60000580
+0x384 → 0x60000584
+0x388 → 0x60000588
+0x38C → 0x6000058C

+0x3DC → 0x600005DC
+0x3E0 → 0x600005E0
+0x3E4 → 0x600005E4
```

Le même estimateur expose donc simultanément :

```text
matrice/corrélation
DC I
DC Q
énergie totale
```

---

#### 36CT.4 Traitement séparé de la composante DC

Le code prend les deux accumulateurs DC et calcule :

```text
DC_I_scaled = signed_shift(DC_I)
DC_Q_scaled = signed_shift(DC_Q)

dc_power =
      DC_I_scaled²
    + DC_Q_scaled²
```

puis applique une normalisation et stocke cette grandeur séparément dans la structure de sortie.

Ce traitement est une preuve fonctionnelle très forte que :

```text
0x600005DC/E0
```

constituent la **composante DC/moyenne complexe** de la fenêtre.

---

#### 36CT.5 La corrélation complexe vient d'autres registres

En parallèle :

```text
R0 = 0x60000580
R1 = 0x60000584
R2 = 0x60000588
R3 = 0x6000058C
```

sont combinés comme :

```text
X = R0 + R3
Y = R1 - R2
```

puis :

```text
corr_power = X² + Y²
```

Cette paire :

```text
(X,Y)
```

est la construction réellement compatible avec les deux composantes d'une **corrélation complexe**.

Donc le bloc sépare explicitement :

```text
DC vector
≠
correlation vector
```

---

#### 36CT.6 Conséquence mathématique pour une constellation QAM équilibrée

Une constellation QPSK/QAM standard est centrée autour de l'origine :

```text
E[I_symbol] ≈ 0
E[Q_symbol] ≈ 0
```

sur une séquence équilibrée.

Une primitive qui calcule :

```text
mean(I)
mean(Q)
```

sur plusieurs symboles tend donc vers :

```text
(0,0)
```

même lorsque les symboles individuels sont parfaitement distincts.

Ainsi :

> **une grande fenêtre `rom_dc_iq_est()` ne peut pas être utilisée comme démodulateur
> de constellation QPSK/QAM symbole-par-symbole.**

---

#### 36CT.7 Le cas `N=0` reste ouvert physiquement

Le logiciel accepte :

```text
N = 0
```

et divise alors par :

```text
N+1 = 1
```

Cela signifie seulement que le chemin CPU autorise une fenêtre minimale.

Ce que la statique ne démontre pas :

```text
- durée physique exacte d'une unité N
- instant d'échantillonnage
- emplacement du tap dans le datapath RX
- relation N=0 → un vrai sample I/Q RF/baseband
- bande passante / cohérence de phase
```

Il serait donc incorrect de transformer :

```text
N=0 est accepté
```

en :

```text
N=0 fournit un point QPSK instantané
```

---

#### 36CT.8 Nouvelle priorité : le vecteur de corrélation

Le candidat RX le plus intéressant devient :

```text
X = R0 + R3
Y = R1 - R2
```

avec :

```text
R0..R3 = 0x60000580..58C
```

car ce couple conserve une information de **phase de corrélation** avant que le SDK ne la réduise à :

```text
X² + Y²
```

pour ses calibrations.

Si le signal externe peut être corrélé contre une référence utile, alors :

```text
atan2(Y,X)
```

pourrait conceptuellement fournir un angle relatif.

Mais la référence interne du corrélateur n'est pas encore identifiée.

---

#### 36CT.9 Verrou RX QPSK désormais exact

La question n'est plus :

```text
"peut-on lire I et Q ?"
```

de façon générale.

Elle devient :

```text
quelle est la référence du corrélateur IQ_EST ?
et peut-elle suivre un signal externe avec une phase cohérente ?
```

Deux scénarios :

```text
A. référence exploitable/cohérente
   → corrélateur potentiellement utilisable comme détecteur vectoriel

B. référence uniquement interne/calibration
   → pas de constellation externe accessible par cette voie
```

---

#### 36CT.10 Impact sur la faisabilité RX

Le classement doit être corrigé :

```text
rom_dc_iq_est comme per-symbol vector sampler
    → rétrogradé

rom_dc_iq_est comme DC/mean probe
    → démontré

IQ_EST correlation vector
    → candidat principal

raw RF I/Q stream
    → toujours absent
```

Le QPSK/QAM RX assisté reste **possible comme recherche**, mais le chemin DC_I/DC_Q
n'est plus considéré comme une quasi-sortie de constellation.

---

#### 36CT.11 Statut v0.81

| Élément | Statut |
|---|---:|
| `DC_I/DC_Q` = moyenne signée | **100 % logiciel** |
| division par `N+1` | **100 %** |
| `DC_I²+DC_Q²` traité comme énergie DC | **100 % instructionnel** |
| corrélation complexe issue de R0..R3 | **~99 % structurel** |
| `DC_I/Q` = stream de constellation | **écarté comme interprétation standard** |
| `N=0` = sample instantané | **non démontré** |
| R0..R3 pour angle relatif | **candidat principal** |
| référence du corrélateur | **ouverte** |
| QPSK RX via IQ_EST | **toujours ouvert, plus contraint** |

---


### 36CU. QPSK/QAM RX — IQ_EST = statistiques intégrées, pas port symbole — v0.82

La v0.81 a corrigé l'interprétation des accumulateurs :

```text
0x600005DC/E0
    → moyenne / DC I,Q
```

et a placé :

```text
0x60000580..58C
```

comme candidat corrélateur complexe.

Cette passe cherche à savoir si ces quatre registres constituent déjà une sortie de constellation.

Le comportement de `ram_rxiq_get_mis()` montre qu'ils appartiennent avant tout à un
**estimateur statistique de mismatch I/Q**.

---

#### 36CU.1 Séquence réelle dans RXIQ calibration

Le chemin v6 démontré est :

```text
tone TX interne / loopback calibration
        ↓
iq_est_enable(1, N)
        ↓
attente DONE matérielle
        ↓
ram_rxiq_get_mis(...)
        ↓
iq_est_disable()
```

Donc les valeurs lues par `ram_rxiq_get_mis()` correspondent à la **fenêtre d'intégration**
configurée par IQ_EST.

Elles ne sont pas lues en continu sample-by-sample.

---

#### 36CU.2 Lectures exactes de `ram_rxiq_get_mis()`

La fonction v6 :

```text
ram_rxiq_get_mis @ 0x1D54
taille = 444 octets
```

charge la base PHY puis lit :

```text
0x60000580 = R0
0x60000584 = R1
0x60000588 = R2
0x6000058C = R3
```

avec `MEMW`.

Chaque valeur est ensuite normalisée/décalée avant utilisation.

---

#### 36CU.3 Combinaisons matricielles explicites

Le flot reconstruit calcule notamment :

```text
R0 - R3
R0 + R3

R1 + R2
R1 - R2
```

puis sélectionne/oriente ces grandeurs selon les paramètres de calibration.

Ce pattern est caractéristique d'une **matrice/corrélation I/Q** destinée à mesurer :

```text
gain mismatch
phase/quadrature mismatch
```

et non d'un simple couple :

```text
I_sample
Q_sample
```

---

#### 36CU.4 Arithmétique 64 bits de correction

Les relocations de la fonction démontrent plusieurs appels à :

```text
__muldi3
__divdi3
```

La fonction construit des produits/rapports 64 bits des statistiques précédentes,
puis réduit les résultats en corrections signées.

Ce traitement est celui d'un estimateur de paramètres.

Il ne ressemble pas à un chemin :

```text
read X
read Y
return symbol
```

---

#### 36CU.5 Sorties finales = corrections de mismatch

Le flot finit par stocker des résultats sur octets signés dans la structure de sortie
du chemin RXIQ.

Ces résultats sont consommés par :

```text
rxiq_cover / rfcal_rxiq
```

pour corriger les déséquilibres du récepteur.

La chaîne de debug embarquée :

```text
rxiq_get_mis: total_pwr=%d, ...
```

confirme la finalité de mesure/calibration.

---

#### 36CU.6 Ensemble des familles de sorties IQ_EST

Après v0.81–0.82, le bloc est mieux classé :

```text
0x600005DC
0x600005E0
    → statistiques de moyenne/DC signées

0x600005E4
    → énergie / puissance accumulée

0x60000580..58C
    → statistiques de corrélation / matrice I-Q
```

Le bloc fournit donc :

```text
premier ordre
second ordre
énergie
```

sur une fenêtre d'intégration.

---

#### 36CU.7 Ce qui manque à une vraie sortie SDR/QAM

Pour une SDR ou un démodulateur QAM logiciel général, on voudrait :

```text
I[0], Q[0]
I[1], Q[1]
I[2], Q[2]
...
```

ou au minimum :

```text
I_symbol[k], Q_symbol[k]
```

à chaque symbole.

Aucune interface de ce type n'est identifiée.

IQ_EST donne plutôt :

```text
SUM / MEAN / CORRELATION / POWER
```

sur une fenêtre.

---

#### 36CU.8 Pourquoi une fenêtre courte reste malgré tout intéressante

La fermeture précédente n'implique pas :

```text
IQ_EST totalement inutile pour QPSK/QAM
```

Une fenêtre courte et synchronisée peut, selon l'emplacement physique du tap, produire des
statistiques dépendantes du symbole.

En particulier, si un corrélateur possède une référence cohérente :

```text
(X,Y)
```

peut encore contenir une phase relative utile.

Mais il manque trois preuves :

```text
1. référence exacte du corrélateur
2. relation fenêtre N → durée/symbole
3. comportement sur signal externe sans loopback calibration
```

---

#### 36CU.9 Conséquence pour `N=0`

`N=0` reste accepté par le logiciel.

Mais la v0.82 renforce la prudence :

```text
N=0
```

signifie seulement :

```text
fenêtre minimale de l'estimateur
```

et non :

```text
accès garanti à un sample ADC I/Q
```

La nature du résultat reste celle du bloc IQ_EST.

---

#### 36CU.10 Nouveau classement RX QPSK/QAM

##### Écarté

```text
DC_I/DC_Q comme flux constellation
R0..R3 comme FIFO I/Q brute
phy_adc_read_fast comme ADC RF I/Q
```

##### Toujours candidat

```text
corrélation courte synchronisée
CFO + corrélateur
démapper Wi-Fi matériel
mode test digital baseband
```

##### Non trouvé

```text
raw I/Q stream
constellation-index RX
soft symbols / LLR accessibles
mapper/demapper bypass
```

---

#### 36CU.11 Implication sur le verdict QAM RX

Le hardware sait évidemment recevoir/démoduler QPSK/QAM Wi-Fi.

En revanche, pour un **QAM propriétaire** :

```text
IQ_EST seul
```

n'est plus considéré comme une voie quasi-fermée.

Il est un instrument de statistiques potentiel.

Le chemin le plus prometteur devient maintenant :

```text
démodulateur/demapper matériel natif
        ↓
chercher une sortie soft/hard symbols avant décodage MAC
```

ou, à défaut :

```text
utiliser des statistiques IQ_EST avec un protocole conçu autour d'elles
```

qui serait alors une modulation/détection propriétaire assistée, mais pas une SDR/QAM classique.

---

#### 36CU.12 Statut v0.82

| Élément | Statut |
|---|---:|
| R0..R3 lus après fenêtre IQ_EST | **100 % chemin** |
| combinaisons R0±R3 / R1±R2 | **100 % instructionnel** |
| calcul mismatch via mul/div 64 bits | **100 % relocations + flot** |
| IQ_EST = moteur statistique fenêtre | **~99 % fonctionnel** |
| R0..R3 = raw I/Q samples | **écarté** |
| R0..R3 = symboles QAM directs | **non démontré / fortement non supporté** |
| corrélation courte comme détecteur | **ouverte** |
| démapper natif comme prochaine cible | **priorité haute** |
| QAM RX propriétaire classique | **plus contraint qu'en v0.67** |

---


### 36CV. QPSK/QAM RX — frontière standard du démapper : bytes + metadata uniquement — v0.83

La v0.81–0.82 a fermé `IQ_EST` comme moteur de statistiques de fenêtre, pas comme port de
constellation. La prochaine cible était donc le **démodulateur/démapper Wi-Fi natif** :

```text
QPSK/QAM RF
    ↓
démodulation OFDM
    ↓
demapper
    ↓
?
    ↓
CPU
```

Cette passe ferme l'interface logicielle standard visible après ce bloc.

---

#### 36CV.1 `RxControl` exact : 12 octets

Le DWARF de `libpp/wdev` décrit :

```text
struct RxControl
size = 12 bytes
```

Les champs nommés sont :

```text
rssi
rate
is_group
sig_mode
legacy_length

damatch0
damatch1
bssidmatch0
bssidmatch1

MCS
CWB
HT_length
Smoothing
Not_Sounding
Aggregation
STBC
FEC_CODING
SGI

rxend_state
ampdu_cnt
channel
noise_floor
```

Ces champs décrivent :

```text
qualité / niveau
mode PHY
rate / MCS
longueurs
paramètres HT
état de fin RX
canal / bruit
```

---

#### 36CV.2 Ce qui n'existe pas dans `RxControl`

Aucun membre DWARF ne correspond à :

```text
I
Q
I_symbol
Q_symbol
constellation
symbol_index
softbit
soft_bit
LLR
confidence par bit
EVM par symbole
phase par symbole
```

Le descriptor standard n'est donc pas une structure de sortie du démapper soft.

Il est une structure de **metadata de paquet reçu**.

---

#### 36CV.3 `esf_buf_s` exact : 40 octets

Le DWARF de `wdev.o` ferme :

```text
struct esf_buf_s
size = 40 bytes
```

avec :

```text
+0   pbuf
+4   ds_head
+8   ds_tail
+12  ds_len
+16  buf_begin
+20  hdr_len
+22  data_len
+24  chl_freq_offset
+28  trc
+32  bqentry
+36  desc
```

La structure transporte donc :

```text
buffer / chaîne DMA
bytes reçus
longueurs
CFO mesuré
états de bookkeeping
descriptor
```

mais toujours aucun tableau de symboles ou soft-bits.

---

#### 36CV.4 Le payload CPU est déjà un flux d'octets

`buf_begin`, `hdr_len` et `data_len` montrent que le CPU reçoit un buffer de trame.

L'architecture visible est donc :

```text
RF
 ↓
OFDM demod
 ↓
QPSK/QAM demapper
 ↓
deinterleave / FEC decode
 ↓
bytes de trame
 ↓
esf_buf_s.buf_begin
```

et non :

```text
RF
 ↓
I/Q symbols
 ↓
CPU
 ↓
software decoder
```

Cette distinction est décisive pour le projet SDR/QAM propriétaire.

---

#### 36CV.5 `wDev_ProcessRxSucData()` n'appelle pas un export soft-symbol

Les relocations exactes de :

```text
wDev_ProcessRxSucData()
```

montrent notamment :

```text
chm_get_current_channel
phy_get_bb_freqoffset
wDev_DiscardFrame
wDev_IndicateFrame
rcUpdateDataRxDone
```

Le chemin standard appelle :

```text
phy_get_bb_freqoffset()
```

pour compléter la metadata fréquence.

Aucune relocation ne cible :

```text
phy_get_bb_evm
rom_dc_iq_est
iq_est_enable
soft demapper
LLR
constellation readout
```

dans cette fonction.

---

#### 36CV.6 Audit global de `phy_get_bb_evm()`

Un audit de tous les objets extraits de :

```text
libphy
libpp
libnet80211
```

recherche les relocations vers :

```text
phy_get_bb_evm
```

Le seul appel retrouvé est :

```text
phy_chip_v6_cal.o
    fix_cache_bug()
        ↓
    phy_get_bb_evm()
```

Ce chemin a déjà été reclassé comme init/cache workaround, pas comme export EVM RX normal.

Donc :

> **le RX paquet standard n'exporte même pas la métrique BB EVM via son chemin WDEV normal.**

---

#### 36CV.7 Conséquence sur le démapper matériel

Le hardware sait nécessairement produire les décisions nécessaires pour :

```text
BPSK
QPSK
16-QAM
64-QAM
```

puis décoder la trame.

Mais la frontière CPU démontrée se situe **après** ces traitements.

La meilleure représentation est :

```text
              digital baseband matériel
┌───────────────────────────────────────────────┐
│ FFT / égalisation                             │
│ dérotation                                    │
│ QPSK/QAM demapper                             │
│ deinterleaver                                 │
│ FEC decode                                    │
│ CRC / RX state                                │
└───────────────────────────────────────────────┘
                 │
                 ▼
          bytes + RxControl
                 │
                 ▼
               LX106
```

Les objets intermédiaires du démapper ne sont pas visibles dans les structures CPU standard.

---

#### 36CV.8 Ce que cela élimine

La piste :

```text
promiscuous RX / RxControl
    ↓
soft symbols cachés
    ↓
QAM logiciel
```

est fermée négativement pour le corpus exact.

De même :

```text
esf_buf_s
    ↓
LLR ou constellation index
```

n'est pas supporté par le layout DWARF.

---

#### 36CV.9 Ce que cela n'élimine pas

Il peut encore exister :

```text
1. un MMIO anonyme de test donnant accès à un stade pré-FEC
2. une SRAM/FIFO interne non référencée par le SDK standard
3. une bibliothèque ATE/usine non présente dans le corpus
4. un test mode caché du digital baseband
```

La v0.83 ne prouve pas l'inexistence physique universelle d'un tel point.

Elle ferme seulement :

> **l'interface RX standard du corpus comme sortie bytes + metadata, sans soft-symbol port.**

---

#### 36CV.10 Impact sur QPSK/QAM RX propriétaire

Le chemin natif standard ne peut pas être détourné simplement en lisant `RxControl`.

Pour recevoir un QAM propriétaire classique, il faudrait désormais l'un des deux scénarios :

##### Scénario A — trouver un tap pré-FEC / pré-demapper caché

```text
baseband
 ↓
soft symbols / constellation
 ↓
CPU
```

##### Scénario B — utiliser le pipeline Wi-Fi natif

Le signal reste suffisamment Wi-Fi-compatible pour que :

```text
synchro + OFDM + demapper + FEC
```

soient réalisés matériellement, et le projet modifie surtout les données/protocole au-dessus.

Sans l'un de ces scénarios, `RxControl` seul ne suffit pas à une modulation QAM propriétaire.

---

#### 36CV.11 Statut v0.83

| Élément | Statut |
|---|---:|
| `RxControl` size 12 | **100 % DWARF** |
| liste des metadata PHY | **100 % DWARF** |
| LLR dans `RxControl` | **absent** |
| I/Q dans `RxControl` | **absent** |
| constellation index dans `RxControl` | **absent** |
| `esf_buf_s` size 40 | **100 % DWARF** |
| bytes/pointeurs/length/CFO dans `esf_buf_s` | **100 % DWARF** |
| soft-symbol buffer dans `esf_buf_s` | **absent** |
| `phy_get_bb_freqoffset` dans RX success | **100 % relocation** |
| `phy_get_bb_evm` dans RX success | **absent** |
| seul caller `phy_get_bb_evm` = `fix_cache_bug` | **100 % corpus fourni** |
| interface RX standard = bytes + metadata | **~99 % fonctionnel** |
| tap pré-FEC caché | **toujours ouvert** |

---

# ANNEXE F — Provenance et empreintes

Les fichiers d'entrée utilisés pour cette consolidation sont :

- `ESP8266_TX_FSK_MFSK_REFERENCE_OFFICIELLE_PROJET_v1.0(2).md` — 21891 octets — SHA-256 `f623b290519f14fb25b58e881b19103eed6012e6fae3c34ead6bfd54b966b9cc`
- `ESP8266_RX_OOK_ASK_REFERENCE_OFFICIELLE_PROJET_v1.0 (1).md` — 27708 octets — SHA-256 `97ed0d60f2848029188b0290977ca34c7de0969e88e371c589071f208eb57b8c`
- `ESP8266_RX_FSK_MFSK_REFERENCE_OFFICIELLE_PROJET_v1.0(2).md` — 19158 octets — SHA-256 `4a2111e8a62e1cac14f017d8ca2cd169aa0c76e6bd916579f1612d75ca9560f9`
- `ESP8266_ASK_OOK_REFERENCE_OFFICIELLE_PROJET_v1.0 (1).md` — 29137 octets — SHA-256 `a37e29542d3f74715d11bccc4897471ed82d13ef64c1831224dac9fbfd63c4da`
- `ESP8266_WIFI_PHY_REVERSE_ENGINEERING_v0.83_QAM_RX_STANDARD_DEMAPPER_EXPORT_BYTES_METADATA_ONLY(2).md` — 770584 octets — SHA-256 `e5070386bb78ed86e0c29176a54022400ad87038ade0cd84a7c0dff2afb96bd5`
- `librftest.a` — 83626 octets — SHA-256 `01c9b9712cd5772823b6b647fa2181c8b594162b5b7091663b8b3bd482400b8e`
