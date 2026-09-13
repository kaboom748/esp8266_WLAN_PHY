# ESP8266 — Reverse-engineering du Wi‑Fi, du PHY et du chemin RF

**Document technique vivant — version 0.51**  
**Date de création : 2026-09-12 — mise à jour : 2026-09-13**  
**Objet :** cartographier le PHY/RF de l’ESP8266 et exploiter directement ses blocs RF hors protocole Wi‑Fi. Les modèles OOK/ASK sont fermés dans leur périmètre logiciel ; la recherche active est étendue au **FSK/M-FSK**, d’abord TX (`tone_control`/tone-step), puis RX (discrimination de fréquence), tout en conservant la chaîne RX autonome `IQ_EST` comme référence de mesure.

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


> **Mise à jour critique v0.44 — binaires exacts :**
> `set_rf_freq_offset()` appelle `ram_rfpll_set_freq()` puis `wait_rfpll_cal_end()` ;
> il est donc reclassé en voie de correction/retuning PLL, et non en fast-FSK privilégié.
> Le pipeline CFO RX exact est maintenant tracé jusqu'à `phy_get_bb_freqoffset()` et au
> champ `sint16 esf_buf_s.chl_freq_offset` à l'offset `+24`.


---


> **Mise à jour critique v0.45 — CFO RX :**
> le binaire exact démontre désormais que `phy_get_bb_freqoffset()` lit
> `0x60009800[15:8]`, vérifie `bit0`, convertit le champ signé par `(raw*107)>>6`,
> stocke le résultat dans `phy_meas_freq_offset`, puis cette valeur est transportée
> sans changement d'échelle jusqu'à `bss_info.freq_offset`. Le verrou FSK RX est
> désormais le mécanisme de rafraîchissement/validation de ce registre hors packet Wi-Fi.



> **Mise à jour critique v0.46 — FSK parallèle :**
> le TX rapide reste centré sur `tone_control`; `set_rf_freq_offset()` est une voie
> RFPLL/calibration. Côté RX, le readout CFO est fermé et la recherche se concentre
> désormais sur l’armement hardware autour de `0x600098xx/0x600099xx`, notamment la
> séquence clear→set observée sur `0x60009988.bit26`.



> **Mise à jour critique v0.47 :**
> `phy_bb_rx_cfg()` est une routine d'initialisation et non le trigger CFO par paquet.
> Le chemin logiciel standard lit le CFO seulement pour `RSSI > -90` et
> `TestStaFreqCalValOK==1`; la recherche du trigger doit maintenant viser le
> synchroniseur/estimateur hardware qui produit `0x60009800.bit0`.



> **Mise à jour v0.48 — calibration vs mesure CFO :**
> `DefFreqCalTimerCB()` ne réarme pas le hardware CFO ; il remet uniquement
> `CanDoFreqCal=1` après un cooldown logiciel one-shot de 1000 ms. Le trigger du
> résultat `0x60009800` reste donc un mécanisme BB distinct à identifier.



> **Mise à jour v0.49 :**
> le chemin logiciel standard vers le CFO est maintenant confirmé comme branche
> RX-success de `wDev_ProcessFiq()`. Cela ne ferme pas encore la condition de production
> hardware de `0x60009800`. Côté TX, `meas_tone_pwr_db()` confirme à nouveau
> `tone_control=64` sans révéler la loi vers les Hz.



> **Mise à jour critique v0.50 — CFO/EVM :**
> `0x60009800` contient au moins deux métriques distinctes. Le getter CFO exige
> `bit0=1` et acquitte ensuite via `0x600098DC|=0xF`; le getter EVM lit `[28:16]`
> sans ces opérations. Le bit0 est donc classé comme condition spécifique au CFO
> dans le logiciel observé. Côté TX, aucune conversion `tone_control→Hz` n'existe
> dans le corpus exact : la fermeture suivante doit passer par la mesure RF.



> **Mise à jour critique v0.51 — localisation RX :**
> `0x3FF2003C[19:16]` est isolé comme un état WDEV lu par le PHY mais non programmé
> par les routines WDEV retrouvées. Le scan ROM montre aussi que les calibrations
> canal/TXIQ utilisent des registres voisins, mais pas `0x60009800/0x600098DC`,
> ce qui renforce la spécificité du chemin CFO/EVM.


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
| 14 | **FSK/M-FSK actif depuis v0.30/v0.31 :** TX = mesurer `tone_control → fréquence` et le hot-update; RX = tracer le producteur brut de `bss_info.freq_offset` afin d'obtenir un discriminateur de fréquence sans paquet 802.11. |
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


# 36AU. Extension FSK TX — séparation tone-step / offset RF / PLL — v0.30

Cette passe rouvre volontairement le chantier FSK/M-FSK en commençant par le TX. Elle ne
change pas les conclusions OOK/ASK déjà figées : elle cherche uniquement à déterminer
quelle primitive peut produire **plusieurs fréquences porteuses sans retuner la PLL à chaque
symbole**.

## 36AU.1 Ce qui reste démontré sur ESP8266

Le corpus ESP8266 v6 démontre déjà que :

```text
0x600005B8 bits bas   = tone_control injecté brut par start_tx_tone()
0x600005B8 bits17:10  = coefficient/scale numérique
0x600005B8 bit18      = gate du tone normal
```

et que `tone_control` est indépendant du coefficient d'amplitude. Les valeurs démontrées
dans le corpus restent :

```text
RXIQ                      → tone_control = 8
TXIQ / PWRCTRL / TXCAP    → tone_control = 64
```

Aucune conversion logicielle `tone_control → Hz` n'existe dans la mask-ROM analysée :
la valeur est envoyée au matériel.

**Fait important inchangé :** le corpus ESP8266 ne démontre pas encore qu'un RMW des bits
bas pendant un tone actif change proprement la fréquence sans restart du générateur.

## 36AU.2 Nouveau recoupement inter-générations : trois commandes séparées

Les tables ROM publiques des PHY Espressif suivants exposent simultanément des primitives
distinctes :

```text
rom_start_tx_tone_step
rom_start_tx_tone
rom_set_rf_freq_offset
```

et, séparément, les primitives de synthèse RF :

```text
rom_write_rfpll_sdm
rom_rfpll_set_freq
rom_set_channel_freq
```

Ce découpage est visible notamment dans les ROM ESP32 et ESP32-S2.

Sources publiques de recoupement :

```text
https://git.liberatedsystems.co.uk/jacob.eva/arduino-esp32/src/commit/19ccc479c31d7fe35d0f30d23329a21ac4b805a5/tools/sdk/ld/esp32.rom.ld
https://git.liberatedsystems.co.uk/jacob.eva/arduino-esp32/src/commit/955675e7124bd3d41e6ff8b5d9505f624e7f5338/tools/sdk/esp32s2/ld/esp32s2.rom.ld
```

Conséquence architecturale :

> `tone step` et `RF frequency offset` sont deux objets distincts dans les PHY Espressif
> ultérieurs, et tous deux sont encore séparés du réglage PLL/canal.

Cela renforce nettement l'hypothèse déjà présente dans ce document :

```text
tone_control ≈ step / incrément d'un générateur numérique de tone
```

plutôt que :

```text
tone_control = commande directe de la PLL RF
```

Ce recoupement **ne prouve pas** que l'ESP8266 utilise exactement la même équation ni la
même granularité ; il réduit seulement l'espace des interprétations possibles.

## 36AU.3 Conséquence pour un TX 2-FSK / M-FSK rapide

La meilleure architecture candidate devient :

```text
PLL / canal        : fixes
TX RF / XPD        : préparés et fixes
TX clock           : ON en permanence
tone_mode bit18    : ON
digital_scale      : fixe
tone_control       : seule variable symbole-par-symbole
```

Conceptuellement :

```text
2-FSK :
    symbole 0 → tone_control = K0
    symbole 1 → tone_control = K1

M-FSK :
    symbole i → tone_control = Ki
```

La primitive à tester doit faire un **RMW ciblé** du champ bas sans toucher au scale ni au
mode :

```c
#define TONE1_ADDR         0x600005B8u
#define TONE_CONTROL_MASK  0x000003FFu  /* convention locale sûre */

static inline void fsk_set_tone_control(uint16_t k)
{
    uint32_t r;

    __asm__ volatile ("memw" ::: "memory");
    r = *(volatile uint32_t *)TONE1_ADDR;

    r &= ~TONE_CONTROL_MASK;
    r |= ((uint32_t)k & TONE_CONTROL_MASK);

    __asm__ volatile ("memw" ::: "memory");
    *(volatile uint32_t *)TONE1_ADDR = r;
}
```

**Attention :** le masque `0x3FF` reste une convention de sûreté du firmware, pas un masque
appliqué par la ROM.

## 36AU.4 Ce qui n'est PAS encore démontré

Quatre points restent ouverts avant de classer le TX FSK comme fermé :

1. relation physique exacte `tone_control → Δf` ;
2. monotonicité de cette relation ;
3. possibilité de modifier `tone_control` à chaud sans glitch/restart indésirable ;
4. temps MMIO→fréquence RF stable et continuité de phase entre K0/K1.

Le corpus ne permet donc pas encore d'annoncer une déviation FSK en kHz/MHz ni un baud
maximal.

## 36AU.5 Pourquoi le hopping PLL devient la voie de secours, pas la voie principale

L'ESP8266 possède bien :

```text
rom_set_channel_freq
rom_write_rfpll_sdm
rom_rfpll_set_freq
```

Un BFSK lent par retuning RF reste donc possible conceptuellement.

Mais ce chemin modifie le synthétiseur RF lui-même et introduit potentiellement :

```text
écriture PLL
→ acquisition / settling
→ éventuel verrouillage
→ fréquence RF stable
```

Il est architecturalement beaucoup plus lourd qu'un changement de step numérique. Pour
un FSK rapide, il doit être considéré comme **fallback expérimental**, pas comme chemin
canonique.

## 36AU.6 Test TX prioritaire qui ferme le plus d'inconnues

La campagne minimale doit conserver absolument constants :

```text
canal / PLL
TX XPD
TX clock
tone_mode = 1
digital_scale
gain TX
scale analogique
```

puis balayer uniquement :

```text
tone_control = 0,1,2,4,8,16,32,64,128,256,511
```

et mesurer :

```text
fréquence RF principale
puissance RF
temps de stabilisation après le write
présence de glitches / raies transitoires
continuité ou reset de phase
```

Les deux valeurs déjà démontrées `8` et `64` doivent être mesurées en priorité.

Une relation linéaire :

```text
Δf = A·tone_control + B
```

ou quasi linéaire fermerait immédiatement le mécanisme TX FSK. Une relation non monotone
indiquerait plutôt un codage de mode/step plus complexe.

## 36AU.7 Statut TX FSK après v0.30

| Élément | Statut |
|---|---:|
| existence d'un générateur tone distinct de la PLL | **démontré** |
| `tone_control` indépendant de l'amplitude | **démontré** |
| valeurs ESP8266 observées `8` et `64` | **démontré** |
| séparation inter-générations tone-step / RF-offset / PLL | **nouveau recoupement fort** |
| `tone_control` = step numérique/NCO | **forte inférence, renforcée** |
| `tone_control → Hz` | **ouvert / matériel** |
| hot-update `tone_control` sous tone actif | **ouvert** |
| 2-FSK rapide par RMW bits bas | **candidat principal, non encore fermé** |
| M-FSK par plusieurs `tone_control` | **plausible si la loi est monotone** |
| FSK par hopping PLL | **possible conceptuellement, voie lente/fallback** |

Conclusion TX v0.30 :

> La piste FSK la plus crédible n'est plus le retuning PLL, mais la modulation du
> `tone_control`/tone-step avec toute la chaîne RF maintenue en régime établi. La preuve
> manquante est désormais presque entièrement **physique** : mesurer `tone_control → Δf`
> et la commutation à chaud.

---



# 36AV. Extension FSK RX — preuve d'un estimateur de fréquence Wi‑Fi et chemin autonome à ouvrir — v0.31

Cette passe termine l'étude FSK demandée après la passe TX v0.30. Le résultat principal
est plus fort qu'une simple hypothèse : **l'ESP8266 calcule effectivement un offset de
fréquence reçu** dans son chemin Wi‑Fi normal. Ce qui reste ouvert est l'accès à cette
métrique sans dépendre du décodage d'un paquet 802.11.

## 36AV.1 Preuve ESP8266 spécifique : `freq_offset` existe dans les résultats RX

Le SDK NONOS officiel expose dans `struct bss_info` :

```c
sint16 freq_offset;
sint16 freqcal_val;
```

Le SDK RTOS ESP8266 conserve également :

```c
int16_t freq_offset;   /* frequency offset of AP */
```

La documentation AT associée aux résultats de scan précise en outre :

```text
freq_offset : offset de fréquence de l'AP
unité       : kHz
ppm         : freq_offset / 2.4
```

Sources publiques de recoupement :

```text
ESP8266_NONOS_SDK/include/user_interface.h
ESP8266_RTOS_SDK/components/esp8266/include/esp_wifi_types.h
ESP8266 AT Instruction Set / résultat CWLAP
```

Conséquence certaine :

> le PHY/stack ESP8266 possède un chemin qui produit une estimation signée de l'écart
> de fréquence d'un signal Wi‑Fi reçu.

Cette preuve est **spécifique à l'ESP8266**, et non une extrapolation depuis une génération
ultérieure.

## 36AV.2 Deuxième preuve : boucle de calibration automatique de fréquence

Espressif a fourni dans le NONOS SDK 2.x :

```c
void system_phy_freq_trace_enable(bool enable);
```

Les notes de version la décrivent comme une interface permettant d'activer la
**calibration automatique de l'offset de fréquence**. La fonction a ensuite été retirée des
versions 3.x et le frequency trace désactivé par défaut.

Cela démontre qu'il existe, au-delà du simple champ de résultat de scan, une logique PHY
capable d'utiliser une information de fréquence reçue pour corriger/ suivre la fréquence.

Ce point ne démontre cependant pas :

```text
adresse MMIO brute de la mesure
cadence de mise à jour
latence
validité hors paquet Wi‑Fi reconnu
```

## 36AV.3 Unité publique utile : kHz

La documentation AT donne une information physique absente du corpus statique :

```text
freq_offset est exprimé en kHz
ppm = freq_offset / 2.4
```

À 2,4 GHz, cette relation est cohérente avec :

```text
1 ppm ≈ 2,4 kHz
```

Cette unité concerne le `freq_offset` exposé par le chemin Wi‑Fi/scan. Elle ne doit pas
être appliquée automatiquement à un futur registre interne tant que le producteur exact
du champ n'est pas tracé.

## 36AV.4 Limite décisive : le champ public est attaché à un AP / paquet décodé

`freq_offset` est actuellement exposé dans une structure de résultat de scan/AP. Le chemin
public est donc, au minimum :

```text
signal Wi‑Fi
   ↓
synchronisation / démodulation PHY
   ↓
paquet / beacon reconnu
   ↓
bss_info.freq_offset
```

Pour le projet autonome FSK, la cible est différente :

```text
porteuse FSK arbitraire
   ↓
RF RX / baseband
   ↓
métrique de fréquence libre
   ↓
classification F0/F1/F2...
```

La découverte actuelle prouve que **le silicium sait estimer une fréquence**, mais pas
encore que la valeur intermédiaire est lisible par le LX106 avant le décodage 802.11.

C'est désormais la question RX-FSK prioritaire.

## 36AV.5 `E4` seul n'est pas un discriminateur FSK général

Le chemin OOK/ASK canonique utilise :

```text
0x600005E4 = énergie / puissance
```

Deux tons FSK de même amplitude, tous deux à l'intérieur de la bande RX, peuvent donner :

```text
E4(F0) ≈ E4(F1)
```

Donc :

> `E4` seul ne doit pas être utilisé comme démodulateur FSK canonique.

Il peut toutefois devenir un détecteur de fréquence **indirect** si le filtre RX est
volontairement décentré/retuné et transforme l'écart de fréquence en différence
d'énergie. Cette solution est un fallback expérimental et non le meilleur chemin.

## 36AV.6 Registres de corrélation `0x580..0x58C` : piste secondaire, pas encore un CFO meter

Le corpus démontre :

```text
R0 = 0x60000580
R1 = 0x60000584
R2 = 0x60000588
R3 = 0x6000058C

X = R0 + R3
Y = R1 - R2
corr_power = X² + Y²
```

et `rxiq_get_mis()` exploite des combinaisons de ces registres pour le mismatch
amplitude/phase I/Q.

Il serait tentant d'interpréter directement l'angle :

```text
atan2(Y, X)
```

comme un estimateur de fréquence. **Cette conclusion n'est pas démontrée par le corpus.**

Les registres restent néanmoins une excellente cible expérimentale : sous deux tones FSK
externes de même puissance, il faut enregistrer `R0..R3`, `E4`, `DC_I`, `DC_Q` et vérifier
si une signature stable dépend de la fréquence.

Statut correct :

```text
corrélations utiles au FSK → plausible / à mesurer
corrélations = CFO direct → non démontré
```

## 36AV.7 `get_fm_sar_dout()` est écarté comme démodulateur FSK RX

Le nom `fm_sar` pourrait suggérer une voie FM. Le désassemblage déjà effectué dans ce
document montre au contraire que :

```text
get_fm_sar_dout
   ↓
bloc SAR / feedback TX
   ↓
ets_delay_us(25)
   ↓
deux mesures différentielles
   ↓
get_power_db / txtone_linear_pwr
```

Cette famille est utilisée pour la mesure de puissance du tone TX et est distincte du
loopback RX + `IQ_EST`.

Conclusion :

> `get_fm_sar_dout()` ne doit pas être présenté comme un démodulateur FM/FSK RX sur la
> seule base de son nom.

## 36AV.8 Recoupement inter-générations : fonction RX fréquence explicite

Des ROM Espressif ultérieures exposent explicitement :

```text
rom_phy_get_rx_freq
rom_phy_freq_correct
```

à côté des primitives RX clock, noise-floor, PBUS et calibration.

Ce recoupement est cohérent avec l'existence, sur ESP8266, de `bss_info.freq_offset` et de
`system_phy_freq_trace_enable()`.

Il ne permet toutefois pas d'inventer une adresse ou une ABI équivalente sur ESP8266.

## 36AV.9 Architecture RX 2-FSK visée si la métrique brute CFO est retrouvée

Le chemin idéal devient :

```text
boot + calibration
      ↓
canal / PLL fixes
      ↓
RX RF normal ON
      ↓
RX clock ON
      ↓
PBUS debug / packet RX retiré
      ↓
gain RX fixe
      ↓
lecture métrique fréquence / CFO
      ↓
      ┌───────────────┐
      │ CFO < T → F0  │
      │ CFO > T → F1  │
      └───────────────┘
```

Pour un BFSK symétrique autour d'une fréquence centrale :

```text
F0 = Fc - Δf
F1 = Fc + Δf
```

un préambule connu permettrait de calibrer :

```text
μ0 = centre métrique pour F0
μ1 = centre métrique pour F1
T  = (μ0 + μ1) / 2
```

sans avoir besoin de convertir la mesure interne en Hz.

Pour M-FSK :

```text
μ0, μ1, ... μM-1
```

puis classification au centre le plus proche ou par seuils entre centres adjacents.

## 36AV.10 Fallback si aucun readout CFO libre n'est retrouvé

Un récepteur FSK peut encore être construit par **discrimination d'énergie sélective** :

```text
mesurer énergie avec RX centré près de F0
mesurer énergie avec RX centré près de F1
choisir la branche la plus forte
```

ou, si un réglage de filtre/baseband plus rapide est trouvé, par deux états de filtre.

Mais un retuning PLL par symbole introduirait une latence importante et ferait perdre le
principal avantage du générateur FSK numérique TX. Cette voie doit rester de secours.

## 36AV.11 Recherche statique prioritaire désormais définie

Pour fermer le RX FSK sans paquet Wi‑Fi, l'ordre de recherche devient :

1. tracer le producteur de `bss_info.freq_offset` dans `libnet80211.a` / `libpp.a` ;
2. remonter jusqu'à la primitive PHY/MMIO qui fournit la mesure ;
3. désassembler l'ancien chemin `system_phy_freq_trace_enable()` et identifier sa source ;
4. rechercher toute lecture de cette source dans `libphy.a` indépendamment du packet RX ;
5. comparer avec les registres `IQ_EST` et déterminer si le CFO vient du même bloc ou d'un
   bloc de synchronisation Wi‑Fi distinct ;
6. mesurer la cadence et la latence de la métrique retrouvée ;
7. seulement ensuite définir le baud FSK maximal.

## 36AV.12 Statut RX FSK après v0.31

| Élément | Statut |
|---|---:|
| ESP8266 mesure un offset de fréquence RX | **démontré par API officielle** |
| sortie publique `freq_offset` signée | **démontré** |
| unité publique de `freq_offset` | **kHz** |
| boucle d'auto-calibration de fréquence | **démontrée par SDK 2.x** |
| `E4` seul comme discriminateur FSK | **rejeté comme solution générale** |
| `get_fm_sar_dout` comme démodulateur FM | **rejeté par désassemblage** |
| corrélations IQ_EST comme signature FSK | **piste expérimentale** |
| corrélations IQ_EST comme CFO direct | **non démontré** |
| readout CFO sans paquet 802.11 | **question critique ouverte** |
| adresse/registre brut du CFO ESP8266 | **ouvert** |
| cadence de la mesure CFO | **ouverte** |
| 2-FSK autonome RX | **architecturalement plausible, non fermé** |
| M-FSK autonome RX | **plausible après accès à une métrique monotone** |

Conclusion RX v0.31 :

> L'obstacle n'est plus de savoir si l'ESP8266 sait mesurer un décalage de fréquence :
> **il le sait**. L'obstacle est maintenant de retrouver le producteur PHY brut de
> `freq_offset` et de démontrer qu'il peut être lu en RX autonome sans paquet 802.11.

---



# 36AW. FSK/CFO — séparation mesure RX / correction RF et piste `libpp` — v0.32

Cette passe poursuit le chantier FSK en corrigeant un point de portée de la v0.31 et en
ajoutant deux faits importants issus des documents historiques Espressif.

Le résultat essentiel est qu'il faut désormais distinguer **trois objets** :

```text
A. mesure CFO reçue d'un AP
   → bss_info.freq_offset

B. valeur de calibration / correction
   → freqcal_val / mécanisme de frequency tracking

C. actionneur de correction de fréquence RF/baseband
   → paramètres PHY-init 112/113, correction par pas de 8 kHz
```

Ces objets sont liés fonctionnellement, mais leur identité binaire/MMIO n'est pas encore
démontrée.

## 36AW.1 Correction de portée de la v0.31 : `system_phy_freq_trace_enable()` n'est pas une preuve de démodulation rapide

La v0.31 utilisait correctement `system_phy_freq_trace_enable()` comme preuve qu'une
infrastructure de suivi/correction de fréquence existe. Il faut toutefois préciser sa
portée.

Les notes officielles Espressif indiquent que :

```text
- le frequency trace appartient au workflow de calibration RF ;
- il a été désactivé par défaut dans plusieurs révisions du SDK ;
- l'utilisateur pouvait l'activer dans user_rf_pre_init() ;
- les versions ultérieures ont finalement supprimé l'API.
```

Les documents historiques de configuration recommandent le tracking automatique
principalement pour des applications à large plage de température, par exemple
`-40 °C ... 125 °C`, afin de compenser la dérive de fréquence.

Conclusion corrigée :

> `system_phy_freq_trace_enable()` démontre une **boucle de tracking/correction RF**,
> mais ne démontre ni une cadence symbole, ni un readout CFO libre, ni une primitive
> directement exploitable comme discriminateur FSK rapide.

Le CFO de paquet `bss_info.freq_offset` et la boucle de correction doivent donc rester
deux pistes séparées jusqu'à traçage binaire.

## 36AW.2 Nouveau fait matériel/documentaire : actionneur de correction par pas de 8 kHz

Le guide historique ESP8266 SDK décrit précisément les octets PHY-init `112` et `113`.

### Octet 112

```text
bit0:
    0 → correction de fréquence désactivée
    1 → correction de fréquence autorisée

bit1:
    0 → BBPLL 168 MHz ; corrections positives ET négatives possibles
    1 → BBPLL 160 MHz ; correction positive seulement

bits3:2:
    0 → tracking/correction automatique, correction initiale = 0
    1 → correction forcée = octet 113, sans tracking automatique
    2 → tracking/correction automatique, correction initiale = octet 113
```

### Octet 113

La valeur est documentée comme :

```text
type : int8 signé
unité : pas de 8 kHz
```

Les exemples Espressif montrent notamment une correction absolue de `160 kHz` encodée par :

```text
160 / 8 = 20 pas
```

avec signe/encodage choisi selon le sens de la correction et le mode BBPLL.

**Nouveau fait établi :**

> l'ESP8266 possède donc un **actionneur de correction de fréquence dont la granularité
> de commande documentée est 8 kHz**.

Cette granularité concerne l'actionneur de correction. Elle **ne prouve pas** que
`bss_info.freq_offset` est quantifié par 8 kHz, ni que le registre interne de mesure CFO
utilise cette unité.

## 36AW.3 Conséquence TX FSK : troisième chemin possible, mais non rapide par défaut

Avant v0.32, deux chemins TX étaient considérés :

```text
1. tone_control / tone-step      → candidat FSK rapide
2. retuning RFPLL / channel      → fallback plus lourd
```

La v0.32 ajoute un troisième objet :

```text
3. actionneur de frequency correction → pas documenté 8 kHz
```

Il pourrait conceptuellement déplacer la fréquence porteuse sans changer de canal
802.11 complet.

Mais la seule interface publique documentée ici est configurée dans les données
d'initialisation PHY, et le `frequency trace` est conçu comme mécanisme de
calibration/tracking.

Il n'existe donc encore aucune preuve que l'on puisse faire :

```text
symbole 0 → correction C0
symbole 1 → correction C1
```

à haute cadence.

Statut correct :

```text
actionneur 8 kHz existe                         → démontré
API/config statique PHY-init                    → démontré
registre runtime sous-jacent                    → non identifié
hot-update symbole-par-symbole                  → non démontré
utilité FSK rapide                              → ouverte
```

Le `tone_control` reste donc **le candidat TX rapide principal**.

## 36AW.4 Nouveau recoupement historique : `libpp.a` est matériellement impliqué dans les problèmes de frequency offset

Les notes de version officielles apportent un indice de localisation très utile.

ESP8266 NONOS SDK v1.2.0 introduit :

```text
sint16 freq_offset dans bss_info
```

pour exposer l'offset de fréquence de l'AP.

Puis :

```text
v1.5.3 → "Optimize RF frequency offset"
v1.5.4 → libpp.a version 10.1,
          corrections de problèmes liés au frequency offset et au sleep
```

Cela ne prouve pas que `libpp.a` calcule lui-même le CFO numérique, mais cela démontre
que le chemin low-MAC/PP est **matériellement impliqué dans le transport, l'usage ou la
correction** de cette information.

Conséquence pour le reverse-engineering :

> la recherche du producteur de `bss_info.freq_offset` doit désormais commencer dans
> `libpp.a`, puis remonter vers WDEV/PHY, avant d'examiner `libnet80211.a` comme simple
> couche de copie/présentation éventuelle.

`libnet80211.a` reste à auditer : son rôle exact n'est pas déclassé sans preuve.

## 36AW.5 `RxControl` n'explique pas `freq_offset`

Le DWARF du corpus a déjà fermé le layout public `RxControl` à 12 octets :

```text
RSSI
rate / sig_mode / legacy_length
MCS / HT fields
rxend_state
ampdu_cnt
channel
noise_floor
```

Aucun champ CFO/frequency-offset n'y apparaît.

Cela impose une conclusion architecturale supplémentaire :

> le `freq_offset` exposé par le scan n'est pas simplement un champ du `RxControl`
> public/sniffer déjà reconstruit.

Il doit donc provenir d'au moins une des catégories suivantes :

```text
- autre métadonnée RX interne non exposée par RxControl ;
- registre PHY/baseband lu par PP/WDEV ;
- état global de synchronisation/CFO maintenu par le PHY ;
- résultat calculé/corrigé dans PP à partir d'une métrique PHY plus primitive.
```

Cette réduction de l'espace de recherche est importante pour le RX FSK autonome.

## 36AW.6 Unité de `freq_offset` : encodage kHz, mais précision absolue à ne pas sur-interpréter

Une ancienne documentation AT ESP8266 décrit explicitement :

```text
freq_offset : offset de fréquence de l'AP
unité       : kHz
ppm         : freq_offset / 2.4
```

Le Hardware Matching Guide Espressif apporte toutefois une nuance importante :

```text
AT+CWLAP retourne une valeur d'offset,
mais cette valeur est relative ;
Espressif recommande la comparaison avec un appareil de référence.
```

Conclusion documentaire :

> l'interface historique **encode/expose** `freq_offset` en kHz, mais il ne faut pas
> traiter cette valeur comme une mesure métrologique absolue non calibrée.

Pour une démodulation FSK relative, cette limitation est peu gênante : la séparation
entre deux centres `μF0` et `μF1` compte davantage que l'exactitude absolue en kHz.

## 36AW.7 `freqcal_val` reste volontairement non décodé

Les structures publiques placent :

```c
sint16 freq_offset;
sint16 freqcal_val;
```

côte à côte, et l'AT historique nomme le second champ "frequency calibration".

Cependant aucune source retrouvée dans cette passe ne démontre :

```text
freqcal_val == octet PHY-init 113
freqcal_val == correction appliquée en kHz
freqcal_val == registre CFO brut
```

Il est donc interdit, à ce stade, de les identifier.

`freqcal_val` devient une variable prioritaire à tracer avec `freq_offset` dans le
désassemblage PP/scan.

## 36AW.8 Nouveau modèle fonctionnel à tester

Le modèle de travail devient :

```text
                 RX Wi-Fi/baseband
                        │
                        ▼
                estimateur CFO brut
                        │
             ┌──────────┴──────────┐
             │                     │
             ▼                     ▼
       métadonnée scan       boucle de tracking
       freq_offset           / correction RF
             │                     │
             ▼                     ▼
        bss_info               actionneur
                               pas 8 kHz
```

La séparation exacte des deux branches reste à retrouver.

Pour le projet FSK, la cible est le nœud **avant** la dépendance au paquet :

```text
estimateur CFO brut
        ↓
lecture CPU libre
        ↓
F0 / F1 / ... / FM-FSK
```

## 36AW.9 Recherche binaire prioritaire après v0.32

Ordre recommandé :

1. dans `libpp.a`, rechercher les écritures vers les offsets de structure qui deviendront
   `freq_offset` / `freqcal_val` dans `bss_info` ou une structure intermédiaire ;
2. comparer si possible `libpp` avant/après la révision `10.1` qui corrige le frequency
   offset ;
3. rechercher les appels PP vers `libphy` / `g_phyFuns` au voisinage du traitement RX
   réussi, beacon/probe response et scan ;
4. chercher un état CFO hors `RxControl` dans les structures WDEV/descriptor privées ;
5. tracer séparément le mécanisme de correction automatique vers l'actionneur 8 kHz ;
6. vérifier si la source de mesure CFO est lisible lorsque le packet RX Wi-Fi est retiré
   mais que la chaîne RF/baseband reste active ;
7. mesurer enfin latence, bruit et cadence de cette primitive.

Sans les binaires `.a` correspondants dans l'espace de travail courant, cette passe ne
peut pas identifier honnêtement l'instruction exacte qui remplit `freq_offset`.

## 36AW.10 Statut v0.32

| Élément | Statut |
|---|---:|
| `bss_info.freq_offset` existe sur ESP8266 | **démontré** |
| interface AT historique en kHz | **démontré** |
| exactitude absolue non calibrée | **à nuancer ; valeur documentée comme relative** |
| `freqcal_val` existe | **démontré** |
| sémantique exacte de `freqcal_val` | **ouverte** |
| tracking/correction automatique de fréquence | **démontré** |
| tracking = discriminateur FSK rapide | **non démontré / ne pas conclure** |
| actionneur correction signé | **démontré** |
| granularité documentée de correction | **8 kHz** |
| actionneur 8 kHz hot-update runtime | **non démontré** |
| `libpp.a` impliqué dans le chemin frequency-offset | **démontré par release notes** |
| `libpp.a` producteur direct du CFO | **non démontré** |
| CFO présent dans `RxControl` 12 octets | **non** |
| source PHY brute du CFO | **ouverte** |
| `tone_control` comme TX FSK rapide | **reste candidat principal** |

Conclusion v0.32 :

> Deux sous-systèmes sont maintenant clairement séparés : **mesure CFO reçue** et
> **correction/tracking de fréquence**. La correction possède une granularité documentée
> de 8 kHz, tandis que la mesure de scan est exposée en kHz mais avec une précision
> absolue relative. Le meilleur prochain levier est le désassemblage de `libpp.a` autour
> du traitement RX/scan, car une révision officielle de cette bibliothèque a explicitement
> corrigé des problèmes de frequency offset.

### Sources publiques ajoutées en v0.32

- Espressif, *ESP8266 SDK Getting Started Guide*, section historique
  "Correct Frequency Offset", octets PHY-init 112/113.
- Espressif, *ESP8266EX Hardware Matching Guide* v1.1, section 2.
- Espressif, notes de version NONOS SDK v1.2.0, v1.5.3 et v1.5.4.
- Espressif, NONOS SDK release notes v2.2.1 / v3.0.1 concernant le
  `frequency trace`.
- Espressif, ancienne documentation AT ESP8266 `AT+CWLAP`.

---



# 36AX. FSK/CFO — chronologie de l'estimateur et de la calibration — v0.33

Cette passe exploite la chronologie des SDK Espressif pour séparer plus proprement :

```text
mesure de fréquence reçue
valeur de calibration
mécanisme d'auto-correction
API publique de contrôle du tracking
```

