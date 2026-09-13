# ESP8266 — Reverse-engineering du Wi‑Fi, du PHY et du chemin RF

**Document technique vivant — version 0.27**  
**Date de création : 2026-09-12**  
**Objet :** cartographier le PHY/RF de l’ESP8266 et exploiter directement ses blocs RF hors protocole Wi‑Fi. Le modèle logiciel TX **OOK/ASK/M-ASK** est désormais fermé ; la cible principale devient la **réception autonome OOK/ASK** via la chaîne RX RF/baseband, `IQ_EST`, CCA et noise-floor, sans remettre en service la pile Wi‑Fi 802.11.

> **Règle de maintenance du document**
>
> Ce document est destiné à être mis à jour à chaque nouvelle découverte concernant ce projet. Les faits établis, les hypothèses, les corrections et les questions ouvertes doivent rester clairement séparés.

---

## 1. Résumé exécutif

L’analyse des bibliothèques binaires du SDK ESP8266 ainsi que d’un dump complet de la mask-ROM montre que l’ESP8266 n’est pas un simple microcontrôleur pilotant un contrôleur Wi‑Fi autonome.

L’architecture observée ressemble plutôt à :

```text
Application / lwIP
       │
       ▼
libnet80211
       │
       ▼
libpp / LMAC / rate control
       │
       ▼
wDev
       │
       ▼
MAC matériel / séquenceurs
       │
       ▼
libphy + PHY mask-ROM
       │
       ▼
Digital baseband
       │
       ▼
RF analogique
```

Une quantité importante de logique Wi‑Fi est exécutée directement par le **LX106** :

- management 802.11 ;
- association/authentification ;
- scheduling ;
- rate control ;
- politique de retry ;
- choix de CW/AIFS/backoff ;
- gestion des ACK/CTS timeout ;
- gestion des collisions ;
- calibration RF ;
- réglage de puissance TX ;
- contrôle du PLL ;
- contrôle des gains RX/TX ;
- loopback interne ;
- calibration I/Q ;
- configuration d’un générateur de tone interne.

Le matériel prend principalement en charge les fonctions nécessitant des timings stricts ou un traitement à haute cadence :

- CCA instantané ;
- timers SIFS/AIFS ;
- countdown du backoff ;
- auto-ACK ;
- gestion temps réel TX/RX ;
- modulation/démodulation DSSS/CCK/OFDM ;
- FFT/FEC/interleaving ;
- datapath ADC/DAC et RF.

La mask-ROM de 64 KiB n’est pas seulement une boot-ROM : elle contient une véritable **bibliothèque PHY/RF** exécutée par le LX106.

Une découverte particulièrement importante est l’existence d’un **générateur de tone numérique multi-slot** contrôlé par les registres :

```text
0x600005B8
0x600005BC
0x600005C4
```

avec, à haute confiance :

- **bit 18 : gate du tone dans le mode normal (`mode_code=1`) ; `rom_stop_tx_tone()` n’efface que ce bit** ;
- **bits 17:10 : code 8 bits égal à `(-digital_scale) mod 256` ; le logiciel démontre le codage, mais pas encore la loi exacte code→puissance RF** ;
- **partie basse : `tone_control` est ORé brut dans le mot du slot ; les valeurs observées `8` et `64` occupent les bits bas, mais la ROM ne lui applique aucun masque `0x3FF`** ;
- **partie haute autour de bit18 : `mode_code` est décalé brut de 18 bits ; TXIQ manipule en plus directement des bits de test supérieurs. Le découpage 10+8+10 reste un modèle de packing très utile, mais n’est plus présenté comme une largeur imposée par l’API ROM** ;
- **bit 28 du slot 3 : copie de `enable_3`, conservée comme configuration/armement sticky ; rôle physique exact encore ouvert**.

Ce générateur est actuellement le meilleur candidat architectural pour comprendre une modulation d’amplitude/OOK primitive sans couper entièrement le PLL, les clocks et la chaîne RF.

La puce possède en outre **deux voies internes de mesure désormais démontrées** : le loopback RX avec `IQ_EST` (corrélation/puissance/mismatch) et le détecteur TX via `rom_txtone_linear_pwr()`/SAR. Elles pourraient servir à caractériser certains états ON/OFF du tone avant la validation RF externe, sans que cette utilisation hors calibration soit encore démontrée.

---

# 2. Corpus analysé

## 2.1 Fichiers fournis

| Fichier | Taille | SHA‑256 |
|---|---:|---|
| `libnet80211.a` | 1,004,540 octets | `7725959a1570633cd91ce034144da44c783011ec1353aea0c509d2a6e813f94f` |
| `libphy.a` | 172,184 octets | `93853fa02cbd4c1ed18bc38562cd1489e7024ceac0a9c5082fa3732f5a67f99a` |
| `libpp.a` | 579,580 octets | `9b4865045bcc78116e7e8f042e1993b9376dd9659290d17c0ace468b50a3d36d` |
| `esp8266_rom.bin` | 65,536 octets | `32f199bf10da9a08c6c4e1b556b29c7606656b8a6d04e4f17d5afa74e5d98c68` |

Le dump ROM couvre :

```text
0x40000000 → 0x4000FFFF
```

soit exactement 64 KiB.

## 2.2 Références publiques utilisées pour recoupement

Les conclusions ont été recoupées avec plusieurs sources publiques :

- linker ROM officiel Espressif `eagle.rom.addr.v6.ld` ;
- `rom.ld` du core Arduino ESP8266 ;
- reverse-engineering de la boot ROM par Trebisky ;
- headers `phy_regs.h` / `wdev_regs.h` d’`esp-open-rtos` ;
- symboles de ROM de générations Espressif ultérieures pour comparaison architecturale.

Références principales :

```text
https://github.com/espressif/ESP8266_NONOS_SDK/blob/master/ld/eagle.rom.addr.v6.ld
https://github.com/esp8266/Arduino/blob/master/bootloaders/eboot/rom.ld
https://raw.githubusercontent.com/trebisky/esp8266/master/reverse/bootrom/boot.txt
https://github.com/pvvx/MinEspSDKLib/blob/master/include/bios/rom_phy.h
```

---

# 3. Légende de confiance

Pour éviter de mélanger faits et hypothèses :

| Niveau | Signification |
|---|---|
| **Certain / ~99 %** | code, symbole, relocation ou comportement directement démontré |
| **Très élevé / >95 %** | plusieurs indices indépendants concordants |
| **Élevé / 80–95 %** | forte inférence architecturale, pas encore totalement prouvée |
| **Moyen / 50–80 %** | hypothèse plausible nécessitant validation |
| **Ouvert** | rôle encore inconnu |

---

# 4. Carte mémoire pertinente

## 4.1 ROM

```text
0x40000000 – 0x4000FFFF
```

Mask-ROM contenant :

- boot ;
- libc/primitives système ;
- crypto ;
- primitives PHY ;
- calibration RF ;
- contrôle I²C analogique ;
- PBUS ;
- générateur de tone ;
- estimateur I/Q.

## 4.2 Région WDEV / MAC

Base observée :

```text
0x3FF20000
```

Zone importante :

```text
0x3FF20A00
```

Utilisée par :

- `wDev_EnableTransmit()` ;
- `wDev_DisableTransmit()` ;
- queues TX ;
- collisions ;
- états TX ;
- FIQ Wi‑Fi.

## 4.3 Région PHY / RF

Plusieurs primitives utilisent des registres autour de :

```text
0x600005xx
```

Notamment :

```text
0x6000057C   IQ estimator control
0x60000580   correlation/result 0
0x60000584   correlation/result 1
0x60000588   correlation/result 2
0x6000058C   correlation/result 3

0x60000594   PBUS command
0x600005A0   PBUS status

0x600005B8   tone slot 1
0x600005BC   tone slot 2
0x600005C4   tone slot 3

0x600005DC   accumulator / DC I
0x600005E0   accumulator / DC Q
0x600005E4   total power / energy
```

---

# 5. Architecture Wi‑Fi reconstruite

```text
                       LX106
┌──────────────────────────────────────────────┐
│ lwIP / TCP-IP                                │
│                                              │
│ libnet80211                                  │
│  - auth / assoc                             │
│  - scan / beacon                            │
│  - 802.11 management                        │
│  - crypto framing                           │
│                                              │
│ libpp                                        │
│  - queues                                   │
│  - LMAC                                     │
│  - rate control                             │
│  - retry policy                             │
│                                              │
│ wDev                                         │
│  - FIQ                                      │
│  - descriptors                              │
│  - programmation MAC hardware               │
└──────────────────────┬───────────────────────┘
                       │ MMIO
═══════════════════════╪════════════════════════
                  MAC HARDWARE
═══════════════════════╪════════════════════════
                       │
          CCA / timers / backoff / auto ACK
                       │
═══════════════════════╪════════════════════════
                       │
┌──────────────────────▼───────────────────────┐
│ libphy + PHY mask-ROM                        │
│                                              │
│ PLL / gains / calibration / I-Q              │
│ tone generator / loopback / IQ estimator     │
└──────────────────────┬───────────────────────┘
                       │
═══════════════════════╪════════════════════════
              DIGITAL BASEBAND
═══════════════════════╪════════════════════════
 DSSS / CCK / OFDM / FFT / FEC / correlators
                       │
═══════════════════════╪════════════════════════
                    RF ANALOG
════════════════════════════════════════════════
 mixer / PLL / LNA / PA / ADC / DAC / antenna
```

---

# 6. `libnet80211.a`

`libnet80211.a` contient le haut niveau du protocole 802.11.

Fonctions observées :

```text
ieee80211_output_pbuf
ieee80211_send_mgmt
ieee80211_send_probereq
ieee80211_hostap_send_beacon
sta_recv_assoc
scan_parse_beacon
scan_start
ieee80211_crypto_encap
ieee80211_crypto_decap
```

Cette couche appelle notamment :

```text
ppTxPkt
ppRecycleRxPkt
ppCheckTxIdle
ppRegisterTxCallback

wDevDisableRx
wDev_SetRxPolicy
wDev_Set_Beacon_Int
wDev_Get_Next_TBTT

phy_change_channel
```

Conclusion :

> `libnet80211` n’est pas la frontière hardware. Elle repose sur `libpp`, `wDev` et `libphy`.

---

# 7. `libpp.a` — Low MAC logiciel

Objets particulièrement importants :

```text
lmac.o
wdev.o
pp.o
rate_control.o
trc.o
```

## 7.1 Structure `Access`

Les informations de debug de `lmac.o` révèlent une structure contenant notamment :

```c
ac
aifs
cw
cw_min
cw_max

qsrc
qlrc

txop
txop_delta
txop_max

MSDULifetime

success_count
failure_count
```

Cela démontre que le LX106 maintient lui-même :

- AIFS ;
- CW ;
- CWmin/CWmax ;
- retry short/long ;
- TXOP ;
- durée de vie MSDU ;
- compteurs de succès/échec.

## 7.2 `lmacTxFrame()`

La fonction possède explicitement une variable locale :

```text
backoff
```

et appelle :

```c
wDev_EnableTransmit(index, aifs, backoff);
```

Conclusion :

> Le logiciel choisit AIFS et backoff. Le hardware réalise ensuite le timing des slots et CCA.

## 7.3 Modèle CSMA/CA

```text
LX106
  │
  ├── CW / CWmin / CWmax
  ├── AIFS
  └── backoff
       │
       ▼
wDev_EnableTransmit()
       │
       ▼
WDEV hardware
       │
       ├── attend AIFS
       ├── observe CCA
       ├── décompte les slots
       └── démarre TX
```

---

# 8. Résultats TX hardware → logiciel

`wDev_ProcessFiq()` reçoit les résultats du moteur MAC.

Appels observés :

```text
lmacProcessTxSuccess
lmacProcessTxRtsError
lmacProcessCtsTimeout
lmacProcessAckTimeout
lmacProcessTxError
lmacProcessAllTxTimeout
lmacProcessCollisions
```

## 8.1 `txcomplete_state`

Le code permet de reconstruire :

| Valeur | Résultat |
|---:|---|
| `0` | `lmacProcessTxSuccess()` |
| `1` | `lmacProcessTxRtsError()` |
| `2` | `lmacProcessCtsTimeout()` |
| `3` | état non normal / diagnostic |
| `4` | `lmacProcessTxError()` |
| `5` | `lmacProcessAckTimeout()` |

Cela démontre que le hardware distingue au minimum :

```text
success
RTS error
CTS timeout
generic TX error
ACK timeout
```

Le moteur matériel connaît donc la phase de l’échange :

```text
RTS → CTS → DATA → ACK
```

mais la politique de récupération est exécutée par le LX106.

---

# 9. Retry logiciel

`lmacProcessAckTimeout()` mène vers :

```text
lmacProcessLongRetryFail()
ou
lmacProcessShortRetryFail()
```

qui utilisent :

```text
rcReachRetryLimit()
lmacMSDUAged()
lmacDiscardFrameExchangeSequence()
lmacRetryTxFrame()
```

`lmacRetryTxFrame()` appelle notamment :

```text
Tx_Copy2Queue
wDev_EnableTransmit
lmacDiscardAgedMSDU
rcGetRate
ppCalFrameTimes
lmacImrTxFrame
lmacTryTxopEnd
lmacTxFrame
```

Chaîne reconstruite :

```text
ACK timeout
   ↓
LX106
   ↓
short/long retry
   ↓
retry limit ?
   ↓
rate control
   ↓
nouveau timing
   ↓
nouveau AIFS/backoff
   ↓
WDEV hardware
```

---

# 10. Rate control

Fonctions observées :

```text
rcGetRate
rcGetSched
rcUpdateTxDone
rcUpdateRxDone
rcReachRetryLimit
rcLowerSched
rcTxUpdatePer
trc_NeedRTS
```

Tables :

```text
rc11BSchedTbl
rc11GSchedTbl
rc11NSchedTbl
BasicOFDMSched
```

Conclusion :

> Le rate control est logiciel.

---

# 11. `wDev_EnableTransmit()` et WDEV

Signature :

```c
wDev_EnableTransmit(index, aifs, backoff)
```

La fonction utilise directement :

```text
0x3FF20A00
```

ainsi que :

```text
0xC0000000
```

La fonction inverse utilise :

```text
0x3FFFFFFF = ~0xC0000000
```

Cela indique que deux bits de poids fort d’un registre WDEV servent à armer/désarmer une fonction TX.

`wDev_GetTxqCollisions()` et `wDev_ClearTxqCollisions()` utilisent également cette région.

Un masque observé :

```text
0xFFFFF000
```

suggère un champ matériel sur les 12 bits bas lié aux queues/états de collision.

---

# 12. Mask-ROM PHY

La ROM contient de nombreuses fonctions PHY/RF.

Exemples :

```text
0x400060D0 rom_chip_v5_disable_cca
0x400060EC rom_chip_v5_enable_cca
0x4000610C rom_chip_v5_sense_backoff

0x40006B08 phy_get_romfuncs

0x40006C50 rom_set_channel_freq

0x40007268 rom_i2c_readReg
0x400072D8 rom_i2c_writeReg

0x4000754C rom_pbus_set_rxgain
0x40007610 rom_pbus_set_txgain

0x400076FC rom_pbus_xpd_tx_off
0x40007740 rom_pbus_xpd_tx_on
0x400077A0 rom_pbus_xpd_tx_on__low_gain

0x40007968 rom_rfpll_set_freq

0x40007EB4 rom_rfcal_pwrctrl
0x4000804C rom_rfcal_rxiq
0x40008388 rom_rfcal_txcap
0x40008610 rom_rfcal_txiq
```

Conclusion :

> La mask-ROM agit comme un BIOS PHY/RF exécuté par le LX106.

---

# 13. `phy_get_romfuncs()` et `g_phyFuns`

À :

```text
0x40006B08
```

le code de `phy_get_romfuncs()` correspond essentiellement à :

```c
void *phy_get_romfuncs(void)
{
    return *(void **)0x3FFFC730;
}
```

La valeur initialisée au boot dans `0x3FFFC730` pointe vers :

```text
0x3FFFC734
```

C’est le début de la table des fonctions PHY.

Une reconstruction publique indépendante (`rom_phy.h`, MinEspSDKLib) fournit la structure complète de cette table et confirme les offsets. Cette reconstruction concorde avec le dump ROM et les symboles Espressif.

## 13.1 Table `phy_func_tab` reconstruite

| Offset hex | Offset déc. | Fonction / entrée |
|---:|---:|---|
| `+0x000` | 0 | `rom_abs_temp` |
| `+0x004` | 4 | `rom_chip_v5_disable_cca` |
| `+0x008` | 8 | `rom_chip_v5_enable_cca` |
| `+0x00C` | 12 | `rom_chip_v5_sense_backoff` |
| `+0x010` | 16 | `rom_dc_iq_est` |
| `+0x014` | 20 | NULL / fonction inconnue |
| `+0x018` | 24 | `rom_en_pwdet` |
| `+0x01C` | 28 | `rom_get_bb_atten` |
| `+0x020` | 32 | `rom_get_corr_power` |
| `+0x024` | 36 | `rom_get_fm_sar_dout` / patch RAM |
| `+0x028` | 40 | `rom_get_noisefloor` / patch RAM |
| `+0x02C` | 44 | `rom_get_power_db` |
| `+0x030` | 48 | `rom_iq_est_disable` |
| `+0x034` | 52 | `rom_iq_est_enable` |
| `+0x038` | 56 | `rom_linear_to_db` |
| `+0x03C` | 60 | `rom_set_txclk_en` |
| `+0x040` | 64 | `rom_set_rxclk_en` |
| `+0x044` | 68 | `rom_mhz2ieee` |
| `+0x048` | 72 | `rom_rxiq_get_mis` / patch RAM |
| `+0x04C` | 76 | `rom_sar_init` |
| `+0x050` | 80 | `rom_set_ana_inf_tx_scale` |
| `+0x054` | 84 | `rom_set_loopback_gain` |
| `+0x058` | 88 | `rom_set_noise_floor` / patch RAM |
| `+0x05C` | 92 | NULL / fonction inconnue |
| `+0x060` | 96 | NULL / fonction inconnue |
| `+0x064` | 100 | `rom_start_noisefloor` / patch RAM |
| `+0x068` | 104 | `rom_start_tx_tone` |
| `+0x06C` | 108 | `rom_stop_tx_tone` |
| `+0x070` | 112 | `rom_txtone_linear_pwr` |
| `+0x074` | 116 | TX MAC disable / patch RAM |
| `+0x078` | 120 | TX MAC enable / patch RAM |
| `+0x07C` | 124 | analog interface gating / patch RAM |
| `+0x080` | 128 | `rom_set_channel_freq` |
| `+0x084` | 132 | `rom_chip_50_set_channel` |
| `+0x088` | 136 | RX init / patch RAM v6 |
| `+0x08C` | 140 | `rom_chip_v5_tx_init` |
| `+0x090` | 144 | `rom_i2c_readReg` |
| `+0x094` | 148 | `rom_i2c_readReg_Mask` |
| `+0x098` | 152 | `rom_i2c_writeReg` |
| `+0x09C` | 156 | `rom_i2c_writeReg_Mask` |
| `+0x0A0` | 160 | PBUS debug mode / patch RAM |
| `+0x0A4` | 164 | `rom_pbus_enter_debugmode` |
| `+0x0A8` | 168 | `rom_pbus_exit_debugmode` |
| `+0x0AC` | 172 | `rom_pbus_force_test` |
| `+0x0B0` | 176 | `rom_pbus_rd` |
| `+0x0B4` | 180 | `rom_pbus_set_rxgain` |
| `+0x0B8` | 184 | `rom_pbus_set_txgain` |
| `+0x0BC` | 188 | `rom_pbus_workmode` |
| `+0x0C0` | 192 | `rom_pbus_xpd_rx_off` |
| `+0x0C4` | 196 | `rom_pbus_xpd_rx_on` |
| `+0x0C8` | 200 | `rom_pbus_xpd_tx_off` |
| `+0x0CC` | 204 | `rom_pbus_xpd_tx_on` |
| `+0x0D0` | 208 | `rom_pbus_xpd_tx_on__low_gain` |
| `+0x0D4` | 212 | `rom_phy_reset_req` |
| `+0x0D8` | 216 | restart calibration / patch RAM |
| `+0x0DC` | 220 | `rom_rfpll_reset` |
| `+0x0E0` | 224 | `rom_write_rfpll_sdm` |
| `+0x0E4` | 228 | `rom_rfpll_set_freq` |
| `+0x0E8` | 232 | calibration TOS / patch RAM v6 |
| `+0x0EC` | 236 | `rom_pbus_dco___SA2` |
| `+0x0F0` | 240 | `rom_rfcal_pwrctrl` |
| `+0x0F4` | 244 | `rom_rfcal_rxiq` |
| `+0x0F8` | 248 | `rom_rfcal_rxiq_set_reg` |
| `+0x0FC` | 252 | `rom_rfcal_txcap` |
| `+0x100` | 256 | `rom_rfcal_txiq` |
| `+0x104` | 260 | `rom_rfcal_txiq_cover` |
| `+0x108` | 264 | `rom_rfcal_txiq_set_reg` |
| `+0x10C` | 268 | RXIQ cover / patch RAM |
| `+0x110` | 272 | `rom_set_txbb_atten` |
| `+0x114` | 276 | `rom_set_txiq_cal` |
| `+0x118` | 280 | NULL / fin de table |

### 13.2 Conséquence architecturale

La table contient côte à côte :

```text
mesure IQ
détecteur de puissance
clocks TX/RX
tone generator
loopback
I²C analogique
PBUS
gains RX/TX
PLL
calibrations
```

Cela montre que `g_phyFuns` constitue réellement une **API interne du PHY/RF**, et non une simple collection de helpers.

# 14. Patch de la ROM par `libphy.a`

`libphy.a` récupère la table ROM puis remplace certaines entrées par des versions plus récentes en RAM/flash.

Entrées observées comme patchées :

| Offset | Fonction installée |
|---:|---|
| `+0x24` | `ram_get_fm_sar_dout` |
| `+0x28` | `ram_get_noisefloor` |
| `+0x48` | `ram_rxiq_get_mis` |
| `+0x58` | `ram_set_noise_floor` |
| `+0x64` | `ram_start_noisefloor` |
| `+0x74` | `ram_tx_mac_disable` |
| `+0x78` | `ram_tx_mac_enable` |
| `+0x7C` | `ram_ana_inf_gating_en` |
| `+0x88` | `ram_chip_v6_rx_init` |
| `+0xA0` | `ram_pbus_debugmode` |
| `+0xD8` | `ram_restart_cal` |
| `+0xE8` | `ram_cal_tos_v60` |
| `+0x10C` | `ram_rxiq_cover_mg_mp` |

Architecture :

```text
                 g_phyFuns
                    │
          ┌─────────┴─────────┐
          ▼                   ▼
     fonction ROM       fonction patchée
     0x4000xxxx         libphy v6
```

La ROM est donc un **PHY patchable**.

---

# 15. I²C analogique interne

Les fonctions :

```text
rom_i2c_readReg
rom_i2c_writeReg
rom_i2c_readReg_Mask
rom_i2c_writeReg_Mask
```

ne correspondent pas au bus I²C sur GPIO.

Elles pilotent un bus interne vers les blocs analogiques du chip.

Exemples identifiés :

## TX clock

```text
bloc 0x77, reg 28, bit 6
bloc 0x7C, reg 21, bit 0
```

## RX clock

```text
bloc 0x77, reg 28, bit 5
bloc 0x7C, reg 21, bit 1
```

## Analog TX scale

```text
bloc 0x77, reg 9
```

---

# 16. PBUS — bus RF interne

La primitive :

```text
rom_pbus_force_test(selector, bank, value)
```

utilise :

```text
0x60000594   commande PBUS
0x600005A0   status PBUS
```

## 16.1 Format reconstruit

```text
bits 15..14 : bank       (2 bits)
bits 13..5  : value      (9 bits)
bits 4..2   : selector   (3 bits)
bit 1       : START
bit 0       : préservé
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

Conclusion :

> PBUS est un vrai bus interne transactionnel vers la chaîne RF.

---

# 17. Activation RF TX

## 17.1 TX OFF

`rom_pbus_xpd_tx_off()` :

```text
PBUS(6, 1,   0)
PBUS(1, 1,  12)
PBUS(2, 1,   0)
```

## 17.2 TX ON

`rom_pbus_xpd_tx_on()` :

```text
PBUS(2, 1,   1)
PBUS(7, 1,  95)
PBUS(0, 1,   x)
PBUS(1, 1, 127)
PBUS(6, 1, 127)
```

## 17.3 Low gain

La variante low-gain diffère notamment par :

```text
normal:   PBUS(7, 1, 95)
low gain: PBUS(7, 1, 0)
```

Le sélecteur `7` est donc fortement lié à un étage de gain/bias TX.

---

# 18. Gain TX

`rom_pbus_set_txgain(x)` utilise :

```text
PBUS(4, 1, value)
```

avec une permutation des bits de `x`.

Conclusion :

```text
selector 4 / bank 1
```

est associé au gain TX RF.

---

# 19. Loopback interne TX → RX

La primitive :

```text
rom_set_loopback_gain()
```

effectue environ :

```text
PBUS(2, 1, 0x185)
PBUS(7, 1, A)
PBUS(2, 1, B)
PBUS(3, 1, C)
PBUS(3, 2, D)
```

Cette fonction est réellement utilisée dans le PHY v6 par :

```text
set_rx_gain_cal_iq()
```

Chaînes de debug observées :

```text
set_rx_gain:
 rftx=%x
 rfrx=%x
 att=%d
 txbb=0x%x
 bbrx1=0x%x
 bbrx2=0x%x
 tdc:%d,%d
```

Cela confirme un contrôle séparé de :

- gain RF TX ;
- gain RF RX ;
- atténuation ;
- baseband TX ;
- plusieurs étages de gain RX ;
- paramètres de calibration.

Architecture probable :

```text
tone TX
   ↓
DAC / RF TX
   ↓
loopback analogique
   ↓
RF RX
   ↓
ADC
   ↓
IQ estimator
```

---

# 20. Générateur de tone

Fonctions :

```text
rom_start_tx_tone
rom_stop_tx_tone
rom_txtone_linear_pwr
```

Registres :

```text
0x600005B8   slot 1
0x600005BC   slot 2
0x600005C4   slot 3
```

## 20.1 Layout actuellement établi

Le mot de slot est désormais mieux décrit comme un **packing logiciel observé** que comme trois champs dont la largeur serait imposée par la ROM. `rom_start_tx_tone()` assemble le mot par OR :

```c
r = REG32(slot);
r &= preserve_mask;
r |= raw_control;
r |= ((uint32_t)((0x100 - digital_scale) & 0xff) << 10);
r |= ((uint32_t)mode_code << 18);
```

Point important : la fonction **ne masque explicitement ni `raw_control` à 10 bits ni `mode_code` avant le décalage**. Les largeurs ci-dessous sont donc les limites naturelles déduites du packing, des masques et de tous les usages observés, pas des validations d’arguments effectuées par l’API ROM.

```text
31        28 27                  18 17          10 9                    0
┌───────────┬──────────────────────┬──────────────┬──────────────────────┐
│ préservés │ mode/test observé    │ scale code   │ raw tone_control     │
│           │ bit18 = gate normal  │ 8 bits       │ valeurs 8 / 64 vues │
└───────────┴──────────────────────┴──────────────┴──────────────────────┘
```

### Partie basse — `tone_control`

`rom_start_tx_tone()` OR directement la valeur fournie dans le mot du slot. Il n’existe pas de :

```c
control &= 0x3ff;
```

dans cette routine. Tous les appels démontrés utilisent néanmoins de petites valeurs positives qui restent dans les bits bas :

```text
RXIQ                  → 8
TXIQ                  → 64
power / TX-cap        → 64
```

Le nom `tone_control` reste donc recommandé. L’hypothèse principale est un step/index du générateur numérique, mais la loi valeur→fréquence se situe vraisemblablement derrière la frontière hardware.

### Bits 17:10 — code de scale numérique

Le code exact écrit est :

```text
digital_field = (-digital_scale) mod 256
slot[17:10]    = digital_field
```

avec `digital_scale` borné à `0..63` par `rom_set_ana_inf_tx_scale()`. Exemples :

```text
digital_scale = 0  → field = 0x00
digital_scale = 1  → field = 0xFF
digital_scale = 2  → field = 0xFE
...
digital_scale = 63 → field = 0xC1
```

Le champ est donc mieux décrit comme un **coefficient/scale signé stocké sous forme négative** que comme un entier positif directement proportionnel à la puissance. La relation exacte entre ce code et l’amplitude RF doit encore être mesurée.

### Partie haute — mode et gate

Le premier paramètre du slot est décalé directement :

```c
r |= ((uint32_t)mode_code << 18);
```

sans `& 1` et sans masque explicite `& 0x3ff`. Dans le mode normal utilisé par les calibrations :

```text
mode_code = 1
```

ce qui positionne uniquement le **bit 18**.

`rom_stop_tx_tone()` n’efface ensuite que :

```text
bit18 = 0
```

avec le masque `~0x00040000`. Cette propriété reste la preuve la plus forte que bit18 constitue le gate minimal du tone dans le mode normal.

TXIQ contourne le wrapper et manipule directement des états supplémentaires dans la partie haute du mot. Les transitions déjà démontrées restent :

```text
phase I/Q : 0x00B → 0x04B
gain I/Q  : 0x10B → 0x20B
```

mais la v0.18 évite désormais d’affirmer que toute la zone `27:18` constitue nécessairement un unique champ matériel de 10 bits. C’est un **packing logiciel cohérent**, dont les sous-champs physiques internes restent partiellement ouverts.

### Slot 3 / bit 28

Pour le troisième slot, le premier paramètre est chargé comme un octet (`l8ui`) et est copié à la fois vers le décalage 18 et vers le décalage 28. `rom_stop_tx_tone(3)` n’efface que bit18 et laisse bit28 intact. Le comportement logiciel est établi ; le rôle physique du bit28 reste ouvert.

---

# 21. TX clock et `stop_tx_tone()`

`rom_start_tx_tone()` active d’abord :

```text
rom_set_txclk_en(1)
```

`rom_stop_tx_tone()` :

1. efface le bit 18 du/des slots ;
2. coupe ensuite les clocks TX.

Conclusion :

> Appeler `start_tx_tone()` / `stop_tx_tone()` pour chaque symbole n’est probablement pas optimal pour une modulation rapide.

---

# 22. Analog TX scale

`rom_set_ana_inf_tx_scale()` sépare exactement la valeur demandée `x` entre une composante numérique et une composante analogique. Le branchement Xtensa est un `bltui a2, 64, ...`, ce qui confirme le seuil `64`.

Pseudo-code reconstruit :

```c
if (x < 64) {
    analog_scale  = 0;
    digital_scale = x;
} else {
    analog_scale  = (63 - x) & 0xff;
    digital_scale = 63;
}

