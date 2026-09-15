# ESP8266 CCA / RF2400 — Journal de reverse-engineering

**Version :** 0.20 — v4.4 fortement asymétrique mais sous-échantillonné, preuve de pulses manqués, logger événementiel v4.5
**Date :** 2026-09-14  
**Objectif principal :** identifier une primitive matérielle ESP8266 permettant de détecter rapidement l’occupation RF du canal, idéalement sous la forme d’un bit `CCA_BUSY`, ou à défaut d’une mesure de puissance RX live suffisamment légère pour remplacer `IQ_EST`.

---

## 1. Résumé exécutif

Les recherches convergent vers une architecture dans laquelle le **CCA (Clear Channel Assessment)** de l’ESP8266 est un mécanisme matériel directement relié au moteur MAC de backoff CSMA.

Plusieurs éléments sont désormais établis :

- `0x60009B00 bit 28` contrôle l’activation/désactivation du CCA.
- La logique est inversée :
  - `bit28 = 0` → CCA actif ;
  - `bit28 = 1` → CCA désactivé.
- `rom_chip_v5_sense_backoff()` configure plusieurs champs associés au CCA/backoff.
- `0x60009B64` est utilisé par `rom_get_noisefloor()`.
- Le moteur MAC de backoff est matériel et programmable via WDEV.
- `rom_get_corr_power()` lit directement des registres RX/IQ sans lancer une acquisition IQ_EST.
- Les registres `RX_IQ_0..3`, le noise floor, les registres WDEV/FIQ et les registres de backoff sont désormais les principales pistes pour trouver :
  1. un bit `CCA_BUSY` réel ;
  2. ou une mesure de puissance RF live très légère.

Le point encore manquant est le **signal instantané FREE/BUSY** utilisé par le matériel CCA.

### Mise à jour v0.13

Cette passe ferme trois questions encore ouvertes.

### 1. Version exacte du PHY

`phy_version_print()` dans le `libphy.a` fourni contient directement :

```asm
movi a3, 1156
```

La bibliothèque analysée est donc :

```text
PHY version 1156
```

Espressif indique officiellement que la branche NonOS SDK 3.0.3 / AT 1.7.3 a introduit :

```text
Update to phy version 1156
```

La release NonOS 3.0.6 ultérieure ne mentionne pas de remplacement du PHY. Les conclusions du présent reverse-engineering portent donc sur une des toutes dernières révisions officielles de la PHY ESP8266 NonOS.

### 2. Piste `sense` définitivement éliminée

Le wrapper :

```text
phy_set_sense()
```

appelle une entrée de la table `g_phyFuns`.

La table installée par `register_chipv6_phy()` pointe cette entrée vers :

```text
chip_v6_set_sense()
```

et l'objet `phy_chip_v6_unused.o` montre que cette fonction fait exactement :

```asm
ret.n
```

sur 2 octets.

`chip_v6_get_sense()` est également un simple :

```asm
ret.n
```

La famille `sense` v6 ne contient donc aucun getter caché du CCA.

### 3. Scan des MMIO calculés dynamiquement

Un scan symbolique de tous les objets `libphy.a` a suivi les formes :

```text
base MMIO constante
+ index dynamique
+ offset
```

pour détecter les accès qui auraient échappé au scan des adresses fixes.

Dans l'espace PHY `0x60000000–0x6000FFFF`, les seuls accès dynamiques retrouvés concernent la mémoire RTC autour de `0x60001000`.

Aucun accès dynamique caché n'a été retrouvé vers :

```text
0x600005xx
0x600098xx
0x60009Axx–0x60009Dxx
```

Cela renforce fortement la conclusion qu'aucun `CCA_BUSY` n'est lu par le logiciel Espressif, même via une adresse MMIO calculée.

---

# 1. CCA — contrôle confirmé

## 2.1 `rom_chip_v5_disable_cca()`

Adresse ROM :

```text
0x400060D0
```

Comportement reconstruit :

```c
volatile uint32_t *r = (uint32_t *)0x60009B00;
*r |= 0x10000000;
```

Cela positionne le **bit 28** de `0x60009B00`.

### Conclusion

```text
0x60009B00 bit28 = 1
```

désactive le CCA.

---

## 2.2 `rom_chip_v5_enable_cca()`

Adresse ROM :

```text
0x400060EC
```

Comportement reconstruit :

```c
volatile uint32_t *r = (uint32_t *)0x60009B00;
*r &= 0xEFFFFFFF;
```

Cela efface le **bit 28**.

### Conclusion

```text
0x60009B00 bit28 = 0
```

active le CCA.

---

## 2.3 Signification du bit 28

Le bit 28 n’est donc probablement **pas** `CCA_BUSY`.

Il est plutôt un contrôle d’activation/désactivation :

```text
bit28 = 0  → CCA actif
bit28 = 1  → CCA désactivé
```

Cette interprétation concorde avec une ancienne annotation du reverse-engineering pvvx :

```text
0x60009b00 bit28 - cca
```

### État

**Confirmé / très fortement établi.**

---

# 2. `rom_chip_v5_sense_backoff()`

Adresse ROM :

```text
0x4000610C
```

La fonction effectue des read/modify/write sur :

```text
0x60009C28
0x60009D24
```

avec :

```text
0x60009C28 = 0x60009A00 + 0x228
0x60009D24 = 0x60009A00 + 0x324
```

Champs manipulés :

```text
0x60009C28  bits [16:10]
0x60009D24  bits [7:1]
```

Architecture probable :

```text
0x60009B00 bit28
       │
       ▼
     CCA HW
       │
       ▼
sense_backoff()
       │
       ├── 0x60009C28 [16:10]
       └── 0x60009D24 [7:1]
                   │
                   ▼
           MAC backoff engine
```

### Interprétation actuelle

`sense_backoff()` ressemble davantage à une fonction de **configuration de seuil/sensibilité du CCA et de son interaction avec le backoff** qu’à une fonction retournant directement l’état FREE/BUSY.

### État

**Fait :** les registres et champs sont réellement modifiés par la fonction.  
**Interprétation :** rôle précis encore à déterminer.

---

# 3. Banque PHY/Wi-Fi `0x60009A00`

Plusieurs fonctions ROM utilisent la base :

```text
0x60009A00
```

Cette zone est désormais considérée comme une banque de registres **Wi-Fi / PHY RX**.

Carte partielle :

```text
               0x60009A00
          Wi-Fi / PHY register bank
                   │
        ┌──────────┼──────────┐
        │          │          │
  0x60009B00 0x60009B64 0x60009C28
     bit28      noise       backoff
   CCA enable   floor         cfg
                              │
                         0x60009D24
                         backoff cfg
```

---

# 4. Noise floor

## 5.1 `rom_get_noisefloor()`

Adresse ROM :

```text
0x40006394
```

La fonction charge la base :

```text
0x60009A00
```

puis lit l’offset :

```text
0x164
```

soit :

```text
0x60009A00 + 0x164 = 0x60009B64
```

### Conclusion

```text
0x60009B64
```

est directement associé au **niveau de bruit RX / noise floor matériel**.

### État

**Confirmé.**

---

# 5. Regroupement fonctionnel des routines PHY ROM

Les fonctions suivantes sont regroupées dans la même région de ROM :

```text
0x400060D0  rom_chip_v5_disable_cca
0x400060EC  rom_chip_v5_enable_cca
0x4000610C  rom_chip_v5_sense_backoff
0x4000615C  rom_dc_iq_est
0x400061B8  rom_en_pwdet
...
0x40006260  rom_get_corr_power
...
0x40006394  rom_get_noisefloor
0x40006400  rom_iq_est_disable
0x40006430  rom_iq_est_enable
```

### Interprétation

CCA, power detector, noise floor, IQ estimator et correlation power semblent appartenir au même sous-système PHY.

Ce regroupement renforce l’hypothèse suivante :

```text
RF / RX front-end
      │
      ├── PWDET
      ├── IQ / corrélation
      ├── noise floor
      └── CCA
             │
             ▼
        MAC backoff
```

---

# 6. Piste PWDET

Fonction importante :

```text
rom_en_pwdet()
```

Adresse ROM :

```text
0x400061B8
```

Hypothèse fonctionnelle :

```text
PWDET = Power Detector
```

Architecture envisagée :

```text
antenne
   ↓
LNA / RX
   ↓
PWDET ───────────────┐
   │                 │
   │                 ▼
   │               CCA
   │             FREE/BUSY
   │                 │
   │                 ▼
   │              backoff
   │
   ├── noise floor
   │
   └── IQ_EST
```

### Pourquoi cette piste est importante

Si le bloc PWDET expose une sortie/status directement lisible, il pourrait fournir la donnée instantanée utilisée par le CCA sans lancer d’acquisition IQ_EST.

### Nouvelle réévaluation

Les symboles publics de `libphy.a` montrent une association très forte entre PWDET et le **contrôle de puissance TX** :

```text
tx_pwctrl_cal
tx_pwctrl_background
tx_pwctrl_bg_init
get_pwctrl_correct
loop_pwctrl_pwdet_error_accum_high_power
loop_pwctrl_correct_atten_high_power
meas_tone_pwr_db
```

Cela change sensiblement l'interprétation de `rom_en_pwdet()`.

Le scénario désormais le plus probable est que PWDET serve principalement à mesurer / corriger la puissance d'émission pendant les routines de calibration ou de contrôle de puissance TX.

Il n'est pas exclu qu'un détecteur matériel analogue soit partagé avec le chemin RX, mais **le nom `PWDET` seul ne constitue plus une raison suffisante pour le considérer comme candidat CCA prioritaire**.

### État

**Piste rétrogradée :** intéressante pour comprendre le PHY, mais plus candidat numéro 1 pour `CCA_BUSY`.

---

# 7. Moteur matériel de backoff WDEV

L’analyse de `libpp.a` montre que le moteur de backoff CSMA est matériel.

## 8.1 `wDev_EnableTransmit(index, aifs, backoff)`

Calcul de base :

```c
base = 0x3FF20A00 - 24 * index;
```

Écritures principales :

```c
REG(base + 0x3C0) = (backoff & 0x3FF) << 12;
REG(base + 0x3C4) |= 0xC0000000;
```

Pour `index = 0` :

```text
0x3FF20DC0  backoff 10 bits dans [21:12]
0x3FF20DC4  contrôle bits 31:30
```

Pour les autres files :

```text
BACKOFF_n = 0x3FF20DC0 - 0x18*n
CTRL_n    = 0x3FF20DC4 - 0x18*n
```

---

## 8.2 `wDev_DisableTransmit(index)`

Comportement :

```c
base = 0x3FF20A00 - 24 * index;
REG(base + 0x3C4) &= 0x3FFFFFFF;
```

### Conclusion architecturale

Le CPU configure le moteur de backoff puis laisse le hardware travailler.

Il n’y a pas de boucle CPU de type :

```c
while (cca_busy())
    ;
```

dans `wDev_EnableTransmit()`.

Architecture :

```text
CPU
 │
 │ écrit valeur backoff
 ▼
┌──────────────────┐
│ MAC backoff HW   │
│                  │
│ compteur         │◄──── CCA
│ AIFS/arbitrage   │
└────────┬─────────┘
         │
         ▼
     TX autorisé
```

### Conclusion

Le CCA est très probablement consommé **directement par le MAC hardware**.

### État

**Très fortement établi.**

---

# 8. Registre de backoff comme « oracle CCA »

Registre candidat :

```text
0x3FF20DC0 [21:12]
```

Question ouverte :

Le champ est-il uniquement un registre de chargement ou reflète-t-il également le compteur hardware live ?

Si le compteur reste lisible pendant sa décrémentation, le comportement attendu serait :

```text
canal FREE

312
311
310
309
...
```

puis lors d’une occupation RF :

```text
canal BUSY

247
247
247
247
...
```

puis :

```text
canal FREE

246
245
244
...
```

### Intérêt

Le gel du compteur fournirait un **oracle indirect extrêmement fiable du CCA matériel**, même sans connaître le registre `CCA_BUSY`.

Cela permettrait ensuite de corréler :

```text
backoff gelé
+
mesure RX
+
changement d’un bit MMIO
```

pour retrouver la sortie CCA réelle.

### État

**Hypothèse testable, non confirmée statiquement.**

---

# 9. Bits 31:30 de `CTRL_n`

`lmacDisableTransmit()` relit le registre :

```text
base + 0x3C4
```

et teste notamment le bit 31.

Masques observés :

```text
0xBFFFFFFF → clear bit 30
0x7FFFFFFF → clear bit 31
```

### Conclusion

Les bits 31:30 sont liés à la commande/état de transmission.

Ils ne constituent pas une bonne piste directe pour `CCA_BUSY`.

### État

**Fausse piste partiellement éliminée.**

---

# 10. `ic_get_rssi()` éliminé comme mesure RF live

Analyse de la fonction :

```c
p = rc_get_trc();

if (!p)
    return 31;

return (int8_t)(p[3] - 96);
```

### Conclusion

`ic_get_rssi()` récupère une valeur mémorisée dans une structure de rate-control / contexte de connexion.

Ce n’est pas un RSSI matériel live générique.

### Conséquence

La fonction n’est pas adaptée pour un détecteur OOK ou pour une mesure RF instantanée indépendante du trafic Wi-Fi.

### État

**Éliminé pour l’objectif principal.**

---

# 11. `rom_get_corr_power()` — candidat majeur

Adresse ROM :

```text
0x40006260
```

Cette fonction ne démarre apparemment **aucune acquisition**.

Elle lit directement plusieurs registres hardware :

```text
0x60000580  RX_IQ_0
0x60000584  RX_IQ_1
0x60000588  RX_IQ_2
0x6000058C  RX_IQ_3

0x600005DC
0x600005E0
0x600005E4
```

Les quatre premiers registres ont été annotés dans du reverse-engineering public comme ressemblant à du RSSI / de la puissance de corrélation, potentiellement par sous-porteuse.

### Pourquoi c’est crucial

Si ces registres évoluent continuellement avec la puissance RF reçue, ils pourraient fournir une lecture instantanée presque gratuite :

```c
uint32_t v = REG_READ(0x60000580);
```

sans cycle complet IQ_EST.

### Limite actuelle

Il n’est pas encore démontré que ces registres soient continuellement rafraîchis hors réception Wi-Fi ou hors contexte corrélateur.

### État

**Candidat numéro 1 pour une mesure RF live.**

---

# 12. IQ_EST

Registre principal identifié :

```text
0x6000057C
```

Bit :

```text
bit31 = IQ_EST DONE
```

### Intérêt

L’acquisition IQ_EST permet déjà d’obtenir une métrique exploitable.

### Limite

Elle nécessite une séquence d’acquisition et reste plus coûteuse qu’une simple lecture MMIO.

### Objectif

Remplacer autant que possible :

```text
start IQ_EST
wait DONE
read result
```

par :

```text
read live register
```

---

# 13. Registres IQ / corrélation actuellement prioritaires

| Registre | Rôle observé / supposé | Priorité |
|---|---|---:|
| `0x6000057C` | IQ_EST / DONE bit31 | élevée |
| `0x60000580` | RX_IQ_0 / corr power | très élevée |
| `0x60000584` | RX_IQ_1 / corr power | très élevée |
| `0x60000588` | RX_IQ_2 / corr power | très élevée |
| `0x6000058C` | RX_IQ_3 / corr power | très élevée |
| `0x600005DC` | résultat IQ/corrélation | élevée |
| `0x600005E0` | résultat IQ/corrélation | élevée |
| `0x600005E4` | résultat IQ/corrélation | très élevée |

---

# 14. Événements MAC / NMI

Registre candidat :

```text
0x3FF20C20
```

Cette zone est associée aux événements MAC / NMI.

Piste initiale :

```text
PWDET
  ↓
CCA
  ↓
transition FREE/BUSY
  ↓
WDEV event
  ↓
NMI
```

### Évaluation actuelle

Cette piste est moins prioritaire que :

1. PWDET ;
2. RX_IQ ;
3. noise-floor ;
4. moteur backoff.

Il reste toutefois utile de reconstruire les bits d’événements de `wDev_ProcessFiq()` pour vérifier si un événement RX pré-décodage existe.

---

# 15. Candidats actuels classés

| Candidat | Coût attendu | Ce qui est connu | Intérêt |
|---|---:|---|---:|
| **CCA status inconnu dans `0x60009xxx`** | 1 MMIO si trouvé | même banque que contrôle CCA/noise/backoff | ★★★★★ |
| Backoff WDEV | 1 lecture MMIO | moteur CSMA matériel, oracle CCA potentiel | ★★★★★ |
| `0x3FF20C18/20/24` | 1 MMIO | fabric enable/pending/status WDEV/FIQ | ★★★★★ pour tester l'hypothèse ISR |
| `0x60000580..0x6000058C` | 1 lecture MMIO chacun | `RX_IQ_0..3`, immédiatement avant `RX_GAIN_CTL` | ★★★★★ |
| `sdt_on_noise_start()` | inconnu | fonction PHY v6 explicitement couplée au noise subsystem | ★★★★★ |
| `read_hw_noisefloor()` / `0x60009B64` | très faible probable | primitive RX hardware réellement utilisée | ★★★★☆ |
| `0x600005DC/E0/E4` | 1 lecture MMIO | résultats IQ/corrélation encore non nommés | ★★★★☆ |
| `0x60009D44` bits 29/26 | 1 lecture MMIO | contrôle modifié par sniffer on/off | ★★★☆☆ |
| `ppCheckTxIdle()` / `lmacIsIdle()` | faible | état MAC/TX, pas encore relié au CCA | ★★☆☆☆ |
| PWDET / bloc `0x60000Dxx` | inconnu | fortement associé à la calibration/puissance TX | ★★☆☆☆ |
| `phy_set_sense` / `chip_v6_*_sense` | négligeable | routines v6 quasi-stubs | ★☆☆☆☆ |
| IQ_EST `0x6000057C` | acquisition nécessaire | DONE bit31 connu | ★★★★☆ |
| `ic_get_rssi()` | faible | RSSI de contexte Wi-Fi | ✗ |

---

# 16. Carte fonctionnelle provisoire

```text
                     ANTENNE
                        │
                        ▼
                    RF / LNA
                        │
           ┌────────────┼────────────┐
           │            │            │
           ▼            ▼            ▼
        PWDET       RX_IQ/CORR    NOISE FLOOR
           │            │            │
           └──────┬─────┴────────────┘
                  │
                  ▼
                 CCA
           FREE / BUSY interne
                  │
                  ▼
           MAC BACKOFF HW
                  │
                  ▼
             TX arbitration
```

Le principal objectif du reverse-engineering est de trouver **où le signal FREE/BUSY interne devient observable par logiciel**.

---

# 17. Hypothèses encore ouvertes

## H1 — un vrai bit `CCA_BUSY` existe

Forme recherchée :

```c
bool busy = REG_READ(addr) & mask;
```

Ce serait la solution idéale.

---

## H2 — PWDET expose un statut de puissance instantané

Même si aucun bit CCA direct n’est accessible, une sortie de power detector pourrait être suffisante.

---

## H3 — `RX_IQ_0..3` sont continuellement mis à jour

Cela permettrait une lecture RF live sans IQ_EST.

---

## H4 — `0x600005DC/E0/E4` contiennent une métrique encore plus directe

Ces registres sont lus dans le même environnement IQ/corrélation et doivent être décodés davantage.

---

## H5 — le compteur WDEV expose son état live

Si le backoff se fige exactement pendant BUSY, il devient un oracle de référence pour reconstruire le CCA.

---

## H6 — un événement WDEV/NMI révèle une transition RX pré-décodage

Cette piste est possible mais actuellement secondaire.

---

# 18. Pistes statiques prioritaires

Ordre de recherche recommandé :

1. **Désassembler les variantes RAM de corr power / noise floor**
   - `ram_get_corr_power`
   - `read_hw_noisefloor`
   - `ram_start_noisefloor`
   - `do_noisefloor`
   - `noise_check_loop`

2. **Reconstruire les registres WDEV/FIQ**
   - distinguer `0x3FF20C20` du véritable registre de statut `0x3FF20C24` ;
   - reconstruire les bits utilisés par `wDev_ProcessFiq()` ;
   - chercher un événement RX/CCA pré-décodage.

3. **Désassembler `rom_en_pwdet()` mais avec priorité réduite**
   - identifier tous les registres touchés ;
   - vérifier s'ils appartiennent au chemin TX power-control ;
   - rechercher une éventuelle réutilisation côté RX.

4. **Construire une table exhaustive des lectures MMIO**
   - adresse ;
   - fonction ;
   - type d’accès ;
   - masque ;
   - valeur comparée ;
   - boucle d’attente éventuelle.

5. **Chercher les mêmes registres dans le MAC/backoff**
   - corréler PHY et WDEV.

6. **Reconstruire `wDev_ProcessFiq()`**
   - bits d’événements ;
   - sources NMI ;
   - événements RX avant décodage de trame.

7. **Étudier les registres autour de**
   - `0x60009B00`
   - `0x60009B64`
   - `0x60009C28`
   - `0x60009D24`
   - `0x6000057C`
   - `0x60000580..0x6000058C`
   - `0x600005DC..0x600005E4`
   - bloc `0x60000Dxx`

---

# 19. Nouvelles découvertes — recherche web/statique du 2026-09-14

## D1 — `wDev_ProcessFiq()` distingue `0x3FF20C20` et `0x3FF20C24`

Le reverse-engineering public de `wDev_ProcessFiq()` indique la séquence suivante :

```text
1. lecture de 0x3FF20C20
2. test si zéro
3. lecture des bits de statut dans 0x3FF20C24
4. dispatch vers les handlers LMAC
```

Cela corrige la formulation précédente où `0x3FF20C20` était traité comme le candidat principal pour les bits d'événements.

Le registre à examiner en priorité pour les **flags FIQ/NMI** est maintenant :

```text
0x3FF20C24
```

Les bits déjà annotés publiquement incluent :

```text
bit 26 → MacTim1 / watchdog path
bit 27 → MacTim
bit 28 → chemin de panic/assert dans la version étudiée
```

Le handler appelle également des fonctions de transmission telles que :

```text
lmacProcessRtsStart
lmacProcessTXStartData
lmacProcessCollisions
lmacProcessAckTimeout
lmacProcessTxSuccess
lmacProcessTxRtsError
lmacProcessCtsTimeout
lmacProcessTxError
```

### Conséquence

La zone WDEV reste intéressante, mais il faut maintenant séparer :

```text
0x3FF20C20  → registre de présence/gating d'événement
0x3FF20C24  → registre de status/flags traité par wDev_ProcessFiq()
```

La recherche de `CCA_BUSY` ou d'un événement RX doit donc inclure prioritairement les bits encore non identifiés de `0x3FF20C24`.

### Niveau de confiance

**Élevé**, basé sur un reverse-engineering public détaillé de `wDev_ProcessFiq()`.

### Source

- pfalcon, miroir du wiki de reverse-engineering ESP8266, `WDev_ProcessFiq_(IoT_RTOS_SDK_0.9.9).mw`
- https://github.com/pfalcon/esp8266-re-wiki-mirror/blob/master/WDev_ProcessFiq_%28IoT_RTOS_SDK_0.9.9%29.mw

---

## D2 — PWDET paraît principalement lié au contrôle de puissance TX

Les tables de symboles de `libphy.a` placent plusieurs symboles PWDET dans le même ensemble que les routines explicites de contrôle/calibration TX :

```text
loop_pwctrl_pwdet_error_accum_high_power
loop_pwctrl_correct_atten_high_power
get_pwctrl_correct
tx_pwctrl_cal
tx_pwctrl_background
tx_pwctrl_bg_init
tx_pwctrl_pk_num
tx_pwctrl_set_chan_flag
meas_tone_pwr_db
```

Cette nomenclature fournit une indication beaucoup plus forte sur le rôle du bloc PWDET que le simple voisinage de `rom_en_pwdet()` dans la table ROM.

### Nouvelle interprétation

```text
TX signal
   │
   ▼
PWDET
   │
   ▼
mesure / erreur de puissance
   │
   ▼
TX power-control / calibration
```

plutôt que l'hypothèse précédente :

```text
RX RF → PWDET → CCA_BUSY
```

### Conséquence pour RF2400

La priorité de recherche change :

```text
1. RX_IQ / corr_power
2. WDEV backoff oracle
3. noise-floor / read_hw_noisefloor
4. WDEV/FIQ status 0x3FF20C24
5. PWDET
```

PWDET n'est pas éliminé, car le silicium peut réutiliser des blocs analogiques, mais il ne doit plus guider la recherche principale sans preuve de lecture côté RX.

### Niveau de confiance

**Moyen à élevé** pour le lien PWDET ↔ TX power-control.  
**Non démontré** que PWDET soit totalement absent du chemin RX.

### Sources

- tables de symboles `libphy.a` publiées par la communauté ESP8266 ;
- esp-open-rtos `allsymbols.rename`.

---

## D3 — existence d'une famille `sense` distincte

Les symboles publics de `libphy.a` contiennent également :

```text
phy_set_sense
chip_v6_set_sense
chip_v6_get_sense
```

et `chip_v6_set_sense` / `chip_v6_get_sense` apparaissent dans :

```text
phy_chip_v6_unused.o
```

Un firmware tiers qui retire des fonctions SDK inutilisées fournit même un stub vide de `chip_v6_set_sense()` pour satisfaire le linker.

### Interprétation prudente

Le mot `sense` mérite une analyse statique, mais cette piste paraît **moins directement liée au CCA live** que son nom ne le suggère, puisque certaines versions peuvent apparemment fonctionner avec un stub.

Il faut néanmoins retrouver les appels à :

```text
phy_set_sense()
```

et déterminer si elle finit par programmer les mêmes champs que :

```text
rom_chip_v5_sense_backoff()
```

### Niveau de confiance

**Élevé** pour l'existence des symboles.  
**Faible** concernant leur rôle exact.

---

# 20. Nouvelles découvertes — v0.3

## D4 — la piste `chip_v6_*_sense` est pratiquement éliminée

Dans la table des symboles de `phy_chip_v6_unused.o` :

```text
0x00000000  chip_v6_set_sense
0x00000004  chip_v6_get_sense
0x00000008  chip_v6_unset_chanfreq
```

Les deux routines `set_sense` et `get_sense` occupent donc chacune au plus quelques octets avant le symbole global suivant.

Cette taille est compatible avec une routine triviale du type :

```c
return;
```

ou :

```c
return constant;
```

Un firmware tiers utilisant le même SDK fournit d'ailleurs explicitement :

```c
void chip_v6_set_sense(void)
{
    // ret.n
}
```

pour satisfaire le linker.

### Conséquence

Le mot `sense` ne doit plus être interprété comme une preuve de présence d'un accès `CCA_BUSY`.

La famille :

```text
phy_set_sense
chip_v6_set_sense
chip_v6_get_sense
```

est rétrogradée au niveau **faible priorité**.

### Niveau de confiance

**Élevé** pour le caractère trivial de `chip_v6_set_sense()`.  
**Moyen à élevé** pour `chip_v6_get_sense()`.