La chronologie apporte une contrainte importante : **l'infrastructure de mesure et de
calibration existait avant l'API publique `system_phy_freq_trace_enable()`**.

## 36AX.1 `freq_offset` et `freqcal_val` existent ensemble dans les headers officiels

Le header officiel `user_interface.h` expose dans `struct bss_info` :

```c
sint16 freq_offset;
sint16 freqcal_val;
```

Ces champs se trouvent directement après :

```text
channel
rssi
authmode
is_hidden
```

et avant les extensions mesh/cipher.

Conclusion certaine :

> `freq_offset` et `freqcal_val` sont deux valeurs distinctes transportées avec les
> informations d'un BSS/AP.

Il ne faut donc pas fusionner les deux concepts dans un unique "CFO".

## 36AX.2 L'ancien AT confirme deux valeurs distinctes

L'ancienne documentation AT ESP8266 décrit les deux éléments :

```text
freq offset
    = frequency offset of AP
    unité = kHz
    ppm = freq_offset / 2.4

freq calibration
    = calibration for frequency offset
```

Cette formulation confirme que :

```text
freq_offset  = mesure/estimation liée au signal reçu
freqcal_val  = valeur liée à la calibration/correction
```

La documentation ne donne cependant pas l'unité ni l'équation de `freqcal_val`.

## 36AX.3 Point chronologique : les champs précèdent l'API publique de frequency trace

Le header d'une base SDK officielle contient déjà :

```c
sint16 freq_offset;
sint16 freqcal_val;
```

alors que la fonction :

```c
system_phy_freq_trace_enable(...)
```

n'est pas présente dans cette interface de base et a été exposée ultérieurement par
patch/révision du SDK.

Conséquence :

> `freqcal_val` n'a pas été créé par `system_phy_freq_trace_enable()`. Le champ et
> l'infrastructure de calibration qu'il reflète sont antérieurs à cette API de contrôle.

Cela renforce le modèle selon lequel `system_phy_freq_trace_enable()` est un **wrapper de
contrôle d'un mécanisme PHY préexistant**, et non l'origine de l'estimateur CFO.

## 36AX.4 L'auto-correction existait elle aussi avant son wrapper public

Les patches officiels autour des SDK 1.5.x documentent la désactivation de l'auto
frequency correction en modifiant les données PHY-init, notamment le paramètre 112.

Le mécanisme était donc déjà implanté dans le PHY avant que l'utilisateur dispose d'une
API dédiée pour l'activer/désactiver dynamiquement.

Le modèle chronologique devient :

```text
estimateur / calibration PHY interne
        │
        ├── métadonnées BSS : freq_offset / freqcal_val
        │
        └── auto-correction commandée par PHY-init
                    │
                    └── wrapper public ajouté plus tard
                        system_phy_freq_trace_enable()
```

## 36AX.5 `libpp.a` devient une cible prioritaire, mais pas encore l'estimateur prouvé

La release officielle NONOS SDK v1.5.4 indique simultanément :

```text
libphy.a → version 972
libpp.a  → version 10.1
```

et précise pour `libpp.a` :

```text
revised issues about frequency offset and sleep mode
```

Cette information est plus forte qu'une simple proximité de nom :

> une révision de `libpp.a` a réellement modifié le comportement lié au frequency offset.

Elle ne prouve toujours pas que l'algorithme CFO est calculé dans PP. Les architectures
encore compatibles avec les faits sont :

```text
A. PHY calcule CFO → PP lit/transporte/utilise la valeur
B. PHY fournit une primitive brute → PP transforme la valeur
C. PHY + PP partagent la boucle de correction/tracking
```

Le fait que le corpus possède un PHY riche en estimateurs, corrélateurs, calibration et
registre RF rend A ou C particulièrement plausibles, mais le désassemblage doit trancher.

## 36AX.6 Absence de CFO dans `RxControl` : implication renforcée

Le `RxControl` public/sniffer déjà reconstruit dans le corpus contient RSSI,
noise-floor, canal et paramètres de paquet, mais pas `freq_offset` ni `freqcal_val`.

En parallèle, `bss_info` possède explicitement ces deux champs.

Le chemin ne peut donc pas être une simple copie :

```text
RxControl → bss_info.freq_offset
```

La valeur doit être obtenue ailleurs, probablement :

```text
PHY/baseband state
      ↓
PP / WDEV receive processing
      ↓
scan / beacon metadata
      ↓
bss_info.freq_offset
```

Cette observation recentre la recherche sur les chemins de réception réussie,
synchronisation PHY et traitement beacon/probe-response.

## 36AX.7 Nuance de documentation : interface historique vs interface actuelle

Les anciennes documentations AT donnent un sens physique à `freq_offset` et
`freqcal_val`.

Les documentations ESP-AT plus récentes conservent les deux champs mais les décrivent
comme :

```text
reserved item
```

Cette évolution documentaire ne signifie pas nécessairement que le matériel ne produit
plus les valeurs. Elle signifie seulement que l'interface moderne ne garantit plus leur
sémantique publique.

Pour ce reverse-engineering, les anciennes versions restent donc importantes pour
reconstruire la signification historique des champs.

## 36AX.8 Conséquence pour le démodulateur FSK autonome

La cible RX FSK ne doit pas viser `bss_info` lui-même, car celui-ci dépend du pipeline de
scan/paquets Wi-Fi.

Il faut remonter **en amont** :

```text
antenne
  ↓
RF RX / ADC
  ↓
synchronisation / estimation de fréquence
  ↓
         ★ métrique brute recherchée ★
  ↓
PP / paquet / scan
  ↓
bss_info.freq_offset
```

Si l'étoile est accessible au LX106 sans paquet Wi-Fi reconnu, alors le démodulateur FSK
peut devenir :

```text
CFO_measure()
  ↓
μF0 / μF1 calibrés
  ↓
seuil ou nearest-centre
  ↓
symbole FSK
```

La conversion absolue en kHz ne serait même pas obligatoire ; une métrique monotone
suffirait.

## 36AX.9 Ce que cette passe ne permet toujours pas d'affirmer

La recherche ne permet toujours pas d'annoncer :

```text
adresse MMIO du CFO brut
fonction ROM/PHY exacte qui produit freq_offset
unité interne du CFO brut
relation CFO brut → freq_offset kHz
relation freqcal_val → actionneur 8 kHz
cadence de mise à jour
latence
baud FSK maximal
```

Ces points restent ouverts.

## 36AX.10 Recherche suivante, désormais très ciblée

Pour fermer le RX FSK, l'ordre de travail devient :

1. obtenir/désassembler les `libpp.a`, `libphy.a`, `libnet80211.a` exacts du corpus ;
2. rechercher dans `libpp.a` les chemins RX/scan/beacon qui écrivent deux `sint16`
   adjacents correspondant à `freq_offset` / `freqcal_val` ;
3. suivre les relocations de ces écritures vers les appels PHY/WDEV immédiatement en
   amont ;
4. identifier toute lecture MMIO/baseband non présente dans `RxControl` ;
5. comparer avec les chemins d'auto-correction de fréquence et avec les primitives
   `rfpll`/frequency-correction ;
6. tester si cette source reste active quand le packet RX Wi-Fi est retiré mais le
   RF/baseband reste actif ;
7. mesurer sa cadence et enfin dériver le baud FSK réel.

Les SHA-256 déjà consignés dans le document restent la meilleure clé pour retrouver une
copie publique exacte des bibliothèques du corpus.

## 36AX.11 Statut v0.33

| Élément | Statut |
|---|---:|
| `freq_offset` et `freqcal_val` sont deux champs distincts | **démontré** |
| `freq_offset` historique exprimé en kHz | **démontré** |
| sémantique publique de `freqcal_val` = calibration frequency offset | **démontré qualitativement** |
| unité/équation de `freqcal_val` | **ouverte** |
| infrastructure calibration antérieure au wrapper public | **fortement démontré par chronologie SDK** |
| auto-correction antérieure au wrapper public | **démontré par patches PHY-init** |
| `libpp.a` impliqué dans les problèmes de frequency offset | **démontré** |
| `libpp.a` = estimateur CFO | **non démontré** |
| CFO contenu dans `RxControl` | **non** |
| source PHY brute en amont de `bss_info` | **ouverte** |
| démodulation FSK autonome via CFO | **reste l'architecture RX cible** |
| baud FSK maximal | **impossible à fermer avant identification/mesure de la primitive** |

Conclusion v0.33 :

> La chronologie élimine une ambiguïté importante : le **mesurage/calibrage de fréquence
> est une infrastructure PHY plus ancienne que son API publique de contrôle**.
> `bss_info.freq_offset` et `freqcal_val` sont des sorties distinctes de cette
> infrastructure au niveau scan. La priorité n'est donc plus de chercher une nouvelle
> fonction "FSK", mais de retrouver la **métrique CFO interne en amont de PP/scan**.

### Sources publiques ajoutées en v0.33

```text
https://github.com/espressif/ESP8266_NONOS_SDK/blob/master/include/user_interface.h
https://www.espressif.com/sites/default/files/documentation/4a-esp8266_at_instruction_set_en.pdf
https://bbs.espressif.com/viewtopic.php?t=2198
https://www.espressif.com/en/products/socs/esp8266ex/resources
```

---



# 36AY. FSK/CFO — le patch frequency-trace ne remplace pas `libphy` — v0.34

La passe v0.34 exploite la composition du patch officiel NONOS SDK v2.0.0 qui introduit
publiquement :

```c
void system_phy_freq_trace_enable(bool enable);
```

La découverte importante n'est pas seulement l'existence de l'API, mais **quelles
bibliothèques le patch remplace**.

## 36AY.1 Composition du patch v2.0.0 du 9 août 2016

Le patch officiel est distribué comme une archive de bibliothèques `.a` à superposer au
SDK v2.0.0.

La recette historique `esp-open-sdk`, qui automatise précisément l'installation de ce
patch officiel, effectue :

```make
unzip ESP8266_NONOS_SDK_V2.0.0_patch_16_08_09.zip

mv libmain.a \
   libnet80211.a \
   libpp.a \
   $(SDK_2_0_0)/lib/
```

Point déterminant :

```text
libphy.a n'est pas remplacé par ce patch
```

alors que la release de base v2.0.0 utilise déjà :

```text
libphy.a version 1055
```

## 36AY.2 Conséquence architecturale

Le patch ajoute publiquement le contrôle :

```text
system_phy_freq_trace_enable()
```

sans installer un nouveau `libphy.a`.

Plusieurs scénarios restent compatibles :

```text
A. le moteur de tracking/correction existe déjà dans libphy 1055
   et le patch ajoute uniquement un wrapper/contrôle au-dessus ;

B. les couches supérieures pilotent directement un état ou registre
   déjà supporté par le PHY/baseband ;

C. une partie du mécanisme était déjà accessible dans libphy,
   et le patch modifie PP/net80211/main pour l'activer, le nourrir
   ou l'intégrer au workflow de connexion.
```

En revanche, le scénario :

```text
"le patch a dû ajouter un nouveau estimateur CFO dans libphy"
```

n'est pas soutenu par la composition du patch, puisque le binaire PHY n'est pas remplacé.

Conclusion de haute confiance :

> **l'infrastructure basse nécessaire au frequency trace est antérieure au patch
> v2.0.0_20160809.**

Ceci concorde avec les patches 1.5.x qui pouvaient déjà couper l'auto frequency
correction par l'octet PHY-init 112.

## 36AY.3 Attention : on ne peut pas attribuer chaque modification à une bibliothèque précise

Le même patch corrige également un problème de connexion lente.

La présence simultanée de :

```text
libmain.a
libnet80211.a
libpp.a
```

ne permet donc pas d'affirmer sans diff binaire :

```text
libpp.a       = implémentation de system_phy_freq_trace_enable
libnet80211.a = producteur de freq_offset
libmain.a     = wrapper
```

Ces attributions restent ouvertes.

Le seul fait sûr est que **le patch entier n'a pas besoin d'un nouveau libphy**.

## 36AY.4 Ce que cela change pour la recherche du CFO brut

La stratégie devient plus précise.

Au lieu de chercher uniquement une fonction nouvellement ajoutée au PHY, il faut
rechercher une primitive **déjà présente dans le corpus PHY/ROM** et consommée ou activée
par PP/net80211/main.

Candidats fonctionnels à auditer autour de la réception :

```text
- états de synchronisation PHY ;
- estimateur de frequency offset interne ;
- paramètres de correction RF/baseband ;
- champs privés du descripteur RX hors RxControl public ;
- appels PP → PHY lors d'un beacon/probe reçu ;
- état global de tracking associé au canal courant.
```

La v0.29 a déjà montré que le `RxControl` public n'embarque pas le CFO. Le producteur
recherché est donc nécessairement ailleurs.

## 36AY.5 Relation avec `libpp.a` v10.1

La release officielle v1.5.4 indique :

```text
libphy.a version 972
libpp.a  version 10.1
```

avec la note :

```text
libpp.a : revised issues about frequency offset and sleep mode
```

Puis quelques mois plus tard, le patch v2.0.0 de frequency trace remplace à nouveau
`libpp.a` avec `libmain.a` et `libnet80211.a`, sans remplacer `libphy.a`.

Pris ensemble, ces faits renforcent le modèle :

```text
PHY/baseband
    ↓ mesure / primitive basse
PP
    ↓ utilisation / état / correction
net80211 / scan
    ↓
bss_info.freq_offset / freqcal_val
```

Le sens exact des flèches et l'éventuel retour de correction vers le PHY restent à
désassembler.

## 36AY.6 Indice supplémentaire : l'évolution ultérieure remet `libphy` dans la boucle

Les releases ultérieures optimisent de nouveau le workflow de calibration de fréquence
tout en mettant à jour `libphy.a` (par exemple vers 1134_0).

Cela indique que la fonctionnalité globale est effectivement **trans-couche** :

```text
couches système / PP
        ↕
PHY calibration / correction
        ↕
RF/baseband
```

Le fait que le patch 2016 n'ait pas remplacé `libphy` ne signifie donc pas que le PHY est
étranger au mécanisme ; il signifie surtout que le support PHY de base existait déjà.

## 36AY.7 Nouvelle priorité de diff binaire

La comparaison idéale est désormais :

```text
SDK 2.0.0 base
    vs
SDK 2.0.0 + patch 20160809
```

pour :

```text
libmain.a
libnet80211.a
libpp.a
```

et **pas** `libphy.a`, qui est inchangé dans ce patch.

Un diff au niveau des objets d'archive permettrait de :

1. identifier quels `.o` ont changé ;
2. trouver dans quelle bibliothèque apparaît
   `system_phy_freq_trace_enable` ;
3. isoler les nouveaux appels externes vers PHY/ROM ;
4. repérer les nouvelles variables globales de tracking ;
5. relier éventuellement ces changements à `freq_offset` / `freqcal_val`.

C'est probablement le chemin statique le plus court vers le nœud de mesure/correction.

## 36AY.8 Limite actuelle de la présente passe

L'archive historique officielle est toujours publiée, et son contenu est servi par le
forum Espressif. Dans l'environnement de recherche courant, le flux binaire de
l'attachement n'a pas pu être matérialisé localement de manière fiable pour lancer
`ar`, `nm`, `objdump` et un diff objet-par-objet.

Il serait incorrect de prétendre avoir réalisé ce diff.

La composition de l'archive est néanmoins recoupée par la recette `esp-open-sdk`, conçue
pour appliquer ce patch précis.

## 36AY.9 Conséquence pour le projet FSK

### TX

Aucun changement du verdict :

```text
tone_control / tone-step = candidat rapide principal
frequency-correction 8 kHz = actionneur intéressant mais hot-update non démontré
PLL hopping = fallback lent
```

### RX

Le modèle recherché devient plus crédible :

```text
estimateur CFO déjà présent avant le patch
        ↓
primitive PHY / état baseband
        ↓
PP / workflow frequency trace
        ↓
freq_offset + freqcal_val
```

L'objectif reste de couper la chaîne **avant** la dépendance au paquet/scan afin de lire
une métrique de fréquence sur un tone FSK arbitraire.

## 36AY.10 Statut v0.34

| Élément | Statut |
|---|---:|
| patch v2.0.0 publie `system_phy_freq_trace_enable` | **démontré** |
| patch remplace `libmain.a` | **recoupé** |
| patch remplace `libnet80211.a` | **recoupé** |
| patch remplace `libpp.a` | **recoupé** |
| patch remplace `libphy.a` | **non** |
| support PHY nécessaire antérieur au patch | **fortement établi** |
| bibliothèque exacte exportant le wrapper | **ouverte** |
| fonction PHY/ROM exacte consommée par le wrapper | **ouverte** |
| diff objet base↔patch réalisé | **non, à faire dès que les archives sont matérialisables** |
| source CFO brute autonome | **ouverte** |

Conclusion v0.34 :

> Le patch qui rend le frequency trace publiquement contrôlable **ne met pas à jour
> `libphy.a`**. L'estimateur/correcteur bas niveau nécessaire existait donc déjà ou était
> déjà pilotable dans le PHY/baseband de la release de base. La recherche doit maintenant
> comparer `libmain`, `libpp` et `libnet80211` avant/après patch pour retrouver le wrapper
> et remonter vers cette primitive PHY préexistante.

### Sources publiques ajoutées en v0.34

```text
https://bbs.espressif.com/viewtopic.php?t=2529
https://github.com/pfalcon/esp-open-sdk/blob/master/Makefile
https://www.espressif.com/en/products/socs/esp8266ex/resources
```

---



# 36AZ. FSK — découverte ESP8266 de `set_rf_freq_offset` et `phy_freq_offset` — v0.35

Cette passe apporte une nouveauté importante qui concerne **directement l'ESP8266**,
et non plus seulement les générations Espressif suivantes.

Deux sources indépendantes issues de l'ancien écosystème SDK ESP8266 publient la table
des symboles de `libphy.a`. Elles montrent explicitement :

```text
phy_chip_v6_ana.o:
    set_rf_freq_offset
    chip_v6_set_chan_offset
    ram_rfpll_set_freq
    ram_set_channel_freq

phy_chip_v6.o:
    phy_freq_offset
```

Sources de recoupement :

```text
https://41j.com/blog/2015/01/esp8266-sdk-library-symbols/
https://git.neulandlabor.de/j3d1/esp-open-rtos/
```

Ces noms n'avaient pas encore été intégrés au présent document.

## 36AZ.1 Fait nouveau : un actionneur runtime d'offset RF existe dans `libphy.a`

Dans le dump `nm` historique :

```text
phy_chip_v6_ana.o:
    0x00000220  T set_rf_freq_offset
    0x000008f8  T ram_rfpll_set_freq
    0x00000a1c  T ram_set_channel_freq
    0x00000e38  T chip_v6_set_chan_offset
```

La coexistence de ces fonctions dans le même objet PHY démontre au minimum une séparation
logicielle entre :

```text
offset fin de fréquence
réglage PLL RF
réglage canal/fréquence
wrapper de canal avec offset
```

Conclusion forte :

> sur ESP8266, `set_rf_freq_offset()` est une primitive distincte du changement de canal
> et du réglage explicite de la RFPLL.

Cela renforce considérablement l'architecture déjà déduite par recoupement
inter-générations dans les versions v0.30-v0.34.

## 36AZ.2 Nouveau symbole d'état : `phy_freq_offset`

Le même `libphy.a` expose :

```text
phy_chip_v6.o:
    B phy_freq_offset
```

Un ancien link-map public place :

```text
0x3ffebe18  phy_freq_offset
0x3ffebe1a  do_pwctrl_flag
```

Dans ce build précis, le symbole suivant commence donc exactement **2 octets** après
`phy_freq_offset`.

Cela constitue un indice fort que l'état `phy_freq_offset` occupait 16 bits dans cette
version du PHY.

Il serait toutefois incorrect d'écrire dès maintenant :

```text
phy_freq_offset == bss_info.freq_offset
```

ou :

```text
phy_freq_offset == freqcal_val
```

La taille et le nom sont compatibles avec plusieurs rôles :

```text
correction actuellement appliquée
offset mesuré/filtré
consigne de calibration
état intermédiaire du frequency tracking
```

Le sens exact reste à tracer.

## 36AZ.3 `chip_v6_set_chan_offset()` devient un wrapper prioritaire à désassembler

La présence simultanée :

```text
set_rf_freq_offset()
chip_v6_set_chan_offset()
chip_v6_set_chan()
ram_set_channel_freq()
ram_rfpll_set_freq()
```

suggère un découpage très utile pour le projet FSK.

Hypothèse de travail, à confirmer :

```text
chip_v6_set_chan_offset()
        │
        ├── fixe / conserve le canal
        └── applique un offset fin via set_rf_freq_offset()
```

Cette relation n'est pas démontrée par les seuls symboles ; elle doit être obtenue par
désassemblage ou relocations.

## 36AZ.4 Relation possible avec l'actionneur documenté en pas de 8 kHz

La documentation Espressif des paramètres PHY-init 112/113 démontre séparément :

```text
frequency correction enable / auto / forced
valeur de correction : signed int8
pas documenté       : 8 kHz
```

L'open-source `esp-open-rtos` décrit également le champ de l'octet 113 comme :

```text
force_freq_offset
1 unité = 8 kHz
plage int8 ≈ ±1016 kHz
```

Il est **très plausible** que `set_rf_freq_offset()` participe au chemin runtime qui
applique cette correction.

Mais aucun élément récupéré dans cette passe ne démontre encore :

```text
signature C de set_rf_freq_offset()
argument = int8 de l'octet 113
unité interne = 8 kHz
set_rf_freq_offset(x) écrit directement x * 8 kHz
```

Ces égalités restent donc des hypothèses à tester, pas des faits.

## 36AZ.5 Conséquence TX FSK : un second candidat rapide devient sérieux

Jusqu'à présent, le meilleur candidat TX FSK rapide était :

```text
tone_control / tone-step
```

La découverte de `set_rf_freq_offset()` ajoute un deuxième candidat de haut niveau :

```text
A. tone_control
   → générateur numérique de tone
   → très rapide en théorie
   → loi step→Hz encore inconnue

B. set_rf_freq_offset
   → actionneur d'offset RF explicitement nommé
   → finalité fréquence certaine
   → granularité/signature/hot-update encore inconnues
```

Le chemin B est particulièrement intéressant parce qu'il pourrait offrir une déviation
de fréquence autour d'un canal RF déjà verrouillé, sans réexécuter une sélection complète
de canal.

Architecture candidate :

```text
canal / RFPLL      fixes
TX RF              actif
TX clock           active
tone / porteuse    active
offset fin         C0 ↔ C1
```

pour obtenir :

```text
F0 = Fc + Δf(C0)
F1 = Fc + Δf(C1)
```

Cependant, tant qu'un changement à chaud n'a pas été démontré :

> `set_rf_freq_offset()` ne doit pas encore remplacer `tone_control` comme chemin FSK
> canonique.

## 36AZ.6 Avantage potentiel : fréquence absolue mieux contrôlable que `tone_control`

Si le chemin `set_rf_freq_offset` se révèle être le même actionneur que celui documenté
par les octets 112/113, on disposerait potentiellement d'une quantification physique
connue :

```text
1 pas ≈ 8 kHz
```

Cela permettrait théoriquement des déviations de type :

```text
±8 kHz
±16 kHz
±24 kHz
...
```

sans avoir d'abord à dériver empiriquement la loi `tone_control → Hz`.

Cette conséquence est **conditionnelle** : la correspondance entre la fonction runtime et
l'octet 113 n'est pas encore prouvée.

## 36AZ.7 Conséquence RX : ne pas confondre actionneur et estimateur

La découverte de `phy_freq_offset` peut être trompeuse.

Le RX public expose déjà :

```text
bss_info.freq_offset
```

et le RTOS SDK moderne conserve encore :

```c
int16_t freq_offset;  /* frequency offset of AP */
```

dans `wifi_ap_record_t`.

En revanche, `freqcal_val` a disparu de cette structure moderne.

Cela suggère, sans le prouver, que :

```text
freq_offset
    = donnée RX fondamentale et durable

freqcal_val / phy_freq_offset
    = état de calibration / correction plus dépendant de l'implémentation
```

Pour le démodulateur FSK RX, la priorité reste donc :

> retrouver le producteur PHY de la **mesure reçue** qui alimente `freq_offset`,
> plutôt que lire aveuglément `phy_freq_offset`.

## 36AZ.8 Convergence avec les PHY Espressif suivants

Les ROM des générations suivantes exposent explicitement des primitives séparées :

```text
rom_phy_get_rx_freq
rom_phy_freq_correct
rom_set_rf_freq_offset
rom_rfpll_set_freq
rom_set_channel_freq
```

Cette nomenclature est remarquablement cohérente avec les symboles ESP8266 historiques :

```text
[mesure RX]              → public freq_offset, producteur encore inconnu
[correction/état]        → phy_freq_offset
[actionneur offset RF]   → set_rf_freq_offset
[PLL]                    → ram_rfpll_set_freq
[canal]                  → ram_set_channel_freq
```

Le tableau de droite est un **modèle de correspondance fonctionnelle**, pas une preuve
d'identité ABI entre générations.

## 36AZ.9 Correction méthodologique : ne pas dater le corpus par la taille brute des `.a`

Une tentative de comparaison des tailles GitHub a montré de fortes différences entre
les archives officielles taguées et celles du corpus.

Cette méthode ne doit pas être utilisée comme preuve de version.

Le document maître montre que plusieurs objets du corpus contiennent des informations
de debug/DWARF. Une archive non stripée peut être beaucoup plus volumineuse qu'une archive
fonctionnellement équivalente distribuée sans ces sections.

La datation correcte doit donc utiliser :

```text
SHA-256 exact
liste d'objets ar
symboles
versions internes / chaînes
relocations
code machine
```

et non la taille brute seule.

## 36AZ.10 Recherche binaire prioritaire après v0.35

L'ordre devient :

1. retrouver `set_rf_freq_offset` dans le `libphy.a` exact du corpus ;
2. récupérer son prototype via DWARF si le type est présent ;
3. désassembler `set_rf_freq_offset` ;
4. désassembler `chip_v6_set_chan_offset` et relever ses appels ;
5. relever toutes les références à `phy_freq_offset` ;
6. déterminer qui écrit cette variable et qui la lit ;
7. vérifier la relation éventuelle avec l'octet PHY-init 113 ;
8. mesurer si l'actionneur accepte des changements répétés sans relock complet de la PLL ;
9. mesurer `argument → Δf RF` ;
10. en parallèle, continuer à tracer le producteur RX de `bss_info.freq_offset`.

## 36AZ.11 Test matériel minimal proposé

Dans un environnement RF contrôlé et atténué :

```text
canal fixe
RFPLL stable
TX RF stable
puissance stable
```

Puis :

```text
offset = C0
mesurer f0

offset = C1
mesurer f1
```

sans changer de canal.

À extraire :

```text
Δf par pas
linéarité
signe
plage
latence de settling
glitches
phase
éventuel relock PLL
```

Si la pente mesurée vaut approximativement :

```text
8 kHz / unité
```

la connexion entre `set_rf_freq_offset()` et l'actionneur PHY-init 113 sera fortement
renforcée.

## 36AZ.12 Statut v0.35

| Élément | Statut |
|---|---:|
| symbole ESP8266 `set_rf_freq_offset` | **démontré par deux jeux de symboles historiques** |
| symbole ESP8266 `phy_freq_offset` | **démontré** |
| `phy_freq_offset` occupe 2 octets dans un ancien build | **fortement établi par link-map** |
| `chip_v6_set_chan_offset` existe | **démontré** |
| offset fin distinct de `rfpll_set_freq` | **fortement établi au niveau API interne** |
| offset fin distinct de `set_channel_freq` | **fortement établi au niveau API interne** |
| signature de `set_rf_freq_offset` | **ouverte** |
| unité runtime de `set_rf_freq_offset` | **ouverte** |
| lien avec pas documentaire 8 kHz | **fortement plausible, non démontré** |
| hot-update sans relock | **ouvert** |
| utilité TX FSK | **candidat majeur supplémentaire** |
| `phy_freq_offset == bss_info.freq_offset` | **non démontré** |
| source brute du CFO RX | **toujours ouverte** |

Conclusion v0.35 :

> L'ESP8266 possède explicitement, dans son `libphy`, un **actionneur nommé
> `set_rf_freq_offset`** et un état global **`phy_freq_offset`**, séparés des primitives
> de canal et de RFPLL. C'est la première preuve ESP8266 directe d'un chemin runtime
> dédié à l'offset fin de fréquence. Pour le TX FSK, ce chemin devient presque aussi
> important que `tone_control`. Pour le RX FSK, il faut encore distinguer strictement
> la mesure CFO reçue de la correction appliquée.

### Sources publiques ajoutées en v0.35

```text
https://41j.com/blog/2015/01/esp8266-sdk-library-symbols/
https://git.neulandlabor.de/j3d1/esp-open-rtos/
https://raw.githubusercontent.com/espressif/ESP8266_NONOS_SDK/v2.0.0/include/user_interface.h
https://raw.githubusercontent.com/espressif/ESP8266_RTOS_SDK/master/components/esp8266/include/esp_wifi_types.h
```

---



# 36BA. FSK/CFO — chaîne PP → scan/hostap de calibration de fréquence — v0.36

Cette passe apporte le premier **pont nominal explicite** entre le PHY, `libpp.a` et
`libnet80211.a` pour la gestion de l'offset/calibration de fréquence.

Un ancien link-map ESP8266, produit lors du link d'un firmware SDK complet, expose
simultanément les symboles suivants :

```text
libphy.a(phy_chip_v6.o)
    phy_freq_offset

libpp.a(pp.o)
    HighestFreqOffsetInOneChk

libnet80211.a(ieee80211_hostap.o)
    ApFreqCalTimer

libnet80211.a(ieee80211_scan.o)
    FreqCalCntForScan
    TestStaFreqCalValInput
```

Ces symboles sont présents dans **le même build lié**, ce qui permet de reconstruire
beaucoup plus précisément la répartition fonctionnelle de la calibration de fréquence.

Source de recoupement principale :

```text
https://pastebin.com/egnFgqD9
```

Le build est historique et ne doit pas être supposé bit-identique au corpus actuel.
Les adresses absolues ci-dessous servent donc uniquement de preuve de présence, de
voisinage et d'appartenance aux objets/bibliothèques.

## 36BA.1 Couche PHY : `phy_freq_offset`

Le link-map place dans :

```text
libphy.a(phy_chip_v6.o)
```

la variable :

```text
0x3ffebe18  phy_freq_offset
0x3ffebe1a  do_pwctrl_flag
```

Comme déjà noté en v0.35, le symbole suivant commence deux octets plus loin. Dans ce
build précis, cela est fortement compatible avec un état `phy_freq_offset` de 16 bits.

Rôle exact toujours ouvert :

```text
mesure CFO ?
valeur filtrée ?
consigne/correction ?
état courant de calibration ?
```

Il reste interdit de l'identifier directement à `bss_info.freq_offset`.

## 36BA.2 Couche PP : `HighestFreqOffsetInOneChk`

Le même link-map expose, dans :

```text
libpp.a(pp.o)
```

le symbole :

```text
0x3ffec290  HighestFreqOffsetInOneChk
```

Le nom est beaucoup plus informatif que les indices précédents.

Interprétation minimale sûre :

> `pp.o` maintient un état explicitement associé au **plus grand frequency offset
> observé dans un "check"**.

Le sens exact de `Chk` n'est pas encore démontré :

```text
un paquet ?
une fenêtre temporelle ?
un lot de beacons ?
une passe de calibration ?
une itération de tracking ?
```

De même, le nom ne démontre pas si la valeur est :

```text
signée ou absolue
en Hz/kHz
en unité PHY interne
mesurée directement ou déjà transformée
```

Mais il établit de façon beaucoup plus forte que les release notes seules que
**PP participe directement au traitement d'une grandeur nommée frequency offset**.

## 36BA.3 Couche scan : deux états de calibration explicites

Dans :

```text
libnet80211.a(ieee80211_scan.o)
```

le link-map donne :

```text
0x3fff1248  gScanStruct
...
0x3fff12fc  FreqCalCntForScan
0x3fff12fe  TestStaFreqCalValInput
0x3fff1300  auth_type
```

La séparation exacte de deux octets entre :

```text
FreqCalCntForScan
TestStaFreqCalValInput
auth_type
```

est fortement compatible, dans ce build, avec deux objets de 16 bits pour les deux
premiers symboles.

Cela ne constitue pas une preuve de type C exacte, mais c'est cohérent avec les
structures publiques qui utilisent des `sint16` pour :

```text
freq_offset
freqcal_val
```

Il serait toutefois incorrect d'identifier directement :

```text
TestStaFreqCalValInput == bss_info.freqcal_val
```

sans désassemblage de `ieee80211_scan.o`.

## 36BA.4 Couche AP : `ApFreqCalTimer`

Dans :

```text
libnet80211.a(ieee80211_hostap.o)
```

le link-map expose :

```text
0x3fff1230  ApFreqCalTimer
```

Le simple fait qu'un état nommé **frequency calibration timer** existe en mode host-AP
montre que la calibration de fréquence ne se limite pas à la construction d'un résultat
de scan STA.

La mécanique est donc au moins partiellement **périodique/temporisée** dans certains
modes du protocole.

Cela concorde avec l'interprétation déjà établie :

> le frequency tracking standard du SDK est une boucle lente de calibration/coexistence,
> et non nécessairement un discriminateur FSK symbole-par-symbole.

## 36BA.5 Architecture trans-couche désormais beaucoup mieux contrainte

Les faits disponibles permettent maintenant le modèle suivant :