I2C_MASK(block=0x77, host=0, reg=9, bits=7..0, value=analog_scale);
return digital_scale;
```

Puis `rom_start_tx_tone()` encode ce retour sous la forme :

```text
slot[17:10] = (-digital_scale) mod 256
```

Ainsi, le chemin est :

```text
valeur x
  │
  ├── x < 64  : numérique = x, analogique = 0
  │
  └── x >= 64 : numérique = 63, analogique = (63-x) mod 256
          │
          ▼
   scale numérique + scale analogique
          │
          ▼
       chaîne TX
```

Interprétation actuelle : le logiciel démontre un **partage de scale** entre numérique et analogique. Il ne démontre pas, à lui seul, la loi en dB ni la puissance RF résultante. En particulier, il ne faut pas confondre la valeur numérique brute du champ `17:10` avec une amplitude linéaire directement lisible.

La documentation officielle de test RF exprime, elle, l’atténuation du mode single-carrier en pas de `0,25 dB`; le lien exact entre ce paramètre public et la variable interne `x` n’est pas encore tracé dans les binaires fournis.

---

# 23. `ram_set_txbb_atten()`

Cette fonction ne semble pas être un simple gate d’amplitude.

Elle programme une table matérielle contenant environ 24 entrées :

```text
0x60000504
0x60000508
...
0x60000560
```

Elle modifie principalement le byte bas de chaque entrée.

Conclusion :

> Cette primitive semble programmer une table de puissance/atténuation utilisée par le PHY selon rate/index, plutôt qu’un gain instantané unique.

---

# 24. IQ estimator / corrélateur

Registre de contrôle :

```text
0x6000057C
```

## 24.1 Layout

```text
31       19 18 17 16........2 1   0
┌──────────┬──┬──┬───────────┬──┬──┐
│ DONE     │M │ ?│     N      │GO│EN│
└──────────┴──┴──┴───────────┴──┴──┘
```

Interprétation :

| Bits | Fonction | Confiance |
|---|---|---:|
| `0` | enable | ~99 % |
| `1` | trigger/start | ~99 % |
| `2..16` | longueur d’intégration N | ~99 % |
| `17` | inconnu | faible |
| `18` | mode binaire probable | élevée |
| `31` | done / ready | ~99 % |

`rom_dc_iq_est()` divise les résultats par :

```text
N + 1
```

ce qui confirme fortement que `N` représente une longueur d’accumulation.

---

# 25. Accumulateurs I/Q

Registres :

```text
0x600005DC
0x600005E0
```

`rom_dc_iq_est()` fait approximativement :

```text
I_DC = (REG_5DC >> 6) / (N + 1)
Q_DC = (REG_5E0 >> 6) / (N + 1)
```

Interprétation probable :

```text
0x600005DC ≈ ΣI × 64
0x600005E0 ≈ ΣQ × 64
```

---

# 26. Puissance totale

Registre :

```text
0x600005E4
```

Il est traité comme une grandeur d’énergie/puissance accumulée.

Interprétation :

```text
total signal energy / power
```

---

# 27. Corrélation I/Q

Registres :

```text
0x60000580
0x60000584
0x60000588
0x6000058C
```

`rom_get_corr_power()` lit ces quatre valeurs puis calcule une structure du type :

```text
X = R0 + R3
Y = R1 - R2

corr_power = X² + Y²
```

C’est fortement compatible avec :

```text
|C|² = Re(C)² + Im(C)²
```

donc une corrélation complexe.

`rom_rxiq_get_mis()` utilise également :

```text
R0 - R3
R0 + R3
R2 + R1
R1 - R2
```

avant de calculer des rapports/carrés.

Interprétation forte :

- amplitude mismatch I/Q ;
- phase mismatch I/Q.

---

# 28. `rom_get_corr_power()`

Cette fonction semble produire trois grandeurs :

```text
out[0] ≈ total energy
out[1] ≈ correlation energy
out[2] ≈ DC energy
```

Avec :

```text
correlation energy =
    (R0 + R3)²
  + (R1 - R2)²

DC energy =
    DC_I²
  + DC_Q²
```

---

# 29. Ce que le CPU ne reçoit PAS

Aucune preuve d’un accès logiciel normal au flux :

```text
I[n], Q[n]
I[n+1], Q[n+1]
...
```

à la cadence PHY.

Donc l’ESP8266 **n’est pas un SDR générique**.

Le CPU possède cependant accès à un DSP de mesure donnant :

- moyenne DC I ;
- moyenne DC Q ;
- puissance totale ;
- corrélation complexe ;
- mismatch I/Q ;
- noise floor ;
- EVM ;
- frequency offset ;
- détecteur de puissance TX via SAR.

---

# 30. Primitives RF les plus basses identifiées

| Primitive | Fonction |
|---|---|
| `rom_i2c_readReg/writeReg` | registres analogiques internes |
| `rom_pbus_force_test` | transaction PBUS |
| `rom_pbus_set_rxgain` | gain RX |
| `rom_pbus_set_txgain` | gain TX |
| `rom_pbus_xpd_rx_on/off` | activation RX RF |
| `rom_pbus_xpd_tx_on/off` | activation TX RF |
| `rom_rfpll_set_freq` | PLL RF |
| `rom_set_channel_freq` | canal/fréquence |
| `rom_set_txclk_en` | clocks TX |
| RX clock enable | clocks RX |
| `rom_set_ana_inf_tx_scale` | scale analogique TX |
| `rom_set_loopback_gain` | loopback RF interne |
| `rom_start_tx_tone` | générateur tone |
| `rom_stop_tx_tone` | arrêt tone |
| `rom_txtone_linear_pwr` | mesure puissance tone |
| `rom_iq_est_enable/disable` | estimateur I/Q |
| `rom_dc_iq_est` | moyenne DC I/Q |
| `rom_get_corr_power` | puissance/corrélation |
| `rom_rxiq_get_mis` | mismatch I/Q |

---

# 31. Candidat OOK

## 31.1 Modèle actuellement privilégié

Plutôt que couper entièrement la RF :

```text
PA OFF → PA ON
```

la solution architecturale la plus propre semble être :

```text
PLL          reste ON
TX clocks    restent ON
RF/PA        restent configurés
tone         configuré une fois

          bit 18
             │
        0 ↔ 1
             │
       tone OFF/ON
```

## 31.2 Candidats classés

| Méthode | Coût | Potentiel OOK |
|---|---:|---:|
| `xpd_tx_on/off` | 3–5 transactions PBUS | faible |
| TX clock ON/OFF | plusieurs réglages analogiques | moyen-faible |
| TX MAC enable | mauvais niveau architectural pour tone | faible |
| PBUS TX gain | transactionnel | moyen |
| analog TX scale | I²C interne | moyen |
| `ram_set_txbb_atten` | table de puissance | moyen |
| **tone coefficient bits 17:10** | MMIO potentiel | très élevé |
| **tone enable bit 18** | MMIO potentiel | **meilleur candidat** |

---

# 32. Hypothèse OOK principale

```text
                    INIT
                      │
                      ▼
               RF PLL verrouillé
                      │
               TX clocks actives
                      │
                 PA stable
                      │
            tone slot configuré
                      │
                      ▼
             ┌────────────────┐
bit 0 ─────► │ bit18 = 0      │
bit 1 ─────► │ bit18 = 1      │
             └────────────────┘
                      │
                      ▼
                  amplitude RF
```

Cette hypothèse est **architecturalement forte**, mais nécessite encore une validation physique contrôlée avant de considérer le comportement RF comme démontré.

---

# 33. Deuxième candidat : modulation par scale numérique

Au lieu de désactiver le slot, on peut conceptuellement garder le gate actif et modifier le code :

```text
bits 17:10 = (-digital_scale) mod 256
```

TXIQ démontre que ce code peut être réécrit **à chaud** alors que la TX clock reste active. Cela rend une modulation d’amplitude multi-niveaux architecturalement plausible.

Cependant, la v0.18 corrige une simplification antérieure : le champ `17:10` n’est pas un entier positif dont la valeur brute pourrait être lue directement comme « amplitude ». Il encode le négatif d’un `digital_scale` 0..63 et travaille avec un scale analogique séparé au-delà du seuil 64.

Avantages potentiels :

- générateur maintenu en régime établi ;
- pas de restart global ;
- possibilité théorique d’ASK multi-niveaux.

Questions encore ouvertes :

- `digital_scale=0` annule-t-il effectivement la contribution du tone dans le hardware ?
- quelle est la courbe `digital_scale → puissance RF` ?
- la phase est-elle préservée pendant une réécriture du scale ?

Le **gate bit18** reste donc le candidat OOK principal ; le scale `17:10` est maintenant classé comme candidat secondaire ASK/OOK à caractériser.

---

# 34. Questions encore ouvertes — priorité RX OOK/ASK

Depuis la v0.22, la recherche principale est la **réception autonome OOK/ASK**. Le modèle logiciel TX OOK/ASK reste considéré comme clos. Les questions ci-dessous sont ordonnées pour construire un démodulateur RX sans dépendre du décodeur de paquets Wi-Fi.

| Priorité | Question |
|---:|---|
| 1 | **Prouver expérimentalement que `IQ_EST`, loopback désactivé, mesure bien le chemin antenne → RF RX → baseband.** Le fait que `rom_iq_est_enable()` ne configure aucun loopback rend cette architecture fortement probable, mais un appel RX externe explicite n’a pas encore été retrouvé. |
| 2 | **Valider le re-arm ultra-rapide avec `ENABLE=1` permanent et un front `START: 1→0→1`.** La v0.27 démontre que `rom_iq_est_disable()` force d’abord `START=0` tout en conservant `ENABLE=1`, puis coupe `ENABLE` dans une seconde écriture. Le point restant à mesurer est de savoir si le front bas→haut de `START` suffit à effacer/réarmer `DONE`. |
| 3 | **Mesurer la loi `(N+1) → durée d’intégration`** avec `CCOUNT`. La v0.27 démontre que `rom_dc_iq_est()` normalise par `N+1`; aucun diviseur/clock-rate public n’a été trouvé. Une régression `cycles = C0 + k·(N+1)` doit fournir directement la cadence réelle de l’estimateur. |
| 4 | Déterminer la stratégie **gain RX fixe** pour ASK/M-ASK. La v0.23 corrige `phy_enable_agc/disable_agc` : ces wrappers pilotent le CCA, pas l’AGC. Les primitives PBUS de gain RX deviennent la voie principale. |
| 5 | Relier la mesure `IQ_EST`/énergie à une puissance reçue relative ou absolue, puis définir des seuils robustes OOK et M-ASK. |
| 6 | Déterminer la sensibilité, le débit OOK maximal, le nombre de niveaux ASK séparables et l’effet du bruit/fading. |
| 7 | Décoder plus précisément `0x60009B64[19:12]` comme seuil/contrôle CCA et `0x60009B64[31:20]` comme noise-floor configuré/filtré, y compris leurs unités. |
| 8 | Décoder le readout matériel `0x60009824[11:0]` retourné par `read_hw_noisefloor()` et sa conversion physique. La v0.24 confirme en parallèle un champ `RxControl.noise_floor` signé sur 8 bits, mais ce champ est attaché au descripteur de paquet et ne constitue pas un readout RF libre. |
| 9 | **Piste secondaire :** déterminer si le CCA peut servir directement de slicer OOK matériel à très faible latence. Aucun readout CPU `CCA busy` continu n’est retrouvé dans le corpus ; ne pas bloquer le chemin `IQ_EST` sur ce point. |
| 10 | Déterminer la relation en dB des gains RX fixes. Le packing logiciel de `pbus_set_rxbbgain()` est maintenant décodé en v0.23 ; il reste à relier ses indices à un gain physique. |
| 11 | Identifier un usage réel de `IQ_EST M=0` et comprendre sa différence physique avec `M=1`. |
| 12 | Vérifier si les registres de corrélation `0x580/584/588/58C` peuvent aider à rejeter du bruit/interférence pour OOK/ASK, au-delà de la simple énergie `0x5E4`. |
| 13 | **TX hors chemin critique RX :** mesurer ultérieurement extinction, latence, jitter et phase du gate OOK TX. |
| 14 | **Extension future FSK/M-FSK :** déterminer la loi physique `tone_control → fréquence`; cette recherche est mise en attente pendant le chantier RX OOK/ASK. |
| 15 | **Extension future :** comprendre slots 2/3 et multi-tone. |

---

# 35. Plan de recherche restant

## Étape RX-A — établir le détecteur d’énergie externe

Priorité actuelle :

1. activer la chaîne RX sans protocole Wi-Fi ;
2. conserver le loopback interne désactivé ;
3. commencer par la séquence ROM complète `enable → DONE → E4 → disable` afin d’établir la référence fonctionnelle ;
4. tester ensuite le **re-arm rapide v0.27** : garder `ENABLE=1`, forcer `START=0`, vérifier l’état de `DONE`, puis reprogrammer `N/mode` avec `START=1` ;
5. lire `0x600005E4` après chaque nouveau `DONE` et vérifier que la valeur suit effectivement ON/OFF d’une porteuse externe ;
6. mesurer en parallèle `CCOUNT` pour `N = 0,1,3,7,15,31,63,127,255,511,1023,2047,4095,8191` et ajuster `cycles = C0 + k·(N+1)` ;
7. comparer bruit seul / porteuse OOK / niveaux ASK et choisir le plus petit `N` donnant une séparation robuste.

Objectif :

> démontrer `antenne → RF RX → baseband → IQ_EST → énergie` comme voie RX autonome.

## Étape RX-B — AGC, gain fixe et ASK

Pour OOK, un seuil énergétique adaptatif peut fonctionner avec un gain automatique tant que sa dynamique est suffisamment lente. Pour ASK/M-ASK, l’AGC risque au contraire d’annuler la différence d’amplitude entre symboles.

Travail restant :

- utiliser éventuellement un préambule pour choisir un gain approprié ;
- passer ensuite en **gain RX PBUS imposé** plutôt que compter sur un hypothétique wrapper AGC ;
- programmer un gain RX stable avec `pbus_set_rxbbgain()` / `rom_pbus_set_rxgain()` ;
- quantifier les niveaux `IQ_EST` pour plusieurs amplitudes reçues ;
- construire des seuils multiples ASK.

Objectif :

> obtenir un détecteur d’enveloppe numérique stable, indépendant du décodeur 802.11.

## Étape RX-C — CCA/noise-floor comme voie rapide et baseline

Le CCA et le noise-floor sont maintenant cartographiés partiellement. La v0.25 montre que le registre WDEV d’événements `0x3FF20C20` est une cause d’interruption latched, et non un candidat satisfaisant à lui seul pour « porteuse présente maintenant ». Il reste à :

- localiser, s’il existe, un **status CCA/busy combinatoire ou quasi instantané** ailleurs dans le PHY/baseband ;
- mesurer sa latence ;
- déterminer les unités du seuil CCA ;
- utiliser le noise-floor comme baseline/adaptation lente plutôt que comme échantillonneur de symboles ;
- comparer CCA et `IQ_EST` en sensibilité et vitesse.

Le noise-floor standard de `libpp` utilise un timer de 100 ms ; cette voie standard est donc beaucoup trop lente pour une démodulation OOK symbole-par-symbole, mais reste utile pour suivre le plancher de bruit.

## Étape RX-D — démodulation autonome

Une fois la mesure d’énergie validée :

```text
OOK :
    E < T  → 0
    E >= T → 1

M-ASK :
    E < T1          → niveau 0
    T1 <= E < T2    → niveau 1
    T2 <= E < T3    → niveau 2
    ...
```

La synchronisation symbole, les préambules et la récupération d’horloge seront ensuite réalisées en logiciel LX106 à partir de cette primitive énergétique.

---

## Recherches TX/extensions mises en second plan

## Étape A — terminer le reverse-engineering statique du bloc tone

Travail restant :

- déterminer la loi physique associée aux valeurs désormais démontrées `8` et `64` par **mesure instrumentée** ou par une source inter-générations qui expose explicitement l’équation du step ;
- poursuivre le décodage de la **zone mode/test** autour de bit18 : `0x10B→0x20B` = gain I/Q et `0x00B→0x04B` = phase I/Q ; la v0.18 ne considère plus la largeur hardware de 10 bits comme démontrée par la ROM ;
- trouver un utilisateur réel des slots 2/3 hors de `start_tx_tone()` si un tel utilisateur existe dans un firmware différent (usine/ATE/autre révision) ;
- tester l’hypothèse principale actuelle : `tone_control` est un champ de step/index du générateur (NCO ou équivalent), sans encore le renommer tant que la loi valeur→effet physique n’est pas démontrée.

Déjà terminé :

- recensement des appels directs ROM à `rom_start_tx_tone()` : trois appels démontrés (`rfcal_pwrctrl`, `rfcal_rxiq`, `rfcal_txcap`), tous **slot 1 uniquement** ;
- audit des accès directs aux offsets `0x3B8/0x3BC/0x3C4` relatifs à la base réelle `0x60000200` : aucun quatrième utilisateur caché du bloc tone n’a été retrouvé dans la mask-ROM fournie ;
- distinction des faux positifs d’offset utilisant une autre base MMIO ;
- démonstration que la mask-ROM n’effectue aucune conversion logicielle de `tone_control` vers une fréquence : le champ est injecté brut dans le registre matériel.

Objectif :

> comprendre entièrement le format et l’usage matériel de chaque tone slot.

## Étape B — comprendre les slots 2 et 3

Le comportement logiciel du bit 28 est maintenant partiellement résolu : il copie `enable_3` à l’activation et reste positionné après `stop_tx_tone(3)`.

Travail restant :

- déterminer le rôle physique exact de ce bit sticky ;
- trouver un utilisateur réel des slots 2/3 ;
- vérifier les pistes multi-tone, stimulus I/Q spécial, ancienne révision silicium ou firmware usine/ATE.

La piste `phy_dig_spur_set/prot` est désormais **écartée provisoirement** : ces fonctions utilisent les blocs `0x600096xx/0x60009Axx`, pas le générateur de tone `0x600005B8/BC/C4`.

## Étape C — mode IQ estimator

Le layout du registre `0x6000057C` est établi. La nouvelle analyse relie maintenant **`M=1`** au chemin RXIQ de mesure corrélation/puissance : les versions ROM et RAM de `rxiq_cover_mg_mp()` activent `iq_est_enable(1, N)`, puis `rxiq_get_mis()` exploite `0x60000580/584/588/58C` ainsi que `0x600005E4`. `set_rx_gain_cal_iq()` utilise aussi `iq_est_enable(1, 1024)` avant une lecture directe de `0x600005E4`.

`rom_dc_iq_est(mode, N, out)` transmet de son côté le paramètre `mode` directement à `iq_est_enable(mode, N)`, puis lit `0x600005DC/5E0`. Cela montre que la sélection de mode et le choix des registres lus sont deux aspects distincts du bloc de mesure.

Travail restant :

- identifier un appel réel `M=0` s’il existe dans un autre firmware/contexte ;
- déterminer ce que `M=0` change physiquement par rapport à `M=1` ;
- tester si `M=1` peut servir à mesurer un tone volontairement gaté par `tone_mode[0]`.

Objectif :

> utiliser l’estimateur comme instrument interne de caractérisation du générateur lorsque cela est démontré possible.

## Étape D — caractériser le chemin loopback et les deux voies de mesure internes

Deux voies de mesure sont maintenant démontrées statiquement :

1. **voie RX loopback / IQ estimator** : corrélation complexe, puissance totale et mismatch I/Q ;
2. **voie TX detector / SAR** : `rom_txtone_linear_pwr(n,q)` calcule et accumule, pour chaque acquisition, un rapport de la forme `((SAR_A << q) / max(SAR_B,1))`. TXIQ utilise exactement `n=4, q=10`, donc un rapport en échelle Q10. Une acquisition SAR contient au moins `25 µs` de délai explicite ; une mesure TXIQ `txtone_linear_pwr(4,10)` contient donc au moins `100 µs` de délais explicites, hors overhead.

Le chemin `meas_tone_pwr_db()` utilise de son côté exactement deux appels à `get_power_db(312)`, puis applique un arrondi signé et une division par 4. `get_power_db(k)` se reconstruit comme `k + linear_to_db(SAR_A,3) - linear_to_db(SAR_B,3)`.

Déterminer le rôle exact de :

```text
PBUS selector 2
PBUS selector 3 bank 1
PBUS selector 3 bank 2
PBUS selector 7
```

pendant le loopback, et relier ces réglages aux mesures `IQ_EST`.

## Étape E — terminer le chemin continuous-TX

Désormais démontré :

- `tx_data1` sauvegarde le retour de `rom_i2c_readReg(0x66, 3, 1)` ;
- `tx_cont_en()` force ensuite `rom_i2c_writeReg_Mask(0x66, 3, 1, 5, 0, 60)` ;
- `tx_cont_dis()` restaure cette valeur par `rom_i2c_writeReg(0x66, 3, 1, tx_data1)` ;
- un octet `.bss + 0xC4` agit comme flag d'état afin d'éviter une double sauvegarde/restauration ;
- `tx_data2/3/4` sauvegardent et restaurent `0x60000594/598/59C`.

Travail restant :

- identifier le rôle physique exact du bloc analogique interne `0x66`, host `3`, registre `1` sur l'ESP8266 ;
- déterminer la fonction physique des bits `5:0` forcés à `0x3C` ;
- déterminer la fonction physique des champs forcés par les masques de `0x60000598/0x6000059C`.

Objectif :

> passer d'une carte save/modify/restore désormais presque complète à une interprétation physique des quatre états modifiés par le mode continuous-TX.

## Étape F — intégration autonome OOK/ASK sans retour Wi‑Fi

La cible du projet est désormais explicitement **non coexistante** : le Wi‑Fi normal n'a pas à survivre à la session de modulation et il n'existe aucun besoin de retour à PP/LMAC/WDEV après la mise en route du générateur.

Architecture cible :

```text
BOOT
  ↓
initialisation / calibration PHY-RF une fois
  ↓
figer canal / PLL et empêcher sleep/re-init
  ↓
abandonner le trafic Wi-Fi normal
  ↓
préparer PBUS / TX XPD / TX clock
  ↓
normaliser complètement le slot 1
  ↓
┌─────────────────────────────────────┐
│ OOK : RMW bit18                     │
│ ASK : RMW bits17:10 (scale digital) │
└─────────────────────────────────────┘
  ↓
reste en mode générateur RF
```

La documentation officielle Espressif du firmware de test expose d'ailleurs un `rftest_init` dont la fonction est « initialize RF to prepare for test », puis un `wifiscwout` de single-carrier. Cela corrobore l'architecture générale : l'état RF de test peut être préparé séparément du trafic Wi-Fi normal.

### OOK — modèle logiciel clos

Registre principal :

```text
TONE1 = 0x600005B8
GATE  = 0x00040000   // bit18
```

Après une programmation initiale du slot en mode normal, la modulation symbole-par-symbole ne doit **pas** appeler `rom_stop_tx_tone()`, car ce wrapper coupe aussi la TX clock globale. La primitive minimale est :

```text
OFF : slot &= ~0x00040000
ON  : slot |=  0x00040000
```

avec les barrières `MEMW` requises autour des MMIO, comme dans la ROM. PLL, TX clock, RF/XPD, `tone_control` et scale restent stables pendant la modulation.

### ASK / M-ASK — modèle logiciel clos

Le champ numérique est :

```text
SCALE_MASK = 0x0003FC00      // bits17:10
field      = ((-digital_scale) & 0xff) << 10
digital_scale = 0..63 dans le chemin canonique
```

Une mise à jour ASK rapide préserve tout le reste du slot :

```text
slot = (slot & ~0x0003FC00)
     | (((-digital_scale) & 0xff) << 10)
```

Le chemin TXIQ démontre que ce champ peut être réécrit **à chaud sous TX clock active**. Il constitue donc la primitive M‑ASK rapide.

Le réglage analogique issu de `rom_set_ana_inf_tx_scale(x)` est distinct :

```text
x < 64:
    digital_scale = x
    analog_scale  = 0

x >= 64:
    digital_scale = 63
    analog_scale  = (63-x) & 0xff

analog_scale → I²C interne bloc 0x77 / reg 9 / bits7:0
```

Cette partie analogique est mieux traitée comme **réglage de plage / niveau de base**, puis le champ numérique comme modulateur ASK symbole-par-symbole. Elle n'est pas nécessaire dans la boucle ASK rapide.

### Convention de packing sûre

La ROM ne masque pas elle-même `tone_control` à 10 bits. Pour une primitive autonome qui veut garantir l'absence de recouvrement avec le scale, la convention logicielle sûre est de réserver les bits `9:0` :

```text
control_safe = tone_control & 0x3ff
```

Il s'agit d'une protection ajoutée par notre code, **pas** d'un masque observé dans `rom_start_tx_tone()`.

### Ce que signifie « 100 % côté logiciel »

À partir de la v0.21, OOK et ASK sont classés **fermés côté commande logicielle/statique** :

- adresse du slot ;
- gate OOK ;
- masque et encodage ASK ;
- programmation initiale normale ;
- mise à jour à chaud ;
- distinction scale numérique / scale analogique ;
- nécessité de garder TX clock/RF stables pendant les symboles ;
- nécessité d'éviter `stop_tx_tone()` dans la boucle ;
- instrumentation interne disponible pour comparer des niveaux stables.

Les points suivants ne sont plus classés comme « logiciel non décodé », mais comme **caractérisation matérielle/RF** :

- extinction RF obtenue par bit18=0 ;
- latence et jitter bit18→RF ;
- continuité de phase OFF→ON ;
- courbe `digital_scale → amplitude/dBm` ;
- linéarité et nombre de niveaux ASK réellement séparables ;
- transitoires spectraux.

Objectif :

> considérer le reverse-engineering logiciel OOK/ASK comme terminé et déplacer la recherche principale vers d'autres capacités du générateur (notamment FSK/M-FSK), tout en gardant une campagne de validation RF OOK/ASK séparée.

## Étape G — validation instrumentée du tone

À effectuer uniquement en environnement RF contrôlé :

- charge/atténuation importante ;
- analyseur de spectre ou instrumentation équivalente ;
- éviter de rayonner un signal de test non standard.

Objectifs :

1. confirmer l’effet RF ON/OFF du bit 18 ;
2. mesurer le temps de transition et le jitter ;
3. caractériser le coefficient 17:10 ;
4. observer les transitoires ;
5. vérifier la continuité de phase ;
6. tester une modification du coefficient pendant un tone actif.

---

# 36. Corrections apportées pendant la recherche

Cette section est importante : plusieurs hypothèses initiales ont été corrigées au fil de l’analyse.

## 36.1 `0x60000200`

Ancienne interprétation :

> « baseband Wi‑Fi »

Correction :

`0x60000200` peut être utilisé simplement comme base d’adressage Xtensa pour atteindre des registres absolus comme :

```text
0x600005B8
```

Il ne faut donc pas attribuer la fonction d’un registre uniquement à partir de la base chargée par le compilateur.

## 36.2 Bits 9:0 du tone

Ancienne hypothèse :

> fréquence/NCO step à ~90 %.

Correction :

Certaines routines passent à ce champ une valeur issue d’un bit de registre analogique.

Conclusion actuelle :

```text
tone_control
```

Rôle exact encore ouvert.

## 36.3 FIQ `0x80000`

Ancienne interprétation :

> bit directement associé à RTS_START.

Correction :

Le FIQ combine plusieurs tests et masques avant l’appel à `lmacProcessRtsStart()`.

Ce bit ne doit pas être nommé seul `RTS_START` sans analyse complémentaire.

## 36.4 TX MAC enable

Ancienne simplification :

```text
0x3FF20C94 bit0
```

Correction :

Les routines TX MAC touchent aussi d’autres registres WDEV, notamment autour de :

```text
0x3FF20DE0
```

Le MAC gate n’est donc pas un simple unique bit.

## 36.5 DWARF

Certaines parties de `libpp.a` contiennent beaucoup d’informations de debug utiles.

Pour `libphy.a`, il ne faut pas supposer un DWARF complet : l’analyse repose davantage sur :

- symboles ;
- relocations ;
- sections Xtensa ;
- ROM désassemblée ;
- chaînes de debug.

---


# 36A. Recherche additionnelle — distinction TX continu / TX tone

La documentation officielle Espressif de test RF distingue clairement deux mécanismes :

```text
tx_contin_en
```

et :

```text
wifiscwout
```

`tx_contin_en` sélectionne le mode de test de transmission continue / paquet à fort duty-cycle ou le mode instrument `iqview`.

`wifiscwout`, lui, commande explicitement le **single-carrier / TX tone** avec :

```text
enable
channel 1..14
power attenuation
```

L’atténuation est exprimée par pas de :

```text
0.25 dB
```

Conséquence importante :

> Il ne faut pas assimiler `tx_cont_en()` de `libphy.a` au générateur `rom_start_tx_tone()`.

Ce sont des mécanismes de test distincts.

Pour le projet OOK, le bloc pertinent reste donc :

```text
rom_start_tx_tone()
        +
registres 0x600005B8 / BC / C4
```

plutôt que le mode de paquets continus.

---

# 36B. Corroboration inter-générations Espressif

Sur des PHY Espressif ultérieurs, les ROM exportent séparément :

```text
rom_start_tx_tone_step
rom_start_tx_tone
rom_set_tx_dig_gain
rom_tx_paon_set
rom_loopback_mode_en
rom_spur_reg_write_one_tone
rom_stop_tx_tone
```

Cette séparation apporte une corroboration architecturale forte :

```text
tone step / paramètres
        │
gain numérique
        │
tone generator
        │
PA enable
        │