### Sources

- 41J — table des symboles `libphy.a` :
  https://41j.com/blog/2015/01/esp8266-sdk-library-symbols/
- ESPTerm — stub explicite `chip_v6_set_sense()` :
  https://git.ondrovo.com/espterm/espterm-firmware/src/commit/d3130c43dab08264dfd49fc791d81bfff872b023/user/user_main.c

---

## D5 — `read_hw_noisefloor()` est une primitive distincte et très compacte

Dans `phy_chip_v6.o`, les symboles de la chaîne noise floor sont placés ainsi :

```text
0x1440  ram_get_noisefloor
0x1460  get_noisefloor_sat
0x1498  ram_set_noise_floor
0x14F8  ram_start_noisefloor
0x1544  read_hw_noisefloor
0x1580  noise_check_loop
0x171C  noise_init
```

En prenant les symboles globaux suivants comme bornes supérieures, on obtient :

```text
ram_get_noisefloor     <= 0x20  octets avant le prochain symbole global
ram_start_noisefloor   <= 0x4C
read_hw_noisefloor     <= 0x3C
noise_check_loop       <= 0x19C
```

> Attention : ces valeurs sont des bornes entre symboles globaux, pas des tailles ELF garanties, car des symboles locaux peuvent exister entre eux.

### Interprétation

La séparation des fonctions est très instructive :

```text
ram_start_noisefloor()
        │
        ▼
 acquisition / déclenchement
        │
        ▼
read_hw_noisefloor()
        │
        ▼
 lecture hardware courte
        │
        ▼
noise_check_loop()
        │
        ▼
 validation / filtrage / adaptation
```

Cette architecture rend `read_hw_noisefloor()` beaucoup plus intéressante que le seul appel générique `ram_get_noisefloor()`.

### Rapport avec `0x60009B64`

Le ROM v5 `rom_get_noisefloor()` lit déjà directement :

```text
0x60009B64
```

et le reverse-engineering public décrit ce registre comme le niveau de bruit du récepteur.

Le nouveau point important est que le PHY v6 possède lui aussi une routine explicitement nommée :

```text
read_hw_noisefloor
```

ce qui indique qu'il existe une notion claire de **lecture hardware immédiate**, distincte du traitement périodique du bruit.

### Conséquence RF2400

Nouvelle priorité :

```text
read_hw_noisefloor()
```

doit être analysée avant de lancer des expériences IQ_EST complexes.

Si elle se réduit essentiellement à :

```c
read register
extract field
sign/scale
return
```

elle peut constituer une mesure RF extrêmement bon marché.

### Niveau de confiance

**Élevé** pour l'existence et la séparation fonctionnelle.  
**Non confirmé** que la valeur soit suffisamment rapide pour suivre une modulation OOK.

### Sources

- 41J — symboles `phy_chip_v6.o` :
  https://41j.com/blog/2015/01/esp8266-sdk-library-symbols/
- pvvx / communauté ESP8266 — `0x60009B64` = niveau de bruit RX :
  https://esp8266.ru/forum/threads/dokumentacija-na-esp8266-na-nashem-sajte-popolnjaemyj-razdel.5/

---

## D6 — le noise floor est intégré au fonctionnement normal du packet processor

`libpp.a` expose dans `pp.o` :

```text
noise_now
pp_disable_noise_timer
pp_enable_noise_timer
pp_noise_test
```

et les maps/symboles SDK montrent également :

```text
NoiseTimerInterval
pend_flag_noise_check
```

Le nom `pp_noise_test` apparaît dans de nombreuses traces réelles ESP8266 depuis le contexte timer système, ce qui confirme qu'il s'agit d'une routine active en fonctionnement normal et pas uniquement d'un outil ATE/calibration.

### Architecture probable

```text
NoiseTimerInterval
       │
       ▼
pp_enable_noise_timer()
       │
       ▼
   timer ETS
       │
       ▼
pp_noise_test()
       │
       ├── vérifie état MAC / power-management
       ├── déclenche ou demande une mesure de bruit
       └── pend_flag_noise_check
                    │
                    ▼
          PHY noise subsystem
                    │
                    ▼
              noise_now
```

La relation exacte entre chaque flèche doit encore être prouvée par désassemblage, mais la coexistence de ces symboles établit qu'un **pipeline logiciel périodique de noise measurement** existe.

### Pourquoi c'est important

Cela change notre modèle :

Avant :

```text
noise floor = primitive de calibration PHY occasionnelle
```

Maintenant :

```text
noise floor = métrique maintenue périodiquement par le Wi-Fi normal
```

Il devient donc plausible que :

```text
noise_now
```

soit une valeur cache/logicielle utile pour observer l'environnement RF, même si elle sera probablement beaucoup trop lente pour reproduire directement une DATA OOK rapide.

### Conséquence

Deux pistes noise doivent désormais être distinguées :

```text
A) read_hw_noisefloor()
   → valeur hardware immédiate
   → intérêt pour RF2400

B) noise_now / pp_noise_test
   → valeur périodique / filtrée
   → intérêt surtout pour comprendre l'algorithme CCA/adaptation
```

### Niveau de confiance

**Élevé** pour l'existence du pipeline périodique.  
**Moyen** pour le rôle exact de `noise_now`.

### Sources

- 41J — `pp.o` :
  https://41j.com/blog/2015/01/esp8266-sdk-library-symbols/
- esp-open-rtos / `allsymbols.rename` :
  https://git.neulandlabor.de/j3d1/esp-open-rtos/src/commit/f5bbff8b87afea6d2e7cd324f50d9640fe9d45f9/lib/allsymbols.rename
- ancien linker map montrant `NoiseTimerInterval` et `pend_flag_noise_check` :
  https://pastebin.com/egnFgqD9

---

## D7 — le PHY v6 remplace explicitement plusieurs entrées ROM v5

La table `phy_func_tab` ROM possède notamment :

```text
+040 get_noisefloor
+088 set_noise_floor
+100 start_noisefloor
```

et le SDK remplace ces entrées par :

```text
ram_get_noisefloor
ram_set_noise_floor
ram_start_noisefloor
```

De même, plusieurs primitives de calibration RX/IQ sont remplacées par des variantes RAM.

### Conséquence

Pour RF2400, il faut désormais distinguer deux objectifs :

1. **comprendre le hardware** :
   - les fonctions ROM v5 sont excellentes car elles sont petites et faciles à désassembler ;

2. **comprendre le comportement réel d'un ESP8266 SDK v6** :
   - les variantes RAM sont plus pertinentes, car Espressif les substitue volontairement au démarrage.

Le registre hardware peut rester identique, mais l'échelle, le filtrage ou les conditions de validité peuvent avoir changé.

### Niveau de confiance

**Élevé.**

### Sources

- pvvx `rom_phy.h` :
  https://github.com/pvvx/MinEspSDKLib/blob/master/include/bios/rom_phy.h
- documentation communautaire de la table ROM :
  https://esp8266.ru/forum/threads/dokumentacija-na-esp8266-na-nashem-sajte-popolnjaemyj-razdel.5/

---

## D8 — `ram_get_corr_power()` est nettement moins triviale que la primitive ROM

Dans `phy_chip_v6.o` :

```text
ram_get_corr_power = 0x09DC
prochain symbole global de code visible = 0x0AD0
```

soit une fenêtre maximale d'environ :

```text
0xF4 = 244 octets
```

avant le symbole global suivant.

Cela ne donne pas sa taille ELF exacte, mais montre que la variante RAM peut être sensiblement plus élaborée que le petit chemin ROM que nous avions déjà étudié.

### Interprétation

Il faut éviter l'hypothèse :

```text
ram_get_corr_power == quatre simples REG_READ()
```

Le SDK v6 peut ajouter :

- sélection de mode ;
- normalisation ;
- moyenne ;
- saturation ;
- dépendance au gain RX ;
- compensation.

### Conséquence

Pour rechercher un signal RF brut rapide, les registres :

```text
0x60000580
0x60000584
0x60000588
0x6000058C
```

restent plus intéressants que l'appel complet à `ram_get_corr_power()`.

### Niveau de confiance

**Moyen**, car la taille est inférée entre symboles globaux et non à partir d'une table de tailles ELF complète.

---

# 21. Nouvelles découvertes — v0.4

## D9 — découverte de `sdt_on_noise_start()` dans le cœur PHY RX/noise

La table de symboles de `phy_chip_v6.o` contient :

```text
0x1928  target_power_backoff
0x198C  sdt_on_noise_start
0x1A34  chip_v6_set_chan_rx_cmp
```

La même unité objet contient aussi :

```text
do_noisefloor
ram_get_noisefloor
ram_set_noise_floor
ram_start_noisefloor
read_hw_noisefloor
noise_check_loop
noise_init
ram_get_corr_power
start_dig_rx
stop_dig_rx
phy_bb_rx_cfg
```

### Pourquoi c'est important

Le nom :

```text
sdt_on_noise_start
```

établit au minimum un couplage logiciel entre une entité appelée `sdt` et le démarrage de la procédure noise-floor.

Il serait tentant d'interpréter `SDT` comme « Signal Detection Threshold », mais **aucune source Espressif retrouvée à ce stade ne confirme cette expansion**.

La conclusion sûre est donc seulement :

```text
noise measurement start
        │
        ▼
sdt_on_noise_start()
        │
        ▼
modification d'un état/paramètre SDT encore inconnu
```

### Taille approximative

Entre les deux symboles globaux :

```text
0x198C → 0x1A34
```

la fenêtre maximale est :

```text
0xA8 = 168 octets
```

Ce n'est pas une taille ELF certaine : des symboles locaux peuvent se trouver dans cet intervalle.

### Conséquence RF2400

Cette fonction devient une des meilleures cibles de désassemblage statique.

Si elle modifie un registre de seuil de détection lorsqu'une mesure de bruit commence, elle pourrait révéler :

- le registre de seuil utilisé par le détecteur RX ;
- la relation entre noise floor et détection de porteuse ;
- éventuellement la logique utilisée en amont du CCA.

### Niveau de confiance

**Élevé** pour l'existence et le placement de la fonction.  
**Faible / hypothétique** pour la signification de l'acronyme SDT.

### Sources

- 41J, table des symboles `phy_chip_v6.o` :
  https://41j.com/blog/2015/01/esp8266-sdk-library-symbols/
- esp-open-rtos, export du symbole :
  https://git.neulandlabor.de/j3d1/esp-open-rtos/src/commit/f5bbff8b87afea6d2e7cd324f50d9640fe9d45f9/lib/allsymbols.rename

---

## D10 — `read_hw_noisefloor()` est utilisée dans un chemin RX polling

Un désassemblage communautaire récent d'une fonction Espressif nommée :

```c
do_rx_poll(uint rx_rate)
```

est présenté comme une routine de réception Wi-Fi de debug fonctionnant en polling.

Au début du chemin, le code reconstruit fait notamment :

```c
esp_rx_valid = 0;

REG(0x60033800) = 0;
REG(0x60035004) = 0;
REG(0x60033C40) |= 0xC;

x = read_hw_noisefloor();
x *= 10;

if (x < 0)
    x += 3;
```

### Ce que cela prouve

`read_hw_noisefloor()` n'est pas uniquement un sous-programme caché d'une calibration périodique.

Il est appelé directement dans un contexte de **réception active par polling**.

### Ce que cela ne prouve pas

Cela ne démontre toujours pas que :

```text
read_hw_noisefloor()
```

se rafraîchit assez vite pour suivre une modulation OOK.

La mesure peut encore être :

- filtrée ;
- moyennée ;
- figée pendant certaines phases ;
- mise à jour sur une fenêtre de temps relativement longue.

### Nouvelle hypothèse de travail

Il faut distinguer :

```text
read_hw_noisefloor()
    = primitive RX directement utilisable

vitesse réelle de mise à jour
    = encore inconnue
```

### Niveau de confiance

**Élevé** pour l'appel observé dans le désassemblage publié.  
**Inconnu** pour la bande passante temporelle de la métrique.

### Source

- Arduino.ru, publication d'un désassemblage Ghidra de `do_rx_poll()` issu de code de debug Espressif :
  https://forum.arduino.ru/t/kto-kakim-ii-polzuetsya-dlya-napisaniya-sketchej/17829?page=54

---

## D11 — carte contiguë du bloc `RX_IQ` / gain RX

Le header reverse-engineeré `esp/phy_regs.h` place les registres ainsi :

```text
PHY_BASE = 0x60000500

0x60000560  TX_DPD
...
0x6000057C  IQ_EST
0x60000580  RX_IQ_0
0x60000584  RX_IQ_1
0x60000588  RX_IQ_2
0x6000058C  RX_IQ_3
0x60000590  RX_GAIN_CTL
0x60000594  PBUS_CTL_0
0x60000598  PBUS_CTL_1
0x6000059C  PBUS_CTL_2
0x600005A0  PBUS_CTL_3
...
0x600005B8  BB_CTL_0
0x600005BC  BB_CTL_1
0x600005C0  BB_CTL_2
0x600005C4  BB_CTL_3
```

Les auteurs annotent `RX_IQ_0..3` comme ressemblant à du RSSI, possiblement par sous-porteuse OFDM, et `0x60000590` comme contrôle du gain RX.

### Nouvelle conséquence

La contiguïté :

```text
IQ_EST → RX_IQ_0..3 → RX_GAIN_CTL
```

renforce nettement l'interprétation des quatre `RX_IQ` comme métriques appartenant au chemin RX immédiat plutôt qu'à une structure MAC ou à une valeur logicielle historique.

Cela suggère également qu'une analyse de `RX_IQ` sans connaître le gain RX peut être trompeuse.

Pour toute future corrélation statique ou matérielle, il faudra donc considérer le couple :

```text
RX_IQ_0..3
+
RX_GAIN_CTL
```

et non uniquement les quatre valeurs RX_IQ.

### Niveau de confiance

**Élevé** pour les adresses et la contiguïté.  
**Moyen** pour l'interprétation exacte RSSI/per-subcarrier.

### Source

- esp-open-rtos, `core/include/esp/phy_regs.h` :
  https://git.neulandlabor.de/j3d1/esp-open-rtos/src/commit/c3e3fb30d93082c8c343734c063b917c32b01625/core/include/esp/phy_regs.h

---

## D12 — `0x60009D44` contrôle une partie du chemin RX/sniffer

Le reverse-engineering de `wdev_go_sniffer()` indique :

```c
REG(0x60009D44) &= 0xDBFFFFFF;
```

et la sortie de sniffer effectue :

```c
REG(0x60009D44) |= 0x24000000;
```

Or :

```text
0x24000000 = bit29 | bit26
```

Donc le passage en mode promiscuous efface les bits :

```text
29
26
```

et le retour au mode normal les rétablit.

### Conclusion sûre

Les bits 29 et 26 de :

```text
0x60009D44
```

font partie d'une configuration matérielle modifiée par le mode sniffer/promiscuous.

Ils sont donc liés, directement ou indirectement, au chemin de réception MAC/PHY.

### Ce qui reste inconnu

On ne peut pas encore leur attribuer précisément :

- filtrage de trames ;
- validation RX ;
- décodeur PHY ;
- BSSID filtering ;
- CCA ;
- ou autre fonction.

### Intérêt pour notre carte de registres

La zone déjà connue :

```text
0x60009C28  backoff/CCA config
0x60009D24  backoff/CCA config
0x60009D44  RX/sniffer config
```

commence à former un cluster cohérent de registres Wi-Fi/MAC-RX autour de `0x60009Cxx–0x60009Dxx`.

### Niveau de confiance

**Élevé** pour les opérations sur les bits.  
**Inconnu** pour leur nom fonctionnel précis.

### Source

- reverse-engineering pvvx / communauté ESP8266 :
  https://esp8266.ru/forum/threads/dokumentacija-na-esp8266-na-nashem-sajte-popolnjaemyj-razdel.5/

---

## D13 — petites primitives ROM noise-floor confirmées dans le linker officiel

Le linker officiel Espressif confirme :

```text
0x40006394  rom_get_noisefloor
0x40006830  rom_set_noise_floor
0x40006874  rom_start_noisefloor
0x400068B4  rom_start_tx_tone
```

On en déduit des fenêtres maximales très petites :

```text
rom_set_noise_floor:
0x40006830 → 0x40006874
<= 0x44 octets

rom_start_noisefloor:
0x40006874 → 0x400068B4
<= 0x40 octets
```

Comme précédemment, il s'agit de fenêtres entre symboles connus, pas forcément de tailles ELF exactes.

### Conséquence

Les deux primitives ROM sont suffisamment compactes pour qu'un désassemblage exact soit particulièrement rentable.

Une fois le vrai dump binaire ROM disponible comme binaire exploitable, ces deux fonctions devraient être analysées en priorité avec :

```text
rom_get_noisefloor
rom_set_noise_floor
rom_start_noisefloor
rom_chip_v5_sense_backoff
```

afin de reconstruire la chaîne complète :

```text
noise measurement
      ↓
noise floor value
      ↓
threshold/configuration
      ↓
CCA/backoff
```

### Source

- Espressif, `ESP8266_NONOS_SDK/ld/eagle.rom.addr.v6.ld` :
  https://github.com/espressif/ESP8266_NONOS_SDK/blob/master/ld/eagle.rom.addr.v6.ld

---

# 22. Nouvelles découvertes — v0.5 : CCA vs WDEV/FIQ/NMI

## D14 — registre officiel d'activation des interruptions WDEV : `0x3FF20C18`

Le header Espressif ESP8266 définit explicitement :

```c
#define INT_ENA_WDEV        0x3ff20c18
#define WDEV_TSF0_REACH_INT BIT(27)
```

Le code RTOS SDK utilise ce registre de la manière attendue :

```c
REG_WRITE(
    INT_ENA_WDEV,
    REG_READ(INT_ENA_WDEV) | WDEV_TSF0_REACH_INT
);
```

pour activer l'événement TSF0.

### Conclusion

`0x3FF20C18` est désormais un point fixe de notre carte :

```text
0x3FF20C18
    │
    └── masque ENABLE des événements WDEV/MAC
```

Si un événement matériel CCA est exposé au CPU par la fabric WDEV, son bit d'activation devrait logiquement apparaître dans ce registre ou dans un mécanisme très proche.

### Important

Cela ne signifie pas que `BIT(27)` est lié au CCA :

```text
BIT27 = TSF0 timer event
```

est au contraire un événement déjà identifié et donc éliminé de la recherche CCA.

### Sources

- Espressif ESP8266 RTOS SDK, `eagle_soc.h`
- étude des timers/WDEV du SDK :
  https://github-wiki-see.page/m/mriksman/esp-idf-homekit/wiki/Timers

---

## D15 — trio WDEV `0x3FF20C18 / 0x3FF20C20 / 0x3FF20C24`

Le vieux reverse-engineering de `wDev_ProcessFiq()` établit la séquence suivante :

```text
wDev_ProcessFiq()
    │
    ├── lit 0x3FF20C20
    │      └── sortie/traitement selon zéro/non-zéro
    │
    └── lit 0x3FF20C24
           └── teste les flags et dispatch les handlers
```

Les flags publiquement décodés incluent notamment :

```text
bit 26 → MacTim1 / watchdog path
bit 27 → MacTim / TSF timer path
bit 28 → panic / état anormal dans cette version
```

Le même reverse-engineering mentionne ensuite des tests des bits :

```text
3
4
et d'autres encore non complètement identifiés
```

### Ambiguïté importante

Une autre étude du timer WDEV rapporte que lorsque le timer MAC expire :

```text
bit27 de 0x3FF20C20 = 1
```

alors que le reverse-engineering du handler dit que les flags sont lus dans :

```text
0x3FF20C24
```

Il ne faut donc **pas encore attribuer définitivement** les rôles :

```text
RAW
MASKED
PENDING
ACK/W1C
```

à `0x20` et `0x24`.

Le modèle prudent est :

```text
0x3FF20C18  enable
0x3FF20C20  pending/raw/gating ? 
0x3FF20C24  status/dispatch ?
```

Les deux derniers rôles exacts restent à reconstruire sur la même version de SDK.

### Pourquoi cela rapproche du CCA

Une éventuelle interruption CCA devrait probablement produire :

```text
bit enable dans C18
       +
bit pending/status dans C20/C24
       +
branche dans wDev_ProcessFiq()
```

Cette chaîne donne désormais une méthode précise pour prouver ou éliminer l'hypothèse `CCA interrupt`.

### Source

- pfalcon / ESP8266 reverse-engineering wiki :
  https://github.com/pfalcon/esp8266-re-wiki-mirror/blob/master/WDev_ProcessFiq_%28IoT_RTOS_SDK_0.9.9%29.mw

---

## D16 — `wDev_ProcessFiq()` traite aussi un événement RX réussi

Le reverse-engineering ancien de SDK 0.9.9 avait surtout identifié des handlers TX/timers :

```text
lmacProcessRtsStart
lmacProcessTXStartData
lmacProcessCollisions
lmacProcessAckTimeout
lmacProcessTxSuccess
lmacProcessTxRtsError
lmacProcessCtsTimeout
lmacProcessTxError
```

Cela pouvait laisser penser que WDEV/FIQ était presque exclusivement TX.

Une trace beaucoup plus récente d'un ESP8266 NonOS fournit cependant les symboles et les lignes internes Espressif suivantes :

```text
wDev_ProcessFiq
  ↓
wDev_ProcessRxSucData       wdev.c:603
  ↓
wDev_IndicateFrame         wdev.c:324
  ↓
lmacRxDone                 lmac.c:1740
```

### Conclusion

La FIQ WDEV est réellement une interruption **MAC multi-événements** incluant au moins :

```text
timers
TX
collisions
RX frame success
```

Cela rend techniquement possible qu'un autre événement PHY/MAC — éventuellement lié au CCA — soit multiplexé dans la même fabric.

### Mais

L'événement RX observé est une **réception de trame réussie**, donc beaucoup plus haut niveau qu'une simple transition d'énergie/CCA.

Il ne faut pas confondre :

```text
RX_SUCCESS interrupt
```

avec :

```text
CCA_BUSY edge interrupt
```

### Source

- esp8266/Arduino issue #9029, backtrace décodée avec chemins source internes Espressif :
  https://github.com/esp8266/Arduino/issues/9029

---

## D17 — Espressif nomme des événements WDEV queue-specific : `Q2_RTS_INT`, `Q0_TX_COMPLETE`

Les release notes officielles du SDK 3.0.3 contiennent le correctif :

```text
fix(pp): Wi-Fi tx hangs when Q2_RTS_INT
         and Q0_TX_COMPLETE come at the same time
```

### Ce que cela apporte

Cela prouve que l'espace d'événements WDEV/MAC contient des interruptions nommées par Espressif du type :

```text
Qn_RTS_INT
Qn_TX_COMPLETE
```

et que plusieurs flags peuvent être actifs simultanément.

Le handler `wDev_ProcessFiq()` est donc bien un **dispatcher de bits d'événement hardware**, et pas seulement un wrapper logiciel autour de timers.

### Conséquence pour la recherche CCA

La stratégie devient :

```text
mapper tous les événements déjà connus
        ↓
retirer leurs bits de C18/C20/C24
        ↓
examiner les bits restants
        ↓
chercher appel / condition liée à CCA, RX, collision ou backoff
```

### Source

- Espressif ESP8266_NONOS_SDK v3.0.3 release notes :
  https://github.com/espressif/ESP8266_NONOS_SDK/releases

---

## D18 — aucun `CCA_INT` public retrouvé à ce stade

Les recherches croisées ont porté notamment sur :

```text
CCA_INT
CCA_BUSY_INT
carrier sense interrupt
clear channel interrupt
wDev + CCA
lmac + CCA
```

ainsi que sur :

- symboles `libpp.a` publics ;
- symboles `libphy.a` publics ;
- anciens reverse-engineerings WDEV ;
- headers officiels ESP8266 ;
- release notes Espressif.

### Résultat

À ce stade, aucune source publique retrouvée ne donne un symbole du type :

```text
CCA_INT
CCA_BUSY_INT
CCA_FREE_INT
CHANNEL_BUSY_INT
```

pour l'ESP8266 Wi-Fi MAC.

### Ce que l'on peut conclure

**Fait :**

```text
aucune preuve de CCA edge interrupt n'a encore été trouvée
```

**Pas un fait :**

```text
"l'ESP8266 n'a définitivement aucune interruption CCA"
```

L'absence de symbole public n'est pas une preuve d'absence hardware.

### Inférence architecturale

Le modèle le plus économique matériellement reste :

```text
PHY CCA
  │
  ├── état BUSY/FREE interne
  │
  └── directement consommé par
      MAC backoff/arbitration
```

sans provoquer une NMI à chaque transition.

Une interruption à chaque changement CCA pourrait produire un taux d'interruptions très élevé sur un canal 2.4 GHz occupé ; ce serait donc moins naturel qu'un simple bit d'état interne.

### Niveau de confiance

- **Élevé** : aucune interruption CCA publique identifiée jusqu'ici.
- **Moyen** : probablement pas d'interruption CCA edge dédiée.
- **Toujours ouvert** : un bit de statut CCA MMIO peut parfaitement exister sans interruption.

---

## D19 — séparation désormais claire : registre CCA vs fabric d'interruptions

Il est important de ne plus mélanger deux banques différentes.

### Banque PHY / CCA

```text
0x60009B00 bit28
    CCA enable/disable confirmé

0x60009B64
    noise floor

0x60009C28 [16:10]
    configuré par rom_chip_v5_sense_backoff()

0x60009D24 [7:1]
    configuré par rom_chip_v5_sense_backoff()

0x60009D44 bits29/26
    modifiés par sniffer on/off
```

Le **vrai `CCA_BUSY`** est encore recherché dans cette famille ou dans une banque MAC adjacente.

### Fabric WDEV / interrupt

```text
0x3FF20C18
    interrupt enable mask

0x3FF20C20
    pending/raw/gating ?  (rôle exact à préciser)

0x3FF20C24
    status/dispatch ?     (rôle exact à préciser)
```

### MAC CSMA/backoff

```text
0x3FF20DC0 [21:12]
    valeur / compteur backoff queue 0

0x3FF20DC4 [31:30]
    commande/état TX queue 0
```

### Modèle de travail

```text
           PHY
            │
            ▼
      [ CCA_BUSY ? ]       ← cible n°1
            │
            ▼
      MAC backoff HW
            │
       ┌────┴─────┐
       │          │
   freeze       TX allowed
       │
       ▼
 WDEV events éventuels
       │
       ▼
C18 / C20 / C24            ← cible n°2 : savoir si CCA y remonte
       │
       ▼
wDev_ProcessFiq()
```

---

## D20 — `ppCheckTxIdle()` n'est pas encore un substitut CCA

`libpp.a` exporte :

```text
ppCheckTxIdle
pp_enable_idle_timer
pp_disable_idle_timer
pp_try_enable_idle_timer
pp_tx_idle_timeout
```

Des expérimentations communautaires anciennes ont observé que `ppCheckTxIdle()` corrélait parfois avec l'activité radio lors de lectures ADC, mais pas de façon parfaitement fiable.