```text
                    RF / baseband
                         │
                         ▼
                estimateur fréquence
                         │
                ┌────────┴────────┐
                │                 │
                ▼                 ▼
        état PHY interne      actionneur RF
        phy_freq_offset       set_rf_freq_offset()
                │                 ▲
                ▼                 │
             libpp.a              │
 HighestFreqOffsetInOneChk        │
                │                 │
                ▼                 │
          libnet80211             │
        ┌───────┴────────┐        │
        ▼                ▼        │
 ieee80211_scan.o   hostap.o      │
 FreqCalCnt...      ApFreqCalTimer│
 TestSta...               │        │
        │                 └────────┘
        ▼
 bss_info.freq_offset
 bss_info.freqcal_val
```

**Attention :** les flèches précises entre variables restent hypothétiques. Le schéma
résume les couches où les états sont maintenant démontrés, pas les appels exacts.

## 36BA.6 Pourquoi cette découverte est importante pour le RX FSK

Avant cette passe, la difficulté principale était de savoir où chercher entre :

```text
PHY
PP
net80211
scan
```

Le link-map réduit fortement l'espace de recherche.

Le candidat de passage le plus intéressant devient :

```text
PHY raw CFO
    ↓
PP: HighestFreqOffsetInOneChk
    ↓
scan/calibration
```

Pour un RX FSK autonome, l'objectif est de couper **avant** :

```text
ieee80211_scan.o
bss_info
beacon reconnu
```

La question décisive devient donc :

> **quelle valeur `pp.o` lit-il pour mettre à jour
> `HighestFreqOffsetInOneChk` ?**

Si cette source est un registre PHY/baseband ou une métadonnée de descripteur disponible
avant le décodage complet du paquet, elle pourrait devenir le discriminateur FSK
recherché.

## 36BA.7 Objets binaires prioritaires — ordre révisé

La recherche statique doit désormais viser exactement :

```text
1. libpp.a(pp.o)
       → xrefs HighestFreqOffsetInOneChk

2. libnet80211.a(ieee80211_scan.o)
       → xrefs FreqCalCntForScan
       → xrefs TestStaFreqCalValInput

3. libnet80211.a(ieee80211_hostap.o)
       → xrefs ApFreqCalTimer

4. libphy.a(phy_chip_v6.o)
       → xrefs phy_freq_offset

5. libphy.a(phy_chip_v6_ana.o)
       → set_rf_freq_offset
       → chip_v6_set_chan_offset
```

Pour chaque objet :

```text
- obtenir le type DWARF si disponible ;
- relever toutes les relocations vers le symbole ;
- désassembler les fonctions qui lisent/écrivent le symbole ;
- relever les appels externes ;
- identifier les MMIO lus juste avant les traitements frequency-offset.
```

Cette liste est nettement plus ciblée que l'audit global PP/net80211 proposé auparavant.

## 36BA.8 Hypothèse de pipeline à tester — sans la promouvoir en fait

Une hypothèse maintenant plausible est :

```text
RX packet/sync hardware
      ↓
offset fréquentiel instantané
      ↓
PP retient / borne / agrège les mesures
      ↓
HighestFreqOffsetInOneChk
      ↓
scan sélectionne ou moyenne une valeur
      ↓
freq_offset / freqcal_val
      ↓
correction lente via set_rf_freq_offset
```

Mais il manque encore les éléments critiques :

```text
fonction PP exacte
source de la mesure
unité interne
signe
filtrage
fréquence de mise à jour
dépendance au packet-valid
```

Aucun de ces détails ne doit être considéré fermé.

## 36BA.9 Conséquence TX

Cette passe concerne surtout le RX et la calibration, mais elle renforce indirectement
la séparation :

```text
mesure de fréquence
        ≠
actionneur de fréquence
```

Pour le TX FSK, les deux voies restent :

```text
A. tone_control
   - potentiel de commutation très rapide
   - loi vers Hz encore à mesurer

B. set_rf_freq_offset
   - finalité fréquence explicitement nommée
   - lien probable avec la correction fine
   - hot-update/latence encore à démontrer
```

Aucune nouvelle preuve ne permet encore de fixer un baud TX maximal.

## 36BA.10 Limite de compatibilité avec le corpus actuel

Le link-map utilisé ici correspond à un ancien SDK/build de 2015.

Le corpus actuel du projet contient des bibliothèques différentes en taille et avec des
sections DWARF supplémentaires. On ne doit donc **pas** recopier les adresses absolues :

```text
0x3ffec290
0x3fff1230
0x3fff12fc
0x3fff12fe
```

dans le firmware final.

La découverte utile est :

```text
nom du symbole
bibliothèque
objet .o
relation de voisinage
existence historique de la chaîne
```

Ces symboles doivent être recherchés à nouveau dans les `.a` exacts du corpus avant
toute utilisation d'adresse.

## 36BA.11 Statut v0.36

| Élément | Statut |
|---|---:|
| `HighestFreqOffsetInOneChk` dans `libpp.a(pp.o)` | **démontré dans un build historique** |
| PP possède un état explicitement frequency-offset | **démontré** |
| `FreqCalCntForScan` dans `ieee80211_scan.o` | **démontré** |
| `TestStaFreqCalValInput` dans `ieee80211_scan.o` | **démontré** |
| `ApFreqCalTimer` dans `ieee80211_hostap.o` | **démontré** |
| calibration fréquence trans-couche PHY→PP→net80211 | **très fortement établie** |
| PP calcule lui-même le CFO brut | **non démontré** |
| `HighestFreqOffsetInOneChk` = CFO instantané | **non démontré** |
| `TestStaFreqCalValInput` = `bss_info.freqcal_val` | **non démontré** |
| source MMIO/PHY du CFO | **toujours ouverte** |
| dépendance à un paquet Wi‑Fi valide | **ouverte** |
| possibilité RX FSK autonome | **renforcée, mais non fermée** |

Conclusion v0.36 :

> Le frequency offset n'est plus une notion abstraite perdue entre PHY et scan :
> un ancien build ESP8266 montre une chaîne d'états nommés répartis dans
> **PHY (`phy_freq_offset`) → PP (`HighestFreqOffsetInOneChk`) →
> net80211 scan/hostap (`FreqCalCntForScan`, `TestStaFreqCalValInput`,
> `ApFreqCalTimer`)**. La prochaine cible décisive est maintenant unique :
> désassembler les xrefs de `HighestFreqOffsetInOneChk` dans le `pp.o` exact du corpus
> afin d'identifier **la source basse du CFO**.

### Sources publiques ajoutées en v0.36

```text
https://pastebin.com/egnFgqD9
https://41j.com/blog/2015/01/esp8266-sdk-library-symbols/
```

---



# 36BB. FSK/CFO — `DefFreqCalTimerCB` et `ppPeocessRxPktHdr` — v0.37

Cette passe resserre encore le chemin RX de la calibration de fréquence en combinant :

```text
- un link-map statique ancien ;
- des dumps de symboles SDK ;
- plusieurs stack traces décodées provenant de firmwares ESP8266 réels ;
- les symboles déjà identifiés dans PP, scan et PHY.
```

Deux fonctions deviennent prioritaires :

```text
ppPeocessRxPktHdr
DefFreqCalTimerCB
```

Le nom `ppPeocessRxPktHdr` contient la faute `Peocess` dans les symboles historiques ;
elle est conservée ici pour permettre les recherches exactes.

## 36BB.1 `ppPeocessRxPktHdr` est un traitement RX substantiel de `pp.o`

Un ancien link-map place dans :

```text
libpp.a(pp.o)
```

la séquence :

```text
0x4024e8d8  pp_try_enable_idle_timer
0x4024e910  ppPeocessRxPktHdr
0x4024ebfc  ppTxPkt
0x4024ed10  ppProcessWaitQ
0x4024ed58  ppRecycleRxPkt
```

Dans ce build, la distance jusqu'au symbole suivant est :

```text
0x4024ebfc - 0x4024e910 = 0x2EC = 748 octets
```

Il s'agit d'un **span lié** entre deux symboles, pas d'une garantie que les 748 octets
constituent exactement le corps sémantique de la fonction sans padding/relaxation.

Néanmoins, cela montre que `ppPeocessRxPktHdr` n'est pas un trivial wrapper de quelques
instructions : c'est un traitement RX non négligeable dans `pp.o`.

## 36BB.2 La fonction persiste dans plusieurs générations de SDK

Des stack traces décodées avec les symboles SDK montrent encore :

```text
ppPeocessRxPktHdr
```

dans des firmwares basés sur différentes versions du core/SDK ESP8266.

Elle apparaît notamment avec :

```text
lmacRxDone
wDev_ProcessFiq
ppEnqueueRxq
sta_input
scan_parse_beacon
```

selon les traces.

Cette persistance renforce son intérêt comme point stable du pipeline RX.

## 36BB.3 Nouveau symbole clé : `DefFreqCalTimerCB`

Plusieurs traces décodées exposent explicitement :

```text
DefFreqCalTimerCB
```

Exemples historiques :

```text
wDev_ProcessFiq
DefFreqCalTimerCB
pp_tx_idle_timeout
...
ppPeocessRxPktHdr
```

et, dans une autre génération :

```text
scan_parse_beacon
DefFreqCalTimerCB
ieee80211_parse_beacon
sta_input
...
ppPeocessRxPktHdr
```

Le nom est fortement compatible avec :

```text
Def = default
FreqCal = frequency calibration
TimerCB = timer callback
```

La présence de ce symbole concorde avec les autres états déjà démontrés :

```text
ApFreqCalTimer
FreqCalCntForScan
TestStaFreqCalValInput
HighestFreqOffsetInOneChk
phy_freq_offset
```

## 36BB.4 Limite méthodologique essentielle : un stack dump n'est pas un call graph

Les stack traces utilisées ici contiennent les mots interprétés comme adresses de
fonctions présents sur la pile lors d'un crash.

Elles démontrent utilement :

```text
- que le symbole existe dans cette génération du firmware ;
- qu'il est lié dans le runtime ;
- que plusieurs fonctions RX/calibration coexistent dans le même contexte système.
```

Elles ne démontrent **pas** à elles seules :

```text
A appelle directement B
B retourne vers C
ordre chronologique exact de tous les symboles affichés
```

Certaines valeurs peuvent être des return addresses, des frames imbriquées ou même des
mots résiduels correspondant accidentellement à du code.

Conclusion prudente :

> les traces renforcent très fortement la proximité fonctionnelle du RX et de la
> calibration de fréquence, mais le graphe d'appels exact doit venir du désassemblage.

## 36BB.5 Deux contextes de timer de calibration deviennent plausibles

Le link-map ancien montre :

```text
libnet80211.a(ieee80211_hostap.o):
    ApFreqCalTimer
```

Les firmwares plus récents exposent en code :

```text
DefFreqCalTimerCB
```

Il est donc plausible qu'il existe au minimum des contextes distincts de calibration :

```text
default / station / système  → DefFreqCalTimerCB
host AP                      → ApFreqCalTimer
scan                         → FreqCalCntForScan
```

Le partage exact d'un même timer, d'un callback commun ou de structures différentes
n'est pas encore démontré.

## 36BB.6 `scan_parse_beacon()` devient une cible encore plus importante

Le corpus maître avait déjà identifié :

```text
scan_parse_beacon
```

dans `libnet80211`.

L'ancien link-map place en plus dans le **même objet** :

```text
libnet80211.a(ieee80211_scan.o)
```

les états :

```text
FreqCalCntForScan
TestStaFreqCalValInput
```

et le code :

```text
scan_parse_beacon
```

Le link-map montre :

```text
0x40255534  début de ieee80211_scan.o
...
0x40255ffc  scan_parse_beacon
0x402562ec  début de l'objet suivant
```

La fonction est donc située dans le même module qui porte les états de calibration
propres au scan.

Cela ne prouve pas que `scan_parse_beacon()` écrit directement ces deux variables, mais
elle devient une cible de xrefs prioritaire.

## 36BB.7 `ic_bss_info_update()` est une autre jonction potentielle PP → BSS

Le même build expose dans :

```text
libpp.a(if_hwctrl.o)
```

la fonction :

```text
ic_bss_info_update
```

Son nom suggère une mise à jour d'informations BSS.

Comme les champs publics :

```text
freq_offset
freqcal_val
```

sont transportés dans `bss_info`, cette fonction mérite un audit secondaire.

Aucune preuve actuelle ne permet d'affirmer qu'elle copie spécifiquement les champs
de fréquence.

## 36BB.8 Pipeline RX candidat après v0.37

Le modèle de travail devient :

```text
                         RF / baseband
                              │
                              ▼
                      synchronisation RX
                              │
                    CFO / metadata interne
                              │
                              ▼
                          LMAC RX done
                         lmacRxDone
                              │
                              ▼
                         PP RX pipeline
                    ppEnqueueRxq / ...
                              │
                              ▼
                    ppPeocessRxPktHdr
                              │
                    ┌─────────┴──────────┐
                    │                    │
                    ▼                    ▼
          aggregation freq?          RX normal
 HighestFreqOffsetInOneChk          sta_input
                    │                    │
                    ▼                    ▼
          calibration timer       beacon / scan
          DefFreqCalTimerCB            │
                    │                   ▼
                    │             scan_parse_beacon
                    │                   │
                    │             FreqCalCntForScan
                    │             TestStaFreqCalValInput
                    │                   │
                    │                   ▼
                    │             bss_info.freq_offset
                    │             bss_info.freqcal_val
                    │
                    ▼
              correction PHY
          set_rf_freq_offset
          phy_freq_offset
```

**Important :** ce schéma est un modèle de recherche. Les flèches marquées par un
traitement de fréquence ne sont pas encore toutes démontrées par xrefs.

## 36BB.9 Nouvelle hypothèse prioritaire : le CFO brut peut être dans les métadonnées traitées par PP

Le nom et la position de :

```text
ppPeocessRxPktHdr
```

suggèrent que PP traite des métadonnées de réception avant la livraison aux couches
802.11 supérieures.

Le `RxControl` public déjà reconstruit ne contient pas de champ CFO.

Deux possibilités deviennent particulièrement intéressantes :

```text
A. le vrai descripteur/header RX interne est plus riche que RxControl public
   et contient un CFO / erreur de fréquence ;

B. ppPeocessRxPktHdr lit directement un registre PHY/baseband en plus du header.
```

Dans les deux cas, désassembler cette fonction peut révéler la source recherchée **avant
`scan_parse_beacon()`**.

C'est actuellement le candidat statique le plus prometteur pour obtenir une métrique
FSK RX sans dépendre du `bss_info` final.

## 36BB.10 `DefFreqCalTimerCB` devient le meilleur candidat pour tracer la branche correction

À l'opposé, la fonction :

```text
DefFreqCalTimerCB
```

est désormais la meilleure cible pour comprendre :

```text
- quand la correction est appliquée ;
- quel état frequency-offset elle consulte ;
- si elle lit HighestFreqOffsetInOneChk ;
- si elle consulte phy_freq_offset ;
- si elle appelle set_rf_freq_offset ;
- comment elle filtre/borne la correction ;
- à quelle cadence standard le SDK agit.
```

Si un désassemblage de ce callback montre :

```text
HighestFreqOffsetInOneChk
      ↓
calcul
      ↓
set_rf_freq_offset(...)
```

la branche de correction sera pratiquement fermée.

Ce résultat ne donnerait pas encore automatiquement le **CFO brut**, mais fournirait
l'échelle, le signe et le filtrage utilisés par Espressif.

## 36BB.11 Cibles exactes à désassembler — ordre v0.37

Ordre actualisé :

```text
RX measurement branch:
1. ppPeocessRxPktHdr
2. xrefs HighestFreqOffsetInOneChk dans pp.o
3. scan_parse_beacon
4. ic_bss_info_update

correction branch:
5. DefFreqCalTimerCB
6. xrefs phy_freq_offset
7. set_rf_freq_offset
8. chip_v6_set_chan_offset
```

Dans `ppPeocessRxPktHdr`, chercher en priorité :

```text
- loads à offsets fixes dans un descripteur RX ;
- valeurs 16 bits signées ;
- masques/champs non présents dans RxControl public ;
- appels vers PHY ;
- lectures 0x6000xxxx ;
- stockage vers HighestFreqOffsetInOneChk ;
- normalisation/abs/max.
```

Dans `DefFreqCalTimerCB`, chercher :

```text
- ets_timer_arm/disarm ;
- HighestFreqOffsetInOneChk ;
- phy_freq_offset ;
- set_rf_freq_offset ;
- bornes liées à fréquence ;
- divisions/multiplications compatibles avec 8 kHz.
```

## 36BB.12 Statut v0.37

| Élément | Statut |
|---|---:|
| `ppPeocessRxPktHdr` appartient à `libpp.a(pp.o)` | **démontré dans un build historique** |
| traitement de header RX substantiel | **fortement établi par span lié** |
| fonction persiste dans plusieurs SDK | **démontré par traces décodées** |
| `DefFreqCalTimerCB` existe dans plusieurs firmwares | **démontré** |
| `DefFreqCalTimerCB` = callback calibration fréquence | **très forte inférence nominale** |
| appel direct `ppPeocessRxPktHdr → DefFreqCalTimerCB` | **non démontré** |
| `scan_parse_beacon` partage son objet avec les états FreqCal du scan | **démontré** |
| `ic_bss_info_update` existe dans PP | **démontré** |
| `ppPeocessRxPktHdr` lit le CFO brut | **hypothèse prioritaire, non démontrée** |
| `DefFreqCalTimerCB` appelle `set_rf_freq_offset` | **ouvert** |
| source MMIO/descripteur du CFO | **ouverte** |
| RX FSK autonome sans paquet valide | **toujours ouvert, mais cible beaucoup plus précise** |

Conclusion v0.37 :

> Deux points de coupe sont maintenant clairement définis. Pour la **mesure RX**, la
> cible n°1 devient `ppPeocessRxPktHdr`, située immédiatement dans le traitement des
> métadonnées RX de PP avant les couches supérieures. Pour la **correction**, la cible
> n°1 devient `DefFreqCalTimerCB`. Le prochain progrès décisif doit venir des xrefs et du
> désassemblage de ces deux fonctions, avec l'objectif de retrouver d'un côté la source
> CFO brute et de l'autre l'appel vers l'actionneur `set_rf_freq_offset`.

### Sources publiques ajoutées en v0.37

```text
https://pastebin.com/egnFgqD9
https://github.com/esp8266/Arduino/issues/1866
https://github.com/espressif/ESP8266_NONOS_SDK/issues/330
https://41j.com/blog/2015/01/esp8266-sdk-library-symbols/
```

---



# 36BC. FSK/CFO — calibration périodique PP et persistance de `DefFreqCalTimerCB` — v0.38

Cette passe relie deux familles d'indices qui étaient jusqu'ici séparées :

```text
1. état frequency-offset maintenu dans PP :
       HighestFreqOffsetInOneChk

2. orchestration de calibration périodique :
       pend_flag_periodic_cal
       periodic_cal()
       periodic_cal_top()
       DefFreqCalTimerCB
```

Le résultat ne ferme pas encore le calcul CFO, mais il clarifie fortement
l'architecture de contrôle qui entoure cette mesure.

## 36BC.1 `pend_flag_periodic_cal` et `HighestFreqOffsetInOneChk` vivent dans le même `pp.o`

Un ancien link-map ESP8266 place dans :

```text
libpp.a(pp.o)
```

la zone BSS suivante :

```text
0x3ffec258  pend_flag_noise_check
0x3ffec260  pend_flag_periodic_cal
...
0x3ffec290  HighestFreqOffsetInOneChk
```

Le fait important n'est pas l'adresse absolue, propre à cet ancien build, mais
l'appartenance des deux symboles au **même objet PP**.

Cela démontre que `pp.o` possède simultanément :

```text
- un état de planification / pending pour une calibration périodique ;
- un état explicitement nommé autour du plus grand frequency offset d'un check.
```

Il serait toutefois excessif de conclure immédiatement que :

```text
pend_flag_periodic_cal
        déclenche uniquement
HighestFreqOffsetInOneChk
```

ou que toute calibration périodique est une calibration de fréquence.

Le PHY effectue aussi d'autres calibrations RF.

## 36BC.2 Le corpus maître connaissait déjà les appels à `periodic_cal_top()`

Le document maître avait déjà établi, par relocations non résolues, des références
directes à :

```text
periodic_cal_top()
```

depuis :

```text
libpp: pm.o
libpp: pm_for_bcn_only_mode.o
libpp: pp.o
libnet80211: ieee80211_hostap.o
```

La nouveauté v0.38 n'est donc **pas** "PP peut déclencher periodic_cal_top".

La nouveauté est :

> dans un ancien build, le **même `pp.o` qui participe à la calibration périodique**
> contient aussi `HighestFreqOffsetInOneChk` et `pend_flag_periodic_cal`.

Cela resserre le lien fonctionnel entre l'orchestration RF périodique et l'état de
frequency offset, sans encore fournir le xref instruction-par-instruction.

## 36BC.3 Le PHY possède sa propre couche de calibration périodique

Les tables de symboles et link-maps historiques exposent dans `libphy.a` :

```text
periodic_cal
periodic_cal_top
periodic_cal_flag
periodic_cal_dc_num
periodic_cal_sat
phy_freq_offset
```

La séparation des couches devient donc :

```text
PP / net80211
    │
    │ scheduling / états protocole / timer
    ▼
periodic_cal_top()
    │
    ▼
libphy
    │
    ├── calibration RF périodique
    └── états PHY associés
```

Le rôle exact de `phy_freq_offset` à l'intérieur de cette mécanique reste ouvert.

## 36BC.4 `DefFreqCalTimerCB` persiste après la suppression de l'API publique

La chronologie officielle apporte une contrainte forte.

La release NONOS SDK v3.0.1 annonce explicitement :

```text
Remove system_phy_freq_trace_enable
disable freq trace in AT project by default
update libphy to version 1143
```

Or une stack trace issue d'un projet utilisant :

```text
ESP8266 NON-OS SDK 3.0.4
```

contient encore :

```text
DefFreqCalTimerCB
pp_tx_idle_timeout
ppPeocessRxPktHdr
scan_parse_beacon
...
```

Conclusion démontrée au niveau du lien binaire :

> la suppression de l'API publique `system_phy_freq_trace_enable()` n'a pas supprimé
> le symbole interne `DefFreqCalTimerCB` de tous les builds ultérieurs.

Ce résultat est important : le callback appartient à une infrastructure interne plus
durable que l'API publique qui permettait à l'application d'activer le frequency trace.

## 36BC.5 Ce que cette persistance ne prouve PAS

La simple présence de :

```text
DefFreqCalTimerCB
```

dans l'image liée ne prouve pas que :

```text
- son timer est toujours armé ;
- le callback s'exécute lorsque le frequency trace est désactivé ;
- il conserve exactement le même algorithme entre SDK ;
- il est exclusivement dédié au CFO ;
- il appelle toujours set_rf_freq_offset().
```

Une fonction peut rester liée pour :

```text
compatibilité interne
autre mode de calibration
code dormant
chemin AP/STA particulier
maintenance RF indépendante du trace public
```

La bonne conclusion est donc **persistance du code**, pas **activation permanente**.

## 36BC.6 Persistance très longue dans les firmwares ESP8266

Une stack trace publique datée de 2023, produite par un firmware Arduino ESP8266,
est encore décodée avec :

```text
DefFreqCalTimerCB
```

dans le même environnement système que :

```text
ppCheckTxIdle
pm_get_sleep_type
register_chipv6_phy
pm_wakeup_init
```

Cela renforce fortement l'idée que le symbole ou son équivalent de bibliothèque est
resté présent très longtemps dans les SDK distribués avec l'écosystème ESP8266.

Comme pour les autres stack dumps :

> cela démontre la présence du symbole décodé, pas le graphe d'appels exact.

## 36BC.7 Modèle de contrôle révisé

Le modèle de recherche devient :

```text
                   RX / PHY
                      │
             mesure / état CFO
                      │
                      ▼
                  libpp.a
      ┌───────────────┴────────────────┐
      │                                │
      ▼                                ▼
HighestFreqOffsetInOneChk      pend_flag_periodic_cal
      │                                │
      │                                ▼
      │                         periodic_cal_top()
      │                                │
      │                                ▼
      │                              PHY
      │                                │
      └──────► logique FreqCal ? ◄─────┘
                     │
                     ▼
             DefFreqCalTimerCB
                     │
                     ▼
               correction fine ?
                     │
                     ▼
            set_rf_freq_offset()
```

Les éléments avec `?` restent **hypothétiques** tant que les xrefs ne sont pas
désassemblés.

Le schéma sert à définir les prochaines cibles, pas à affirmer un call graph.

## 36BC.8 Conséquence importante pour le projet FSK

Il ne faut plus considérer :

```text
system_phy_freq_trace_enable()
```

comme une dépendance nécessaire du futur démodulateur FSK.

L'API publique a disparu, mais :

```text
- les états PP de frequency offset existent historiquement ;
- les primitives PHY de calibration périodique existent ;
- DefFreqCalTimerCB reste lié dans des SDK ultérieurs ;
- set_rf_freq_offset existe comme actionneur PHY dédié.
```

Donc la cible correcte reste les **primitives et états internes**, pas le wrapper public.

Pour le RX FSK :

```text
objectif = métrique CFO brute avant scan / BSS / correction lente
```

Pour le TX FSK :

```text
objectif = actionneur rapide
           tone_control
           ou
           set_rf_freq_offset
```

## 36BC.9 Ce que la co-localisation suggère sur `HighestFreqOffsetInOneChk`

Le nom :

```text
HighestFreqOffsetInOneChk
```

et sa cohabitation avec :

```text
pend_flag_periodic_cal
```

rendent plausible un mécanisme du type :

```text
pendant une fenêtre / check :
    recevoir plusieurs mesures
    retenir une valeur extrême ou représentative
    déclencher / préparer une calibration périodique
```

Cependant, sans désassemblage, on ne connaît pas :

```text
"highest" = valeur signée max ?
"highest" = max(abs(offset)) ?
"one check" = paquet, beacon, fenêtre ou cycle ?
unité = kHz, pas PHY, autre ?
```

Ces quatre questions doivent rester ouvertes.

## 36BC.10 Cibles de désassemblage encore plus précises

### Branche mesure

```text
ppPeocessRxPktHdr
      ↓
xrefs vers HighestFreqOffsetInOneChk
      ↓
identifier la valeur source
```

Rechercher :

```text
load 8/16 bits dans un descripteur RX
sign extension
abs / max / compare
lecture MMIO PHY
stockage vers HighestFreqOffsetInOneChk
```

### Branche orchestration

```text
xrefs pend_flag_periodic_cal
      ↓
qui le positionne ?
qui le efface ?
      ↓
periodic_cal_top
```

### Branche timer/correction

```text
DefFreqCalTimerCB
      ↓
HighestFreqOffsetInOneChk ?
phy_freq_offset ?
set_rf_freq_offset ?
ets_timer_arm ?
```

Le premier xref démontré entre deux de ces groupes réduira fortement l'incertitude.

## 36BC.11 Limite de l'environnement de recherche

Plusieurs archives binaires publiques du SDK sont visibles via GitHub/Arduino, mais
l'environnement actuel ne permet pas de récupérer de manière fiable les `.a` bruts
pour exécuter localement :

```text
ar
nm
objdump
```

Un `git clone` direct a également échoué pour cause de résolution réseau dans le
conteneur.

Aucun désassemblage local nouveau de `pp.o` ou `DefFreqCalTimerCB` n'est donc revendiqué
dans cette révision.

Les nouvelles conclusions reposent sur :

```text
link-map historique
symbol dumps
release notes officielles
stack traces décodées
corpus déjà désassemblé du projet
```

## 36BC.12 Statut v0.38

| Élément | Statut |
|---|---:|
| `pend_flag_periodic_cal` dans `libpp.a(pp.o)` | **démontré dans un build historique** |
| `HighestFreqOffsetInOneChk` dans le même `pp.o` | **démontré** |
| PP référence `periodic_cal_top()` dans le corpus maître | **déjà démontré avant v0.38** |
| co-localisation orchestration périodique + état freq-offset | **nouveau recoupement fort** |
| `periodic_cal/periodic_cal_top` dans PHY | **démontré** |
| `DefFreqCalTimerCB` présent dans SDK 3.0.4 | **démontré par stack décodée** |
| API publique frequency trace supprimée avant SDK 3.0.4 | **démontré** |
| callback interne survit à la suppression API | **démontré au niveau présence du code** |
| callback actif lorsque trace désactivé | **non démontré** |
| callback appelle `set_rf_freq_offset` | **ouvert** |
| `HighestFreqOffsetInOneChk` alimente periodic calibration | **très plausible, non démontré** |
| source CFO brute | **ouverte** |
| RX FSK autonome | **toujours non fermé** |

Conclusion v0.38 :

> L'architecture frequency-offset apparaît désormais comme une **infrastructure interne
> durable et trans-couche**, et non comme une simple fonctionnalité de l'API publique
> `system_phy_freq_trace_enable()`. PP possède dans le même objet un état de calibration
> périodique et un agrégat de frequency offset ; le PHY fournit `periodic_cal_top` et
> l'actionneur `set_rf_freq_offset`; `DefFreqCalTimerCB` reste présent même dans un
> SDK 3.0.4 postérieur à la suppression de l'API publique. Le verrou reste maintenant
> purement statique : retrouver les xrefs entre ces états et identifier la **mesure CFO
> brute avant la couche paquet/scan**.

### Sources publiques ajoutées en v0.38

```text
https://pastebin.com/egnFgqD9
https://github.com/espressif/ESP8266_NONOS_SDK/releases
https://github.com/espressif/ESP8266_NONOS_SDK/issues/330
https://github.com/esp8266/Arduino/issues/1866
https://github.com/ph1p/ikea-led-obegraensad/issues/49
```

---



# 36BD. Provenance du corpus — forte correspondance avec NONOS SDK 3.0.5 — v0.39

Cette passe ne modifie pas directement le modèle FSK, mais réduit fortement une
incertitude qui bloquait les recherches externes : **quelle génération exacte des
bibliothèques Espressif est analysée dans ce projet ?**

Le corpus maître donne les tailles exactes suivantes :

```text
libpp.a       =   579 580 octets
libphy.a      =   172 184 octets
libnet80211.a = 1 004 540 octets
```

Converties en KiB :

```text
libpp.a       = 565,996 KiB  → 566 KiB arrondi
libphy.a      = 168,148 KiB  → 168 KiB arrondi
libnet80211.a = 980,996 KiB  → 981 KiB arrondi
```

Or le dépôt actuel `esp8266/Arduino`, dans le dossier :

```text
tools/sdk/lib/NONOSDK305/
```

affiche exactement :

```text
libpp.a       566 KB
libphy.a      168 KB
libnet80211.a 981 KB
```

Les trois signatures de taille arrondie concordent simultanément.

## 36BD.1 Identification du dossier NONOSDK305

Le core Arduino associe explicitement :

```text
NONOSDK305 = nonos-sdk 3.0.5
```

et son fichier `version` contient :

```text
v3.0.5-g7b5b35d
(shows as SDK:3.0.5(b29dcd3) in debug mode)
```

Le `commitlog.txt` embarqué documente notamment :

```text
commit 7b5b35da98ad9ee2de7afc63277d4933027ae91c
release/v3.0.5
```

ainsi qu'une mise à jour du header de version vers 3.0.5.

Conclusion :

> le candidat public le plus proche et de loin le plus cohérent pour le corpus est
> désormais **NONOS SDK 3.0.5, snapshot v3.0.5-g7b5b35d** tel qu'embarqué par le core
> Arduino récent.

## 36BD.2 Comparaison avec les autres SDK embarqués dans le même core

La comparaison est discriminante.

### NONOS SDK 2.2.1 legacy

GitHub affiche :

```text
libpp.a       235 KB
libphy.a      153 KB
libnet80211.a 328 KB
```

très loin du corpus.

### NONOS SDK 2.2.1 + patches 2019

Les variantes testées :

```text
NONOSDK22x_190313
NONOSDK22x_190703
NONOSDK22x_191024
NONOSDK22x_191105
NONOSDK22x_191122
```

affichent toutes un `libpp.a` d'environ :

```text
260 KB
```

également très loin des 566 KiB du corpus.

Le critère `libpp.a` suffit donc déjà à séparer clairement ces familles de
`NONOSDK305`.

## 36BD.3 Pourquoi cette correspondance est beaucoup plus significative qu'une taille unique

Une seule taille arrondie pourrait coïncider accidentellement.

Ici on dispose de trois archives indépendantes :

```text
PP
PHY
net80211
```

dont les arrondis correspondent simultanément :

```text
corpus       Arduino NONOSDK305
566 KiB  ↔  566 KB
168 KiB  ↔  168 KB
981 KiB  ↔  981 KB
```

et dont les générations 2.2.x voisines diffèrent fortement.

Cette signature à trois dimensions constitue donc un **marqueur de provenance fort**.

## 36BD.4 Ce qui empêche encore de dire "identique bit pour bit"

Le téléchargement des fichiers `Raw` GitHub/SourceForge n'a pas pu être matérialisé
dans l'environnement d'analyse courant.

Il n'a donc pas été possible de calculer directement :

```text
SHA256(public NONOSDK305/libpp.a)
SHA256(public NONOSDK305/libphy.a)
SHA256(public NONOSDK305/libnet80211.a)
```

et de les comparer aux hashes du corpus :

```text
libpp.a:
9b4865045bcc78116e7e8f042e1993b9376dd9659290d17c0ace468b50a3d36d

libphy.a:
93853fa02cbd4c1ed18bc38562cd1489e7024ceac0a9c5082fa3732f5a67f99a

libnet80211.a:
7725959a1570633cd91ce034144da44c783011ec1353aea0c509d2a6e813f94f
```

Le statut correct reste donc :

```text
même famille / snapshot très probable → oui
identité binaire SHA-256              → non encore démontrée
```

## 36BD.5 Pourquoi la taille brute redevient utile ici malgré la correction v0.35