RF
```

Elle ne prouve pas que chaque champ de l’ESP8266 possède exactement le même sens, mais elle renforce deux conclusions :

1. le générateur de tone est bien une brique numérique distincte du PA ;
2. le champ `tone_control` de l’ESP8266 peut contenir une notion de step/mode plutôt qu’un simple gain.

Des travaux académiques de reverse-engineering sur des PHY Espressif plus récents ont aussi retrouvé :

```text
g_phyFuns
rom_set_loopback_gain
rom_loopback_mode_en
```

et décrivent l’usage d’un **loopback TX→RX avec génération de signaux sinusoïdaux** durant les calibrations.

Cela constitue une validation indépendante du modèle que nous avons reconstruit sur ESP8266.

---

# 36C. Nouvelle hiérarchie de confiance pour le projet OOK

| Élément | Interprétation | Confiance actuelle |
|---|---|---:|
| `rom_start_tx_tone` | générateur de tone/single-carrier interne | très élevée |
| `0x600005B8` | slot tone 1 | très élevée |
| `0x600005BC` | slot tone 2 | très élevée |
| `0x600005C4` | slot tone 3 | très élevée |
| zone 27:18 | zone de mode/test dans le packing observé ; `mode_code=1` ne positionne que bit18 en usage normal. Largeur hardware exacte non imposée par la ROM | élevée |
| bits 17:10 | code `(-digital_scale) mod 256`, `digital_scale` borné à 0..63 | ~99 % pour le codage logiciel |
| partie basse | `tone_control` brut ; valeurs observées 8/64, largeur 10 bits inférée du packing mais non masquée par la ROM | élevé pour le packing observé, sens physique ouvert |
| bit 18 | gate du mode normal ; seul bit du slot effacé par `stop_tx_tone()` | ~99 % |
| bit 28 slot 3 | copie du paramètre de chemin 3 ; configuration/armement sticky, rôle physique exact ouvert | élevée pour le comportement logiciel |
| `set_ana_inf_tx_scale` | partage exact : numérique `min(x,63)` + scale analogique au-delà du seuil 64 | très élevée |
| `pbus_set_txgain` | gain RF analogique | très élevée |
| `pbus_xpd_tx_*` | power/bias/activation chaîne RF | très élevée |
| `tx_cont_en` | mode de test continu, **pas le tone lui-même** | très élevée |


# 36D. Décodage plus précis de `rom_start_tx_tone()`

Le désassemblage ROM permet maintenant de reconstruire le comportement de la fonction avec beaucoup plus de précision.

## 36D.1 Organisation en trois triplets

La fonction accepte conceptuellement neuf paramètres groupés par trois :

```text
slot1: mode_1, control_1, coeff_1
slot2: mode_2, control_2, coeff_2
slot3: mode_3, control_3, coeff_3
```

Les six premiers sont transmis dans les registres ABI `a2..a7`. Les trois paramètres du troisième slot sont récupérés depuis la pile.

Pour le troisième triplet, le code charge explicitement :

```text
mode_3    : lecture 8 bits
control_3 : lecture 16 bits signée
coeff_3   : lecture 8 bits
```

Il est très probable que les trois slots utilisent des types conceptuellement symétriques.

## 36D.2 TX clock activé une seule fois à l'entrée

Avant de toucher aux slots, `rom_start_tx_tone()` appelle :

```text
g_phyFuns + 0x3C
= rom_set_txclk_en(1)
```

Donc :

```text
start_tx_tone()
     │
     ├── TX clock ON
     └── programmation conditionnelle des slots
```

## 36D.3 Un slot avec `enable = 0` n'est PAS effacé

Chaque slot commence par un test :

```text
if (mode == 0)
    skip_slot_write;
```

Conséquence :

> passer `mode=0` à `start_tx_tone()` ne désactive pas nécessairement un slot déjà actif ; cela laisse simplement son registre inchangé.

C'est une distinction importante entre :

```text
ne pas programmer un slot
```

et :

```text
désactiver un slot
```

La désactivation explicite est réalisée par `rom_stop_tx_tone()` ou par une modification directe du bit 18.

## 36D.4 Formule exacte — slot 1

Le code réalise conceptuellement :

```c
if (mode_1 != 0) {
    uint32_t r = REG32(0x600005B8);

    uint8_t neg_coeff = (uint8_t)(0x100 - coeff_1);

    r &= 0xF0000000;
    r |= control_1;
    r |= ((uint32_t)neg_coeff << 10);
    r |= ((uint32_t)mode_1 << 18);

    REG32(0x600005B8) = r;
}
```

Cela établit formellement :

```text
bits 31..28 : préservés
zone 27..18 : résultat de (mode_1 << 18), sans masque de largeur dans la ROM
bits 17..10 : (-coeff_1) modulo 256, exactement 8 bits
partie basse : control_1 ORé brut ; usages observés compatibles avec les bits 9..0
```

## 36D.5 Formule exacte — slot 2

Le slot 2 est presque identique :

```c
if (mode_2 != 0) {
    uint32_t r = REG32(0x600005BC);

    uint8_t neg_coeff = (uint8_t)(0x100 - coeff_2);

    r &= 0xF0000000;
    r |= control_2;
    r |= ((uint32_t)neg_coeff << 10);
    r |= ((uint32_t)mode_2 << 18);

    REG32(0x600005BC) = r;
}
```

## 36D.6 Formule exacte — slot 3

Le troisième slot diffère :

```c
if (enable_3 != 0) {
    uint32_t r = REG32(0x600005C4);

    uint8_t neg_coeff = (uint8_t)(0x100 - coeff_3);

    r &= 0xE0000000;
    r |= ((uint32_t)enable_3 << 28);
    r |= ((uint32_t)enable_3 << 18);
    r |= control_3;
    r |= ((uint32_t)neg_coeff << 10);

    REG32(0x600005C4) = r;
}
```

Donc le même paramètre `enable_3` alimente :

```text
bit 18
ET
bit 28
```

### Interprétation corrigée du bit 28

Le bit 28 ne semble donc pas être un paramètre séparé fourni par l'appelant.

Il est automatiquement activé avec le troisième slot.

Mais `rom_stop_tx_tone(3)` n'efface que le **bit 18** et laisse le **bit 28** intact.

Conclusion actuelle :

> le bit 28 ressemble davantage à un **mode/configuration sticky du troisième chemin** qu'à son gate temps réel principal.

Confiance : élevée.

---

# 36E. Comportement exact de `rom_stop_tx_tone()`

`rom_stop_tx_tone(selector)` accepte :

```text
1 → slot 1
2 → slot 2
3 → slot 3
autre → les trois slots
```

Pour chacun, il applique :

```text
REG &= ~0x00040000
```

donc il efface uniquement :

```text
bit 18
```

Il ne remet pas à zéro les autres champs du registre.

Ensuite, **dans tous les cas**, la fonction appelle :

```text
rom_set_txclk_en(0)
```

Donc même :

```text
stop_tx_tone(1)
```

coupe finalement l'horloge TX globale.

Conséquence architecturale :

> `rom_stop_tx_tone()` est un wrapper de sortie du mode tone, et non un gate indépendant idéal pour une modulation rapide.

---

# 36F. Correction majeure — `TXIQ` ne prouve PAS une mise à jour à chaud du tone

Une confusion d'offset a été identifiée et corrigée.

La table `g_phyFuns` contient :

```text
+104 décimal = +0x68  → rom_start_tx_tone
+260 décimal = +0x104 → rom_rfcal_txiq_cover
```

Dans `rom_rfcal_txiq()`, les deux instructions précédemment interprétées comme des appels à
`start_tx_tone()` chargent en réalité :

```text
g_phyFuns + 0x104
```

c'est-à-dire :

```text
rom_rfcal_txiq_cover
```

et non :

```text
rom_start_tx_tone
```

Conséquence :

> **la reprogrammation "à chaud" du coefficient `bits 17:10` pendant un tone actif n'est PAS démontrée par TXIQ.**

Cette affirmation des versions antérieures du document est retirée.

`rom_rfcal_txiq()` appelle ensuite :

```text
g_phyFuns + 108 décimal = rom_stop_tx_tone
```

avec le sélecteur `1`.

Le corps observé de `rom_rfcal_txiq()` ne contient cependant aucun appel direct à
`rom_start_tx_tone()`.

Interprétations encore possibles :

- le tone a été activé par le contexte appelant ;
- un chemin imbriqué prépare le stimulus avant cette portion de calibration ;
- `stop_tx_tone(1)` sert simplement de nettoyage défensif.

À ce stade, aucune de ces explications n'est suffisamment démontrée pour être retenue comme fait.

---

# 36G. Recensement des vrais appels ROM à `start_tx_tone()`

La recherche de l'instruction correspondant à :

```text
l32i ..., ..., 104 décimal
```

dans toute la ROM désassemblée fournit **trois appels directs démontrés** à
`g_phyFuns[104/4] = rom_start_tx_tone`.

## 36G.1 `rom_rfcal_pwrctrl()`

Séquence observée :

```text
set_ana_inf_tx_scale(...)
        ↓
start_tx_tone(
    enable1 = 1,
    control1 = argument / variable de calibration,
    coeff1 = retour set_ana_inf_tx_scale,
    enable2 = 0,
    control2 = 0,
    coeff2 = 0,
    enable3 = 0,
    control3 = 0,
    coeff3 = 0
)
```

Donc :

```text
slot 1 utilisé
slot 2 non programmé
slot 3 non programmé
```

Le `tone_control` du slot 1 reste un axe séparé de la puissance : il provient d'une
valeur de calibration distincte, tandis que le coefficient passe par
`set_ana_inf_tx_scale()`.

## 36G.2 `rom_rfcal_rxiq()`

La séquence est de la même forme :

```text
set_ana_inf_tx_scale(level)
        ↓
start_tx_tone(
    1,
    tone_control,
    digital_scale,
    0,0,0,
    0,0,0
)
        ↓
mesure / correction RXIQ
        ↓
stop_tx_tone(1)
```

Le `tone_control` vient ici d'une autre branche de configuration analogique et peut,
dans ce contexte, prendre une valeur très petite (notamment issue d'un champ/bit
analogique).

Cela réduit encore la confiance dans l'ancienne hypothèse :

```text
tone_control = fréquence pure
```

Le nom neutre `tone_control` reste donc préférable.

## 36G.3 `rom_rfcal_txcap()`

Troisième appel direct :

```text
set_ana_inf_tx_scale(...)
        ↓
start_tx_tone(
    1,
    tone_control externe,
    digital_scale,
    0,0,0,
    0,0,0
)
```

Là encore :

```text
slot 1 uniquement
```

## 36G.4 Conclusion sur les trois slots

Dans les **trois usages directs du générateur trouvés dans la ROM** :

```text
slot 1 = utilisé
slot 2 = jamais activé
slot 3 = jamais activé
```

Cela change sensiblement l'interprétation des slots 2 et 3.

Il n'existe actuellement **aucune preuve ROM** qu'ils soient utilisés par les calibrations
ordinaires `pwrctrl`, `rxiq`, `txcap` ou `txiq`.

Hypothèses encore ouvertes pour slots 2/3 :

- génération multi-tone ;
- cancellation / mesure de spur ;
- modes usine/test ;
- chemins utilisés uniquement par `libphy` v6 ;
- fonctions laissées pour une autre révision du PHY.

Ces hypothèses restent non démontrées.

---

# 36G-A. Raffinement du slot 3 / bit 28

Le décodage exact de `rom_start_tx_tone()` montre :

```c
if (enable_3 != 0) {
    ...
    r |= ((uint32_t)enable_3 << 28);
    r |= ((uint32_t)enable_3 << 18);
    ...
}
```

Donc :

> **le bit 28 n'est pas un paramètre indépendant : il est une copie du même `enable_3` qui alimente le bit 18.**

En revanche, `rom_stop_tx_tone(3)` applique seulement :

```text
REG &= ~0x00040000
```

donc efface :

```text
bit 18
```

mais ne retire pas :

```text
bit 28
```

Le meilleur modèle actuel est donc :

```text
bit 18 → gate actif/arrêt du slot 3
bit 28 → configuration/armement sticky associé au chemin 3
```

Confiance :

```text
bit18 = gate          très élevée
bit28 = sticky config élevée sur le comportement,
                       rôle physique exact encore ouvert
```

---

# 36G-B. Propriété importante de `start_tx_tone()`

Pour chaque slot :

```c
if (enable == 0)
    skip_write();
```

Ainsi :

> appeler `start_tx_tone()` avec `enable=0` ne remet pas un slot à zéro et ne l'éteint pas ; la routine laisse simplement son registre inchangé.

Le mécanisme explicite d'arrêt utilisé par la ROM est :

```text
effacement du bit18
```

via `rom_stop_tx_tone()`.

Cependant `rom_stop_tx_tone()` coupe ensuite globalement :

```text
set_txclk_en(0)
```

ce qui confirme que le wrapper ROM est davantage une **routine de sortie du mode tone**
qu'un gate rapide par slot.

---

# 36G-C. État de la piste coefficient `bits 17:10`

Le rôle du troisième paramètre reste fortement lié au niveau numérique parce qu'il est
alimenté par le retour de :

```text
set_ana_inf_tx_scale()
```

dans les trois appels de calibration observés.

Ce qui est démontré :

```text
bits17:10 = (256 - coeff) & 0xff
coeff     = sortie numérique de set_ana_inf_tx_scale()
```

Ce qui n'est PAS encore démontré :

```text
modification dynamique pendant un tone actif
coeff=0 → extinction RF
relation linéaire coeff → puissance RF
```

La piste ASK/OOK par coefficient reste donc intéressante, mais revient au statut :

```text
plausible / à caractériser
```

et non :

```text
mise à jour à chaud démontrée
```

# 36H. Conséquences pour le candidat OOK

Le classement est maintenant légèrement modifié.

## Gate bit 18

Toujours le candidat le plus évident pour un ON/OFF binaire :

```text
bit18 = 0 / 1
```

Avantages :

- un seul champ ;
- `stop_tx_tone()` confirme qu'il s'agit du mécanisme de désactivation du slot ;
- pas besoin de PBUS/XPD.

## Coefficient bits 17:10

Cette piste reste **architecturalement intéressante**, mais la reprogrammation à chaud du coefficient n'est plus considérée comme démontrée.

Architecture possible :

```text
TX clock stable
tone slot stable
RF chain stable
      │
      ▼
coefficient numérique potentiellement modifiable
      │
      ▼
amplitude/composante du tone modifiée
```

Question critique restante :

> quelle relation exacte existe entre le coefficient et l'amplitude RF finale ?

## Slot 3

Le bit28 n'est plus considéré comme un gate indépendant.

Il est maintenant classé comme :

```text
configuration/mode sticky associé au slot 3
```

car :

- `start_tx_tone()` le pose automatiquement ;
- `stop_tx_tone()` ne le retire pas.

---

# 36I. Nouvelles questions prioritaires

| Priorité | Question |
|---:|---|
| 1 | Quelle amplitude RF correspond à chaque valeur du coefficient du slot ? |
| 2 | `coeff=0` produit-il zéro composante, amplitude maximale, ou une autre convention ? |
| 3 | Le bit18 arrête-t-il le NCO ou seulement sa sortie ? |
| 4 | La phase continue-t-elle pendant bit18=0 ? |
| 5 | Le coefficient peut-il être modifié pendant un tone actif, et si oui est-il pris immédiatement ou à une frontière de cycle ? |
| 6 | Où les slots 2 et 3 sont-ils réellement utilisés, puisqu'aucun appel ROM direct trouvé ne les active ? |
| 7 | Pourquoi le slot3 nécessite-t-il aussi bit28 ? |
| 8 | Quelle fonction hardware lit le champ `control` bas ? |
| 9 | `control` est-il un pas de NCO, une phase, un index de spur ou une combinaison de champs ? |
| 10 | **Résolu en §36K : non.** `phy_dig_spur_set/prot` utilisent un sous-système baseband distinct (`0x600096xx/0x60009Axx`). |


# 36J. Nouvelle constatation — slots 2/3 absents des usages ROM directs

Le scan des trois opcodes `l32i ..., ..., 104 décimal` associés à la table PHY permet de
dire avec une confiance élevée que les appels directs à `rom_start_tx_tone()` trouvés
dans la mask-ROM sont limités à :

```text
rom_rfcal_pwrctrl
rom_rfcal_rxiq
rom_rfcal_txcap
```

Dans chacun des trois cas :

```text
enable1 = 1
enable2 = 0
enable3 = 0
```

C'est une information importante pour la suite du reverse-engineering :

> les slots 2 et 3 ne sont pas nécessaires au stimulus de base des trois calibrations ROM principales qui utilisent le tone.

La prochaine recherche doit donc se concentrer sur :

```text
libphy v6:
  phy_dig_spur_set()
  phy_dig_spur_prot()
  tx_cont_en()/dis()
  chemins de test/calibration v6

ou

modes usine / production non présents comme appels directs ROM
```

Le nom des fonctions `phy_dig_spur_set` / `phy_dig_spur_prot` en fait des candidats
particulièrement intéressants pour expliquer l'existence de plusieurs générateurs,
mais **aucune connexion aux slots 2/3 n'est encore démontrée**.


# 36K. Analyse de `phy_dig_spur_set()` / `phy_dig_spur_prot()`

Une nouvelle piste a été étudiée pour expliquer l'existence des tone slots 2 et 3 :

```text
phy_dig_spur_set()
phy_dig_spur_prot()
```

Ces fonctions existent dans `phy_chip_v6.o` de la bibliothèque analysée.

Tailles observées dans le binaire fourni :

```text
phy_dig_spur_set   : 0x359 octets
phy_dig_spur_prot  : 0x223 octets
```

Ce sont donc des routines relativement importantes, et non de simples wrappers.

## 36K.1 Littéraux hardware de `phy_dig_spur_set()`

La literal pool immédiatement associée contient notamment :

```text
0x60009600
0x00005DC0
0x00000028
0x000FFFFF
0xC0000000
```

ainsi que plusieurs constantes utilisées par du calcul flottant/entier.

La routine contient de nombreuses relocations vers :

```text
__modsi3
__divsi3
__floatsisf
__floatunsisf
__divsf3
__mulsf3
__fixsfsi
__floatsidf
__muldf3
__divdf3
__fixdfsi
```

Interprétation forte :

> `phy_dig_spur_set()` calcule des coefficients/paramètres numériques avant de programmer un bloc du baseband, au lieu de seulement basculer un bit de contrôle.

Le seul bloc MMIO direct clairement visible dans sa literal pool est :

```text
0x60009600
```

Aucune référence directe à :

```text
0x600005B8
0x600005BC
0x600005C4
```

n'apparaît dans la literal pool de cette fonction.

Aucun appel externe à :

```text
rom_start_tx_tone
rom_stop_tx_tone
PBUS
I2C analogique
```

n'apparaît dans ses relocations.

## 36K.2 `phy_dig_spur_prot()`

La routine de protection utilise directement deux bases hardware :

```text
0x60009600
0x60009A00
```

Là encore :

- pas de référence directe aux registres tone `0x600005B8/BC/C4` ;
- pas d'appel à `start_tx_tone()` ;
- pas de PBUS ;
- pas d'I²C analogique.

Conclusion :

> le mécanisme **digital spur** du PHY v6 est un sous-système du baseband distinct du générateur de tone ROM.

## 36K.3 Conséquence pour les slots 2 et 3

L'hypothèse suivante était auparavant considérée plausible :

```text
tone slot 2/3
     ↓
spur cancellation
```

L'analyse de `phy_dig_spur_set/prot` ne la supporte pas.

Statut actuel :

```text
slots 2/3 = mécanisme spur v6
→ improbable / non supporté par les binaires analysés
```

Les fonctions spur utilisent leurs propres registres dans les régions :

```text
0x600096xx
0x60009Axx
```

et non le bloc tone autour de :

```text
0x600005B8
```

Les raisons possibles de l'existence des slots 2/3 restent donc :

- mode multi-tone de test ;
- stimulus spécial de calibration non utilisé par la ROM standard ;
- fonction liée à une autre révision silicium ;
- mode usine/ATE ;
- fonctionnalité prévue mais rarement utilisée ;
- chemin utilisé indirectement par un autre firmware/bloc non encore identifié.

---

# 36L. `IQ_EST` : layout confirmé instruction par instruction

Le désassemblage annoté de la ROM confirme maintenant sans ambiguïté le corps de :

```text
rom_iq_est_enable @ 0x40006430
rom_iq_est_disable @ 0x40006400
```

Le registre de contrôle est atteint via :

```text
0x60000200 + 0x37C
= 0x6000057C
```

`rom_iq_est_enable(a2, a3)` effectue :

```text
REG |= 1
```

puis :

```text
N = a3 & 0x7FFF
N <<= 2

mode = a2 << 18

control =
    preserved_bits
  | mode
  | N
  | 0x2
```

Le bit 0 est donc activé séparément avant le lancement.

La fonction attend ensuite la transition du bit de signe du registre, donc :

```text
bit 31 = état/done
```

Le layout est confirmé comme :

```text
31                       18 17 16........2 1 0
┌─────────────────────────┬──┬────────────┬─┬─┐
│ DONE / status           │M │ N[14:0]    │S│E│
└─────────────────────────┴──┴────────────┴─┴─┘
```

avec :

```text
E = enable
S = start
M = argument a2, placé au bit18
N = longueur d'intégration / accumulation
```

Le rôle physique exact du mode `M` reste encore ouvert.

---

# 36M. Nouvelle hiérarchie des hypothèses sur les tone slots

Après élimination de la piste spur :

| Hypothèse pour slots 2/3 | Statut |
|---|---|
| digital spur cancellation v6 | **peu probable / écartée provisoirement** |
| multi-tone de test | plausible |
| stimulus I/Q spécial | plausible |
| mode usine / ATE | plausible |
| fonctionnalité d'une autre révision PHY | plausible |
| utilisation par calibrations ROM ordinaires | non observée |
| utilisation par `rfcal_pwrctrl/rxiq/txcap` | non : slot1 uniquement |

Le prochain travail doit cibler toutes les fonctions qui :

1. accèdent directement au bloc `0x600005xx` ;
2. utilisent le mode test TX continu ;
3. utilisent les objets `ate_test` ou fonctions de production ;
4. manipulent des coefficients numériques TX sans passer par `start_tx_tone()`.


# 36N. Décodage de `tx_cont_en()` / `tx_cont_dis()`

Les relocations Xtensa et les opcodes simples `L32R/L32I/S32I/AND/OR` permettent
maintenant de reconstruire le cœur de ces deux routines du `libphy.a` fourni.

Symboles :

```text
tx_cont_en   @ 0x2C74, taille 0x9E
tx_cont_dis  @ 0x2D20, taille 0x4F
tx_cont_cfg  @ 0x2D70, taille 0x18
```

`tx_cont_cfg()` est essentiellement un dispatcher qui sélectionne `tx_cont_en()` ou
`tx_cont_dis()`.

## 36N.1 Registres sauvegardés

`tx_cont_en()` charge :

```text
base = 0x60000200
```

puis lit successivement :

```text
base + 0x394 = 0x60000594
base + 0x398 = 0x60000598
base + 0x39C = 0x6000059C
```

et sauvegarde les trois valeurs dans une structure `.bss`.

Cette séquence est directement visible sous la forme :

```text
L32I ..., base, 0xE5  → offset 0xE5*4 = 0x394
L32I ..., base, 0xE6  → offset 0x398
L32I ..., base, 0xE7  → offset 0x39C
```

## 36N.2 Modifications appliquées par `tx_cont_en()`

Après sauvegarde, la routine revient sur les mêmes trois registres.

### `0x6000059C`

La valeur est lue, puis combinée par OR avec :

```text
0x0FE03F80
```

Pseudo-code :

```c
REG32(0x6000059C) |= 0x0FE03F80;
```

### `0x60000598`

La valeur est combinée par OR avec :

```text
0x0FFFFFFF
```

Pseudo-code :

```c
REG32(0x60000598) |= 0x0FFFFFFF;
```

### `0x60000594`

La valeur est combinée par AND avec :

```text
0xFFCFFFFF
```

donc les bits correspondant au masque complémentaire :

```text
~0xFFCFFFFF = 0x00300000
```

sont effacés.

Pseudo-code :

```c
REG32(0x60000594) &= 0xFFCFFFFF;
```

soit :

```text
clear bits 20 et 21
```

## 36N.3 `tx_cont_dis()`

La routine inverse recharge les trois valeurs sauvegardées et les réécrit dans :

```text
0x60000594
0x60000598
0x6000059C
```

Elle restaure donc explicitement l'état hardware précédant le mode TX continu.

## 36N.4 Interprétation architecturale

Le registre :

```text
0x60000594
```

est déjà connu comme registre de commande utilisé par `rom_pbus_force_test()`.

Le mode continuous-TX travaille donc dans le **bloc de commande/test PHY-PBUS adjacent** :

```text
0x60000594
0x60000598
0x6000059C
```

et non dans les tone slots :

```text
0x600005B8
0x600005BC
0x600005C4
```

C'est une séparation importante :

```text
TX continuous test
       │
       └── 0x60000594/598/59C

TX tone / single carrier generator
       │
       └── 0x600005B8/5BC/5C4
```

Conclusion :

> `tx_cont_en()` et `rom_start_tx_tone()` sont deux mécanismes hardware réellement
> distincts, même s'ils appartiennent à la même grande région PHY `0x600005xx`.

Cette observation est cohérente avec la documentation officielle de test RF Espressif,
qui distingue transmission continue et single-carrier/tone.

---

# 36O. Carte affinée de la région PHY `0x600005xx`

Les découvertes permettent maintenant de proposer la carte partielle suivante :

```text
0x60000504..560  table d'atténuation / puissance TX BB

0x6000057C       contrôle IQ estimator
0x60000580..58C  résultats / matrice de corrélation IQ

0x60000594       PBUS/test command
0x60000598       contrôle test/continuous-TX associé
0x6000059C       contrôle test/continuous-TX associé
0x600005A0       PBUS status

0x600005B8       tone slot 1
0x600005BC       tone slot 2
0x600005C4       tone slot 3

0x600005DC       accumulateur I / DC-I
0x600005E0       accumulateur Q / DC-Q
0x600005E4       énergie / puissance totale
```

Le bloc présente donc une organisation remarquable :

```text
power tables
    ↓
IQ estimator
    ↓
PBUS / test controls
    ↓
tone generators
    ↓
IQ/power accumulators
```

Cela renforce l'idée que `0x600005xx` correspond à une zone importante
d'instrumentation/calibration du PHY.

---

# 36P. Conséquence pour le projet OOK

Deux chemins qui pouvaient initialement sembler proches doivent désormais être
strictement séparés.

## Chemin A — TX continu

```text
tx_cont_en()
    ↓
sauvegarde 0x594/598/59C
    ↓
configuration spéciale du bloc test
    ↓
continuous TX
```

Ce chemin modifie plusieurs registres de test et nécessite une sauvegarde/restauration.

Il n'est donc pas le candidat le plus minimal pour un keying rapide.

## Chemin B — tone generator

```text
TX clocks ON
    ↓
slot tone configuré
    ↓
bit18 du slot
```

Ce chemin reste le meilleur candidat architectural pour un gate numérique minimal,
car il se situe après la configuration globale du mode PHY et n'exige pas la
reconfiguration du triplet continuous-test.

La distinction est maintenant fondée directement sur le binaire du SDK fourni.


# 36Q. `tx_cont_cfg()` est un setter booléen

Le désassemblage manuel des instructions Xtensa autour de :

```text
tx_cont_cfg @ 0x2D70
```

permet maintenant de reconstruire sa logique avec une forte confiance.

Séquence essentielle :

```text
0x2D75  BNEI  a2, 1, disable_path
0x2D78  CALL0 tx_cont_en
...
0x2D7E  CALL0 tx_cont_dis
```

Pseudo-code :

```c
void tx_cont_cfg(int enable)
{
    if (enable == 1)
        tx_cont_en();
    else
        tx_cont_dis();
}
```

Donc :

> `tx_cont_cfg()` n'encode pas un niveau de puissance ou un sous-mode ; il active ou désactive simplement le mode TX continu.

## 36Q.1 Correction v0.20 — appel depuis `chip_v6_initialize_bb()`, pas `periodic_cal()`

Une nouvelle vérification des **bornes de symboles** et des relocations de `phy_chip_v6.o` corrige l'attribution faite dans les versions antérieures.

Les deux relocations vers `tx_cont_cfg()` sont situées à :

```text
0x2EFD
0x2F05
```

alors que les symboles sont :

```text
chip_v6_initialize_bb @ 0x2DCC
periodic_cal          @ 0x2F24
```

Les appels appartiennent donc à **`chip_v6_initialize_bb()`**, avant le début de `periodic_cal()`. Le code sélectionne bien :

```text
tx_cont_cfg(1)
```

ou :

```text
tx_cont_cfg(0)
```

selon un octet d'état interne, mais cette sélection se fait pendant l'initialisation BB et non à chaque exécution de `periodic_cal()`.

`periodic_cal()` lui-même appelle les primitives de calibration/backup observées dans son propre intervalle de symbole, sans relocation vers `tx_cont_cfg()`.

Conséquence OOK :

> la calibration périodique ne rebascule pas directement le mode continuous-TX à chaque passage. Les interférences logicielles avec un tone autonome sont donc moins fréquentes que ce que suggérait l'ancienne attribution.

## 36Q.2 Appel depuis `phy_wakeup_rf()`

La même structure existe dans `phy_wakeup_rf()` :

```text
MOVI.N a2, 1
CALL0  tx_cont_cfg
```

contre :

```text
MOVI.N a2, 0
CALL0  tx_cont_cfg
```

Cela confirme indépendamment que le paramètre est booléen.

Conséquence :

> le continuous-TX est géré comme un **état hardware global à activer/désactiver**, et non comme une primitive d'amplitude fine.

---

# 36R. Identification de `tx_data2`, `tx_data3`, `tx_data4`

Le fichier `phy_chip_v6.o` contient quatre variables `.bss` :

```text
tx_data1 @ +0xD0
tx_data2 @ +0xD4
tx_data3 @ +0xD8
tx_data4 @ +0xDC
```

Le désassemblage de `tx_cont_en()` relie maintenant trois d'entre elles sans ambiguïté aux registres du mode TX continu.

## 36R.1 Sauvegardes réalisées par `tx_cont_en()`

Le code effectue :

```text
read 0x60000594
store -> tx_data2

read 0x60000598
store -> tx_data3

read 0x6000059C
store -> tx_data4
```

Pseudo-code :

```c
tx_data2 = REG32(0x60000594);
tx_data3 = REG32(0x60000598);
tx_data4 = REG32(0x6000059C);
```

Puis la routine applique les modifications déjà reconstruites :

```c
REG32(0x6000059C) |= 0x0FE03F80;
REG32(0x60000598) |= 0x0FFFFFFF;
REG32(0x60000594) &= 0xFFCFFFFF;
```

## 36R.2 Restauration par `tx_cont_dis()`

Le chemin inverse charge :

```text
tx_data2 -> 0x60000594
tx_data3 -> 0x60000598
tx_data4 -> 0x6000059C
```

Pseudo-code :

```c
REG32(0x60000594) = tx_data2;
REG32(0x60000598) = tx_data3;
REG32(0x6000059C) = tx_data4;
```

La paire :

```text
tx_cont_en()
tx_cont_dis()
```

forme donc bien une transaction de type :

```text
save state
    ↓