### Interprétation

Le nom et le contexte indiquent plutôt :

```text
TX/MAC idle
```

que :

```text
RF channel clear
```

Il reste utile de le désassembler parce qu'il peut lire un bit MAC hardware intéressant, mais il ne faut pas le considérer comme `CCA_FREE` sans preuve.

### Nouvelle cible associée

Les symboles :

```text
lmacIsIdle
is_lmac_idle
```

sur les générations Espressif suivantes indiquent qu'il existe bien une notion interne d'état LMAC idle.

La prochaine étape statique est de déterminer si la version ESP8266 :

- teste uniquement les files TX / DMA ;
- teste RX/TX actif ;
- ou incorpore réellement l'état du médium/CCA.

### Sources

- table de symboles historique `libpp.a` :
  https://41j.com/blog/2015/01/esp8266-sdk-library-symbols/
- observation communautaire ancienne de `ppCheckTxIdle()` :
  https://esp8266.com/viewtopic.php?f=13&t=272

---

## D21 — comparaison architecturale avec un MAC Espressif ultérieur

**Cette section est uniquement comparative, pas une preuve ESP8266.**

Un reverse-engineering récent du MAC ESP32-C3 retrouve dans `wDev_ProcessFiq()` des événements :

```text
RX frame
TX complete
TX collision
TBTT
TSF timers
```

avec un registre d'événements MAC central.

Aucun événement CCA edge n'est mentionné dans la cartographie publiée.

### Intérêt

Cette architecture ultérieure est cohérente avec notre hypothèse ESP8266 :

```text
CCA → consommé par MAC/backoff
```

alors que la CPU reçoit surtout des événements de niveau plus élevé :

```text
RX complete
TX complete
collision
timers
```

### Limite

Les adresses et masques ESP32-C3 ne doivent **jamais** être appliqués à l'ESP8266.

### Source comparative

- LabWired, reverse-engineering ESP32-C3 MAC:
  https://docs.labwired.com/esp32c3_wifi_mac_bridge/

---

## D22 — plan statique spécifique pour prouver ou éliminer `CCA interrupt`

La recherche est maintenant suffisamment resserrée pour utiliser une procédure déterministe.

### Étape 1 — reconstruire tous les writes à `0x3FF20C18`

Pour chaque fonction SDK :

```text
REG_SET_BIT(0x3FF20C18, mask)
REG_CLR_BIT(0x3FF20C18, mask)
```

relever :

```text
mask
caller
événement associé
```

On doit obtenir une table :

| bit | enable par | événement |
|---:|---|---|
| 27 | TSF/MacTim | connu |
| ... | Q0/Q1/Q2/Q3 | TX |
| ... | RX path | RX |
| ... | inconnu | candidat |

### Étape 2 — reconstruire tous les tests de `0x3FF20C24`

Dans `wDev_ProcessFiq()` :

```text
read status
AND mask
branch
call handler
```

Chaque masque peut être relié à un événement.

### Étape 3 — identifier le bit RX success

Les versions récentes prouvent qu'un chemin :

```text
wDev_ProcessRxSucData()
```

existe dans la FIQ.

Trouver son masque permettra de retirer un candidat important.

### Étape 4 — identifier les événements TX queue

Associer :

```text
Q2_RTS_INT
Q0_TX_COMPLETE
collision
ACK_TIMEOUT
CTS_TIMEOUT
TX_ERROR
```

aux bits exacts.

### Étape 5 — examiner les bits encore orphelins

Pour chaque flag non identifié :

```text
est-il activé en permanence ?
est-il associé à RX enable ?
est-il configuré par sense_backoff ?
est-il lu/clear lors des transitions RX ?
```

C'est là qu'un éventuel événement CCA pourrait apparaître.

### Étape 6 — en parallèle, trouver le `CCA_BUSY` MMIO

Même si aucune ISR n'existe, le meilleur résultat reste :

```c
static inline bool cca_busy(void)
{
    return REG_READ(CCA_STATUS_REG) & CCA_BUSY_MASK;
}
```

La recherche doit donc continuer dans :

```text
0x60009Bxx
0x60009Cxx
0x60009Dxx
```

en donnant la priorité aux **lectures** de bits effectuées par :

```text
lmacProcessCollisions
wDev_EnableTransmit
wDev_ProcessFiq
noise / SDT routines
RX enable/disable
```

---

# 23. Nouvelles découvertes — v0.6 : séparation PHY CCA / MAC WDEV

## D23 — les trois fonctions CCA restent ROM dans la table PHY runtime étudiée

Le reverse-engineering de la table retournée par :

```c
phy_get_romfuncs()
```

situe la table à :

```text
0x3FFFC734
```

avec notamment :

```text
+004  0x400060D0  rom_chip_v5_disable_cca
+008  0x400060EC  rom_chip_v5_enable_cca
+012  0x4000610C  rom_chip_v5_sense_backoff
```

Dans **la même table**, plusieurs fonctions voisines sont remplacées par le SDK par des variantes RAM :

```text
+036  ram_get_fm_sar_dout
+040  ram_get_noisefloor
+072  ram_rxiq_get_mis
+088  ram_set_noise_floor
+100  ram_start_noisefloor
+116  ram_tx_mac_disable
+120  ram_tx_mac_enable
+124  ram_ana_inf_gating_en
+136  ram_chip_v6_rx_init
...
```

### Point nouveau

Dans la version de SDK analysée par pvvx, les trois entrées CCA ne sont **pas** remplacées par des routines `ram_*`.

Le chemin CCA reste donc :

```text
SDK PHY v6
   │
   └── table phy_func_tab
          │
          ├── CCA disable → ROM 0x400060D0
          ├── CCA enable  → ROM 0x400060EC
          └── sense/backoff → ROM 0x4000610C
```

alors que le noise floor et plusieurs fonctions RX/calibration sont patchés en RAM.

### Pourquoi c'est très important

Cela rend le désassemblage ROM déjà effectué beaucoup plus pertinent :

```text
0x60009B00 bit28
0x60009C28 [16:10]
0x60009D24 [7:1]
```

ne sont pas simplement des vestiges d'une ancienne PHY v5 : dans le SDK étudié, ces primitives ROM semblent toujours être les implémentations appelées via la table PHY.

### Limite

Cette conclusion est certaine pour la table runtime publiée pour l'ancien SDK analysé. Elle ne prouve pas que **toutes** les révisions ultérieures de `libphy.a` aient conservé exactement la même politique de remplacement.

### Niveau de confiance

**Élevé** pour le SDK/table analysé.  
**Moyen** pour les versions NonOS beaucoup plus récentes.

### Sources

- pvvx, documentation/reverse-engineering de `phy_func_tab` :
  https://esp8266.ru/forum/threads/dokumentacija-na-esp8266-na-nashem-sajte-popolnjaemyj-razdel.5/
- pvvx, `rom_phy.h` :
  https://github.com/pvvx/MinEspSDKLib/blob/master/include/bios/rom_phy.h
- Espressif, linker ROM officiel :
  https://github.com/espressif/ESP8266_NONOS_SDK/blob/master/ld/eagle.rom.addr.v6.ld

---

## D24 — `0x3FF20Cxx` est confirmé comme banque MAC/WDEV, pas comme banque PHY CCA

Le guide API officiel ESP8266 NonOS d'Espressif utilise :

```c
#define WDEV_NOW() REG_READ(0x3FF20C00)
```

dans son exemple de timer hardware.

Cela établit officiellement que :

```text
0x3FF20C00
```

est une source de temps WDEV/MAC accessible directement.

Le reverse-engineering communautaire place aussi dans la même banque :

```text
0x3FF20CB0  wDev_SetWaitingQueue()
0x3FF20CC0  wDev_GetTxqCollisions()
```

### Carte fonctionnelle désormais plus robuste

```text
0x3FF20C00    MAC/WDEV time
     │
0x3FF20C18    WDEV interrupt enable
0x3FF20C20    interrupt/event raw/pending ? 
0x3FF20C24    interrupt/event status/dispatch ?
     │
...
0x3FF20CB0    waiting queue state
0x3FF20CC0    TX collision state
```

### Conséquence

Cette banque est clairement orientée :

```text
MAC timing
queues
collisions
interrupt fabric
```

et non vers les mesures analogiques/PHY.

Cela rend plus probable l'architecture :

```text
0x60009xxx
    │
    └── CCA state / thresholds
            │
            ▼
      MAC arbitration
            │
            ▼
      0x3FF20Cxx
```

plutôt qu'un `CCA_BUSY` primaire directement situé dans `0x3FF20Cxx`.

### Niveau de confiance

**Élevé.**

### Sources

- Espressif ESP8266 Non-OS SDK API Reference, exemple `WDEV_NOW()` :
  https://www.espressif.com/sites/default/files/documentation/2c-esp8266_non_os_sdk_api_reference_en.pdf
- pvvx ESP8266 register map :
  https://esp8266.ru/forum/threads/dokumentacija-na-esp8266-na-nashem-sajte-popolnjaemyj-razdel.5/

---

## D25 — `0x3FF20C20` et `0x3FF20C24` ressemblent de plus en plus à un couple RAW/STATUS

Une analyse du timer WDEV rapporte que lorsque MacTim expire :

```text
bit27 de 0x3FF20C20 = 1
```

et que cela déclenche le chemin NMI/FIQ.

Parallèlement, l'ancien reverse-engineering de :

```text
wDev_ProcessFiq()
```

indique :

```text
read 0x3FF20C20
if zero → sortie/traitement court

read 0x3FF20C24
test bits
dispatch handlers
```

### Déduction

Le couple ressemble conceptuellement à l'une de ces architectures classiques :

```text
C20 = RAW interrupt state
C24 = masked interrupt status
```

ou :

```text
C20 = pending summary/status
C24 = detailed status / acknowledge
```

### Mais

On ne possède toujours pas assez d'informations pour choisir entre ces modèles.

Il serait dangereux d'écrire maintenant :

```text
C20 = RAW
C24 = MASKED
```

comme un fait.

### Conséquence pour la chasse au CCA interrupt

Si une transition CCA remonte dans WDEV, il faudra rechercher son bit **dans les deux registres**, et idéalement sur une même révision de `libpp.a`.

Un test futur devra donc capturer simultanément :

```text
0x3FF20C18
0x3FF20C20
0x3FF20C24
```

mais l'analyse statique reste prioritaire avant cette étape.

### Niveau de confiance

**Élevé** pour l'existence d'au moins deux niveaux d'état.  
**Faible à moyen** pour leurs noms exacts.

### Sources

- WDEV timer analysis :
  https://github-wiki-see.page/m/mriksman/esp-idf-homekit/wiki/Timers
- `wDev_ProcessFiq()` reverse-engineering :
  https://github.com/pfalcon/esp8266-re-wiki-mirror/blob/master/WDev_ProcessFiq_%28IoT_RTOS_SDK_0.9.9%29.mw

---

## D26 — attention à la numérotation : WDEV source 0 ≠ nécessairement « Xtensa NMI 0 »

Les analyses RTOS listent :

```text
ETS_WDEV_INUM = 0
```

comme :

```text
WDEV process FIQ interrupt
```

La même documentation Xtensa indique séparément l'existence d'une NMI matérielle dans la configuration du cœur.

### Pourquoi cette précision compte

Il ne faut pas mélanger :

```text
numéro/source d'interruption dans la couche ETS/SDK
```

et :

```text
numéro architectural du vecteur/interruption Xtensa
```

L'ancien chemin reverse-engineeré reste :

```text
_NMIExceptionVector
      ↓
wrapper de sauvegarde registres
      ↓
wDev_ProcessFiq()
```

Donc le terme « NMI/FIQ WDEV » reste approprié fonctionnellement, mais écrire :

```text
NMI = interrupt 0
```

serait trop simplificateur.

### Conséquence

Aucun effet direct sur la localisation du CCA, mais cela évite une fausse piste lors de l'analyse des masques `INTENABLE`.

### Sources

- WDEV/interrupt analysis :
  https://github-wiki-see.page/m/mriksman/esp-idf-homekit/wiki/Timers
- `wDev_ProcessFiq()` reverse-engineering :
  https://github.com/pfalcon/esp8266-re-wiki-mirror/blob/master/WDev_ProcessFiq_%28IoT_RTOS_SDK_0.9.9%29.mw

---

## D27 — la surface symbolique LMAC/WDEV ne contient toujours aucun CCA explicite

Les symboles publics historiques de `libpp.a` exposent notamment :

```text
lmacIsActive
lmacIsIdle

lmacProcessCollision
lmacProcessCollisions
lmacProcessAckTimeout
lmacProcessCtsTimeout
lmacProcessTxSuccess
lmacProcessTxError
lmacRxDone

wDev_ProcessCollision
wDev_GetTxqCollisions
wDev_ClearTxqCollisions

wDev_EnableTransmit
wDev_DisableTransmit
wDev_SetWaitingQueue
wDev_ClearWaitingQueue

wDev_ProcessFiq
wDevEnableRx
wDevDisableRx
```

mais aucune primitive nommée :

```text
CCA
carrier_sense
channel_busy
channel_clear
energy_detect
```

n'apparaît dans cette surface publique historique.

### Interprétation

Ce n'est pas une preuve que le MAC ne possède pas de bit CCA.

En revanche, cela renforce une séparation architecturale :

```text
PHY CCA
   │
   └── consommé silencieusement par MAC/backoff

LMAC/WDEV
   │
   └── expose plutôt files, collisions,
       succès/erreurs TX, RX et timers
```

### Conséquence

Les fonctions les plus intéressantes à désassembler pour remonter **indirectement** au CCA restent :

```text
wDev_EnableTransmit
lmacProcessCollision(s)
lmacIsIdle
wDev_GetTxqCollisions
```

mais on ne doit pas s'attendre nécessairement à y trouver un appel C nommé `cca_busy()`.

### Niveau de confiance

**Élevé** pour l'absence de symbole CCA dans la table publiée.  
**Moyen** pour l'inférence architecturale.

### Sources

- tables de symboles historiques ESP8266 :
  https://41j.com/blog/2015/01/esp8266-sdk-library-symbols/
- esp-open-rtos `allsymbols.rename` :
  https://git.neulandlabor.de/j3d1/esp-open-rtos/src/commit/f5bbff8b87afea6d2e7cd324f50d9640fe9d45f9/lib/allsymbols.rename

---

## D28 — les collisions fournissent un deuxième oracle MAC, distinct du compteur backoff

Le register map reverse-engineeré place :

```text
0x3FF20CC0
```

dans le chemin :

```text
wDev_GetTxqCollisions()
```

et `libpp.a` expose également :

```text
wDev_ProcessCollision()
wDev_ClearTxqCollisions()
lmacProcessCollision()
lmacProcessCollisions()
```

### Intérêt

Nous avions déjà l'oracle :

```text
backoff counter freeze
```

Un deuxième oracle est maintenant clairement identifiable :

```text
collision bookkeeping / collision event
```

Ce n'est pas équivalent au CCA :

- **CCA busy** empêche normalement le début d'une transmission ;
- une **collision** est une conséquence MAC distincte.

Mais les deux passent par la même logique d'arbitrage.

### Usage futur

Lorsqu'un candidat `CCA_BUSY` aura été trouvé, on pourra vérifier trois relations séparées :

```text
CCA busy
   ↓
backoff freeze

CCA clear + TX
   ↓
transmission

TX conflict/failure
   ↓
collision counters/events
```

Cela réduira fortement le risque de prendre un simple bit « TX active/collision » pour le véritable CCA.

### Niveau de confiance

**Élevé** pour l'existence de la voie collision.  
**Indirect** pour CCA.

### Source

- pvvx ESP8266 register map :
  https://esp8266.ru/forum/threads/dokumentacija-na-esp8266-na-nashem-sajte-popolnjaemyj-razdel.5/
- symboles `libpp.a` :
  https://41j.com/blog/2015/01/esp8266-sdk-library-symbols/

---

## D29 — révision du classement des zones à inspecter

Après cette passe, l'ordre de recherche du **vrai `CCA_BUSY`** devient :

```text
1. lectures dans 0x60009B00..0x60009BFF
2. lectures dans 0x60009C00..0x60009CFF
3. lectures dans 0x60009D00..0x60009DFF
4. sdt_on_noise_start() et chip_v6_set_chan_rx_cmp()
5. corrélation avec RX_IQ / RX_GAIN_CTL
6. backoff WDEV comme oracle
7. C18/C20/C24 seulement pour l'hypothèse interrupt
```

### Cibles particulièrement intéressantes

Déjà connues :

```text
0x60009B00 bit28      CCA enable/disable
0x60009B64            noise floor
0x60009C28 [16:10]    sense/backoff configuration
0x60009D24 [7:1]      sense/backoff configuration
0x60009D44 [29,26]    RX/sniffer configuration
```

Le prochain objectif statique n'est plus de chercher n'importe quel registre Wi-Fi :

> il faut trouver **des lectures conditionnelles de bits** dans les voisinages de ces cinq points.

Une séquence idéale à retrouver serait :

```asm
l32i    aX, aBase, offset
extui   aY, aX, bit, 1
beqz/bnez aY, ...
```

dans une routine MAC/RX ou de configuration de détection.

---

# 24. Nouvelles découvertes — v0.7 : analyse directe du dump ROM brut

## D30 — validation cryptographique et mapping du dump

Le fichier ROM brut fourni fait exactement :

```text
65536 octets = 64 KiB
```

Il est donc mappable directement comme :

```text
offset fichier 0x0000 → adresse ROM 0x40000000
offset fichier 0xFFFF → adresse ROM 0x4000FFFF
```

Empreinte du fichier analysé :

```text
SHA-256
32f199bf10da9a08c6c4e1b556b29c7606656b8a6d04e4f17d5afa74e5d98c68
```

Cela permet désormais de distinguer sans ambiguïté les résultats issus de **ce dump précis** des résultats communautaires.

---

## D31 — le littéral `0x60009A00` n'existe qu'une seule fois dans toute la ROM

Une recherche binaire exacte de :

```text
00 9A 00 60
```

ne trouve qu'une seule occurrence :

```text
0x400060CC = 0x60009A00
```

Cette adresse sert donc de pool de littéraux commun aux routines PHY qui accèdent à cette banque.

### Xrefs `L32R` retrouvés vers ce littéral

| Adresse de l'instruction | Registre cible | Routine |
|---|---:|---|
| `0x400060D3` | `a3` | `rom_chip_v5_disable_cca()` |
| `0x400060EF` | `a3` | `rom_chip_v5_enable_cca()` |
| `0x4000610F` | `a7` | `rom_chip_v5_sense_backoff()` |
| `0x40006394` | `a2` | `rom_get_noisefloor()` |
| `0x40006836` | `a4` | `rom_set_noise_floor()` |
| `0x4000687D` | `a4` | `rom_start_noisefloor()` |
| `0x40006FF2` | `a8` | `rom_chip_50_set_channel()` |
| `0x400073DD` | `a8` | `rom_pbus_debugmode()` |
| `0x4000764F` | `a3` | `rom_pbus_workmode()` |
| `0x4000876D` | `a10` | `rom_rfcal_txiq()` |

### Conséquence

Nous avons maintenant une liste **exhaustive des charges directes de cette base précise** dans la ROM.

Cela réduit énormément l'espace de recherche pour les registres `0x60009xxx`.

### Limite

Une fonction pourrait toujours construire une adresse `0x60009xxx` à partir d'une autre base (`0x60009600`, `0x60000000`, etc.). Cette découverte ne prétend donc pas couvrir toutes les constructions arithmétiques possibles d'adresses MMIO.

---

## D32 — carte directe des registres accessibles depuis `0x60009A00`

Le décodage des instructions `L32I/S32I` dans les routines ci-dessus donne la carte suivante.

| Registre | Accès ROM identifié | Fonction |
|---|---|---|
| `0x60009B00` | R/W | CCA enable/disable |
| `0x60009B08` | R/W | PBUS debug/work mode |
| `0x60009B14` | R/W | channel configuration |
| `0x60009B60` | R/W | noise-floor control |
| `0x60009B64` | R/W ou R | noise-floor config/result |
| `0x60009C28` | R/W | sense/backoff field |
| `0x60009C70` | R | channel-related |
| `0x60009D24` | R/W | sense/backoff field |

### Adresses d'instructions confirmées

#### CCA

```text
0x400060D9  L32I ... 0x60009B00
0x400060E2  S32I ... 0x60009B00

0x400060F5  L32I ... 0x60009B00
0x400060FE  S32I ... 0x60009B00
```

#### sense/backoff

```text
0x40006127  L32I ... 0x60009C28
0x40006133  S32I ... 0x60009C28

0x40006145  L32I ... 0x60009D24
0x40006151  S32I ... 0x60009D24
```

#### noise floor

```text
0x4000639A  L32I ... 0x60009B64

0x40006842  L32I ... 0x60009B64
0x4000684E  S32I ... 0x60009B64
0x40006859  L32I ... 0x60009B60
0x40006865  S32I ... 0x60009B60

0x40006886  L32I ... 0x60009B64
0x40006895  S32I ... 0x60009B64
0x4000689E  L32I ... 0x60009B60
0x400068A7  S32I ... 0x60009B60
```

### Conclusion CCA

Parmi les utilisateurs ROM directs de `0x60009A00`, aucun autre registre inconnu n'est lu comme un simple statut booléen dans la zone CCA.

Cela ne prouve pas que le silicium ne possède pas `CCA_BUSY`, mais cela établit que :

```text
les routines ROM CCA elles-mêmes
ne lisent pas un second registre obvious CCA_BUSY
```

Le modèle :

```text
CCA state → consommé directement dans le MAC hardware
```

gagne donc encore en plausibilité.

---

## D33 — `rom_set_noise_floor()` révèle le rôle de `0x60009B60`

Le dump permet de reconstruire deux opérations read/modify/write.

### Sur `0x60009B64`

La fonction charge :

```text
0xFFFFFE00
```

puis fait conceptuellement :

```c
v = REG(0x60009B64);
v &= 0xFFFFFE00;     // efface les bits [8:0]
v |= encoded_value;
REG(0x60009B64) = v;
```

### Sur `0x60009B60`

Elle charge le littéral :

```text
0xFFFD7FFD
```

dont les seuls bits à zéro sont :

```text
bit 17
bit 15
bit 1
```

puis OR avec :

```text
0x00000002
```

Donc l'opération finale est exactement équivalente à :

```c
v = REG(0x60009B60);
v &= 0xFFFD7FFD;
v |= 0x00000002;
REG(0x60009B60) = v;
```

soit :

```text
bit17 = 0
bit15 = 0
bit1  = 1
```

### Interprétation

`0x60009B60` est clairement un registre de **contrôle du mécanisme noise-floor**, pas uniquement une valeur de résultat.

Le nom précis des bits 17 et 15 reste inconnu.

---

## D34 — `rom_start_noisefloor()` positionne exactement les bits 17, 15 et 1 de `0x60009B60`

Le littéral ROM chargé par `rom_start_noisefloor()` vaut :

```text
0x00028002
```

Décomposition :

```text
0x00020000 → bit17
0x00008000 → bit15
0x00000002 → bit1
```

La fonction effectue :

```c
v = REG(0x60009B60);
v |= 0x00028002;
REG(0x60009B60) = v;
```

Donc :

```text
bit17 = 1
bit15 = 1
bit1  = 1
```

### Comparaison avec `rom_set_noise_floor()`

```text
set_noise_floor:
    bit17 = 0
    bit15 = 0
    bit1  = 1

start_noisefloor:
    bit17 = 1
    bit15 = 1
    bit1  = 1
```

La meilleure interprétation prudente est :

```text
bit17 / bit15 = contrôle de lancement/gating du sous-système noise-floor
bit1          = état commun requis par les deux chemins
```

Le rôle exact individuel des bits 17 et 15 n'est pas encore démontré.

---

## D35 — une trace QEMU indépendante confirme `0x60009B60 = 0x00028002`

Une ancienne exécution du SDK ESP8266 sous QEMU rapporte :

```text
unassigned: write +0x00009b64 = 000003a0
unassigned: read  +0x00009b60
unassigned: write +0x00009b60 = 00028002
```

Cette trace est remarquable car elle a été produite indépendamment du présent désassemblage.

Elle correspond exactement au masque trouvé dans le dump :

```text
rom_start_noisefloor()
    OR 0x00028002
```

### Conséquence

Le couple :

```text
0x60009B64
0x60009B60
```

est confirmé comme un chemin effectivement utilisé par le SDK réel pour le noise-floor, et non comme du code ROM mort.

### Source publique

- SuperHouse / esp-open-rtos issue #230, traces QEMU ESP8266.

---

## D36 — `rom_start_noisefloor()` reconfigure les 12 bits bas de `0x60009B64`

La fonction charge le masque :

```text
0xFFFFF000
```

et effectue un read/modify/write sur :

```text
0x60009B64
```

Conceptuellement :

```c
v = REG(0x60009B64);
v &= 0xFFFFF000;
v |= configuration_low_12_bits;
REG(0x60009B64) = v;
```

### Comparaison

`rom_set_noise_floor()` efface :

```text
bits [8:0]
```

alors que `rom_start_noisefloor()` efface :

```text
bits [11:0]
```

avant d'y injecter une nouvelle configuration.

### Conséquence

`0x60009B64` ne doit pas être considéré comme un simple registre « valeur RSSI pure ».

Il contient manifestement au moins :

```text
champs de configuration / état
+
champ(s) utilisés par get_noisefloor()
```

La valeur retournée par `rom_get_noisefloor()` est donc une extraction/transformée d'un registre composite.

---

## D37 — `rom_chip_v5_sense_backoff()` : masques exacts confirmés par le dump

Le littéral :

```text
0xFFFE03FF
```

est le complément de :

```text
0x0001FC00
```

soit exactement les bits :

```text
[16:10]
```

Dans une branche, le code réalise :

```c
v = REG(0x60009C28);
v &= 0xFFFE03FF;
v |= 0x0001FC00;
REG(0x60009C28) = v;
```

donc le champ :

```text
0x60009C28[16:10]
```

peut être forcé à :

```text
0b1111111
```

Le second registre utilise le masque :

```text
0xFFFFFF01
```

qui efface exactement :

```text
0x60009D24[7:1]
```

avant d'y insérer une valeur calculée.

### Conséquence

Les deux champs précédemment soupçonnés sont maintenant confirmés directement par les opcodes et littéraux du dump :

```text
C28[16:10]
D24[7:1]
```

Ce sont les deux champs de configuration les plus proches du comportement « sense/backoff ».

---

## D38 — `rom_get_corr_power()` confirmé directement depuis le dump utilisateur

Le dump montre que `rom_get_corr_power()` charge une base :

```text
0x60000200
```

puis lit directement les offsets :

```text
+0x380 → 0x60000580
+0x384 → 0x60000584
+0x388 → 0x60000588
+0x38C → 0x6000058C
+0x3DC → 0x600005DC
+0x3E0 → 0x600005E0
+0x3E4 → 0x600005E4
```

Donc les lectures sont matériellement confirmées :