La v0.35 a correctement interdit de dater un corpus par la taille brute seule lorsque
l'on compare :

```text
SDK différents
archives stripées vs non stripées
builds avec/sans DWARF
```

La présente comparaison est différente :

```text
- même dépôt de distribution ;
- mêmes noms de bibliothèques ;
- ensemble cohérent de trois archives ;
- variantes SDK voisines comparées côte à côte ;
- tailles arrondies correspondant toutes au corpus.
```

La taille n'est toujours pas une preuve cryptographique, mais devient ici un
**fingerprint multi-fichier pertinent**.

La règle v0.35 reste donc valide.

## 36BD.6 Conséquence immédiate pour la recherche FSK/CFO

Si le corpus est bien ce NONOSDK305, plusieurs résultats externes deviennent beaucoup
plus proches de la cible réelle.

En particulier :

```text
NONOS SDK 3.0.4:
    DefFreqCalTimerCB encore présent dans une image liée

NONOS SDK 3.0.5:
    candidat très probable du corpus actuel
```

La distance de version n'est alors plus "plusieurs générations historiques", mais :

```text
3.0.4 → 3.0.5
```

ce qui renforce fortement la pertinence des symboles :

```text
DefFreqCalTimerCB
ppPeocessRxPktHdr
scan_parse_beacon
```

retrouvés dans une stack 3.0.4.

Cela ne prouve évidemment pas que leurs corps sont identiques entre 3.0.4 et 3.0.5.

## 36BD.7 Le header Arduino courant confirme encore `freq_offset` / `freqcal_val`

Le `user_interface.h` utilisé par le core moderne définit :

```c
#if defined(NONOSDK305)
#define NONOSDK (0x30500)
#endif
```

et conserve dans :

```c
struct bss_info
```

les champs :

```c
sint16 freq_offset;
sint16 freqcal_val;
```

Donc le modèle :

```text
scan/BSS
    ↓
freq_offset
freqcal_val
```

reste cohérent avec la famille SDK candidate du corpus.

## 36BD.8 Nouvelle stratégie de recherche grâce à l'identification probable

Les recherches publiques doivent désormais privilégier explicitement :

```text
NONOS SDK 3.0.5
NONOSDK305
v3.0.5-g7b5b35d
SDK:3.0.5(b29dcd3)
```

et non plus principalement les builds 2015–2016.

Les anciens link-maps restent utiles pour comprendre les noms/objets, mais les
conclusions structurales doivent maintenant être recoupées autant que possible avec
3.0.4/3.0.5.

Priorités :

```text
1. obtenir un map ou nm de NONOSDK305 ;
2. confirmer DefFreqCalTimerCB ;
3. confirmer HighestFreqOffsetInOneChk ;
4. confirmer pend_flag_periodic_cal ;
5. confirmer phy_freq_offset ;
6. confirmer set_rf_freq_offset ;
7. comparer objets .o et signatures avec le corpus ;
8. seulement ensuite transposer les xrefs historiques.
```

## 36BD.9 Test de confirmation définitive dès que les octets publics sont accessibles

La procédure de fermeture est triviale :

```bash
sha256sum libpp.a libphy.a libnet80211.a
```

Résultat attendu pour une identité exacte :

```text
libpp.a       9b4865045bcc78116e7e8f042e1993b9376dd9659290d17c0ace468b50a3d36d
libphy.a      93853fa02cbd4c1ed18bc38562cd1489e7024ceac0a9c5082fa3732f5a67f99a
libnet80211.a 7725959a1570633cd91ce034144da44c783011ec1353aea0c509d2a6e813f94f
```

Une seule divergence suffira à reclasser le résultat en :

```text
même génération / build proche
```

au lieu de :

```text
même snapshot exact
```

## 36BD.10 Statut v0.39

| Élément | Statut |
|---|---:|
| dossier Arduino `NONOSDK305` = SDK 3.0.5 | **démontré** |
| snapshot public = `v3.0.5-g7b5b35d` | **démontré** |
| debug version = `SDK:3.0.5(b29dcd3)` | **démontré** |
| taille publique `libpp.a` = 566 KB affichés | **démontré** |
| taille publique `libphy.a` = 168 KB affichés | **démontré** |
| taille publique `libnet80211.a` = 981 KB affichés | **démontré** |
| les trois arrondis correspondent au corpus | **démontré** |
| SDK 2.2.x comparés nettement plus petits | **démontré** |
| corpus = famille NONOSDK305 | **très haute confiance** |
| corpus = snapshot exact g7b5b35d | **fort candidat, pas encore SHA-confirmé** |
| identité SHA-256 | **ouverte faute d'accès binaire public local** |

Conclusion v0.39 :

> Le corpus du projet est désormais **très probablement issu de NONOS SDK 3.0.5**.
> Le trio de tailles exactes du corpus reproduit simultanément les trois tailles
> arrondies du dossier public `NONOSDK305`, alors que toutes les variantes 2.2.x
> comparées diffèrent nettement. Le dossier public identifie son snapshot comme
> `v3.0.5-g7b5b35d`, affiché en debug comme `SDK:3.0.5(b29dcd3)`.
> La dernière étape de preuve de provenance est une simple comparaison SHA-256,
> actuellement bloquée uniquement par l'accès aux octets `Raw`.

### Sources publiques ajoutées en v0.39

```text
https://github.com/esp8266/Arduino/blob/master/tools/sdk/lib/NONOSDK305/libpp.a
https://github.com/esp8266/Arduino/blob/master/tools/sdk/lib/NONOSDK305/libphy.a
https://github.com/esp8266/Arduino/blob/master/tools/sdk/lib/NONOSDK305/libnet80211.a
https://github.com/esp8266/Arduino/blob/master/tools/sdk/lib/NONOSDK305/version
https://github.com/esp8266/Arduino/blob/master/tools/sdk/lib/NONOSDK305/commitlog.txt
https://github.com/esp8266/Arduino/blob/master/tools/platformio-build.py
```

---



# 36BE. NONOSDK305 patché + signe de correction fréquence — v0.40

Cette passe corrige la provenance binaire de `NONOSDK305` et précise le test expérimental
à réaliser sur `set_rf_freq_offset()`.

Deux conclusions doivent être séparées :

```text
1. NONOSDK305 public dans Arduino dérive bien du NONOS SDK 3.0.5 Espressif.
2. Les archives `.a` embarquées par Arduino ne sont pas nécessairement bit-identiques
   aux archives Espressif d'origine, car Arduino les repatche après import.
```

Cette distinction est importante pour toute comparaison SHA-256.

## 36BE.1 Patch binaire Arduino des SDK NONOS 3.0.x

Le script officiel `tools/sdk/lib/fix_sdk_libs.sh` du core Arduino ESP8266 applique aux
répertoires dont le nom commence par :

```text
NONOSDK30
```

une redéfinition globale de symbole dans les archives `.a` :

```text
pvPortMalloc  →  sdk3_pvPortMalloc
```

avec `xtensa-lx106-elf-objcopy --redefine-sym`.

Le but documenté par le projet Arduino est de gérer le changement d'ABI de
`pvPortMalloc()` dans les SDK Espressif 3.0.x et de rediriger les appels vers le wrapper
Arduino `sdk3_pvPortMalloc`.

Le patch touche la famille :

```text
3.0.0
3.0.1
3.0.2
3.0.3
3.0.4
3.0.5
```

et l'historique du commit montre notamment, pour `NONOSDK305`, des versions modifiées de :

```text
libpp.a
libphy.a
libnet80211.a
```

## 36BE.2 Conséquence sur les SHA-256

Une simple opération :

```text
objcopy --redefine-sym
```

peut modifier :

```text
tables de symboles
relocations associées aux symboles externes
métadonnées ELF des objets
contenu de l'archive `.a`
```

sans changer le comportement du code RF concerné.

Donc :

```text
SHA(corpus) != SHA(Espressif SDK 3.0.5 brut)
```

ne permet plus, à lui seul, de rejeter l'hypothèse `SDK 3.0.5`.

La comparaison de provenance doit se faire dans cet ordre :

```text
1. comparer d'abord au NONOSDK305 patché du core Arduino ;
2. sinon comparer à l'Espressif 3.0.5 brut après normalisation des patches ;
3. comparer ensuite objets, symboles, relocations et code machine.
```

## 36BE.3 Portée du patch Arduino vis-à-vis du reverse-engineering RF

Le patch `pvPortMalloc → sdk3_pvPortMalloc` est un patch de compatibilité allocation
mémoire/ABI.

Aucun élément retrouvé ne montre qu'il modifie intentionnellement :

```text
set_rf_freq_offset
phy_freq_offset
periodic_cal_top
IQ_EST
PBUS
tone generator
RFPLL
```

Conclusion :

> le patch explique pourquoi les **hashes** peuvent différer ; il ne remet pas en cause
> les conclusions fonctionnelles RF/FSK déjà obtenues.

Il faut néanmoins garder la discipline suivante :

```text
adresses / offsets / relocations exactes
    → toujours vérifier sur le binaire exact du corpus
```

## 36BE.4 Actionneur documentaire `force_freq_offset` : signe et granularité

La structure PHY-init documentée pour ESP8266 possède :

```text
freq_correct_mode   offset 0x70
force_freq_offset   offset 0x71
```

avec :

```text
type force_freq_offset : int8_t
unité                  : 1 pas = 8 kHz
plage théorique         : environ ±1016 kHz
```

Le mode forcé remplace la mesure/correction automatique par la valeur
`force_freq_offset`.

L'information la plus utile pour le FSK est le **signe de la correction**.

Dans les exemples Espressif historiques :

```text
erreur mesurée +160 kHz
→ commande de correction -20 pas
→ -20 × 8 kHz = -160 kHz

erreur mesurée -160 kHz
→ commande de correction +20 pas
→ +20 × 8 kHz = +160 kHz
```

Le modèle documentaire de compensation est donc :

```text
correction_kHz ≈ - erreur_mesurée_kHz
code ≈ correction_kHz / 8
```

ou :

```text
code ≈ - erreur_mesurée_kHz / 8
```

pour une compensation manuelle idéale.

## 36BE.5 Attention : ne pas transférer automatiquement cette ABI à `set_rf_freq_offset()`

Le symbole ESP8266 :

```text
set_rf_freq_offset
```

est démontré dans `libphy.a`.

Le champ :

```text
force_freq_offset
```

est également démontré dans la configuration PHY.

Mais la relation exacte suivante n'est PAS encore démontrée :

```c
set_rf_freq_offset(int8_t force_freq_offset);
```

Il reste donc ouvert de savoir si la fonction reçoit :

```text
- directement le code int8 à pas de 8 kHz ;
- une valeur déjà transformée ;
- un entier de plus grande largeur ;
- une structure ou plusieurs paramètres ;
- une unité interne différente.
```

Le pas 8 kHz devient une **hypothèse de test très précise**, pas encore une ABI fermée.

## 36BE.6 Test TX FSK désormais beaucoup plus discriminant

Si `set_rf_freq_offset()` peut être appelé de façon contrôlée dans le firmware de test,
la campagne la plus informative consiste à conserver :

```text
canal            fixe
RFPLL            verrouillée
TX XPD           actif
TX clock         active
gain / puissance fixes
tone / porteuse  stable
```

et balayer une petite série symétrique :

```text
-4, -2, -1, 0, +1, +2, +4
```

puis mesurer la fréquence RF.

Deux résultats possibles :

### Hypothèse H1 — fonction dans le domaine documentaire 8 kHz

On attendrait une pente proche de :

```text
8 kHz / unité
```

soit des déplacements relatifs de l'ordre de :

```text
-32, -16, -8, 0, +8, +16, +32 kHz
```

avec éventuellement une inversion de signe selon que l'argument représente :

```text
correction appliquée
```

ou :

```text
erreur mesurée à compenser.
```

### Hypothèse H2 — unité interne différente

Une autre pente, une non-linéarité ou une saturation indiquerait que
`set_rf_freq_offset()` travaille dans un domaine interne distinct de l'octet PHY-init 113.

Ce test fermerait simultanément :

```text
unité
signe
linéarité
plage locale
latence de settling
```

## 36BE.7 Conséquence pour un BFSK basé sur `set_rf_freq_offset`

Si H1 est confirmée et si la fonction est hot-update :

```text
symbole 0 → offset = -K
symbole 1 → offset = +K
```

donnerait une séparation théorique :

```text
ΔF_symboles ≈ 16 kHz × K
```

Exemples conditionnels :

```text
K=1 → séparation F0/F1 ≈ 16 kHz
K=2 → séparation F0/F1 ≈ 32 kHz
K=4 → séparation F0/F1 ≈ 64 kHz
```

Ces valeurs ne sont **pas** revendiquées comme performances démontrées : elles ne sont
que la conséquence mathématique du pas documentaire 8 kHz si l'ABI runtime est la même.

## 36BE.8 Comparaison actuelle des deux candidats TX FSK

| Propriété | `tone_control` | `set_rf_freq_offset` |
|---|---:|---:|
| existence ESP8266 | démontrée | démontrée |
| finalité fréquence explicite dans le nom | moyenne | forte |
| loi vers Hz | inconnue | candidat 8 kHz/unité |
| hot-update démontré | non | non |
| maintien RFPLL possible | très probable | probable |
| amplitude indépendante | probable | probable |
| potentiel haute cadence | très élevé | ouvert |
| meilleur usage actuel | candidat FSK rapide | candidat FSK à déviation calibrable |

Aucune des deux voies n'est encore fermée à 100 %.

## 36BE.9 Côté RX : la cible reste inchangée

Le nouveau travail de provenance ne change pas le verrou RX.

La cible demeure :

```text
ppPeocessRxPktHdr
        ↓
source de la mesure frequency-offset
        ↓
HighestFreqOffsetInOneChk
        ↓
scan / calibration
```

et, pour la branche correction :

```text
DefFreqCalTimerCB
        ↓
phy_freq_offset ?
        ↓
set_rf_freq_offset ?
```

La découverte décisive sera un xref ou un désassemblage montrant la source exacte
utilisée pour construire/mettre à jour l'offset reçu.

## 36BE.10 Statut v0.40

| Élément | Statut |
|---|---:|
| NONOSDK305 dérive du SDK Espressif 3.0.5 | **très haute confiance** |
| archives Arduino NONOSDK30x repatchées | **démontré** |
| `pvPortMalloc → sdk3_pvPortMalloc` via `objcopy` | **démontré** |
| SHA Arduino = SHA Espressif brut obligatoire | **non** |
| patch identifié comme changement RF/FSK | **non, patch ABI mémoire** |
| `force_freq_offset` signé | **démontré** |
| granularité documentaire | **8 kHz / unité** |
| signe de compensation opposé à l'erreur mesurée | **démontré par exemples documentaires** |
| `set_rf_freq_offset` utilise directement cette unité | **hypothèse forte à tester** |
| hot-update de `set_rf_freq_offset` | **ouvert** |
| BFSK par `set_rf_freq_offset` | **candidat majeur, non fermé** |
| CFO brut RX | **toujours ouvert** |

Conclusion v0.40 :

> La provenance doit désormais être traitée comme
> **Espressif NONOS SDK 3.0.5 + patches binaires Arduino**, et non comme une archive
> Espressif nécessairement intacte. Cela explique d'éventuels écarts de SHA sans affecter
> les conclusions RF. En parallèle, la documentation de `force_freq_offset` fixe une
> hypothèse expérimentale très précise pour `set_rf_freq_offset` :
> **8 kHz par unité avec signe de correction opposé à l'erreur mesurée**.
> La prochaine fermeture TX consiste à mesurer cette loi et la capacité de hot-update ;
> la prochaine fermeture RX reste l'identification de la source CFO dans le pipeline PP.

### Sources publiques ajoutées en v0.40

```text
https://github.com/esp8266/Arduino/pull/8736
https://github.com/esp8266/Arduino/commit/4a0b66b
https://github.com/esp8266/Arduino/blob/master/tools/sdk/lib/fix_sdk_libs.sh
https://git.neulandlabor.de/j3d1/esp-open-rtos/
```

---



# 36BF. FSK/CFO — taxonomie des contrôles fréquence et portée réelle des indices statiques — v0.41

Cette passe consolide les recherches FSK autour de trois questions :

```text
1. `set_rf_freq_offset()` ressemble-t-elle à un retuning complet de canal/PLL ?
2. `test_rffreq_txcap` est-il un algorithme de fréquence ou seulement un état/table de test ?
3. le lien documentaire "pas de 8 kHz" peut-il déjà être transféré à l'API runtime ?
```

La réponse actuelle est :

```text
1. non : l'API interne sépare nettement offset fin, canal et PLL ;
2. `test_rffreq_txcap` est un petit objet de données, pas une fonction ;
3. non : le lien 8 kHz reste une hypothèse de test, pas une ABI démontrée.
```

## 36BF.1 Taxonomie ESP8266 historique des fonctions fréquence

Les symboles historiques de `libphy.a` exposent plusieurs niveaux distincts.

Dans :

```text
phy_chip_v6_ana.o
```

on trouve :

```text
set_rf_freq_offset
ram_rfpll_set_freq
ram_set_channel_freq
chip_60_set_channel
chip_v6_set_chan_offset
chip_v6_set_chan
chip_v6_set_chan_wakeup
```

Dans :

```text
phy_chip_v6.o
```

on trouve également :

```text
chip_v6_set_chanfreq
phy_freq_offset
register_chipv6_phy_init_param
periodic_cal
periodic_cal_top
```

Conclusion de haute confiance :

> le PHY ESP8266 ne traite pas "la fréquence" comme une commande monolithique.
> Il possède au minimum des couches séparées pour **offset fin**, **PLL**, **fréquence
> de canal**, **wrapper canal+offset** et **état/calibration périodique**.

Cette taxonomie rend très peu probable que `set_rf_freq_offset()` soit simplement un
alias de `ram_set_channel_freq()` ou de `ram_rfpll_set_freq()`.

## 36BF.2 Taille liée de `set_rf_freq_offset()` : petit actionneur, pas preuve de formule

Dans un ancien link-map :

```text
set_rf_freq_offset      0x40243C98
chip_v6_rxmax_ext_ana   0x40243D0C
```

Le span entre symboles est :

```text
0x74 = 116 octets
```

Dans le dump `nm` de l'objet avant link-relaxation :

```text
set_rf_freq_offset      0x0220
chip_v6_rxmax_ext_ana   0x029C
```

soit :

```text
0x7C = 124 octets
```

La différence est compatible avec la relaxation/linking Xtensa.

Il faut donc parler de :

```text
petite fonction / wrapper d'environ une centaine d'octets dans cet ancien build
```

et non prétendre connaître sa taille exacte dans le corpus NONOSDK305.

Cette petite taille est compatible avec :

```text
validation/bornage léger
calcul simple
écriture de quelques registres I2C/RF
mise à jour d'un état global
```

mais ne permet pas de déterminer laquelle de ces opérations est réellement effectuée.

## 36BF.3 `chip_v6_set_chan_offset()` est distinct du canal normal

Le même link-map donne :

```text
chip_v6_set_chan_offset  0x40244A84
chip_v6_set_chan         0x40244ABC
```

soit un petit span lié entre les deux symboles dans ce build.

Le dump objet avant relaxation donne cependant une distance beaucoup plus grande entre
leurs offsets. Il est donc incorrect de transformer le span lié en "taille exacte de
fonction".

Le fait important est uniquement :

> `chip_v6_set_chan_offset` et `chip_v6_set_chan` sont deux symboles différents,
> tous deux présents à côté des primitives PLL/canal.

Le nom et la séparation fonctionnelle soutiennent un modèle :

```text
set channel normal
        ≠
set channel with fine offset / compensation
```

sans démontrer l'ordre exact de leurs appels.

## 36BF.4 `test_rffreq_txcap` est un objet de données de 3 octets dans un ancien build

Le link-map historique place :

```text
.data phy_chip_v6.o
...
0x3FFE8890  test_rffreq_txcap
0x3FFE8893  *fill* 1
```

Le segment suivant commence à :

```text
0x3FFE8894
```

Dans ce build, `test_rffreq_txcap` occupe donc exactement la plage :

```text
0x3FFE8890 .. 0x3FFE8892
```

soit **3 octets**.

Conclusion démontrée pour ce vieux build :

> `test_rffreq_txcap` est un **petit objet de données**, pas une routine exécutée.

Ce nom peut correspondre à :

```text
trois valeurs de test
trois flags
trois paramètres fréquence/TX-cap
un petit vecteur de calibration
```

mais aucune de ces interprétations n'est démontrée.

Il ne faut surtout pas le présenter comme un "estimateur de fréquence".

## 36BF.5 `register_chipv6_phy_init_param()` est au contraire un parseur/configurateur substantiel

Dans le même link-map :

```text
register_chipv6_phy_init_param  0x402480D8
change_bbpll160_sleep           0x40248378
```

Le span lié atteint :

```text
0x2A0 = 672 octets
```

Cela est compatible avec une routine qui traite de nombreux champs de la structure
d'initialisation PHY.

Le document maître v0.29 n'avait pas encore désassemblé cette fonction et ne contenait
pas de traçage des offsets :

```text
0x70  = paramètre de mode de correction fréquence
0x71  = force_freq_offset
```

vers :

```text
phy_freq_offset
set_rf_freq_offset
```

Donc la relation :

```text
PHY-init[0x71]  →  set_rf_freq_offset(argument identique)
```

reste **non démontrée**.

## 36BF.6 Le pas 8 kHz reste une hypothèse expérimentale très forte, pas une ABI

La documentation du PHY-init démontre :

```text
force_freq_offset : int8
1 unité            : 8 kHz
```

et des exemples de correction avec signe opposé à l'erreur mesurée.

En revanche, les recherches publiques n'ont pas retrouvé de prototype C documenté pour :

```text
set_rf_freq_offset()
```

sur ESP8266.

Le statut recommandé devient :

```text
actionneur runtime nommé                       → démontré
champ d'init forcé à pas 8 kHz                 → démontré
les deux appartiennent au même sous-système    → très probable
argument runtime == champ int8 brut            → non démontré
```

Pour le firmware final, il est donc prématuré de coder :

```c
extern void set_rf_freq_offset(int8_t step);
```

comme s'il s'agissait d'une ABI certifiée.

## 36BF.7 Corroboration inter-générations : séparation mesure / correction / actionneur

Les PHY Espressif ultérieurs exposent simultanément :

```text
rom_phy_get_rx_freq
rom_phy_freq_correct
rom_set_rf_freq_offset
rom_set_channel_freq
rom_rfpll_set_freq
rom_start_tx_tone_step
```

Cette nomenclature est particulièrement utile parce qu'elle sépare explicitement :

```text
RX measurement
      ↓
frequency correction logic
      ↓
RF frequency offset actuator

et séparément :

channel selection
RFPLL synthesis
digital TX tone step
```

Cette architecture est cohérente avec le modèle ESP8266 reconstruit :

```text
bss_info.freq_offset / source CFO brute
        ↓
HighestFreqOffsetInOneChk / logique PP
        ↓
DefFreqCalTimerCB / calibration
        ↓
phy_freq_offset
        ↓
set_rf_freq_offset
```

Le schéma ESP8266 reste une hypothèse de chaîne d'appels tant que les xrefs ne sont pas
désassemblés.

## 36BF.8 Conséquence TX FSK : deux mécanismes qui ne doivent plus être mélangés

### A — FSK par tone step

```text
tone_control
    ↓
générateur numérique
    ↓
offset spectral du tone / stimulus
```

Avantage attendu :

```text
modification MMIO très rapide
RFPLL inchangée
```

Inconnues :

```text
loi step→Hz
hot-update du champ bas
phase/transitoires
```

### B — FSK par RF frequency offset

```text
set_rf_freq_offset
    ↓
actionneur de correction fréquence RF/baseband
```

Avantage attendu :

```text
finalité fréquence explicite
possible granularité physique calibrable
```

Inconnues :

```text
prototype
unité runtime
hot-update
latence
effet sur RX/TX simultanément
besoin éventuel de recalibration
```

Il n'est donc plus correct de traiter ces deux voies comme deux noms différents du même
mécanisme.

## 36BF.9 Conséquence RX FSK : la mesure doit rester en amont de l'actionneur

L'existence d'une séparation nette sur les PHY suivants renforce la stratégie RX :

```text
ne pas utiliser directement phy_freq_offset
comme si c'était nécessairement la mesure CFO reçue
```

La cible reste :

```text
source de la mesure RX
        ↓
PP / HighestFreqOffsetInOneChk
        ↓
scan / bss_info.freq_offset
```

La branche :

```text
DefFreqCalTimerCB
        ↓
phy_freq_offset
        ↓
set_rf_freq_offset
```

est plutôt candidate pour la **correction appliquée**.

Cette distinction évite de confondre discriminateur FSK et boucle d'asservissement.

## 36BF.10 État de la récupération binaire publique

Le vieux miroir `esp-open-rtos` expose toujours un `libphy.a` binaire de 180 KiB et un
lien `Raw`.

Le navigateur de recherche peut atteindre la page du fichier et identifier le lien,
mais refuse de transmettre le contenu brut car il est servi en :

```text
application/octet-stream
```

Le téléchargement direct depuis le conteneur échoue également dans l'environnement
courant.

Aucun nouveau désassemblage de ce binaire n'est donc revendiqué dans cette version.

Cette limitation explique pourquoi l'ABI de `set_rf_freq_offset()` reste ouverte malgré
la présence publique du fichier.

## 36BF.11 Priorité suivante

Le travail le plus rentable devient maintenant :

```text
1. obtenir le libphy.a NONOSDK305 ou ancien miroir sous forme d'octets ;
2. ar x libphy.a ;
3. objdump -dr phy_chip_v6_ana.o ;
4. désassembler set_rf_freq_offset ;
5. relever ses arguments, registres écrits et références globales ;
6. désassembler chip_v6_set_chan_offset ;
7. désassembler register_chipv6_phy_init_param autour des offsets 0x70/0x71 ;
8. relier ou séparer définitivement :
       force_freq_offset
       phy_freq_offset
       set_rf_freq_offset
```

En parallèle côté RX :

```text
ppPeocessRxPktHdr
HighestFreqOffsetInOneChk
DefFreqCalTimerCB
```

restent les trois points prioritaires.

## 36BF.12 Statut v0.41

| Élément | Statut |
|---|---:|
| offset RF séparé de la RFPLL | **très fortement établi** |
| offset RF séparé du changement de canal | **très fortement établi** |
| `set_rf_freq_offset` petite fonction historique | **démontré** |
| ABI de `set_rf_freq_offset` | **ouverte** |
| `chip_v6_set_chan_offset` distinct de `chip_v6_set_chan` | **démontré** |
| `test_rffreq_txcap` = fonction | **non : objet de données** |
| taille historique de `test_rffreq_txcap` | **3 octets dans le build observé** |
| sémantique des 3 octets | **ouverte** |
| `register_chipv6_phy_init_param` = routine substantielle | **démontré par span lié** |
| mapping init[0x71] → fonction runtime | **non démontré** |
| pas documentaire 8 kHz | **démontré pour force_freq_offset** |
| pas runtime de `set_rf_freq_offset` | **hypothèse à mesurer** |
| séparation RX-measure / correction / offset sur PHY suivants | **démontrée inter-générations** |
| source CFO brute ESP8266 | **toujours ouverte** |

Conclusion v0.41 :

> Le modèle FSK se clarifie nettement : **tone-step numérique**, **mesure CFO RX**,
> **logique de correction**, **offset RF fin**, **sélection de canal** et **RFPLL**
> doivent être traités comme des couches distinctes. Sur ESP8266, `set_rf_freq_offset`
> est bien un actionneur interne séparé de canal/PLL, mais son ABI et son unité runtime
> restent à fermer. `test_rffreq_txcap`, malgré son nom suggestif, est seulement un petit
> objet de données de 3 octets dans le build historique observé.

### Sources publiques ajoutées en v0.41

```text
https://41j.com/blog/2015/01/esp8266-sdk-library-symbols/
https://pastebin.com/egnFgqD9
https://git.neulandlabor.de/j3d1/esp-open-rtos/
https://git.liberatedsystems.co.uk/jacob.eva/arduino-esp32/
https://techoverflow.net/2025/08/09/esp32-how-to-list-all-functions-in-rom/
```

---



# 36BG. CFO RX — mode PHY explicite « auto measure frequency offset » — v0.42

Cette passe apporte une preuve architecturale importante pour le RX FSK :

> le bloc PHY de l'ESP8266 possède explicitement un mode de **mesure automatique de
> l'offset de fréquence**, distinct du mode où l'utilisateur impose directement une
> correction fixe.

Cette conclusion n'est plus seulement déduite des champs `bss_info.freq_offset`,
des timers de calibration ou des symboles `phy_freq_offset` : elle est décrite
directement dans les données d'initialisation PHY utilisées par le core Arduino
avec les SDK NONOS 3.0.x, y compris 3.0.5.

## 36BG.1 Byte 112 : trois contrôles indépendants

Dans `core_esp8266_phy.cpp`, le byte PHY-init 112 est commenté comme suit :

```text
bit0:
    0 → ne pas corriger l'offset de fréquence
    1 → corriger l'offset de fréquence

bit1:
    0 → BBPLL = 168 MHz ; correction positive et négative possible
    1 → BBPLL = 160 MHz ; correction positive seulement

bit2:
    0 → mesurer automatiquement l'offset de fréquence et le corriger
    1 → utiliser le byte 113 `force_freq_offset`
```

Les valeurs documentées sont :

```text
0 → correction désactivée
1 → auto measure + correction, BBPLL 168 MHz, signe ±
3 → auto measure + correction, BBPLL 160 MHz, correction positive seulement
5 → correction forcée via byte113, BBPLL 168 MHz
7 → correction forcée via byte113, BBPLL 160 MHz
```

Le byte 113 est décrit séparément comme :

```text
force_freq_offset
signed
unité = 8 kHz
```

## 36BG.2 Pourquoi cette preuve est plus forte que les indices précédents

Avant v0.42, le projet avait déjà :

```text
bss_info.freq_offset
freqcal_val
phy_freq_offset
HighestFreqOffsetInOneChk
DefFreqCalTimerCB
set_rf_freq_offset
```

Ces éléments démontraient qu'une infrastructure de fréquence existait, mais la
localisation exacte de la **mesure** restait ambiguë.

Le commentaire PHY-init ajoute maintenant explicitement :

```text
AUTO MEASURE FREQUENCY OFFSET
```

dans la logique de configuration basse du PHY.

Conclusion de haute confiance :

> l'ESP8266 contient bien un mécanisme interne capable de **mesurer** un offset de
> fréquence, puis d'utiliser cette mesure pour une correction automatique.

Il ne s'agit donc pas seulement d'un offset estimé dans `net80211` après décodage
d'un beacon.

## 36BG.3 Recoupement indépendant avec `esp-open-rtos`

Le reverse-engineering `esp-open-rtos` reconstruit le même byte sous forme de :

```c
#define FREQ_CORRECT_DISABLE  0
#define FREQ_CORRECT_ENABLE   BIT(0)
#define FREQ_CORRECT_BB_160M  BIT(1)
#define FREQ_CORRECT_FORCE    BIT(2)
```

avec le commentaire :

```text
FREQ_CORRECT_FORCE set:
    utiliser force_freq_offset

FREQ_CORRECT_FORCE unset:
    automatically measure & correct offset
```

et :

```c
uint8_t freq_correct_mode;  /* 0x70 */
int8_t  force_freq_offset;  /* 0x71 */
```

Le champ forcé est également décrit comme :

```text
1 unité = 8 kHz
plage int8 ≈ ±1016 kHz
```

Ce recoupement est important parce qu'il provient d'un projet de reverse-engineering
indépendant du core Arduino.

Limite : `esp-open-rtos` signale que certains champs de `sdk_phy_info_t` étaient encore
insuffisamment testés dans sa configuration par défaut. Son implémentation par défaut
désactive d'ailleurs cette correction.

## 36BG.4 Point important pour NONOS SDK 3.0.5

Le même fichier Arduino précise que son mécanisme de spoof des 128 octets PHY a été
vérifié avec :

```text
SDK 3.0.5
```

et le tableau PHY contient exactement les bytes :

```text
[112] freq_correct_en
[113] force_freq_offset
```

Cela relie beaucoup plus directement cette sémantique à la génération de SDK qui
correspond très probablement au corpus du projet.

## 36BG.5 Séparation entre « mesure » et « correction »

Le bit0 et le bit2 permettent de distinguer deux concepts :

```text
MESURE:
    mécanisme interne qui estime l'offset

CORRECTION:
    décision d'appliquer ou non une compensation
```

Cependant, la configuration publique ne fournit pas un mode explicitement documenté :

```text
mesure ON
correction OFF
```

Le byte 112 décrit surtout le comportement de la boucle de correction.

Il serait donc excessif de conclure que la **même mesure interne** reste accessible
ou continuellement active lorsque :

```text
bit0 = 0
```

Cette question reste à démontrer.

## 36BG.6 Indice intéressant : le core expose toujours `bss_info.freq_offset`

Dans la même famille Arduino/SDK, le header `user_interface.h` conserve :

```c
sint16 freq_offset;
sint16 freqcal_val;
```

dans :

```c
struct bss_info
```

alors que le tableau PHY par défaut Arduino utilise :

```text
byte112 = 0
```

c'est-à-dire correction de fréquence désactivée par défaut.

Cela montre au minimum que :

```text
présence de l'interface scan `freq_offset`
        ≠
activation obligatoire de la correction automatique
```

Mais ce n'est **pas encore** une preuve que `freq_offset` est réellement renseigné avec
une valeur non nulle lorsque byte112=0.

Pour fermer ce point, il faudrait soit :

```text
- un scan réel montrant freq_offset variable avec correction désactivée ;
- soit le désassemblage du producteur de bss_info.freq_offset.
```

## 36BG.7 Conséquence majeure pour le RX FSK