enter continuous-TX mode
    ↓
...
    ↓
restore previous state
```

## 36R.3 `tx_data1` — identification complète

Le désassemblage instruction par instruction montre que le helper appelé juste avant les
trois snapshots MMIO est :

```text
g_phyFuns + 0x90
= rom_i2c_readReg
```

avec les arguments :

```text
block   = 0x66
host_id = 3
reg     = 1
```

Le retour est immédiatement sauvegardé dans :

```text
tx_data1 @ .bss + 0xD0
```

Donc :

```c
tx_data1 = rom_i2c_readReg(0x66, 3, 1);
```

Après les snapshots, `tx_cont_en()` appelle :

```text
g_phyFuns + 0x9C
= rom_i2c_writeReg_Mask
```

avec :

```c
rom_i2c_writeReg_Mask(0x66, 3, 1, 5, 0, 60);
```

soit :

```text
registre analogique interne (0x66, host 3, reg 1)
bits 5:0 := 0x3C
```

Le chemin inverse est également démontré. `tx_cont_dis()` charge :

```text
g_phyFuns + 0x98
= rom_i2c_writeReg
```

puis restaure :

```c
rom_i2c_writeReg(0x66, 3, 1, (uint8_t)tx_data1);
```

Conclusion :

> `tx_data1` est le snapshot du registre analogique interne `(block=0x66, host=3, reg=1)`.
> Le continuous-TX modifie donc **un état analogique I²C interne en plus des trois registres MMIO**.

### Prudence sur le nom du bloc `0x66`

Sur plusieurs générations Espressif ultérieures, l'identifiant interne `0x66` est utilisé
pour un bloc nommé `BBPLL`. En revanche, un header public ESP8266 associe `i2c_bbpll` à
`0x67` avec un autre host ID. Il serait donc incorrect de transférer automatiquement le
nom `BBPLL` au bloc `0x66` de ce binaire ESP8266.

Statut recommandé :

```text
0x66 / host 3 / reg 1 = bloc analogique interne, fonction physique encore ouverte
```

---

# 36S. Évolution des chemins ATE/test entre anciens SDK et la bibliothèque analysée

Les anciennes listes publiques de symboles ESP8266 montraient notamment :

```text
ate_test.o
operation_test()
slop_test()
```

Dans le `libphy.a` actuellement analysé :

```text
ate_test.o
```

n'est plus présent.

Le membre :

```text
phy_chip_v6_unused.o
```

ne contient plus que quelques octets de stubs très courts, notamment :

```text
chip_v6_set_sense
chip_v6_get_sense
chip_v6_unset_chanfreq
```

Les anciens chemins :

```text
operation_test
slop_test
```

n'y sont plus.

Interprétation :

> une partie des anciens chemins de test usine/ATE a été retirée, déplacée ou éliminée dans cette révision du SDK.

Conséquence pour le reverse-engineering des tone slots 2/3 :

- leur absence dans les calibrations ROM normales reste confirmée ;
- leur absence des chemins test actuels ne prouve pas qu'ils soient inutilisés dans le silicium ;
- certaines fonctionnalités peuvent avoir été réservées à d'anciens outils usine ou à un firmware ATE non inclus dans cette bibliothèque.

---

# 36T. Carte d'état affinée du continuous-TX

Le modèle actuellement démontré est :

```text
                    tx_cont_cfg(enable)
                           │
               ┌───────────┴───────────┐
               │                       │
          enable == 1             enable != 1
               │                       │
               ▼                       ▼
          tx_cont_en()             tx_cont_dis()
               │                       │
       save tx_data1..4          restore state
               │                       │
               ▼                       ▼
      0x594/598/59C modifiés     0x594/598/59C restaurés
               │
               ▼
        continuous-TX mode
```

Snapshots démontrés :

```text
tx_data1 ↔ I²C analogique interne (block 0x66, host 3, reg 1)
tx_data2 ↔ 0x60000594
tx_data3 ↔ 0x60000598
tx_data4 ↔ 0x6000059C
```

Modification analogique supplémentaire démontrée :

```text
rom_i2c_writeReg_Mask(0x66, 3, 1, 5, 0, 60)
→ bits 5:0 = 0x3C
```

Ce mécanisme reste architecturalement plus lourd que le gate local du tone slot.

Pour le projet OOK, cela renforce encore la distinction :

```text
continuous-TX
= état global / multi-registres / save-restore

tone slot bit18
= gate local à un générateur
```

# 36U. Flag d'état du continuous-TX (`.bss + 0xC4`)

Le désassemblage révèle un octet `.bss` sans symbole public à l'offset :

```text
+0xC4
```

Son comportement permet de lui attribuer fonctionnellement le nom provisoire :

```text
tx_cont_state_flag
```

## 36U.1 Dans `tx_cont_en()`

La routine lit d'abord ce byte. Si sa valeur est déjà non nulle, elle saute la séquence
de sauvegarde/configuration et termine en maintenant le flag à `1`.

Pseudo-code fonctionnel :

```c
if (tx_cont_state_flag == 0) {
    tx_data1 = rom_i2c_readReg(0x66, 3, 1);
    tx_data2 = REG32(0x60000594);
    tx_data3 = REG32(0x60000598);
    tx_data4 = REG32(0x6000059C);

    rom_i2c_writeReg_Mask(0x66, 3, 1, 5, 0, 60);

    REG32(0x6000059C) |= 0x0FE03F80;
    REG32(0x60000598) |= 0x0FFFFFFF;
    REG32(0x60000594) &= 0xFFCFFFFF;
}

tx_cont_state_flag = 1;
```

Conséquence : une deuxième activation ne remplace pas les snapshots originaux par l'état
déjà modifié.

## 36U.2 Dans `tx_cont_dis()`

La restauration n'est exécutée que si le flag vaut `1` :

```c
if (tx_cont_state_flag == 1) {
    rom_i2c_writeReg(0x66, 3, 1, (uint8_t)tx_data1);

    REG32(0x60000594) = tx_data2;
    REG32(0x60000598) = tx_data3;
    REG32(0x6000059C) = tx_data4;
}

tx_cont_state_flag = 0;
```

Conclusion :

> le mode continuous-TX possède un mécanisme explicite d'idempotence/save-restore.
> Le snapshot original est protégé contre une réactivation imbriquée.

Ce comportement confirme encore que `tx_cont_en/dis` est un changement d'état global
du PHY et non un gate d'amplitude destiné à une modulation symbole par symbole.

---

# 36V. Raffinement de `rom_rfcal_pwrctrl()` : indépendance de `tone_control` et du coefficient

Le chemin de `rom_rfcal_pwrctrl()` vers `rom_start_tx_tone()` a été retracé plus
précisément.

À l'entrée, le premier argument de `rom_rfcal_pwrctrl()` est conservé. Juste avant
l'appel au générateur, cette valeur est placée directement dans le deuxième argument de
`rom_start_tx_tone()`, c'est-à-dire `control_1`.

En parallèle, `coeff_1` provient du retour de :

```text
rom_set_ana_inf_tx_scale(...)
```

Le schéma est donc :

```text
rom_rfcal_pwrctrl(arg0, ...)
         │
         ├──────────────► control_1 = arg0
         │
         └─ set_ana_inf_tx_scale(...)
                        │
                        └──────────► coeff_1

start_tx_tone(
    enable_1  = 1,
    control_1 = arg0,
    coeff_1   = digital_scale,
    0,0,0,
    0,0,0
)
```

Conclusion :

> dans `rfcal_pwrctrl`, `tone_control` et le coefficient d'amplitude sont deux entrées
> indépendantes. `tone_control` n'est pas dérivé du réglage de puissance numérique.

Cela renforce le choix de conserver le nom neutre `tone_control` tant que son sens
physique exact n'est pas établi.

---


# 36W. Topologie des appelants des calibrations v6 et conséquence pour `tone_control`

Une nouvelle passe sur les relocations Xtensa du `phy_chip_v6.o` fourni permet de distinguer
les simples pointeurs/littéraux (`R_XTENSA_32`) des **appels de code réels**
(`R_XTENSA_ASM_EXPAND`).

Les appels directs suivants sont démontrés dans le binaire fourni :

| Appelant | Offset de l’appel | Cible |
|---|---:|---|
| `set_rx_gain_cal_iq()` | `0x36A` | `ram_rfcal_rxiq()` |
| `tx_cap_init()` | `0xCBF` | `ram_rfcal_pwrctrl()` |
| `tx_cap_init()` | `0xCF8` | `ram_rfcal_txcap()` |
| `tx_pwctrl_init_cal()` | `0xEEA` | `ram_rfcal_pwrctrl()` |
| `chip_v6_initialize_bb()` | `0x2E88` | `ram_rfcal_txiq()` |

La partie importante pour le générateur de tone est que :

```text
              tx_cap_init()
              /          \
             v            v
 ram_rfcal_pwrctrl()   ram_rfcal_txcap()
             ^
             |
     tx_pwctrl_init_cal()
```

`ram_rfcal_pwrctrl()` n’est donc pas un helper réservé à une seule boucle de puissance :
il participe aussi au chemin d’initialisation/calibration TX-cap.

Or la section 36V a déjà établi que le premier argument de `rfcal_pwrctrl()` est transféré
directement vers `control_1` de `start_tx_tone()`, tandis que l’amplitude numérique vient
séparément de `set_ana_inf_tx_scale()`.

Conséquence :

> `tone_control` ressemble davantage à un paramètre définissant le **stimulus numérique**
> (step/incrément, signe, mode ou combinaison de sous-champs) qu’à un second gain.

Cette conclusion est renforcée, mais pas prouvée, par la structure de PHY Espressif
ultérieurs : leurs ROM exposent explicitement une primitive `rom_start_tx_tone_step()`
séparée de `rom_start_tx_tone()` et du gain numérique.

Statut de confiance actuel :

| Hypothèse pour `tone_control` | Statut actuel |
|---|---|
| second gain/amplitude | **très faible / pratiquement écarté** |
| step / index NCO ou équivalent | **hypothèse principale, élevée mais non prouvée physiquement** |
| valeur signée | **non supportée par les chemins v6 décodés : seules `8` et `64` sont observées** |
| sélection de sideband / phase | plausible comme effet ou sous-champ associé au step |
| mode I/Q ou autre sélection | encore possible |

Il n’est toujours pas justifié de renommer formellement les bits `9:0` en `tone_step`.
La nouveauté importante de la v0.11 est que leur valeur n’est plus seulement théorique :
les chemins v6 décodés utilisent au moins **deux valeurs distinctes, `8` et `64`**, suivant
le type de calibration. La loi qui relie ces valeurs à l’effet physique du générateur reste
à démontrer.

## 36W.1 Vérification statique ciblée — état v0.11

Les deux premiers objectifs de la v0.10 sont désormais terminés :

1. les arguments de `ram_rfcal_pwrctrl()` aux offsets `0xCBF` et `0xEEA` ont été reconstruits ;
2. le chemin RXIQ a été retracé jusqu’à son appelant `set_rx_gain_testchip_50()`.

Le travail restant consiste à :

1. déterminer l’effet physique de `8` par rapport à `64` ;
2. rechercher d’autres valeurs dans la ROM et les chemins de test ;
3. déterminer si le champ représente un incrément NCO pur ou un bitfield combinant step/mode/phase.

Corroboration externe utilisée uniquement comme comparaison architecturale :

```text
https://github.com/espressif/esp-rom-elfs
https://github.com/espressif/arduino-esp32/blob/master/tools/sdk/esp32/ld/esp32.rom.ld
https://docs.espressif.com/projects/esp8266-rtos-sdk/en/latest/api-guides/factory-test.html
```

---


# 36X. Décodage des valeurs réelles de `tone_control` dans le PHY v6

Cette passe reconstruit instruction par instruction les registres d’arguments autour des appels
au générateur de tone dans `phy_chip_v6.o` et `phy_chip_v6_cal.o`. Elle remplace l’ancienne
hypothèse de v0.10 selon laquelle le paramètre pourrait surtout se distinguer par son signe.

## 36X.1 `ram_rfcal_pwrctrl()` : le premier argument est bien `control_1`

Au début de `ram_rfcal_pwrctrl()`, l’argument d’entrée `a2` est sauvegardé dans la pile.
Juste avant l’appel indirect à `g_phyFuns + 0x68 = rom_start_tx_tone`, le code reconstruit :

```text
a2 = 1                    ; enable_1
a3 = argument_a2_sauvé   ; control_1
a4 = coefficient/scale   ; coeff_1
a5 = 0                    ; enable_2
a6 = 0
a7 = 0
stack = 0,0,0             ; slot 3
```

Le premier argument de `ram_rfcal_pwrctrl()` est donc **directement** le `tone_control`
du slot 1.

Les deux appelants v6 démontrés chargent exactement :

```text
tx_cap_init()          → a2 = 64 → ram_rfcal_pwrctrl()
tx_pwctrl_init_cal()   → a2 = 64 → ram_rfcal_pwrctrl()
```

Conclusion certaine :

> dans ces deux contextes v6, `tone_control = 64`.

## 36X.2 `ram_rfcal_txcap()` : même valeur `64`

`tx_cap_init()` appelle aussi `ram_rfcal_txcap()`. Juste avant cet appel, le code charge :

```text
a2 = 64
```

Dans `ram_rfcal_txcap()`, cet argument est conservé puis transféré dans `a3` avant
`rom_start_tx_tone()`, c’est-à-dire dans la position `control_1`.

Donc :

```text
tx_cap_init() → ram_rfcal_txcap() → start_tx_tone(control_1 = 64)
```

## 36X.3 `meas_tone_pwr_db()` : `64` explicite, amplitude séparée

La routine `meas_tone_pwr_db()` contient un appel direct particulièrement clair :

```text
a2 = 1
a3 = 64
a4 = argument_entrée & 0xFF
...
call start_tx_tone
```

Ici :

```text
control_1 = 64
coeff_1   = argument de mesure (8 bits)
```

Cette séquence apporte une preuve indépendante supplémentaire que `control_1` n’est pas
le coefficient d’amplitude : le contrôle reste fixé à `64` pendant que l’amplitude est
transportée séparément dans `coeff_1`.

## 36X.4 Chemin RXIQ : `tone_control = 8`

Le seul appel direct trouvé à `set_rx_gain_cal_iq()` dans `set_rx_gain_testchip_50()`
prépare les arguments suivants :

```text
a2 = 0
a3 = 8
a4 = ...
a5 = ...
a6 = ...
a7 = ...
call set_rx_gain_cal_iq
```

À l’entrée de `set_rx_gain_cal_iq()`, `a3` est sauvegardé puis réutilisé comme :

```text
control_1 = valeur sauvegardée de a3
```

lors de son appel direct à `rom_start_tx_tone()`. La même valeur est ensuite passée comme
second argument à `ram_rfcal_rxiq()`.

Le décodage de `ram_rfcal_rxiq()` confirme la chaîne :

```text
argument a3
   ↓
sauvegarde locale
   ↓
a3 = valeur sauvegardée
   ↓
rom_start_tx_tone(control_1 = a3)
```

Donc, pour ce chemin :

```text
set_rx_gain_testchip_50()
        ↓ a3 = 8
set_rx_gain_cal_iq()
        ↓
ram_rfcal_rxiq()
        ↓
start_tx_tone(control_1 = 8)
```

Conclusion certaine :

> le chemin RXIQ v6 utilise `tone_control = 8`.

## 36X.5 Ensemble de valeurs démontrées et conséquence

Les usages v6 décodés donnent maintenant :

| Contexte | `tone_control` | Coefficient d’amplitude |
|---|---:|---|
| `meas_tone_pwr_db()` | `64` (`0x040`) | séparé, variable |
| `ram_rfcal_pwrctrl()` depuis `tx_cap_init()` | `64` (`0x040`) | séparé |
| `ram_rfcal_pwrctrl()` depuis `tx_pwctrl_init_cal()` | `64` (`0x040`) | séparé |
| `ram_rfcal_txcap()` | `64` (`0x040`) | séparé |
| `set_rx_gain_cal_iq()` / `ram_rfcal_rxiq()` | `8` (`0x008`) | séparé |

Ainsi, le champ bas n’est pas une constante globale et n’est pas un simple gain. Il choisit
au minimum **deux configurations de stimulus distinctes** selon la calibration.

L’hypothèse actuellement la plus forte devient :

```text
tone_control ≈ step / index du générateur numérique (NCO ou mécanisme équivalent)
```

La différence par facteur 8 entre `8` et `64` est compatible avec des pas de tone différents,
mais elle ne suffit pas à déterminer une fréquence exacte ni à prouver un accumulateur de phase
10 bits. Aucune valeur négative de `tone_control` n’a été observée dans les chemins v6 décodés
pendant cette passe.

Confiance actuelle :

- indépendance par rapport à l’amplitude : **très élevée / démontrée** ;
- existence d’au moins deux valeurs de stimulus (`8`, `64`) : **certaine** ;
- interprétation step/index NCO : **élevée mais encore inférentielle** ;
- caractère signé : **non démontré et actuellement non supporté par les usages v6 observés**.

---


# 36Y. `IQ_EST` dans le PHY v6 : tous les appels directs observés utilisent `M = 1`

Un balayage de toutes les sections `.irom0.text` des objets `libphy.a` fournis a recherché les
appels indirects à :

```text
g_phyFuns + 0x34 = rom_iq_est_enable
```

Deux appels directs ont été identifiés dans le code v6 standard :

```text
set_rx_gain_cal_iq():
    iq_est_enable(1, 1024)

ram_rxiq_cover_mg_mp():
    iq_est_enable(1, N_variable)
```

Le chemin ROM correspondant `rom_rxiq_cover_mg_mp()` observé dans le dump fourni utilise lui
aussi `a2 = 1` avant l’appel à `g_phyFuns + 0x34`.

Conclusion :

> dans les chemins RXIQ v6 et ROM inspectés, le mode `M=1` est le mode effectivement utilisé.

Aucun appel direct `iq_est_enable(0, N)` n’a été retrouvé dans les objets `libphy.a` de cette
version pendant cette passe. Cela **ne démontre pas** que `M=0` est inutilisable ou absent du
silicium ; cela indique seulement qu’il n’est pas employé par les chemins v6 standard décodés.
Le sens physique de `M` reste ouvert.

---

# 36Z. Découverte majeure — TXIQ programme directement le tone slot 1 par MMIO

La fonction `txiq_get_mis_pwr()` (`phy_chip_v6_cal.o`, offset `0x814`) contient des accès
directs calculés depuis la base :

```text
0x60000200
```

avec l’offset :

```text
0x3B8
```

soit exactement :

```text
0x60000200 + 0x3B8 = 0x600005B8
```

Il s’agit donc d’un accès direct au **tone slot 1**. Cette découverte explique pourquoi le chemin
TXIQ ne contient pas nécessairement d’appel à `rom_start_tx_tone()`.

## 36Z.1 Première écriture : reconstruction quasi exacte

Les constantes locales de `txiq_get_mis_pwr()` sont :

```text
0xF0000000
0x002C0000
0x60000200
```

Pour les arguments conceptuels :

```text
sel     = a2       ; 0 ou 1 dans txiq_cover()
coeff   = a3
control = a4
```

le code reconstruit approximativement :

```c
r = REG32(0x600005B8);

r &= 0xF0000000;
r |= ((uint32_t)sel << 26);
r |= ((uint32_t)((-coeff) & 0xFF) << 10);
r |= control;
r |= 0x002C0000;

REG32(0x600005B8) = r;
```

La constante :

```text
0x002C0000
```

force les bits :

```text
21, 19, 18
```

Donc **le bit 18 du tone est explicitement activé par cette écriture MMIO directe**.
Le coefficient utilise exactement la convention déjà reconstruite :

```text
bits 17:10 = (-coeff) & 0xFF
```

Après cette écriture, la routine appelle :

```text
rom_txtone_linear_pwr(4, 10)
```

pour mesurer le résultat.

## 36Z.2 Deuxième écriture : changement de mode sans toucher au coefficient

La routine relit ensuite le même slot, applique :

```text
r &= 0xF0FFFFFF
```

ce qui efface uniquement :

```text
bits 27:24
```

puis construit un nouveau nibble de mode à partir de `sel`. Pour les deux valeurs réellement
utilisées par `txiq_cover()` :

```text
sel = 1  → bits27:24 = 0x8
sel = 0  → bits27:24 = 0x1
```

Le bit18 et le coefficient restent donc intacts pendant cette seconde modification. Une deuxième
mesure `rom_txtone_linear_pwr(4,10)` est ensuite effectuée.

## 36Z.3 `txiq_cover()` réécrit réellement le coefficient à chaud

`txiq_cover()` appelle `txiq_get_mis_pwr()` deux fois de suite, sans `stop_tx_tone()` entre les
deux appels :

```text
appel 1 : sel = 1, coeff = (valeur - 12)
appel 2 : sel = 0, coeff = valeur
```

Chaque appel réécrit `0x600005B8`, y compris `bits17:10`. La chaîne TXIQ environnante active
l’horloge TX avant ce travail et n’appelle `rom_stop_tx_tone(1)` qu’après le retour de
`txiq_cover()`.

La séquence architecturale démontrée est donc :

```text
rom_set_txclk_en(1)
        ↓
txiq_cover()
        ↓
txiq_get_mis_pwr()
        ├── MMIO slot1 : enable + coeff A + mode
        ├── mesure puissance
        ├── MMIO slot1 : changement bits27:24
        ├── mesure puissance
        ↓
txiq_get_mis_pwr()
        ├── MMIO slot1 : enable + coeff B + mode
        └── ...
        ↓
rom_stop_tx_tone(1)
```

## 36Z.4 Correction d’une conclusion antérieure

La section 36F avait correctement corrigé une erreur : les appels `g_phyFuns+0x104` de TXIQ
ne sont pas `start_tx_tone()`. À ce moment, il était donc juste de retirer la prétendue preuve
de mise à jour à chaud **via `start_tx_tone()`**.

La présente analyse apporte cependant un autre mécanisme :

> TXIQ démontre bien une reprogrammation à chaud du générateur, mais **par MMIO direct sur
> `0x600005B8`**, et non par un nouvel appel à `rom_start_tx_tone()`.

Cela ferme une question importante du projet :

```text
coefficient bits17:10 modifiable pendant tone actif ?
→ OUI, démontré dans TXIQ
```

La question encore ouverte est :

```text
la phase/NCO est-elle préservée pendant cette réécriture ?
```

## 36Z.5 Conséquence pour l’architecture OOK

Cette découverte ne prouve pas encore l’extinction RF par un simple clear du bit18, mais elle
renforce fortement le modèle architectural recherché :

- le PHY officiel utilise des writes MMIO directs sur le tone slot ;
- l’horloge TX peut rester active pendant plusieurs reconfigurations du slot ;
- le coefficient peut changer sans passage par `stop_tx_tone()` ;
- le bit18 est un champ explicitement forcé par la programmation directe du slot.

Ainsi, l’idée d’un gate/modulateur construit autour du registre du slot est beaucoup mieux
étayée qu’avant cette passe. La validation RF réelle du niveau OFF, du timing et de la phase
reste nécessaire avant de considérer le comportement OOK comme démontré physiquement.

---


# 36AA. TXIQ : décodage exact du nibble de test `bits 27:24`

La passe précédente avait reconstruit la seconde écriture de `txiq_get_mis_pwr()` sans nommer
l'instruction Xtensa située à l'offset `0x86F`. Son opcode est :

```text
0x834670
```

Le décodage ISA confirme qu'il s'agit de :

```text
moveqz a4, a6, a7
```

avec la sémantique :

```text
if (a7 == 0)
    a4 = a6;
```

Dans cette routine :

```text
a5 = sel << 3
a7 = sel & 1
a6 = 1
a4 = 0
moveqz a4, a6, a7
a4 |= a5
a4 <<= 24
```

La valeur injectée dans `bits27:24` est donc exactement :

```c
mode2 = (sel << 3) | ((sel & 1) == 0 ? 1 : 0);
```

Pour les deux valeurs réellement utilisées :

| `sel` | première écriture TXIQ | seconde écriture TXIQ |
|---:|---:|---:|
| `0` | nibble `0x0` | nibble `0x1` |
| `1` | nibble `0x4` (bit26) | nibble `0x8` (bit27) |

La première écriture provient directement de :

```text
sel << 26
```

La routine parcourt donc quatre états matériels observés :

```text
0x0, 0x1, 0x4, 0x8
```

Leur sens physique exact n'est pas encore démontré. Comme ils sont utilisés exclusivement dans
la mesure/correction TXIQ décodée, le nom neutre recommandé devient :

```text
TXIQ test-state nibble (bits27:24)
```

Hypothèses encore ouvertes : sélection I/Q, phase/quadrature, polarité ou vecteurs de test
orthogonaux. Aucune de ces interprétations n'est encore promue au rang de fait.

## 36AA.1 Carte affinée du slot 1 en mode TXIQ

La première programmation directe de `0x600005B8` par `txiq_get_mis_pwr()` peut maintenant être
écrite :

```text
bits31:28 : anciens bits préservés
bits27:24 : état TXIQ initial (`0x0` ou `0x4`)
bits23:22 : 0 dans ce chemin
bit21     : forcé à 1
bit20     : 0 dans ce chemin
bit19     : forcé à 1
bit18     : tone enable = 1
bits17:10 : (-coeff) & 0xFF
bits9:0   : tone_control
```

Les bits `21` et `19` viennent de la constante :

```text
0x002C0000 = bit21 | bit19 | bit18
```

Comme `bit18` est déjà identifié comme gate du tone, `bit21` et `bit19` sont désormais classés
comme **flags supplémentaires du mode TXIQ direct**. Leur fonction physique précise reste ouverte.

La seconde écriture conserve `bit21`, `bit19`, `bit18`, le coefficient et `tone_control`, et ne
remplace que `bits27:24`.

---

# 36AB. Corroboration mask-ROM : le même chemin MMIO TXIQ existe dans le silicium

Le dump ROM fourni contient :

```text
rom_rfcal_txiq_cover @ 0x400088B8
```

Le désassemblage de cette fonction montre un chemin matériel presque identique à celui du patch
`phy_chip_v6_cal.o`. La fonction utilise la base :

```text
0x60000200
```

et accède à :

```text
base + 0x3B8 = 0x600005B8
```

Elle emploie exactement les mêmes constantes :

```text
0xF0000000   ; préserver le nibble supérieur avant reconstruction
0x002C0000   ; forcer bits21,19,18
0xF0FFFFFF   ; préserver tout sauf bits27:24 lors du second état
```

La mask-ROM exécute également le même couple logique :

```text
sel << 26
```

pour le premier état, puis :

```text
(sel << 3) | (sel == 0 ? 1 : 0)
```

placé dans `bits27:24` pour le second état.

La séquence observée dans la ROM est donc la même :

```text
sel=0 : 0x0 -> 0x1
sel=1 : 0x4 -> 0x8
```

La ROM passe d'abord la valeur d'amplitude par :

```text
rom_set_ana_inf_tx_scale()
```

puis écrit directement le coefficient numérique résultant dans `bits17:10` du slot 1.

Conclusion forte :

> la programmation directe du tone slot par TXIQ, le nibble d'état `27:24`, les flags `21/19`
> et le gate `18` ne sont pas une invention du patch PHY v6 : le même mécanisme existe déjà
> dans la mask-ROM du composant analysé.

Cela augmente fortement la confiance que ces champs appartiennent à l'interface matérielle stable
du bloc tone/TXIQ.

---

# 36AC. TXIQ v6 utilise lui aussi `tone_control = 64`

Le traçage de l'appel depuis `chip_v6_initialize_bb()` ferme la provenance du champ bas utilisé
par le chemin TXIQ direct. Juste avant l'appel à `ram_rfcal_txiq()`, le code charge explicitement :

```text
a5 = 64
```

Dans `ram_rfcal_txiq()`, cet argument `a5` est conservé puis transmis comme `a3` à
`txiq_cover()`. À l'entrée de `txiq_cover()`, cette valeur est sauvegardée et réutilisée comme
`a4` lors des deux appels à `txiq_get_mis_pwr()`. Or `txiq_get_mis_pwr()` OR directement `a4`
dans le registre `0x600005B8`.

Chaîne démontrée :

```text
chip_v6_initialize_bb()
    a5 = 64
        ↓
ram_rfcal_txiq(..., a5=64, ...)
        ↓
txiq_cover(..., control=64, ...)
        ↓
txiq_get_mis_pwr(..., control=64, ...)
        ↓
0x600005B8 bits9:0 |= 64
```

Conclusion :

> le chemin TXIQ v6 utilise explicitement `tone_control = 64`, comme les chemins
> power-measurement / power-control / TX-cap déjà décodés.

L'ensemble des usages v6 connus devient donc :

```text
RXIQ                      : tone_control = 8
TXIQ                      : tone_control = 64
PWRCTRL / TXCAP / tone pwr: tone_control = 64
```

Cela renforce l'idée que `8` et `64` sélectionnent deux stimuli numériques distincts. Le rapport
exact avec une fréquence, une phase ou un pas NCO reste toutefois non démontré.

## 36AC.1 Conséquence pour le gate bit18

Dans les chemins TXIQ ROM et v6, les flags de test (`bits21`, `19` et `27:24`) peuvent rester
configurés alors que `rom_stop_tx_tone(1)` n'efface que `bit18`. Le prochain appel normal à
`rom_start_tx_tone()` reconstruit les bits bas du registre et élimine ces anciens flags.

Cette séparation renforce encore l'interprétation fonctionnelle :

```text
bit18        = gate ON/OFF du slot
configuration TXIQ = champs séparés qui peuvent rester sticky lorsque le gate est OFF
```

Elle ne remplace toujours pas la validation physique RF nécessaire pour savoir si `bit18=0`
produit une extinction suffisante à l'antenne.

---


# 36AD. TXIQ : les états `4→8` mesurent le gain, `0→1` mesurent la phase

Une nouvelle passe sur `txiq_cover()` permet de donner un sens fonctionnel aux deux paires
jusqu'ici seulement décrites comme « états TXIQ ».

La bibliothèque `phy_chip_v6_cal.o` contient explicitement la chaîne de debug :

```text
txiq_gain=%d, txiq_phase=%d
```

À la fin de `txiq_cover()`, le premier octet de la structure de correction est chargé comme
premier argument numérique de ce message (`txiq_gain`), et le second octet comme deuxième
argument (`txiq_phase`).

Le traçage des deux appels à `txiq_get_mis_pwr()` montre :

```text
1er appel : sel = 1
             ↓
      états 0x4 → 0x8
             ↓
      calcul / stockage byte[0]
             ↓
          txiq_gain