```text
RX_IQ_0  0x60000580
RX_IQ_1  0x60000584
RX_IQ_2  0x60000588
RX_IQ_3  0x6000058C

         0x600005DC
         0x600005E0
         0x600005E4
```

### Point essentiel

Il n'y a aucun déclenchement d'acquisition visible dans cette primitive avant ces lectures.

Cela maintient `RX_IQ_0..3` parmi les meilleurs candidats de mesure RX instantanée.

---

## D39 — autres bases MMIO `0x6000xxxx` réellement référencées par `L32R`

Une analyse de tous les `L32R` dont le littéral pointe vers une valeur `0x60000000–0x6000FFFF` donne les bases principales suivantes :

```text
0x60000200
0x60000328
0x600005A4
0x60000600
0x60000A00
0x60009600
0x60009A00
```

Le littéral :

```text
0x60009600
```

est notamment utilisé par quatre chemins ROM.

Les accès directs retrouvés autour de ces xrefs sont surtout :

```text
0x600098A0
0x60009864
0x60009860
```

et ne fournissent pas pour l'instant de nouveau statut CCA dans `0x60009Bxx–0x60009Dxx`.

### Prudence

Cette analyse couvre les accès construits directement depuis les littéraux repérés. Une fonction peut encore dériver une adresse par plusieurs opérations arithmétiques ou via un pointeur chargé depuis la RAM.

---

## D40 — conséquence principale pour la recherche `CCA_BUSY`

Avec le dump réel, la situation devient :

### Ce qui est directement observable dans la ROM

```text
0x60009B00 bit28
    CCA enable/disable

0x60009C28 [16:10]
    sense/backoff configuration

0x60009D24 [7:1]
    sense/backoff configuration

0x60009B60
    noise-floor control

0x60009B64
    noise-floor composite config/result
```

### Ce que l'on ne voit toujours pas

Aucune routine CCA ROM ne fait quelque chose du type :

```c
v = REG(UNKNOWN_CCA_STATUS);
return v & CCA_BUSY_MASK;
```

### Interprétation actuelle

Le scénario n°1 devient :

```text
PHY detector
    │
    ▼
CCA_BUSY interne
    │
    ├── éventuellement observable par MMIO
    │
    └── directement câblé au moteur MAC/backoff
```

Le scénario « une fonction ROM lit explicitement `CCA_BUSY` » devient moins probable.

### Pour l'ISR

Rien dans ce nouveau dump ROM ne révèle une interruption CCA.

Cela est cohérent avec le fait que :

```text
wDev_ProcessFiq()
```

se trouve dans `libpp.a`/RAM et non dans cette ROM PHY.

La prochaine étape statique vraiment décisive est donc l'analyse binaire de :

```text
libpp.a / wdev.o / lmac.o
```

pour reconstruire complètement :

```text
0x3FF20C18
0x3FF20C20
0x3FF20C24
```

et identifier tous les bits WDEV encore orphelins.

---

# 25. Nouvelles découvertes — v0.8 : désassemblage direct de `libpp.a`

## D41 — `libpp.a` contient du DWARF exploitable

L'archive fournie contient notamment :

```text
wdev.o
lmac.o
pp.o
pm.o
pm_for_bcn_only_mode.o
if_hwctrl.o
rate_control.o
trc.o
...
```

`wdev.o`, `lmac.o` et `pp.o` sont :

```text
ELF 32-bit little-endian
Tensilica Xtensa
debug_info présent
non stripés
```

Le DWARF de `wDev_ProcessFiq()` conserve même les variables source :

```text
int_enabled
event
txpmd
txcomplete_state
ack_snr
...
```

et les numéros de lignes de `wdev.c`.

Cela permet de reconstruire le dispatcher WDEV à partir du binaire fourni, sans dépendre uniquement des anciens reverse-engineerings communautaires.

---

## D42 — correction majeure : `0x3FF20C24` est un registre CLEAR/ACK, pas un registre de statut

Début reconstruit de `wDev_ProcessFiq()` :

```c
base = 0x3FF20A00;

int_enabled = REG(base + 0x218);   // 0x3FF20C18
event       = REG(base + 0x220);   // 0x3FF20C20
txpmd       = REG(base + 0x284);   // 0x3FF20C84

REG(base + 0x218) = 0;             // coupe les IRQ WDEV
REG(base + 0x224) = event;         // 0x3FF20C24
```

Puis à la sortie :

```c
REG(0x3FF20C18) = int_enabled;
```

### Conséquence

La valeur lue dans `C20` est l'événement actuellement à traiter.

Le fait de réécrire **exactement cette valeur** dans `C24` est le comportement classique d'un registre :

```text
INT_CLR / ACK
```

probablement **write-one-to-clear**.

La carte précédente :

```text
C20 pending/raw ?
C24 status ?
```

doit donc être corrigée.

---

## D43 — découverte de `0x3FF20C1C` : registre RAW des événements WDEV

`wDev_SnifferRxHT40()` charge :

```text
base = 0x3FF20A00
```

puis lit à plusieurs reprises :

```text
base + 0x21C = 0x3FF20C1C
```

et teste le bit 2.

Le chemin attend notamment que ce bit apparaisse, puis fait :

```c
REG(0x3FF20C24) = 4;   // acknowledge/clear bit2
```

Cette séquence donne une carte très cohérente :

```text
0x3FF20C18  INT_ENA
0x3FF20C1C  INT_RAW
0x3FF20C20  INT_ST / masked pending
0x3FF20C24  INT_CLR
```

### Niveau de confiance

- `C18 = enable` : **confirmé**
- `C24 = clear/ack` : **très fortement établi**
- `C1C = raw` : **très fortement établi**
- `C20 = masked status/pending` : **très fortement établi**

Les noms `INT_RAW`, `INT_ST`, `INT_CLR` sont des noms fonctionnels reconstruits ; ils ne proviennent pas d'un header officiel retrouvé.

---

## D44 — `wDev_Initialize()` fournit le masque d'interruptions normal exact

Le désassemblage de `wDev_Initialize()` donne :

```c
REG(0x3FF20C18) = 0;
REG(0x3FF20C24) = 0xFFFFFFFF;
...
REG(0x3FF20C18) = 0x2C9F0300;
```

Le premier write désactive toutes les interruptions WDEV.

Le second write acquitte/efface tous les événements pending.

Le masque final est :

```text
0x2C9F0300
```

Bits actifs :

```text
bit 8
bit 9
bit 16
bit 17
bit 18
bit 19
bit 20
bit 23
bit 26
bit 27
bit 29
```

### Point crucial pour CCA

Ce masque nous dit exactement quels événements WDEV sont autorisés à provoquer la FIQ dans le mode normal de ce SDK.

Il n'y a donc plus à deviner quels bits pourraient être actifs.

---

## D45 — tous les bits du masque normal sont consommés par `wDev_ProcessFiq()`

### Bits 26 et 27 — timers MAC

Le code fait :

```text
event bit27 → wDev_MacTimerISRHdl(0)
event bit26 → wDev_MacTimerISRHdl(1)
```

Cela concorde avec les anciens travaux qui reliaient ces bits aux timers MAC/TSF.

---

### Bit 20 — collisions MAC

Le code est direct :

```text
if (event & BIT(20))
    lmacProcessCollisions();
```

Ce bit est donc un événement collision/arbitrage MAC, pas un front CCA générique.

---

### Bit 29 — timeout global TX

Le code fait :

```text
if (event & BIT(29))
    lmacProcessAllTxTimeout();
```

---

### Bit 8 — chemin RX

Le bit 8 (`0x100`) ouvre un gros chemin de traitement RX qui peut notamment aboutir à :

```text
wDev_SnifferRxData2()
wDev_ProcessRxSucData()
wDev_DiscardFrame()
```

Il est donc fortement associé à la disponibilité/fin d'un événement RX, et non à une simple transition d'énergie du canal.

---

### Bit 9 — `rx_hung_count`

Le bit 9 conduit à l'incrément d'un champ de la structure DWARF `wDevStatus`.

Le calcul d'offset donne exactement :

```text
my_event.rx_hung_count
```

Donc :

```text
event bit9 → compteur RX hung
```

---

### Bit 23 — `panic_reset_count`

De la même manière, le bit 23 incrémente :

```text
my_event.panic_reset_count
```

Ce bit correspond donc à un événement d'erreur/diagnostic, pas à CCA busy/free.

---

### Bits 16,17,18,19 — groupe TX/RTS

Ces bits alimentent les branches qui appellent :

```text
lmacProcessRtsStart()
lmacProcessTXStartData()
```

avec des tests supplémentaires sur `txpmd`.

Ils appartiennent donc au groupe TX/RTS/arbitrage.

Le nom exact de chaque bit individuel n'est pas encore complètement reconstruit, mais **le groupe entier est déjà rattaché au traitement TX**, pas à une ISR de transition CCA autonome.

---

## D46 — bit 28 : panic path, mais absent du masque normal

`wDev_ProcessFiq()` teste également :

```text
event bit28
```

et entre dans un chemin de panic/printf infini dans cette build.

Cependant :

```text
BIT(28)
```

n'est **pas** contenu dans :

```text
0x2C9F0300
```

Il ne fait donc pas partie des événements WDEV normaux activés par `wDev_Initialize()`.

Il peut s'agir d'un événement fatal/non standard ou d'un événement activé dans un autre contexte non observé.

Il n'est pas un candidat crédible pour une transition CCA normale.

---

## D47 — bits 2 et 3 : événements réservés au mode sniffer dans cette build

En fonctionnement normal :

```text
C18 = 0x2C9F0300
```

donc bits 2 et 3 désactivés.

Lors de :

```text
wdev_go_sniffer()
```

le code effectue :

```c
REG(0x3FF20C18) |= 0x0000000C;
```

c'est-à-dire :

```text
bit2 = enabled
bit3 = enabled
```

Lors de :

```text
wdev_exit_sniffer()
```

il fait :

```c
REG(0x3FF20C18) &= 0xFFFFFFF3;
```

et les bits 2/3 sont de nouveau désactivés.

Dans `wDev_ProcessFiq()`, ces bits alimentent les handlers :

```text
wDev_SnifferRxHT40()
wDev_SnifferRxLDPC()
```

### Conclusion

Les bits 2 et 3 ne sont pas des candidats CCA : ce sont des événements RX/sniffer spécifiques au mode promiscuous/debug.

---

## D48 — carte WDEV/FIQ reconstruite

Carte actuelle :

```text
                     WDEV / MAC interrupt fabric

        0x3FF20C18
        INT_ENA
            │
            │ mask
            ▼
0x3FF20C1C ─────────► 0x3FF20C20
INT_RAW               INT_ST / pending
                          │
                          ▼
                   wDev_ProcessFiq()
                          │
                          │ event bits
                          ▼
                     MAC handlers
                          │
                          ▼
                    0x3FF20C24
                    INT_CLR / ACK
```

Masque normal :

```text
0x2C9F0300
```

Masque sniffer :

```text
0x2C9F030C
```

au minimum pour les bits ajoutés 2 et 3.

---

## D49 — résultat principal : aucune place naturelle pour un `CCA_INT` dans le masque WDEV normal

Avant cette analyse, l'hypothèse était :

```text
un bit non identifié de C18/C20/C24
pourrait être CCA interrupt
```

Dans **ce `libpp.a`**, cette possibilité devient beaucoup plus faible.

Pourquoi :

1. le masque normal exact est connu ;
2. tous les bits activés sont effectivement testés ;
3. chaque groupe mène à un chemin MAC identifiable :
   - RX ;
   - RX hung ;
   - TX/RTS ;
   - collision ;
   - timers ;
   - TX timeout ;
   - diagnostic/panic ;
4. aucun handler ne ressemble à :
   ```text
   CCA busy
   CCA free
   carrier sense
   channel busy
   energy detect
   ```
5. les seuls bits ajoutés dynamiquement identifiés sont 2/3 pour le sniffer.

### Conclusion v0.8

Pour le mode Wi-Fi normal de cette build :

> **aucune interruption WDEV dédiée à une transition CCA BUSY/FREE n'est visible.**

Cette conclusion est beaucoup plus forte que la simple absence d'un symbole `CCA_INT`.

Elle est maintenant fondée sur le **masque d'activation réel et le dispatcher réel**.

### Ce qui reste possible

Un événement CCA pourrait théoriquement exister dans `INT_RAW` sur un bit que le SDK n'active jamais.

Mais dans ce cas :

```text
il n'est pas utilisé comme ISR CCA par libpp.a
```

et il faudrait le découvrir par documentation silicium ou expérimentation hardware.

---

## D50 — seulement deux littéraux `0x60009A00` dans tout `libpp.a`

Une recherche binaire dans tous les membres de l'archive donne :

```text
0x60009A00 → 2 occurrences
```

toutes les deux dans :

```text
wdev.o
```

Elles appartiennent à :

```text
wdev_go_sniffer()
wdev_exit_sniffer()
```

Elles servent notamment à modifier :

```text
0x60009D44
```

pour le passage sniffer on/off.

En revanche, aucune occurrence littérale exacte n'a été trouvée pour :

```text
0x60009B00
0x60009B64
0x60009C28
0x60009D24
```

dans `libpp.a`.

### Interprétation

Le packet processor / LMAC ne semble pas aller lire directement, via ces adresses absolues, le CCA ou le noise-floor PHY.

Cela cadre très bien avec :

```text
CCA hardware → câblé directement au moteur MAC/backoff
```

plutôt qu'avec :

```text
CPU lit CCA_BUSY dans libpp à chaque décision
```

### Limite

Une adresse peut être construite par arithmétique ou être atteinte via un pointeur/table de fonctions. L'absence de littéral n'est donc pas une preuve mathématique d'absence de tout accès.

---

## D51 — `wdev_go_sniffer()` confirme directement `0x60009D44`

Le `libpp.a` fourni confirme l'ancien reverse-engineering.

Entrée sniffer :

```c
REG(0x60009D44) &= 0xDBFFFFFF;
```

soit effacement de :

```text
bit29
bit26
```

Sortie sniffer :

```c
REG(0x60009D44) |= 0x24000000;
```

soit remise à 1 des mêmes bits.

En parallèle :

```text
C18 bits2/3 enabled  pendant sniffer
C18 bits2/3 cleared  à la sortie
```

Cela fournit une corrélation directe entre :

```text
configuration PHY RX
+
événements WDEV RX/sniffer
```

mais aucun de ces bits n'est CCA.

---

## D52 — `lmacIsIdle()` éliminé comme substitut de `CCA_FREE`

Le désassemblage direct de `lmacIsIdle(ac)` montre qu'il ne lit **aucun MMIO**.

Il fait essentiellement :

```c
state = our_instances[ac].field_at_offset_17;

return state == 0;
```

où les instances LMAC sont des structures logicielles indexées par AC.

Donc :

```text
lmacIsIdle()
```

signifie « cet état LMAC logiciel est idle », et non :

```text
le canal RF est libre
```

### Conséquence

`lmacIsIdle()` ne peut pas fournir directement le `CCA_FREE` recherché.

---

## D53 — `ppCheckTxIdle()` n'est pas non plus un CCA checker

Le désassemblage de `ppCheckTxIdle()` montre notamment :

```text
lock interrupts
for ac = 0..3:
    call lmacIsIdle(ac)
inspect TX/software queue state
...
unlock interrupts
```

Il traite les états LMAC/files/buffers.

Aucun accès direct aux registres PHY CCA :

```text
0x60009B00
0x60009C28
0x60009D24
```

n'est effectué.

### Conclusion

La vieille piste :

```text
ppCheckTxIdle() ≈ channel clear
```

est éliminée.

C'est un test de **TX/MAC idle**, pas un Clear Channel Assessment.

---

## D54 — le registre RAW `0x3FF20C1C` reste une piste secondaire intéressante

Même si aucune ISR CCA n'est activée par le SDK, la découverte du registre RAW ouvre une possibilité :

```text
un événement hardware non utilisé par le SDK
pourrait exister sur un bit RAW désactivé
```

Autrement dit :

```text
CCA edge raw bit ?
    ↓
0x3FF20C1C
    ↓
non activé dans C18
    ↓
aucune FIQ
```

### Niveau de confiance

**Hypothèse uniquement.**

Rien dans le binaire n'indique actuellement qu'un tel bit existe.

Cette piste sera utile au moment des tests matériels finaux : on pourra alors observer `C1C` sans activer de nouvelles interruptions.

---

## D55 — classement actualisé du problème CCA / ISR

### ISR CCA dédiée en fonctionnement normal

Probabilité actuelle :

```text
faible
```

car le masque WDEV et tous ses handlers sont maintenant cartographiés.

---

### Bit CCA_BUSY lisible par MMIO

Probabilité actuelle :

```text
toujours élevée / objectif principal
```

mais le bit n'est pas lu directement par les routines ROM CCA ni par `libpp.a`.

---

### CCA uniquement interne au MAC/backoff

Probabilité actuelle :

```text
très élevée
```

et compatible avec toutes les observations :

```text
CCA control      → 0x60009B00
CCA/backoff cfg  → 0x60009C28 / 0x60009D24
                   │
                   ▼
             MAC backoff engine
                   │
       aucun CPU polling nécessaire
```

---

### RAW CCA event caché mais IRQ désactivée

Probabilité :

```text
ouverte mais sans preuve
```

cible potentielle :

```text
0x3FF20C1C
```

uniquement pour une phase de test matériel ultérieure.

---

## D56 — nouvelle architecture de référence

```text
                         PHY / RX

                    énergie / corrélation
                            │
                            ▼
                   ┌─────────────────┐
                   │   CCA hardware  │
                   │                 │
                   │ BUSY/FREE ???   │ ← registre recherché
                   └────────┬────────┘
                            │
                            │ liaison hardware
                            ▼
                   ┌─────────────────┐
                   │ MAC backoff HW  │
                   └────────┬────────┘
                            │
           ┌────────────────┼────────────────┐
           │                │                │
        TX start        collision        timeout/RX
           │                │                │
           └────────────────┼────────────────┘
                            ▼
                    WDEV event fabric

              C18 ENA → C1C RAW → C20 ST
                                   │
                                   ▼
                           wDev_ProcessFiq()
                                   │
                                   ▼
                              C24 CLEAR

```

Dans cette architecture, le CCA peut parfaitement piloter le compteur de backoff **sans jamais générer une FIQ à chaque changement BUSY/FREE**.

C'est désormais le modèle principal.

---


# Nouvelles découvertes — v0.9 : backoff hardware et pipeline noise-floor dans `libpp.a`

## D57 — le registre de backoff `base + 0x3C0` n'est jamais relu par le SDK

L'analyse de tous les accès `L32I/S32I` à l'offset :

```text
+0x3C0
```

dans l'ensemble du `libpp.a` fourni donne un résultat très net.

Dans :

```text
wDev_EnableTransmit(index, aifs, backoff)
```

le code écrit :

```c
base = 0x3FF20A00 - 0x18 * index;

REG(base + 0x3C0) = (backoff & 0x3FF) << 12;
```

Pour `index = 0` :

```text
0x3FF20DC0 [21:12]
```

reçoit la valeur de backoff.

### Recherche globale dans `libpp.a`

Aucun `L32I` utilisant :

```text
base + 0x3C0
```

n'a été retrouvé dans les objets analysés.

Le champ est donc :

```text
CPU → charge valeur initiale
hardware → consomme / décrémente / arbitre
CPU → ne relit pas le compteur dans le SDK
```

### Conséquence CCA

C'est exactement le comportement attendu si :

```text
CCA_BUSY
   │
   ▼
gel / reprise du compteur backoff
```

se fait entièrement dans le MAC hardware.

Le logiciel n'a besoin ni de polling `CCA_BUSY`, ni de polling du compteur pour implémenter CSMA/CA.

### Niveau de confiance

**Très élevé pour cette build de `libpp.a`.**

---

## D58 — `base + 0x3C4` contient de la commande/état TX, pas un CCA générique

Le registre voisin :

```text
base + 0x3C4
```

est au contraire relu par plusieurs chemins TX.

Pour la queue 0 :

```text
0x3FF20DC4
```

Dans `wDev_EnableTransmit()` :

```c
REG(base + 0x3C4) |= 0xC0000000;
```

donc bits :

```text
31
30
```

sont positionnés.

`wDev_DisableTransmit()` les efface.

`lmacDisableTransmit()` utilise explicitement les masques :

```text
0xBFFFFFFF  → clear bit30
0x7FFFFFFF  → clear bit31
```

et teste également le bit31 via une branche sur le signe de la valeur lue.

### Conclusion

Les bits 31/30 de `CTRL_n` appartiennent au chemin :

```text
TX enable / TX state / TX command
```

Ils ne sont pas le signal `CCA_BUSY`.

### Conséquence

Une autre fausse piste est éliminée :

```text
0x3FF20DC4[31:30] ≠ CCA BUSY/FREE
```

au vu de leur utilisation réelle par les fonctions TX.

---

## D59 — seulement deux appels `read_hw_noisefloor()` dans `pp.o`

La table des relocations du `pp.o` fourni contient exactement deux références d'appel vers :

```text
read_hw_noisefloor
```

Les sites sont dans :

```text
pp_enable_noise_timer()
ppRxProtoProc()
```

Cela donne une séparation très instructive :

```text
maintenance périodique du noise-floor
+
rafraîchissement au cours du traitement RX
```

### Conséquence

Le noise-floor hardware est une métrique réellement consommée par le packet processor ; il ne s'agit pas uniquement d'une primitive de calibration PHY isolée.

---

## D60 — `pp_enable_noise_timer()` appelle `noise_check_loop(1,1)` puis lit le hardware

Le désassemblage de `pp_enable_noise_timer()` montre notamment :

```c
if (RF fermé) {
    pend_flag_noise_check = 1;
    ...
} else {
    noise_check_loop(1, 1);
    ...
}
```

Plus loin, la fonction appelle :

```c
v = read_hw_noisefloor();
```

puis effectue exactement :

```c
noise_now = (v + 2) >> 2;
```

La séquence Xtensa reconstruite est :

```text
call read_hw_noisefloor
addi  result, result, 2
srai  result, result, 2
s8i   result, data_base, 6
```

Le symbole local situé à :

```text
.data + 6
```

est :

```text
noise_now_114
```

### Valeurs initiales retrouvées dans `.data`

```text
NoiseTimerInterval = 100
noise_now          = 23
```

Ces valeurs sont les initialiseurs présents dans l'objet ; l'unité exacte de `NoiseTimerInterval` dépend de l'appel timer et n'est pas nécessaire pour la conclusion CCA.

### Ce que cela suggère

`read_hw_noisefloor()` retourne vraisemblablement une représentation plus fine que `noise_now`, puisque le packet processor en stocke une version divisée par quatre.

Il ne faut cependant pas encore attribuer d'unité dBm précise sans désassembler la primitive PHY.

---

## D61 — `ppRxProtoProc()` rafraîchit le noise-floor pendant la réception normale

Le second appel à :

```text
read_hw_noisefloor()
```

se situe dans :

```text
ppRxProtoProc()
```

Le code lit d'abord :

```text
noise_now
```

comme un octet signé.

Le chemin reconstruit est :

```c
int8_t n = noise_now;

if (n >= 1) {
    int v = read_hw_noisefloor();
    noise_now = (v + 2) >> 2;
}
```

La séquence de conversion du byte est visible directement :

```text
L8UI
SLLI 24
SRAI 24
```

ce qui réalise la sign-extension.

### Pourquoi c'est important

Nous savons maintenant que le packet processor peut obtenir une mesure du noise-floor hardware **au moment du traitement RX**, et pas seulement sur un timer lent.

Cela renforce l'intérêt de :

```text
read_hw_noisefloor()
```

comme primitive de lecture RX.

### Mais

Le code ne l'utilise toujours pas comme :

```text
bool busy = ...
```

Il maintient une métrique de bruit mise en cache dans `noise_now`.

Le noise-floor et le `CCA_BUSY` restent donc deux concepts distincts.

---

## D62 — `pp_noise_test()` est essentiellement un wrapper du mécanisme noise timer

Le symbole :

```text
pp_noise_test
```

ne contient qu'une très petite séquence.

Sa relocation principale appelle :

```text
pp_enable_noise_timer()
```

Il ne contient pas lui-même un algorithme séparé de détection CCA.

### Conséquence

La chaîne du packet processor peut être simplifiée ainsi :

```text
pp_noise_test
     │
     ▼
pp_enable_noise_timer
     │
     ├── noise_check_loop(1,1)
     │
     ├── read_hw_noisefloor()
     │
     └── mise à jour noise_now
```

---

## D63 — `noise_check_loop()` devient une cible beaucoup plus importante pour le CCA

`pp.o` appelle explicitement la fonction externe PHY :

```text
noise_check_loop
```

avec :

```text
a2 = 1
a3 = 1
```

avant de relire le noise-floor hardware.

Cela établit une communication réelle :

```text
libpp.a
   │
   ▼
noise_check_loop()    [libphy.a]
   │
   ▼
read_hw_noisefloor() [libphy.a]
```

### Pourquoi cette fonction est importante pour CCA

Le CCA doit nécessairement disposer d'un seuil ou d'une logique d'adaptation au niveau de bruit.

La présence d'un `noise_check_loop()` appelé par le packet processor en fonctionnement normal en fait une cible de très haute priorité pour déterminer :

- quels registres de seuil RX sont modifiés ;
- si `sdt_on_noise_start()` est appelé ;
- si `chip_v6_set_chan_rx_cmp()` est impliqué ;
- si un bit de comparateur / detector status est lu ;
- comment le niveau de bruit influence le hardware CCA.

### Prudence

**Aucun de ces liens internes n'est encore prouvé sans le corps de `libphy.a`.**

Cette section établit uniquement la priorité de reverse-engineering.

---

## D64 — le SDK ne poll pas le CCA pour faire fonctionner le backoff

En combinant les analyses ROM et `libpp.a` :

### PHY ROM

```text
0x60009B00 bit28       CCA enable/disable
0x60009C28 [16:10]     sense/backoff cfg
0x60009D24 [7:1]       sense/backoff cfg
```

### MAC / libpp

```text
wDev_EnableTransmit()
     │
     ├── charge BACKOFF_n
     └── active CTRL_n

aucune relecture BACKOFF_n par le SDK
aucun accès direct aux registres CCA PHY connus
```

### Architecture désormais la plus probable

```text
                PHY detector
                     │
                     ▼
                CCA_BUSY
                     │
              liaison hardware
                     ▼
             MAC backoff engine
                     │
             compteur freeze/run
                     │
                     ▼
                  TX grant
```

Le CPU intervient principalement pour :

```text
charger
armer
traiter les événements MAC de plus haut niveau
```

et non pour échantillonner CCA à chaque slot.

---

## D65 — état actuel de l'hypothèse ISR

Après analyse directe du masque WDEV, du dispatcher FIQ et du moteur backoff :

### ISR CCA dédiée dans le mode normal de cette build

```text
très improbable
```

Aucun bit du masque normal :

```text
0x2C9F0300
```

ne reste non attribué.

### Bit RAW caché non activé

