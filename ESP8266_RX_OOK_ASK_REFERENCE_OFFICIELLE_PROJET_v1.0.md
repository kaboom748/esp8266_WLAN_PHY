# ESP8266 — Récepteur autonome OOK / ASK
## Référence technique officielle du projet de reverse-engineering

**Édition unique : v1.0**  
**Date : 2026-09-12**  
**Statut : FIGÉE — émission après fermeture à 100 % du périmètre logiciel RX canonique**  
**Portée : ESP8266 / mask-ROM + PHY v6 du corpus analysé**

> **Important — statut du mot “officiel”.** Ce document est la référence officielle **du projet de reverse-engineering mené sur le corpus analysé**. Il ne s’agit pas d’un document officiel d’Espressif et il ne doit pas être présenté comme tel.

> **Règle de maintenance.** Cette édition est volontairement figée. Les découvertes ultérieures, optimisations de vitesse ou caractérisations RF ne modifieront pas cette édition ; elles continuent dans le document maître vivant `ESP8266_WIFI_PHY_REVERSE_ENGINEERING.md`.

---

## 1. Objet

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

## 2. Définition du « 100 % » dans cette référence

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

### 2.1 Tableau final de fermeture logicielle

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

## 3. Corpus et fonctions utilisées

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

# PARTIE I — ÉTAT RX NORMAL

## 4. Activation RF RX normale

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

## 5. RX OFF n’est pas RX normal

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

## 6. RX clock

Le RX RF et la logique de mesure nécessitent la RX clock.

La commande canonique est :

```text
rom_set_rxclk_en(1)
```

La calibration RXIQ officielle utilise elle-même cette primitive avant les opérations de mesure puis la coupe en sortie.

Dans la cible autonome, la RX clock reste active pendant la session de réception.

---

# PARTIE II — RETIRER LE WI‑FI SANS COUPER LE RX

## 7. PBUS debug mode

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

## 8. `pbus_workmode()` n’est pas nécessaire entre symboles

`pbus_workmode()` remet le contrôle PBUS dans le fonctionnement normal Wi‑Fi.

Pour un récepteur autonome qui ne revient pas au Wi‑Fi entre les symboles, la bonne architecture est de **rester en PBUS debug** pendant toute la session de démodulation.

La restauration en workmode n’est donc pas dans la boucle de symboles.

---

# PARTIE III — GAIN RX FIXE

## 9. Pourquoi le gain doit être fixe pour ASK

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

## 10. Packing logiciel du gain RX forcé

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

## 11. Gain baseband

Le PHY expose également `pbus_set_rxbbgain(...)`.

Son packing coarse/fine est distinct du gain RF. Pour la référence RX, il suffit que le choix soit :

- déterministe ;
- fixé avant la classification ASK ;
- inchangé pendant la trame.

La loi exacte index→dB n’est pas requise pour une classification relative en unités E4.

---

# PARTIE IV — SÉPARATION RX NORMAL / RXIQ LOOPBACK

## 12. Le point critique : IQ_EST n’active pas le loopback

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

## 13. `rom_iq_est_enable()` ne configure aucun loopback

`rom_iq_est_enable()` ne programme :

- ni le tone TX ;
- ni `set_loopback_gain()` ;
- ni les bits I²C RXIQ ci-dessus ;
- ni un mux RXIQ spécifique.

Il ne pilote que le bloc `IQ_EST_CTRL`.

Conclusion canonique :

> **Si l’on part du RX normal et que l’on n’exécute pas la séquence spéciale RXIQ, IQ_EST mesure le datapath RX dans son état normal courant.**

---

## 14. Ce qu’il ne faut pas appeler dans le RX externe canonique

Pendant la préparation du récepteur externe :

```text
NE PAS appeler set_loopback_gain()
NE PAS activer les bits I2C RXIQ spéciaux
NE PAS démarrer de tone TX
```

Ces primitives appartiennent à la calibration interne et ne sont pas nécessaires au détecteur RX externe.

---

# PARTIE V — IQ ESTIMATOR

## 15. Registre de contrôle

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

## 16. Protocole de `rom_iq_est_enable(mode,N)`

La routine :

1. active le bloc ;
2. programme `MODE` ;
3. programme `N[14:0]` ;
4. positionne `START` ;
5. attend matériellement `DONE=1` ;
6. retourne seulement lorsque la mesure est terminée.

Aucune temporisation logicielle arbitraire n’est utilisée pour attendre la fin de la mesure.

---

## 17. Valeur canonique de N

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

## 18. Désactivation / re-arm sûr

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

# PARTIE VI — MÉTRIQUE RX

## 19. Registre de puissance / énergie

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

## 20. Espressif utilise E4 comme valeur de décision

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

## 21. Pourquoi aucune conversion dBm n’est nécessaire

Pour OOK et ASK, on peut travailler directement dans l’espace numérique E4.

La seule exigence est que les classes apprises soient séparées dans cette métrique.

La conversion :

```text
E4 → dBm
```

est donc optionnelle et n’entre pas dans le chemin canonique.

---

# PARTIE VII — OOK

## 22. Principe OOK RX

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

## 23. Primitive OOK canonique

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

# PARTIE VIII — ASK / M-ASK

## 24. Principe ASK RX

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

## 25. Pourquoi le gain doit rester identique entre calibration et données

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