2e appel :  sel = 0
             ↓
      états 0x0 → 0x1
             ↓
      calcul / stockage byte[1]
             ↓
         txiq_phase
```

Conclusion démontrée :

| États `bits27:24` | Fonction de calibration |
|---|---|
| `0x4 → 0x8` | mesure / correction du **gain I/Q TX** |
| `0x0 → 0x1` | mesure / correction de la **phase I/Q TX** |

Cette conclusion est plus précise que la classification antérieure « quatre états de test TXIQ ».

## 36AD.1 Interprétation probable des bits du nibble

La paire gain est particulièrement suggestive :

```text
0x4 = bit26
0x8 = bit27
```

Le code compare la puissance obtenue dans ces deux états afin de produire la correction de gain.
Il est donc **fortement plausible**, mais pas encore démontré au niveau électrique, que `bit26` et
`bit27` sélectionnent deux chemins orthogonaux du stimulus TXIQ (par exemple les deux branches I/Q,
ou deux vecteurs équivalents permettant de mesurer leur déséquilibre de gain).

La paire phase :

```text
0x0 → 0x1
```

ne diffère que par `bit24`. Ce bit est donc directement impliqué dans la mesure de l'erreur de
phase, mais son sens précis (polarité, quadrature, inversion, combinaison I/Q, etc.) reste ouvert.

Niveau de confiance :

```text
4→8 = calibration gain I/Q   : très élevé / démontré par le flot de données + chaîne debug
0→1 = calibration phase I/Q  : très élevé / démontré par le flot de données + chaîne debug
bit26/bit27 = sélection I/Q   : élevé mais inférentiel
bit24 = contrôle de phase     : élevé mais sens électrique exact ouvert
```

## 36AD.2 Conséquence pour le générateur OOK

Ces bits ne sont pas nécessaires au gate OOK lui-même. Ils appartiennent à la configuration de
calibration TXIQ qui entoure le même tone slot.

Pour une primitive OOK minimale, la séparation devient encore plus nette :

```text
bits27:24  → états de stimulus / calibration TXIQ
bits21/19  → flags TXIQ supplémentaires
bit18      → gate du tone slot
bits17:10  → coefficient numérique
bits9:0    → tone_control / step-index probable
```

Cela réduit le risque d'attribuer à `bit18` une fonction de sélection I/Q : la sélection de stimulus
TXIQ est clairement portée par d'autres champs du registre.

## 36AD.3 Vérification de la piste `phy_dig_spur_prot()`

Un scan exhaustif des instructions `S32I` utilisant les offsets `0x3B8/0x3BC` a retrouvé deux
écritures dans `phy_dig_spur_prot()`. Leur base a été revérifiée explicitement :

```text
base = 0x60009600
```

et non :

```text
base = 0x60000200
```

Les adresses correspondantes appartiennent donc au bloc digital-spur (`0x600099B8/0x600099BC`)
et **ne sont pas** les tone slots `0x600005B8/0x600005BC`.

La séparation établie en v0.5/v0.13 entre digital-spur et tone generator reste donc valide.


# 36AE. Recoupement avec le mode officiel `wifiscwout`

La documentation officielle de test RF ESP8266 expose le mode single-carrier via :

```text
wifiscwout <enable> <channel> <power attenuation>
```

Les trois paramètres publics sont donc :

```text
enable
canal Wi-Fi 1..14
atténuation de puissance (pas de 0,25 dB)
```

Aucun paramètre public de fréquence relative, de phase, de step NCO ou d'index de tone n'est
exposé par cette commande.

Source officielle :

```text
https://docs.espressif.com/projects/esp8266-rtos-sdk/en/latest/api-guides/factory-test.html
```

Conséquence :

> le mode single-carrier utilisateur ne permet pas, à lui seul, d'identifier la loi de
> `tone_control`. Le firmware de test choisit vraisemblablement une configuration interne fixe
> pour le générateur et ne laisse à l'utilisateur que le canal RF et l'atténuation.

Cette absence de paramètre ne prouve ni que `tone_control` est une fréquence, ni qu'il ne l'est pas.
Elle indique seulement que la correspondance `8/64 → effet physique` devra être obtenue soit par
reverse-engineering plus profond d'un chemin de test, soit par caractérisation instrumentée.

# 36AF. Découverte majeure — `bits27:18` forment un champ `tone_mode` unifié

La relecture instruction par instruction de `rom_start_tx_tone()` corrige une simplification ancienne : le premier paramètre de chaque slot, jusque-là nommé `enable`, n'est jamais réduit à un seul bit avant son insertion dans le registre. Pour le slot 1 :

```text
r &= 0xF0000000
r |= control_1
r |= ((256 - coeff_1) & 0xFF) << 10
r |= mode_1 << 18
```

Le masque `0xF0000000` réserve exactement `bits27:0` à la reconstruction du slot. Comme `control_1` occupe les bits bas et le coefficient commence au bit10, le premier argument remplit naturellement le champ :

```text
bits27:18 = tone_mode[9:0]
```

Le code ne réalise pas de masque explicite `&0x3FF`; la largeur de 10 bits est donc une reconstruction du layout matériel, confirmée par toutes les valeurs observées et par le chemin TXIQ direct.

## 36AF.1 Le mode normal vaut `0x001`

Les appels ROM/v6 ordinaires à `start_tx_tone()` passent :

```text
mode_1 = 1
```

soit :

```text
tone_mode = 0x001
bit18 = 1
bits27:19 = 0
```

C'est pourquoi le bit18 a été correctement identifié depuis le début comme gate du tone. La correction porte sur le fait qu'il est **le LSB d'un champ plus large**, et non l'unique information portée par le premier argument.

## 36AF.2 La constante TXIQ `0x002C0000` est exactement `0x00B << 18`

Le chemin TXIQ direct force :

```text
0x002C0000
```

et :

```text
0x002C0000 >> 18 = 0x00B
```

Ainsi les anciens « flags bit21/bit19/bit18 » sont plus proprement interprétés comme :

```text
tone_mode = 0x00B
```

avec le bit0 du champ (`bit18`) toujours actif.

## 36AF.3 Les quatre états TXIQ deviennent quatre valeurs de `tone_mode`

Les bits de test `27:24` ne sont pas un registre logique séparé : ils occupent la partie haute du même champ `tone_mode`. Les deux séquences deviennent donc :

```text
phase I/Q : 0x00B → 0x04B
gain  I/Q : 0x10B → 0x20B
```

Détail :

| Test TXIQ | Premier `tone_mode` | Second `tone_mode` | Fonction démontrée |
|---|---:|---:|---|
| `sel=0` | `0x00B` | `0x04B` | mesure/correction phase I/Q |
| `sel=1` | `0x10B` | `0x20B` | mesure/correction gain I/Q |

Cette représentation explique en une seule structure les bits `18`, `19`, `21`, `24`, `26` et `27` observés précédemment.

## 36AF.4 Conséquence pour `rom_stop_tx_tone()` et l'OOK

`rom_stop_tx_tone()` applique uniquement :

```text
REG &= ~0x00040000
```

c'est-à-dire :

```text
tone_mode[0] = 0
```

sans modifier `tone_mode[9:1]`. Le meilleur modèle logiciel devient donc :

```text
tone_mode[9:1] = configuration avancée / stimulus
tone_mode[0]   = gate du générateur
```

Pour un OOK minimal, cela renforce la stratégie de modifier uniquement le bit18 par RMW, afin de préserver tout le reste du mode configuré. La latence RF, l'extinction réelle et la continuité de phase restent des questions expérimentales.

## 36AF.5 Conséquence pour le layout du slot

Le slot 1/2 peut désormais être représenté comme :

```text
31        28 27                  18 17          10 9                    0
┌───────────┬──────────────────────┬──────────────┬──────────────────────┐
│ préservés │ tone_mode [9:0]      │ coeff [7:0]  │ tone_control [9:0]   │
└───────────┴──────────────────────┴──────────────┴──────────────────────┘
```

Le rôle détaillé de `tone_mode[9:1]` n'est pas encore entièrement résolu, mais son existence comme champ unifié est maintenant une conclusion à haute confiance.

## 36AF.6 Les trois slots peuvent être armés dans un même appel ROM

`rom_start_tx_tone()` traite successivement les trois triplets et n'impose aucune exclusivité logicielle : si les trois paramètres de mode sont non nuls, les trois registres `0x600005B8`, `0x600005BC` et `0x600005C4` sont programmés avant le retour de la fonction. Inversement, un slot avec `mode=0` est simplement laissé inchangé.

Conclusion démontrée côté logiciel :

> l'API ROM est conçue pour permettre plusieurs slots armés simultanément.

Ce que le code seul ne démontre pas encore est la combinaison physique de ces slots dans le datapath RF : addition multi-tone, injection dans plusieurs composantes, chemins de test distincts ou autre mécanisme.

---

# 36AG. Audit exhaustif des accès ROM aux tone slots et frontière de l’analyse statique

Une passe exhaustive a été effectuée sur le dump mask-ROM fourni afin de distinguer les vrais accès aux tone slots des simples coïncidences d’offset Xtensa.

## 36AG.1 Une seule constante de base `0x60000200` dans la ROM

La valeur :

```text
0x60000200
```

n’apparaît qu’une seule fois comme littéral 32 bits dans le dump ROM, à l’offset :

```text
0x0FC4
```

Ce littéral partagé est chargé par de nombreux `L32R` dans les routines PHY. L’audit a recensé **48 références** à ce littéral.

## 36AG.2 Audit des offsets des trois slots

Les offsets relatifs à cette base sont :

```text
slot 1 : 0x3B8 → 0x600005B8
slot 2 : 0x3BC → 0x600005BC
slot 3 : 0x3C4 → 0x600005C4
```

Toutes les instructions ROM décodables comme `L32I/S32I` sur ces trois offsets ont été recensées, puis leur registre de base a été remonté jusqu’au chargement du littéral `0x60000200`.

Trois régions seulement satisfont réellement cette condition :

```text
0x68DA ... 0x6971  → rom_start_tx_tone()
0x6994 ... 0x6A14  → rom_stop_tx_tone()
0x88EA ... 0x898A  → chemin TXIQ direct / rom_rfcal_txiq_cover()
```

Des correspondances brutes supplémentaires existent autour de :

```text
0x1419
0x7F65
0xAF3F
```

mais leur registre de base n’est pas issu du littéral `0x60000200` dans le même chemin ; ce sont donc des **faux positifs d’offset**, pas des accès aux tone slots.

### Conclusion

> Dans la mask-ROM fournie, aucun quatrième utilisateur caché des registres `0x600005B8/BC/C4` n’a été retrouvé au-delà de `start_tx_tone`, `stop_tx_tone` et du chemin TXIQ direct.

Cette conclusion augmente la confiance dans l’exhaustivité de la carte logicielle actuelle du générateur.

## 36AG.3 Conséquence pour `tone_control`

Dans tous les chemins démontrés, `tone_control` est :

1. fourni par l’appelant ;
2. éventuellement chargé comme constante (`8` ou `64`) ;
3. injecté directement dans les bits bas du slot ;
4. jamais converti par une formule logicielle en fréquence, phase ou période.

Aucun code ROM ne contient donc une relation du type :

```text
frequency = f(tone_control)
```

ou :

```text
phase_step = scale * tone_control
```

La correspondance physique est très probablement réalisée **à l’intérieur du bloc numérique matériel**.

La présence, sur des PHY Espressif ultérieurs, d’une primitive nommée séparément `rom_start_tx_tone_step` et d’un contrôle de gain numérique distinct renforce l’interprétation « step/index » de `tone_control`, mais ne fournit toujours pas l’équation exacte pour l’ESP8266.

Statut recommandé :

```text
tone_control = paramètre brut de stimulus / step-index probable
confiance sur “step/index” : élevée par convergence architecturale
équation valeur → fréquence : ouverte
```

## 36AG.4 Les accès au tone sont explicitement sérialisés par `MEMW`

Le désassemblage de la ROM montre un autre détail important pour le timing du futur OOK : les accès au bloc tone sont entourés de barrières Xtensa `MEMW`.

Dans `rom_start_tx_tone()` :

```text
MEMW
L32I  slot
...
MEMW
S32I  slot
```

Cette paire apparaît pour chacun des slots programmés.

Dans `rom_stop_tx_tone()` :

```text
MEMW
L32I  slot
AND   ~bit18
MEMW
S32I  slot
```

Et le chemin TXIQ direct autour de `0x600005B8` suit la même convention avant ses lectures et écritures.

Cela démontre que le code Espressif traite ces registres comme des MMIO nécessitant une **sérialisation mémoire explicite**. Le write utile reste un `S32I`, mais la barrière `MEMW` fait partie du chemin logiciel observé.

Conséquences :

- le modèle de latence CPU ne doit pas être réduit à « un store = un cycle » ;
- les mesures de débit/jitter OOK devront inclure la barrière mémoire et la latence du bus périphérique ;
- la présence de `MEMW` explique aussi pourquoi une caractérisation instrumentée est nécessaire pour obtenir une latence bit18→RF fiable.

Cette observation est cohérente avec le comportement du compilateur Xtensa, qui sérialise par défaut les accès `volatile`, mais ici elle est surtout **démontrée directement par la mask-ROM fournie**.

## 36AG.5 Ce que la statique ne peut plus résoudre seule

La recherche statique a maintenant atteint une frontière nette pour trois questions :

```text
1. tone_control=8/64 → fréquence physique exacte
2. tone_mode[0]=0 → NCO arrêté ou sortie seulement masquée ?
3. latence/jitter/transitoires → comportement analogique/RF réel
```

Aucun registre de phase explicite ni primitive de lecture de phase du générateur n’a été identifié dans les chemins ROM disponibles. De même, aucun chemin logiciel n’effectue un OFF temporaire par `tone_mode[0]` tout en lisant ensuite un état de phase permettant de déduire si l’accumulateur a continué.

La prochaine progression significative pour l’OOK doit donc combiner :

- les dernières comparaisons inter-générations utiles ;
- puis une **validation instrumentée en environnement RF contrôlé** pour l’extinction, la fréquence de tone, la latence et la continuité de phase.

---

# 36AH. Instrumentation interne du tone : `IQ_EST` et détecteur SAR TX

Cette passe vise à déterminer si l’ESP8266 possède déjà, dans son PHY, des primitives permettant de caractériser le tone sans dépendre immédiatement d’un instrument RF externe. Deux chemins indépendants sont maintenant démontrés.

## 36AH.1 `M=1` de `IQ_EST` est le mode utilisé pour la mesure RXIQ corrélation/puissance

Dans `set_rx_gain_cal_iq()`, le chemin observé est :

```text
start_tx_tone(...)
        ↓
iq_est_enable(1, 1024)
        ↓
lecture 0x600005E4
        ↓
iq_est_disable()
```

`0x600005E4` est le registre déjà identifié comme accumulateur de puissance/énergie totale. Le code décale ensuite cette mesure avant de l’utiliser dans la logique de réglage de gain RX.

Dans `ram_rxiq_cover_mg_mp()` et son équivalent mask-ROM `rom_rxiq_cover_mg_mp()`, la séquence est également :

```text
iq_est_enable(1, N)
        ↓
rxiq_get_mis(...)
        ↓
iq_est_disable()
```

Or `ram_rxiq_get_mis()` lit directement :

```text
0x60000580
0x60000584
0x60000588
0x6000058C
0x600005E4
```

et combine les quatre premiers registres comme composantes de corrélation avant de calculer les corrections de mismatch I/Q.

Conclusion démontrée :

> `IQ_EST` avec **`M=1`** est le mode réellement utilisé par les calibrations RXIQ pour produire les mesures de corrélation et de puissance nécessaires à l’estimation du mismatch I/Q.

Cela remplace l’ancienne description trop vague « mode M inconnu ». Le **sens différentiel exact de `M=0`** reste cependant ouvert.

## 36AH.2 `rom_dc_iq_est()` confirme que le paramètre `M` est transmis directement à l’estimateur

Le désassemblage de :

```text
rom_dc_iq_est @ 0x4000615C
```

montre que la fonction transmet ses deux premiers paramètres directement à :

```text
iq_est_enable(mode, N)
```

puis lit :

```text
0x600005DC   accumulateur DC I
0x600005E0   accumulateur DC Q
```

avec une normalisation par `N + 1`, avant d’appeler `iq_est_disable()`.

Conséquence :

> le mode `M` n’est pas équivalent à « choisir quel registre résultat lire ». Le même bloc d’estimation expose plusieurs familles de résultats (DC, corrélation, puissance), et le consommateur logiciel choisit ensuite celles qui l’intéressent.

Aucun appel concret `iq_est_enable(0, N)` n’a encore été identifié dans les chemins `libphy.a` et calibrations ROM inspectés.

## 36AH.3 Deuxième instrument interne : `rom_txtone_linear_pwr()`

Le désassemblage de :

```text
rom_txtone_linear_pwr @ 0x40006A1C
```

montre une boucle qui appelle répétitivement l’entrée :

```text
g_phyFuns + 0x24
= get_fm_sar_dout / ram_get_fm_sar_dout
```

avec deux buffers 16 bits, transforme les valeurs retournées puis accumule le résultat sur le nombre d’échantillons demandé.

Le chemin `ram_get_fm_sar_dout()` pilote lui-même le bloc SAR/feedback matériel avant de restituer les mesures.

Le chemin v6 `ram_get_fm_sar_dout()` affine encore cette carte. Sa literal pool fournit :

```text
base SAR = 0x60000A00
```

et la routine contrôle notamment :

```text
0x60000A00 + 0x350 = 0x60000D50
```

avant d’appeler `ets_delay_us(25)`, d’attendre la fin d’acquisition, puis d’appeler `read_sar_dout()`. Cette dernière collecte huit valeurs depuis une série de registres commençant autour de :

```text
0x60000A00 + 0x380 = 0x60000D80
```

`rom_get_power_db()` appelle directement `get_fm_sar_dout()`, convertit séparément les deux sorties avec `rom_linear_to_db(..., 3)`, puis forme une différence corrigée par son argument. Dans le PHY v6, `meas_tone_pwr_db()` démarre un tone (`tone_control=64`), appelle `get_power_db(312)` à plusieurs reprises, puis arrête le tone.

Le désassemblage complet de `rom_txtone_linear_pwr(n, qshift)` permet maintenant d'aller plus loin. Pour chaque acquisition, la fonction :

```text
get_fm_sar_dout(&sar_a, &sar_b)
        ↓
den = (sar_b != 0) ? sar_b : 1
        ↓
term = (sar_a << qshift) / den
        ↓
accumulate term dans un accumulateur signé 16 bits
```

Il n'y a **pas de division finale par `n`** dans `rom_txtone_linear_pwr()` elle-même.

Dans TXIQ, les versions RAM et mask-ROM appellent explicitement :

```text
rom_txtone_linear_pwr(4, 10)
```

avant puis après la modification directe du mode du slot. Le résultat est donc un **rapport de détecteur en échelle Q10**, cumulé sur quatre acquisitions.

Le chemin `rom_get_power_db(k)` est également reconstruit :

```text
get_fm_sar_dout(&sar_a, &sar_b)
        ↓
db_a = rom_linear_to_db(sar_a, 3)
db_b = rom_linear_to_db(sar_b, 3)
        ↓
return int16(k + db_a - db_b)
```

Enfin, `meas_tone_pwr_db()` :

```text
start_tx_tone(mode=1, tone_control=64, scale=x)
        ↓
get_power_db(312)
get_power_db(312)
        ↓
stop_tx_tone(1)
        ↓
arrondi signé puis division par 4
```

La routine `rom_linear_to_db()` ajoute **48 unités par octave** (facteur ×2) via son terme d'exposant. Cela est fortement compatible avec une échelle interne d'environ **1/8 dB**, et la division finale par 4 de `meas_tone_pwr_db()` est donc fortement compatible avec une sortie en **quart de dB**. Cette unité est classée comme **inférence forte**, pas comme fait directement nommé par le binaire.

Conclusion :

> la chaîne `get_fm_sar_dout → get_power_db/txtone_linear_pwr` constitue une **voie de mesure TX/SAR matériellement distincte**. `txtone_linear_pwr()` n'agrège pas des ADC bruts : il mesure un rapport normalisé entre deux voies SAR, tandis que `get_power_db()` forme leur différence logarithmique. Cette famille de mesures est distincte du loopback RX + `IQ_EST`.

Cette séparation est importante :

```text
voie A : tone → TX/RF → loopback RX → IQ_EST → corrélation / énergie
voie B : tone → chaîne TX → détecteur feedback/SAR → txtone_linear_pwr
```

## 36AH.4 Conséquence pour la validation OOK

L’ESP8266 possède donc au moins deux mécanismes internes susceptibles de comparer des états du générateur. Cela ouvre une stratégie de validation **interne au silicium** pour certaines propriétés d’amplitude :

```text
tone configuré
   ↓
mesure ON
   ↓
gate tone_mode[0] modifié
   ↓
mesure OFF
```

La voie SAR possède toutefois une limite temporelle maintenant quantifiée : `ram_get_fm_sar_dout()` contient un `ets_delay_us(25)` par acquisition. Ainsi, `txtone_linear_pwr(4,10)` comporte **au minimum 100 µs de délai explicite**, et les deux acquisitions de `meas_tone_pwr_db()` au minimum **50 µs**, sans compter le contrôle SAR et le code de calcul. Cette instrumentation est donc adaptée à la mesure **steady-state** du contraste ON/OFF ou de niveaux ASK, pas à la mesure directe des fronts OOK rapides.

Mais cette séquence précise n’est **pas démontrée dans le firmware fourni**. En particulier, il reste à vérifier expérimentalement que :

- l’estimateur reste exploitable avec le tone gaté OFF mais les clocks TX maintenues ;
- le détecteur SAR possède assez de dynamique pour quantifier le résiduel OFF ;
- la mesure interne reflète suffisamment le niveau RF utile et pas seulement un nœud interne avant certains étages ;
- aucun état de calibration n’est perturbé par une utilisation hors du chemin prévu.

La mesure externe reste donc nécessaire pour caractériser l’extinction à l’antenne, les transitoires et la continuité de phase, mais l’instrumentation interne peut potentiellement réduire fortement le nombre d’inconnues avant cette étape.

---

# 36AI. Raffinement du packing du tone et du scale numérique — v0.18

Cette passe corrige deux simplifications introduites au fil des versions précédentes.

## 36AI.1 `tone_control` n’est pas masqué à 10 bits par la ROM

Dans `rom_start_tx_tone()`, le paramètre `control` est ORé directement dans le mot du slot. Aucun `AND 0x3FF` n’est présent avant l’écriture. Les valeurs réelles observées (`8`, `64`) restent dans les bits bas, ce qui rend le modèle `bits9:0` cohérent, mais la **largeur 10 bits est une inférence de packing**, non une validation effectuée par l’API.

Conséquence : la ROM fait confiance au contrat de ses appelants. Les valeurs hors domaine observé ne doivent pas être interprétées comme automatiquement tronquées par le logiciel.

## 36AI.2 Le `mode_code` est lui aussi décalé sans masque explicite

Le chemin normal fait :

```text
mode_code << 18
```

sans `&1` et sans `&0x3FF`. Le mode normal vaut `1`, donc bit18 seul est positionné. TXIQ écrit directement des états plus riches dans la zone haute.

La conclusion robuste pour l’OOK ne change pas :

```text
stop_tx_tone() → clear bit18 uniquement
```

mais la v0.18 remplace l’affirmation « `bits27:18` = champ hardware `tone_mode[9:0]` certain » par :

> `bits27:18` constituent une zone de mode/test cohérente dans le packing logiciel observé ; ses sous-champs hardware physiques ne sont pas encore totalement séparés.

## 36AI.3 Décodage exact du scale numérique/analogique

Le branchement de `rom_set_ana_inf_tx_scale()` est maintenant identifié comme un test non signé contre `64`. Le comportement exact est :

```text
x < 64:
    digital_scale = x
    analog_scale  = 0

x >= 64:
    digital_scale = 63
    analog_scale  = (63-x) mod 256
```

Le registre analogique interne modifié est :

```text
bloc 0x77 / host 0 / reg 9 / bits 7:0
```

Puis `start_tx_tone()` encode le numérique sous forme négative :

```text
field17_10 = (-digital_scale) mod 256
```

Cette découverte explique pourquoi le champ brut ne doit pas être traité comme une amplitude unsigned simple.

## 36AI.4 `meas_tone_pwr_db()` confirme la séparation contrôle / scale / mesure

Le décodage de `meas_tone_pwr_db()` montre :

```text
mode_code    = 1
tone_control = 64
scale        = octet fourni par l’appelant
        ↓
start_tx_tone(...)
        ↓
get_power_db(312)
get_power_db(312)
        ↓
stop_tx_tone(1)
```

Le PHY peut donc faire varier le paramètre de scale tout en gardant `tone_control=64`, puis mesurer le résultat avec son détecteur interne. C’est une séparation logicielle nette entre **stimulus/step**, **scale**, et **mesure de puissance**.

## 36AI.5 Limite temporelle de la voie SAR interne

`ram_get_fm_sar_dout()` déclenche une acquisition, appelle `ets_delay_us(25)`, puis attend/collecte les résultats SAR. Cette voie est donc adaptée à la comparaison de niveaux quasi-statiques du tone, mais elle ne peut pas être considérée comme un instrument de mesure direct des fronts OOK plus rapides que cette échelle temporelle.

`IQ_EST` reste une seconde voie intégrée pour puissance/corrélation sur fenêtre d’intégration, mais sa propre résolution temporelle dépend de `N` et du datapath matériel.

## 36AI.6 Recoupement avec l’interface RF officielle

La documentation Espressif de test non-signaling décrit `wifiscwout` comme un test single-carrier avec un paramètre d’atténuation en pas de `0,25 dB`. Cela confirme qu’un contrôle de puissance/atténuation du tone existe dans la chaîne officielle, mais **le lien mathématique exact** entre cette valeur publique et `digital_scale`/`analog_scale` n’est pas encore démontré dans le corpus binaire fourni.

---


# 36AJ. Décodage bas niveau de la voie SAR et métrique Q10 — v0.19

Cette passe descend sous `txtone_linear_pwr()` afin de comprendre ce que représentent réellement les deux sorties de `get_fm_sar_dout()`.

## 36AJ.1 `read_sar_dout()` collecte huit résultats puis les corrige

`read_sar_dout()` lit huit résultats matériels successifs dans la région SAR. Chaque valeur est ramenée à 11 bits puis corrigée par une petite compensation issue des bits bas et d'une table/masque interne avant d'être écrite dans un tableau de huit `uint16_t`.

La transformation exacte de chaque sample n'est pas nécessaire pour l'OOK ; le point important est que `ram_get_fm_sar_dout()` ne retourne **pas** directement deux ADC bruts. Il combine les huit résultats corrigés.

## 36AJ.2 Formules exactes des deux sorties de `ram_get_fm_sar_dout()`

Après `read_sar_dout()`, les samples `s1..s7` sont regroupés ainsi :

```text
A = 2 × (s1 + s2 + s3)
B = 3 × (s6 + s7)
C = 3 × (s4 + s5)
```

Les branches unsigned et les opérations `subx2/sub` montrent ensuite :

```text
out0 = max(A - B, 0)
out1 = max(C - B, 0)
```

soit :

```text
out0 = max(2×(s1+s2+s3) - 3×(s6+s7), 0)
out1 = max(3×(s4+s5)    - 3×(s6+s7), 0)
```

La paire retournée par `get_fm_sar_dout()` est donc composée de **deux mesures différentielles positives partageant une même référence/baseline interne** `3×(s6+s7)`.

Le rôle analogique exact des trois groupes de samples reste inconnu ; il serait prématuré de nommer `out0` « signal » et `out1` « référence ». En revanche, leur structure mathématique est maintenant démontrée.

## 36AJ.3 Signification de `txtone_linear_pwr(4,10)`

En combinant 36AJ.2 avec le décodage de `rom_txtone_linear_pwr()` :

```text
metric_Q10 = Σ[i=0..3] ((out0_i << 10) / max(out1_i,1))
```

TXIQ compare donc quatre rapports normalisés de deux voies SAR différentielles, avant et après une modification du stimulus tone.

Conséquence importante :

> le détecteur interne TX possède déjà une normalisation de type rapport qui réduit la dépendance à un niveau absolu commun. Il est donc particulièrement intéressant pour comparer deux états stables du générateur, par exemple gate ON contre gate OFF, à condition que les deux sorties restent dans leur domaine de mesure.

## 36AJ.4 Bornes temporelles de l'instrument interne

Puisque chaque `get_fm_sar_dout()` contient au moins :

```text
ets_delay_us(25)
```

les bornes minimales dues au seul délai explicite sont :

```text
txtone_linear_pwr(4,10)  ≥ 100 µs
meas_tone_pwr_db()        ≥  50 µs
```

auxquelles s'ajoutent la programmation du SAR, les polls d'état, les lectures, divisions et appels.

Cela ferme une question d'architecture :

> la voie SAR interne peut servir à mesurer **l'amplitude steady-state** et le contraste d'un gate OOK, mais elle ne peut pas caractériser directement une transition OOK rapide à l'échelle de quelques microsecondes ou moins.

## 36AJ.5 Conséquence pour la suite OOK

La meilleure utilisation des instruments internes devient donc :

```text
1. maintenir PLL / TX clock / configuration du tone stables
2. mesurer l'état ON avec IQ_EST et/ou SAR
3. ne modifier que le gate bit18
4. laisser l'état se stabiliser
5. mesurer l'état OFF avec les mêmes instruments
6. comparer les rapports/énergies internes
```

Cette stratégie peut établir le **contraste interne** du gate et la réponse relative du `digital_scale`. Elle ne remplace pas la mesure RF externe pour la latence, le jitter, les transitoires, la phase et l'extinction réellement disponible à la sortie RF.

---

# 36AK. Intégration logicielle autonome OOK — concurrence, initialisation et restauration — v0.20

Cette passe cible les deux derniers chantiers purement logiciels du projet OOK : construire une séquence autonome propre et identifier les concurrents du PHY normal.

## 36AK.1 Correction de l'attribution `tx_cont_cfg()`

Les symboles de `phy_chip_v6.o` donnent :

```text
tx_cont_cfg           @ 0x2D70
chip_v6_initialize_bb @ 0x2DCC
periodic_cal          @ 0x2F24
periodic_cal_top      @ 0x303C
phy_wakeup_rf         @ 0x40F0
```

Les relocations `tx_cont_cfg` sont observées à :

```text
0x2EFD
0x2F05
0x42E0
0x42E8
```

Les deux premières sont donc dans `chip_v6_initialize_bb()` et les deux dernières dans `phy_wakeup_rf()`.

Conclusion démontrée :

> `periodic_cal()` n'est pas un appelant de `tx_cont_cfg()` dans le binaire fourni. L'attribution des versions antérieures est corrigée.

## 36AK.2 La pile Wi-Fi peut néanmoins déclencher des calibrations de fond

Les symboles non résolus montrent des références directes à `periodic_cal_top()` depuis :

```text
libpp: pm.o
libpp: pm_for_bcn_only_mode.o
libpp: pp.o
libnet80211: ieee80211_hostap.o
```

Le chemin de transmission normal contient aussi :

```text
ppProcTxDone()
    ↓