Toujours théoriquement possible dans :

```text
0x3FF20C1C
```

mais **aucune preuve statique** ne l'indique.

### Bit `CCA_BUSY` MMIO lisible

Toujours possible.

Il pourrait exister dans le PHY sans être utilisé par le code Espressif, parce que la liaison matérielle CCA→backoff suffit au fonctionnement normal.

### CCA strictement interne

Désormais l'hypothèse la plus forte.

---

## D66 — frontière statique atteinte : `libphy.a`

À ce stade, `libpp.a` a donné presque tout ce qu'il pouvait donner sur la question CCA :

- fabric d'interruption WDEV cartographiée ;
- ISR/FIQ CCA normale pratiquement éliminée ;
- compteur de backoff confirmé comme hardware autonome ;
- `lmacIsIdle()` et `ppCheckTxIdle()` éliminés ;
- noise-floor hardware connecté au packet processor.

Les fonctions les plus prometteuses restantes sont maintenant dans :

```text
libphy.a
```

priorité :

```text
1. noise_check_loop
2. sdt_on_noise_start
3. chip_v6_set_chan_rx_cmp
4. read_hw_noisefloor
5. ram_start_noisefloor
6. ram_get_corr_power
7. phy_bb_rx_cfg
```

Leur désassemblage permettra de chercher exactement le motif :

```text
L32I / RSR
AND / EXTUI
branch
```

sur un registre RX/CCA inconnu, ou les écritures de seuil qui alimentent le comparateur de CCA.

---



# Nouvelles découvertes — v0.10 : `libphy.a` et bloc CCA/RX comparator

## D67 — `libphy.a` contient `set_cca()` dans une section RAM distincte

Dans `phy_chip_v6.o` :

```text
set_cca
section : .text
offset  : 0x10
taille  : 0x57 = 87 octets
```

Cela est important car cette fonction est différente des trois entrées ROM connues :

```text
rom_chip_v5_disable_cca
rom_chip_v5_enable_cca
rom_chip_v5_sense_backoff
```

Le SDK v6 possède donc une couche supplémentaire de configuration CCA en RAM.

---

## D68 — reconstruction quasi complète de `set_cca()`

Les littéraux de `.text` utilisés par `set_cca()` sont :

```text
0x60009A00
0xFFF00FFF
0x00040000
0x000B4000
```

La première branche commence par :

```text
BEQZ.N a2, deuxième_branche
SLLI   a9, a3, 12
```

Le masque :

```text
0xFFF00FFF
```

efface exactement :

```text
bits [19:12]
```

de `0x60009B64`.

### Branche `a2 != 0`

Reconstruction :

```c
uint32_t v;

v = REG(0x60009B64);
v &= 0xFFF00FFF;
v |= ((uint32_t)a3 << 12);
REG(0x60009B64) = v;

v = REG(0x60009D68);
v |= 0x00040000;
REG(0x60009D68) = v;
```

### Branche `a2 == 0`

Reconstruction :

```c
uint32_t v;

v = REG(0x60009B64);
v &= 0xFFF00FFF;
v |= 0x000B4000;
REG(0x60009B64) = v;

v = REG(0x60009D68);
v |= 0x00040000;
REG(0x60009D68) = v;
```

### Résultat

Le champ :

```text
0x60009B64 [19:12]
```

est **explicitement configuré par une fonction nommée `set_cca()`**.

Le registre :

```text
0x60009D68
```

voit son :

```text
bit18
```

forcé à 1 dans les deux branches.

### Hypothèse très forte

```text
0x60009B64 [19:12]
    = niveau/seuil CCA programmable

0x60009D68 bit18
    = enable / mode du comparateur RX utilisé par CCA
```

Le nom exact des champs n'est pas publié par Espressif ; les noms ci-dessus restent donc fonctionnels/reconstruits.

---

## D69 — correction v0.11 : `0xB4` n'est probablement pas le seuil CCA normal

La v0.10 notait que la branche fallback de `set_cca()` place :

```text
0xB4
```

dans :

```text
0x60009B64[19:12]
```

et observait que `0xB4`, interprété comme `int8_t`, donnerait `-76`.

La poursuite du désassemblage invalide l'idée d'en faire le **seuil CCA normal**.

`phy_bb_rx_cfg()` programme en fonctionnement d'initialisation :

```c
REG(0x60009B64) =
    (REG(0x60009B64) & 0xFFF00FFF)
    | 0x00022000;
```

donc :

```text
0x60009B64[19:12] = 0x22
```

avant d'initialiser aussi les 12 bits bas à `0xFA6`.

### Conclusion corrigée

```text
0x22 = valeur d'initialisation normale observée dans phy_bb_rx_cfg()
0xB4 = valeur spéciale/fallback utilisée par set_cca()
```

Il n'existe plus de base suffisante pour affirmer :

```text
0xB4 ≈ -76 dBm
```

Le champ `[19:12]` pourrait être un codage interne, un seuil transformé, ou un paramètre dépendant du mode.

### État

**Hypothèse -76 dBm retirée.**

---

## D70 — `chip_v6_set_chan_rx_cmp()` configure le même registre `0x60009D68`

La fonction :

```text
chip_v6_set_chan_rx_cmp()
offset : 0x1AE8
taille : 0x180
```

charge :

```text
0x60009A00
```

et utilise le masque :

```text
0xFFFC03FF
```

Le complément du masque est :

```text
~0xFFFC03FF = 0x0003FC00
```

soit exactement :

```text
bits [17:10]
```

Le code réalise un read/modify/write de :

```text
0x60009A00 + 0x368
= 0x60009D68
```

Donc :

```text
0x60009D68 [17:10]
```

est un champ configuré par une fonction explicitement nommée :

```text
set_chan_rx_cmp
```

### Conséquence

Nous avons maintenant :

```text
0x60009D68
    bit18       ← set_cca()
    bits17:10   ← chip_v6_set_chan_rx_cmp()
```

C'est actuellement **le meilleur registre candidat pour la configuration du comparateur RX/CCA**.

---

## D71 — `chip_v6_set_chan_rx_cmp()` modifie aussi `0x60009A34`

Dans la même fonction, après le traitement de `0x60009D68`, des accès narrow Xtensa reconstruits donnent :

```text
L32I.N ..., a4, 0x34
S32I.N ..., a4, 0x34
```

avec :

```text
a4 = 0x60009A00
```

Donc le second registre est :

```text
0x60009A34
```

Il est lu/modifié/écrit à deux reprises.

### Interprétation

`0x60009A34` appartient lui aussi au chemin :

```text
channel / RX compare
```

mais son rôle exact reste inconnu.

Il doit désormais être ajouté à la liste des registres à surveiller lors du futur test matériel.

---

## D72 — correction importante : `read_hw_noisefloor()` ne lit pas `0x60009B64`

La fonction RAM :

```text
read_hw_noisefloor()
offset : 0x1600
taille : 28 octets
```

charge les littéraux :

```text
0xFFFFF001
0x60009600
```

puis effectue une lecture :

```text
base + 0x224
```

soit :

```text
0x60009600 + 0x224
= 0x60009824
```

### Conclusion

La primitive RAM v6 :

```text
read_hw_noisefloor()
```

lit réellement :

```text
0x60009824
```

et non directement :

```text
0x60009B64
```

comme la primitive ROM v5 `rom_get_noisefloor()`.

### Conséquence

Il faut désormais distinguer clairement :

```text
ROM v5:
rom_get_noisefloor()
    → 0x60009B64

PHY v6 RAM:
read_hw_noisefloor()
    → 0x60009824
```

Espressif a donc modifié le chemin de lecture hardware du noise-floor dans la PHY v6.

---

## D73 — `noise_check_loop()` reconfigure `0x60009B64` et surveille `0x60009B60`

La fonction :

```text
noise_check_loop()
offset : 0x1638
taille : 0x152
```

charge notamment :

```text
0xFFFFF1FF
0x60009A00
```

Le masque :

```text
0xFFFFF1FF
```

efface :

```text
bits [11:9]
```

de `0x60009B64`.

Le début du code effectue donc un read/modify/write sur :

```text
0x60009B64
```

puis lit :

```text
0x60009B60
```

à plusieurs endroits.

### Carte nouvelle de `0x60009B64`

Nous savons maintenant que le même registre possède plusieurs champs utilisés par des logiques différentes :

```text
0x60009B64

bits [19:12]   set_cca()
bits [11:9]    noise_check_loop()
bits bas       noise-floor / config historique
```

### Conséquence

`0x60009B64` est un **registre composite CCA/noise-floor**, et non un simple registre de valeur RSSI.

---

## D74 — `sdt_on_noise_start()` touche uniquement le contrôle noise-floor `0x60009B60`

La fonction :

```text
sdt_on_noise_start()
offset : 0x1A40
taille : 0x77
```

charge :

```text
0x60009A00
```

et ses accès MMIO directs reconstruits sont :

```text
L32I  0x60009B60
L32I  0x60009B60
S32I  0x60009B60
L32I  0x60009B60
```

Aucun autre registre PHY direct n'apparaît dans cette fonction.

### Conséquence

La piste SDT reste liée au démarrage/contrôle du sous-système noise-floor, mais elle ne révèle pas un registre indépendant évident :

```text
CCA_BUSY
```

Le nom `SDT` reste non décodé.

---

## D75 — `register_chipv6_phy()` confirme la même paire CCA

Dans `register_chipv6_phy()`, le binaire contient de nouveau les littéraux :

```text
0x60009A00
0xFFF00FFF
0x00040000
```

et réalise la même famille d'opérations :

```text
read/modify/write 0x60009B64
read/modify/write 0x60009D68
```

avec :

```text
0x60009D68 |= BIT(18)
```

### Pourquoi c'est important

La paire :

```text
0x60009B64
0x60009D68
```

n'est pas seulement utilisée par une fonction de debug ou un chemin marginal.

Elle est configurée lors de l'enregistrement/initialisation normal de la PHY v6.

Cela renforce fortement son rôle central dans le CCA/RX comparator.

---

## D76 — carte CCA mise à jour

La meilleure carte actuelle devient :

```text
                    PHY / RX detector
                          │
                          ▼
                ┌─────────────────────┐
                │ CCA configuration   │
                │                     │
                │ 0x60009B00 bit28    │ master enable/disable
                │                     │
                │ 0x60009B64[19:12]   │ set_cca threshold/parameter
                │                     │
                │ 0x60009D68 bit18    │ comparator/CCA enable-like
                │ 0x60009D68[17:10]   │ channel RX compare cfg
                │                     │
                │ 0x60009C28[16:10]   │ sense/backoff cfg
                │ 0x60009D24[7:1]     │ sense/backoff cfg
                └─────────┬───────────┘
                          │
                          ▼
                    CCA decision
                    BUSY / FREE ???
                          │
                          ▼
                  MAC backoff hardware
```

### Cible n°1 actuelle

```text
0x60009D68
```

n'est probablement pas lui-même le `BUSY` instantané, car les fonctions observées l'utilisent comme **configuration**.

Mais il est probablement extrêmement proche du bloc qui produit le statut CCA.

### Cibles de lecture adjacentes

Pour la recherche du statut instantané, il devient logique de prioriser :

```text
0x60009D60
0x60009D64
0x60009D68
0x60009D6C
0x60009D70
```

ainsi que :

```text
0x60009A30
0x60009A34
0x60009A38
```

lors du futur test hardware.

---

## D77 — impact sur la piste ISR

Cette nouvelle découverte ne réhabilite pas une interruption CCA.

Au contraire :

```text
set_cca()
chip_v6_set_chan_rx_cmp()
```

configurent le comparateur PHY, tandis que :

```text
libpp.a
```

ne contient toujours aucune ISR dédiée à BUSY/FREE dans son masque WDEV normal.

Le modèle principal reste donc :

```text
comparateur PHY
     │
     ▼
CCA BUSY/FREE interne
     │
     ▼
MAC backoff hardware
```

avec éventuellement un bit MMIO de statut encore non identifié, mais sans ISR dédiée utilisée par Espressif.

---

## D78 — priorité statique après `libphy.a`

La recherche doit maintenant se concentrer sur les **lectures** voisines du nouveau bloc CCA :

```text
0x60009D68
```

et non uniquement sur les anciennes zones :

```text
0x60009B00
0x60009C28
0x60009D24
```

Ordre de priorité :

```text
1. toutes les lectures autour de 0x60009D60–0x60009D70
2. toutes les lectures autour de 0x60009A30–0x60009A38
3. appels/callers de set_cca()
4. phy_bb_rx_cfg()
5. register_chipv6_phy()
6. chip_v6_set_chan_rx_cmp()
7. registre RAW WDEV 0x3FF20C1C comme piste secondaire
```

Le but est maintenant très précis :

> trouver un registre voisin du **comparateur configuré à `0x60009D68`** qui contient son résultat instantané.



# Nouvelles découvertes — v0.11 : provenance des paramètres CCA et recherche de la sortie comparator

## D79 — `phy_bb_rx_cfg()` initialise le champ CCA à `0x22`

Dans `phy_bb_rx_cfg()`, les littéraux utilisés autour de `0x60009B64` sont :

```text
0xFFF00FFF
0x00022000
0xFFFFF000
0x00000FA6
```

La première séquence est :

```c
v = REG(0x60009B64);
v &= 0xFFF00FFF;
v |= 0x00022000;
REG(0x60009B64) = v;
```

Donc :

```text
0x60009B64[19:12] = 0x22
```

La séquence immédiatement suivante est :

```c
v = REG(0x60009B64);
v &= 0xFFFFF000;
v |= 0x00000FA6;
REG(0x60009B64) = v;
```

donc :

```text
0x60009B64[11:0] = 0xFA6
```

### État du registre après cette initialisation

En ignorant les bits supérieurs conservés :

```text
B64[19:12] = 0x22
B64[11:0]  = 0xFA6
```

### Conséquence

Le champ manipulé par `set_cca()` fait partie de la configuration normale du baseband RX.

La valeur `0x22`, et non `0xB4`, est la valeur de base observée dans ce chemin d'initialisation.

---

## D80 — provenance exacte du byte CCA : `init_data[84]`

Dans :

```text
register_chipv6_phy_init_param()
```

les instructions sont directement décodables :

```text
0x3219  L8UI a7, a2, 84
0x321C  S8I  a7, a8, 170
```

avec :

```text
a2 = pointeur du bloc init_data
a8 = base .bss de phy_chip_v6.o
```

Donc :

```c
bss[170] = init_data[84];
```

Le symbole :

```text
chip6_phy_init_ctrl
```

commence à :

```text
.bss + 0x70
```

Ainsi :

```text
bss[170] = chip6_phy_init_ctrl[0x3A]
```

et :

```text
chip6_phy_init_ctrl[0x3A] = init_data[84]
```

### Utilisation

`register_chipv6_phy()` relit ensuite ce byte et l'injecte dans :

```text
0x60009B64[19:12]
```

dans les modes concernés.

### Conclusion

L'octet d'init PHY :

```text
offset 84 = 0x54
```

est un **paramètre CCA/comparateur caché** dans cette génération du SDK.

Son nom officiel n'apparaît pas dans les symboles.

---

## D81 — provenance exacte du mode CCA : `init_data[88]`

Toujours dans :

```text
register_chipv6_phy_init_param()
```

on trouve :

```text
0x321F  L8UI a6, a2, 88
0x3222  S8I  a6, a8, 171
```

Donc :

```c
bss[171] = init_data[88];
```

soit :

```text
chip6_phy_init_ctrl[0x3B] = init_data[88]
```

Cet octet est ensuite utilisé comme **sélecteur de branche** dans `register_chipv6_phy()`.

---

## D82 — mode 3 vs mode 4 : rôle plus précis de `0x60009D68 bit18`

Dans `register_chipv6_phy()` :

```text
mode = chip6_phy_init_ctrl[0x3B]
value = chip6_phy_init_ctrl[0x3A]
```

Le code reconstruit est conceptuellement :

```c
if (mode == 3) {
    REG(0x60009B64) =
        (REG(0x60009B64) & 0xFFF00FFF)
        | ((uint32_t)value << 12);
}

if (mode == 4) {
    REG(0x60009B64) =
        (REG(0x60009B64) & 0xFFF00FFF)
        | ((uint32_t)value << 12);

    REG(0x60009D68) |= 0x00040000;
}
```

Les autres valeurs de `mode` sautent ce bloc.

### Conséquence

`0x60009D68 bit18` n'est probablement **pas simplement “CCA enable”**.

Il distingue au minimum deux modes de configuration :

```text
mode 3 : paramètre B64 appliqué, D68.bit18 = inchangé
mode 4 : même paramètre B64 + D68.bit18 forcé à 1
```

### Nouvelle interprétation prudente

Le bit18 ressemble davantage à :

```text
sélection/force d'un chemin de comparaison CCA
```

ou :

```text
activation d'une variante du comparator
```

qu'à un master-enable général.

Le master enable/disable CCA confirmé reste :

```text
0x60009B00 bit28
```

---

## D83 — l'ancien tableur Espressif cache explicitement ces deux paramètres

L'ancien document :

```text
ESP8266_RF_init.xls
```

décrit les 128 octets du bloc `esp_init_data_default`.

Dans cette version publique, les lignes :

```text
84
88
```

sont encore indiquées :

```text
Reserved
do not change
```

avec une valeur de laboratoire/default affichée à zéro.

### Important

Cela ne signifie pas que ces bytes sont réellement inutilisés.

Le `libphy.a` fourni prouve exactement le contraire :

```text
84 → CCA config byte
88 → CCA mode selector
```

Le tableur public masque donc volontairement ou historiquement leur vraie sémantique.

### Source externe

- copie consultable de `ESP8266_RF_init.xls` :
  https://www.scribd.com/document/484630213/ESP8266-RF-init
- archive communautaire du fichier d'origine :
  https://esp8266.ru/downloads/esp8266-doc/

### Prudence

Le tableur retrouvé est ancien. Il permet de confirmer le format et les offsets, mais pas d'attribuer un nom officiel moderne à ces bytes pour cette révision précise de `libphy.a`.

---

## D84 — scan global : aucune sortie CCA directe dans `0x60009D60–0x60009D74`

Un scan de tous les accès MMIO directs du `libphy.a` fourni donne :

| Registre | Type d'accès observé | Fonction(s) |
|---|---|---|
| `0x60009D60` | write-only | `ant_switch_init()` |
| `0x60009D64` | write-only | `ant_switch_init()` |
| `0x60009D68` | read/modify/write | `set_cca()`, `chip_v6_set_chan_rx_cmp()`, `register_chipv6_phy()` |
| `0x60009D6C` | aucun accès direct | — |
| `0x60009D70` | read/modify/write | `phy_bb_rx_cfg()` |
| `0x60009D74` | write-only | `phy_get_bb_evm()` |

### Résultat décisif

Aucune fonction ne fait :

```c
status = REG(0x60009D6x);
if (status & ... )
```

dans cette archive.

### Conclusion

Le bloc :

```text
0x60009D68
```

est confirmé comme **configuration comparator / CCA**, mais aucune sortie instantanée BUSY/FREE n'est directement lue dans ses voisins immédiats par le PHY Espressif.

---

## D85 — scan global des lectures directes `0x60009600–0x60009FFF`

En recherchant spécifiquement les registres qui sont **lus sans être immédiatement utilisés comme RMW de configuration**, les candidats directs retrouvés sont essentiellement :

```text
0x60009800
0x60009824
0x60009830
```

avec :

```text
0x60009824 → read_hw_noisefloor()
```

Dans la banque :

```text
0x60009A00–0x60009DFF
```

les accès identifiés sont massivement des opérations de configuration/RMW.

### Conséquence

Le résultat instantané CCA ne semble pas être une valeur que `libphy.a` doit consulter pour fonctionner.

Cela renforce le modèle :

```text
comparateur PHY
      │
      └── sortie câblée directement vers le MAC/backoff
```

### Limite

Cette conclusion porte sur les accès MMIO directs reconstruits.

Un statut peut encore être :

- atteint par un pointeur calculé ;
- présent dans une autre banque (`0x600005xx`, par exemple) ;
- non lu par le logiciel Espressif ;
- purement interne au hardware.

---

## D86 — `chip_v6_rxmax_ext_dig()` relie le comparator à l'état RX

`chip_v6_rxmax_ext_dig()` manipule d'abord :

```text
0x60000590 = RX_GAIN_CTL
```

en fonction de son argument.

Puis il appelle :

```text
chip_v6_set_chan_rx_cmp()
```

avec :

```text
argument 1 = byte signé issu de chip6_sleep_params
argument 2 = argument d'entrée de chip_v6_rxmax_ext_dig(), signé sur 8 bits
```

### Conséquence

La configuration :

```text
0x60009D68[17:10]
```

est recalculée en fonction d'un état/paramètre RX, en même temps qu'un réglage de `RX_GAIN_CTL`.

Cela renforce encore l'interprétation :

```text
D68[17:10] = seuil/offset/paramètre du comparateur RX
```

et non un registre de status.

Le rôle mathématique exact des deux arguments reste à reconstruire complètement.

---

## D87 — `set_cca()` n'est pas appelé directement à l'intérieur de `libphy.a` ou `libpp.a`

La recherche des relocations vers :

```text
set_cca
```

dans les deux archives fournies ne trouve aucun appel direct.

### Interprétation possible

`set_cca()` peut être :

- une primitive exportée pour un autre objet/binaire ;
- appelée indirectement ;
- une API de réglage/debug ;
- conservée même si le chemin d'initialisation normal du SDK reproduit directement sa logique.

En revanche :

```text
register_chipv6_phy()
```

reproduit bien la programmation de :

```text
B64[19:12]
D68 bit18
```

dans le chemin d'initialisation normal.

### Conséquence

Pour comprendre **le fonctionnement CCA normal**, `register_chipv6_phy()` et `phy_bb_rx_cfg()` ont désormais plus de poids que la valeur fallback `0xB4` de `set_cca()`.

---

## D88 — conclusion v0.11 sur le vrai `CCA_BUSY`

Nous avons maintenant localisé avec une confiance élevée le bloc de **configuration** CCA :

```text
0x60009B00 bit28
    master CCA on/off

0x60009B64[19:12]
    paramètre CCA issu de l'init PHY / set_cca

0x60009D68 bit18
    mode / force comparator

0x60009D68[17:10]
    RX comparator configuration

0x60009C28[16:10]
0x60009D24[7:1]
    sense/backoff configuration
```

Mais la sortie :

```text
CCA_BUSY / CCA_FREE
```

n'apparaît toujours pas comme une lecture CPU directe dans :

```text
ROM
libpp.a
libphy.a
```

analysés jusqu'ici.

### Modèle dominant

```text
             RX / energy / correlation
                       │
                       ▼
             CCA comparator hardware
                       │
               BUSY/FREE interne
                       │
                       ▼
               MAC backoff engine
                       │
                       ▼
                 arbitration TX
```

### Probabilités actuelles

```text
CCA_BUSY interne au hardware                très élevée
CCA_BUSY également exposé en MMIO           encore possible
CCA ISR dédiée utilisée par le SDK normal   très faible
CCA RAW event caché mais non activé          possible, sans preuve
```

---

## D89 — prochaines cibles statiques

La recherche ne doit plus se concentrer sur `D68` lui-même comme status.

Les meilleures pistes deviennent :

```text
1. banque RX status/correlation 0x600005xx
2. lectures calculées/non directes dans les fonctions RX
3. registres autour de 0x60009800–0x60009830
4. relation RX_IQ / RX_GAIN_CTL / comparator
5. registre RAW WDEV 0x3FF20C1C, uniquement comme piste d'événement caché
```

Une attention particulière doit être portée à :

```text
0x6000057C
0x60000580..0x6000058C
0x60000590
0x600005DC..0x600005E4
0x60009800
0x60009824
0x60009830
```

Le but reste de trouver soit :

```c
bool busy = REG(...) & MASK;
```

soit une métrique RX live qui reproduit la décision CCA à coût minimal.



# Nouvelles découvertes — v0.12 : recherche systématique des statuts hardware

## D90 — scan `MMIO → bit/champ → branch` sur tout `libphy.a`

Un scanner de flux simple a été construit pour suivre les valeurs chargées depuis les MMIO à travers les opérations :

```text
L32I / L32I.N
EXTUI
AND
SRLI / SRAI / SLLI
MOV
```

jusqu'aux branches :

```text
BBCI / BBSI
BEQZ / BNEZ
BEQI / BNEI
BLT / BGE
...
```

### Résultat dans la banque CCA/noise

Les seules branches hardware directes significatives retrouvées autour de :

```text
0x60009A00–0x60009DFF
```

sont centrées sur :

```text
0x60009B60
```

dans :

```text
do_noisefloor()
noise_check_loop()
sdt_on_noise_start()
```

Aucun test de bit conditionnel direct n'a été retrouvé sur :

```text
0x60009B00
0x60009B64
0x60009C28
0x60009D24
0x60009D68
```

### Conséquence

Si le CCA BUSY était normalement lu par le logiciel Espressif sous la forme :

```c
if (REG(x) & BIT(n))
```

on s'attendrait à voir exactement ce type de motif.

Il est absent dans les trois binaires analysés :

```text
ROM
libphy.a
libpp.a
```

### Limite méthodologique

Ce scan couvre les accès MMIO directs reconstruits à partir des bases/littéraux connues.

Il peut manquer :

- une adresse calculée de manière complexe ;
- un accès indirect via pointeur ;
- un statut hardware jamais lu par le SDK.

Il s'agit donc d'une **preuve négative forte**, pas d'une preuve mathématique d'absence du bit dans le silicium.

---

## D91 — `0x60009B60 bit1` est le vrai statut d'acquisition noise-floor

`do_noisefloor()` effectue directement :

```text
read 0x60009B60
BBCI bit1, ...
```

et répète cette lecture pendant une boucle temporelle basée sur :

```text
WDEV_NOW = 0x3FF20C00
```

Le code revient régulièrement vérifier :

```text
B60.bit1
```

jusqu'à changement d'état ou timeout.

En parallèle, `ram_start_noisefloor()` :

```text
teste B60.bit1
```

avant de déclencher une nouvelle acquisition.

### Reconstruction fonctionnelle

Le comportement est cohérent avec :

```text
B60.bit1 = noise measurement busy / active
```

ou un latch équivalent de procédure en cours.

### Pourquoi c'est important pour la recherche CCA

Nous avons maintenant un exemple réel, dans le même bloc PHY, de la manière dont Espressif lit un **vrai statut hardware** :

```text
L32I
   ↓
BBCI/BBSI sur un bit
   ↓
boucle/branche
```

Aucun motif similaire n'a été retrouvé pour le CCA.

---

## D92 — bits 15 et 17 de `0x60009B60` forment l'état de lancement noise-floor

Dans `noise_check_loop()` :

```c
v = REG(0x60009B60);
state = (v >> 15) & 5;
```

Le masque :

```text
5 = 0b101
```

sélectionne, après décalage, les bits originaux :

```text
bit15
bit17
```

Puis le code compare :

```text
state == 5
```

donc :