Le modèle cible devient encore plus crédible :

```text
                        RF RX
                          │
                          ▼
                 estimateur CFO interne
                          │
                 ┌────────┴─────────┐
                 │                  │
                 ▼                  ▼
        chemin de mesure       boucle correction
        / métadonnée           automatique
                 │                  │
                 ▼                  ▼
          PP / scan ?         phy_freq_offset
                 │                  │
                 ▼                  ▼
    HighestFreqOffset...    set_rf_freq_offset
                 │
                 ▼
        bss_info.freq_offset
```

La nouveauté v0.42 est que le bloc :

```text
estimateur CFO interne
```

n'est plus une simple nécessité théorique du schéma : son existence fonctionnelle est
explicitement décrite par le mode **auto measure frequency offset**.

## 36BG.8 Ce qui reste encore inconnu

Cette découverte ne révèle toujours pas :

```text
registre MMIO contenant la mesure CFO
fonction qui lit la mesure
fréquence d'échantillonnage / mise à jour
unité interne avant conversion
latence
filtrage
dépendance à la synchronisation Wi-Fi
dépendance à un préambule/paquet valide
accès lorsque packet RX est retiré
```

Ces points déterminent directement si cette voie peut devenir un discriminateur
FSK autonome.

## 36BG.9 Nouvelle priorité : chercher la primitive de mesure, pas seulement le callback de correction

Le travail doit maintenant être séparé en deux branches.

### Branche A — estimateur CFO RX

Chercher dans :

```text
ppPeocessRxPktHdr
lmacRxDone
WDEV / RX descriptor
PHY baseband
```

une valeur qui :

```text
- est signée ;
- varie avec l'écart de fréquence ;
- alimente HighestFreqOffsetInOneChk ou bss_info.freq_offset ;
- existe avant la correction lente.
```

### Branche B — boucle de correction

Continuer à tracer :

```text
DefFreqCalTimerCB
phy_freq_offset
set_rf_freq_offset
periodic_cal_top
```

afin de comprendre :

```text
filtrage
signe
pas de correction
cadence du timer
```

La branche A est désormais la plus importante pour le FSK RX.

## 36BG.10 Expérience logicielle particulièrement discriminante

Une expérience simple avec le Wi-Fi normal encore actif peut aider à séparer mesure et
correction avant même le mode autonome.

Comparer les résultats de scan pour une source RF stable sous :

```text
byte112 = 0   correction OFF
byte112 = 1   auto measure + correction
byte112 = 5   correction forcée
```

et enregistrer :

```text
freq_offset
freqcal_val
RSSI
canal
```

Si `freq_offset` reste sensible à la fréquence reçue avec :

```text
byte112 = 0
```

cela démontrerait que la **mesure CFO de scan est indépendante de l'activation de la
boucle de correction**.

Cette expérience doit rester en environnement RF contrôlé ; elle n'exige pas de
modifier la fréquence d'autres réseaux.

## 36BG.11 Impact sur le pourcentage de compréhension

Après cette passe :

```text
existence d'un estimateur CFO interne : ~95 %
architecture mesure/correction        : ~85 %
source brute lisible par CPU          : ~45 %
RX FSK autonome global                : ~65 %
```

L'augmentation du RX FSK vient de la preuve explicite que le PHY sait
**automatiquement mesurer** l'offset, pas d'une découverte du registre final.

## 36BG.12 Statut v0.42

| Élément | Statut |
|---|---:|
| mode PHY auto-measure frequency offset | **explicitement documenté dans le core Arduino** |
| recoupement indépendant `esp-open-rtos` | **oui** |
| bit0 = enable correction | **fortement établi** |
| bit1 = BBPLL 168/160 MHz | **fortement établi** |
| bit2 = auto-measure vs forced offset | **fortement établi** |
| byte113 signé | **établi** |
| unité byte113 | **8 kHz** |
| compatibilité avec SDK 3.0.5 | **directement mentionnée par le core Arduino** |
| mesure CFO interne existe | **très haute confiance** |
| mesure accessible correction OFF | **ouverte** |
| source MMIO de la mesure | **ouverte** |
| mesure disponible sans paquet Wi-Fi | **ouverte** |
| `freq_offset` alimenté directement par cette mesure | **très plausible, non démontré** |
| RX FSK autonome | **renforcé à ~65 %, non fermé** |

Conclusion v0.42 :

> Le point central du RX FSK change de nature. Il n'est plus nécessaire de prouver que
> l'ESP8266 possède un estimateur de fréquence : le PHY possède explicitement un mode
> **« auto measure frequency offset and correct it »**. Le verrou est maintenant de
> retrouver **où cette mesure existe avant sa consommation par la boucle de correction
> ou le pipeline Wi-Fi**, et si elle peut être lue avec le packet RX retiré. C'est cette
> primitive, plus que `E4`, qui est désormais le meilleur candidat au discriminateur
> FSK autonome.

### Sources publiques ajoutées en v0.42

```text
https://github.com/esp8266/Arduino/blob/master/cores/esp8266/core_esp8266_phy.cpp
https://git.neulandlabor.de/j3d1/esp-open-rtos/src/commit/3e5af479bc1f02027db340d57870313d02425987/include/espressif/phy_info.h
https://github.com/esp8266/Arduino/blob/master/tools/sdk/include/user_interface.h
```

---



# 36BH. TX+RX FSK — sondage runtime via `phy_freq_offset` exporté — v0.43

Cette passe apporte une nouvelle stratégie de fermeture qui avance **TX et RX en parallèle**
sans devoir connaître immédiatement le prototype de `set_rf_freq_offset()` ni l'adresse
interne de `HighestFreqOffsetInOneChk`.

Le point clé est la différence de visibilité des symboles historiques.

`esp-open-rtos` renomme explicitement comme symboles SDK accessibles :

```text
phy_freq_offset
pend_flag_periodic_cal
periodic_cal
periodic_cal_top
ppPeocessRxPktHdr
set_rf_freq_offset
chip_v6_set_chan_offset
```

En revanche, la table de renommage ne contient pas :

```text
HighestFreqOffsetInOneChk
DefFreqCalTimerCB
FreqCalCntForScan
TestStaFreqCalValInput
```

alors qu'un ancien link-map prouve que plusieurs de ces symboles existent bien dans les
objets liés.

Interprétation :

> `phy_freq_offset` est un candidat beaucoup plus facile à **observer directement par
> symbole** dans un firmware test que `HighestFreqOffsetInOneChk`, qui semble être un
> état interne non exposé par la table de renommage communautaire.

Cette observation ne prouve pas le type C exact ni la sémantique de la variable.

## 36BH.1 Taille de `phy_freq_offset` dans le build historique

Le link-map historique place :

```text
0x3ffebe18  phy_freq_offset
0x3ffebe1a  do_pwctrl_flag
```

Le symbole suivant commence exactement deux octets plus loin.

Dans ce build :

```text
taille occupée compatible = 16 bits
```

Le type exact reste ouvert :

```text
int16_t ?
uint16_t ?
autre type 16 bits ?
```

Pour un sondage runtime, il est donc plus prudent de traiter d'abord la valeur comme :

```text
raw 16-bit
```

et de déterminer ensuite signe et unité expérimentalement.

## 36BH.2 Expérience RX/TX croisée sans appeler `set_rf_freq_offset()`

La configuration PHY donne déjà trois états discriminants :

```text
MODE A : byte112 = 0
         correction désactivée

MODE B : byte112 = 1
         auto measure + correction
         BBPLL 168 MHz
         corrections ±

MODE C : byte112 = 5
         correction forcée via byte113
         BBPLL 168 MHz
```

L'expérience proposée en environnement RF contrôlé consiste à enregistrer simultanément :

```text
raw_phy_freq_offset
bss_info.freq_offset
bss_info.freqcal_val
RSSI
canal
mode 112
force_freq_offset 113
```

pour plusieurs écarts de fréquence connus de la source.

Cette expérience ne nécessite pas encore d'appeler une fonction PHY non documentée.

## 36BH.3 Hypothèses discriminables

### H1 — `phy_freq_offset` = état de correction appliquée

Attendu :

```text
mode B auto:
    phy_freq_offset suit la correction nécessaire

mode C forcé:
    phy_freq_offset suit principalement byte113

mode A off:
    phy_freq_offset reste fixe / nul / état précédent
```

Si H1 est vraie, `phy_freq_offset` appartient surtout à la branche **actionneur/correction**.

### H2 — `phy_freq_offset` = mesure CFO ou état filtré de mesure

Attendu :

```text
mode B auto:
    varie avec CFO reçu

mode C forcé:
    peut continuer à varier indépendamment du code forcé
    ou être remplacé par l'état de correction

mode A off:
    si la mesure reste active, varie encore avec CFO reçu
```

Si la variable continue à suivre la fréquence reçue en mode A, elle devient un candidat
direct très fort pour le **RX FSK autonome**.

### H3 — `phy_freq_offset` = représentation mixte

Possible :

```text
mesure convertie puis réutilisée comme consigne
état saturé/quantifié
correction finale après filtrage
```

Dans ce cas, les comparaisons A/B/C permettent quand même de reconstruire son rôle.

## 36BH.4 Pourquoi byte112=5 est particulièrement utile

Le mode forcé fournit une référence connue :

```text
force_freq_offset = int8
1 unité = 8 kHz
```

On peut donc faire varier uniquement :

```text
byte113 = -4, -2, -1, 0, +1, +2, +4
```

sans appeler directement `set_rf_freq_offset()`.

Si `phy_freq_offset` prend une loi simple par rapport à byte113 :

```text
raw = a * force + b
```

on obtient immédiatement :

```text
signe interne
échelle
offset éventuel
saturation locale
```

et potentiellement le domaine d'entrée de `set_rf_freq_offset()`.

Cette méthode est plus sûre que de deviner le prototype C de la fonction.

## 36BH.5 Conséquence TX

Si le mode forcé produit effectivement un déplacement RF mesurable :

```text
byte113 = -K → F0
byte113 = +K → F1
```

on confirme physiquement l'actionneur fin de fréquence **sans appeler son API runtime**.

Cela permet de mesurer :

```text
Δf / pas
signe
linéarité
plage
effet sur la puissance
besoin éventuel de relock
```

Le seul point que cette expérience ne ferme pas est :

```text
latence d'un changement runtime symbole-par-symbole
```

puisque le byte PHY-init est une configuration, pas nécessairement une commande hot.

## 36BH.6 Conséquence RX

En parallèle, le même montage permet de comparer :

```text
source CFO connue
        ↓
bss_info.freq_offset
phy_freq_offset
freqcal_val
```

Cela répond à trois questions critiques :

```text
1. quelle variable suit réellement la fréquence reçue ?
2. quelle variable suit plutôt la correction appliquée ?
3. la mesure existe-t-elle encore lorsque la correction est OFF ?
```

Si `bss_info.freq_offset` reste valide en mode A, alors :

> la mesure CFO RX et la boucle de correction sont fonctionnellement séparables.

Si `phy_freq_offset` reste lui aussi sensible à la fréquence reçue en mode A, son
intérêt pour le RX FSK augmente fortement.

## 36BH.7 `HighestFreqOffsetInOneChk` reste une cible statique, pas la première sonde runtime

L'ancien link-map démontre :

```text
HighestFreqOffsetInOneChk
    dans libpp.a(pp.o)
```

mais la table de renommage `esp-open-rtos` ne l'expose pas alors qu'elle expose :

```text
ppPeocessRxPktHdr
pend_flag_periodic_cal
phy_freq_offset
```

Il est donc méthodologiquement préférable de :

```text
1. sonder d'abord les symboles accessibles ;
2. reconstruire leur comportement ;
3. revenir ensuite aux xrefs de HighestFreqOffsetInOneChk
   pour trouver la source brute exacte.
```

Cette stratégie réduit le besoin de connaître immédiatement l'adresse BSS exacte de
`HighestFreqOffsetInOneChk` dans NONOSDK305.

## 36BH.8 Point important sur `ppPeocessRxPktHdr`

`ppPeocessRxPktHdr` apparaît dans la table de renommage SDK et dans plusieurs générations
de stack traces.

L'ancien link-map place :

```text
ppPeocessRxPktHdr  0x4024e910
ppTxPkt            0x4024ebfc
```

soit un span lié de :

```text
0x2EC = 748 octets
```

dans ce build.

Cette fonction reste donc la meilleure cible de désassemblage pour la branche
**mesure RX brute**, mais elle n'est plus le seul moyen d'avancer : le sondage de
`phy_freq_offset` peut fournir des informations fonctionnelles avant son désassemblage.

## 36BH.9 Nouveau recoupement inter-générations

Les PHY Espressif ultérieurs séparent explicitement :

```text
rom_phy_get_rx_freq
rom_phy_freq_correct
rom_set_rf_freq_offset
rom_set_channel_freq
rom_rfpll_set_freq
```

et certaines générations ajoutent même :

```text
rom_phy_en_hw_set_freq
rom_phy_dis_hw_set_freq
```

Cette nomenclature est cohérente avec une architecture :

```text
mesure RX fréquence
      ↓
logique correction
      ↓
actionneur offset RF
```

séparée du canal et de la RFPLL.

Ce recoupement ne fournit aucune ABI ESP8266, mais renforce fortement l'intérêt du test
A/B/C décrit ci-dessus.

## 36BH.10 Squelette de lecture prudent

Si le symbole est résolu par le linker du SDK exact, commencer par une lecture brute :

```c
extern volatile uint16_t phy_freq_offset;

uint16_t raw = phy_freq_offset;
```

Le choix `uint16_t` ici ne prétend pas définir le type réel du SDK : il sert uniquement
à capturer les 16 bits sans imposer de sémantique de signe.

Ensuite enregistrer :

```text
raw
raw interprété signé int16
mode 112
force 113
freq_offset scan
freqcal_val
```

et laisser les données déterminer la représentation.

Si le linker refuse le symbole dans NONOSDK305, il faudra revenir à :

```text
nm / map / adresse exacte du build
```

plutôt que forcer une adresse historique.

## 36BH.11 Sondage de `pend_flag_periodic_cal`

La table de renommage expose également :

```text
pend_flag_periodic_cal
periodic_cal
periodic_cal_top
```

Une lecture non intrusive du flag peut aider à corréler :

```text
changement de CFO
mise à jour phy_freq_offset
déclenchement calibration périodique
```

sans appeler le callback interne `DefFreqCalTimerCB`.

La sémantique exacte du flag reste cependant à confirmer.

## 36BH.12 Ordre expérimental recommandé

En environnement RF contrôlé/atténué :

```text
Phase 1 — correction OFF
    byte112=0
    varier CFO externe
    logger scan + phy_freq_offset

Phase 2 — auto measure
    byte112=1
    même balayage CFO
    logger scan + phy_freq_offset + pend_flag_periodic_cal

Phase 3 — forced
    byte112=5
    CFO externe fixe
    balayer byte113
    mesurer RF + phy_freq_offset
```

Résultats recherchés :

```text
RX:
    variable qui suit le CFO reçu

TX/correction:
    variable qui suit le code forcé
    déplacement RF par pas

architecture:
    séparation mesure vs correction
```

## 36BH.13 Statut v0.43

| Élément | Statut |
|---|---:|
| `phy_freq_offset` présent comme symbole global | **démontré historiquement** |
| taille compatible 16 bits | **fortement établie dans le build mapé** |
| `phy_freq_offset` exporté/renommé par esp-open-rtos | **démontré** |
| `pend_flag_periodic_cal` exporté/renommé | **démontré** |
| `ppPeocessRxPktHdr` exporté/renommé | **démontré** |
| `HighestFreqOffsetInOneChk` présent dans PP | **démontré historiquement** |
| `HighestFreqOffsetInOneChk` dans table de renommage | **non retrouvé** |
| lecture runtime de `phy_freq_offset` sur NONOSDK305 exact | **à tester** |
| `phy_freq_offset` = mesure CFO | **ouvert** |
| `phy_freq_offset` = correction appliquée | **hypothèse concurrente** |
| mode forcé comme référence 8 kHz | **démontré au niveau PHY-init** |
| méthode A/B/C pour séparer mesure/correction | **nouveau protocole de fermeture** |
| RX FSK autonome | **~68 % compris** |
| TX FSK via offset fin | **~82 % compris au niveau architecture, hot-update ouvert** |

Conclusion v0.43 :

> La recherche n'est plus bloquée par l'absence de désassemblage complet. Un symbole
> global 16 bits, `phy_freq_offset`, est visible dans les anciens SDK et renommé
> explicitement par `esp-open-rtos`. En combinant correction OFF, auto-measure et
> correction forcée à pas documenté de 8 kHz, on peut déterminer expérimentalement
> si cet état représente la **mesure CFO**, la **correction appliquée**, ou une forme
> mixte. La même campagne caractérise simultanément l'actionneur TX de fréquence et
> la branche RX de mesure, tout en évitant pour l'instant d'appeler
> `set_rf_freq_offset()` avec un prototype encore inconnu.

### Sources publiques ajoutées en v0.43

```text
https://git.neulandlabor.de/j3d1/esp-open-rtos/blame/commit/c8716747bbdeb2c8d6a67b98751c44ca96152edf/lib/allsymbols.rename
https://pastebin.com/egnFgqD9
https://github.com/esp8266/Arduino/blob/master/cores/esp8266/core_esp8266_phy.cpp
```

---



# 36BI. Corpus exact — fermeture du pipeline CFO RX et correction du chemin TX `set_rf_freq_offset` — v0.44

Cette passe est qualitativement différente des précédentes : les quatre binaires exacts du
corpus de référence ont été fournis et analysés directement.

SHA-256 vérifiés :

```text
libnet80211.a
7725959a1570633cd91ce034144da44c783011ec1353aea0c509d2a6e813f94f

libphy.a
93853fa02cbd4c1ed18bc38562cd1489e7024ceac0a9c5082fa3732f5a67f99a

libpp.a
9b4865045bcc78116e7e8f042e1993b9376dd9659290d17c0ace468b50a3d36d

esp8266_rom.bin
32f199bf10da9a08c6c4e1b556b29c7606656b8a6d04e4f17d5afa74e5d98c68
```

Ces quatre hashes correspondent exactement au corpus consigné au début du document.
Les conclusions ci-dessous ne reposent donc plus sur un SDK historique approximatif.

## 36BI.1 Correction TX majeure : `set_rf_freq_offset()` utilise réellement la RFPLL

Dans le `phy_chip_v6_ana.o` exact :

```text
set_rf_freq_offset
    offset objet = 0x220
    taille       = 0x6D
```

Les relocations de la fonction contiennent deux appels directs déterminants :

```text
0x253 → ram_rfpll_set_freq
0x26E → wait_rfpll_cal_end
```

Conclusion certaine :

> `set_rf_freq_offset()` n'est pas un simple registre de déviation numérique indépendant
> de la synthèse RF. Dans ce corpus, la primitive reprogramme la RFPLL et attend
> explicitement la fin de sa calibration.

Cette découverte **supplante** les hypothèses v0.35–v0.43 qui considéraient encore
`set_rf_freq_offset()` comme un candidat possible de FSK très rapide sans relock.

Le chemin réel est au minimum :

```text
set_rf_freq_offset(...)
        ↓
ram_rfpll_set_freq(...)
        ↓
wait_rfpll_cal_end()
        ↓
nouvelle fréquence RF stabilisée
```

L'unité documentaire de `force_freq_offset` à 8 kHz reste pertinente pour comprendre
la correction de fréquence, mais elle ne transforme pas ce chemin en modulateur
symbole-par-symbole rapide.

## 36BI.2 `chip_v6_set_chan_offset()` confirme le caractère lourd de la branche correction

Dans le même objet :

```text
chip_v6_set_chan_offset
    offset = 0xF60
    taille = 0x58
```

La fonction possède des littéraux/références vers :

```text
chip6_phy_init_ctrl
phy_freq_offset
```

et appelle directement :

```text
chip_v6_set_chan()
```

Le `chip_v6_set_chan()` exact appelle à son tour notamment :

```text
stop_dig_rx
chip_60_set_channel
bbpll_cal
start_dig_rx
```

Donc la branche standard :

```text
frequency calibration / channel offset
```

est structurellement beaucoup plus lourde qu'un simple RMW du générateur de tone.

Conclusion TX v0.44 :

```text
tone_control / tone-step
    → reste le candidat n°1 pour FSK rapide

set_rf_freq_offset / chip_v6_set_chan_offset
    → correction / retuning PLL
    → candidat lent ou calibration
    → pas chemin canonique de modulation rapide
```

## 36BI.3 Découverte RX décisive : `phy_get_bb_freqoffset()`

Dans le `phy_chip_v6_cal.o` exact :

```text
phy_get_bb_freqoffset
    offset = 0x1A8
    taille = 0x67
```

Cette fonction constitue désormais le meilleur candidat identifié pour le readout
de frequency offset du baseband.

`wdev.o` contient **un seul appel externe direct** à cette fonction, dans :

```text
wDev_ProcessRxSucData()
```

Le DWARF exact de `wdev.o` fournit la variable locale :

```c
sint16 chl_freq_offset;
```

déclarée dans cette fonction.

La relocation d'appel est située dans le traitement RX réussi, puis la valeur est passée
au chemin d'indication de trame.

Architecture démontrée :

```text
RX hardware / baseband
        ↓
wDev_ProcessRxSucData()
        ↓
phy_get_bb_freqoffset()
        ↓
sint16 chl_freq_offset
```

## 36BI.4 `wDev_IndicateFrame()` reçoit explicitement le CFO

Le DWARF de `wdev.o` donne le prototype interne de :

```text
wDev_IndicateFrame(...)
```

avec comme sixième paramètre :

```c
sint16 chl_freq_offset;
```

Les paramètres visibles sont :

```text
ch
isampdu
tail
nblks
rxstart_time
chl_freq_offset
```

Le chemin est donc explicitement :

```text
phy_get_bb_freqoffset()
        ↓
local sint16 chl_freq_offset
        ↓
wDev_IndicateFrame(..., chl_freq_offset)
```

Ce point ferme une grande partie de la question « où le CFO entre-t-il dans la chaîne
logicielle RX ? ».

## 36BI.5 Structure interne `esf_buf_s` : CFO par buffer à l'offset +24

Le DWARF exact de `pp.o` reconstruit :

```text
struct esf_buf_s
```

de taille 40 octets.

Champs pertinents :

```text
+0   pbuf
+4   ds_head
+8   ds_tail
+12  ds_len
+16  buf_begin
+20  hdr_len
+22  data_len
+24  chl_freq_offset   ← sint16
+28  trc
+32  bqentry
+36  desc
```

Donc le CFO n'est pas seulement une valeur finale de scan.

Il est transporté **avec chaque buffer RX interne** :

```text
esf_buf_s.chl_freq_offset
```

et son type est explicitement :

```c
sint16
```

Conclusion majeure :

> l'ESP8266 transporte un **frequency offset signé par buffer RX** avant la couche
> `bss_info`/scan.

## 36BI.6 `HdlChlFreqCal()` : cœur de traitement/calibration dans `pp.o`

Le `pp.o` exact contient une fonction locale auparavant non exploitée :

```text
HdlChlFreqCal
    offset = 0x13FC
    taille = 0x24B
```

Le DWARF fournit son prototype fonctionnel :

```text
HdlChlFreqCal(
    esf_buf_t *eb,
    RxControl *rxCtrl,
    ...,
    subtype
)
```

et une variable locale explicite :

```c
sint16 tmp_chl_freq_offset;
```

Le même objet contient les globals :

```text
PktsNumInOneChk            uint16-compatible / 2 octets
AllFreqOffsetInOneChk      sint32
AvgFreqOffsetInOneChk      sint16
HighestFreqOffsetInOneChk  sint16
LowestFreqOffsetInOneChk   sint16
pktnum_sta_freqcal         sint16
all_freqoffset_sta_freqcal 2 octets
avg_freqoffset_sta_freqcal 2 octets
```

Les noms, types et coexistence dans le même module démontrent un vrai traitement
statistique de frequency offset sur plusieurs paquets/checks.

## 36BI.7 `ppRxProtoProc()` appelle réellement `HdlChlFreqCal()`

Dans le `pp.o` exact :

```text
ppRxProtoProc
    offset = 0x16A0
```

possède deux branches d'appel vers :

```text
HdlChlFreqCal
```

aux offsets de relocation :

```text
0x1846
0x187E
```

Donc la chaîne n'est plus hypothétique :

```text
WDEV RX
   ↓
esf_buf avec chl_freq_offset
   ↓
PP RX protocol processing
   ↓
HdlChlFreqCal()
   ↓
agrégation / décision de calibration
```

## 36BI.8 `HdlChlFreqCal()` commande ensuite la correction de canal

Les relocations exactes de `HdlChlFreqCal()` montrent notamment des appels vers :

```text
freq_change_check_scan_work
chm_get_current_channel
__divsi3
chip_v6_set_chan_offset
ets_timer_disarm
ets_timer_setfn
ets_timer_arm_new
```

Le cœur PP :

```text
mesure CFO reçue
        ↓
statistiques / moyennes / seuils
        ↓
décision de calibration
        ↓
chip_v6_set_chan_offset
```

est donc maintenant directement démontré dans le corpus exact.

Le `chip_v6_set_chan_offset` finit lui-même dans le chemin de changement de canal/PLL
décrit plus haut.

## 36BI.9 Deux états PHY 16 bits distincts : mesure vs correction

Le corpus exact contient :

Dans `phy_chip_v6_cal.o` :

```text
phy_meas_freq_offset
    BSS
    taille = 2 octets
```

Dans `phy_chip_v6.o` :

```text
phy_freq_offset
    BSS
    taille = 2 octets
```

Ce sont donc bien **deux variables distinctes**, et non deux noms d'un même stockage.

En outre :

```text
phy_get_freq_param()
```

référence directement :

```text
phy_meas_freq_offset
```

alors que :

```text
chip_v6_set_chan_offset()
```

référence directement :

```text
phy_freq_offset
```

Cela renforce très fortement le découpage :

```text
phy_meas_freq_offset
    → domaine mesure

phy_freq_offset
    → domaine correction / consigne appliquée
```

La sémantique mathématique exacte de chaque valeur reste à décoder instruction par
instruction, mais la séparation architecturale est désormais presque fermée.

## 36BI.10 Pipeline CFO RX reconstruit — état v0.44

Le meilleur modèle actuel est :

```text
          RF RX / ADC / baseband
                    │
                    ▼
         estimateur de fréquence BB
                    │
                    ▼
        phy_get_bb_freqoffset()
                    │
                    ▼
       sint16 chl_freq_offset
                    │
                    ▼
      wDev_ProcessRxSucData()
                    │
                    ▼
 wDev_IndicateFrame(..., CFO)
                    │
                    ▼
  esf_buf_s.chl_freq_offset
             offset +24
                    │
                    ▼
            lmac / PP RX
                    │
                    ▼
           ppRxProtoProc()
                    │
                    ▼
           HdlChlFreqCal()
                    │
      ┌─────────────┼──────────────┐
      ▼             ▼              ▼
   moyenne       maximum         minimum
 AvgFreq...   HighestFreq...   LowestFreq...
      │
      ▼
 décision de calibration
      │
      ▼
 chip_v6_set_chan_offset()
      │
      ▼
 chip_v6_set_chan()
      │
      ├── stop_dig_rx
      ├── chip_60_set_channel
      ├── bbpll_cal
      └── start_dig_rx
```

La partie allant de `phy_get_bb_freqoffset()` au CFO stocké par buffer est maintenant
fondée sur les **symboles, DWARF et relocations du corpus exact**.

## 36BI.11 Limite cruciale pour le FSK autonome

Cette découverte apporte aussi une limite importante.

`phy_get_bb_freqoffset()` est appelé dans :

```text
wDev_ProcessRxSucData()
```

c'est-à-dire dans le chemin de **réception réussie** de WDEV.

Cela suggère fortement que le readout standard CFO est consommé après une
synchronisation/réception PHY Wi-Fi valide.

Il n'est donc pas encore démontré que :

```text
porteuse FSK arbitraire non-802.11
        ↓
phy_get_bb_freqoffset()
```

produise une valeur fraîche.

Deux possibilités restent ouvertes :

### Cas A — estimateur BB free-running ou réarmable

Alors :

```text
phy_get_bb_freqoffset()
```

pourrait devenir directement le discriminateur FSK autonome recherché.

### Cas B — estimateur mis à jour seulement pendant la synchro packet Wi-Fi

Alors il faudra descendre encore un niveau :

```text
registre / corrélateur CFO du baseband
```

et le déclencher/lire sans le pipeline packet.

Cette distinction devient maintenant **le verrou RX principal**.

## 36BI.12 Expérience RX décisive après cette découverte

Sans désassembler davantage, une expérience contrôlée peut tester la fraîcheur du readout :

```text
1. recevoir un paquet Wi-Fi avec CFO connu ;
2. lire phy_get_bb_freqoffset() / état associé ;
3. supprimer le packet RX normal mais conserver RF/baseband ;
4. injecter une porteuse ou tone décalé connu ;
5. rappeler la primitive ;
6. vérifier si la valeur change sans nouveau packet valide.
```

Interprétation :

```text
la valeur suit le tone arbitraire
    → voie CFO autonome presque fermée

la valeur reste figée / invalide
    → estimateur packet-coupled
    → reverse-engineering du registre BB nécessaire
```

Cette expérience doit être réalisée sur banc RF contrôlé/atténué.

## 36BI.13 Impact direct sur le TX FSK

La hiérarchie TX est désormais beaucoup plus nette :

| Méthode | Statut v0.44 |
|---|---|
| `tone_control` hot RMW | **candidat principal FSK rapide** |
| `set_rf_freq_offset` | **retune RFPLL + attente calibration ; trop lourd par défaut** |
| `chip_v6_set_chan_offset` | **encore plus lourd : changement canal + BBPLL/RX restart** |
| `ram_rfpll_set_freq` direct | **possible expérimentalement, settling à mesurer** |

Le chantier TX doit donc revenir au point critique :

```text
déterminer physiquement tone_control → Δf
```

puis :

```text
tester changement à chaud K0 ↔ K1
```

sans arrêter le tone ni la TX clock.

## 36BI.14 Pourcentages de compréhension après analyse des binaires exacts

Estimation technique actuelle :

```text
TX OOK/ASK logiciel                         ≈ 100 %
TX FSK architecture générale               ≈ 85 %
TX FSK rapide via tone_control              ≈ 80 %
set_rf_freq_offset comme fast-FSK           ≈ 20 %  (piste fortement déclassée)
chaîne correction RFPLL                     ≈ 95 %

existence CFO RX                            ≈ 99 %
pipeline CFO Wi-Fi par trame                ≈ 95 %
mesure vs correction PHY                    ≈ 92 %
agrégation PP / calibration                 ≈ 90 %
source registre BB exacte                   ≈ 55 %
CFO disponible sans paquet Wi-Fi            ≈ 45 %
RX FSK autonome complet                     ≈ 70 %
```

Le pourcentage RX autonome ne monte pas davantage malgré la grosse découverte, car
le nouveau pipeline révèle aussi sa probable dépendance à la réussite RX Wi-Fi.

## 36BI.15 Priorités après v0.44

### TX

```text
1. fermer tone_control → fréquence
2. mesurer hot-update K0/K1
3. mesurer phase / settling / jitter
```

### RX

```text
1. désassembler ou caractériser phy_get_bb_freqoffset()
2. identifier le registre BB qu'elle lit
3. déterminer quand ce registre est mis à jour
4. tester la primitive hors packet-valid
5. si nécessaire, reproduire seulement le trigger CFO sans la pile Wi-Fi
```

### Calibration / compréhension secondaire

```text
1. décoder exactement phy_meas_freq_offset
2. décoder exactement phy_freq_offset
3. reconstruire les seuils/moyennes de HdlChlFreqCal
```

Conclusion v0.44 :

> Les binaires exacts ferment enfin la chaîne CFO Wi-Fi :
> **`phy_get_bb_freqoffset()` → `wDev_ProcessRxSucData()` →
> `esf_buf_s.chl_freq_offset` → `ppRxProtoProc()` → `HdlChlFreqCal()` → correction
> de canal/PLL**. En parallèle, `set_rf_freq_offset()` est définitivement reclassé :
> il retune la RFPLL et attend sa calibration, donc il n'est pas le chemin privilégié
> pour un FSK rapide. Le prochain verrou scientifique est désormais très précis :
> savoir si le CFO baseband peut être obtenu **sans réception packet Wi-Fi valide**.

---



# 36BJ. CFO RX — registre BB exact et formule `phy_get_bb_freqoffset()` — v0.45

Cette passe ferme presque entièrement la **primitive logicielle de lecture CFO** à partir
du binaire exact `phy_chip_v6_cal.o`.

Contrairement aux versions précédentes, le corps de `phy_get_bb_freqoffset()` a été
décodé instruction par instruction à partir de la section `.text` exacte, en recoupant :

```text
- symboles ELF ;
- relocations Xtensa ;
- littéraux de la section .text ;
- table d'opcodes Xtensa LX106 ;
- DWARF des couches WDEV / PP / net80211.
```

## 36BJ.1 Emplacement exact

Dans `phy_chip_v6_cal.o` :

```text
phy_get_bb_freqoffset
    section : .text
    offset  : 0x1A8
    taille  : 0x67 octets

unsign_to_sign
    section : .text
    offset  : 0x168
    taille  : 0x2C octets
```

La fonction CFO appelle directement :

```text
unsign_to_sign(raw, 8)
```

ce qui confirme que la quantité brute extraite du baseband est un champ **8 bits signé**.

## 36BJ.2 Littéraux matériels utilisés par la fonction

Le pool de littéraux immédiatement avant `phy_get_bb_freqoffset()` contient :

```text
0x0194 → 0x60009600
0x0198 → 0x3FF1FE00
0x019C → 0x00007FFF
0x01A0 → 0x00007FFF
0x01A4 → relocation .bss
```