tx_pwctrl_background(...)
```

`tx_pwctrl_background()` utilise la chaîne de mesure/contrôle de puissance et peut conduire à `periodic_cal_top()` selon son état interne. En revanche, l'audit de ce chemin n'a pas retrouvé d'écriture directe vers `0x600005B8/BC/C4` ni d'appel à `start_tx_tone()`.

Interprétation :

> le suivi de puissance après un paquet normal peut modifier/calibrer la chaîne RF, mais il ne constitue pas un second écrivain démontré du tone slot.

## 36AK.3 Les écrivains du tone sont principalement des chemins d'initialisation/réinitialisation

Le graphe d'appels reconstruit place les principaux générateurs de tone de calibration dans :

```text
chip_v6_initialize_bb()
    ├── tx_cap_init()
    │    ├── ram_rfcal_pwrctrl()
    │    └── ram_rfcal_txcap()
    ├── ram_rfcal_txiq()
    └── chemins RXIQ via configuration RX
```

Cela réduit fortement le risque d'une réécriture du slot à chaque TX Wi-Fi normal.

Les événements réellement préoccupants pendant une fenêtre OOK sont plutôt :

- réinitialisation BB/RF ;
- `phy_wakeup_rf()` ;
- changement de canal / reconfiguration PLL ;
- calibration périodique qui peut toucher le PLL ou des gains ;
- une transmission MAC normale concurrente sur la même chaîne RF.

## 36AK.4 Les hooks PHY `ram_tx_mac_enable/disable()` sont des no-op

Dans `phy_chip_v6.o` :

```text
ram_tx_mac_enable  @ 0x0000, taille 2
ram_tx_mac_disable @ 0x0004, taille 2
```

Les deux corps contiennent :

```text
0D F0  = RET.N
```

Donc, pour cette version du PHY :

```c
ram_tx_mac_enable()  { return; }
ram_tx_mac_disable() { return; }
```

Conséquence importante :

> les entrées `g_phyFuns +0x74/+0x78` ne fournissent pas de mécanisme réel d'exclusion du MAC. Un générateur OOK qui exige une fenêtre RF exclusive devra coordonner le MAC plus haut, via PP/LMAC/WDEV ou une autre primitive encore à identifier.

`wDev_DisableTransmit()` existe et désarme des bits TX dans WDEV, mais l'audit des symboles de `libpp.a` ne trouve **aucun autre objet qui le référence comme API de pause globale**. Le helper local `lmacDisableTransmit()` est, lui, référencé par le chemin `lmacProcessTxTimeout()` et manipule plusieurs états LMAC/WDEV sous verrou d'interruptions ; il ne constitue pas une primitive publique générale de suspension.

Conclusion actuelle :

> aucun « global TX pause » déjà prêt n'est démontré dans le corpus. `wDev_DisableTransmit()` ne doit donc pas être considéré, sans analyse supplémentaire, comme suffisant pour bloquer toutes les queues avant une session OOK.

## 36AK.5 `start_tx_tone()` n'est pas toute l'initialisation RF

Les calibrations ROM démontrent une séparation nette entre :

```text
préparation RF/PBUS
        ↓
start_tx_tone()
        ↓
mesure/calibration
        ↓
stop_tx_tone()
        ↓
restauration RF/PBUS
```

Par exemple, les chemins `rfcal_pwrctrl` / `rfcal_txcap` partagent un prologue directement visible dans la ROM :

```text
PBUS debug/test
RX RF off
TX XPD on
power detector off/config
set_ana_inf_tx_scale(...)
start_tx_tone(...)
```

Pour `rom_rfcal_pwrctrl()`, le cleanup observé est explicitement :

```text
stop_tx_tone(1)
TX XPD off
RX RF on
PBUS workmode
```

`rom_rfcal_txcap()` partage le prologue mais termine par `stop_tx_tone(1)` puis `pbus_exit_debugmode()`, ce qui démontre qu'il n'existe pas un unique wrapper de restauration identique pour toutes les calibrations.

Fait établi :

> `start_tx_tone()` active la TX clock et programme les slots, mais ne doit pas être assimilé à lui seul à « mettre toute la chaîne RF en émission ».

La séquence PBUS exacte diffère selon la calibration, donc la **séquence minimale autonome** reste une question ouverte plutôt que d'être copiée aveuglément d'une calibration particulière.

## 36AK.6 `phy_wakeup_rf()` est un concurrent plus sérieux qu'une calibration périodique ordinaire

`phy_wakeup_rf()` reprogramme plusieurs états RF/PHY, dont des primitives liées au PLL, au reset PHY et à l'initialisation TX, puis choisit également `tx_cont_cfg(1/0)` selon l'état sauvegardé.

Pour l'OOK :

> une transition sleep→wakeup pendant une session tone doit être considérée comme susceptible d'invalider l'état RF supposé stable, même si elle ne réécrit pas directement `0x600005B8` dans les chemins actuellement identifiés.

Une session OOK autonome devra donc soit empêcher ces transitions pendant sa fenêtre active, soit refaire sa préparation après un réveil.

## 36AK.7 Normaliser le slot avant le premier bit OOK

TXIQ peut programmer des états de test dans le mot du slot 1, tandis que `stop_tx_tone(1)` efface seulement :

```text
bit18
```

Les autres bits restent présents.

Il serait donc incorrect de démarrer une session autonome par :

```text
REG(0x600005B8) |= BIT18
```

sans savoir quel état supérieur a été laissé par la dernière calibration.

La séquence logicielle robuste est plutôt :

```text
1. préparer RF/PLL
2. programmer UNE FOIS un slot1 normal complet
   mode normal + tone_control + digital_scale
3. utiliser ensuite uniquement bit18 comme gate OOK
4. terminer avec bit18=0
5. restaurer RF/clock/état MAC
```

C'est désormais un point logiciel de haute confiance.

## 36AK.8 État du chantier logiciel après v0.20

| Élément logiciel OOK | État |
|---|---:|
| format/programming du slot1 | ~98–99 % |
| gate bit18 | ~99 % |
| scale numérique/analogique | ~99 % |
| instruments internes | ~95–98 % |
| concurrents directs du tone slot | ~95 % cartographiés |
| séquence RF/PBUS minimale autonome | ~75–80 % |
| exclusion MAC/Wi-Fi fiable | ~65–75 % |
| gestion sleep/wakeup/calibration pendant OOK | ~80 % comprise, mécanisme pratique à choisir |
| séquence init→gate→restore globale | ~85–90 % |

Les deux derniers verrous logiciels réellement importants deviennent donc :

1. **obtenir une fenêtre RF/MAC exclusive de manière démontrée** ;
2. **isoler la préparation PBUS/RF minimale et sa restauration exacte**.

Une fois ces deux points résolus, les inconnues majeures restantes seront surtout matérielles : extinction RF, latence, jitter RF et continuité de phase.

---

# 36AL. Clôture du reverse-engineering logiciel OOK/ASK — v0.21

## 36AL.1 Changement de cible : générateur autonome, pas coexistence Wi-Fi

La cible est corrigée par rapport à la v0.20 : **aucun retour au Wi-Fi n'est requis**. Les travaux de verrou MAC réversible et de restauration complète de PP/LMAC/WDEV sont donc retirés du chemin critique. La pile Wi-Fi peut être utilisée au boot uniquement si nécessaire pour obtenir l'initialisation/calibration RF, puis le système reste dans un état RF de test/générateur.

Cette architecture est cohérente avec l'interface de factory-test Espressif, qui sépare `rftest_init` (préparation RF) des commandes de single-carrier `wifiscwout`.

## 36AL.2 Primitive OOK canonique

Après normalisation du slot 1 :

```text
registre    : 0x600005B8
gate        : bit18 = 0x00040000
OFF         : clear bit18
ON          : set bit18
TX clock    : maintenue active
RF / XPD    : maintenus actifs/configurés
MEMW        : conserver la sérialisation MMIO observée dans la ROM
```

`rom_stop_tx_tone()` reste réservé à la sortie globale du mode tone, pas aux symboles OOK, puisqu'il coupe ensuite la TX clock.

## 36AL.3 Primitive ASK/M-ASK canonique

```text
registre        : 0x600005B8
masque          : 0x0003FC00
champ           : bits17:10
digital_scale   : 0..63 (chemin canonique)
encodage        : ((-digital_scale) & 0xff) << 10
```

Le chemin TXIQ démontre la réécriture à chaud de ce champ pendant que le TX reste actif. Pour une M-ASK rapide, les symboles doivent donc modifier uniquement `bits17:10`, en préservant bit18, le contrôle bas et les modes supérieurs.

Le registre analogique `0x77/reg9` peut fixer une plage de puissance différente, mais son écriture I²C interne n'est pas requise à chaque symbole.

## 36AL.4 Ce que le binaire ne peut pas fournir

Le désassemblage ne peut pas transformer les observations suivantes en valeurs RF absolues, car la fonction de transfert correspondante est dans le silicium analogique/numérique :

```text
bit18=0     → combien de dB d'extinction ?
scale=N     → quelle amplitude / puissance RF exacte ?
write MMIO  → combien de ns/us avant le changement RF ?
OFF→ON      → quelle continuité de phase ?
```

Le fait que ces valeurs restent à mesurer **n'indique plus une lacune du reverse-engineering logiciel OOK/ASK**. Elles constituent la phase de caractérisation matérielle.

## 36AL.5 Statut de clôture

| Sous-système OOK/ASK | Statut logiciel/statique |
|---|---:|
| adresse / accès slot1 | **100 %** |
| gate OOK bit18 | **100 %** |
| comportement logiciel `start/stop` | **100 %** |
| encodage ASK bits17:10 | **100 %** |
| hot-update ASK sous TX clock | **100 % démontré** |
| séparation digital/analog scale | **100 %** |
| initialisation normale du mot de slot | **100 % définie** |
| stratégie autonome sans retour Wi-Fi | **100 % définie au niveau architecture** |
| conversion code→dBm | **hors logiciel : mesure RF** |
| extinction / latence / phase | **hors logiciel : mesure RF** |

Conclusion :

> **OOK et ASK sont désormais considérés comme fermés à 100 % côté modèle logiciel de commande dans le corpus fourni.** Les inconnues restantes sont explicitement reclassées en caractérisation matérielle/RF.

---

# 36AM. Réception autonome OOK/ASK — cartographie RX, CCA et détecteur d’énergie — v0.22

Cette passe ouvre le chantier **RX OOK/ASK autonome**. L’objectif n’est pas de faire décoder un paquet 802.11 par le MAC, mais d’exploiter directement la chaîne RF/baseband comme détecteur d’enveloppe/énergie.

## 36AM.1 Gate RX WDEV exact

Le désassemblage de `wDevEnableRx()` / `wDevDisableRx()` identifie un gate RX haut niveau dans WDEV :

```text
REG32(0x3FF20004) bit31
```

Comportement observé :

```text
wDevEnableRx():
    REG32(0x3FF20004) |= 0x80000000

wDevDisableRx():
    REG32(0x3FF20004) &= 0x7FFFFFFF
```

La pile conserve en parallèle un flag logiciel `WdevRxIsClose`, mais le bit31 ci-dessus est le gate matériel directement manipulé.

**Fait démontré :** le RX WDEV peut être activé/désactivé indépendamment du décodage utilisateur de paquets.

## 36AM.2 Contrôle du RX numérique dans `libphy`

Le PHY v6 expose une couche plus basse avec `start_dig_rx()` et `stop_dig_rx()`. Le décodage donne :

```text
start_dig_rx(arg):
    REG32(0x60009B08) &= ~0x08000000   // clear bit27
    REG32(0x60009B60) |= 1
    REG32(0x60009B60) &= ~1            // pulse bit0
    REG32(0x60009A2C)  = arg

stop_dig_rx():
    old = REG32(0x60009A2C)
    REG32(0x60009B08) |= 0x08000000    // set bit27
    REG32(0x60009A2C) &= 0xFFF7FFFF    // clear bit19
    return old / état associé
```

Cela montre que le RX numérique/baseband possède son propre mécanisme start/stop distinct du gate WDEV.

## 36AM.3 Gate CCA et seuil CCA localisés

Les fonctions mask-ROM :

```text
rom_chip_v5_disable_cca @ 0x400060D0
rom_chip_v5_enable_cca  @ 0x400060EC
```

agissent exactement sur :

```text
0x60009B00 bit28
```

avec la convention :

```text
bit28 = 1 → CCA désactivé
bit28 = 0 → CCA activé
```

La routine `set_cca()` travaille de son côté sur `0x60009B64` avec le masque `0xFFF00FFF`, soit une reconstruction des bits `19:12`, et injecte soit `(arg << 12)`, soit la valeur par défaut `0x000B4000`.

Le packing observé est donc :

```text
0x60009B64[19:12] = seuil / contrôle CCA observé
```

La largeur fonctionnelle exacte du champ au niveau silicium reste à confirmer, car l’argument n’est pas explicitement masqué avant insertion.

## 36AM.4 Noise-floor : deux readouts, mais la voie standard est lente

Deux grandeurs de noise-floor sont désormais localisées :

```text
ram_get_noisefloor():
    lit 0x60009B64[31:20]

read_hw_noisefloor():
    lit 0x60009824[11:0]
    puis applique une conversion/sign-extension
```

Le premier partage donc le même mot que le champ CCA `19:12`; le second apparaît comme un readout hardware plus brut.

Cependant le suivi standard dans `libpp` utilise `NoiseTimerInterval = 100` avec un timer armé en millisecondes. Le noise-floor logiciel normal est donc actualisé à l’échelle de **100 ms**, beaucoup trop lent pour échantillonner directement des symboles OOK/ASK.

**Conclusion :** noise-floor = excellente baseline/adaptation lente ; pas la primitive symbole principale.

## 36AM.5 Le RSSI de paquet Wi-Fi est rejeté comme détecteur OOK générique

`ic_get_rssi()` dans `if_hwctrl.o` ne lit pas un détecteur RF libre. Il récupère une structure de trace/rate-control associée au paquet, lit son octet RSSI et applique un offset de `-96`.

Le RSSI exposé par le sniffer appartient également aux métadonnées `RxControl` d’un paquet reçu.

**Conséquence :** pour un signal OOK/ASK arbitraire qui n’est pas un paquet 802.11 valide, la voie paquet/sniffer RSSI ne constitue pas notre démodulateur.

## 36AM.6 `IQ_EST` devient la primitive RX principale

Le bloc déjà décodé fournit :

```text
control              0x6000057C
corrélations         0x60000580 / 584 / 588 / 58C
DC I/Q               0x600005DC / 5E0
énergie / puissance  0x600005E4
```

`rom_iq_est_enable(mode,N)` configure le bloc d’estimation et attend sa fenêtre de mesure ; il ne programme pas lui-même le loopback RF. Le loopback étant configuré séparément par d’autres primitives, la meilleure architecture actuelle est :

```text
antenne
   ↓
RF RX / ADC / baseband
   ↓
IQ_EST, loopback OFF
   ↓
0x600005E4 = énergie de la fenêtre
   ↓
seuil(s) logiciel(s)
```

**Classification : forte inférence, pas encore preuve finale externe.** Le fait que `IQ_EST` appartienne au datapath RX et que le loopback soit un état séparé rend ce chemin très probable, mais aucun appel standard inspecté ne démontre encore explicitement « antenne externe OOK → IQ_EST ».

`rom_get_corr_power()` confirme que le hardware expose simultanément corrélation, DC et puissance à partir de cette famille de registres.

## 36AM.7 Architecture OOK RX proposée

Pour OOK, aucune information de phase n’est nécessaire. Une primitive minimale devient :

```text
E = IQ_EST_POWER(N)

si E >= threshold : symbole = 1
sinon              : symbole = 0
```

Le threshold peut être dérivé d’une baseline de bruit lente, éventuellement alimentée par les registres de noise-floor.

Le prochain paramètre critique à décoder est la durée réelle de la fenêtre `N`, car elle fixe directement :

- le débit maximal ;
- le compromis sensibilité / bruit ;
- la latence de décision.

## 36AM.8 Architecture ASK/M-ASK et problème AGC

ASK ajoute une contrainte absente de l’OOK : un AGC rapide peut ramener différents niveaux reçus vers une amplitude baseband semblable et donc effacer l’information ASK.

Correction v0.23 : les symboles publics `phy_enable_agc()` / `phy_disable_agc()` sont trompeurs. Le traçage de `register_phy_ops()` montre que la table enregistrée commence à `.data+4`; les wrappers chargent respectivement les entrées `+0x10/+0x14` de cette table utile, qui correspondent à `rom_chip_v5_enable_cca()` / `rom_chip_v5_disable_cca()`. Ils commandent donc le **CCA**, pas un AGC de gain RX.

Le contrôle de gain RX exploitable pour ASK/M-ASK passe plutôt par les primitives PBUS (`rom_pbus_set_rxgain`, `pbus_set_rxbbgain`, tables de gain RX).

La stratégie la plus plausible est donc :

```text
1. préambule / acquisition avec AGC actif
2. figer ou désactiver AGC
3. conserver un gain RX fixe
4. mesurer E avec IQ_EST
5. quantifier E avec plusieurs seuils
```

Exemple conceptuel :

```text
E < T1          → ASK niveau 0
T1 <= E < T2    → ASK niveau 1
T2 <= E < T3    → ASK niveau 2
E >= T3         → ASK niveau 3
```

Cette stratégie est une **inférence d’ingénierie**, pas encore une caractérisation RF mesurée.

## 36AM.9 Le CCA reste un candidat de détection OOK très rapide

Le CCA doit nécessairement réagir plus vite que le suivi software du noise-floor pour permettre le fonctionnement CSMA/CA. Le gate et son seuil sont maintenant localisés, mais le corpus analysé n’a pas encore révélé un registre CPU clairement identifié comme **CCA busy instantané**.

Deux scénarios restent donc ouverts :

1. `IQ_EST` fournit la meilleure primitive programmable d’énergie ;
2. un bit/status CCA encore à identifier fournit un slicer binaire OOK beaucoup plus rapide.

Trouver ce readout CCA devient une priorité élevée.

## 36AM.10 Classement actuel des primitives RX

| Primitive | Usage OOK/ASK | Statut |
|---|---|---|
| `IQ_EST` + `0x600005E4` | énergie programmable, OOK et ASK | **candidat principal** |
| CCA | détection binaire potentiellement très rapide | seuil/gate décodés, readout busy encore ouvert |
| noise-floor | baseline/adaptation lente | registres partiellement décodés ; suivi standard 100 ms |
| RSSI packet/sniffer | métadonnée d’un paquet Wi-Fi décodé | **rejeté pour OOK/ASK arbitraire** |
| corrélations IQ_EST | rejet d’interférence / information supplémentaire possible | disponible, exploitation OOK à étudier |

## 36AM.11 État du chantier RX après v0.22

| Élément RX OOK/ASK | Compréhension actuelle |
|---|---:|
| gate RX WDEV | ~100 % |
| start/stop RX numérique | ~95 % |
| gate CCA | ~100 % |
| seuil CCA / packing | ~90 % |
| noise-floor registers | ~85–90 % |
| packet RSSI | ~95 % compris, non adapté |
| `IQ_EST` contrôle/résultats | ~95 % |
| `IQ_EST` sur antenne externe | ~75–85 % — forte inférence à valider |
| architecture OOK RX | ~75–80 % |
| architecture ASK RX | ~60–70 % |
| débit/sensibilité/niveaux réels | à mesurer |

---


# 36AN. RX OOK/ASK — correction CCA, `sense_backoff` et gain RX fixe PBUS — v0.23

Cette passe affine trois points critiques de la réception autonome : la fausse piste des wrappers nommés AGC, la fonction `rom_chip_v5_sense_backoff()`, et la possibilité d'imposer un gain RX fixe pour préserver les niveaux ASK.

## 36AN.1 `phy_enable_agc()` / `phy_disable_agc()` ne pilotent pas l'AGC

Dans `phy.o`, les wrappers chargent un pointeur de table enregistré par `register_phy_ops()` puis appellent :

```text
phy_enable_agc()  → entrée +0x10 de la table utile
phy_disable_agc() → entrée +0x14 de la table utile
```

Le pointeur fourni par `phy_chip_v6.o` vise `.data+4` : le premier mot `.data` est un header (`0x0BF00055`), puis les relocations de la table sont :

```text
+0x00 chip_v6_rf_init
+0x04 chip_v6_set_chanfreq
+0x08 chip_v6_set_chan
+0x0C chip_v6_unset_chanfreq
+0x10 rom_chip_v5_enable_cca
+0x14 rom_chip_v5_disable_cca
+0x18 chip_v6_initialize_bb
+0x1C chip_v6_set_sense
```

Conclusion démontrée :

> malgré leurs noms publics, `phy_enable_agc()` et `phy_disable_agc()` sont ici des wrappers **CCA enable/disable**. Ils ne constituent pas un freeze de gain RX.

Cette correction retire une fausse solution du chantier ASK.

## 36AN.2 `rom_chip_v5_sense_backoff()` programme des seuils, il ne lit pas le CCA

Le corps ROM à `0x4000610C` utilise la base :

```text
0x60009A00
```

et modifie deux registres :

```text
0x60009A00 + 0x228 = 0x60009C28
0x60009A00 + 0x324 = 0x60009D24
```

Pour un argument non négatif `s`, la routine programme :

```text
0x60009C28 bits16:10 ← s
0x60009D24 bits7:1   ← s
```

Les masques observés sont :

```text
0xFFFE03FF  → efface bits16:10
0xFFFFFF01  → efface bits7:1
```

Pour une valeur négative, la routine force respectivement :

```text
bits16:10 = 0x7F
bits7:1   = 0x7F
```

Conclusion :

> `sense_backoff()` est une primitive de **configuration de sensibilité/seuil de sensing/backoff**, pas une primitive de lecture `CCA busy`.

La recherche du readout CCA instantané reste donc ouverte. Le scan de `libpp` et `libnet80211` ne révèle pas de lecture CPU évidente de la base BB `0x60009A00` en dehors de chemins WDEV/sniffer ; cela renforce l'hypothèse que le résultat CCA est principalement consommé directement par le matériel MAC.

## 36AN.3 Gain RX fixe : `pbus_set_rxbbgain()` est décodé

`pbus_set_rxbbgain(n)` transforme l'index `n` en deux programmations PBUS de bank 3.

Le premier étage est grossier :

```text
n =  0..5   → coarse = 0x00
n =  6..11  → coarse = 0x40
n = 12..17  → coarse = 0x60
n = 18..23  → coarse = 0x70
n >= 24     → coarse = 0x78
```

puis :

```text
rom_pbus_force_test(bank=3, selector=1, value=coarse)
```

Le deuxième étage utilise :

```text
r = n mod 6
fine = (r << 3) | 6
rom_pbus_force_test(bank=3, selector=2, value=fine)
```

Le gain BB RX est donc décomposé en **étage grossier + étage fin**, avec une granularité logicielle d'index claire. La relation exacte index→dB n'est pas encore démontrée.

## 36AN.4 `rom_pbus_set_rxgain()` permet un mot de gain RX plus complet

La ROM `rom_pbus_set_rxgain(code)` ne se limite pas à un niveau unique. Elle extrait plusieurs groupes de bits du mot `code` et les répartit sur plusieurs sélecteurs PBUS, tout en préservant certains bits déjà présents.

Cette fonction confirme l'existence d'un **mode de gain RX imposé multi-étages** indépendant de la logique de paquet Wi-Fi.

Pour le futur démodulateur ASK/M-ASK, la stratégie devient donc :

```text
RX RF ON
   ↓
choisir / calibrer un index de gain
   ↓
forcer le gain RX via PBUS
   ↓
IQ_EST(mode=1,N)
   ↓
énergie 0x600005E4
   ↓
quantification ASK
```

Cette voie est plus solide que l'ancien plan « appeler phy_disable_agc() », désormais invalidé.

## 36AN.5 État du CCA comme démodulateur OOK rapide

Le CCA possède maintenant :

- son gate : `0x60009B00 bit28` ;
- un champ de seuil/configuration : `0x60009B64[19:12]` ;
- `sense_backoff()` qui programme deux champs de sensibilité supplémentaires.

En revanche, aucun **readout CPU `busy`** n'est encore démontré.

Conséquence :

> pour la réception OOK actuelle, `IQ_EST + 0x600005E4` reste la primitive de détection principale. Le CCA ne devient un slicer matériel direct que si un status exploitable est retrouvé.

## 36AN.6 Limite actuelle de `N → temps`

`rom_iq_est_enable(mode,N)` place exactement `N[14:0]` dans `0x6000057C bits16:2`, déclenche la mesure puis attend le bit31 `DONE`.

La dépendance de la durée à `N` est donc matériellement certaine, mais **le clock exact de l'accumulateur IQ_EST n'est toujours pas exposé par le logiciel**. Le désassemblage ne fournit aucune conversion `N→µs`.

Ainsi :

- `N` est bien la longueur matérielle de fenêtre ;
- le débit symbolique dépend directement de cette fenêtre ;
- la constante de temps réelle doit être obtenue par mesure ou par identification indépendante du clock du bloc.

---


# 36AO. RX OOK/ASK — DWARF WDEV, `RxControl` exact et nouvelle piste FIQ — v0.24

Cette passe exploite les informations DWARF conservées dans `libpp.a`, en particulier dans `wdev.o`. Contrairement au PHY v6 principal, `wdev.o` n'est pas seulement symbolisé : il conserve les noms de types, membres, variables locales et numéros de lignes de `wdev.c`. Cela permet de séparer plus proprement les métadonnées de paquet du chemin RF/baseband libre.

## 36AO.1 Le `RxControl` exact du corpus NONOS est reconstruit

Le DWARF déclare `RxControl` à la ligne source 360 avec une taille exacte de **12 octets**. Le packing reconstruit est :

```text
word0 (+0x00)
 bits  7:0   rssi            signé 8 bits
 bits 11:8   rate
 bit     12  is_group
 bit     13  réservé
 bits 15:14  sig_mode
 bits 27:16  legacy_length
 bit     28  damatch0
 bit     29  damatch1
 bit     30  bssidmatch0
 bit     31  bssidmatch1

word1 (+0x04)
 bits  6:0   MCS
 bit      7  CWB
 bits 23:8   HT_length
 bit     24  Smoothing
 bit     25  Not_Sounding
 bit     26  réservé
 bit     27  Aggregation
 bits 29:28  STBC
 bit     30  FEC_CODING
 bit     31  SGI

word2 (+0x08)
 bits  7:0   rxend_state
 bits 15:8   ampdu_cnt
 bits 19:16  channel
 bits 23:20  réservés
 bits 31:24  noise_floor      signé 8 bits
```

Le point nouveau important pour le chantier RX est donc démontré directement dans **ce** binaire NONOS :

> le moteur RX produit bien une valeur `noise_floor` signée de 8 bits dans le descripteur `RxControl`.

Cette structure est cohérente avec les structures de métadonnées RX publiées plus tard par Espressif, mais ici la preuve vient directement du DWARF du corpus analysé.

## 36AO.2 Ce `noise_floor` reste une métrique de paquet, pas une enveloppe RF libre

`RxControl` est manipulé dans le chemin `wDev_ProcessRxSucData()` / sniffer. Sa présence ne change donc pas la conclusion de v0.22 : cette valeur apparaît avec le traitement d'une réception reconnue par le moteur MAC/PHY.

Pour un signal OOK/ASK arbitraire sans trame 802.11 valide :

```text
RxControl.rssi        → métadonnée de paquet
RxControl.noise_floor → métadonnée de paquet
```

et non :

```text
RF instantanée → valeur CPU libre à échantillonner à chaque symbole
```

Conséquence : `RxControl.noise_floor` peut servir à caractériser le récepteur ou à recouper le noise-floor, mais il ne remplace pas `IQ_EST` comme primitive de démodulation autonome.

## 36AO.3 `wDev_ProcessFiq()` expose une nouvelle cible : le mot local `event`

Le DWARF de `wDev_ProcessFiq()` donne :

```text
wDev_ProcessFiq() : ligne source 1072

locaux :
  int_enabled       ligne 1073
  event             ligne 1074
  txpmd             ligne 1076
  index             ligne 1085
  rxstart_time      ligne 1112
  txcomplete_state  ligne 1211
  ack_snr           ligne 1212
```

Cette fonction est donc explicitement structurée autour d'un **mot d'événements hardware**. Les constantes MMIO présentes dans son bloc de code montrent les bases :

```text
0x3FF20A00
0x3FF20E00
0x3FF1FE00
```

Ces régions sont celles du moteur MAC/WDEV, distinctes de la base BB `0x60009A00` utilisée par les routines PHY/CCA.

Observation négative utile : dans les constantes/literals directement associées à `wDev_ProcessFiq()`, aucune base `0x60009A00` n'apparaît. Cela renforce l'idée que les décisions CCA sont consommées par le moteur MAC, puis éventuellement reflétées sous forme d'événements/status dans WDEV, plutôt que relues par le CPU via le registre BB de seuil.

**Classification : forte inférence.** Il reste à décoder l'offset exact lu dans `0x3FF20A00` pour former `event`, puis la signification de ses bits.

## 36AO.4 Nouvelle stratégie pour retrouver `CCA busy`

La recherche ne doit plus se limiter à la base BB `0x60009A00`. Deux familles sont maintenant à distinguer :

```text
PHY / CCA configuration
    0x60009A00...
    gate, seuils, noise-floor, sensing

MAC / WDEV event path
    0x3FF20A00...
    0x3FF20E00...
    0x3FF1FE00...
    FIQ, RXSTART/RXEND, TX-complete, événements