```text
bit15 = 1
ET
bit17 = 1
```

### Cohérence avec `ram_start_noisefloor()`

On avait déjà reconstruit :

```c
REG(0x60009B60) |= 0x00028002;
```

or :

```text
0x00028002
= bit17 | bit15 | bit1
```

Donc le trio forme bien une petite machine d'état de la mesure :

```text
bit17 / bit15 → lancement/mode
bit1          → activité/busy de la mesure
```

---

## D93 — découpage fonctionnel complet de `0x60009B64`

Plusieurs fonctions indépendantes touchent des champs non chevauchants de `B64`.

### `ram_get_noisefloor()`

Le code fait :

```text
EXTUI ..., 20, 12
```

donc lit :

```text
B64[31:20]
```

puis convertit la valeur.

### `set_cca()` / `register_chipv6_phy()`

Le masque :

```text
0xFFF00FFF
```

efface :

```text
B64[19:12]
```

avant d'y écrire le paramètre CCA.

### `noise_check_loop()`

Le masque :

```text
0xFFFFF1FF
```

efface :

```text
B64[11:9]
```

### `ram_set_noise_floor()`

Le masque :

```text
0xFFFFFE00
```

cible les :

```text
B64[8:0]
```

### Carte actuelle

```text
31                    20 19        12 11   9 8          0
+-----------------------+------------+------+-------------+
| noise-floor result    | CCA param  | NF   | NF config   |
|                       |            | cfg  | / encoding  |
+-----------------------+------------+------+-------------+
```

### Conclusion

`0x60009B64` est un registre composite extrêmement central :

```text
noise-floor result
+
CCA configuration
+
noise-floor control parameters
```

Le fait que le résultat noise et la configuration CCA soient physiquement voisins renforce le lien matériel entre mesure RX et décision CCA, sans pour autant exposer directement le bit BUSY.

---

## D94 — unité du noise-floor : forte indication de quart de dB

`get_noisefloor_sat()` borne le résultat entre :

```text
-392
-340
```

Le packet processor fait ensuite :

```c
noise_now = (read_hw_noisefloor() + 2) >> 2;
```

### Conversion

Si l'unité PHY vaut :

```text
1/4 dB
```

alors :

```text
-392 / 4 = -98
-340 / 4 = -85
```

ce qui donne une plage réaliste de noise-floor 2.4 GHz :

```text
-98 à -85
```

### Conclusion

L'interprétation la plus cohérente est :

```text
read_hw_noisefloor()
ram_get_noisefloor()
get_noisefloor_sat()
```

utilisent une unité interne d'environ :

```text
0,25 dB
```

et `noise_now` est la version ramenée approximativement en dB entiers.

### Niveau de confiance

**Fort**, mais le suffixe exact `dBm` n'est pas explicitement documenté dans le binaire.

---

## D95 — `0x60009800` éliminé comme CCA : BB EVM / frequency-offset

Deux fonctions utilisent ce registre en lecture :

```text
phy_get_bb_freqoffset()
phy_get_bb_evm()
```

### `phy_get_bb_freqoffset()`

Le code teste :

```text
0x60009800 bit0
```

puis extrait notamment :

```text
bits [15:8]
```

pour calculer le frequency offset.

### `phy_get_bb_evm()`

La même adresse est utilisée comme résultat de mesure EVM/baseband.

### Conclusion

`0x60009800` contient bien un statut/résultat de mesure BB, mais ce statut appartient au chemin :

```text
EVM / frequency-offset
```

et non au CCA.

Il est donc éliminé comme candidat principal `CCA_BUSY`.

---

## D96 — `0x60009824` est exclusivement le noise-floor v6

Le seul accès direct retrouvé à :

```text
0x60009824
```

dans le `libphy.a` fourni est :

```text
read_hw_noisefloor()
```

La fonction extrait :

```text
bits [11:0]
```

puis transforme la valeur en entier signé.

Aucune branche booléenne n'est réalisée directement sur ce registre.

### Conclusion

```text
0x60009824 = mesure noise-floor brute v6
```

est une interprétation maintenant très forte.

Ce registre est intéressant comme métrique RF live, mais pas comme `CCA_BUSY` natif.

---

## D97 — `0x60009830` éliminé : ADC / random / calibration

`0x60009830` est lu par :

```text
get_adc_rand()
phy_wakeup_rf()
chip_60_set_channel()
```

`get_adc_rand()` prélève plusieurs petits champs de ce registre pour construire une valeur pseudo-aléatoire issue du bruit analogique.

### Conclusion

Cette adresse appartient au chemin :

```text
ADC / bruit analogique / calibration
```

et n'est pas un candidat direct CCA BUSY.

---

## D98 — `RX_IQ_0..3` sont des métriques, pas des flags

Les lectures directes :

```text
0x60000580
0x60000584
0x60000588
0x6000058C
```

apparaissent dans :

```text
ram_rxiq_get_mis()
```

La fonction :

- décale les valeurs ;
- les additionne/soustrait ;
- effectue des calculs multiprécision ;
- produit des erreurs/mismatch IQ.

Elle ne teste pas ces registres comme des booléens hardware.

### Conclusion

Les `RX_IQ_0..3` restent d'excellentes **métriques RX/corrélation live potentielles**, mais ce ne sont pas des registres `CCA_BUSY`.

C'est cohérent avec ce que montrait déjà `rom_get_corr_power()`.

---

## D99 — `0x600005E4` appartient aussi au calcul IQ/calibration

Le registre :

```text
0x600005E4
```

est lu dans :

```text
set_rx_gain_cal_iq()
ram_rxiq_get_mis()
```

Il participe à des calculs de gain / mismatch / calibration IQ.

Aucun chemin conditionnel direct ne l'utilise comme :

```text
channel busy
```

Il reste donc une métrique/résultat RX intéressante, mais pas un bit CCA identifié.

---

## D100 — `pm_check_mac_idle()` lit deux états MAC, mais pas le CCA PHY

`pm_check_mac_idle()` dans `phy_sleep.o` lit :

```text
0x3FF20410[3:0]
0x3FF201D0[15:12]
```

et boucle avec un délai tant que certains de ces états ne sont pas au repos.

### Interprétation

La fonction vérifie un **état d'activité MAC** avant le sleep.

Cela est différent de :

```text
medium free / CCA clear
```

Un MAC peut être idle alors que le canal RF est occupé par une autre station.

### Conclusion

Cette fonction est une nouvelle fausse piste éliminée pour `CCA_FREE`.

---

## D101 — inventaire actuel des vraies lectures “status-only” RX/PHY

Après scan direct des accès reconstruits, les principaux registres lus sans écriture correspondante sont :

| Registre | Fonction connue |
|---|---|
| `0x60000580` | RX IQ / mismatch |
| `0x60000584` | RX IQ / mismatch |
| `0x60000588` | RX IQ / mismatch |
| `0x6000058C` | RX IQ / mismatch |
| `0x600005E4` | IQ/calibration |
| `0x60009800` | BB EVM / frequency-offset |
| `0x60009824` | hardware noise-floor |
| `0x60009830` | ADC/random/calibration |

### Résultat

Tous les candidats directs ont une fonction identifiable qui n'est pas :

```text
CCA BUSY/FREE
```

### Portée

Cette liste concerne les accès **directement reconstruits** à partir des objets fournis.

---

## D102 — état de la recherche après le scan exhaustif

La situation devient maintenant très contrainte.

### Trouvé

```text
CCA master enable
    0x60009B00 bit28

CCA parameter
    0x60009B64[19:12]

RX comparator config
    0x60009D68 bit18
    0x60009D68[17:10]

CCA/backoff related cfg
    0x60009C28[16:10]
    0x60009D24[7:1]

noise-floor live
    0x60009824
```

### Non trouvé

```text
CCA_BUSY bit lu par CPU
CCA_FREE bit lu par CPU
CCA edge ISR utilisée par le SDK
```

### Hypothèse dominante

```text
                       PHY
                        │
                énergie/corrélation
                        │
                        ▼
                 CCA comparator
                        │
             BUSY/FREE interne
                        │
                        ▼
                  MAC backoff HW
```

Le SDK configure le bloc et lit des métriques RX, mais ne consulte pas directement sa décision BUSY/FREE.

---

## D103 — prochain axe statique

La meilleure recherche restante avant les tests matériels consiste à chercher :

1. des **adresses calculées indirectement** non couvertes par le scan direct ;
2. les registres RX status autour de `0x600005xx` dont l'adresse peut être construite dynamiquement ;
3. toute comparaison entre :
   ```text
   noise_now
   RX_IQ
   gain RX
   seuil CCA
   ```
   dans d'autres objets SDK ;
4. les versions différentes de `libphy.a` pour voir si une révision plus ancienne ou plus récente expose un getter/status CCA.

La probabilité de découvrir une **ISR CCA utilisée normalement** est maintenant faible.

La probabilité d'un **bit status caché mais inutilisé par Espressif** reste ouverte.



# Nouvelles découvertes — v0.13 : fermeture des dernières pistes statiques

## D104 — le `libphy.a` fourni est exactement PHY version 1156

La fonction :

```text
phy_version_print()
```

est située à :

```text
phy_chip_v6.o
.irom0.text + 0x4318
```

Son début se désassemble ainsi :

```asm
0x4318  l32r  a2, ...
0x431B  movi  a3, 1156
0x431E  movi.n a4, 0
...
call os_printf_plus
```

Le nombre :

```text
1156
```

est donc encodé directement dans le binaire comme numéro de version du PHY.

### Corrélation avec les releases officielles Espressif

Espressif indique dans les release notes NonOS :

```text
SDK 3.0.2 → PHY 1155
SDK 3.0.3 → PHY 1156
```

La mise à jour vers PHY 1156 apparaît également dans le PCN officiel des modules ESP8266 AT V1.7.3.

### Sources externes

- Espressif ESP8266_NONOS_SDK releases:
  https://github.com/espressif/ESP8266_NONOS_SDK/releases
- Espressif PCN-2020-004:
  https://documentation.espressif.com/en/PCN-2020-004%20ESP8266%20modules%20AT%20bin%20upgrade%20to%20V1.7.3.html

### Conséquence

Les découvertes :

```text
set_cca()
0x60009B64[19:12]
0x60009D68
noise_check_loop()
read_hw_noisefloor()
```

ne concernent pas seulement une vieille PHY 2015.

Elles sont présentes dans la révision :

```text
PHY 1156
```

utilisée par la branche tardive/finale du SDK NonOS officiel.

---

## D105 — `register_phy_ops()` reçoit la table à `.data + 4`

Dans :

```text
register_chipv6_phy()
```

un `L32R` charge un littéral local.

Le littéral brut vaut :

```text
0x00000004
```

et possède une relocation :

```text
.data + 0
```

Après link, il représente donc :

```text
&.data[4]
```

Cette adresse est passée à :

```text
register_phy_ops()
```

qui la stocke directement comme pointeur global de dispatch PHY.

### Pourquoi le +4 est important

Les relocations de la table à partir de `.data + 4` sont :

```text
+0x00  chip_v6_rf_init
+0x04  chip_v6_set_chanfreq
+0x08  chip_v6_set_chan
+0x0C  chip_v6_unset_chanfreq
+0x10  rom_chip_v5_enable_cca
+0x14  rom_chip_v5_disable_cca
+0x18  chip_v6_initialize_bb
+0x1C  chip_v6_set_sense
```

Cela permet de mapper précisément les wrappers de `phy.o`.

---

## D106 — `phy_enable_agc()` appelle réellement `rom_chip_v5_enable_cca()`

Dans `phy.o` :

```text
phy_enable_agc()
```

charge le pointeur global de table puis appelle :

```text
table + 0x10
```

Or la table v6 enregistrée place à cet offset :

```text
rom_chip_v5_enable_cca
```

Donc, pour cette PHY :

```text
phy_enable_agc()
      ↓
rom_chip_v5_enable_cca()
      ↓
0x60009B00 bit28 = 0
```

### Résultat surprenant mais direct

Le nom public/historique :

```text
phy_enable_agc
```

masque en réalité un dispatch vers :

```text
enable_cca
```

dans cette table de fonctions.

Cela renforce encore la continuité entre les wrappers PHY historiques et le CCA ROM déjà désassemblé.

---

## D107 — `phy_disable_agc()` appelle `rom_chip_v5_disable_cca()`

Le wrapper suivant :

```text
phy_disable_agc()
```

appelle :

```text
table + 0x14
```

qui correspond à :

```text
rom_chip_v5_disable_cca
```

Donc :

```text
phy_disable_agc()
      ↓
rom_chip_v5_disable_cca()
      ↓
0x60009B00 bit28 = 1
```

### Conséquence

Le couple :

```text
phy_enable_agc()
phy_disable_agc()
```

constitue en pratique, pour cette table PHY, une façade vers :

```text
CCA enable
CCA disable
```

La relation est maintenant confirmée par le dispatch binaire, et plus seulement par le nom des fonctions ROM.

---

## D108 — `phy_set_sense()` dispatch vers un stub vide

`phy_set_sense()` effectue :

```asm
load global phy ops pointer
load function pointer at +0x1C
callx0
```

Le slot :

```text
+0x1C
```

de la table enregistrée est :

```text
chip_v6_set_sense
```

### Corps exact du stub

Dans :

```text
phy_chip_v6_unused.o
```

les symboles sont :

```text
chip_v6_set_sense  offset 0x00  size 2
chip_v6_get_sense  offset 0x04  size 2
```

Les bytes de `chip_v6_set_sense()` sont :

```text
0D F0
```

soit exactement :

```asm
ret.n
```

`chip_v6_get_sense()` contient la même instruction.

### Conclusion

La piste :

```text
phy_set_sense()
chip_v6_set_sense()
chip_v6_get_sense()
```

est **définitivement éliminée** comme source potentielle de :

```text
CCA_BUSY
CCA_FREE
carrier sense status
```

dans PHY 1156.

---

## D109 — scan symbolique des adresses MMIO calculées dynamiquement

Les scans précédents retrouvaient les adresses du type :

```c
REG(0x60009D68)
```

ou :

```c
base = 0x60009A00;
REG(base + 0x368);
```

Il restait une possibilité :

```c
base = MMIO_BASE;
addr = base + index * stride + offset;
value = *addr;
```

qui aurait pu cacher un statut CCA.

### Méthode

Un interpréteur symbolique léger a suivi les registres Xtensa à travers :

```text
L32R
MOV
MOV.N
ADDI
ADDMI
ADD
ADD.N
ADDX2
ADDX4
ADDX8
SUB
```

puis signalé les :

```text
L32I / L16 / L8
S32I / S16 / S8
```

dont la base résultait d'une :

```text
constante MMIO + terme dynamique
```

### Résultat sur tout `libphy.a`

Dans l'espace :

```text
0x60000000–0x6000FFFF
```

les seuls accès dynamiques retrouvés sont dans :

```text
rtc_mem_backup()
rtc_mem_recovery()
```

autour de :

```text
0x60001000
```

Aucun accès dynamique n'a été retrouvé dans les banques :

```text
0x600005xx
0x600098xx
0x60009Axx
0x60009Bxx
0x60009Cxx
0x60009Dxx
```

### Conséquence

L'hypothèse :

> « le SDK lit `CCA_BUSY`, mais via une adresse MMIO calculée que les scans fixes ont manquée »

devient beaucoup moins probable.

### Limite

Comme tout interpréteur statique simplifié, ce scan reste conservateur et ne constitue pas une preuve formelle contre :

- un pointeur passé en argument ;
- une adresse chargée depuis la RAM ;
- un calcul très complexe que l'interpréteur abandonne.

Mais aucun indice de ce type n'a été trouvé dans les fonctions CCA/RX étudiées.

---

## D110 — scan dynamique équivalent dans `libpp.a`

Le même principe appliqué à `libpp.a` retrouve principalement les accès attendus :

```text
BACKOFF_n
CTRL_n
TX queue descriptors
waiting queue
frame ack configuration
key entries
```

Exemples :

```text
wDev_EnableTransmit()
    base dynamique par queue
    +0x3C0 → BACKOFF_n
    +0x3C4 → CTRL_n

wDev_SetWaitingQueue()
    base dynamique
    +0x3A4

lmacSetTxFrame()
    +0x3C4
    +0x3C8
    +0x3CC
    +0x3D0
```

### Résultat CCA

Aucun accès dynamique caché n'a révélé :

```text
un nouveau registre WDEV CCA
un nouveau status BUSY/FREE
un nouveau registre d'interruption CCA
```

Les accès dynamiques sont cohérents avec :

```text
queues TX
descripteurs
arbitrage
```

déjà cartographiés.

---

## D111 — la piste “CCA interrupt cachée par `sense`” est fermée

Avant l'analyse complète de la table PHY, on pouvait imaginer :

```text
phy_set_sense()
     ↓
fonction v6 inconnue
     ↓
activation d'un événement CCA
```

Le dispatch réel montre au contraire :

```text
phy_set_sense()
     ↓
chip_v6_set_sense()
     ↓
ret.n
```

Donc aucune configuration d'IRQ, de seuil ou de statut n'est cachée derrière cette API.

### Impact sur l'ISR

Cela renforce le modèle déjà obtenu depuis `libpp.a` :

```text
pas de CCA BUSY/FREE interrupt utilisée par la pile normale
```

---

## D112 — niveau de confiance après PHY 1156

Après analyse de :

```text
ROM 64 KiB
libpp.a
libphy.a PHY 1156
```

et après :

```text
scan des accès directs
scan read→bit→branch
scan des accès MMIO dynamiques
reconstruction du dispatcher WDEV
reconstruction du dispatch PHY
```

le niveau de confiance devient :

### CCA hardware interne

```text
très élevé
```

### CCA directement câblé au backoff MAC

```text
très élevé
```

### Bit `CCA_BUSY` potentiellement présent dans le silicium mais inutilisé par le SDK

```text
toujours possible
```

### Bit `CCA_BUSY` lu quelque part par la pile Espressif fournie

```text
très improbable
```

### ISR/FIQ CCA BUSY/FREE activée en fonctionnement normal

```text
très improbable
```

### Getter logiciel CCA caché derrière `sense`

```text
éliminé
```

---

## D113 — conséquence pratique pour la suite du reverse-engineering

Nous arrivons à une frontière nette entre :

```text
ce que le logiciel Espressif révèle
```

et :

```text
ce que seul le silicium peut encore révéler
```

L'analyse statique a localisé :

```text
master CCA
threshold/config
RX comparator config
noise-floor live
RX IQ metrics
MAC backoff engine
interrupt fabric
```

mais pas la sortie :

```text
CCA_BUSY
```

### Les trois pistes restantes sont désormais

#### A — registre hardware caché et inutilisé par le SDK

Trouver par observation des registres voisins pendant un signal RF connu.

#### B — registre RAW WDEV inutilisé

Observer :

```text
0x3FF20C1C
```

sans activer de nouvelles interruptions et corréler ses bits à BUSY/FREE.

#### C — utiliser un oracle hardware

Corréler :

```text
RX_IQ / noise-floor
+
gel/reprise du backoff
+
registres voisins du comparator
```

pour identifier expérimentalement la sortie CCA.

### Conclusion

La recherche statique n'est pas totalement épuisée, mais les gains restants sont désormais nettement plus faibles qu'avant l'analyse de PHY 1156.



# Nouvelles découvertes — v0.14 : première capture hardware

## D114 — première capture synchronisée réussie

Le premier CSV du probe montre un front :

```text
sample 62  sync=0
sample 63  sync=1
```

Le GPIO de synchronisation fonctionne donc correctement et le probe est aligné sur le début de la rafale du générateur.

---

## D115 — trou artificiel de 8866 cycles au trigger dans le probe v2

Entre :

```text
sample 63
sample 64
```

la différence de timestamp vaut :

```text
8866 cycles
```

alors que les échantillons voisins sont séparés d'environ :

```text
388 cycles avant trigger
298 cycles après trigger
```

Ce gros trou n'est pas interprété comme une interruption Wi-Fi :

le code v2 copiait le ring-buffer de pré-trigger **immédiatement après la détection du front**, avant de commencer la capture post-trigger.

### Correction

Probe v3 :

```text
aucune copie au trigger
capture POST immédiate
dump/copie uniquement après acquisition
```

---

## D116 — la première capture ne voit pas le front descendant

La rafale générateur dure :

```text
10 ms
```

mais le probe v2 capture seulement quelques centaines de samples rapides après le front montant.

Dans le CSV :

```text
sample 63 ... 319
sync reste constamment à 1
```

Le front descendant n'est donc jamais observé.

### Correction

Probe v3 utilise :

```text
25 us nominal / sample
640 samples POST
```

soit environ :

```text
16 ms
```

de fenêtre nominale, suffisante pour voir :

```text
avant
front montant
rafale 10 ms
front descendant
après
```

---

## D117 — aucune variation des registres de configuration CCA

Pendant toute la première fenêtre observée :

```text
0x60009B60 = 0x00069510
0x60009B64 = 0xD20223A0
0x60009D68 = 0x00037DCE
```

restent constants.

### Interprétation

C'est cohérent avec le reverse-engineering statique :

```text
B64 / D68 = configuration
```

et non sortie CCA instantanée.

Cette première mesure expérimentale est donc compatible avec la conclusion statique.

---

## D118 — `RX_IQ_0..3` restent à zéro dans ce mode

Pendant toute la capture :

```text
RX_IQ_0 = 0
RX_IQ_1 = 0
RX_IQ_2 = 0
RX_IQ_3 = 0
RX_GAIN = 0
```

### Conclusion provisoire

Les registres RX_IQ ne fournissent pas, dans ce contexte STA normal et dans cette fenêtre, une métrique live directement exploitable.

Cela peut signifier qu'ils :

- ne sont mis à jour que pendant une primitive IQ/corrélation particulière ;
- sont latchés uniquement dans un chemin calibration/debug ;
- sont remis à zéro hors fenêtre de calcul.

### Prochaine correction

Probe v3 conserve un résumé `RXIQ_OR` pour détecter toute activité non nulle sur la fenêtre complète.

---

## D119 — `WDEV_STATUS` reste à zéro

Le CSV montre :

```text
0x3FF20C20 = 0
```

sur tous les samples.

### Interprétation

Cela est cohérent avec :

```text
C20 = status/pending masqué
```

dont les événements activés sont immédiatement consommés/acquittés par la FIQ WDEV.

Un polling depuis le main loop a peu de chances d'attraper des pulses courts dans ce registre.

---

## D120 — `WDEV_RAW` reste constant à `0x02009CEF`

La première capture donne :

```text
0x3FF20C1C = 0x02009CEF
```

sur toute la fenêtre observée.

Bits à 1 :

```text
0
1
2
3
5
6
7
10
11
12
15
25
```

### Conclusion

Aucun de ces bits ne corrèle avec le front montant dans cette première capture.

Le registre RAW peut contenir :

- états/latches persistants ;
- événements non masqués non acquittés ;
- signaux de niveau qui restent actifs.

Il ne fournit donc pas encore un candidat CCA.

---

## D121 — `BACKOFF_0 = 7` constant n'est pas encore un test valide de l'oracle backoff

Le registre :

```text
0x3FF20DC0
```

reste :

```text
0x00007007
```

et donc :

```text
[21:12] = 7
```

pendant la capture.

### Important

Cela ne permet **pas encore** de conclure que le registre est seulement un registre de chargement.

Le probe ne génère quasiment aucune transmission applicative pendant la fenêtre.

Il peut donc simplement ne pas avoir de countdown actif à observer.

### Test futur requis

Il faudra faire en sorte que le probe possède une file TX réellement en contention pendant la rafale RF du générateur.

---

## D122 — `0x60009824` reste constant sur la courte première fenêtre

La valeur brute observée est :

```text
0x00000D2C
```

sur tous les samples du CSV.

### Interprétation

La lecture brute du registre hardware noise-floor n'est pas une métrique instantanée de type RSSI rapide sur cette courte fenêtre.

Probe v3 ajoute directement :

```text
read_hw_noisefloor()
```

afin de comparer :

```text
valeur brute 0x60009824
valeur transformée par la primitive PHY officielle
```

sur une fenêtre beaucoup plus longue.

---

## D123 — état après la première expérience

La première capture ne révèle aucun `CCA_BUSY`, mais elle élimine déjà plusieurs attentes trop optimistes :

```text
RX_IQ live gratuit     non observé
WDEV_STATUS polling    trop éphémère / toujours zéro
WDEV_RAW               constant dans cette fenêtre
B64/D68                 bien configuration-only
```

Le protocole doit maintenant :

```text
capturer le front descendant
supprimer le trou logiciel au trigger
allonger la fenêtre
lire read_hw_noisefloor()
puis créer un vrai TX pending pour tester le backoff
```

Le probe v3 implémente les trois premières corrections.



# Nouvelles découvertes — v0.15 : analyse complète de la capture v3

## D124 — la fenêtre v3 couvre correctement RF OFF → ON → OFF

La capture contient :

```text
64 samples PRE
640 samples POST
704 samples total
```

Répartition réelle :

```text
SYNC=1 : 405 samples
SYNC=0 : 299 samples
```

Le front montant se trouve au dernier sample PRE :

```text
cycles = 2327323223
```

Le front descendant apparaît à :

```text
POST 404
cycles = 2328133837
```

Durée entre les deux :

```text
810614 cycles
/ 80 MHz
= 10,132675 ms
```

Cette durée est très proche des :

```text
10 ms
```

programmées dans le générateur.

### Conclusion

Le GPIO `SYNC` est désormais une bonne vérité terrain pour distinguer la fenêtre RF-heavy du générateur.

---

## D125 — cadence v3 stable à environ 25 µs

Les deltas `CCOUNT` donnent typiquement :

```text
1996
1998
2002
2005 cycles
```

avec une médiane :

```text
1998 cycles
```

À 80 MHz :

```text
1998 / 80 = 24,975 µs
```

Les plus gros écarts ponctuels sont inférieurs à environ :

```text
2923 cycles ≈ 36,5 µs
```

dans cette capture.

### Conclusion

La capture v3 est suffisamment régulière pour comparer des **signaux de niveau** qui dureraient des dizaines de microsecondes ou plus.

Elle peut toutefois manquer un pulse hardware très bref.

---

## D126 — corrélation ON/OFF exactement nulle pour tous les champs v3

Sur les :

```text
405 samples RF ON
299 samples RF OFF
```

chaque champ suivant possède exactement une valeur unique :

```text
WDEV_RAW       0x020094CF
WDEV_STATUS    0x00000000
BACKOFF_RAW    0x00003003
BACKOFF[21:12] 3
RXIQ_OR        0x00000000
RX_GAIN        0x00000000
NOISE_RAW      0x00000D24
NOISE_HW       -366
```

### Conséquence

Aucun de ces champs ne constitue un `CCA_BUSY` de niveau observable à 25 µs dans cette configuration.

Cette conclusion est plus forte que celle de la première capture, car v3 contient maintenant des périodes :

```text
avant RF
pendant RF
après RF
```

dans le même enregistrement.

---

## D127 — `WDEV_RAW` ne suit pas CCA, même si sa valeur de fond peut changer entre captures