La relocation `.bss` au dernier littéral, combinée à l'écriture finale `+10`, pointe
exactement vers :

```text
phy_meas_freq_offset
    BSS offset = 0x0A
    taille     = 2 octets
```

## 36BJ.3 Registre principal CFO : `0x60009800`

Le chemin valide effectue :

```text
base = 0x60009600
REG  = base + 0x200
     = 0x60009800
```

La séquence décodée est équivalente à :

```c
uint32_t r = REG32(0x60009800);

if ((r & 1) == 0) {
    result = 0x7fff;
} else {
    uint8_t raw_u8 = (r >> 8) & 0xff;
    int32_t raw_s8 = unsign_to_sign(raw_u8, 8);

    result = (raw_s8 * 107) >> 6;
    result = (int16_t)result;
}
```

Donc :

```text
bit 0      → validité / disponibilité de la mesure CFO
bits 15:8  → mesure CFO brute signée sur 8 bits
```

Le rôle des autres bits du registre n'est pas entièrement décodé dans cette fonction.

## 36BJ.4 Deuxième condition de validité / contexte

Avant de lire `0x60009800`, la fonction charge :

```text
0x3FF1FE00 + 0x23C
= 0x3FF2003C
```

puis extrait :

```text
bits 19:16
```

Si ce champ est :

```text
>= 8
```

la fonction retourne directement :

```text
0x7FFF
```

sans utiliser le CFO.

Le rôle exact de `0x3FF2003C[19:16]` reste ouvert. Il agit néanmoins comme un
**gate de contexte/validité** pour le readout CFO.

Il ne faut pas lui attribuer un nom matériel précis sans autre preuve.

## 36BJ.5 Valeur d'invalidité explicite : `0x7FFF`

Deux chemins différents aboutissent à :

```text
result = 0x7FFF
```

1. contexte `0x3FF2003C[19:16] >= 8` ;
2. `0x60009800.bit0 == 0`.

La sentinelle CFO est donc démontrée :

```c
#define BB_FREQOFFSET_INVALID  ((int16_t)0x7fff)
```

Cela sera important pour un futur démodulateur autonome : une lecture CPU de la fonction
ne suffit pas ; il faut vérifier qu'elle ne renvoie pas cette sentinelle hors packet RX.

## 36BJ.6 Conversion exacte de la mesure brute

Sur le chemin valide :

```text
raw8 = sign_extend(0x60009800[15:8])

CFO = (raw8 * 107) >> 6
```

soit mathématiquement environ :

```text
CFO ≈ raw8 × 1,671875
```

avec arithmétique entière signée.

Plage nominale issue du seul champ 8 bits :

```text
raw = -128 → environ -214
raw = +127 → environ +212
```

hors détails d'arrondi du décalage arithmétique.

## 36BJ.7 Effet de bord après lecture : `0x600098DC |= 0xF`

Après le calcul — y compris sur les chemins invalides — la fonction effectue :

```c
r = REG32(0x600098DC);
r |= 0x0000000f;
REG32(0x600098DC) = r;
```

avec barrières `memw`.

Le rôle physique exact de ces quatre bits n'est pas encore démontré.

Hypothèses possibles :

```text
acknowledge / clear de résultat
réarmement
commande de status
```

Aucune n'est promue en fait dans cette version.

Le fait certain est :

> la lecture CFO standard a un **effet de bord matériel** sur `0x600098DC[3:0]`.

## 36BJ.8 Stockage exact dans `phy_meas_freq_offset`

À la fin :

```text
result
    ↓
s16i vers .bss + 0x0A
    ↓
phy_meas_freq_offset
```

La variable est donc définitivement classée comme :

```text
dernière mesure CFO calculée / lue par phy_get_bb_freqoffset()
```

et non comme la consigne de correction.

Cela ferme la séparation avec :

```text
phy_freq_offset
```

utilisé de l'autre côté par `chip_v6_set_chan_offset()`.

## 36BJ.9 Pseudo-code reconstruit

Le comportement utile peut être résumé ainsi :

```c
int16_t phy_get_bb_freqoffset(void)
{
    int16_t out;

    uint32_t ctx = REG32(0x3FF2003C);

    if (((ctx >> 16) & 0x0f) >= 8) {
        out = 0x7fff;
    } else {
        uint32_t bb = REG32(0x60009800);

        if ((bb & 1) == 0) {
            out = 0x7fff;
        } else {
            int32_t raw =
                unsign_to_sign((bb >> 8) & 0xff, 8);

            out = (int16_t)((raw * 107) >> 6);
        }
    }

    uint32_t s = REG32(0x600098DC);
    s |= 0x0f;
    REG32(0x600098DC) = s;

    phy_meas_freq_offset = out;
    return out;
}
```

Les noms `ctx`, `bb`, `s` sont descriptifs et ne prétendent pas reproduire les noms
de source Espressif.

## 36BJ.10 Preuve que la valeur est transportée sans conversion jusqu'au scan

Le DWARF exact avait déjà démontré :

```text
esf_buf_s.chl_freq_offset
    offset = +24
    type   = sint16
```

Le désassemblage ciblé de `scan_parse_beacon()` montre maintenant explicitement :

```text
l16si ..., 24(esf_buf)
```

puis cette valeur est placée dans le registre d'argument correspondant à :

```c
scan_add_ssid(..., sint16 freq_offset, ...);
```

Le DWARF de `scan_add_ssid()` donne les paramètres :

```text
wh
scan
rssi
freq_offset  ← sint16, registre a5
ch_freq
```

Au début de `scan_add_ssid()` :

```text
a14 = a5
```

donc :

```text
a14 = paramètre freq_offset
```

Puis la fonction écrit directement :

```text
s16i a14, +54(bss)
```

Or le DWARF exact de `struct bss_info` donne :

```text
+54  freq_offset  sint16
+56  freqcal_val  sint16
```

Conclusion certaine :

> `esf_buf_s.chl_freq_offset` est copié **sans changement d'échelle** dans
> `bss_info.freq_offset`.

Le champ `freqcal_val` à +56 est alimenté séparément par un autre état du scan.

## 36BJ.11 Unité physique : kHz à très haute confiance

La documentation AT Espressif historique définit explicitement :

```text
freq offset = frequency offset of AP
unit        = kHz
ppm         = freq_offset / 2.4
```

Puisque le binaire exact démontre :

```text
phy_get_bb_freqoffset()
    ↓ aucune conversion d'échelle
esf_buf.chl_freq_offset
    ↓ aucune conversion d'échelle
scan_add_ssid(freq_offset)
    ↓
bss_info.freq_offset
```

la sortie de `phy_get_bb_freqoffset()` doit être considérée, à très haute confiance,
comme étant déjà dans l'unité exposée par l'API historique :

```text
kHz
```

Donc la granularité brute vaut approximativement :

```text
1 LSB raw BB ≈ 107 / 64 kHz
             ≈ 1,671875 kHz
```

Cette unité est reconstruite par combinaison **code exact + documentation Espressif**,
et non par un commentaire présent dans `phy_chip_v6_cal.o`.

## 36BJ.12 Registre `0x60009800` partagé avec l'EVM

La fonction immédiatement suivante :

```text
phy_get_bb_evm()
```

lit elle aussi :

```text
0x60009800
```

mais exploite un autre champ :

```text
bits 28:16   (13 bits)
```

Cela renforce fortement l'interprétation du registre comme **résultat/metadata qualité
RX du baseband**, contenant plusieurs métriques liées à la réception :

```text
bit 0       validité CFO observée
bits 15:8   CFO brut
bits 28:16  métrique EVM lue par phy_get_bb_evm
```

Les bits restants nécessitent encore un audit.

## 36BJ.13 Conséquence pour le RX FSK autonome

Le verrou est désormais extrêmement précis.

Nous savons lire le CFO par :

```text
REG32(0x60009800)
    ↓
bit0 valid ?
    ↓
signed bits15:8
    ↓
×107 >> 6
    ↓
CFO kHz
```

Mais nous ne savons pas encore ce qui fait passer :

```text
0x60009800.bit0
```

à 1, ni à quel moment :

```text
0x60009800[15:8]
```

est rafraîchi.

Dans le SDK standard, `phy_get_bb_freqoffset()` n'est appelé que depuis le chemin
`wDev_ProcessRxSucData()`.

La prochaine question unique devient donc :

> **le hardware met-il à jour `0x60009800[15:8]` et bit0 sur une porteuse/tone arbitraire,
> ou seulement après la synchronisation PHY d'un paquet 802.11 ?**

C'est cette réponse qui décide directement si le CFO devient un discriminateur FSK
autonome.

## 36BJ.14 Test matériel minimal désormais possible sans appeler le code Wi-Fi

Sur banc RF contrôlé et atténué, il est maintenant possible de tester directement :

```c
uint32_t r = REG32(0x60009800);

bool valid = r & 1;
int8_t raw = (int8_t)((r >> 8) & 0xff);
int16_t khz = (int16_t)(((int32_t)raw * 107) >> 6);
```

tout en observant :

```text
0x600098DC
phy_meas_freq_offset
```

Il faut commencer en RX Wi-Fi normal pour établir la référence, puis retirer
progressivement le packet pipeline tout en conservant RF/baseband actifs.

Ne pas écrire `0x600098DC` arbitrairement avant d'avoir reproduit exactement la
séquence standard ; les quatre bits bas ont un effet matériel encore non nommé.

## 36BJ.15 Pourcentages après v0.45

Estimation actuelle :

```text
existence CFO RX                              100 %
registre résultat CFO                        98 %
champ brut CFO                               98 %
conversion brute → valeur API                98 %
unité kHz                                    95 %
stockage mesure phy_meas_freq_offset         100 %
pipeline vers bss_info.freq_offset           98 %
condition hardware de rafraîchissement       45 %
CFO sur tone non-Wi-Fi                       45 %
RX FSK autonome global                       ~78 %

TX FSK via tone_control                      ~80 %
TX correction set_rf_freq_offset             ~97 %
set_rf_freq_offset comme fast-FSK            ~15 %
```

L'augmentation du RX FSK vient du fait que le problème n'est plus
« trouver un estimateur CFO », mais uniquement **réarmer/alimenter un registre BB déjà
parfaitement localisé** hors réception Wi-Fi normale.

Conclusion v0.45 :

> Le readout CFO ESP8266 est maintenant reconstruit jusqu'au registre :
> **`0x60009800[15:8]` = mesure brute signée, `bit0` = validité, conversion
> `CFO ≈ signed8 × 107 / 64`, puis stockage dans `phy_meas_freq_offset` et copie
> sans changement d'échelle vers `bss_info.freq_offset`.**
> Le dernier verrou RX FSK n'est plus le calcul : c'est le **trigger / mécanisme
> hardware qui rafraîchit cette mesure**.

---



# 36BK. TX+RX FSK — séparation mesure/correction et zone RX BB — v0.46

> **Correction v0.47 :** `phy_bb_rx_cfg()` n'a qu'un seul appelant dans le corpus exact, `chip_v6_initialize_bb()`. Les manipulations de `0x60009988.bit26` décrites ci-dessous sont donc reclassées comme **initialisation/gate du bloc RX**, et non comme preuve d'un réarmement CFO par paquet.

Cette passe poursuit **TX et RX en parallèle** sur les binaires exacts.

Le résultat principal est double :

```text
TX :
    la voie `set_rf_freq_offset()` est confirmée comme une voie PLL/calibration lente ;
    `tone_control` reste le seul candidat actuellement crédible pour un FSK rapide.

RX :
    la chaîne CFO est maintenant séparée en trois couches :
        résultat hardware BB
        mesure logiciellement lue
        correction appliquée
    et la fonction `phy_bb_rx_cfg()` configure plusieurs registres immédiatement voisins
    du registre CFO `0x60009800`.
```

---

## 36BK.1 Séparation exacte mesure / correction dans le PHY

Le corpus exact contient deux globals différents, tous deux de 16 bits :

```text
phy_chip_v6_cal.o:
    phy_meas_freq_offset   taille = 2 octets

phy_chip_v6.o:
    phy_freq_offset        taille = 2 octets
```

`phy_get_bb_freqoffset()` stocke explicitement sa sortie dans :

```text
phy_meas_freq_offset
```

tandis que :

```text
chip_v6_set_chan_offset()
```

référence :

```text
phy_freq_offset
```

La fonction exacte :

```text
phy_get_freq_param()
```

référence les deux variables séparément.

Conclusion désormais à très haute confiance :

```text
phy_meas_freq_offset
    = domaine MESURE CFO reçue

phy_freq_offset
    = domaine CORRECTION / offset appliqué
```

La relation mathématique complète entre les deux est traitée par la logique de
calibration PP/PHY, mais il ne faut plus les confondre.

---

## 36BK.2 Mapping exact des paramètres PHY-init fréquence

L’analyse de `register_chipv6_phy_init_param()` du corpus exact confirme que les bytes
d’initialisation de fréquence sont copiés dans la structure interne de contrôle PHY.

Le mapping utile est :

```text
PHY-init byte 112
    ↓
chip6_phy_init_ctrl[76]

PHY-init byte 113
    ↓
chip6_phy_init_ctrl[77]
```

Cela relie directement les paramètres documentés de correction de fréquence aux états
utilisés ensuite par le PHY.

En mode forcé, la logique de correction utilise le byte 113 comme quantité signée et
effectue un décalage gauche de 3 bits :

```text
signed(byte113) << 3
```

soit :

```text
× 8
```

Cette opération binaire concorde exactement avec la documentation historique :

```text
force_freq_offset
1 unité = 8 kHz
```

La granularité de 8 kHz n’est donc plus seulement documentaire : le facteur `×8`
apparaît réellement dans le chemin de correction du binaire analysé.

---

## 36BK.3 `HdlChlFreqCal()` rejette la sentinelle CFO invalide

La v0.45 a démontré que :

```text
phy_get_bb_freqoffset()
```

retourne :

```text
0x7FFF
```

lorsque la mesure CFO n’est pas valide.

Le traitement exact dans `pp.o` confirme que `HdlChlFreqCal()` reconnaît cette valeur et
ne la traite pas comme une mesure normale.

Le pipeline est donc cohérent de bout en bout :

```text
BB CFO invalide
      ↓
phy_get_bb_freqoffset() = 0x7FFF
      ↓
esf_buf_s.chl_freq_offset
      ↓
HdlChlFreqCal()
      ↓
mesure rejetée / non agrégée comme CFO normal
```

Cela renforce fortement que `0x7FFF` est bien une sentinelle logicielle officielle de la
chaîne CFO.

---

## 36BK.4 `phy_bb_rx_cfg()` configure la zone matérielle immédiatement voisine du CFO

Dans le `phy_chip_v6.o` exact :

```text
phy_bb_rx_cfg
    offset = 0x2510
    taille = 0x406 octets
```

Cette fonction d'**initialisation BB** utilise comme base principale :

```text
0x60009600
```

qui est également la base utilisée par `phy_get_bb_freqoffset()` pour atteindre :

```text
0x60009800
```

Les accès exacts retrouvés dans `phy_bb_rx_cfg()` comprennent notamment :

```text
0x600098EC
0x60009988
0x600098A0
0x60009838
0x60009860
```

Le readout CFO :

```text
0x60009800
```

se trouve donc dans le même sous-espace matériel `0x600098xx`.

Conclusion :

> **Correction v0.47 :** ces accès prouvent une configuration du voisinage RX BB, mais `phy_bb_rx_cfg()` n'est appelée qu'à l'initialisation. Ils ne prouvent donc pas le trigger/réarmement CFO par paquet.

---

## 36BK.5 Séquence notable sur `0x60009988`

Dans `phy_bb_rx_cfg()`, le registre :

```text
0x60009988
```

est modifié au moins deux fois.

Les masques observés indiquent une séquence compatible avec :

```text
effacer bit 26
...
repositionner bit 26
```

c’est-à-dire conceptuellement :

```c
r &= ~0x04000000;
...
r |=  0x04000000;
```

Cette structure ressemble fortement à une séquence :

```text
disable / reset
      ↓
configuration
      ↓
enable / release
```

mais **le rôle physique du bit 26 n’est pas démontré**.

Statut correct :

```text
bit26 de 0x60009988 manipulé comme gate/reset candidat → élevé
bit26 = enable CFO                                → non démontré
```

Ce registre devient néanmoins une cible expérimentale prioritaire pour le RX FSK.

---

## 36BK.6 RMW exact sur `0x600098EC` — correction v0.47

Le désassemblage Xtensa exact montre :

```c
r = REG32(0x600098EC);
r &= 0x7FFFFFFF;      /* clear bit31 */
r |= 0x01900000;
REG32(0x600098EC) = r;
```

La v0.46 associait trop rapidement `0x01900000` à une valeur décimale `400`.
Cette interprétation est retirée.

Le `movi 400` observé dans la même fonction est en réalité écrit dans :

```text
0x60009D18
```

et ce même registre reçoit plus tard la valeur `128`.

Conclusion correcte :

> `0x600098EC` reçoit un RMW précis `clear bit31` puis `OR 0x01900000`, mais la
> sémantique de ces bits reste inconnue. Les constantes `400` et `128` appartiennent
> à `0x60009D18`, pas à `0x600098EC`.

---

## 36BK.7 Autres registres RX voisins configurés

La même routine modifie également :

```text
0x600098A0
0x60009838
0x60009860
```

et d’autres fonctions du PHY utilisent le même voisinage :

```text
phy_dig_spur_set/prot   → plusieurs 0x600098xx / 0x600099xx
phy_set_rx11b_reg       → 0x60009820
read_hw_noisefloor      → 0x60009824
get_adc_rand            → 0x60009830
```

Cela confirme que :

```text
0x600098xx / 0x600099xx
```

forme une zone de configuration/résultats du RX baseband, et non un registre isolé
spécifique au scan.

---

## 36BK.8 Aucun producteur CPU direct de `0x60009800` identifié

Dans les accès statiquement localisés dans le corpus exact :

```text
phy_get_bb_freqoffset()
    lit 0x60009800

phy_get_bb_evm()
    lit 0x60009800
```

Aucune écriture CPU normale vers :

```text
0x60009800
```

n’a été identifiée.

Cela renforce fortement :

> `0x60009800` est un **registre de résultat produit par le hardware/baseband**.

Cette conclusion reste formulée comme « aucune écriture localisée dans le corpus »,
et non comme preuve mathématique qu’aucun chemin indirect ne peut jamais l’écrire.

---

## 36BK.9 `0x600098DC` devient candidat d’acknowledge / re-arm

Le getter CFO effectue après lecture :

```c
REG32(0x600098DC) |= 0x0F;
```

Parmi les accès localisés, cette opération apparaît spécifiquement dans le chemin
`phy_get_bb_freqoffset()`.

Le comportement est compatible avec :

```text
acknowledge résultat
clear status
re-arm mesure
```

mais aucune de ces trois interprétations n’est encore démontrée.

La paire de registres devient donc :

```text
0x60009800
    → résultat CFO/EVM

0x600098DC
    → contrôle/status associé probable
```

avec niveau de confiance plus élevé pour le premier que pour le second.

---

## 36BK.10 Indice de dépendance à la synchronisation paquet

Le getter standard CFO n’est pas appelé arbitrairement dans le SDK.

Le seul appel direct identifié dans les bibliothèques exactes se trouve dans :

```text
wDev_ProcessRxSucData()
```

Le DWARF indique :

```text
fonction wDev_ProcessRxSucData : ligne source 359
local chl_freq_offset          : ligne 382
appel phy_get_bb_freqoffset    : autour ligne 386
```

Le nom même du chemin :

```text
ProcessRxSucData
```

et le fait que le registre `0x60009800` contient aussi l’EVM rendent **très plausible**
que la métrique CFO soit mise à jour pendant/à la fin d’une réception PHY synchronisée.

Mais ce point ne suffit toujours pas à conclure :

```text
« impossible sans paquet Wi-Fi »
```

Le hardware peut éventuellement produire la mesure dès qu’un synchroniseur interne est
verrouillé, même si la couche MAC n’accepte pas ensuite le paquet.

C’est désormais le test RX le plus important.

---

# TX — poursuite en parallèle

## 36BK.11 `set_rf_freq_offset()` définitivement déclassé pour le fast-FSK

Le binaire exact a déjà démontré :

```text
set_rf_freq_offset()
    ↓
ram_rfpll_set_freq()
    ↓
wait_rfpll_cal_end()
```

et :

```text
chip_v6_set_chan_offset()
    ↓
chip_v6_set_chan()
    ↓
stop_dig_rx
chip_60_set_channel
bbpll_cal
start_dig_rx
```

Conclusion v0.46 :

> le chemin de correction fréquence standard est un chemin PLL/calibration et n’est pas
> le bon candidat pour une modulation FSK rapide.

Il reste utile pour :

```text
calibration
correction lente
validation de fréquence
éventuellement FSK très lente
```

mais plus comme primitive principale symbole-par-symbole.

---

## 36BK.12 `tone_control` reste la frontière TX rapide

Le générateur de tone conserve les propriétés déjà fermées :

```text
PLL               stable
TX clock          stable
bit18             gate
bits17:10         amplitude / scale
tone_control      injecté brut dans les bits bas
```

La ROM ne réalise aucune conversion :

```text
tone_control → fréquence
```

en logiciel.

Les valeurs établies dans le corpus restent :

```text
8
64
```

dans des calibrations différentes.

Les PHY Espressif ultérieurs exposent séparément une primitive nommée :

```text
start_tx_tone_step
```

en plus de :

```text
start_tx_tone
set_rf_freq_offset
rfpll_set_freq
```

Ce recoupement continue de soutenir fortement :

```text
tone_control ≈ step/incrément du générateur numérique
```

mais **aucune formule publique fiable `step → Hz` n’a été trouvée**.

---

## 36BK.13 Limite statique TX désormais claire

Le reverse-engineering logiciel peut encore déterminer :

```text
packing
hot RMW possible du registre
séparation amplitude/fréquence
clocks
gate
chemins de calibration
```

mais il n’existe dans le code analysé aucune table ou formule permettant de convertir :

```text
tone_control
```

en :

```text
Hz
```

La loi est donc probablement implémentée directement dans le matériel du tone generator.

Conclusion méthodologique :

> la fermeture TX suivante doit être **physique**, pas seulement statique.

---

## 36BK.14 Test TX minimal qui ferme la loi de fréquence

Sur banc RF contrôlé/atténué, conserver constants :

```text
canal
RFPLL
TX clock
XPD TX
gain
digital_scale
bit18 = 1
```

puis mesurer uniquement :

```text
tone_control = 8
tone_control = 64
```

en première étape.

Si les fréquences sont différentes :

```text
Δf8
Δf64
```

on confirme immédiatement que le champ bas agit bien sur la fréquence.

Ensuite seulement, tester quelques valeurs prudentes :

```text
1, 2, 4, 8, 16, 32, 64
```

pour déterminer :

```text
linéarité
signe
granularité
wrap éventuel
phase
settling
```

Le balayage complet n’est pas nécessaire avant d’avoir prouvé le comportement sur
les valeurs déjà utilisées par le SDK.

---

## 36BK.15 Test RX minimal pour le trigger CFO

Avec le RX normal comme référence :

```text
1. recevoir une trame Wi-Fi connue avec CFO contrôlé ;
2. lire 0x60009800 ;
3. confirmer bit0=1 et le champ [15:8] ;
4. reproduire la lecture/ack standard de 0x600098DC ;
5. désactiver progressivement le chemin packet sans couper RF/BB ;
6. injecter une porteuse/tone connue ;
7. observer si bit0 revient à 1 et si [15:8] change.
```

Résultats :

```text
bit0 se réarme + CFO suit le tone
    → RX FSK autonome presque fermé

bit0 reste à 0
    → estimateur packet/sync-coupled
    → il faut identifier le trigger/synchroniseur BB
```

Dans un second temps seulement, tester les candidats de configuration :

```text
0x60009988 bit26
0x600098EC champ autour de bits25:16
```

en reproduisant d’abord exactement les valeurs standard.

---

## 36BK.16 État parallèle TX/RX après v0.46

| Bloc | Compréhension estimée |
|---|---:|
| TX OOK/ASK logiciel | **100 %** |
| TX FSK architecture | **~90 %** |
| `tone_control` comme candidat fast-FSK | **~85 %** |
| loi `tone_control → Hz` | **~45 %** |
| hot-update `tone_control` | **~65 % architectural, à mesurer RF** |
| `set_rf_freq_offset` | **~98 % compris comme voie PLL/calibration** |
| CFO RX registre/formule | **~99 %** |
| mesure vs correction PHY | **~98 %** |
| pipeline CFO par buffer | **~98 %** |
| zone BB de configuration CFO | **~70 %** |
| trigger / validité CFO | **~55 %** |
| CFO sans packet-valid | **~50 %** |
| RX FSK autonome global | **~80 %** |

Conclusion v0.46 :

> Le travail parallèle réduit maintenant le projet à deux inconnues physiques très
> précises. **TX :** déterminer la loi matérielle `tone_control → fréquence` et la
> dynamique du hot-update. **RX :** déterminer ce qui arme le résultat
> `0x60009800.bit0` et rafraîchit `bits15:8`, probablement dans la configuration/synchro
> du bloc RX autour de `0x600098xx/0x600099xx`. Tout le reste du pipeline CFO est
> désormais largement reconstruit sur les binaires exacts.

---



# 36BL. TX+RX parallèle — gating logiciel CFO exact et appelants `tone_control` — v0.47

Cette passe exploite le désassemblage Xtensa local du **corpus exact**.

Elle corrige une sur-interprétation de v0.46 côté RX et ferme plusieurs détails côté TX.

---

## 36BL.1 Correction RX : `phy_bb_rx_cfg()` est une routine d'initialisation

Recherche exhaustive des xrefs dans le corpus exact :

```text
phy_bb_rx_cfg
    offset = 0x2510
    taille = 0x406

xref unique :
    chip_v6_initialize_bb + 0xCF environ
```

Aucun appel par paquet, timer CFO ou traitement WDEV n'a été retrouvé.

Conclusion :

> la séquence observée dans `phy_bb_rx_cfg()` autour de `0x60009988.bit26` appartient
> à l'**initialisation/configuration du RX baseband**.

La séquence exacte reste :

```c
/* début de configuration */
REG32(0x60009988) &= ~0x04000000;

/* longue configuration RX */

/* fin de configuration */
REG32(0x60009988) |= 0x04000000;
```

Cela est compatible avec un :

```text
gate / reset / release du bloc RX
```

mais **pas** avec la preuve d'un trigger CFO par réception.

La priorité « trigger CFO » doit donc quitter `phy_bb_rx_cfg()`.

---

## 36BL.2 Correction exacte de `0x600098EC`

Le flot Xtensa exact de `phy_bb_rx_cfg()` donne :

```c
r = REG32(0x600098EC);
r &= 0x7FFFFFFF;
r |= 0x01900000;
REG32(0x600098EC) = r;
```

Donc :

```text
bit31 est explicitement effacé
puis la constante 0x01900000 est ORée
```

La v0.46 avait relié à tort cette constante à :

```text
0x190 = 400
```

Le vrai `movi 400` est stocké à :

```text
0x60009D18
```

et la même adresse reçoit plus tard :

```text
128
```

Cette correction retire une fausse piste de « fenêtre CFO = 400 ».

---

## 36BL.3 `0x600098DC` devient plus spécifique au readout CFO

Le corpus exact montre :

```text
phy_get_bb_freqoffset()
    lit 0x60009800
    puis fait 0x600098DC |= 0x0F

phy_get_bb_evm()
    lit également 0x60009800
    mais ne touche pas 0x600098DC
```

Un balayage des objets PHY/PP/WDEV n'a retrouvé aucun autre accès direct équivalent à
l'offset `0x98DC`.

Le ROM exact n'a pas non plus fourni, dans le balayage effectué, une séquence directe
équivalente vers ce registre.

Conclusion renforcée :

> `0x600098DC[3:0]` est désormais un candidat **CFO-spécifique** de
> post-traitement / acknowledge / clear / re-arm.

Le mot exact reste inconnu : aucune des quatre sémantiques suivantes n'est encore prouvée :

```text
acknowledge
clear valid
clear accumulator
re-arm estimator
```

Mais le contraste avec `phy_get_bb_evm()` rend l'hypothèse d'un simple « ack global des
métriques RX » moins probable.

---

## 36BL.4 `start_dig_rx()` / `stop_dig_rx()` ne pilotent pas directement le latch CFO

Les deux fonctions exactes :

```text
start_dig_rx
stop_dig_rx
```

travaillent principalement autour de :

```text
0x60009A00 + offsets
```

et aucun accès direct au couple :

```text
0x60009800
0x600098DC
```

n'a été identifié.

Conclusion :

> le latch CFO n'est pas trivialement armé par le simple start/stop logiciel du RX
> numérique.

Le trigger se situe probablement plus profondément dans le synchroniseur/estimateur BB.

---

## 36BL.5 Gating logiciel exact avant `phy_get_bb_freqoffset()`

Le désassemblage exact de :

```text
wDev_ProcessRxSucData()
```

révèle les conditions autour de l'appel.

Le premier octet du contrôle RX est chargé signé. Le DWARF/layout déjà fermé identifie ce
champ comme :

```text
RSSI
```

Le code compare ce RSSI à :

```text
-90
```

Puis il lit :

```text
TestStaFreqCalValOK
```

et n'appelle le getter CFO normal que lorsque :

```c
RSSI > -90
&& TestStaFreqCalValOK == 1
```

Pseudo-code de la branche démontrée :

```c
if (rxCtrl->rssi > -90 && TestStaFreqCalValOK == 1) {
    chl_freq_offset = phy_get_bb_freqoffset();
    ...
}
```

Cette condition est un **gating logiciel de consommation du CFO**.

Elle ne dit pas ce qui positionne matériellement :

```text
0x60009800.bit0
```

à 1.

---

## 36BL.6 Filtre logiciel de plage : −299 … +299

Après :

```c
chl_freq_offset = phy_get_bb_freqoffset();
```

le code sign-extend la valeur 16 bits puis applique deux bornes.

Le chemin d'ajustement est pris seulement si :

```text
chl_freq_offset <= +299
et
chl_freq_offset > -300
```

soit exactement :

```text
-299 ... +299
```

Dans cette fenêtre seulement :

```c
chl_freq_offset += TestStaFreqCalValDev;
```

Pour une valeur hors plage, le chemin d'ajout du `Dev` est sauté.

Cette plage est cohérente avec un garde-fou autour d'une mesure de fréquence en kHz,
mais son intention exacte doit rester décrite comme **filtre logiciel observé**.

---

## 36BL.7 États scan exacts et valeurs initiales

Dans le `ieee80211_scan.o` exact :

```text
TestStaFreqCalValOK
    .data
    offset 0x00
    taille 1 octet

FreqCalCntForScan
    .bss
    offset 0xAD
    taille 1 octet

TestStaFreqCalValInput
    .bss
    offset 0xAE
    taille 2 octets

TestStaFreqCalValDev
    .bss
    offset 0xB0
    taille 2 octets
```

La section `.data` commence avec :

```text
01
```

donc la valeur initiale démontrée de :

```text
TestStaFreqCalValOK = 1
```

est réelle dans ce build.

Comme `TestStaFreqCalValDev` est en BSS :

```text
valeur initiale = 0
```

Donc au démarrage standard :

```text
gating CFO autorisé
Dev ajouté = 0
```

tant que la logique de calibration/scan ne modifie pas ces états.

---

## 36BL.8 Ce que le gating WDEV change pour le RX FSK

Le pipeline devient :

```text
hardware / synchronisation BB
        ↓
0x60009800 bit0 + raw CFO
        ↓
          [gating WDEV]
        RSSI > -90
        TestStaFreqCalValOK == 1
        ↓
phy_get_bb_freqoffset()
        ↓
si -299..+299 :
    + TestStaFreqCalValDev
        ↓
esf_buf.chl_freq_offset
```

Il faut donc distinguer trois niveaux :

```text
A. hardware :
   le résultat existe-t-il et bit0 est-il valide ?

B. PHY getter :
   raw → kHz + sentinelle 0x7FFF + post-write 0x98DC

C. WDEV :
   décide quand appeler le getter et applique Dev
```

Pour un RX FSK autonome, la couche C peut être contournée si la couche A fonctionne sur
un tone arbitraire.

Le verrou demeure donc **A**, pas le seuil RSSI logiciel.

---

# TX — appelants exacts du générateur de tone

## 36BL.9 RXIQ utilise exactement `tone_control = 8`

Le chemin exact du binaire est :

```text
set_rx_gain_testchip_50()
        ↓
set_rx_gain_cal_iq(..., 8, ...)
        ↓
ram_rfcal_rxiq(..., 8, ...)
        ↓
rom_start_tx_tone(..., tone_control=8, ...)
```

Dans `set_rx_gain_testchip_50()`, juste avant l'appel :

```text
movi.n a2, 0
movi.n a3, 8
```

`set_rx_gain_cal_iq()` conserve ce deuxième argument puis le passe sans transformation
comme deuxième argument à `ram_rfcal_rxiq()`.

`ram_rfcal_rxiq()` le transmet ensuite au champ `tone_control` de `rom_start_tx_tone()`.

Conclusion :

> `8` est **exactement** le `tone_control` utilisé par la calibration RXIQ dans ce
> corpus, pas une simple constante voisine.

---

## 36BL.10 Les calibrations puissance / TX-cap utilisent exactement `64`

Trois chemins exacts indépendants utilisent `64`.

### `tx_cap_init()` → power control

Avant :

```text
ram_rfcal_pwrctrl
```

on observe :

```text
movi.n a2, 64
```

`ram_rfcal_pwrctrl()` conserve son premier argument et le transmet comme `tone_control`
à `rom_start_tx_tone()`.

Donc :