```

La prochaine cible prioritaire devient donc :

1. identifier l'offset MAC lu au début de `wDev_ProcessFiq()` pour construire `event`;
2. reconstruire les tests de bits associés aux branches RX ;
3. déterminer si un bit correspond à `CCA busy`, `CCA edge`, `RXSTART` ou uniquement à des événements de paquet ;
4. si un bit CCA existe, évaluer s'il peut être lu/pollé directement sans conserver la pile 802.11.

Cette piste est plus prometteuse que de chercher aveuglément un troisième registre BB de statut.

## 36AO.5 Conséquence pour l'architecture RX autonome

L'architecture principale reste :

```text
ASK / M-ASK : RF RX → gain fixe PBUS → IQ_EST → 0x600005E4 → quantification
OOK         : même voie sûre aujourd'hui
```

Mais une voie rapide potentielle se précise :

```text
RF RX → CCA matériel → moteur MAC/WDEV → bit d'événement/status ? → slicer OOK
```

Le point d'interrogation est désormais localisé beaucoup plus précisément : **le décodage du mot `event` de `wDev_ProcessFiq()` et de ses registres MAC associés**.

---

# 36AP. RX OOK/ASK — lecture CPU de l’état RX resserrée sur `IQ_EST` — v0.25

Cette passe est volontairement limitée à une seule question : **comment le LX106 peut-il lire l’état du signal reçu sans attendre une trame 802.11 valide ?**

## 36AP.1 Le mot `event` de `wDev_ProcessFiq()` est maintenant localisé

Le début de `wDev_ProcessFiq()` permet de rattacher les variables DWARF aux registres WDEV :

```text
base WDEV locale          = 0x3FF20A00
int_enabled               = *(0x3FF20A00 + 0x218) = 0x3FF20C18
event                     = *(0x3FF20A00 + 0x220) = 0x3FF20C20
acquittement/clear event  =  (0x3FF20A00 + 0x224) = 0x3FF20C24
```

`wDev_ProcessFiq()` sauvegarde les événements, masque temporairement les interruptions puis traite les causes latched. Le DWARF de la même fonction expose notamment `rxstart_time`, `txcomplete_state` et `ack_snr`, ce qui confirme que ce mot appartient au moteur d’événements MAC/RX/TX.

**Conclusion :** `0x3FF20C20` est utile pour comprendre les événements RX du MAC, mais ce n’est pas, en l’état des preuves, un registre continu du type « énergie RF au-dessus du seuil CCA maintenant ». Il ne faut donc pas l’utiliser comme primitive OOK principale.

## 36AP.2 `rom_get_corr_power()` prouve le bloc de résultats de puissance

La mask-ROM fournit :

```text
rom_get_corr_power @ 0x40006260
```

Le corps de cette routine charge la base `0x60000200` puis lit exactement :

```text
0x60000580
0x60000584
0x60000588
0x6000058C
0x600005DC
0x600005E0
0x600005E4
```

Les quatre premiers sont les résultats de corrélation, `0x5DC/0x5E0` les accumulateurs DC I/Q, et `0x600005E4` la grandeur de puissance/énergie totale. Le nom ROM et les accès MMIO sont indépendants des métadonnées de paquet WDEV.

Ce point est important : **`0x600005E4` est bien une sortie du bloc de mesure PHY**, et non le RSSI d’un descripteur de paquet.

## 36AP.3 Méthode actuellement la plus solide pour lire l’état RX

Le chemin CPU minimal devient :

```text
RX RF/baseband actif
       ↓
loopback interne OFF
       ↓
rom_iq_est_enable(1, N)
       ↓
attente DONE effectuée par la ROM sur 0x6000057C bit31
       ↓
E = *(volatile uint32_t *)0x600005E4
       ↓
rom_iq_est_disable()
       ↓
E > seuil ? RX=1 : RX=0
```

Prototype pratique pour une validation sur silicium :

```c
typedef void (*iq_est_enable_fn)(uint32_t mode, uint32_t n);
typedef void (*iq_est_disable_fn)(void);

#define ROM_IQ_EST_ENABLE   ((iq_est_enable_fn) 0x40006430)
#define ROM_IQ_EST_DISABLE  ((iq_est_disable_fn)0x40006400)
#define REG32(a)            (*(volatile uint32_t *)(a))
#define IQ_POWER_REG        REG32(0x600005E4)

static inline uint32_t rx_energy_once(uint32_t n)
{
    ROM_IQ_EST_ENABLE(1, n);
    uint32_t e = IQ_POWER_REG;
    ROM_IQ_EST_DISABLE();
    return e;
}
```

Cette version volontairement simple utilise la ROM pour respecter la séquence exacte de programmation et d’attente du bloc. Une boucle MMIO manuelle plus rapide pourra être construite ensuite, une fois le comportement matériel mesuré.

## 36AP.4 `0x600005E4` n’est probablement pas une valeur free-running

La séquence logicielle standard est toujours :

```text
start IQ_EST → attendre DONE → lire les résultats → disable
```

Il faut donc considérer `0x600005E4` comme le **résultat de la dernière fenêtre terminée**, pas comme un RSSI live que l’on peut lire seul en boucle. Pour obtenir un état RX continu, le CPU doit déclencher des fenêtres successives.

Cela reste parfaitement utilisable pour OOK :

```text
E0 = fenêtre 0
E1 = fenêtre 1
E2 = fenêtre 2
...

bit[k] = (Ek >= T)
```

## 36AP.5 Fenêtre minimale : `N=0` devient un test prioritaire

Le désassemblage de `rom_iq_est_enable()` montre que `N` est seulement masqué à 15 bits puis placé dans `0x6000057C[16:2]`; aucune vérification logicielle `N>0` n’est effectuée. En parallèle, `rom_dc_iq_est()` normalise ses accumulateurs par `N+1`.

Forte inférence :

> `N` est très probablement un compteur « nombre d’échantillons moins un », ce qui ferait de `N=0` la fenêtre matérielle minimale d’une accumulation.

Ce point **n’est pas encore validé sur silicium**. La première caractérisation RX doit donc tester `N = 0, 1, 3, 7, 15, 31, 63` et mesurer à la fois la durée et la séparation bruit/porteuse.

## 36AP.6 Ce qui est désormais acquis et ce qui reste ouvert

| Élément | Statut v0.25 |
|---|---|
| lire un événement MAC RX/TX | `0x3FF20C20`, démontré comme mot d’événements latched |
| lire une énergie PHY après mesure | `0x600005E4`, **démontré** |
| déclencher/attendre la mesure | `rom_iq_est_enable(1,N)` / `0x6000057C`, **démontré** |
| construire un état binaire OOK CPU | seuil logiciel sur `0x600005E4`, architecture maintenant concrète |
| prouver antenne externe → `IQ_EST` sans loopback | **validation silicium encore requise** |
| trouver un bit CCA busy directement lisible | **toujours ouvert**, mais n’est plus bloquant pour lire un état RX |
| déterminer `N → temps` | **toujours ouvert** |

La priorité n’est donc plus « trouver n’importe quel moyen de lire RX » : ce moyen existe maintenant côté logiciel avec `IQ_EST`. La priorité devient de **valider que cette mesure voit bien l’antenne externe et de réduire `N` jusqu’à la fenêtre utile la plus rapide**.

---

# 36AQ. RX OOK/ASK — décision : `IQ_EST` ultra-rapide devient la voie principale — v0.26

## 36AQ.1 Verdict

Après audit ciblé du CCA et de `IQ_EST`, la décision d’ingénierie est désormais :

> **La voie principale pour le RX autonome OOK/ASK sera `IQ_EST` piloté directement par MMIO.**
>
> La recherche d’un **bit CCA busy CPU-visible** reste une optimisation secondaire et ne doit plus bloquer le projet.

Ce choix n’est pas fondé sur le fait que le CCA matériel serait lent — au contraire, il doit nécessairement prendre des décisions très rapidement pour le MAC — mais sur l’interface réellement retrouvée dans le corpus ESP8266 :

- `IQ_EST` possède une commande CPU explicite, un bit `DONE` et un résultat de puissance lisible ;
- les routines CCA retrouvées dans la ROM/PHY ne font que **configurer** le CCA ;
- aucun readout CPU continu de type `get_cca_busy()` n’a été retrouvé dans la ROM, `libphy.a`, `libpp.a` ou `libnet80211.a` analysés ;
- le registre d’événements WDEV `0x3FF20C20` est latched/interrupt-driven et n’est pas un niveau RF instantané ;
- `IQ_EST` fournit une grandeur multi-niveaux, donc convient à **OOK et M-ASK**, alors qu’un CCA direct ne fournirait au mieux qu’un slicer binaire.

Cette absence de readout CCA dans le logiciel constitue une **preuve négative forte dans le corpus**, pas une preuve qu’aucun bit caché n’existe physiquement dans le silicium.

## 36AQ.2 Audit CCA : configuration trouvée, sortie CPU non trouvée

La mask-ROM confirme :

```text
rom_chip_v5_disable_cca @ 0x400060D0
    0x60009B00 |= 0x10000000      ; bit28 = 1

rom_chip_v5_enable_cca @ 0x400060EC
    0x60009B00 &= 0xEFFFFFFF      ; bit28 = 0
```

Donc `0x60009B00 bit28` est bien un **gate disable/enable CCA**, et non son résultat.

`set_cca()` dans le PHY v6 agit sur :

```text
0x60009B64[19:12]    seuil / contrôle CCA
0x60009D68 bit18     mise à jour / validation associée
```

`rom_chip_v5_sense_backoff()` programme également deux champs de sensibilité/backoff :

```text
0x60009C28[16:10]
0x60009D24[7:1]
```

Aucune de ces routines ne lit un bit `busy/free` pour le retourner au CPU.

L’audit de la ROM autour de la seule base littérale `0x60009A00` retrouve les accès de CCA/noise-floor/configuration, mais pas de branche logicielle démontrée sur un résultat CCA instantané. Le seul bit de statut BB clairement testé dans les chemins noise-floor inspectés est `0x60009B60 bit1`, associé à la machine de mesure du noise-floor et non au CCA busy.

## 36AQ.3 Registre de contrôle `IQ_EST` : bitfield logiciel reconstruit

La routine ROM `rom_iq_est_enable@0x40006430` permet maintenant de reconstruire le protocole logiciel du registre :

```text
IQ_EST_CTRL = 0x6000057C

bit 0       block enable                [fortement démontré par la séquence ROM]
bit 1       start / request             [fortement démontré]
bits 16:2   N[14:0]                     [démontré par le packing ROM]
bit 18      mode                        [démontré pour mode 0/1]
bit 31      DONE                        [démontré : boucle ROM jusqu’à bit31=1]
```

La séquence exacte de `rom_iq_est_enable(mode,N)` est :

```text
R = IQ_EST_CTRL
R |= 1
IQ_EST_CTRL = R

R = IQ_EST_CTRL
R = (R & 0xFFFA0001)
  | (mode << 18)
  | ((N & 0x7FFF) << 2)
  | 2
IQ_EST_CTRL = R

while ((int32_t)IQ_EST_CTRL >= 0) {
    ; attend DONE bit31
}
```

Il n’existe **aucun délai logiciel fixe** dans cette routine : pas de `ets_delay_us()`, pas de timer OS. La durée d’acquisition est donc imposée par la machine matérielle `IQ_EST`, la valeur `N` et les latences MMIO.

L’échelle temporelle exacte `N → temps` ne peut pas être déduite avec certitude des seuls binaires ; elle doit être mesurée sur silicium.

## 36AQ.4 Deux usages réels confirment le mécanisme

Deux chemins de calibration du PHY fourni utilisent effectivement cette primitive :

### Cas 1 — `set_rx_gain_cal_iq()`

Le code charge :

```text
mode = 1
N    = 1024
```

puis appelle l’entrée `IQ_EST enable`, lit `0x600005E4`, puis appelle l’entrée `IQ_EST disable`.

### Cas 2 — `ram_rxiq_cover_mg_mp()`

Le code construit :

```text
N = 1 << argument
mode = 1
```

et, dans le chemin RXIQ appelé depuis la calibration observée avec `argument=13`, cela correspond à une fenêtre de :

```text
N = 8192
```

La fonction utilise à nouveau le couple `enable → traitement → disable`.

Ces valeurs sont des fenêtres de calibration et **ne constituent pas un minimum hardware**. Elles montrent seulement que le même estimateur est utilisé pour des mesures RX réelles avec plusieurs longueurs de fenêtre.

## 36AQ.5 `N=0` reste le candidat prioritaire pour la fenêtre minimale

Le packing ROM accepte tout `N` sur 15 bits :

```text
(N & 0x7FFF) << 2
```

sans test `N != 0` ni clamp logiciel.

De plus, `rom_dc_iq_est()` normalise les accumulateurs par `N+1`. Cela reste une forte indication que le comptage matériel est probablement exprimé comme une longueur moins un, ou qu’au minimum une fenêtre `N=0` est prévue par le calcul logiciel.

Statut :

> **Forte inférence, à valider sur silicium.**

Le premier balayage de caractérisation doit donc être :

```text
N = 0, 1, 2, 3, 7, 15, 31, 63, ...
```

et mesurer simultanément :

- durée jusqu’à `DONE` ;
- valeur `0x600005E4` bruit seul ;
- valeur avec porteuse ;
- variance de la mesure ;
- capacité à séparer plusieurs niveaux ASK.

## 36AQ.6 Première primitive directe — fidèle à la ROM

Une version directe, sans appel de fonction ROM, peut reproduire le protocole :

```c
#define IQ_EST_CTRL (*(volatile uint32_t *)0x6000057C)
#define IQ_EST_PWR  (*(volatile uint32_t *)0x600005E4)

static inline uint32_t iq_est_once_fast(uint32_t n)
{
    uint32_t r;

    r = IQ_EST_CTRL;
    IQ_EST_CTRL = r | 1U;               // block enable
    __asm__ __volatile__("memw");

    r = IQ_EST_CTRL;
    r = (r & 0xFFFA0001U)
      | (1U << 18)                       // mode = 1
      | ((n & 0x7FFFU) << 2)
      | 2U;                              // start
    IQ_EST_CTRL = r;
    __asm__ __volatile__("memw");

    while ((int32_t)IQ_EST_CTRL >= 0) { // bit31 = DONE
        __asm__ __volatile__("memw");
    }

    __asm__ __volatile__("memw");
    return IQ_EST_PWR;
}
```

Cette primitive évite l’appel indirect et expose directement le handshake matériel. Pour une première validation, il est prudent de conserver la séquence de désactivation ROM entre acquisitions ou de reproduire exactement son reset avant de tenter le mode streaming ci-dessous.

## 36AQ.7 Optimisation suivante : garder `bit0` activé et ne retrigger que la fenêtre

La ROM commence chaque `iq_est_enable()` par :

```text
IQ_EST_CTRL |= 1
```

puis programme immédiatement `start + N + mode`. Cela suggère qu’un mode de mesure répétée pourrait conserver le **block enable bit0** à 1 et ne réécrire que la commande de nouvelle fenêtre.

Architecture visée :

```text
BEGIN:
    bit0 = 1 une fois

LOOP:
    programmer mode/N + start
    attendre DONE
    lire 0x600005E4
    seuillage / quantification
    retrigger

END:
    disable/reset
```

Ce serait le chemin `IQ_EST ultra-rapide` idéal, car l’overhead par symbole deviendrait essentiellement :

```text
1 RMW/start + polling DONE + 1 lecture power + décision
```

Mais **le re-arm de DONE/start sans passage par `rom_iq_est_disable()` n’est pas encore démontré par le corpus**. Il s’agit désormais du test matériel prioritaire.

## 36AQ.8 Pourquoi `IQ_EST` gagne même si un bit CCA caché est découvert plus tard

Pour OOK uniquement, un bit CCA direct pourrait encore gagner en latence et devenir un slicer matériel très intéressant.

Mais pour l’objectif complet du projet :

```text
OOK + ASK + M-ASK
```

`IQ_EST` reste supérieur fonctionnellement :

```text
CCA direct hypothétique
    → 1 bit : below/above threshold

IQ_EST
    → valeur de puissance
    → seuil OOK
    → plusieurs seuils ASK
    → calibration dynamique bruit/signal
```

Décision finale de v0.26 :

> **On arrête de dépendre de la découverte du bit CCA direct.**
> **Le chantier principal devient l’optimisation et la caractérisation temporelle de `IQ_EST`.**
> **CCA direct reste un bonus à rechercher opportunément, pas le chemin critique.**

---

# 36AR. IQ_EST — re-arm deux phases et métrologie exacte de `N` — v0.27

Cette passe traite en parallèle les deux verrous laissés ouverts en v0.26 :

1. peut-on réarmer `IQ_EST` sans couper le bloc entre chaque fenêtre ?
2. comment convertir `N` en durée réelle sans supposer arbitrairement la fréquence du datapath RX ?

## 36AR.1 Désassemblage exact de `rom_iq_est_disable()` : le disable est en deux phases

Le désassemblage de la mask-ROM à `0x40006400` donne la séquence suivante :

```text
rom_iq_est_disable @ 0x40006400

mask = 0xFFFA0001
base = 0x60000200

r = [base + 0x37C]          ; 0x6000057C
r = (r & 0xFFFA0001) | 0x00001000
[base + 0x37C] = r

r = [base + 0x37C]
r &= 0xFFFFFFFE
[base + 0x37C] = r
return
```

Le premier write est beaucoup plus informatif qu'un simple `ENABLE=0` :

```text
0xFFFA0001 conserve : bit0, bit17, bits19..31
0xFFFA0001 efface   : bit1, bits2..16, bit18
0x00001000          : N = 0x1000 >> 2 = 1024
```

Donc la première phase réalise :

```text
START = 0
N     = 1024
MODE  = 0
ENABLE reste inchangé, donc encore = 1 après une acquisition normale
```

La seconde phase seulement effectue :

```text
ENABLE = 0
```

**Fait démontré :** la ROM remet explicitement `START` à zéro **avant** de couper le bloc.

Cette structure est exactement celle que l'on attend d'un bloc qui doit voir une phase basse de `START` entre deux acquisitions.

## 36AR.2 `DONE` n'est pas effacé par logiciel

Le point le plus important du désassemblage est que le masque `0xFFFA0001` **conserve le bit31**. La routine `rom_iq_est_enable()` utilise le même masque avant d'écrire le nouveau `START`, et `rom_iq_est_disable()` conserve lui aussi bit31 pendant sa première phase.

Autrement dit, le logiciel ROM ne fait jamais :

```text
DONE = 0
```

par une écriture directe dans bit31.

Le bit `DONE` doit donc être remis à zéro par une transition matérielle du bloc. Les mécanismes plausibles sont :

```text
A. START passe à 0
B. nouveau START passe à 1
C. ENABLE passe à 0/1
```

Le corpus statique permet maintenant d'écarter l'idée d'un clear logiciel séparé, mais pas encore de distinguer A/B/C.

**Conséquence expérimentale majeure :** après une première mesure terminée, une simple lecture du registre juste après `START=0` permet de déterminer immédiatement si la phase basse réarme déjà `DONE`.

## 36AR.3 Hypothèse de streaming désormais principale

La séquence ROM complète par fenêtre est :

```text
ENABLE=1
START=1, N, MODE
attendre DONE
lire résultat
START=0, N=1024, MODE=0
ENABLE=0
```

La séquence optimisée candidate devient :

```text
une fois : ENABLE=1

boucle :
    START=0                  ← phase de réarmement
    START=1 + N + MODE       ← nouvelle fenêtre
    attendre DONE
    lire 0x600005E4
```

Donc le gain potentiel est la suppression, à chaque symbole, de :

- la coupure `ENABLE=0` ;
- le RMW de remise `ENABLE=1` ;
- l'appel/retour des wrappers ROM.

**Niveau de confiance : forte inférence, pas encore preuve silicium.** Le fait que la ROM impose explicitement une phase `START=0` avant `ENABLE=0` rend cette architecture nettement plus probable qu'en v0.26.

## 36AR.4 Primitive de test ROM-faithful avec `ENABLE` maintenu

Le test doit préserver les bits non commandés exactement comme la ROM :

```c
#define REG32(a) (*(volatile uint32_t *)(a))
#define IQ_EST_CTRL REG32(0x6000057C)
#define IQ_EST_PWR  REG32(0x600005E4)

#define IQ_CMD_KEEP_MASK 0xFFFA0001U
#define IQ_IDLE_N1024    0x00001000U
#define IQ_ENABLE        (1U << 0)
#define IQ_START         (1U << 1)
#define IQ_MODE1         (1U << 18)
#define IQ_DONE          (1U << 31)

static inline void xt_memw(void)
{
    __asm__ __volatile__("memw" ::: "memory");
}

static inline void iq_stream_enable_once(void)
{
    uint32_t r = IQ_EST_CTRL;
    r |= IQ_ENABLE;
    xt_memw();
    IQ_EST_CTRL = r;
    xt_memw();
}

static inline void iq_stream_start(uint32_t n)
{
    uint32_t r;

    /* phase basse reproduisant la première moitié du disable ROM */
    r = IQ_EST_CTRL;
    r = (r & IQ_CMD_KEEP_MASK) | IQ_IDLE_N1024;
    xt_memw();
    IQ_EST_CTRL = r;
    xt_memw();

    /* phase haute : nouvelle fenêtre */
    r = IQ_EST_CTRL;
    r = (r & IQ_CMD_KEEP_MASK)
      | IQ_MODE1
      | ((n & 0x7FFFU) << 2)
      | IQ_START;
    xt_memw();
    IQ_EST_CTRL = r;
    xt_memw();
}

static inline uint32_t iq_stream_wait_power(void)
{
    while ((IQ_EST_CTRL & IQ_DONE) == 0) {
        xt_memw();
    }
    xt_memw();
    return IQ_EST_PWR;
}
```

Cette primitive n'est pas présentée comme « validée matériellement ». Elle est construite pour tester **la plus petite déviation possible** par rapport à la séquence ROM connue.

Test discriminant recommandé :

```text
1. acquisition ROM/référence → DONE=1
2. écrire seulement la phase START=0 en gardant ENABLE=1
3. relire immédiatement bit31

si DONE devient 0 :
    le clear est déjà causé par START=0 → streaming quasiment confirmé
sinon :
    écrire START=1 et observer si DONE chute puis remonte
si DONE reste stale :
    le toggle ENABLE est nécessaire
```

Pour éviter un faux positif où `DONE` resterait à 1, il faut également vérifier que `0x600005E4` change lorsqu'on fait varier volontairement le niveau RF entre deux fenêtres.

## 36AR.5 `rom_dc_iq_est()` confirme explicitement la normalisation par `N+1`

Le désassemblage de `rom_dc_iq_est @ 0x4000615C` contient :

```text
addi.n a13, a13, 1          ; N → N+1
...
l32i   ..., 0x600005DC
srai   ..., 6
call0  division_helper      ; division par N+1
...
l32i   ..., 0x600005E0
srai   ..., 6
call0  division_helper      ; division par N+1
```

Cette observation renforce fortement le modèle :

```text
nombre d'unités accumulées ≈ N + 1
```

Elle ne prouve toutefois pas qu'une unité égale exactement **un échantillon ADC brut**. Il peut s'agir d'un échantillon à une cadence interne décimée du baseband.

La bonne formulation devient donc :

```text
T_est(N) = Tfixe + (N+1) / F_est
```

où `F_est` est la cadence effective du moteur d'estimation, encore inconnue.

## 36AR.6 Pourquoi la fréquence ne doit pas être devinée à partir du Wi-Fi 20 MHz

La documentation matérielle publique décrit deux ADC RX rapides derrière la conversion I/Q, mais ne donne pas leur fréquence d'échantillonnage ni la cadence interne de `IQ_EST`. Le fait que le canal Wi-Fi soit 20 MHz ne suffit pas à conclure que `F_est=20 MHz` : le front-end peut suréchantillonner puis décimer avant l'accumulateur.

L'audit de `rom_set_rxclk_en()` montre également une commande de gating/activation analogique par I²C interne, pas un diviseur logiciel clair permettant de récupérer directement `F_est`.

**Conclusion :** la conversion `N→temps` doit être mesurée, pas supposée.

## 36AR.7 Mesure exacte avec `CCOUNT` : la pente élimine l'overhead

Le LX106 fournit un compteur de cycles CPU. On peut mesurer :

```text
C(N) = cycles entre START et DONE
```

puis ajuster :

```text
C(N) = C0 + k·(N+1)
```

Le terme `C0` absorbe :

- RMW MMIO ;
- `MEMW` ;
- premier passage de la boucle de polling ;
- latence fixe de synchronisation des domaines d'horloge.

La pente `k` révèle la cadence de l'estimateur :

```text
F_est = F_CPU / k
```

ou, sans même calculer l'intercept :

```text
F_est = F_CPU · (N2-N1) / (C(N2)-C(N1))
```

si la relation est linéaire.

Primitive de lecture :

```c
static inline uint32_t read_ccount(void)
{
    uint32_t c;
    __asm__ __volatile__("rsr.ccount %0" : "=a"(c));
    return c;
}
```

Mesurer de préférence une série géométrique :

```text
N = 0, 1, 3, 7, 15, 31, 63, 127,
    255, 511, 1023, 2047, 4095, 8191
```

Les grands `N` donnent une pente robuste malgré l'overhead fixe ; les très petits `N` permettent ensuite de déterminer la véritable fenêtre minimale et de voir si une latence plancher domine.

Pour chaque `N`, conserver au minimum :

```text
N
cycles START→DONE
DONE avant START-low
DONE après START-low
DONE juste après START-high
0x600005E4
```

Cette seule table tranche **simultanément** le re-arm et `N→temps`.

## 36AR.8 Critères de décision après mesure

### Cas A — idéal

```text
START=0 → DONE tombe à 0
START=1 → nouvelle mesure
C(N) linéaire avec N+1
```

Alors la primitive RX OOK devient :

```text
ENABLE une fois
START low/high
poll DONE
read E4
threshold
repeat
```

### Cas B — START-low ne clear pas DONE, mais nouveau START relance

Le streaming reste possible ; il faut distinguer le `DONE` stale du nouveau cycle, par exemple en observant le passage 1→0→1 ou la modification de la sortie.

### Cas C — aucun nouveau cycle sans toggle ENABLE

On conserve la séquence ROM complète en MMIO direct. Elle reste plus rapide que les appels de fonctions ROM et fournit toujours la voie RX principale.

## 36AR.9 Conclusion de v0.27

Les deux recherches parallèles convergent :

- **re-arm :** le désassemblage prouve une phase `START=0` distincte avant `ENABLE=0`; garder `ENABLE=1` est désormais la stratégie expérimentale principale ;
- **timing :** `N+1` est démontré côté normalisation, mais sa durée absolue n'est pas récupérable de façon fiable par le corpus statique ; `CCOUNT` et la pente sur plusieurs `N` fournissent une mesure exacte sur silicium.

Le prochain test matériel n'a donc plus besoin de chercher « au hasard ». Une seule campagne `CCOUNT + snapshots de DONE + E4` peut fermer les deux derniers verrous de la primitive RX rapide.

---


# 36AT. RX externe — préservation du chemin normal sous PBUS debug — v0.29

Cette passe cherche à fermer le principal doute statique restant après v0.28 : **`IQ_EST` peut-il être utilisé sur le chemin RX RF normal, sans dépendre du loopback de calibration ?**

La réponse statique devient maintenant **très fortement positive**. Il reste une validation RF sur silicium pour démontrer la réponse quantitative de `0x600005E4` à une porteuse appliquée à l'antenne, mais la séquence logicielle permettant de conserver le chemin RX normal est désormais reconstruite.

## 36AT.1 État PBUS du RX normal : `(2,1)=0x184`, `(3,2)=6`

Le désassemblage de `rom_pbus_xpd_rx_on @ 0x400076CC` donne directement :

```text
PBUS(2,1) = 0x184
PBUS(3,2) = 6
```

Il s'agit de l'état explicite installé par la primitive ROM nommée `pbus_xpd_rx_on`, donc de notre meilleure référence statique pour le **chemin RX RF normal actif**.

## 36AT.2 `pbus_debugmode()` ne remplace pas le chemin analogique RX

`rom_pbus_debugmode()` et le patch RAM `ram_pbus_debugmode()` ont maintenant été recoupés.

Le patch RAM :

1. lit l'état courant `PBUS(2,1)` ;
2. lit l'état courant `PBUS(3,2)` ;
3. utilise `0x184` et `6` comme valeurs de référence/secours ;
4. positionne `0x60009B08[27]`, le même bit que `stop_dig_rx()` ;
5. positionne `0x60000594[0]`, le latch du mode PBUS debug/force.

Il **ne force pas un nouvel état analogique de loopback lors de l'entrée en debug**.

Inversement, `rom_pbus_workmode()` :

```text
0x60000594[0] = 0
0x60009B08[27] = 0
```

et revient au fonctionnement RX numérique normal.

Conséquence architecturale :

> **PBUS debug sépare le contrôle manuel des gains / RF du moteur numérique de réception de paquets. Il n'implique pas, à lui seul, un basculement vers un loopback interne.**

## 36AT.3 `pbus_set_rxgain()` préserve explicitement les bits de contrôle du chemin RX

Le désassemblage exact de `rom_pbus_set_rxgain @ 0x4000754C` est particulièrement important.

Avant de réécrire `PBUS(2,1)`, la routine lit l'ancienne valeur puis applique :

```text
old_21 & 0x185
```

avant d'y ORer les nouveaux champs de gain.

`0x185` conserve les bits :

```text
bit 0
bit 2
bit 7
bit 8
```

Ces bits ne sont donc **pas remplacés par le mot de gain** : ils sont considérés comme des bits d'état/contrôle à préserver pendant un changement de gain RX.

C'est exactement la propriété nécessaire pour notre récepteur ASK : entrer en mode manuel depuis un RX normal, changer le gain, tout en conservant le mode/chemin RF préexistant.

## 36AT.4 Raffinement de `set_loopback_gain()` : `0x185` est transitoire ; `0x104` est l'état RXIQ final observé

Le désassemblage de `rom_set_loopback_gain @ 0x400067C8` montre cinq écritures PBUS et **aucun accès à `IQ_EST` ni à un registre baseband explicitement identifié comme mux loopback** :

```text
PBUS(2,1) = 0x185
PBUS(7,1) = arg2
PBUS(2,1) = arg3
PBUS(3,1) = arg4
PBUS(3,2) = arg5
```

La première valeur `0x185` est donc transitoire. Dans `set_rx_gain_cal_iq()`, l'appel correspondant est :

```text
arg1 = 1
arg2 = 0x104
arg3 = valeur variable
arg4 = 22
```

avec la convention d'appel reconstruite de la routine, ce qui conduit finalement à :

```text
PBUS(2,1) = 0x104
```

pour ce chemin de calibration RXIQ.

La comparaison utile devient donc :

```text
RX normal       : 0x184
RXIQ calibration: 0x104
                  ^
                  différence = 0x80 = bit7