Première capture v2 :

```text
WDEV_RAW = 0x02009CEF
```

Capture v3 :

```text
WDEV_RAW = 0x020094CF
```

XOR :

```text
0x00000820
```

soit différence sur :

```text
bit5
bit11
```

Mais dans chaque capture, le registre reste constant pendant le passage RF OFF/ON.

### Conclusion

Les bits 5 et 11 peuvent refléter un état/latch de fond qui varie entre sessions, mais ils ne se comportent pas comme un niveau `CCA_BUSY` dans les mesures présentes.

---

## D128 — décodage expérimental exact de `read_hw_noisefloor()`

La capture donne simultanément :

```text
0x60009824 = 0xD24
read_hw_noisefloor() = -366
```

Le champ brut sur 12 bits vaut :

```text
0xD24 = 3364
```

Comme entier signé 12 bits :

```text
3364 - 4096 = -732
```

et :

```text
-732 >> 1 = -366
```

### Conclusion

Pour cette valeur mesurée :

```text
read_hw_noisefloor()
```

est exactement cohérent avec :

```text
sign_extend_12(raw) / 2
```

ou un décalage arithmétique équivalent.

### Lien avec `libpp.a`

Le packet processor fait ensuite :

```c
noise_now = (read_hw_noisefloor() + 2) >> 2;
```

Pour :

```text
-366
```

cela donne approximativement :

```text
-91
```

en entier signé.

Cette mesure expérimentale soutient fortement l'idée que la représentation retournée par `read_hw_noisefloor()` possède une résolution plus fine que 1 dB, vraisemblablement le quart de dB déjà inféré statiquement.

---

## D129 — `BACKOFF_0 = 3` constant confirme que l'oracle doit être testé avec TX pending

Dans v3 :

```text
0x3FF20DC0 = 0x00003003
```

sur toute la capture.

Le champ :

```text
[21:12] = 3
```

ne change jamais.

### Mais

Le probe ne crée pas de nouvelle transmission pendant la fenêtre.

Le registre peut donc contenir :

- une valeur de chargement ;
- une valeur associée à une queue idle ;
- un compteur qui n'est actif que lorsqu'une TX est pending.

### Prochaine expérience

Le probe v4 créera une vraie transmission UDP immédiatement après le front montant puis lira :

```text
Q0 BACKOFF 0x3FF20DC0
Q1 BACKOFF 0x3FF20DA8
Q2 BACKOFF 0x3FF20D90
Q3 BACKOFF 0x3FF20D78
```

avec leurs registres CTRL associés.

---

## D130 — pourquoi le test WDEV doit devenir beaucoup plus rapide

Le fait que :

```text
C1C RAW = constant
C20 ST  = 0
```

à 25 µs exclut un **niveau CCA persistant** dans ces deux registres.

Il n'exclut toutefois pas un pulse court du type :

```text
CCA transition
     ↓
RAW pulse de quelques cycles / µs
     ↓
manqué entre deux samples de 25 µs
```

### Probe v4

La commande :

```text
w
```

ne stocke plus tous les samples.

Elle lit :

```text
0x3FF20C1C
0x3FF20C20
```

en boucle serrée et accumule séparément :

```text
OR
AND
CHANGE mask
```

pour :

```text
PRE-OFF
RF-ON
POST-OFF
```

Un pulse très court peut ainsi laisser une trace dans :

```text
OR
ou
CHANGE mask
```

même s'il ne dure qu'une petite fraction de la rafale.

---

## D131 — protocole v4 proposé

### Test 1

Sur le probe :

```text
w
```

Résultats importants :

```text
RAW_ON_ONLY
RAW_CHANGED_ONLY_ON
ST_ON_ONLY
ST_CHANGED_ONLY_ON
```

Un résultat non nul mérite analyse bit par bit.

### Test 2

Puis :

```text
b
```

Le probe :

1. attend `SYNC` montant ;
2. envoie une trame UDP normale au générateur ;
3. capture les quatre queues MAC pendant environ 15 ms ;
4. traverse donc le reste de la rafale puis son front descendant.

### Signature recherchée

Exemple idéal :

```text
SYNC=1:
Qx backoff = 17,17,17,17,17

SYNC=0:
Qx backoff = 16,15,14,...
```

Cela démontrerait directement le gel/reprise hardware piloté par CCA.



# Nouvelles découvertes — v0.16 : événement WDEV bit8 pendant RF ON

## D132 — le scan haute vitesse attrape un bit RAW uniquement pendant la rafale

Commande utilisée :

```text
w
```

Résultats :

```text
RAW_PRE
count=8837
OR=0x02009CCF
AND=0x02009CCF
CHANGE=0

RAW_ON
count=5273
OR=0x02009DCF
AND=0x02009CCF
CHANGE=0x100

RAW_POST
count=928
OR=0x02009CCF
AND=0x02009CCF
CHANGE=0
```

Ainsi :

```text
0x02009DCF XOR 0x02009CCF = 0x100
```

Le seul bit supplémentaire détecté est :

```text
bit8
```

Il apparaît de manière transitoire pendant la période `SYNC=1`.

---

## D133 — bit8 est déjà connu comme événement RX, pas CCA

Le reverse-engineering de :

```text
wDev_ProcessFiq()
```

avait déjà établi que :

```text
WDEV event bit8
```

entre dans le chemin RX.

Le masque normal :

```text
0x2C9F0300
```

active également bit8.

### Corrélation expérimentale

Le générateur envoie pendant `SYNC=1` une rafale de vraies trames UDP Wi-Fi.

Le probe :

```text
est associé au SoftAP
est sur le même canal
reçoit donc ces trames
```

Il est donc attendu que le chemin RX s'active.

### Conclusion

```text
0x3FF20C1C bit8
```

est expérimentalement confirmé comme événement/activité RX.

Il ne doit pas être renommé `CCA_BUSY`.

---

## D134 — la différence RAW est transitoire, pas un niveau permanent

Dans la phase ON :

```text
OR  contient bit8
AND ne contient pas bit8
FIRST ne contient pas bit8
LAST ne contient pas bit8
CHANGE contient bit8
```

Donc bit8 :

```text
0 → 1 → 0
```

au moins une fois pendant la rafale.

Il s'agit d'un pulse/événement, pas d'un niveau qui reste haut pendant toute l'occupation du médium.

Cela est encore plus compatible avec :

```text
RX event
```

qu'avec un `CCA_BUSY` de niveau.

---

## D135 — `C20` montre aussi bit8, mais hors de la fenêtre ON

Résultats :

```text
ST_PRE
OR=0x100
AND=0
CHANGE=0x100
FIRST=0
LAST=0

ST_ON
OR=0
AND=0
CHANGE=0

ST_POST
OR=0
AND=0
CHANGE=0
```

### Interprétation

Un événement bit8 est visible dans `C20` pendant PRE, probablement lié à :

```text
beacon
trame de management
activité RX normale du SoftAP
```

pendant la phase calme.

Pendant ON, `C20` peut être :

```text
asserté puis acquitté par la FIQ
```

avant que la boucle de polling ne le voie.

### Conséquence

Le comportement renforce l'interprétation :

```text
C20 = pending/status rapidement consommé
C1C = RAW plus facile à observer
```

sans fournir de nouveau bit CCA.

---

## D136 — résultat du test W sur l'hypothèse « IRQ CCA cachée »

Avant le test, l'hypothèse était :

```text
CCA edge
   ↓
bit RAW non activé / non utilisé
```

Le seul nouveau bit corrélé trouvé est :

```text
bit8
```

or ce bit est déjà :

```text
activé dans le masque normal
consommé par wDev_ProcessFiq()
attribué au RX
```

### Conclusion actuelle

Le test ne fournit **aucune preuve d'un événement CCA caché**.

Il valide au contraire la capacité du scan à détecter de vrais pulses WDEV.

Donc l'absence d'un autre bit pendant cette rafale devient une preuve négative plus significative.

---

## D137 — prochaine étape : oracle backoff actif

Le test restant le plus discriminant est :

```text
b
```

Le probe va :

1. attendre le front `SYNC` montant ;
2. demander une transmission UDP normale ;
3. lire les quatre queues :
   ```text
   Q0 0x3FF20DC0
   Q1 0x3FF20DA8
   Q2 0x3FF20D90
   Q3 0x3FF20D78
   ```
4. suivre également leurs registres CTRL ;
5. continuer après le front descendant.

### Signature recherchée

```text
RF ON:
backoff stable

RF OFF:
backoff décrémente
```

Si cette signature apparaît, elle fournira un oracle hardware direct du CCA même si aucun bit BUSY MMIO n'est exposé.



# Nouvelles découvertes — v0.17 : pourquoi le test backoff v4.1 ratait la fenêtre active

## D138 — la capture `b` contient bien le front descendant

La capture commence pendant :

```text
SYNC=1
```

et le front descendant apparaît à :

```text
sample 310
cycles = 3138220538
```

Les samples suivants restent bien :

```text
SYNC=0
```

jusqu'à la fin.

### Valeurs avant/après le front

Avant :

```text
Q0 = 1
Q1 = 0
Q2 = 13
Q3 = 0
```

Après :

```text
Q0 = 1
Q1 = 0
Q2 = 13
Q3 = 0
```

Aucune variation n'est observée.

---

## D139 — les 512 samples montrent des registres de backoff constants

Sur toute la capture parseable :

```text
Q0_RAW = 0x00001001
Q1_RAW = 0x00000000
Q2_RAW = 0x0000D00D
Q3_RAW = 0x00000000
```

Les champs `[21:12]` sont donc constamment :

```text
Q0 = 1
Q1 = 0
Q2 = 13
Q3 = 0
```

Les trois lignes légèrement corrompues dans le texte collé ne changent pas cette conclusion : les valeurs avant et après restent identiques.

---

## D140 — `endPacket()` cache environ 851 µs de comportement MAC

Le probe imprime :

```text
sync_seen_cycles   = 3137406069
tx_queued_cycles   = 3137474132
queue_call_cycles  = 68063
```

À 80 MHz :

```text
68063 / 80 = 850,7875 µs
```

Le premier sample CSV est à :

```text
3137476542
```

soit :

```text
70473 cycles après le front
= 880,9125 µs
```

et seulement :

```text
2410 cycles
= 30,125 µs
```

après le retour de `endPacket()`.

### Conclusion

Le v4.1 ne mesure pas la période où la pile est en train de :

```text
mettre la trame en queue
armer le MAC
effectuer le backoff
émettre
```

Il commence précisément **après** cette fenêtre.

---

## D141 — les CTRL ne montrent plus l'état « armed »

Au premier sample :

```text
Q0_CTRL = 0x0162CC88
Q2_CTRL = 0x0162BD6C
```

Pour chacun :

```text
CTRL & 0xC0000000 = 0
```

donc :

```text
bits31:30 = 00
```

Or `wDev_EnableTransmit()` avait été reconstruit comme positionnant :

```text
bits31:30 = 11
```

lors de l'activation d'une transmission.

### Interprétation

Le fait que ces bits soient déjà à zéro au premier sample soutient fortement :

```text
TX déjà terminée / queue non armée
```

plutôt que :

```text
backoff actif mais compteur invisible
```

à cet instant précis.

---

## D142 — la durée restante de la rafale était pourtant suffisante

Le front descendant est observé vers :

```text
10,18 ms
```

après le front montant.

Après le retour de `endPacket()`, il reste encore environ :

```text
9,33 ms
```

de `SYNC=1`.

Mais la trame du probe a probablement déjà été traitée pendant les premiers ~851 µs.

### Point important

`SYNC=1` signifie :

```text
fenêtre de trafic RF lourd
```

et non :

```text
porteuse RF continuellement occupée
```

Il existe donc des intervalles libres entre les trames du générateur, suffisants pour que le probe transmette très tôt.

---

## D143 — correction protocolaire : sampler pendant `endPacket()`

Le probe v4.2 utilise :

```text
Timer1
```

pour échantillonner indépendamment du foreground.

### Séquence

```text
1. construire beginPacket()/write() avant la rafale
2. attendre SYNC=0
3. armer Timer1
4. ISR détecte SYNC montant
5. foreground appelle endPacket()
6. Timer1 continue à lire les queues pendant endPacket()
7. capture jusqu'à ~16 ms après le front
```

### Cadence

Timer1 est configuré :

```text
80 MHz / 16 = 5 MHz
100 ticks = 20 µs
```

La capture contient :

```text
64 samples PRE
800 samples POST
```

soit environ :

```text
1,28 ms avant
16 ms après
```

---

## D144 — représentation compacte du nouveau probe

Pour éviter de consommer trop de RAM, chaque sample v4.2 stocke :

```text
CCOUNT
SYNC
backoff Q0
backoff Q1
backoff Q2
backoff Q3
CTRL bits31:30 des quatre queues
```

Les deux bits hauts des quatre CTRL sont empaquetés dans un seul octet.

### Pourquoi ces bits

La recherche statique montre que :

```text
wDev_EnableTransmit()
```

positionne précisément ces bits lors de l'activation.

Si le timer les voit passer :

```text
00 → 11 → 00
```

pendant `endPacket()`, on aura enfin la fenêtre exacte de vie de la queue TX.

---

## D145 — critères du test v4.2

Trois issues sont possibles.

### A — CTRL devient `11` et backoff change

Alors :

```text
registre de backoff probablement live
```

et on peut analyser son comportement par rapport au trafic RF.

### B — CTRL devient `11` mais backoff reste figé

Alors :

```text
champ [21:12] probablement valeur de chargement
compteur interne ailleurs/non lisible
```

### C — CTRL ne devient jamais `11`

Alors il faudra revalider :

```text
adresse de queue
mapping des bits CTRL
timing Timer1
```

ou considérer que ces bits sont trop fugitifs / write-only / auto-clear.



# Nouvelles découvertes — v0.18 : armement Q2 observé directement

## D146 — Timer1 capture bien l'activité MAC masquée par `endPacket()`

Le test v4.2 mesure :

```text
endPacket_start = 1115377644
endPacket_done  = 1115445105
duration        = 67461 cycles
```

À 80 MHz :

```text
843,2625 µs
```

Le Timer1 continue à échantillonner pendant cette période.

Le front `SYNC` montant est vu à :

```text
1115377065
```

soit environ :

```text
7,24 µs
```

avant l'appel `endPacket()`.

---

## D147 — Q2 est la queue activée par la transmission UDP du probe

Jusqu'à :

```text
POST 44
```

on lit :

```text
Q0 = 1   CTRL=0
Q1 = 0   CTRL=0
Q2 = 11  CTRL=0
Q3 = 0   CTRL=0
```

À :

```text
POST 45
cycles = 1115449070
```

on obtient brutalement :

```text
Q2 backoff = 15
Q2 CTRL[31:30] = 3
```

Les autres queues restent inchangées.

### Conclusion

La TX UDP du probe est placée dans :

```text
Q2
```

dans cette configuration.

---

## D148 — validation expérimentale de `wDev_EnableTransmit()`

Le reverse-engineering statique avait montré que :

```text
wDev_EnableTransmit()
```

positionne les bits :

```text
CTRL[31:30] = 11
```

lors de l'activation d'une queue.

La capture hardware montre précisément :

```text
Q2 CTRL : 00 → 11 → 00
```

pendant une vraie transmission.

Cette relation n'est donc plus seulement une inférence statique : elle est validée expérimentalement.

---

## D149 — durée observée de l'état actif Q2

Q2 devient actif à :

```text
1115449070
```

et le premier sample où il est redevenu inactif est :

```text
1115458670
```

Différence :

```text
9600 cycles
```

À 80 MHz :

```text
120 µs
```

Comme la cadence est de 20 µs, la durée réelle exacte est quantifiée avec environ une période d'incertitude aux deux bords.

---

## D150 — activation Q2 après le retour de `endPacket()`

`endPacket()` retourne à :

```text
1115445105
```

Q2 est vu actif à :

```text
1115449070
```

Différence :

```text
3965 cycles
≈ 49,56 µs
```

### Conséquence

Le traitement de la transmission continue de façon asynchrone après le retour de :

```text
WiFiUDP::endPacket()
```

Ce comportement explique pourquoi le test v4.1 pouvait manquer la vie réelle de la queue selon sa cadence et son instant de départ.

---

## D151 — le champ backoff n'est probablement pas le compteur live

Avant l'activation Q2 :

```text
backoff = 11
```

Au moment de l'activation :

```text
backoff = 15
```

Puis :

```text
CTRL = 11 pendant ~120 µs
CTRL revient à 00
backoff reste 15
```

Le champ reste encore :

```text
15
```

après le front `SYNC` descendant et jusqu'à la fin de la capture.

### Conclusion forte

Le champ :

```text
BACKOFF[21:12]
```

se comporte comme :

```text
valeur programmée / valeur initiale
```

et non comme :

```text
compteur hardware live
```

Le vrai countdown semble rester interne au MAC.

### Révision d'une hypothèse antérieure

L'ancien « oracle » proposé :

```text
lire BACKOFF[21:12] et observer son gel/reprise
```

doit donc être **retiré comme méthode de lecture directe**.

La relation architecturale CCA → backoff hardware reste très probable, mais le compteur interne n'est pas reflété par ce champ observable.

---

## D152 — le front RF descendant ne modifie pas la valeur de backoff

Le `SYNC` passe de :

```text
1 → 0
```

vers `POST 500`.

Avant :

```text
Q2 backoff = 15
CTRL = 00
```

Après :

```text
Q2 backoff = 15
CTRL = 00
```

et ces valeurs restent identiques jusqu'à `POST 799`.

Cela confirme que la valeur 15 n'est pas un countdown suspendu qui reprendrait pendant la phase calme.

---

## D153 — nouveau type d'oracle : durée de vie de la queue active

Même si le countdown est invisible, on peut mesurer :

```text
temps entre CTRL 00→11
et CTRL 11→00
```

Cette durée inclut potentiellement :

```text
attente medium
AIFS/DIFS
backoff interne
transmission
ACK/retry
```

Elle peut donc servir d'oracle **statistique**, pas comme bit CCA instantané.

### Test nécessaire

Comparer la même TX :

```text
A — lancée au début de SYNC=1
B — lancée au début de SYNC=0
```

sur plusieurs essais.

Une durée active systématiquement plus longue pendant `SYNC=1` fournirait une preuve dynamique supplémentaire que la queue est retardée par le CCA/medium busy.

---

## D154 — probe v4.3

Le probe v4.3 ne capture plus toutes les queues.

Il échantillonne Q2 seul à :

```text
10 µs
```

pendant environ :

```text
12 ms
```

et exécute :

```text
6 paires ON/OFF
```

avec la commande :

```text
p
```

Pour chaque essai il produit :

```text
durée endPacket
latence avant CTRL=11
durée CTRL=11
backoff avant/pendant/après
nombre de valeurs backoff distinctes
état SYNC à l'activation et à la fin
```

Le but est maintenant de caractériser statistiquement le retard MAC, et non de poursuivre l'hypothèse abandonnée d'un compteur backoff CPU-readable.



# Nouvelles découvertes — v0.19 : le protocole v4.3 était contaminé

## D155 — plusieurs latences géantes sont des underflows, pas des phénomènes physiques

Le v4.3 affiche notamment :

```text
active_delay_cycles = 4294967174
active_delay_cycles = 4294967178
active_delay_cycles = 4294967170
```

Ces valeurs sont proches de :

```text
2^32
```

et correspondent à :

```text
-122 cycles
-118 cycles
-126 cycles
```

si elles sont interprétées en signé 32 bits.

### Conclusion

Dans ces essais :

```text
Q2 était déjà CTRL=11 avant txStart
```

et le calcul non signé a transformé une petite latence négative en environ :

```text
53,7 secondes
```

à 80 MHz.

Ces lignes doivent être rejetées.

---

## D156 — certains essais n'observent jamais l'activation Q2

Exemples :

```text
pair0 OFF : active=0
pair1 ON  : active=0
```

Causes possibles :

- Q2 n'a pas été utilisé pour cette TX ;
- activation postérieure à la fenêtre ;
- packet traité autrement par la pile ;
- activité précédente ou background perturbant le séquencement.

Ces essais ne peuvent pas entrer dans une comparaison de durée active.

---

## D157 — certains essais commencent bien mais ne voient pas le retour à idle

Exemples :

```text
pair1 OFF
active_delay ≈ 9,59 ms
active_samples = 241
duration = 0
```

et :

```text
pair4 ON
active_samples = 1139
duration = 0
```

Le zéro ne signifie pas :

```text
durée active = 0
```

mais :

```text
aucun front 11→00 observé avant la fin de capture
```

Ces essais doivent également être rejetés pour une comparaison de durée.

---

## D158 — sous-échantillon propre du v4.3

Essais ON avec activation et désactivation observées :

```text
pair0 ON = 110,68 µs
pair3 ON = 90,50 µs
```

Moyenne :

```text
100,59 µs
```

Essais OFF avec activation et désactivation observées :

```text
pair2 OFF = 19,38 µs
pair3 OFF = 149,99 µs
pair5 OFF = 49,38 µs
```

Moyenne :

```text
72,92 µs
```

### Interprétation

Il n'existe pas de séparation nette :

```text
ON > OFF
```

sur ce très petit échantillon.

La dispersion OFF est même très forte.

Aucune conclusion CCA ne doit être tirée de ces cinq essais.

---

## D159 — `endPacket()` n'est pas lui-même un bon oracle CCA

Moyennes sur les six lignes de chaque phase du v4.3 :

```text
ON  endPacket ≈ 528,87 µs
OFF endPacket ≈ 467,74 µs
```

L'écart est faible par rapport à la dispersion et les essais sont contaminés.

En outre :

```text
endPacket()
```

reflète beaucoup de travail logiciel/lwIP/driver et pas seulement l'attente medium.

Il ne doit pas être utilisé comme oracle CCA principal.

---

## D160 — exigences d'un essai exploitable

Un essai doit maintenant satisfaire simultanément :

```text
Q2 idle stable avant essai
CTRL=00 avant txStart
premier CTRL=11 après txStart
SYNC au moment de l'activation = phase demandée
retour CTRL=00 observé dans la capture
```

Tout essai ne satisfaisant pas ces critères est marqué invalide.

---

## D161 — probe v4.4

Commande :

```text
c
```

Le probe cherche automatiquement :

```text
8 essais ON valides
8 essais OFF valides
```

avec jusqu'à 24 tentatives par phase.

Avant chaque tentative :

```text
Q2 CTRL doit rester 00 pendant 1,5 ms
```

La fenêtre de capture est portée à :

```text
25 ms
```

à :

```text
20 µs/sample
```

pour éviter de classer trop tôt les activations retardées.

### États de rejet

```text
NO_ACTIVE
NO_CLEAR
PREACTIVE
WRONG_PHASE
PREP_FAIL
```

Seuls les essais :

```text
VALID
```

entrent dans les moyennes finales.

---

## D162 — statut actuel de l'hypothèse CCA

Après v4.3 :

- aucun bit MMIO `CCA_BUSY` direct trouvé ;
- WDEV bit8 confirmé RX ;
- backoff `[21:12]` confirmé valeur de chargement, pas countdown live ;
- Q2 CTRL[31:30] confirme l'état TX active ;
- aucune différence statistique ON/OFF encore démontrée.

La prochaine mesure doit donc être considérée comme :

```text
test statistique indirect de retard MAC sous occupation du médium
```

et non comme une lecture du signal CCA lui-même.



# Nouvelles découvertes — v0.20 : le v4.4 sous-échantillonne encore les queues

## D163 — le ratio 1,863 n'est pas interprétable

Résumé v4.4 :

```text
ON_valid  = 1
OFF_valid = 8

ON_mean_active_duration  = 139,99 µs
OFF_mean_active_duration = 75,15 µs
ratio = 1,863
```

Un ratio calculé avec un seul essai ON ne peut pas être utilisé comme preuve.

Le seul ON valide :

```text
active delay = 838,43 µs
duration     = 139,99 µs
```

est compatible avec un retard plus important, mais il n'existe pas assez de répétitions.

---

## D164 — forte asymétrie du taux de détection Q2

Phase ON :

```text
24 tentatives
1 VALID
22 NO_ACTIVE
1 PREACTIVE
```

Phase OFF :

```text
17 tentatives
8 VALID
5 NO_CLEAR
4 NO_ACTIVE
```

La queue Q2 est donc bien plus souvent visible pendant les essais OFF.

### Hypothèses possibles

1. Q2 s'active pendant ON mais le pulse est plus court que 20 µs.
2. une autre queue EDCA est utilisée.
3. la TX reste plus longtemps dans une couche software/driver avant l'armement MAC.
4. le trafic RX du générateur perturbe le scheduling software.
5. l'occupation du médium/CCA reporte effectivement l'accès.

Le v4.4 seul ne distingue pas ces cas.

---

## D165 — preuve directe que des activations sont manquées

Deux essais OFF sont particulièrement révélateurs.

### OFF attempt 4

```text
status      = NO_ACTIVE
bo_before   = 14
bo_after    = 1
bo_unique   = 2
```

### OFF attempt 12

```text
status      = NO_ACTIVE
bo_before   = 11
bo_after    = 0
bo_unique   = 2
```

Le champ backoff est reprogrammé mais :

```text
CTRL=11
```

n'a jamais été observé.

### Conclusion

L'activation/désactivation complète de Q2 a pu se produire entre deux échantillons de 20 µs.

Le statut :

```text
NO_ACTIVE
```

ne signifie donc pas nécessairement :

```text
aucune transmission MAC
```

---

## D166 — des activations valides ne durent qu'un sample

Plusieurs essais OFF valides donnent :

```text
20 µs
```

avec :

```text
active_samples = 1
```

La vraie durée peut être comprise approximativement entre :

```text
presque 0
et
< 40 µs
```

selon l'alignement aux samples.

Cela confirme que :

```text
20 µs
```

est une cadence insuffisante pour classer correctement tous les pulses.

---

## D167 — les NO_CLEAR révèlent aussi des états très longs

Plusieurs essais OFF donnent :

```text
NO_CLEAR
active_samples ≈ 1220
```

à 20 µs/sample.

Cela correspond à environ :

```text
24,4 ms
```

d'état Q2 observé actif sans retour à zéro dans la fenêtre restante.

### Conséquence

La durée de vie Q2 possède une distribution extrêmement large :

```text
<20 µs
jusqu'à >20 ms
```

Cette dispersion peut intégrer :

- attente medium ;
- retries ;
- ACK ;
- scheduling du MAC ;
- trafic background.

La durée Q2 n'est donc pas un proxy simple du CCA instantané.

---

## D168 — le logger v4.5 observe toutes les queues

Le nouveau probe lit simultanément :

```text
Q0 CTRL[31:30]
Q1 CTRL[31:30]
Q2 CTRL[31:30]
Q3 CTRL[31:30]
```

à une cadence Timer1 nominale de :

```text
5 µs
```

Il ne stocke pas tous les samples.

Il enregistre uniquement :

```text
changement d'un CTRL
changement SYNC
```

et capture au même instant :

```text
backoff Q0..Q3
```