```text
tx_cap_init
    → ram_rfcal_pwrctrl(tone_control=64)
```

### `tx_cap_init()` → TX-cap

Le même `tx_cap_init()` charge à nouveau :

```text
64
```

avant :

```text
ram_rfcal_txcap
```

et `ram_rfcal_txcap()` transmet son premier argument au tone generator.

Donc :

```text
tx_cap_init
    → ram_rfcal_txcap(tone_control=64)
```

### `tx_pwctrl_init_cal()` → power control

Une troisième occurrence exacte :

```text
tx_pwctrl_init_cal
    → movi.n a2,64
    → ram_rfcal_pwrctrl
```

confirme encore le même stimulus pour la calibration de puissance.

---

## 36BL.11 Signification fonctionnelle des deux valeurs maintenant fermée

Le corpus exact démontre donc :

```text
tone_control = 8
    → calibration RXIQ

tone_control = 64
    → calibration puissance TX
    → calibration TX-cap
```

Cela ne fournit toujours pas :

```text
8  → X MHz
64 → Y MHz
```

mais la séparation fonctionnelle est désormais certaine.

Le ratio :

```text
64 / 8 = 8
```

reste compatible avec un step/incrément numérique linéaire, sans constituer une preuve
de linéarité fréquentielle.

---

## 36BL.12 ROM exact : aucune conversion `tone_control → Hz`

Le ROM fourni a été redésassemblé directement à :

```text
rom_start_tx_tone = 0x400068B4
```

Le comportement utile est inchangé :

```text
enable TX clock
RMW du slot 0x600005B8 / BC / C4
OR direct du tone_control fourni par l'appelant
packing indépendant de digital_scale et mode
```

Aucune :

```text
multiplication
division
table de fréquence
conversion Hz
```

n'est appliquée au `tone_control`.

Conclusion :

> l'échelle `tone_control → fréquence physique` est située derrière l'interface
> matérielle du générateur de tone.

La recherche statique des bibliothèques/ROM peut préciser les appelants, mais ne peut
plus fournir à elle seule la fréquence RF exacte des valeurs 8/64.

---

## 36BL.13 Recoupement inter-générations conservé, sans sur-promesse

Les générations Espressif suivantes exposent séparément des symboles de type :

```text
start_tx_tone_step
start_tx_tone
set_rf_freq_offset
rfpll_set_freq
```

Ce découpage renforce l'interprétation :

```text
tone_control ≈ step / incrément numérique
```

mais ne fournit aucune équation ABI valable automatiquement pour l'ESP8266.

La formule devra donc être mesurée sur silicium ou retrouvée dans une documentation
matérielle non encore disponible.

---

## 36BL.14 Nouvelle hiérarchie des verrous

### TX

Le verrou est désormais essentiellement physique :

```text
tone_control 8  → mesurer Δf8
tone_control 64 → mesurer Δf64
```

puis seulement tester :

```text
1, 2, 4, 16, 32
```

pour établir la loi.

Le hot-update doit être mesuré séparément :

```text
8 ↔ 64
```

sans arrêt TX clock / XPD / PLL.

### RX

Le verrou n'est plus la configuration statique du BB.

Il est :

```text
qu'est-ce qui provoque matériellement :
    0x60009800.bit0 = 1
et
    mise à jour de bits15:8 ?
```

Le meilleur candidat conceptuel est maintenant le **synchroniseur / estimateur de
réception PHY**, pas `phy_bb_rx_cfg()`.

---

## 36BL.15 Pourcentages après v0.47

```text
TX FSK architecture générale               ≈ 92 %
appelants exacts tone_control               ≈ 98 %
signification fonctionnelle 8 / 64          ≈ 95 %
loi tone_control → Hz                       ≈ 45 %
hot-update physique                         ≈ 65 %
TX FSK rapide global                        ≈ 84 %

registre/formule CFO RX                     ≈ 99 %
gating logiciel WDEV                        ≈ 98 %
états TestStaFreqCalVal*                    ≈ 98 %
post-write 0x600098DC CFO-spécifique        ≈ 80 %
trigger hardware CFO                        ≈ 58 %
CFO hors packet-valid                       ≈ 50 %
RX FSK autonome global                      ≈ 81 %
```

L'amélioration est modeste en pourcentage global parce que la recherche a surtout
**corrigé les faux candidats** et réduit l'espace de recherche.

Conclusion v0.47 :

> **TX :** les valeurs `8` et `64` sont maintenant reliées exactement à leurs chemins
> RXIQ et puissance/TX-cap, mais aucune formule logicielle vers les Hz n'existe.
> **RX :** `phy_bb_rx_cfg()` est reclassée comme configuration d'initialisation ;
> le vrai chemin de lecture CFO normal est gated par `RSSI > -90` et
> `TestStaFreqCalValOK == 1`, puis corrigé par `TestStaFreqCalValDev` seulement dans
> la fenêtre `-299…+299`. Le post-write `0x600098DC |= 0x0F` devient le meilleur
> candidat logiciel lié au re-arm/ack du CFO, tandis que le mécanisme hardware qui
> produit `0x60009800.bit0` reste le dernier verrou RX majeur.

---



# 36BM. CFO RX — handshake de calibration PP et timer 1000 ms — v0.48

Cette passe ferme presque entièrement la **machine logicielle de calibration** située
au-dessus du readout CFO hardware.

Le point essentiel est désormais clair :

```text
DefFreqCalTimerCB
```

n'est pas un trigger du matériel CFO.

C'est seulement un callback logiciel qui réautorise une calibration de fréquence dans PP.

---

## 36BM.1 Corps exact de `DefFreqCalTimerCB()`

Dans le `pp.o` exact :

```text
DefFreqCalTimerCB
    offset = 0x1384
    taille = 0x0A
```

Le corps décodé est équivalent à :

```c
static void DefFreqCalTimerCB(void *arg)
{
    CanDoFreqCal = 1;
}
```

Instructions exactes :

```text
l32r   a3, .data_base
movi.n a2, 1
s8i    a2, a3, 20
ret.n
```

Or :

```text
CanDoFreqCal
    .data + 0x14
    taille = 1 octet
```

Donc :

```text
.data_base + 20 = CanDoFreqCal
```

est démontré sans ambiguïté.

---

## 36BM.2 État initial de la logique de calibration

La section `.data` exacte de `pp.o` contient :

```text
offset 0x14 : 0x01  → CanDoFreqCal
offset 0x15 : 0x01  → test_freq_val_first
```

Donc au démarrage du build analysé :

```c
CanDoFreqCal       = 1;
test_freq_val_first = 1;
```

La calibration n'attend donc pas le premier callback timer pour devenir autorisée.

Le timer sert à **réautoriser ultérieurement** une calibration après que PP a volontairement
mis `CanDoFreqCal` à zéro.

---

## 36BM.3 `HdlChlFreqCal()` vérifie réellement `CanDoFreqCal`

Dans `HdlChlFreqCal()`, un littéral pointe vers la même base `.data`.

Le code lit :

```text
.data_base + 20
```

donc :

```c
CanDoFreqCal
```

et évite la branche de correction lorsque ce flag n'autorise pas une nouvelle calibration.

Cela confirme la sémantique fonctionnelle du nom :

```text
CanDoFreqCal = permission logicielle de lancer/appliquer une calibration fréquence
```

et non :

```text
enable hardware du mesureur CFO
```

---

## 36BM.4 Handshake exact autour de `chip_v6_set_chan_offset()`

Dans la branche qui applique une correction, le code exact effectue :

```c
TestStaFreqCalValOK = 0;

chip_v6_set_chan_offset(
    current_channel,
    TestStaFreqCalValInput
);

TestStaFreqCalValOK = 1;
TestStaFreqCalValDev = TestStaFreqCalValInput;
```

L'ordre est important.

### Avant le retuning

```text
TestStaFreqCalValOK = 0
```

bloque le chemin WDEV normal qui, comme démontré en v0.47, appelle
`phy_get_bb_freqoffset()` seulement lorsque :

```text
TestStaFreqCalValOK == 1
```

### Pendant le retuning

```text
chip_v6_set_chan_offset(...)
```

applique la correction fréquence au canal/PLL.

### Après le retuning

Le code fait :

```text
TestStaFreqCalValOK = 1
```

puis :

```text
TestStaFreqCalValDev = TestStaFreqCalValInput
```

La lecture CFO WDEV redevient alors autorisée avec la déviation appliquée mémorisée.

Conclusion :

> `TestStaFreqCalValOK` agit comme un **verrou de cohérence logiciel** empêchant WDEV de
> consommer une mesure CFO pendant que la référence fréquence est en train d'être modifiée.

---

## 36BM.5 Pourquoi WDEV ajoute `TestStaFreqCalValDev`

La v0.47 a démontré le code WDEV :

```c
if (-299 <= cfo && cfo <= 299) {
    cfo += TestStaFreqCalValDev;
}
```

La v0.48 démontre maintenant que :

```text
TestStaFreqCalValDev
    ← TestStaFreqCalValInput
```

juste après l'application de cet offset via :

```text
chip_v6_set_chan_offset()
```

Le modèle fonctionnel devient donc :

```text
CFO BB mesuré après correction
        +
offset de correction mémorisé
        =
CFO logiciel référencé au canal nominal
```

Cette interprétation est très fortement soutenue par le flot exact.

Le signe physique absolu reste dépendant de la convention utilisée dans
`chip_v6_set_chan_offset()`, mais l'opération logicielle est bien une addition.

---

## 36BM.6 `CanDoFreqCal` est remis à zéro après le cycle

Après la branche de correction et la remise à zéro des agrégats, `HdlChlFreqCal()`
écrit :

```c
CanDoFreqCal = 0;
```

Le code utilise de nouveau :

```text
.data_base + 20
```

mais cette fois avec la valeur zéro.

On obtient donc une vraie machine d'état :

```text
CanDoFreqCal = 1
        ↓
collecte / décision CFO
        ↓
correction éventuelle
        ↓
CanDoFreqCal = 0
        ↓
cooldown
        ↓
DefFreqCalTimerCB
        ↓
CanDoFreqCal = 1
```

---

## 36BM.7 Timer one-shot de 1000 ms

Sur la branche où :

```text
pm_is_open() == 0
```

le code exact effectue :

```c
ets_timer_disarm(&DefFreqCalTimer);

ets_timer_setfn(
    &DefFreqCalTimer,
    DefFreqCalTimerCB,
    NULL
);

ets_timer_arm_new(
    &DefFreqCalTimer,
    1000,
    0,
    1
);
```

L'API officielle NONOS définit :

```c
void ets_timer_arm_new(
    os_timer_t *ptimer,
    uint32_t time,
    bool repeat_flag,
    bool ms_flag
);
```

avec :

```text
ms_flag = 1 → unité milliseconde
repeat_flag = 0 → one-shot
```

Le timer est donc :

```text
1000 ms
one-shot
```

soit un **cooldown logiciel d'environ une seconde** sur cette branche.

Ce résultat explique pourquoi cette infrastructure ne doit jamais être utilisée comme
preuve d'un discriminateur FSK symbole-par-symbole.

---

## 36BM.8 Une branche de calibration utilise des pas de 40 unités

Dans la partie de `HdlChlFreqCal()` qui ajuste :

```text
TestStaFreqCalValInput
```

le désassemblage exact contient :

```text
+40
-40
division par 40
re-multiplication par 40
```

et un clamp local compatible avec :

```text
-320 ... 0
```

sur cette branche particulière.

Comme :

```text
TestStaFreqCalValInput
```

est ensuite :

1. passé à `chip_v6_set_chan_offset()`,
2. recopié dans `TestStaFreqCalValDev`,
3. ajouté au CFO en kHz par WDEV,

le domaine est très probablement celui du même offset fréquence logiciel.

Donc, pour cette branche :

```text
pas de correction observé ≈ 40 kHz
plage locale observée      ≈ -320 ... 0 kHz
```

Le qualificatif **branche locale** est important.

Il serait incorrect d'en faire une règle universelle pour tous les modes BBPLL,
AP/STA ou toutes les APIs de correction.

---

## 36BM.9 Distinction définitive des trois temporalités

On peut maintenant séparer trois échelles temporelles totalement différentes.

### A — mesure CFO hardware

```text
0x60009800
```

produit la mesure liée à la réception PHY.

Sa cadence réelle hardware reste à déterminer.

### B — lecture/transport par trame

```text
wDev_ProcessRxSucData()
    ↓
phy_get_bb_freqoffset()
    ↓
esf_buf.chl_freq_offset
```

se fait dans le pipeline RX réussi.

### C — correction lente de référence

```text
HdlChlFreqCal()
    ↓
chip_v6_set_chan_offset()
    ↓
CanDoFreqCal=0
    ↓
timer ~1000 ms
```

est une boucle de calibration lente.

Conclusion :

> les performances de la boucle C ne donnent aucune limite directe sur la cadence
> intrinsèque du mesureur CFO A.

C'est une distinction centrale pour le projet FSK RX.

---

## 36BM.10 Conséquence pour un RX FSK autonome

La boucle standard Espressif peut être presque entièrement ignorée pour la future
démodulation FSK.

Le chemin intéressant est :

```text
RF / BB
  ↓
0x60009800.bit0
0x60009800[15:8]
  ↓
conversion ×107/64
```

Les éléments :

```text
CanDoFreqCal
DefFreqCalTimerCB
TestStaFreqCalValInput
TestStaFreqCalValDev
chip_v6_set_chan_offset
```

appartiennent surtout à la **maintenance lente de la référence fréquence Wi-Fi**.

Le dernier verrou RX demeure donc :

> qu'est-ce qui fait produire au baseband une nouvelle valeur valide dans
> `0x60009800` ?

---

## 36BM.11 Impact sur les pourcentages

```text
boucle software de frequency calibration      ≈ 98 %
rôle DefFreqCalTimerCB                        100 %
rôle CanDoFreqCal                             ≈ 99 %
handshake TestStaFreqCalValOK                 ≈ 99 %
rôle TestStaFreqCalValDev                     ≈ 95 %
cadence correction standard                   ≈ 95 %
trigger matériel CFO                          ≈ 58 %
RX FSK autonome global                        ≈ 82 %
```

L'augmentation du RX FSK est limitée : cette passe ferme surtout la logique de
maintenance, pas le trigger hardware.

Conclusion v0.48 :

> La calibration frequency-offset du SDK est désormais clairement séparée du mesureur :
> **`DefFreqCalTimerCB()` ne fait que `CanDoFreqCal=1` après un cooldown one-shot de
> 1000 ms.** Lors d'un retuning, PP fait `TestStaFreqCalValOK=0`, applique
> `chip_v6_set_chan_offset`, mémorise l'offset dans `TestStaFreqCalValDev`, puis remet
> `OK=1`. WDEV ajoute ensuite cette déviation au CFO résiduel. Le dernier problème RX
> n'est donc plus la boucle de calibration : c'est exclusivement le **trigger hardware
> du résultat CFO**.

---



# 36BN. TX+RX parallèle — consommation CFO sur RX-success et nouveau recoupement tone=64 — v0.49

Cette passe poursuit les deux axes sur les binaires exacts.

Deux résultats nouveaux sont ajoutés :

```text
RX :
    le getter CFO standard est appelé depuis la branche RX-success de wDev_ProcessFiq,
    tandis qu'une branche adjacente élimine explicitement la trame avec wDev_DiscardFrame.

TX :
    meas_tone_pwr_db() démarre elle aussi un tone avec tone_control=64,
    ce qui renforce encore l'association de 64 aux calibrations de puissance.
```

---

## 36BN.1 `wDev_ProcessFiq()` : bifurcation exacte discard vs RX-success

Dans le `wdev.o` exact :

```text
wDev_ProcessFiq
    source DWARF : wdev.c:1072
```

La fin du chemin RX contient deux appels distincts.

Branche 1 :

```text
wDev_DiscardFrame(...)
```

avec relocation autour de :

```text
fonction + 0x3E5
```

Branche 2 :

```text
wDev_ProcessRxSucData(...)
```

avec relocation autour de :

```text
fonction + 0x3FC
```

Le DWARF associe précisément cette zone aux lignes :

```text
wdev.c:1179 → branche discard
wdev.c:1186 → préparation de la branche succès
wdev.c:1187 → retour immédiat après l'appel RX-success
```

Un stack dump public d'un SDK 3.0.x recoupe indépendamment :

```text
wDev_ProcessRxSucData  → wdev.c:603
wDev_ProcessFiq        → wdev.c:1187
```

Conclusion de haute confiance :

> dans le chemin logiciel standard, le CFO n'est pas lu par une tâche périodique ou
> un sampler libre. Il est consommé depuis la branche de réception que WDEV considère
> suffisamment valide pour entrer dans `wDev_ProcessRxSucData()`.

---

## 36BN.2 Validation de chaîne de blocs avant la branche succès

Le code immédiatement avant cette bifurcation parcourt les descripteurs/blocs RX.

Le DWARF de `wDev_ProcessFiq()` donne notamment les variables locales :

```text
head
tail
nblks
rxstart_time
tmp
tmp_next
tmp_blk_num
```

Dans la zone source autour de :

```text
wdev.c:1167–1187
```

le code :

1. initialise un compteur temporaire ;
2. parcourt une chaîne via le champ `next` ;
3. inspecte un champ de status dans le descripteur ;
4. choisit ensuite entre :
   - `wDev_DiscardFrame()`,
   - `wDev_ProcessRxSucData()`.

La sémantique exacte de tous les bits de descripteur n'est pas encore nommée.

Mais la conclusion importante est démontrée :

> l'appel qui mène au CFO arrive **après une validation du chemin RX/descripteurs**, pas
> simplement à chaque interruption FIQ.

---

## 36BN.3 Ce que cela prouve — et ce que cela ne prouve pas

### Démontré

Le SDK standard suit :

```text
FIQ / RX event
    ↓
validation des descripteurs RX
    ↓
branche acceptée
    ↓
wDev_ProcessRxSucData()
    ↓
phy_get_bb_freqoffset()
```

### Non démontré

Cela ne suffit pas à prouver :

```text
0x60009800 ne peut jamais être mis à jour sans packet Wi-Fi valide.
```

Le hardware peut théoriquement mettre à jour le CFO plus tôt :

```text
préambule reconnu
synchroniseur verrouillé
estimateur carrier/frequency actif
```

puis le logiciel ne lire le résultat qu'après validation plus tardive.

Conclusion correcte :

```text
consommation SDK = packet/RX-success coupled
production hardware = encore ouverte
```

Cette distinction doit rester stricte.

---

## 36BN.4 Nouvelle conséquence expérimentale RX

Le test autonome doit désormais contourner deux couches distinctes.

### Couche logicielle

Elle peut être ignorée :

```text
RSSI > -90
TestStaFreqCalValOK
wDev_ProcessRxSucData
```

si le registre hardware est lu directement.

### Couche hardware

Il faut déterminer ce qui provoque :

```text
0x60009800.bit0 = 1
```

et rafraîchit :

```text
0x60009800[15:8]
```

Le test discriminant devient donc :

```text
A. paquet 802.11 valide
B. préambule/trame incomplète
C. tone continu non-802.11
```

et observer uniquement le registre/resultat.

L'objectif n'est plus de reproduire toute la pile WDEV/PP.

---

# TX — renforcement du rôle de `tone_control=64`

## 36BN.5 `meas_tone_pwr_db()` utilise exactement 64

Dans le `phy_chip_v6_cal.o` exact :

```text
meas_tone_pwr_db
    offset = 0x504
    taille = 0x6D
```

Le désassemblage exact commence par préparer :

```text
a2 = 1
a3 = 64
```

avant un appel indirect via la table PHY correspondant au démarrage du tone.

La routine mesure ensuite la puissance du tone, effectue deux acquisitions/accumulations,
puis arrête le tone.

Conclusion :

> `meas_tone_pwr_db()` utilise elle aussi **exactement `tone_control=64`**.

Ce résultat est indépendant des trois chemins déjà identifiés :

```text
tx_cap_init → ram_rfcal_pwrctrl(64)
tx_cap_init → ram_rfcal_txcap(64)
tx_pwctrl_init_cal → ram_rfcal_pwrctrl(64)
```

---

## 36BN.6 Carte fonctionnelle actuelle des valeurs 8 et 64

Le corpus exact fournit maintenant :

```text
tone_control = 8
    → RXIQ calibration

tone_control = 64
    → TX power calibration
    → TX-cap calibration
    → direct tone-power measurement
    → autres chemins TXIQ déjà identifiés dans le maître
```

Cette distribution fonctionnelle rend très forte l'interprétation :

```text
64 = stimulus tone "standard" de puissance/TX
8  = stimulus distinct pour RXIQ
```

mais ne démontre toujours pas :

```text
64 Hz
64 kHz
64 × constante
```

La valeur reste un **code matériel de step/control**.

---

## 36BN.7 Pourquoi `meas_tone_pwr_db()` ne donne toujours pas la loi en Hz

La routine :

```text
meas_tone_pwr_db()
```

ne calcule aucune fréquence.

Elle :

1. démarre le tone avec code `64`,
2. déclenche une mesure de puissance,
3. accumule/convertit cette puissance,
4. arrête le tone.

Il n'existe pas dans cette fonction de :

```text
table code→Hz
multiplication de tone_control
division par clock
conversion MHz/kHz
```

Donc cette nouvelle preuve **renforce l'usage de 64**, mais n'ajoute aucune équation
physique.

---

## 36BN.8 Recoupement inter-générations maintenu

Les ROM Espressif suivantes exposent séparément :

```text
rom_start_tx_tone_step
rom_start_tx_tone
rom_set_rf_freq_offset
rom_rfpll_set_freq
```

et les PHY ultérieurs exposent aussi :

```text
rom_phy_get_rx_freq
```

Cette séparation reste compatible avec notre architecture :

```text
TX tone step numérique
    ≠
RF frequency correction / PLL

RX CFO measurement
    ≠
slow frequency correction
```

Mais aucune constante d'une génération ultérieure ne doit être transposée à l'ESP8266
sans validation.

---

## 36BN.9 Statut v0.49

```text
RX standard CFO consumption packet/RX-success coupled   ≈ 98 %
RX hardware CFO production timing                       ≈ 60 %
CFO direct register/formula                             ≈ 99 %
RX FSK autonome global                                  ≈ 82 %

TX tone callers exacts                                  ≈ 99 %
rôle fonctionnel tone_control=64                        ≈ 98 %
rôle fonctionnel tone_control=8                         ≈ 97 %
loi tone_control→Hz                                     ≈ 45 %
TX fast-FSK global                                      ≈ 84 %
```

Conclusion v0.49 :

> Le **SDK standard** ne consomme le CFO qu'après validation du chemin RX et entrée dans
> `wDev_ProcessRxSucData`; cela rend le pipeline logiciel clairement packet-coupled.
> Le dernier doute concerne uniquement le **moment où le baseband produit le résultat** :
> ce résultat peut encore apparaître avant la validation MAC. Côté TX, `64` est désormais
> confirmé dans une quatrième famille de mesure/calibration (`meas_tone_pwr_db`), mais
> la conversion `tone_control→Hz` reste derrière la frontière hardware.

---



# 36BO. TX+RX parallèle — validité CFO spécifique, EVM partagé et plafond statique du tone-step — v0.50

Cette passe affine la sémantique du registre :

```text
0x60009800
```

et corrige une ambiguïté restante : le bit de validité testé par
`phy_get_bb_freqoffset()` ne doit pas être interprété comme un simple
« registre RX globalement valide ».

Le même mot matériel contient aussi une métrique EVM, mais le getter EVM suit une
séquence différente et n'utilise pas ce bit.

En parallèle, le chantier TX confirme que `tone_control` est arrivé à une véritable
**frontière hardware** : plusieurs usages indépendants fixent les valeurs 8 et 64,
mais aucune équation `control → Hz` n'existe dans les binaires exacts.

---

## 36BO.1 `phy_get_bb_evm()` partage exactement `0x60009800`

Dans le `phy_chip_v6_cal.o` exact :

```text
phy_get_bb_freqoffset
    offset = 0x1A8
    taille = 0x67

phy_get_bb_evm
    offset = 0x228
    taille = 0x5F
```

`phy_get_bb_evm()` lit le même registre :

```text
0x60009800
```

mais extrait :

```text
bits 28:16
```

sur 13 bits.

Le getter CFO, lui, exploite :

```text
bit 0       → condition de validité CFO
bits 15:8   → CFO brut signé 8 bits
```

On peut donc décrire le mot observé comme :

```text
31        29 28                 16 15          8 7      1 0
┌───────────┬────────────────────┬──────────────┬────────┬─┐
│  ouvert   │ métrique EVM       │ CFO brut     │ ouvert │V│
│           │ 13 bits observés   │ signed 8 bit │        │ │
└───────────┴────────────────────┴──────────────┴────────┴─┘
```

Le nom du bit `V` reste volontairement fonctionnel :

```text
condition de validité utilisée par le getter CFO
```

et non un nom de registre officiel Espressif.

---

## 36BO.2 Le getter EVM ne teste pas `0x60009800.bit0`

Le corps exact de `phy_get_bb_evm()` :

1. prépare/configure un autre sous-bloc ;
2. choisit une constante selon un état matériel ;
3. lit `0x60009800` ;
4. extrait directement `bits 28:16` ;
5. retourne la valeur.

Aucun test de :

```text
0x60009800.bit0
```

n'est effectué avant l'extraction de l'EVM.

Conclusion :

> le logiciel Espressif traite `bit0` comme une condition propre au chemin CFO,
> ou au minimum comme une condition que **seul le getter CFO exige**.

Il serait donc trop fort d'écrire :

```text
bit0 = toutes les métriques RX sont valides
```

Cette interprétation n'est pas supportée.

---

## 36BO.3 Préparation spécifique du getter EVM

Avant de lire `0x60009800`, `phy_get_bb_evm()` effectue une séquence distincte du CFO.

Les accès décodés comprennent :

```text
0x60000D48
```

avec une séquence RMW sur le bit 0, puis :

```text
0x3FF00058[15:12]
```

qui choisit entre deux constantes écrites vers :

```text
0x60009D74
```

Les constantes observées sont :

```text
0xE690A568
0xEAB4D027
```

Le rôle physique exact de ces valeurs n'est pas encore nommé.

Le fait important est structurel :

> EVM et CFO partagent un registre de résultat, mais **leurs séquences de lecture et de
> préparation ne sont pas identiques**.

---

## 36BO.4 `0x600098DC |= 0xF` reste spécifique au chemin CFO observé

Dans les accès MMIO localisés dans les objets exacts :

```text
phy_get_bb_freqoffset()
    lit   0x60009800
    lit   0x600098DC
    écrit 0x600098DC

phy_get_bb_evm()
    lit   0x60009800
    ne touche pas 0x600098DC
```

La séquence CFO est :

```c
r = REG32(0x600098DC);
r |= 0x0F;
REG32(0x600098DC) = r;
```

Cela renforce le classement :

```text
0x600098DC[3:0]
    → contrôle / acknowledge / clear / re-arm associé au CFO
```

mais le sous-rôle exact reste ouvert.

Aucune des trois étiquettes :

```text
ACK
CLEAR
REARM
```

ne doit encore être promue en fait.

---

## 36BO.5 Deux gates successifs avant de produire un CFO valide

Le comportement exact de `phy_get_bb_freqoffset()` contient deux conditions indépendantes.

### Gate A — état WDEV

```text
0x3FF2003C[19:16] < 8
```

Sinon :

```text
CFO = 0x7FFF
```

### Gate B — résultat baseband

```text
0x60009800.bit0 == 1
```

Sinon :

```text
CFO = 0x7FFF
```

Puis seulement :

```text
raw = signed8(0x60009800[15:8])
CFO = (raw * 107) >> 6
```

Le modèle devient donc :

```text
état WDEV acceptable ?
        ↓ oui
résultat CFO BB valide ?
        ↓ oui
extraire signed8
        ↓
conversion kHz
```

---

## 36BO.6 `0x3FF2003C` appartient au bloc WDEV, mais son champ reste sans nom

Les reconstructions publiques du matériel ESP8266 placent :

```text
WDEV_BASE = 0x3FF20000
```

donc :

```text
0x3FF2003C = WDEV_BASE + 0x3C
```

Dans le corpus exact, aucun autre consommateur logiciel de cette adresse précise n'a été
identifié au cours du traçage du CFO.

Le getter traite uniquement :

```text
bits 19:16
```

et invalide le CFO si la valeur est :

```text
>= 8
```

Cela suggère un état matériel de réception/modulation/contexte, mais aucune preuve
ne permet actuellement de choisir entre :

```text
état RX
mode PHY
rate/modulation
phase de réception
autre champ de contrôle WDEV
```

Ce champ devient donc le **gate WDEV inconnu n°1** du chantier RX.

---

## 36BO.7 Le producteur du CFO reste hardware, pas logiciel

Le scan des accès exacts confirme :

```text
0x60009800
    → lectures par phy_get_bb_freqoffset()
    → lecture par phy_get_bb_evm()
```

sans écriture CPU normale identifiée vers ce registre de résultat.

Le pipeline logiciel standard n'écrit donc pas :

```text
CFO raw
```

dans le registre.

Modèle actuel :

```text
synchroniseur / démodulateur baseband
        ↓
hardware produit résultat CFO/EVM
        ↓
0x60009800
        ↓
CPU lit / valide / transporte
```

C'est cohérent avec la localisation de `phy_get_bb_freqoffset()` dans le chemin
RX réussi WDEV.

---

## 36BO.8 Consommation CFO toujours couplée au RX-success standard

La v0.49 a montré que :

```text
wDev_ProcessFiq()
```

sépare les chemins :

```text
discard
```

et :

```text
wDev_ProcessRxSucData()
```

et que le getter CFO est appelé depuis ce dernier.

Le résultat v0.50 ne change donc pas cette conclusion :

> le SDK standard **consomme** le CFO après une réception qualifiée comme réussie.

Il ne prouve toujours pas que le hardware **produit** le CFO seulement à cet instant.

La différence entre :

```text
production hardware
```

et :

```text
consommation software
```

reste essentielle.

---

## 36BO.9 Expérience RX qui localise le stade exact de production CFO

Le test matériel peut maintenant être beaucoup plus discriminant.

Utiliser trois stimuli RF contrôlés :

```text
A. trame Wi-Fi valide
B. signal qui synchronise le PHY mais finit en trame rejetée / FCS invalide
C. tone / porteuse non-802.11
```

Pour chaque cas, enregistrer directement :

```text
state = (REG32(0x3FF2003C) >> 16) & 0xF
bb    = REG32(0x60009800)

wdev_gate  = state < 8
cfo_valid  = bb & 1
cfo_raw    = (int8_t)((bb >> 8) & 0xFF)
evm_raw    = (bb >> 16) & 0x1FFF
```

Puis reproduire seulement après le snapshot la séquence standard :

```text
REG32(0x600098DC) |= 0x0F
```

Interprétation :

```text
A seulement valide
    → résultat probablement couplé à une validation tardive du paquet

A + B valides
    → résultat produit après synchro/démodulation, avant validation MAC/FCS

A + B + C valides
    → estimateur potentiellement autonome / exploitable directement pour FSK
```

Ce test distingue beaucoup mieux les hypothèses que la simple lecture après un paquet.

---

# TX — poursuite parallèle

## 36BO.10 Nouveau recoupement exact : `tone_control = 64` dans `meas_tone_pwr_db()`

Dans le `phy_chip_v6_cal.o` exact :

```text
meas_tone_pwr_db
    offset = 0x504
    taille = 0x6D
```

la préparation de l'appel au générateur de tone place explicitement :

```text
mode/control path utilisant la valeur 64
```

avant la mesure de puissance.

Cela ajoute un usage indépendant aux usages déjà connus.

Le tableau des valeurs observées devient :

```text
RXIQ calibration        → tone_control = 8

TXIQ stimulus           → tone_control = 64
TX power calibration    → tone_control = 64
TX-cap calibration      → tone_control = 64
TX power-control init   → tone_control = 64
meas_tone_pwr_db        → tone_control = 64
```

Donc `64` est clairement la valeur standard du tone de test TX dans plusieurs chemins
indépendants du SDK.

---

## 36BO.11 Ce que le rapport 8 ↔ 64 permet et ne permet pas de conclure

Les deux seules valeurs distinctes démontrées sont :

```text
8
64
```

avec un rapport :

```text
64 / 8 = 8
```

Cela démontre seulement que le hardware accepte plusieurs valeurs et que les calibrations
choisissent des steps différents.

Cela **ne démontre pas** :

```text
f(64) = 8 × f(8)
```

même si cette loi linéaire est une hypothèse naturelle pour un NCO/phase-step.

Aucune équation :

```text
tone_control × constante
```

n'apparaît dans les binaires exacts.

---

## 36BO.12 Hypothèses TX à garder uniquement comme tests

Un générateur numérique à phase-step possède normalement une loi de type :

```text
f = step × Fclk / 2^N
```

Le packing observé donne un espace naturel de 10 bits pour le contrôle, mais la ROM ne
masque pas explicitement l'argument à 10 bits.

Il serait donc tentant de tester des hypothèses comme :

```text
N = 10
Fclk = clock baseband connue
```

mais **aucune de ces constantes n'est démontrée pour le tone generator ESP8266**.

Ces équations doivent être utilisées uniquement pour choisir des points de mesure,
jamais comme résultat du reverse-engineering statique.

---

## 36BO.13 Plafond actuel du reverse-engineering TX statique

Les binaires exacts permettent maintenant de fermer :

```text
registre
packing
slots
gate
amplitude
valeurs de contrôle utilisées
séparation d'avec RFPLL
chemins d'appel
```

mais pas :

```text
step → Hz
phase accumulator interne
clock exacte du tone generator
wrap/modulo matériel
latence de propagation du nouveau step
```

Conclusion :

> sans mesure RF ou documentation matérielle supplémentaire, `tone_control → Hz`
> est désormais à considérer comme **plafond statique atteint**.