```

Et `pbus_set_rxgain()` préserve précisément ce **bit7** via le masque `0x185`.

### Niveau de certitude

- **certain logiciel :** normal RX utilise `0x184` ; le chemin RXIQ observé programme `0x104` ; bit7 diffère ; le setter de gain préserve bit7 ;
- **forte inférence :** bit7 participe à une sélection de mode/chemin RX normal versus chemin de calibration interne ;
- **non encore démontré :** nom physique exact de bit7 et topologie analogique détaillée derrière ce bit.

Il serait donc prématuré de nommer bit7 « LOOPBACK_SEL » à 100 %, mais il devient le **meilleur candidat statique de séparation de chemin**.

### 36AT.4b Le bit0 de `PBUS(2,1)` est fortement relié au power-down RX, pas au loopback

Un recoupement supplémentaire retire une ambiguïté importante. Dans plusieurs calibrations ROM indépendantes (`rom_rfcal_pwrctrl`, `rom_rfcal_txcap`, `rom_rfcal_txiq`), l'appel à l'entrée `g_phyFuns + 0xC0`, identifiée comme `rom_pbus_xpd_rx_off`, est effectué avec :

```text
a2 = 1
rom_pbus_xpd_rx_off(1)
```

Le désassemblage de `rom_pbus_xpd_rx_off @ 0x40007688` montre que cette valeur est écrite directement dans `PBUS(2,1)`, puis que `PBUS(3,1)` et `PBUS(3,2)` sont mis à zéro. À l'inverse, `rom_pbus_xpd_rx_on()` établit `PBUS(2,1)=0x184` et `PBUS(3,2)=6`.

Le bit0 est donc **très fortement associé à l'état OFF/power-down du RX**. Cela explique naturellement pourquoi `set_loopback_gain()` écrit d'abord :

```text
0x185 = 0x184 | 1
```

comme état transitoire avant de programmer son état final `0x104` : le RX est vraisemblablement placé dans un état sûr/power-down pendant la reconfiguration PBUS.

Conséquence :

> **bit0 n'est plus un candidat sérieux au sélecteur de loopback. La différence bit7 entre `0x184` et `0x104` devient beaucoup plus discriminante.**

Le nom électrique exact de bit0 reste non documenté, mais son rôle fonctionnel OFF/XPD est maintenant soutenu par plusieurs chemins indépendants.

## 36AT.5 `set_loopback_gain()` n'est pas l'activation générique d'`IQ_EST`

Sur les générations Espressif ultérieures, les tables PHY exposent séparément :

```text
set_loopback_gain
loopback_mode_en
rx_gain_force
IQ estimator / fonctions de mesure
```

Cette séparation inter-générations est cohérente avec ce que montre l'ESP8266 : `set_loopback_gain()` configure des valeurs PBUS ; `IQ_EST` est un instrument baseband distinct.

Ce recoupement ne sert pas à transposer arbitrairement les registres d'un autre SoC, mais il renforce la distinction fonctionnelle entre **gain de loopback**, **activation du loopback**, **gain RX forcé** et **mesure IQ**.

## 36AT.6 Séquence RX externe statiquement la plus solide

La séquence de référence devient :

```text
initialisation PHY/RF normale
        ↓
rom_pbus_xpd_rx_on()
        ↓
PBUS(2,1)=0x184 ; PBUS(3,2)=6
        ↓
rom_set_rxclk_en(1)
        ↓
pbus_debugmode()
        ↓
packet/digital RX arrêté
état analogique PBUS RX conservé
        ↓
pbus_set_rxgain(...)
pbus_set_rxbbgain(...)
        ↓
bits de contrôle du chemin RX préservés
        ↓
NE PAS appeler set_loopback_gain()
NE PAS démarrer de TX tone
        ↓
IQ_EST enable / START
        ↓
DONE
        ↓
lecture 0x600005E4
        ↓
seuil OOK ou quantification ASK
```

Cette construction est importante parce qu'elle ne dépend plus d'une extrapolation depuis le chemin RXIQ complet : elle **retire volontairement les deux éléments propres à la calibration interne**, `set_loopback_gain()` et `start_tx_tone()`, tout en conservant les primitives démontrées compatibles avec `IQ_EST` et le gain forcé.

## 36AT.7 La voie noise-floor fournit une corroboration indépendante du RX normal

Le PHY v6 expose également une mesure matérielle indépendante :

```text
ram_start_noisefloor()
    → configure 0x60009B60 / 0x60009B64

read_hw_noisefloor()
    → lit 0x60009824[11:0]
    → conversion signée /2
```

Cette voie appartient au fonctionnement normal de réception et n'utilise ni TX tone ni `set_loopback_gain()`.

Elle ne remplace pas `IQ_EST` pour OOK/ASK rapide : la logique de noise-floor standard comporte filtrage, état et délais plus lents. Mais elle constitue une **deuxième preuve indépendante que le RX normal possède un readout de niveau matériel CPU-visible**.

## 36AT.8 Ce qui est maintenant fermé statiquement et ce qui reste physique

| Élément RX | Statut après v0.29 |
|---|---:|
| état PBUS RX normal | **~99 %** |
| entrée/sortie PBUS debug | **~99 %** |
| arrêt packet RX sans couper le contrôle RF manuel | **~99 %** |
| conservation du mode RX lors de `pbus_set_rxgain()` | **~99 %** |
| contrôle `IQ_EST` / `DONE` / `E4` | **~99 %** |
| séparation fonctionnelle IQ_EST vs helper loopback | **~98–99 %** |
| chemin logiciel proposé RX externe → IQ_EST | **~98 %** |
| rôle exact de `PBUS(2,1).bit7` | **~95 %** comme bit de mode/chemin, nom physique exact ouvert |
| re-arm streaming sans toggle ENABLE | **~90–95 %**, validation silicium restante |
| réponse de `E4` à une porteuse sur antenne | **à valider physiquement** |
| `N → temps` absolu | **à mesurer** |
| gain PBUS → dB | **à mesurer** |
| sensibilité / dynamique / niveaux ASK séparables | **à mesurer** |

### Estimation globale mise à jour

Pour distinguer reverse-engineering et caractérisation RF :

```text
Architecture logicielle/statique RX OOK : ~98–99 %
Architecture logicielle/statique RX ASK : ~95–97 %

Validation physique RF complète : pas 100 % tant que les tests
antenne + CCOUNT + niveaux de gain n'ont pas été exécutés sur silicium.
```

Le principal verrou de compréhension logicielle n'est donc plus « comment atteindre le RX externe ? ». La séquence est désormais reconstruite. Les derniers points décisifs sont des **mesures matérielles discriminantes** : réponse `E4` antenne OFF/ON, comportement de `DONE` au re-arm, pente `N→cycles`, puis gain/dynamique.

---

# 37. Conclusions actuelles

## 37.1 Jusqu’où le Wi‑Fi est logiciel ?

Très bas.

Le LX106 exécute notamment :

```text
802.11 management
rate control
retry policy
CW/AIFS/backoff selection
TXOP policy
collision handling
ACK/CTS timeout policy
PLL configuration
gain control
power calibration
I/Q calibration
loopback setup
tone generation configuration
IQ estimator control
```

## 37.2 Ce qui reste matériel

Principalement :

```text
microsecond timing
slot countdown
CCA real-time
auto ACK
PHY symbol processing
DSSS/CCK/OFDM
FFT/FEC
high-rate I/Q datapath
ADC/DAC
RF analog
```

## 37.3 L’ESP8266 n’est pas un SDR

Il ne semble pas exposer les échantillons I/Q bruts.

Mais il expose :

- générateur de stimuli TX ;
- loopback RF interne ;
- estimateur I/Q ;
- corrélateur ;
- mesures de puissance ;
- DC I/Q ;
- mismatch amplitude/phase ;
- gains RF ;
- PLL ;
- PBUS ;
- I²C analogique.

C’est donc beaucoup plus proche d’un **PHY programmable avec instrumentation RF intégrée** qu’un simple contrôleur Wi‑Fi opaque.

## 37.4 OOK/ASK — statut final logiciel

Pour la cible autonome sans retour Wi-Fi, les deux primitives sont désormais fermées côté logiciel :

```text
OOK : 0x600005B8 bit18
ASK : 0x600005B8 bits17:10 = ((-digital_scale)&0xff)<<10
```

Le slot doit être normalisé une fois en mode tone normal, puis :

- OOK ne modifie que bit18 ;
- M-ASK ne modifie que bits17:10 ;
- PLL, TX clock et chaîne RF restent configurés ;
- `rom_stop_tx_tone()` n'est pas utilisé dans la boucle de symboles.

Les mesures de dBm, extinction, latence, jitter et phase sont désormais classées comme **validation matérielle/RF**, non comme reverse-engineering logiciel restant.


## 37.5 Réception OOK/ASK — direction principale v0.22

La réception OOK/ASK ne doit pas passer par le décodage de paquets Wi-Fi. Le chemin privilégié devient :

```text
antenne → RF RX → baseband → IQ_EST → énergie → seuil(s) logiciel(s)
```

Le RSSI de paquet est rejeté pour cette fonction ; CCA/noise-floor servent plutôt de détecteur binaire rapide potentiel et de baseline lente. Pour ASK/M-ASK, le contrôle/freeze de l’AGC devient une exigence majeure afin de préserver les écarts d’amplitude.

La priorité de recherche après v0.27 est donc : **valider le chemin antenne → `IQ_EST`, exécuter la campagne combinée `START-low/high + DONE + CCOUNT + E4`, puis figer le gain RX pour M-ASK. Le status CCA instantané reste une optimisation secondaire.**

---

# 38. Journal des versions

| Version | Date | Changements |
|---|---|---|
| 0.1 | 2026-09-12 | Première consolidation complète de l’analyse : ROM, `g_phyFuns`, LMAC/WDEV, PBUS, I²C analogique, loopback, tone generator, IQ estimator et état des recherches OOK. |
| 0.2 | 2026-09-12 | Reconstruction complète de `phy_func_tab`, correction TX continu vs TX tone, corroboration avec PHY Espressif ultérieurs et mise à jour des niveaux de confiance OOK. |
| 0.3 | 2026-09-12 | Décodage quasi complet de `rom_start_tx_tone()` et `rom_stop_tx_tone()` : formules des 3 slots, comportement des slots désactivés, bit28 du slot3 et coupure globale du TX clock. Une interprétation TXIQ introduite dans cette version est corrigée en v0.4. |
| 0.4 | 2026-09-12 | Correction de la confusion `+104 décimal` / `+0x104` : TXIQ appelle `rfcal_txiq_cover`, pas `start_tx_tone`. Recensement des trois vrais appels ROM à `start_tx_tone`, tous slot1-only ; statut de la mise à jour à chaud du coefficient rétrogradé à non démontré ; rôle du bit28 du slot3 raffiné. |
| 0.5 | 2026-09-12 | Analyse de `phy_dig_spur_set/prot` dans le `libphy.a` fourni : ces routines utilisent les blocs `0x60009600/0x60009A00`, font des calculs de coefficients et ne passent pas par le générateur de tone, PBUS ou l’I²C analogique. Hypothèse slots2/3 = spur cancellation rétrogradée. Layout de `IQ_EST` reconfirmé instruction par instruction. |
| 0.6 | 2026-09-12 | Décodage de `tx_cont_en/dis`: le mode TX continu sauvegarde/modifie/restaure `0x60000594/598/59C`. Modifications reconstruites (`5C |= 0x0FE03F80`, `598 |= 0x0FFFFFFF`, `594 &= 0xFFCFFFFF`). Confirmation binaire que TX-continu et tone-generator sont deux mécanismes distincts. |
| 0.7 | 2026-09-12 | `tx_cont_cfg()` décodé comme setter booléen (`1`→enable, `0`→disable). Cette version attribuait les appels `0x2EFD/0x2F05` à `periodic_cal()` ; **correction v0.20 : ils appartiennent à `chip_v6_initialize_bb()`**. `phy_wakeup_rf()` reste confirmé comme autre appelant 1/0. Identification de `tx_data2/3/4` comme snapshots de `0x60000594/598/59C`. |
| 0.8 | 2026-09-12 | Relecture et consolidation interne : synchronisation du résumé, du bit28 slot3, des questions ouvertes et du plan de recherche avec les découvertes 36D–36T. Retrait des tâches déjà closes (recensement des appels ROM, piste `phy_dig_spur_*`→tone), ajout explicite des travaux encore ouverts sur slots2/3, `IQ_EST` mode M, `tx_data1` et validation RF. Aucune nouvelle propriété RF n’est revendiquée dans cette version. |
| 0.9 | 2026-09-12 | Décodage complet du quatrième état de `tx_cont_en/dis` : `tx_data1` est le snapshot de `rom_i2c_readReg(0x66,3,1)` ; bits 5:0 forcés à `0x3C` par `rom_i2c_writeReg_Mask`, puis restauration exacte par `rom_i2c_writeReg`. Identification fonctionnelle du flag `.bss+0xC4` protégeant le save/restore contre les activations imbriquées. Raffinement de `rom_rfcal_pwrctrl()` : `control_1` reprend directement son premier argument, indépendamment du coefficient issu de `set_ana_inf_tx_scale()`. |
| 0.10 | 2026-09-12 | Nouvelle passe sur les relocations de `phy_chip_v6.o` : graphe d’appels direct confirmé (`tx_cap_init→ram_rfcal_pwrctrl/ram_rfcal_txcap`, `tx_pwctrl_init_cal→ram_rfcal_pwrctrl`, `set_rx_gain_cal_iq→ram_rfcal_rxiq`, `chip_v6_initialize_bb→ram_rfcal_txiq`). Conséquence pour le tone : `tone_control`, déjà démontré indépendant du coefficient d’amplitude, est reclassé comme paramètre de stimulus ; l’hypothèse step/incrément NCO devient l’hypothèse principale, renforcée par la séparation `start_tx_tone_step/start_tx_tone` des PHY Espressif ultérieurs, sans être considérée comme prouvée sur ESP8266. |
| 0.11 | 2026-09-12 | Décodage instruction par instruction des chemins tone du PHY v6 : preuve que le premier argument de `ram_rfcal_pwrctrl()` devient directement `control_1`; ses deux appelants v6 utilisent `64`. `tx_cap_init→ram_rfcal_txcap` utilise également `64`, et `meas_tone_pwr_db()` fixe explicitement `control_1=64` tout en faisant varier séparément le coefficient. Le chemin RXIQ est retracé de `set_rx_gain_testchip_50()` à `set_rx_gain_cal_iq()` puis `ram_rfcal_rxiq()` et utilise `tone_control=8`. L’ensemble démontré devient donc `{8,64}`; l’hypothèse step/index NCO est renforcée, tandis que l’hypothèse d’un paramètre signé est rétrogradée faute de valeur négative observée. |
| 0.12 | 2026-09-12 | Découverte du chemin MMIO direct TXIQ : `txiq_get_mis_pwr()` programme `0x600005B8` via la base `0x60000200+0x3B8`, force notamment le bit18, reconstruit le coefficient `17:10`, puis modifie `bits27:24` avant des mesures `txtone_linear_pwr`. `txiq_cover()` appelle cette routine deux fois sans stop intermédiaire, avec deux coefficients différents ; la reprogrammation à chaud du coefficient sous TX clock active est donc démontrée. Correction de 36F : TXIQ ne prouve pas un hot-update via `start_tx_tone`, mais le prouve par MMIO direct. Recensement `IQ_EST` : les appels v6/ROM inspectés utilisent tous `M=1`; aucun usage `M=0` n’a été retrouvé dans les objets fournis. |
| 0.13 | 2026-09-12 | Décodage exact du nibble TXIQ `bits27:24` grâce à l'identification de `MOVEQZ` : états `0→1` pour `sel=0` et `4→8` pour `sel=1`. Classification de `bit21` et `bit19` comme flags supplémentaires du chemin TXIQ direct. Corroboration indépendante dans le dump mask-ROM : `rom_rfcal_txiq_cover@0x400088B8` programme le même `0x600005B8` avec les mêmes masques `F0000000/002C0000/F0FFFFFF` et la même machine d'états. Traçage complet du paramètre v6 : `chip_v6_initialize_bb()` passe `a5=64` à `ram_rfcal_txiq`, qui aboutit directement à `tone_control=64` dans `txiq_get_mis_pwr`. |
| 0.14 | 2026-09-12 | Fonction des paires TXIQ résolue par traçage de `txiq_cover()` et de la chaîne debug `txiq_gain=%d, txiq_phase=%d` : `bits27:24 = 4→8` alimente la correction de **gain I/Q TX**, tandis que `0→1` alimente la correction de **phase I/Q TX**. Forte inférence que bits26/27 sélectionnent deux chemins orthogonaux pour la mesure de gain et que bit24 participe au test de phase. Revalidation de la fausse piste `phy_dig_spur_prot()` : ses offsets `0x3B8/0x3BC` sont relatifs à `0x60009600`, pas aux tone slots. Recoupement avec la commande officielle `wifiscwout` : l’interface publique single-carrier n’expose que enable/canal/atténuation, aucun paramètre équivalent à `tone_control`; la loi `8/64 → effet physique` reste donc interne. |
| 0.15 | 2026-09-12 | Correction architecturale majeure du slot tone : le premier argument de `rom_start_tx_tone()` n'est pas un booléen `enable`, mais alimente directement un champ `tone_mode` situé en `bits27:18`. Le mode normal vaut `0x001`, dont le LSB est le gate bit18. La constante TXIQ `0x002C0000` se réécrit exactement `0x00B<<18`; les séquences TXIQ deviennent donc `tone_mode 0x00B→0x04B` (phase I/Q) et `0x10B→0x20B` (gain I/Q). Les anciens flags bit19/21 et le nibble 27:24 sont ainsi réunifiés dans un même champ de 10 bits. `stop_tx_tone()` n'efface que `tone_mode[0]`, ce qui renforce le bit18 comme primitive OOK minimale préservant la configuration supérieure. Vérification supplémentaire : `start_tx_tone()` peut programmer les trois slots au cours d'un même appel sans exclusivité logicielle, donc l'armement multi-slot est prévu par l'API même si la combinaison RF reste à caractériser. |
| 0.16 | 2026-09-12 | Audit exhaustif des accès mask-ROM aux offsets des tone slots : après remontée systématique de la base `0x60000200`, seuls trois chemins réels accèdent à `0x600005B8/BC/C4` (`start_tx_tone`, `stop_tx_tone`, TXIQ direct) ; les autres occurrences d’offset sont des faux positifs avec une autre base. Aucune formule logicielle `tone_control→fréquence` n’existe dans ces chemins : `tone_control` est injecté brut dans le matériel, ce qui place la loi physique derrière la frontière hardware. Découverte supplémentaire de timing : `start_tx_tone`, `stop_tx_tone` et TXIQ encadrent systématiquement les accès aux slots par `MEMW`, démontrant une sérialisation explicite des MMIO et imposant d’inclure barrière/bus périphérique dans toute future mesure de latence OOK. |
| 0.17 | 2026-09-12 | Instrumentation interne affinée : `M=1` de `IQ_EST` est relié directement aux calibrations RXIQ de corrélation/puissance (`0x580/584/588/58C` + `0x5E4`) et `set_rx_gain_cal_iq()` l’utilise avec `N=1024` pour une mesure de puissance interne. `rom_dc_iq_est()` transmet son paramètre mode à `iq_est_enable()` puis lit `0x5DC/5E0`, ce qui sépare la notion de mode du choix des registres résultats. Deuxième famille indépendante confirmée : `get_fm_sar_dout()` pilote un bloc autour de `0x60000D50/0x60000D80`, attend 25 µs, puis `get_power_db()` convertit ses deux sorties en dB ; `txtone_linear_pwr()` agrège également cette primitive de feedback. L’ESP8266 dispose donc de deux chaînes internes candidates pour comparer le tone ON/OFF (RX loopback/IQ_EST et TX/SAR) ; leur usage volontaire avec `tone_mode[0]=0` reste à valider. |
| 0.18 | 2026-09-12 | Raffinement du packing du générateur : `start_tx_tone()` ne masque explicitement ni `tone_control` à 10 bits ni `mode_code` avant leurs OR/décalage ; le modèle 10+8+10 est conservé comme packing observé plutôt que largeur hardware imposée. Décodage exact de `set_ana_inf_tx_scale()` : seuil non signé 64, `digital_scale=min(x,63)`, scale analogique `(63-x)&0xff` au-delà, et champ bits17:10 stocké sous forme `(-digital_scale)&0xff`. `meas_tone_pwr_db()` confirme `mode=1`, `tone_control=64`, scale variable puis deux mesures `get_power_db(312)`. La voie SAR comporte au moins un délai explicite de 25 µs par acquisition et est donc classée comme mesure de niveau, pas comme instrument de fronts OOK rapides. |
| 0.19 | 2026-09-12 | Décodage détaillé de la chaîne de mesure SAR du tone : `rom_txtone_linear_pwr(n,q)` accumule exactement des rapports `((SAR_A<<q)/max(SAR_B,1))`; TXIQ utilise `txtone_linear_pwr(4,10)` dans la ROM et le PHY v6, soit une métrique Q10 sur quatre acquisitions. `get_power_db(k)` est reconstruit comme `k + linear_to_db(SAR_A,3) - linear_to_db(SAR_B,3)` et `meas_tone_pwr_db()` effectue deux mesures puis un arrondi/division par 4. `rom_linear_to_db()` ajoute 48 unités par octave, donnant une forte inférence d'échelle 1/8 dB puis quart-de-dB après la division finale. Décodage supplémentaire de `ram_get_fm_sar_dout()` : ses sorties sont `max(2×(s1+s2+s3)-3×(s6+s7),0)` et `max(3×(s4+s5)-3×(s6+s7),0)`. Une mesure Q10 à 4 acquisitions implique au minimum 100 µs de délai SAR explicite : adaptée aux niveaux steady-state ON/OFF, pas aux fronts OOK rapides. |
| 0.20 | 2026-09-12 | Intégration logicielle OOK : correction de l'attribution des appels `tx_cont_cfg(1/0)` (`chip_v6_initialize_bb`, pas `periodic_cal`), cartographie des déclencheurs `periodic_cal_top` depuis PP/PM/hostap et du `tx_pwctrl_background` après TX normal, constat que les écrivains directs du tone sont surtout des chemins d'initialisation/réinitialisation. Découverte que les patches `ram_tx_mac_enable/disable` sont des `ret.n` (no-op), donc pas de verrou MAC au niveau PHY. Confirmation que `start_tx_tone()` ne prépare pas seul toute la RF : les calibrations activent extérieurement PBUS/TX-XPD. Définition d'une machine d'état autonome OOK et identification des deux derniers verrous logiciels : fenêtre MAC/RF exclusive et séquence PBUS/RF minimale réversible. |
| 0.21 | 2026-09-12 | **Clôture logicielle OOK/ASK.** Changement de cible : fonctionnement Wi-Fi abandonné après initialisation RF, aucun retour/coexistence requis. OOK canonique = RMW du bit18 avec TX clock/RF maintenus ; ASK/M-ASK canonique = hot-update de bits17:10 avec encodage `(-digital_scale)&0xff`, analog scale traité comme réglage de plage. Les anciennes exigences de fenêtre MAC réversible et restauration Wi-Fi sont retirées du chemin critique. Les inconnues restantes (dBm, extinction, latence, jitter, phase, linéarité RF) sont reclassées en caractérisation matérielle et non en logiciel non décodé. |
| 0.22 | 2026-09-12 | **Ouverture du chantier RX OOK/ASK autonome.** Décodage du gate RX WDEV (`0x3FF20004 bit31`), des primitives `start_dig_rx/stop_dig_rx`, du gate CCA (`0x60009B00 bit28`) et du champ seuil CCA (`0x60009B64[19:12]`). Cartographie des noise-floor (`0x60009B64[31:20]`, `0x60009824[11:0]`) et démonstration que le suivi standard `libpp` est à 100 ms, donc trop lent pour les symboles. `ic_get_rssi()`/sniffer RSSI sont reclassés comme métriques de paquet Wi-Fi, non comme enveloppe RF libre. `IQ_EST` + `0x600005E4` devient le détecteur d’énergie RX principal ; CCA reste candidat de slicer OOK rapide et AGC/fixed-gain devient le verrou majeur pour ASK/M-ASK. |
| 0.23 | 2026-09-12 | **RX OOK/ASK : correction AGC/CCA et gain fixe PBUS.** Le traçage de `register_phy_ops()` démontre que les wrappers nommés `phy_enable_agc/phy_disable_agc` appellent en réalité `rom_chip_v5_enable_cca/disable_cca`; ils ne figent pas le gain RX. `rom_chip_v5_sense_backoff()` est décodée comme programmation de deux champs de sensibilité 7 bits (`0x60009C28[16:10]` et `0x60009D24[7:1]`), pas comme readout CCA. `pbus_set_rxbbgain(n)` est reconstruit en étage grossier + fin via PBUS bank3/selectors1-2, fournissant une voie démontrée de gain RX fixe pour M-ASK. `rom_pbus_set_rxgain()` confirme un mot de gain RX multi-étages. Le status CCA instantané et la loi `IQ_EST N→temps` restent matériels/ouverts. |
| 0.24 | 2026-09-12 | **RX OOK/ASK : exploitation du DWARF WDEV.** Reconstruction exacte du `RxControl` 12 octets du corpus NONOS, dont `rssi` signé en bits7:0 et `noise_floor` signé en bits31:24 du troisième mot. Ces deux valeurs sont confirmées comme métadonnées de réception de paquet et non comme enveloppe RF libre. Le DWARF de `wDev_ProcessFiq()` révèle un local `event` et les variables `rxstart_time/txcomplete_state/ack_snr`; les literals de la fonction pointent vers les blocs MAC/WDEV `0x3FF20A00`, `0x3FF20E00` et `0x3FF1FE00`, sans base BB `0x60009A00`. La recherche du CCA temps réel est donc recentrée sur le décodage du mot d’événements MAC/FIQ. |
| 0.25 | 2026-09-12 | **RX : méthode de lecture CPU concrétisée.** `wDev_ProcessFiq()` est rattaché à `0x3FF20C18/20/24` (enable, événements latched, clear), ce qui reclassifie la piste FIQ comme événements MAC plutôt que readout RF continu. La mask-ROM `rom_get_corr_power@0x40006260` confirme directement que `0x600005E4` appartient au bloc de résultats puissance/corrélation. La primitive pratique devient `rom_iq_est_enable(1,N) → lire 0x600005E4 → rom_iq_est_disable()`. Le packing de `N` et la normalisation `N+1` rendent `N=0` candidat de fenêtre minimale, à valider sur silicium. Le bit CCA busy direct reste ouvert mais n’est plus bloquant pour obtenir un état RX binaire par seuil logiciel. |
| 0.26 | 2026-09-12 | **Décision RX : `IQ_EST` ultra-rapide devient la voie principale.** Audit CCA ciblé : `0x60009B00 bit28` est seulement le gate enable/disable ; `set_cca()` et `sense_backoff()` ne font que configurer seuil/sensibilité, et aucun readout CPU continu `CCA busy` n’est retrouvé dans le corpus. Reconstruction du contrôle `0x6000057C` : bit0 block-enable, bit1 start, bits16:2 `N`, bit18 mode, bit31 DONE. Deux usages réels sont retrouvés (`N=1024` et `N=8192`). La ROM n’insère aucun délai logiciel dans `iq_est_enable()`. Proposition d’une primitive MMIO directe et d’un mode streaming expérimental gardant bit0 actif ; le re-arm sans disable et la loi `N→temps` restent à mesurer sur silicium. |
| 0.27 | 2026-09-12 | **IQ_EST : re-arm deux phases + métrologie `N+1`.** Désassemblage exact de `rom_iq_est_disable()` : première écriture `(CTRL & 0xFFFA0001) | 0x1000` force `START=0`, `N=1024`, `MODE=0` tout en conservant `ENABLE=1`; seconde écriture seulement efface `ENABLE`. `DONE` bit31 est préservé par les RMW et doit donc être géré/clear matériellement par une transition START/ENABLE. `rom_dc_iq_est()` confirme une division par `N+1`. La stratégie principale devient `ENABLE` permanent + front START bas→haut, à valider sur silicium, et une campagne `CCOUNT` sur plusieurs N pour extraire exactement la cadence effective de l’estimateur sans supposer la fréquence ADC/baseband. |
| 0.28 | 2026-09-12 | **RX autonome : PBUS debug = gain forcé avec packet RX arrêté.** Désassemblage croisé de `rom_pbus_debugmode`, `rom_pbus_workmode`, `start_dig_rx` et `stop_dig_rx` : le même `0x60009B08[27]` arrête/réactive le RX numérique, tandis que `0x60000594[0]` est le latch debug/force PBUS. Traçage de `set_rx_gain_testchip_50()` : entrée en `ram_pbus_debugmode`, boucle `ram_pbus_set_rxgain`, appel `set_rx_gain_cal_iq` qui exécute `IQ_EST(1,1024)`, puis retour `pbus_workmode`. Cela démontre que `IQ_EST` fonctionne avec le packet RX numérique arrêté et le gain RX sous contrôle PBUS manuel. L’architecture OOK monte à ~96–97 %, ASK/M-ASK à ~92–94 % ; les inconnues restantes sont principalement matérielles (antenne externe, timing, dB, sensibilité). |
| 0.29 | 2026-09-12 | **RX externe : préservation du chemin normal sous PBUS debug.** `rom_pbus_xpd_rx_on()` fixe le RX normal à `PBUS(2,1)=0x184`, `PBUS(3,2)=6`; `pbus_debugmode()` coupe le packet RX sans imposer un autre état analogique; `rom_pbus_set_rxgain()` préserve explicitement `old_21 & 0x185`, donc les bits de contrôle dont bit7. `set_rx_gain_cal_iq()` programme finalement `0x104` via `set_loopback_gain()`, faisant de la différence `0x184↔0x104` (bit7) le meilleur candidat statique de séparation normal/calibration. Les appels indépendants `pbus_xpd_rx_off(1)` relient fortement bit0 au power-down RX et éliminent bit0 comme candidat loopback. La séquence de référence externe devient RX normal → RX clock → PBUS debug → gains forcés → IQ_EST direct, sans `set_loopback_gain` ni TX tone. Architecture statique OOK ~98–99 %, ASK ~95–97 %; les restes sont physiques. |

---

# 39. Convention pour les prochaines mises à jour

Chaque nouvelle version devra :

1. conserver les découvertes précédentes utiles ;
2. corriger explicitement les erreurs antérieures ;
3. ajouter une entrée dans le journal des versions ;
4. mettre à jour le tableau des questions ouvertes ;
5. distinguer clairement :
   - **fait démontré** ;
   - **forte inférence** ;
   - **hypothèse** ;
   - **question ouverte**.

Nom de référence du document :

```text
ESP8266_WIFI_PHY_REVERSE_ENGINEERING.md
```