### Avantages

- bien moins de RAM ;
- pulses courts moins faciles à manquer ;
- identification d'un éventuel changement de queue ;
- fenêtre longue sans énorme CSV.

---

## D169 — mesure de la qualité réelle du Timer1

Le v4.5 imprime aussi :

```text
isr_count
max_isr_gap_us
```

Le but est de vérifier que demander 5 µs ne signifie pas forcément obtenir réellement 5 µs sous charge Wi-Fi.

Si :

```text
max_isr_gap_us
```

devient très grand, un pulse peut encore être manqué.

Cette métrique est donc nécessaire avant toute conclusion négative.

---

## D170 — prochaine décision expérimentale

Commande :

```text
e
```

Le probe réalise :

```text
1 essai ON
1 essai OFF
```

et imprime uniquement les transitions.

Les questions prioritaires deviennent :

1. quelle queue passe réellement à `CTRL=11` ?
2. combien de temps dure chaque pulse ?
3. les essais ON déplacent-ils la TX vers une autre queue ?
4. le Timer1 tient-il réellement sa cadence sous trafic ?
5. un pulse peut-il être associé au passage RF ON/OFF plutôt qu'à une TX ?


# 29. Tests matériels à conserver pour la fin

Aucun test matériel n’est requis pour la phase actuelle.

Quand l’analyse statique sera épuisée, un firmware de probe pourra capturer :

```text
timestamp CPU
RX_IQ_0
RX_IQ_1
RX_IQ_2
RX_IQ_3
BACKOFF_0
CTRL_0
NOISE
WDEV_EVENT
IQ_EST / E4
```

avec un deuxième ESP générant un motif RF déterministe.

Le résultat recherché serait du type :

```text
             OFF      ON      OFF
RX_IQ_0       17      122      16
backoff      218      218     217
             217      218     216
             216      218     215
                      ↑
                compteur gelé
```

Cette étape est explicitement repoussée après les recherches statiques.

---

# 30. Critère de succès final

Deux résultats sont acceptables.

## A. Solution idéale

Trouver :

```text
REG bit = CCA_BUSY
```

avec lecture instantanée, faible coût CPU et sans acquisition IQ_EST.

## B. Solution alternative

Trouver :

```text
REG = puissance RX live
```

avec une fréquence de lecture suffisamment élevée pour fabriquer directement une sortie DATA / détecteur OOK.

---

# 31. État des conclusions

## Confirmé

- `0x60009B00 bit28` contrôle le CCA.
- `bit28 = 0` → CCA actif.
- `bit28 = 1` → CCA désactivé.
- `rom_chip_v5_sense_backoff()` touche `0x60009C28` et `0x60009D24`.
- `rom_get_noisefloor()` lit `0x60009B64`.
- `wDev_EnableTransmit()` programme un backoff matériel.
- `0x3FF20DC0 [21:12]` contient la valeur de backoff pour la file 0.
- `0x3FF20DC4 bits31:30` est lié au contrôle TX.
- `ic_get_rssi()` n’est pas un RSSI RF live générique.
- `rom_get_corr_power()` lit directement `0x60000580..0x6000058C` ainsi que `0x600005DC/E0/E4`.
- `wDev_ProcessFiq()` teste `0x3FF20C20` puis lit ses flags de statut dans `0x3FF20C24`.

## Très probable

- Le CCA est consommé directement par le MAC hardware.
- Les fonctions PWDET sont fortement associées au contrôle/calibration de puissance TX ; leur implication directe dans le CCA RX n'est plus supposée.
- `chip_v6_set_sense()` / `chip_v6_get_sense()` sont des routines v6 minuscules et probablement triviales.
- Le SDK possède une primitive distincte `read_hw_noisefloor()` en plus du pipeline `start/get/check`.
- `libpp.a` possède un mécanisme périodique de mesure de bruit (`pp_noise_test`, timer, `noise_now`).
- `sdt_on_noise_start()` existe dans `phy_chip_v6.o` et relie explicitement une entité `sdt` au démarrage du noise subsystem.
- `read_hw_noisefloor()` est appelée dans un chemin de réception par polling publié sous le nom `do_rx_poll()`.
- `RX_IQ_0..3` sont immédiatement suivis par `RX_GAIN_CTL`, renforçant leur appartenance au chemin RX hardware.
- `0x60009D44` bits 29/26 sont modifiés par l'entrée/sortie du mode sniffer.
- `0x3FF20C18` est le registre officiel d'activation des événements WDEV.
- `BIT27` de la fabric WDEV correspond à un événement timer/TSF, donc il est éliminé comme candidat CCA.
- `wDev_ProcessFiq()` traite aussi des événements RX réussis dans les SDK plus récents.
- aucune interruption nommée `CCA_INT` / `CCA_BUSY_INT` n'a été retrouvée dans les sources publiques analysées.
- l'hypothèse prioritaire devient : **bit CCA lisible mais transitions non interrompues**, consommées directement par le backoff hardware.
- dans la table PHY runtime publiée, les trois fonctions CCA restent sur les implémentations ROM alors que plusieurs fonctions noise/RX sont patchées en RAM ;
- `0x3FF20C00` est officiellement utilisé par Espressif comme `WDEV_NOW()`, confirmant la nature MAC/WDEV de la banque `0x3FF20Cxx` ;
- `0x3FF20CB0` et `0x3FF20CC0` sont associés respectivement aux waiting queues et aux collisions dans le reverse-engineering communautaire ;
- la séparation la plus probable est maintenant : **CCA/status primaire dans `0x60009xxx`, événements dérivés dans `0x3FF20Cxx`**.
- l'analyse directe du dump ROM confirme les accès `B00/C28/D24` du CCA et fournit une carte exhaustive des utilisateurs directs de la base `0x60009A00`;
- aucun des utilisateurs directs ROM de `0x60009A00` ne révèle un second registre évident de statut `CCA_BUSY`;
- `0x60009B60` est un registre de contrôle noise-floor : `start_noisefloor` y positionne les bits 17, 15 et 1 (`0x00028002`);
- une trace QEMU indépendante reproduit exactement l'écriture `0x00028002` vers l'offset `0x09B60`;
- `rom_get_corr_power()` et ses sept lectures hardware sont désormais confirmés directement dans le dump utilisateur.
- `wDev_Initialize()` programme `INT_ENA_WDEV = 0x2C9F0300`, ce qui donne la liste exacte des événements WDEV normaux.
- `0x3FF20C1C` est très fortement identifié comme registre RAW, `0x3FF20C20` comme status/pending masqué et `0x3FF20C24` comme clear/ack.
- tous les bits WDEV normaux activés sont rattachés à des chemins RX/TX/collision/timer/timeout/diagnostic ; aucun candidat naturel `CCA_INT` ne reste.
- les bits 2/3 sont ajoutés uniquement en mode sniffer et alimentent les handlers RX sniffer.
- `lmacIsIdle()` et `ppCheckTxIdle()` sont des tests d'état logiciel MAC/TX et sont éliminés comme substituts CCA.
- le modèle principal devient : **CCA_BUSY interne ou MMIO lisible, directement consommé par le moteur backoff, sans ISR dédiée en régime normal**.
- `RX_IQ_0..3` sont des métriques de corrélation / puissance RX.

## Non résolu

- Adresse et masque du véritable `CCA_BUSY`.
- Nature exacte des registres PWDET.
- Mise à jour continue ou non de `RX_IQ_0..3`.
- Nature exacte de `0x600005DC/E0/E4`.
- Lisibilité live du compteur de backoff.
- Existence d’un événement WDEV/NMI associé au CCA.

---

# 32. Journal des versions

## v0.20 — 2026-09-15

Analyse v4.4 :

- ratio ON/OFF 1,863 rejeté comme non significatif : 1 ON valide seulement ;
- ON : 1 VALID / 24 tentatives ; OFF : 8 VALID / 17 ;
- preuve que 20 µs manque des activations : des essais `NO_ACTIVE` changent pourtant de valeur backoff ;
- plusieurs activations valides ne durent qu'un seul sample ;
- plusieurs `NO_CLEAR` restent actifs ~24 ms ou plus ;
- durée Q2 extrêmement dispersée, donc mauvais proxy simple du CCA ;
- création du probe v4.5 : logger événementiel 5 µs nominal, quatre queues simultanément, stockage uniquement sur changement CTRL/SYNC, mesure du max ISR gap.

## v0.19 — 2026-09-15

Analyse du probe v4.3 :

- plusieurs `active_delay` proches de `2^32` identifiés comme underflow de petites latences négatives ;
- preuve que Q2 était parfois déjà actif avant `txStart` ;
- essais `NO_ACTIVE` et `NO_CLEAR` non exploitables pour une comparaison de durée ;
- sous-échantillon propre : ON = 110,68 / 90,50 µs ; OFF = 19,38 / 149,99 / 49,38 µs ;
- aucune séparation ON/OFF concluante ;
- `endPacket()` moyen ON ≈ 528,87 µs, OFF ≈ 467,74 µs, non concluant et trop logiciel ;
- création du probe v4.4 avec filtre d'idle stable, latences signées, rejet des essais contaminés, fenêtre 25 ms et retries automatiques.

## v0.18 — 2026-09-15

Résultat du probe Timer1 v4.2 :

- Q2 identifié comme queue utilisée par la TX UDP du probe ;
- première observation directe `Q2 CTRL[31:30] : 00 → 11 → 00` ;
- validation expérimentale de la reconstruction `wDev_EnableTransmit()` ;
- Q2 backoff passe de 11 à 15 au moment de l'armement ;
- état CTRL=11 observé pendant environ 120 µs ;
- activation Q2 observée ~49,6 µs après le retour de `endPacket()` ;
- le champ backoff reste à 15 après désarmement et après `SYNC=0` ;
- conclusion renforcée : `BACKOFF[21:12]` est une valeur de chargement, pas le countdown live ;
- retrait de l'ancien oracle « lire directement le gel/reprise du compteur » ;
- création du probe v4.3 pour comparer statistiquement la durée active Q2 pendant RF ON vs RF OFF.

## v0.17 — 2026-09-15

Analyse du test backoff actif `b` :

- les quatre champs backoff restent constants sur toute la capture ;
- front `SYNC` descendant correctement observé au sample 310 ;
- Q0=1 et Q2=13 restent identiques avant/après RF ON ;
- `endPacket()` prend 68063 cycles ≈ 850,79 µs ;
- le premier sample arrive ≈ 880,91 µs après le front montant, donc après le retour de `endPacket()` ;
- Q0_CTRL et Q2_CTRL ont déjà bits31:30=00 au premier sample ;
- le test v4.1 a donc très probablement raté la fenêtre où la queue TX était réellement armée ;
- création du probe v4.2 avec Timer1 à 20 µs, qui sample les queues pendant l'exécution bloquante de `endPacket()`.

## v0.16 — 2026-09-15

Résultat du scan WDEV haute vitesse `w` :

- `WDEV_RAW bit8 (0x100)` apparaît transitoirement uniquement pendant la rafale RF ;
- `RAW_ON_ONLY=0x100` et `RAW_CHANGED_ONLY_ON=0x100` ;
- bit8 était déjà cartographié statiquement comme événement RX dans `wDev_ProcessFiq()` ;
- l'observation expérimentale valide donc le mapping RX, pas un `CCA_BUSY` ;
- `C20` montre aussi un pulse bit8 pendant PRE, probablement activité RX/beacon, mais est zéro pendant ON ;
- aucune nouvelle IRQ/RAW CCA cachée n'est révélée ;
- le scan haute vitesse est néanmoins validé comme capable d'attraper des pulses WDEV ;
- prochaine étape : commande `b` pour tester un vrai compteur backoff avec TX pending.

## v0.15 — 2026-09-15

Analyse complète de la capture v3 :

- fenêtre ON/OFF complète confirmée ;
- 405 samples RF ON et 299 RF OFF ;
- durée SYNC HIGH mesurée : 10,132675 ms ;
- cadence médiane : 1998 cycles ≈ 24,975 µs ;
- corrélation ON/OFF exactement nulle sur tous les champs capturés ;
- `WDEV_RAW=0x020094CF`, `WDEV_STATUS=0`, `BACKOFF=3`, `RXIQ=0`, `RX_GAIN=0`, `noise_raw=0xD24`, `noise_hw=-366` constants ;
- différence inter-captures de WDEV_RAW sur bits5/11, mais aucune corrélation intra-capture avec CCA ;
- décodage expérimental `signed12(0xD24)=-732`, puis `>>1 = -366` pour `read_hw_noisefloor()` ;
- création du probe v4 :
  - commande `w` : scan WDEV RAW/status en boucle serrée avec OR/AND/change masks ;
  - commande `b` : transmission UDP pending + capture des quatre queues backoff/CTRL.

## v0.14 — 2026-09-14

Première campagne hardware synchronisée :

- front `SYNC` correctement capturé ;
- découverte d'un trou artificiel de 8866 cycles causé par la copie du pré-trigger dans le probe v2 ;
- fenêtre v2 trop courte : aucun front descendant de la rafale 10 ms ;
- `B60/B64/D68` constants, cohérents avec leur rôle de configuration ;
- `RX_IQ_0..3` et `RX_GAIN` restent à zéro dans ce contexte ;
- `WDEV_STATUS (C20)` toujours zéro ;
- `WDEV_RAW (C1C)` constant à `0x02009CEF`, sans corrélation au front observé ;
- `BACKOFF_0[21:12] = 7` constant, mais test non concluant faute de TX actif côté probe ;
- `0x60009824 = 0xD2C` constant sur la courte fenêtre ;
- création du probe v3 : pas de trou au trigger, 16 ms de fenêtre, cadence nominale 25 us, appel direct à `read_hw_noisefloor()`.

## v0.13 — 2026-09-14

Fermeture des dernières pistes statiques importantes :

- `phy_version_print()` confirme directement PHY **1156** ;
- corrélation avec les release notes officielles Espressif : PHY 1156 est la révision introduite avec NonOS 3.0.3 / AT 1.7.3 ;
- reconstruction de la base réelle de la table `g_phyFuns` (`.data + 4`) ;
- `phy_enable_agc()` dispatch vers `rom_chip_v5_enable_cca()` ;
- `phy_disable_agc()` dispatch vers `rom_chip_v5_disable_cca()` ;
- `phy_set_sense()` dispatch vers `chip_v6_set_sense()` ;
- `chip_v6_set_sense()` et `chip_v6_get_sense()` confirmés byte pour byte comme simples `ret.n` ;
- piste `sense` définitivement éliminée ;
- scan symbolique des MMIO dynamiques de tout `libphy.a` : aucun accès caché dans les banques PHY CCA/RX ;
- scan dynamique `libpp.a` : seulement queues/descripteurs TX/WDEV connus, aucun nouveau candidat CCA ;
- conclusion renforcée : si un `CCA_BUSY` MMIO existe, il n'est vraisemblablement pas utilisé par la pile Espressif PHY 1156.

## v0.12 — 2026-09-14

Recherche systématique de statuts hardware dans les binaires fournis :

- scan `MMIO read → bit/champ → branch` sur tout `libphy.a` ;
- aucun test conditionnel direct retrouvé sur le bloc CCA `B00/B64/C28/D24/D68` ;
- `0x60009B60 bit1` identifié comme statut d'activité/busy de la procédure noise-floor ;
- `B60 bits15/17` confirmés comme état de lancement/configuration de cette mesure ;
- découpage fonctionnel de `0x60009B64` :
  - `[31:20]` résultat noise-floor ;
  - `[19:12]` paramètre CCA ;
  - `[11:9]` noise-check cfg ;
  - `[8:0]` noise-floor cfg/encoding ;
- forte indication d'une unité interne noise-floor de 1/4 dB grâce aux bornes `-392..-340` et à la conversion `/4` dans `libpp` ;
- `0x60009800` rattaché à EVM/frequency-offset, éliminé comme CCA ;
- `0x60009824` confirmé comme lecture hardware noise-floor v6 ;
- `0x60009830` rattaché ADC/random/calibration ;
- `RX_IQ_0..3` et `0x600005E4` confirmés comme métriques/calibration IQ, pas comme flags BUSY ;
- `pm_check_mac_idle()` analysé et éliminé comme substitut de `CCA_FREE` ;
- conclusion renforcée : aucune lecture CPU directe du CCA BUSY/FREE dans ROM + libphy + libpp.

## v0.11 — 2026-09-14

Poursuite du reverse-engineering `libphy.a` :

- correction de l'hypothèse `0xB4 = -76 dBm` : elle n'est plus retenue ;
- `phy_bb_rx_cfg()` initialise `0x60009B64[19:12] = 0x22` et `[11:0] = 0xFA6` ;
- provenance exacte du paramètre CCA :
  - `init_data[84] → chip6_phy_init_ctrl[0x3A] → B64[19:12]` ;
  - `init_data[88] → chip6_phy_init_ctrl[0x3B] → sélecteur mode CCA` ;
- reconstruction des modes :
  - mode 3 : programme B64 sans forcer `D68.bit18` ;
  - mode 4 : programme B64 puis force `D68.bit18` ;
- l'ancien `ESP8266_RF_init.xls` marque encore 84/88 comme `Reserved`, malgré leur utilisation CCA réelle dans le binaire ;
- scan global `0x60009D60–0x60009D74` :
  - aucun registre n'est lu comme status direct ;
  - `D68/D70` sont configuration RMW ;
  - `D60/D64/D74` sont write-only dans les chemins observés ;
- aucune lecture directe de `CCA_BUSY` retrouvée dans la banque `0x60009A00–0x60009DFF` ;
- `chip_v6_rxmax_ext_dig()` relie `RX_GAIN_CTL` au recalcul du comparateur RX ;
- `set_cca()` n'a aucun caller direct par relocation dans les `libphy.a/libpp.a` fournis ;
- conclusion renforcée : le comparator CCA est configuré par CPU, mais sa sortie BUSY/FREE semble consommée directement en hardware.

## v0.10 — 2026-09-14

Analyse directe du `libphy.a` utilisateur :

- découverte et reconstruction de `set_cca()` ;
- `set_cca()` programme `0x60009B64[19:12]` et force `0x60009D68 bit18` ;
- valeur par défaut du champ CCA : `0xB4` (hypothèse intéressante : -76 si codage signé, unité non prouvée) ;
- `chip_v6_set_chan_rx_cmp()` programme `0x60009D68[17:10]` via le masque `0xFFFC03FF` ;
- découverte d'accès `RX compare` à `0x60009A34` ;
- correction importante : `read_hw_noisefloor()` RAM v6 lit `0x60009824`, pas directement `0x60009B64` ;
- `noise_check_loop()` modifie `0x60009B64[11:9]` et lit `0x60009B60` ;
- `sdt_on_noise_start()` manipule uniquement `0x60009B60` parmi les registres PHY directs ;
- `register_chipv6_phy()` confirme l'initialisation du couple `0x60009B64 / 0x60009D68` ;
- nouveau candidat principal pour le bloc CCA/comparateur : `0x60009D68`.

## v0.9 — 2026-09-14

Analyse approfondie du `libpp.a` :

- confirmation globale que le champ `BACKOFF_n` à `base+0x3C0` est écrit mais jamais relu par le SDK ;
- `CTRL_n +0x3C4` confirmé comme chemin TX via bits31/30, pas comme CCA ;
- deux appels exacts à `read_hw_noisefloor()` identifiés dans `pp.o` ;
- `pp_enable_noise_timer()` appelle `noise_check_loop(1,1)` puis maintient `noise_now = (read_hw_noisefloor()+2)>>2` ;
- `ppRxProtoProc()` rafraîchit aussi `noise_now` depuis le hardware pendant le traitement RX normal ;
- `NoiseTimerInterval=100` et `noise_now=23` retrouvés dans les initialiseurs de `.data` ;
- `pp_noise_test()` identifié comme wrapper de `pp_enable_noise_timer()` ;
- séparation renforcée entre métrique noise-floor et statut instantané CCA ;
- conclusion renforcée : CSMA/backoff est autonome en hardware et ne nécessite aucun polling CCA par le CPU ;
- prochaine frontière statique : `libphy.a`, en priorité `noise_check_loop`, `sdt_on_noise_start` et `chip_v6_set_chan_rx_cmp`.

## v0.8 — 2026-09-14

Analyse directe du `libpp.a` utilisateur :

- extraction et analyse de `wdev.o`, `lmac.o`, `pp.o` avec DWARF non stripé ;
- correction du bloc WDEV :
  - `0x3FF20C18` = enable ;
  - `0x3FF20C1C` = raw events ;
  - `0x3FF20C20` = status/pending masqué ;
  - `0x3FF20C24` = clear/ack ;
- `wDev_Initialize()` programme exactement `0x2C9F0300` dans C18 ;
- cartographie de tous les bits du masque normal vers les chemins RX/TX/collision/timers/timeout/diagnostic ;
- bits2/3 identifiés comme événements sniffer activés uniquement par `wdev_go_sniffer()` ;
- forte élimination de l'hypothèse d'une ISR CCA dédiée en mode normal ;
- confirmation directe de `0x60009D44` dans le chemin sniffer ;
- seulement deux littéraux `0x60009A00` dans toute l'archive, tous deux dans le chemin sniffer ;
- aucun littéral direct `0x60009B00/B64/C28/D24` dans `libpp.a` ;
- `lmacIsIdle()` et `ppCheckTxIdle()` éliminés comme lecteurs CCA.

## v0.7 — 2026-09-14

Première analyse directe du **dump ROM brut utilisateur** :

- validation du fichier 64 KiB et de son SHA-256 ;
- découverte que le littéral `0x60009A00` n'existe qu'une fois dans la ROM ;
- reconstruction des dix xrefs `L32R` vers cette base ;
- cartographie directe des registres `B00`, `B08`, `B14`, `B60`, `B64`, `C28`, `C70`, `D24`;
- aucun registre de statut CCA évident supplémentaire dans les utilisateurs ROM directs de cette base ;
- décodage exact du contrôle `0x60009B60` :
  - `set_noise_floor`: bits17/15=0, bit1=1 ;
  - `start_noisefloor`: OR `0x00028002`, donc bits17/15/1=1 ;
- confirmation indépendante par une trace QEMU du write `0x09B60 = 0x00028002`;
- confirmation des masques `C28[16:10]` et `D24[7:1]`;
- confirmation directe des sept lectures de `rom_get_corr_power()` depuis le dump ;
- conclusion renforcée : le CCA BUSY semble être un état consommé sous le niveau ROM CPU, avec ISR dédiée toujours non démontrée.

## v0.6 — 2026-09-14

Recherche centrée sur la séparation **registre CCA primaire** / **fabric d'interruptions WDEV** :

- découverte importante : dans la table PHY runtime publiée, `disable_cca`, `enable_cca` et `sense_backoff` restent les routines ROM, contrairement à plusieurs primitives noise/RX remplacées en RAM ;
- confirmation officielle de `0x3FF20C00` comme `WDEV_NOW()` ;
- consolidation de `0x3FF20Cxx` comme banque MAC/WDEV contenant temps, interruption, waiting queues et collisions ;
- réévaluation de `0x3FF20C20/24` comme couple d'états d'interruption dont la sémantique RAW/STATUS exacte reste non prouvée ;
- clarification de la différence entre `ETS_WDEV_INUM` et la numérotation architecturale Xtensa/NMI ;
- confirmation qu'aucun symbole historique LMAC/WDEV n'expose explicitement CCA/carrier-sense ;
- ajout du chemin collision comme second oracle indépendant du compteur backoff ;
- priorité maximale donnée aux lectures conditionnelles dans `0x60009Bxx–0x60009Dxx`.

## v0.5 — 2026-09-14

Recherche spécifiquement centrée sur **CCA status + ISR/FIQ** :

- identification officielle de `INT_ENA_WDEV = 0x3FF20C18`;
- confirmation de `BIT27` comme événement TSF/MacTim, donc non-CCA;
- séparation prudente du trio `0x3FF20C18 / 0x3FF20C20 / 0x3FF20C24`;
- confirmation qu'un SDK NonOS récent route un RX-success via `wDev_ProcessFiq() → wDev_ProcessRxSucData()`;
- identification dans les release notes Espressif des événements `Q2_RTS_INT` et `Q0_TX_COMPLETE`;
- aucune preuve publique retrouvée d'un `CCA_INT` dédié;
- nouvelle stratégie déterministe : mapper tous les bits WDEV connus et inspecter uniquement les bits orphelins;
- maintien de la recherche parallèle du véritable bit MMIO `CCA_BUSY` dans la banque Wi-Fi/PHY.

## v0.4 — 2026-09-14

Nouvelles découvertes :

- identification de `sdt_on_noise_start()` dans `phy_chip_v6.o`; piste prioritaire, sans attribuer prématurément une signification à l'acronyme `SDT`;
- confirmation qu'un chemin RX de debug/polling Espressif appelle directement `read_hw_noisefloor()`;
- extension de la carte PHY : `RX_IQ_0..3` sont contigus à `RX_GAIN_CTL`, ce qui impose de considérer le gain RX dans l'interprétation des valeurs;
- découverte de `0x60009D44` bits 29/26 comme configuration modifiée par le mode sniffer;
- confirmation officielle des petites fenêtres ROM pour `rom_set_noise_floor()` et `rom_start_noisefloor()`.

## v0.3 — 2026-09-14

Nouvelles découvertes :

- la famille `chip_v6_set_sense/get_sense` est rétrogradée : les deux routines n'occupent que quelques octets et `set_sense` est remplacé par un simple `ret.n` dans un firmware tiers ;
- `read_hw_noisefloor()` est identifié comme primitive distincte, compacte, séparée de `ram_start_noisefloor()` et `noise_check_loop()` ;
- le packet processor maintient un pipeline périodique de mesure de bruit via `pp_noise_test`, `NoiseTimerInterval`, `noise_now` et `pend_flag_noise_check` ;
- séparation désormais explicite entre valeur hardware immédiate du bruit et valeur logicielle périodique/cache ;
- rappel important : le SDK v6 remplace plusieurs primitives ROM v5 de noise floor par des variantes RAM ;
- `ram_get_corr_power()` semble plus complexe que la primitive ROM et ne doit pas être assimilé à une simple lecture des quatre `RX_IQ`.

## v0.2 — 2026-09-14

Nouvelles découvertes :

- `wDev_ProcessFiq()` utilise `0x3FF20C24` comme registre de flags/status après avoir testé `0x3FF20C20`.
- PWDET est rétrogradé comme piste CCA : les symboles `libphy.a` l'associent fortement à la boucle de contrôle de puissance TX.
- découverte de la famille `phy_set_sense` / `chip_v6_set_sense` / `chip_v6_get_sense`, à analyser séparément.

## v0.1 — 2026-09-14

Création du journal consolidé à partir de toutes les découvertes déjà accumulées.

Les prochaines versions devront uniquement ajouter des éléments nouveaux, avec pour chaque découverte :

- source ;
- fonction concernée ;
- registre/adresse ;
- désassemblage pertinent ;
- niveau de confiance ;
- conséquence pour RF2400 ;
- impact sur les hypothèses existantes.