## 26. Classificateur M-ASK conceptuel

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

# PARTIE IX — POURQUOI LE RSSI DE PAQUET EST REJETÉ

## 27. `RxControl` / packet RSSI

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

# PARTIE X — POURQUOI LE CCA N’EST PAS NÉCESSAIRE

## 28. CCA

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

# PARTIE XI — INDÉPENDANCE DE PP / NET80211

## 29. Aucun pilote caché nécessaire

L’audit de `libpp.a` et `libnet80211.a` n’a révélé aucun second pilote nécessaire de :

```text
0x6000057C
```

Les occurrences apparentes d’offset `+0x37C` inspectées dans ces bibliothèques étaient relatives à d’autres bases ou des faux décodages.

La primitive IQ_EST canonique reste donc encapsulée par le PHY/ROM et peut être utilisée sans logique packet PP/net80211.

---

# PARTIE XII — PROCÉDURE AUTONOME COMPLÈTE

## 30. Phase A — boot et calibration initiale

1. démarrer normalement le silicium ;
2. laisser le PHY réaliser l’initialisation et les calibrations requises ;
3. laisser `rom_rfcal_rxiq()` terminer et restaurer ses bits spéciaux ;
4. fixer le canal / PLL ;
5. empêcher une mise en sleep ou une reprise Wi‑Fi pendant la session autonome.

Cette référence ne prétend pas remplacer toute l’initialisation analogique depuis un reset totalement froid par quelques écritures PBUS isolées.

---

## 31. Phase B — établir le RX externe

```text
rom_pbus_xpd_rx_on()
rom_set_rxclk_en(1)
PBUS debug mode
programmer gain RX fixe
```

Puis vérifier qu’aucune primitive de calibration RXIQ/loopback n’est appelée pendant la session.

---

## 32. Phase C — calibration des classes

### OOK

Mesurer plusieurs fenêtres connues OFF et ON :

```text
μ_OFF = moyenne / centre robuste des mesures OFF
μ_ON  = moyenne / centre robuste des mesures ON
T     = milieu des deux centres
```

### M-ASK

Pour chaque niveau connu du préambule :

```text
μ0 ... μM-1
```

Puis trier et placer les seuils entre centres voisins.

---

## 33. Phase D — boucle de réception

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

# PARTIE XIII — SQUELETTE DE FIRMWARE

## 34. API abstraite recommandée

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

## 35. Discipline MMIO

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

# PARTIE XIV — ÉLÉMENTS DÉLIBÉRÉMENT NON NÉCESSAIRES

## 36. Re-arm START-only

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

## 37. Nom électrique exact des bits PBUS conservés

`pbus_set_rxgain()` préserve explicitement les bits `0,2,7,8` du mot `PBUS(2,1)`.

Leur comportement logiciel pertinent est connu et préservé automatiquement par la primitive de gain.

Leur nom électrique détaillé n’est pas nécessaire au chemin canonique et n’est donc pas revendiqué dans cette référence.

---

## 38. Mode IQ_EST M=0

Le corpus démontre l’usage RX pertinent de :

```text
M=1
```

Le sens physique détaillé de `M=0` n’est pas requis pour OOK/ASK et est exclu de cette référence.

---

# PARTIE XV — VALIDATION PHYSIQUE

## 39. Pourquoi une validation silicium reste nécessaire

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

## 40. Mesures physiques à réaliser

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

# PARTIE XVI — CE QU’IL NE FAUT PAS FAIRE

## 41. Ne pas utiliser le packet RSSI comme détecteur OOK

Il dépend d’un paquet Wi‑Fi décodé.

---

## 42. Ne pas appeler `set_loopback_gain()` pour le RX externe

Cette primitive appartient à l’état spécial de calibration interne.

---

## 43. Ne pas lancer un tone TX dans le chemin RX externe

Le tone TX est utilisé dans RXIQ interne, pas dans la réception externe canonique.

---

## 44. Ne pas laisser un AGC implicite reprendre le contrôle

Rester dans le contexte PBUS manuel et conserver un gain déterministe pendant la classification ASK.

---

## 45. Ne pas changer le gain pendant un symbole ASK

Les centres et seuils E4 doivent être valables pour un état de gain stable.

---

## 46. Ne pas dépendre du CCA pour fonctionner

CCA peut être étudié comme accélérateur OOK, mais il ne fait pas partie du chemin de référence.

---

## 47. Ne pas présenter E4 comme des dBm sans calibration

`E4` est une métrique interne exploitable directement ; sa conversion physique exige une caractérisation.

---

# PARTIE XVII — RÉFÉRENCE RAPIDE

## 48. Cheat-sheet

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

## 49. Architecture complète recommandée

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

## 50. Frontière exacte entre « décodé » et « à mesurer »

### Décodé à 100 % dans cette référence

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

### À mesurer sur silicium

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

## 51. Conclusion officielle du projet pour RX OOK/ASK

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

## 52. Statut documentaire

**Document :** `ESP8266_RX_OOK_ASK_REFERENCE_OFFICIELLE_PROJET_v1.0.md`  
**Édition :** 1.0  
**Émission :** unique  
**Statut logiciel :** **100 % fermé dans le périmètre canonique décrit**  
**Maintenance future :** aucune — document figé  
**Suite du projet :** caractérisation silicium/RF et optimisations optionnelles dans le document maître vivant.