La recherche logicielle reste utile pour confirmer les usages et le hot-update, mais
la calibration physique devient l'outil principal.

---

## 36BO.14 État après v0.50

| Bloc | Compréhension estimée |
|---|---:|
| CFO registre / formule | **~99 %** |
| CFO validité logicielle | **~99 %** |
| `0x60009800.bit0` = condition utilisée par CFO | **~99 %** |
| bit0 = validité globale RX | **non supporté** |
| rôle exact `0x600098DC[3:0]` | **~65 % structurel, fonction précise ouverte** |
| gate WDEV `0x3FF2003C[19:16]` | **~70 % structurel, sémantique ouverte** |
| stade hardware de production CFO | **~65 %** |
| CFO sur tone non-Wi-Fi | **~50 %** |
| RX FSK autonome global | **~83 %** |
| TX FSK architecture | **~93 %** |
| `tone_control` = step/stimulus numérique | **~92 %** |
| loi `tone_control → Hz` | **~45 %** |
| plafond statique TX atteint | **haute confiance** |

Conclusion v0.50 :

> Le mot `0x60009800` est un conteneur de plusieurs métriques RX. Le logiciel Espressif
> traite explicitement `bit0` comme condition requise par le CFO, alors que l'EVM
> `[28:16]` est lue sans ce test et sans l'acquittement `0x600098DC|=0xF`.
> Le CFO dépend donc de deux gates : un état WDEV inconnu dans
> `0x3FF2003C[19:16]` et un état hardware dans `0x60009800.bit0`.
> Côté TX, les usages indépendants de `tone_control=64` se multiplient, mais aucune
> formule en Hz n'existe dans le logiciel : la prochaine fermeture TX est désormais
> nécessairement expérimentale.

---



# 36BP. RX/TX parallèle — gate WDEV isolé et sous-bloc CFO/EVM distingué des calibrations ROM — v0.51

Cette passe poursuit la localisation matérielle du CFO par deux scans indépendants :

```text
1. cartographie des accès WDEV dans le `wdev.o` exact ;
2. cartographie des accès au bloc PHY `0x60009600` dans la ROM exacte.
```

Le résultat principal est négatif mais très utile :

> le champ WDEV `0x3FF2003C[19:16]` lu par `phy_get_bb_freqoffset()` n'est pas
> manipulé par les routines WDEV normales retrouvées dans le corpus, et les routines
> ROM qui utilisent le même bloc PHY ne touchent ni `0x60009800` ni `0x600098DC`.

Le couple :

```text
0x60009800
0x600098DC
```

devient donc encore plus spécifique au chemin de métriques RX/CFO que ne le laissait
penser la simple proximité d'adresses.

---

## 36BP.1 Voisinage WDEV exact autour de `0x3FF2003C`

Le getter CFO calcule son premier gate depuis :

```text
base    = 0x3FF1FE00
offset  = 0x23C
adresse = 0x3FF2003C
```

et extrait :

```text
bits 19:16
```

Le scan des accès de `wdev.o` utilisant la même base `0x3FF1FE00` retrouve de nombreux
registres voisins, notamment :

```text
0x3FF20000
0x3FF20004
0x3FF20008
0x3FF2000C
0x3FF20010
0x3FF2006C
0x3FF2007C
0x3FF20080
0x3FF20084
0x3FF20088
0x3FF20178
```

mais pas :

```text
0x3FF2003C
```

Cette absence est significative dans le corpus observé.

---

## 36BP.2 Fonctions associées aux registres WDEV voisins

Les offsets de code exacts permettent d'associer plusieurs accès aux fonctions WDEV.

### `wDev_Rxbuf_Init()`

Dans la plage de cette fonction, le code écrit notamment :

```text
0x3FF2007C
0x3FF20080
0x3FF20084
0x3FF20088
0x3FF20000
0x3FF20008
0x3FF2000C
0x3FF20010
```

Ces accès sont cohérents avec l'initialisation de la chaîne RX / buffers / descripteurs.

### `wDev_Bssid_Init()`

La fonction effectue plusieurs RMW sur :

```text
0x3FF2006C
```

### `wDev_Initialize()`

La routine d'initialisation manipule notamment :

```text
0x3FF20178
0x3FF20004
```

### `wDevEnableRx()`

La routine touche :

```text
0x3FF20004
```

### Sniffer

Les fonctions :

```text
wdev_go_sniffer
wdev_set_sniffer_addr
wdev_exit_sniffer
```

manipulent notamment :

```text
0x3FF2000C
0x3FF2006C
```

Ce voisinage confirme que la zone :

```text
0x3FF20000...
```

porte réellement des contrôles RX/MAC/WDEV.

---

## 36BP.3 `0x3FF2003C` reste un état observé, pas programmé

Malgré ce grand nombre d'accès voisins :

```text
aucune écriture
aucun RMW
aucun autre getter
```

vers :

```text
0x3FF2003C
```

n'a été localisé dans les objets exacts analysés.

Le seul usage directement relié au chantier CFO reste :

```text
phy_get_bb_freqoffset()
    ↓
read 0x3FF2003C
    ↓
extract [19:16]
    ↓
reject if >= 8
```

Conclusion renforcée :

> `0x3FF2003C[19:16]` ressemble davantage à un **état matériel WDEV/RX observé par le
> PHY** qu'à un paramètre de configuration logiciel.

Le sens exact reste ouvert.

---

## 36BP.4 Ne pas assimiler trop vite le gate à `rxend_state`

L'API publique ESP8266 possède également dans `RxControl` un champ :

```text
rxend_state : 8 bits
```

mais aucun lien binaire direct n'a été démontré entre :

```text
RxControl.rxend_state
```

et :

```text
0x3FF2003C[19:16]
```

Le gate hardware ne fait que 4 bits dans le getter CFO.

Il serait donc prématuré d'écrire :

```text
0x3FF2003C[19:16] == rxend_state
```

Le terme recommandé reste :

```text
WDEV CFO context/state gate
```

jusqu'à preuve supplémentaire.

---

# Scan ROM du bloc `0x60009600`

## 36BP.5 Une seule constante de base dans la ROM exacte

Le dump ROM exact contient une seule occurrence littérale de :

```text
0x60009600
```

et quatre chargements `l32r` y font référence dans le code ROM.

Les xrefs tombent dans quatre routines identifiables :

```text
rom_chip_50_set_channel
rom_cal_tos_v50
rom_rfcal_txiq
rom_set_txiq_cal
```

Cela démontre que :

```text
0x60009600
```

est une base PHY/baseband générale partagée par plusieurs fonctions RF/calibration.

---

## 36BP.6 `rom_chip_50_set_channel()`

Dans cette routine, les accès reconstruits comprennent :

```text
0x60009B14  RMW
0x600098A0  RMW
```

La routine ne touche pas :

```text
0x60009800
0x600098DC
```

dans les accès localisés.

---

## 36BP.7 `rom_cal_tos_v50()`

Cette calibration touche :

```text
0x60009864  RMW
```

sans accès localisé à :

```text
0x60009800
0x600098DC
```

---

## 36BP.8 `rom_rfcal_txiq()`

Les accès reconstruits comprennent :

```text
0x60009A28  RMW
0x6000983C  RMW
0x60009864  RMW
0x60009860  RMW
```

Encore une fois, aucun accès à :

```text
0x60009800
0x600098DC
```

n'apparaît dans cette routine ROM.

---

## 36BP.9 `rom_set_txiq_cal()`

Cette fonction lit notamment :

```text
0x60009860
```

mais pas :

```text
0x60009800
0x600098DC
```

dans les accès reconstruits.

---

## 36BP.10 Conséquence : résultat CFO/EVM distinct des registres de calibration voisins

Le sous-espace matériel peut maintenant être séparé fonctionnellement :

```text
0x60009800
    → résultat partagé CFO / EVM

0x600098DC
    → contrôle/status spécifique au chemin CFO observé

voisins comme :
0x6000983C
0x60009860
0x60009864
0x600098A0
...
    → configuration/calibration PHY utilisée par RX/TXIQ/canal
```

Cela ne signifie pas que les circuits matériels sont totalement indépendants.

Cela signifie que, dans le logiciel/ROM observé :

> **les registres de résultat CFO/EVM ne sont pas les mêmes registres que ceux utilisés
> pour programmer les calibrations TXIQ/canal voisines.**

---

## 36BP.11 `0x600098DC` devient encore plus intéressant

Après :

```text
phy_get_bb_freqoffset()
```

le CPU exécute :

```text
0x600098DC |= 0xF
```

Or :

```text
- le getter EVM ne touche pas 0x600098DC ;
- les quatre routines ROM partageant la base 0x60009600 ne le touchent pas ;
- les scans statiques localisés n'ont pas retrouvé d'autre utilisateur normal.
```

Le classement peut donc monter à :

```text
registre de contrôle/status très probablement spécifique
au résultat CFO ou à son cycle de mesure
```

avec, pour le détail :

```text
ACK       → plausible
CLEAR     → plausible
RE-ARM    → plausible
sens exact → toujours ouvert
```

---

## 36BP.12 Hypothèse expérimentale la plus discriminante pour `0x600098DC`

Le test peut être réduit à une séquence minimale :

```text
1. attendre `0x60009800.bit0 == 1`;
2. lire plusieurs fois `0x60009800` SANS écrire 0x600098DC;
3. noter si bit0 / raw restent figés;
4. exécuter `0x600098DC |= 0xF`;
5. observer immédiatement l'évolution de bit0;
6. attendre la prochaine activité RX et observer le retour de bit0.
```

Interprétation :

```text
bit0 tombe après l'écriture
    → clear/ack très probable

bit0 peut ensuite remonter sur nouvel événement RX
    → fonction de re-arm/ack de cycle renforcée

aucun changement
    → 0x600098DC contrôle probablement autre chose
```

Ce protocole permet de distinguer plusieurs hypothèses sans modifier les autres registres
PHY inconnus.

---

# TX — poursuite parallèle

## 36BP.13 La ROM officielle confirme l'adresse ESP8266 de `rom_start_tx_tone`

Le linker ROM Espressif officiel publie :

```text
rom_start_tx_tone = 0x400068B4
rom_stop_tx_tone  = 0x4000698C
rom_txtone_linear_pwr = 0x40006A1C
```

Cela correspond à la ROM exacte analysée dans le projet.

Le reverse-engineering local de `rom_start_tx_tone()` reste donc ancré sur une primitive
ROM officiellement nommée.

---

## 36BP.14 Recoupement inter-générations inchangé

Les ROM Espressif suivantes séparent :

```text
start_tx_tone_step
start_tx_tone
set_rf_freq_offset
rfpll_set_freq
set_channel_freq
```

Ce recoupement continue de soutenir le modèle :

```text
tone_control / step
    ≠
offset RFPLL
    ≠
changement de canal
```

mais il ne fournit pas l'échelle ESP8266 en Hz.

Aucun prototype public exploitable ni aucune formule fiable de `start_tx_tone_step`
n'a été retrouvé dans cette passe.

---

## 36BP.15 Statut après v0.51

| Élément | Compréhension estimée |
|---|---:|
| `0x3FF2003C` appartient au bloc WDEV | **~99 %** |
| `[19:16]` = état hardware observé | **~90 %** |
| signification exacte du champ | **~45 %** |
| lien direct avec `rxend_state` | **non démontré** |
| `0x60009800` = résultat CFO/EVM | **~99 %** |
| `0x600098DC` spécifique au cycle CFO | **~80 % structurel** |
| rôle exact ACK/CLEAR/REARM | **~65 %** |
| CFO produit avant validation MAC/FCS | **~60 %** |
| RX FSK autonome global | **~84 %** |
| TX tone-step séparé RFPLL | **~95 %** |
| `tone_control → Hz` | **~45 %** |
| plafond statique TX | **toujours atteint** |

Conclusion v0.51 :

> Le gate WDEV du CFO est désormais isolé comme un **état matériel lu mais non programmé**
> par le code WDEV observé. En parallèle, le scan ROM démontre que les calibrations canal,
> TOS et TXIQ utilisent plusieurs registres voisins de `0x60009800`, mais jamais le
> couple `0x60009800/0x600098DC`. Ce couple apparaît donc comme un sous-chemin de
> résultat/contrôle RX beaucoup plus spécifique. Le test le plus rentable côté RX est
> maintenant d'observer directement l'effet de `0x600098DC|=0xF` sur `bit0`, puis de
> comparer le retour du valid bit sur trame valide, trame rejetée après synchronisation
> et tone continu. Côté TX, la recherche statique confirme la séparation tone-step/PLL
> mais ne révèle toujours aucune loi en Hz.

### Sources publiques recoupées en v0.51

- Linker ROM officiel Espressif ESP8266 (`eagle.rom.addr.v6.ld`).
- `esp-open-rtos`, carte partielle de la région WDEV `0x3FF20000`.
- Documentation Espressif des métadonnées RX (`RxControl` / `rxend_state`).

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

| 0.30 | 2026-09-13 | **Ouverture FSK TX.** Nouveau recoupement inter-générations : les PHY ESP32/ESP32-S2 exposent séparément `rom_start_tx_tone_step`, `rom_set_rf_freq_offset` et les primitives PLL, ce qui renforce fortement `tone_control ≈ step numérique` et écarte l'interprétation PLL directe. Architecture candidate 2-FSK/M-FSK = PLL/RF/TX-clock fixes + RMW du champ bas; la loi `tone_control→Hz`, le hot-update et le settling restent à mesurer. |

| 0.31 | 2026-09-13 | **Ouverture FSK RX.** Preuve ESP8266 spécifique d'un estimateur de fréquence reçu : `bss_info.freq_offset` / `wifi_ap_record_t.freq_offset`, documenté en kHz dans l'interface AT; l'ancien `system_phy_freq_trace_enable()` démontre également une boucle d'auto-calibration de l'offset. Limite critique : la métrique publique est attachée à un AP/paquet décodé; le readout PHY brut autonome reste à retrouver. `E4` seul est rejeté comme discriminateur FSK général, `get_fm_sar_dout()` est confirmé comme voie TX/SAR et non démodulateur FM, et les corrélations IQ_EST restent une piste expérimentale sans être promues en CFO meter. |

| 0.32 | 2026-09-13 | **FSK/CFO : séparation mesure / correction.** Correction de portée : `system_phy_freq_trace_enable()` est une infrastructure de tracking/calibration RF, pas une preuve de discriminateur FSK symbole-par-symbole. Les octets PHY-init 112/113 démontrent un actionneur de correction signé à pas de **8 kHz**; cette granularité ne doit pas être confondue avec la résolution de mesure `freq_offset`. Les release notes montrent en outre que `libpp.a` v10.1 a corrigé des problèmes de frequency offset, ce qui recentre la recherche du producteur CFO vers PP/low-MAC. `RxControl` 12 octets ne contient aucun CFO; la source brute reste à retrouver. |

| 0.33 | 2026-09-13 | **FSK/CFO : chronologie de l'infrastructure fréquence.** `freq_offset` et `freqcal_val` sont confirmés comme deux métadonnées distinctes; l'ancien AT décrit la première en kHz et la seconde comme valeur de calibration. Le mécanisme d'auto-correction et ces métadonnées précèdent l'API publique `system_phy_freq_trace_enable()`, qui doit donc être traitée comme un wrapper de contrôle d'une infrastructure PHY préexistante. La release officielle v1.5.4 confirme aussi que `libpp.a` v10.1 a corrigé des problèmes de frequency offset, ce qui recentre le traçage binaire sur PP→PHY/WDEV. Aucun registre CFO brut ni baud FSK n'est encore revendiqué. |

| 0.34 | 2026-09-13 | **FSK/CFO : layering du patch frequency-trace.** Le patch officiel v2.0.0_20160809 qui publie `system_phy_freq_trace_enable()` est appliqué historiquement en remplaçant `libmain.a`, `libnet80211.a` et `libpp.a`, mais pas `libphy.a`. Cela renforce fortement que le support PHY/baseband nécessaire au tracking existait déjà avant le patch. La priorité devient un diff objet-par-objet des trois archives remplacées pour retrouver le wrapper et remonter vers la primitive PHY/ROM préexistante. Aucune attribution précise du wrapper à une des trois bibliothèques n'est revendiquée sans ce diff. |

| 0.35 | 2026-09-13 | **FSK : primitive ESP8266 `set_rf_freq_offset`.** Deux jeux de symboles historiques de `libphy.a` montrent `set_rf_freq_offset`, `chip_v6_set_chan_offset` et la variable globale `phy_freq_offset`, séparés de `ram_rfpll_set_freq` et `ram_set_channel_freq`. Un ancien link-map place le symbole suivant exactement deux octets après `phy_freq_offset`, indiquant fortement un état 16 bits dans ce build. Le lien avec l'actionneur documenté à pas de 8 kHz est plausible mais non encore démontré. Pour le TX FSK, `set_rf_freq_offset` devient un second candidat majeur après `tone_control`; pour le RX, il reste interdit de confondre cet état de correction avec `bss_info.freq_offset`. Correction méthodologique également ajoutée : la taille brute des archives `.a` n'est pas une preuve de version car le corpus contient des sections DWARF/debug. |

| 0.36 | 2026-09-13 | **FSK/CFO : chaîne PP→scan/hostap identifiée nominalement.** Un ancien link-map lie `libpp.a(pp.o)` à `HighestFreqOffsetInOneChk`, `ieee80211_scan.o` à `FreqCalCntForScan` et `TestStaFreqCalValInput`, et `ieee80211_hostap.o` à `ApFreqCalTimer`, tandis que le PHY porte `phy_freq_offset`. Cela établit très fortement une mécanique de calibration trans-couche PHY→PP→net80211, sans encore démontrer l'algorithme ni le registre CFO brut. La priorité devient les xrefs de `HighestFreqOffsetInOneChk` dans le `pp.o` exact du corpus. Les adresses du vieux build ne sont pas transposées. |

| 0.37 | 2026-09-13 | **FSK/CFO : points de coupe RX et correction identifiés.** `ppPeocessRxPktHdr` est confirmé dans `libpp.a(pp.o)` comme un traitement de header RX substantiel et persistant entre SDK. Des traces décodées de plusieurs générations montrent aussi `DefFreqCalTimerCB`, souvent dans le même contexte runtime que `wDev_ProcessFiq`, `lmacRxDone`, `sta_input`, `scan_parse_beacon` et `ppPeocessRxPktHdr`; ces traces ne sont pas traitées comme un call graph. `scan_parse_beacon` partage `ieee80211_scan.o` avec `FreqCalCntForScan` et `TestStaFreqCalValInput`. La branche mesure doit maintenant désassembler `ppPeocessRxPktHdr`; la branche correction doit désassembler `DefFreqCalTimerCB` et chercher un xref vers `set_rf_freq_offset`. |

| 0.38 | 2026-09-13 | **FSK/CFO : calibration périodique PP et persistance interne.** Un ancien link-map place `pend_flag_periodic_cal` et `HighestFreqOffsetInOneChk` dans le même `libpp.a(pp.o)`. Le maître avait déjà démontré des références PP/PM/hostap vers `periodic_cal_top`; la nouveauté est donc la co-localisation de l'état de scheduling périodique et de l'agrégat frequency-offset. En parallèle, `DefFreqCalTimerCB` reste présent dans une stack NONOS SDK 3.0.4, postérieure à la suppression publique de `system_phy_freq_trace_enable` en v3.0.1; cela démontre la persistance du code interne, pas son activation lorsque le trace est désactivé. La priorité devient les xrefs `ppPeocessRxPktHdr→HighestFreqOffsetInOneChk`, `pend_flag_periodic_cal→periodic_cal_top` et `DefFreqCalTimerCB→set_rf_freq_offset/phy_freq_offset`. |

| 0.39 | 2026-09-13 | **Provenance du corpus : candidat NONOS SDK 3.0.5.** Les tailles exactes du corpus donnent 566/168/981 KiB arrondis pour `libpp.a`/`libphy.a`/`libnet80211.a`, exactement les trois tailles affichées par le dossier Arduino `NONOSDK305`. Les SDK 2.2.1 legacy et 2.2.1+2019 comparés sont nettement plus petits (`libpp.a` 235–260 KB). Le dossier `NONOSDK305` se déclare `v3.0.5-g7b5b35d`, affiché en debug comme `SDK:3.0.5(b29dcd3)`. Le corpus est donc classé **très probablement 3.0.5**, mais l'identité exacte du snapshot reste à confirmer par SHA-256 dès que les octets Raw publics sont récupérables. |

| 0.40 | 2026-09-13 | **NONOSDK305 patché + signe frequency correction.** Le core Arduino repatche les SDK NONOS 3.0.x après import : `pvPortMalloc` est redéfini en `sdk3_pvPortMalloc` dans les archives `.a` via `objcopy --redefine-sym`, et `libpp.a`, `libphy.a`, `libnet80211.a` de `NONOSDK305` font partie des archives modifiées. Une différence SHA avec l'Espressif 3.0.5 brut ne suffit donc pas à rejeter la provenance 3.0.5. La documentation du champ PHY-init `force_freq_offset` confirme en parallèle un `int8` à pas de 8 kHz et des exemples où la correction a le signe opposé à l'erreur mesurée. Le lien ABI direct avec `set_rf_freq_offset()` reste à prouver et devient un test expérimental prioritaire. |

| 0.41 | 2026-09-13 | **Taxonomie fréquence / FSK.** Les symboles historiques séparent explicitement `set_rf_freq_offset`, `ram_rfpll_set_freq`, `ram_set_channel_freq`, `chip_v6_set_chan_offset`, `chip_v6_set_chan` et `chip_v6_set_chanfreq`. Dans un vieux link-map, `set_rf_freq_offset` est un petit wrapper lié d'environ une centaine d'octets, mais son ABI reste inconnue. `test_rffreq_txcap` est reclassé : le map le montre comme un objet `.data` de **3 octets**, pas une fonction. `register_chipv6_phy_init_param` est au contraire une routine substantielle (~0x2A0 octets liés dans ce build), mais le maître ne trace pas encore les paramètres PHY-init 0x70/0x71 vers `set_rf_freq_offset`. Le pas 8 kHz reste donc démontré pour `force_freq_offset`, pas pour l'ABI runtime. Les PHY Espressif suivants corroborent une séparation explicite mesure RX / correction / offset RF / canal / PLL / tone-step. |

| 0.42 | 2026-09-13 | **CFO RX : mode PHY auto-measure explicitement confirmé.** Le `core_esp8266_phy.cpp` actuel documente le byte112 : bit0 active la correction, bit1 sélectionne BBPLL 168/160 MHz, bit2 choisit entre **auto measure frequency offset and correct it** et le byte113 `force_freq_offset`; ce dernier est signé et documenté en pas de 8 kHz. Le même fichier indique explicitement que le mécanisme des 128 octets PHY a été vérifié avec SDK 3.0.5. `esp-open-rtos` recoupe indépendamment les mêmes flags et décrit le mode non-FORCE comme mesure+correction automatique. L'existence fonctionnelle d'un estimateur CFO interne passe donc à très haute confiance. Le verrou devient l'accès à sa mesure brute avant correction/scan, notamment via `ppPeocessRxPktHdr`/WDEV/PHY. RX FSK autonome réévalué à ~65 %. |

| 0.43 | 2026-09-13 | **TX+RX FSK en parallèle : sonde runtime `phy_freq_offset`.** `esp-open-rtos` renomme explicitement `phy_freq_offset`, `pend_flag_periodic_cal`, `periodic_cal_top`, `ppPeocessRxPktHdr` et `set_rf_freq_offset`, alors que `HighestFreqOffsetInOneChk` n'apparaît pas dans sa table de renommage malgré sa présence démontrée dans `pp.o`. Un ancien map place le symbole suivant exactement 2 octets après `phy_freq_offset`, compatible avec un état 16 bits. Nouveau protocole de fermeture : comparer byte112=0 (off), 1 (auto-measure), 5 (forced) en loggant `phy_freq_offset`, `bss_info.freq_offset`, `freqcal_val` et, en forced, balayer byte113 à pas documenté de 8 kHz. Cela peut séparer mesure CFO et correction appliquée sans appeler encore l'ABI inconnue de `set_rf_freq_offset`. RX FSK ~68 %, TX FSK offset-fin ~82 % au niveau architecture; hot-update toujours ouvert. |

| 0.44 | 2026-09-13 | **Analyse des binaires exacts : pipeline CFO RX fermé jusqu'au buffer PP ; correction TX majeure.** Les quatre SHA du corpus fourni correspondent exactement au maître. Dans `phy_chip_v6_ana.o`, `set_rf_freq_offset` (0x6D octets) appelle directement `ram_rfpll_set_freq` puis `wait_rfpll_cal_end` : la piste fast-FSK par cette primitive est fortement déclassée. `chip_v6_set_chan_offset` référence `phy_freq_offset` puis appelle `chip_v6_set_chan`, qui stoppe RX, change le canal, exécute `bbpll_cal` et redémarre RX. Côté RX, `wDev_ProcessRxSucData` appelle `phy_get_bb_freqoffset`, stocke le résultat dans un `sint16 chl_freq_offset`, le transmet à `wDev_IndicateFrame`, puis le CFO est présent dans `esf_buf_s` à **+24**. `ppRxProtoProc` appelle `HdlChlFreqCal`, fonction de 0x24B avec locale `tmp_chl_freq_offset` et les agrégats `All/Avg/Highest/LowestFreqOffsetInOneChk`; elle appelle ensuite `chip_v6_set_chan_offset`. Deux globals PHY 16 bits distincts sont confirmés : `phy_meas_freq_offset` (mesure) et `phy_freq_offset` (correction). Verrou restant : déterminer si `phy_get_bb_freqoffset` produit une valeur fraîche sans packet Wi-Fi valide. |

| 0.45 | 2026-09-13 | **CFO BB exact : registre et formule fermés.** `phy_get_bb_freqoffset()` du binaire exact est décodé : gate sur `0x3FF2003C[19:16]`, puis `0x60009800.bit0` doit être valide; la mesure brute est `0x60009800[15:8]`, convertie en signé 8 bits puis en `result = (raw * 107) >> 6`. La sentinelle invalide est `0x7FFF`. Après lecture, la fonction effectue `0x600098DC |= 0xF` puis stocke le résultat 16 bits directement dans `phy_meas_freq_offset`. Le désassemblage de `scan_parse_beacon` et `scan_add_ssid` démontre ensuite une copie sans changement d'échelle de `esf_buf_s.chl_freq_offset` vers `bss_info.freq_offset` (+54). La documentation Espressif historique donnant `freq_offset` en kHz, la sortie du getter est classée kHz à très haute confiance (~1,671875 kHz par LSB raw). `phy_get_bb_evm()` lit également `0x60009800`, bits 28:16. Verrou RX restant : comprendre ce qui met à jour bit0 et bits15:8 hors synchronisation paquet Wi-Fi. |

| 0.46 | 2026-09-13 | **TX+RX FSK en parallèle : zone d’armement CFO resserrée.** `phy_meas_freq_offset` et `phy_freq_offset` sont confirmés comme deux globals 16 bits distincts; `phy_get_freq_param` référence les deux. `register_chipv6_phy_init_param` mappe les bytes 112/113 vers `chip6_phy_init_ctrl[76]/[77]`, et le chemin forcé utilise un décalage `<<3`, concordant avec le pas 8 kHz. `HdlChlFreqCal` rejette la sentinelle `0x7FFF`. `phy_bb_rx_cfg` (0x406 octets) configure le voisinage du CFO : `0x600098EC`, `0x60009988`, `0x600098A0`, `0x60009838`, `0x60009860`; `0x60009988.bit26` est manipulé clear→set et devient candidat gate/reset, sans être nommé CFO-enable. Aucune écriture CPU normale vers `0x60009800` n’a été localisée, renforçant son statut de résultat hardware. `0x600098DC|=0xF` reste candidat ack/rearm. Côté TX, aucune formule logicielle `tone_control→Hz` n’existe dans le corpus : `tone_control` reste le seul candidat fast-FSK et la prochaine fermeture doit être une mesure RF contrôlée. |

| 0.47 | 2026-09-13 | **TX+RX parallèle : correction du faux trigger BB et gating CFO WDEV exact.** `phy_bb_rx_cfg` n'a qu'un xref, depuis `chip_v6_initialize_bb`; sa séquence `0x60009988.bit26 clear→set` est donc reclassée initialisation/gate RX, pas réarmement CFO par paquet. Correction également de `0x600098EC` : RMW exact `&=0x7fffffff; |=0x01900000`; le `movi 400` observé est écrit à `0x60009D18`, qui reçoit ensuite 128. `0x600098DC|=0x0f` reste spécifique au getter CFO parmi les accès localisés, tandis que `phy_get_bb_evm` lit le même `0x60009800` sans ce post-write. Dans `wDev_ProcessRxSucData`, `phy_get_bb_freqoffset` est appelé seulement pour `RSSI > -90` et `TestStaFreqCalValOK==1`; dans la fenêtre `-299..+299`, `TestStaFreqCalValDev` est ajouté. `ieee80211_scan.o` confirme `TestStaFreqCalValOK` initialisé à 1 et `Dev` en BSS donc 0. Côté TX, appelants exacts fermés : `tone_control=8` pour RXIQ; `64` pour power-control et TX-cap, avec une troisième occurrence 64 dans `tx_pwctrl_init_cal`. Le ROM exact confirme toujours l'injection brute du control sans conversion Hz. |

| 0.48 | 2026-09-13 | **CFO RX : handshake PP et cooldown fermés.** `DefFreqCalTimerCB` (10 octets) est décodé exactement : il ne fait que `CanDoFreqCal=1`. La `.data` exacte initialise `CanDoFreqCal=1` et `test_freq_val_first=1`. `HdlChlFreqCal` vérifie ce flag, met `TestStaFreqCalValOK=0` avant `chip_v6_set_chan_offset`, puis remet `OK=1` et copie `TestStaFreqCalValInput` dans `TestStaFreqCalValDev`; WDEV ajoute ensuite ce Dev au CFO résiduel dans la fenêtre `-299..+299`. Après le cycle, `CanDoFreqCal=0`. Quand `pm_is_open()==0`, PP désarme/configure `DefFreqCalTimer` puis appelle `ets_timer_arm_new(timer,1000,0,1)`, soit un one-shot de 1000 ms selon l'API NONOS. Une branche d'ajustement utilise des pas ±40 et une plage locale `[-320,0]`, compatible avec des pas 40 kHz dans ce domaine, sans être généralisée à tous les modes. La boucle lente est donc clairement distincte du trigger CFO hardware. |

| 0.49 | 2026-09-13 | **TX+RX parallèle : CFO consommé sur branche RX-success, tone=64 renforcé.** Dans `wDev_ProcessFiq` exact, la zone finale bifurque entre `wDev_DiscardFrame` (relocation ~+0x3E5, DWARF ligne1179) et `wDev_ProcessRxSucData` (relocation ~+0x3FC, DWARF lignes1186–1187) après parcours/validation de la chaîne de blocs RX. Le SDK standard est donc clairement packet/RX-success coupled pour la consommation du CFO; la production hardware de `0x60009800` peut néanmoins être plus précoce et reste ouverte. Côté TX, `meas_tone_pwr_db` (0x6D octets) démarre elle aussi un tone avec `tone_control=64`, mesure sa puissance deux fois puis l'arrête. Cette quatrième famille renforce 64 comme stimulus standard puissance/TX, sans fournir de conversion Hz. |

| 0.50 | 2026-09-13 | **Validité CFO spécifique + EVM partagé + plafond statique TX.** `phy_get_bb_evm()` lit le même `0x60009800` mais extrait `[28:16]` sans tester `bit0` et sans toucher `0x600098DC`, alors que `phy_get_bb_freqoffset()` exige `bit0=1`, utilise `[15:8]`, puis fait `0x600098DC|=0xF`. Cela renforce une sémantique de validité/ack propre au chemin CFO plutôt qu'un simple valid global du registre. Le second gate CFO est `0x3FF2003C[19:16] < 8`, dans l'espace WDEV `0x3FF20000`; son sens exact reste ouvert et aucun autre consommateur de cette adresse précise n'a été identifié. Un protocole de test A/B/C (trame valide, trame synchronisée mais rejetée, tone non-802.11) est ajouté pour localiser le stade hardware de production CFO. Côté TX, `meas_tone_pwr_db()` exact confirme un nouvel usage de `tone_control=64`; les seuls controls distincts prouvés restent 8 et 64 et aucune loi `step→Hz` n'existe dans le logiciel, donc le plafond statique du tone generator est considéré atteint. |

| 0.51 | 2026-09-13 | **Gate WDEV isolé + résultat CFO/EVM séparé des calibrations ROM.** Le scan exact de `wdev.o` retrouve de nombreux accès voisins (`0x3FF20000/04/08/0C/10`, `+0x6C`, `+0x7C...+0x88`, `+0x178`) dans les routines d'initialisation RX/BSSID/sniffer, mais aucun autre accès à `0x3FF2003C`; le champ `[19:16]` utilisé par `phy_get_bb_freqoffset` est donc reclassé comme état hardware WDEV observé, sans l'assimiler à `rxend_state`. Dans la ROM exacte, les quatre xrefs à la base `0x60009600` appartiennent à `rom_chip_50_set_channel`, `rom_cal_tos_v50`, `rom_rfcal_txiq` et `rom_set_txiq_cal`; ils touchent notamment `0x6000983C`, `0x60009860/64`, `0x600098A0`, `0x60009A28`, `0x60009B14`, mais jamais `0x60009800` ni `0x600098DC`. Cela isole davantage le couple résultat CFO/EVM + contrôle/status CFO. Un test direct de l'effet de `0x600098DC|=0xF` sur bit0 est ajouté comme prochaine expérience RX prioritaire. Côté TX, aucune nouvelle formule `tone_control→Hz` n'est trouvée. |

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
