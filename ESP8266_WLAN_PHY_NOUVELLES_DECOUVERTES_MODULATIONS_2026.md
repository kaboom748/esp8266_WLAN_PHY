# ESP8266 WLAN PHY — Synthèse technique 2026
## Nouveautés de reverse-engineering et potentiel OOK / ASK / FSK / M-FSK / autres modulations

**Projet étudié :** `kaboom748/esp8266_WLAN_PHY`  
**Portée :** ESP8266 mask-ROM + PHY v6, avec focus sur le corpus PHY 1156 / SDK NonOS tardif  
**Statut :** document de synthèse vivant, distinct des références figées du dépôt  
**But :** consolider les connaissances utiles pour transformer l’ESP8266 en générateur/récepteur RF expérimental hors du chemin Wi-Fi normal.

> Ce document décrit des résultats de reverse-engineering et des possibilités de laboratoire.  
> Les registres RF sont non documentés publiquement par Espressif, les relations code→Hz, code→dB et les limites spectrales ne doivent pas être supposées.  
> Les émissions non standard doivent rester dans un environnement RF contrôlé, avec atténuation ou blindage approprié et sans perturber d’autres communications.

---

# 1. Résumé exécutif

Le dépôt `esp8266_WLAN_PHY` fournit déjà une base solide pour quatre familles :

| Fonction | Statut actuel | Chemin principal |
|---|---|---|
| TX OOK | **solide / fermé au niveau logiciel** | gate du tone slot 1 |
| TX ASK / M-ASK | **solide au niveau logiciel** | digital scale du tone slot 1 |
| TX FSK / M-FSK | **solide au niveau logiciel/statique** | champ `K/tone_control` du tone slot 1 |
| RX OOK | **solide comme détecteur relatif** | `IQ_EST` + résultat `E4` |
| RX ASK / M-ASK | **faisable par classification relative** | `IQ_EST` avec gain fixe |
| RX FSK / M-FSK | **partiellement fermé** | CFO / BB frequency-offset à valider sur tonalités arbitraires |
| QPSK/QAM arbitraire | **non démontré** | aucun FIFO I/Q / mapper arbitraire trouvé |
| CCA comme démodulateur | **à abandonner** | aucun `CCA_BUSY` CPU-readable démontré |

Les découvertes récentes sur CCA changent fortement la stratégie RX :

```text
NE PAS construire un récepteur OOK autour d'un hypothétique CCA_BUSY.

Utiliser :
    OOK / ASK  -> IQ_EST / E4
    FSK        -> CFO / frequency-offset si validé
    occupation -> noise-floor / IQ_EST selon la vitesse voulue
```

Le modèle architectural qui ressort est :

```text
TX
CPU
 │
 ├─ tone_control K  ───────────────> fréquence relative du tone
 ├─ digital_scale  ────────────────> amplitude numérique
 └─ gate bit18     ────────────────> ON/OFF rapide
                │
                v
        générateur numérique
                │
        RF/PBUS + TX clock
                │
                v
              antenne


RX
antenne
  │
RF / AGC / baseband
  │
  ├─ IQ_EST ----------> métrique E4 -> OOK / ASK
  ├─ CFO / BB result -> décalage fréquentiel -> FSK potentiel
  ├─ noise-floor ----> bruit/occupation lente
  └─ CCA ------------> MAC/backoff interne
                         |
                         +-> pas de bit BUSY CPU démontré
```

---

# 2. Sources utilisées

Cette synthèse croise :

## Dépôt GitHub

- `README.md`
- `ESP8266_PHY_MODULATIONS_REFERENCE_CONSOLIDEE_v2.0.md`
- `ESP8266_ASK_OOK_REFERENCE_OFFICIELLE_PROJET_v1.0.md`
- `ESP8266_RX_OOK_ASK_REFERENCE_OFFICIELLE_PROJET_v1.0.md`
- `ESP8266_TX_FSK_MFSK_REFERENCE_OFFICIELLE_PROJET_v1.0.md`
- `ESP8266_RX_FSK_MFSK_REFERENCE_OFFICIELLE_PROJET_v1.0.md`
- `ESP8266_WIFI_PHY_REVERSE_ENGINEERING_v0.51_WDEV_GATE_ROM_RESULT_ISOLATION.md`
- `TEST-TONEv5.yaml`
- `TEST-RX-OOK-SPEED-v3.yaml`
- `TEST-RX-FREQ-LAB-v4.yaml`

## Reverse-engineering de la session actuelle

Analyse directe de :

- ROM ESP8266 64 KiB ;
- `libphy.a` ;
- `libpp.a` ;
- `wdev.o` ;
- `lmac.o` ;
- tables PHY ;
- accès MMIO directs et calculés ;
- chemins WDEV / FIQ ;
- tests de corrélation CCA/WDEV/backoff.

Les résultats de cette session sont volontairement distingués des hypothèses et des anciens commentaires de projet.

---

# 3. Version PHY et contexte logiciel

Le `libphy.a` étudié contient dans `phy_version_print()` la constante :

```text
PHY version = 1156
```

Cette version correspond aux versions tardives du SDK NonOS ESP8266.

C'est important :

- on n'analyse pas seulement un vieux prototype RF ;
- les comportements CCA / noise-floor / comparator observés appartiennent à une PHY très tardive ;
- les résultats sont donc particulièrement intéressants pour les cores Arduino/NonOS qui héritent de ces bibliothèques.

Le commentaire de compilation conserve toutefois des métadonnées plus anciennes :

```text
HW_VERSION="LX3.0.1"
RELEASE_NAME="RF-2015.2"
Xtensa compiler 11.0.2
```

Il faut distinguer :

```text
métadonnées historiques de build
≠
version PHY runtime imprimée
```

---

# 4. Carte synthétique des registres importants

## 4.1 TX / tone / PBUS

| Adresse | Champ / rôle | Statut |
|---|---|---|
| `0x600005B8` | tone slot 1 | **central TX OOK/ASK/FSK** |
| `0x60000594` | commande PBUS | reconstruit |
| `0x600005A0` | busy/statut PBUS | reconstruit |

### Tone slot 1 `0x600005B8`

Modèle actuel :

```text
bits  9:0   tone_control / K
bits 17:10  code digital scale
bit     18  gate tone ON/OFF
bits 31:28  bits supérieurs à préserver
```

Le champ ASK est encodé :

```text
code = (-digital_scale) & 0xff
```

Le projet utilise une plage canonique :

```text
digital_scale = 0..63
```

Le champ `K` est protégé sur :

```text
0..1023
```

### Conséquence fondamentale

Les trois modulations principales peuvent partager **le même générateur** :

```text
OOK  -> modifier bit18
ASK  -> modifier bits17:10
FSK  -> modifier bits9:0
```

sans retuner le RFPLL entre chaque symbole.

---

# 5. TX OOK — ce qui est réellement démontré

OOK = On-Off Keying.

Le mécanisme correct est :

```text
RFPLL      reste actif
RF TX      reste préparé
TX clock   reste active
tone slot  reste configuré

symbole 1 -> bit18 = 1
symbole 0 -> bit18 = 0
```

Adresse :

```c
#define TONE1_ADDR       0x600005B8u
#define TONE_GATE_BIT    18u
#define TONE_GATE_MASK   0x00040000u
```

## 5.1 Pourquoi ne pas appeler start/stop à chaque bit

`rom_stop_tx_tone()` :

1. efface le gate ;
2. coupe ensuite la TX clock.

Donc :

```text
start_tx_tone()
stop_tx_tone()
start_tx_tone()
...
```

introduit une reconfiguration bien plus lourde que nécessaire.

Pour une vraie OOK symbol-rate :

```text
TX clock toujours active
+
RMW du bit18 uniquement
```

## 5.2 Ce que cela permet

Directement :

- OOK binaire ;
- Manchester sur OOK ;
- NRZ OOK ;
- pulse-distance coding ;
- pulse-width coding ;
- PPM construit logiciellement ;
- trames de télécommandes simples ;
- burst / beacon expérimental.

Ces protocoles sont des **encodages temporels** au-dessus du même gate OOK.

## 5.3 Ce qui reste à caractériser

- temps RF ON/OFF réel après écriture MMIO ;
- jitter symbole ;
- asymétrie ON→OFF / OFF→ON ;
- fuite RF quand gate=0 ;
- spectre en fonction du débit ;
- comportement de phase entre deux périodes ON ;
- débit OOK maximal fiable.

---

# 6. TX ASK / M-ASK

Le même tone slot fournit un contrôle numérique d'amplitude :

```text
bits17:10
```

Le projet appelle ce paramètre :

```text
digital_scale
```

et la commande console correspondante :

```text
ASK n
```

## 6.1 Ce que signifie réellement ASK ici

Ce champ est :

- un code numérique interne ;
- rapide à modifier ;
- distinct du contrôle analogique de puissance.

Ce champ **n'est pas** démontré comme :

- une tension RF linéaire ;
- un pourcentage de puissance ;
- une valeur dBm ;
- une loi monotone garantie.

## 6.2 Modulations réalisables

Si les niveaux RF sont suffisamment distincts après caractérisation :

```text
2 niveaux  -> 2-ASK
4 niveaux  -> 4-ASK
8 niveaux  -> 8-ASK
...
```

On peut donc construire M-ASK en définissant :

```text
symbol 0 -> scale A0
symbol 1 -> scale A1
symbol 2 -> scale A2
...
```

## 6.3 OOK vs ASK

OOK devrait idéalement utiliser :

```text
bit18
```

et non :

```text
ASK=0 / ASK=max
```

Raison :

- le gate est le mécanisme explicitement identifié pour ON/OFF ;
- le digital scale est un contrôle d'amplitude ;
- un scale minimal n'est pas forcément équivalent à une extinction RF parfaite.

## 6.4 APWR n'est pas le symbole ASK

Le dépôt contient aussi un contrôle expérimental nommé `APWR`.

Il faut garder la séparation :

```text
digital_scale
    = modulation numérique rapide

APWR
    = réglage analogique / puissance expérimental
    = pas calibré en dBm
    = pas destiné à être commuté à chaque symbole
```

Pour une modulation rapide :

```text
APWR fixe
digital_scale variable
```

---

# 7. TX FSK / M-FSK

Le champ :

```text
0x600005B8[9:0]
```

est utilisé comme :

```text
tone_control
K
```

## 7.1 Architecture FSK recommandée

Ne pas changer de canal Wi-Fi entre symboles.

Maintenir :

```text
RFPLL         fixe
canal         fixe
RF TX         actif
TX clock      active
gate          actif
digital_scale fixe
```

Puis commuter uniquement :

```text
K
```

### 2-FSK

```text
bit 0 -> K0
bit 1 -> K1
```

### M-FSK

```text
symbole 0 -> K0
symbole 1 -> K1
...
symbole M-1 -> K(M-1)
```

## 7.2 Ce qui est démontré

Démontré au niveau logiciel/statique :

- champ K identifiable ;
- champ limité à 10 bits ;
- écriture rapide possible ;
- possibilité de changer K sans retuner tout le RFPLL ;
- chemin adapté à la génération de tones multiples.

## 7.3 Ce qui n'est PAS démontré

Il ne faut pas écrire :

```text
frequency = F0 + K * constante
```

sans mesure.

Restent à caractériser :

- loi `K -> Hz` ;
- éventuels wraps ;
- zones non monotones ;
- signe de variation ;
- settling time ;
- bruit de phase ;
- spurs ;
- continuité de phase entre deux valeurs K ;
- amplitude en fonction de K.

---

# 8. FSK, CPFSK, GFSK, MSK : distinction importante

Le fait de pouvoir commuter une fréquence ne prouve pas une modulation à phase continue.

## 8.1 FSK simple

**Oui, support logiciel crédible.**

Il suffit que :

```text
K0 != K1
```

produise deux fréquences RF séparables.

## 8.2 M-FSK

**Oui, support logiciel crédible.**

Plusieurs codes K peuvent former plusieurs symboles.

## 8.3 CPFSK

**Non démontré.**

Il faut établir que l'accumulateur de phase n'est pas réinitialisé lors d'un changement K.

## 8.4 MSK

**Non démontré.**

MSK exige une relation précise :

```text
modulation index h = 0.5
+
continuité de phase
```

Aucun des deux points n'est fermé.

## 8.5 GFSK

**Non démontré nativement.**

On pourrait théoriquement appliquer une trajectoire K lissée logiciellement :

```text
K[n] = filtre_gaussien(bits)
```

mais :

- cadence de mise à jour ;
- relation K→Hz ;
- phase ;
- filtre RF effectif ;

restent à mesurer.

Conclusion :

```text
K switching -> FSK / M-FSK
K ramping   -> expérimentation FM/GFSK possible
              mais pas encore une GFSK validée
```

---

# 9. Balayage fréquentiel / chirp / CSS

Le firmware du dépôt sait balayer K.

Cela peut servir à produire :

- sweep de laboratoire ;
- recherche de la loi K→Hz ;
- tone stepping ;
- pseudo-chirp expérimental.

Mais il ne faut pas conclure :

```text
K sweep = LoRa/CSS
```

car un vrai chirp CSS impose notamment :

- pente fréquentielle contrôlée ;
- linéarité ;
- timing ;
- largeur de bande connue ;
- continuité et répétabilité.

Le projet possède donc les **briques d'exploration**, pas un modem CSS validé.

---

# 10. RX OOK — chemin à privilégier

La découverte la plus importante côté réception est que le meilleur détecteur n'est pas le CCA.

Le chemin démontré est :

```text
RX RF
  |
IQ_EST
  |
DONE
  |
E4
  |
seuil calibré
  |
OOK 0 / 1
```

Registres :

```text
0x6000057C   IQ_EST_CTRL
0x600005E4   métrique/resultat utile dans le firmware RX OOK
```

---

# 11. IQ_EST_CTRL `0x6000057C`

Bitfields reconstruits :

```text
bit 0       ENABLE
bit 1       START / trigger
bits16:2    N
bit18       MODE
bit31       DONE
```

Le mode utilisé pour la mesure RX :

```text
MODE = 1
```

Le processus :

```text
configurer N
activer
trigger
attendre DONE
lire E4
```

## 11.1 IQ_EST n'implique pas le loopback RXIQ

C'est un point capital.

La calibration interne RXIQ configure explicitement des mux/bits I²C spéciaux.

`rom_iq_est_enable()` ne programme pas :

- tone TX ;
- loopback gain ;
- bits RXIQ spéciaux ;
- mux de calibration.

Donc :

> démarré depuis un RX normal, IQ_EST peut mesurer le datapath RX externe normal.

Cela rend le bloc très utile comme détecteur d'énergie/corrélation relatif.

---

# 12. `0x600005E4` et réception OOK

Le firmware `TEST-RX-OOK-SPEED-v3.yaml` utilise :

```text
E4 = REG(0x600005E4)
```

avec calibration :

```text
source OFF -> E_OFF
source ON  -> E_ON

threshold = milieu / règle dérivée
```

Le sens n'est pas forcé.

Le logiciel détermine si :

```text
ON > OFF
```

ou :

```text
ON < OFF
```

puis adapte le comparateur.

C'est une excellente décision de conception : les unités internes ne doivent pas être interprétées comme dBm sans preuve.

---

# 13. Vitesse IQ_EST observée

Le dépôt rapporte environ :

| N | mesures/s observées |
|---:|---:|
| 16 | ~305 810 |
| 32 | ~292 397 |
| 64 | ~235 294 |
| 128 | ~169 491 |
| 256 | ~110 375 |
| 512 | ~64 977 |
| 1024 | ~35 323 |
| 2048 | ~18 556 |
| 4096 | ~9 515 |

Conséquence :

```text
petit N
 -> réponse rapide
 -> plus de bruit / moins d'intégration

grand N
 -> meilleure intégration
 -> débit de mesures inférieur
```

Mais :

```text
mesures/s != bit/s fiable
```

Le bit rate fiable dépend aussi :

- du rapport signal/bruit ;
- de la durée symbole ;
- du seuil ;
- de l'AGC/gain ;
- du fading ;
- de la stabilité de l'émetteur.

---

# 14. RX ASK / M-ASK

Une fois le gain RX fixé :

```text
E4
```

peut être traité comme une métrique relative.

Pour 2-ASK :

```text
E4 < seuil -> symbole A
E4 > seuil -> symbole B
```

Pour M-ASK :

```text
niveau 0 -> plage E4_0
niveau 1 -> plage E4_1
...
```

## Conditions importantes

Le gain doit rester :

```text
déterministe
fixe pendant la trame
```

Sinon l'AGC peut volontairement annuler les différences d'amplitude que le démodulateur cherche à mesurer.

Le document RX du dépôt mentionne explicitement la nécessité d'un gain RF/baseband déterministe.

---

# 15. RX FSK — voie CFO / frequency offset

Le reverse-engineering a isolé :

```text
0x60009800
```

dans :

```text
phy_get_bb_freqoffset()
phy_get_bb_evm()
```

Le chemin `phy_get_bb_freqoffset()` :

- lit ce registre ;
- teste un indicateur de validité ;
- extrait notamment un champ dans la zone `bits15:8` ;
- applique une conversion/scaling.

Cela montre qu'un estimateur de fréquence existe dans le baseband.

## 15.1 Ce qui est utilisable

Conceptuellement :

```text
tone proche F0-dF -> CFO négatif / classe 0
tone proche F0+dF -> CFO positif / classe 1
```

Pour M-FSK :

```text
plusieurs plages CFO
```

## 15.2 Limite actuelle

Le dépôt lui-même garde correctement le statut :

```text
RX FSK CFO software path : documenté
fresh CFO sur tones arbitraires non-802.11 : validation silicium nécessaire
```

Pourquoi ?

Un résultat CFO peut dépendre de :

- détection préalable d'un paquet ;
- synchroniseur Wi-Fi ;
- corrélateur préambule ;
- état de la chaîne BB ;
- valid bit.

Donc :

```text
présence du registre CFO
≠
garantie de CFO frais sur une CW/FSK arbitraire
```

---

# 16. Autre stratégie RX FSK : banque de mesures

Si le CFO natif ne fonctionne pas sur une porteuse arbitraire, une stratégie alternative est :

```text
mesurer l'énergie pour plusieurs réglages/fenêtres fréquentielles
```

Par exemple conceptuellement :

```text
bin K0 / f0
bin K1 / f1
...
choisir le bin d'énergie maximale
```

Cependant l'ESP8266 n'expose pas actuellement une FFT/IQ FIFO générale.

La banque devrait donc s'appuyer sur :

- retuning ;
- comparateur RX ;
- blocs internes encore à cartographier ;
- ou une mesure IQ_EST après changement de fréquence.

Ce chemin est plus lent et encore expérimental.

---

# 17. Nouveauté majeure : CCA n'est pas un RSSI live accessible

Le reverse-engineering a désormais fermé plusieurs fausses pistes.

## 17.1 `0x60009B00 bit28`

Les fonctions ROM montrent :

```text
rom_chip_v5_disable_cca()
    -> SET bit28

rom_chip_v5_enable_cca()
    -> CLEAR bit28
```

Donc :

```text
bit28 = contrôle enable/disable
```

et non :

```text
bit28 = CCA_BUSY
```

Sens :

```text
bit28 = 0 -> CCA actif
bit28 = 1 -> CCA désactivé
```

---

# 18. `0x60009B64` — registre composite, pas simple seuil CCA

Carte reconstruite :

```text
B64[31:20]  résultat noise-floor
B64[19:12]  paramètre CCA
B64[11:9]   configuration noise-check
B64[8:0]    configuration / encoding noise-floor
```

Cette carte vient de fonctions indépendantes utilisant des masques non chevauchants.

## 18.1 Champ CCA

`set_cca()` utilise :

```text
mask = 0xFFF00FFF
```

donc modifie :

```text
B64[19:12]
```

## 18.2 Ancienne hypothèse à supprimer

L'idée :

```text
0xB4 = -76 dBm
```

n'est pas soutenue.

Le `0xB4` observé correspond à une valeur spéciale/fallback dans `set_cca()`, tandis que l'initialisation normale utilise d'autres valeurs, par exemple `0x22`.

Il ne faut plus présenter `0xB4` comme un seuil dBm.

---

# 19. Origine des paramètres CCA

Dans `phy_bb_rx_cfg()` / init PHY :

```text
init_data[84] -> chip6_phy_init_ctrl[0x3A] -> B64[19:12]
init_data[88] -> chip6_phy_init_ctrl[0x3B] -> mode selector
```

Ces octets sont historiquement décrits publiquement comme réservés, mais le binaire les consomme réellement.

Selon le mode :

```text
mode 3 -> programme B64 field
mode 4 -> programme B64 field + D68 bit18
```

Cela montre que le CCA/comparateur possède une configuration plus riche qu'un simple seuil.

---

# 20. `0x60009D68` — RX comparator / CCA configuration

`set_cca()` :

```text
D68 |= 0x00040000
```

donc :

```text
D68 bit18
```

est lié au mode/configuration comparator/CCA.

`chip_v6_set_chan_rx_cmp()` modifie :

```text
D68[17:10]
```

avec une valeur calculée à partir :

- du canal ;
- de tables de calibration ;
- de paramètres signés.

Conclusion :

```text
D68 = configuration RX comparator / calibration
```

et non :

```text
D68 = CCA_BUSY
```

---

# 21. Sense/backoff CCA

La ROM modifie :

```text
0x60009C28[16:10]
0x60009D24[7:1]
```

dans :

```text
rom_chip_v5_sense_backoff()
```

Ces champs appartiennent donc à la logique :

```text
CCA
 -> sensing
 -> backoff MAC
```

mais aucun de ces champs n'a été démontré comme sortie BUSY/FREE.

---

# 22. PHY dispatch table : surprise `enable_agc` / CCA

La table d'opérations PHY contient :

```text
+0x10 -> rom_chip_v5_enable_cca
+0x14 -> rom_chip_v5_disable_cca
```

Les wrappers :

```text
phy_enable_agc()
phy_disable_agc()
```

dispatchent respectivement vers ces fonctions CCA.

Cela montre que certains noms d'API historiques ne reflètent pas précisément le bloc matériel appelé.

Règle de reverse-engineering :

> ne jamais déduire le rôle matériel uniquement du nom du wrapper SDK.

---

# 23. `set_sense()` est vide dans PHY 1156

Dans `phy_chip_v6_unused.o` :

```text
chip_v6_set_sense -> ret.n
chip_v6_get_sense -> ret.n
```

Ainsi :

```text
phy_set_sense()
 -> dispatch
 -> chip_v6_set_sense()
 -> return
```

Il n'y a pas de getter de sense/CCA caché dans cette voie.

---

# 24. Recherche exhaustive d'un CCA_BUSY CPU-readable

Deux types de scans ont été faits :

## 24.1 Accès MMIO directs

Recherche :

```text
MMIO read
 -> mask / extui / shift
 -> branch
```

Résultat :

- nombreuses branches sur `B60` pour noise-floor ;
- aucune branche comparable sur :
  - `B00`
  - `B64`
  - `C28`
  - `D24`
  - `D68`

## 24.2 Accès MMIO calculés

Un scanner symbolique a suivi :

- `L32R`
- `MOV`
- `ADDI`
- `ADDMI`
- `ADD`
- `ADDX2/4/8`
- `SUB`
- puis les loads/stores.

Dans `libphy.a`, aucun accès dynamique caché vers la zone CCA n'a émergé.

Dans `libpp.a`, les accès dynamiques retrouvés concernent principalement :

- queues MAC ;
- WDEV ;
- clés ;
- descriptors.

Conclusion :

> il existe de fortes preuves négatives contre l'existence d'un getter CCA_BUSY utilisé par le SDK.

Cela ne prouve pas mathématiquement que le silicium n'expose aucun bit caché, mais si ce bit existe, le SDK analysé ne semble pas l'utiliser.

---

# 25. WDEV interrupts : pas d'IRQ CCA trouvée

Bloc WDEV :

```text
0x3FF20C18   enable
0x3FF20C1C   raw
0x3FF20C20   masked / pending
0x3FF20C24   clear / ACK
```

Masque normal :

```text
0x2C9F0300
```

bits activés :

```text
8, 9,
16,17,18,19,20,
23,
26,27,
29
```

Tous ces bits ont des consommateurs connus dans les handlers.

Aucun « bit orphelin évident » n'a été identifié comme CCA.

---

# 26. Validation expérimentale WDEV bit8

Un scan haute vitesse a donné :

```text
RAW_ON_ONLY         = 0x100
RAW_CHANGED_ONLY_ON = 0x100
```

Donc :

```text
bit8
```

pulse pendant un burst Wi-Fi.

Or le désassemblage avait déjà relié ce bit au chemin RX.

Conclusion renforcée :

```text
WDEV bit8 = événement/activité RX
```

et non :

```text
CCA_BUSY
```

Un RX OOK ne doit donc pas utiliser ce bit comme détecteur de porteuse.

---

# 27. Backoff MAC : nouvelle compréhension

Registres queues observés :

```text
Q0 BACKOFF  0x3FF20DC0
Q0 CTRL     0x3FF20DC4

Q1 BACKOFF  0x3FF20DA8
Q1 CTRL     0x3FF20DAC

Q2 BACKOFF  0x3FF20D90
Q2 CTRL     0x3FF20D94

Q3 BACKOFF  0x3FF20D78
Q3 CTRL     0x3FF20D7C
```

Champ historiquement considéré comme compteur :

```text
BACKOFF[21:12]
```

## 27.1 Nouvelle observation

Pendant une vraie TX :

```text
Q2 CTRL[31:30]
    00 -> 11 -> 00
```

et le champ backoff est rechargé :

```text
11 -> 15
```

mais reste à 15 après désarmement.

Il ne décrémente pas de façon CPU-visible.

Conclusion actuelle :

```text
BACKOFF[21:12] = valeur de chargement / initialisation
```

Le vrai countdown est probablement interne au MAC.

## 27.2 Ce que cela dit sur CCA

Architecture la plus probable :

```text
PHY CCA decision
      |
      v
backoff engine interne
      |
      v
TX queue state

CPU programme le backoff,
mais ne voit pas nécessairement le compteur interne.
```

Cela explique l'absence de `CCA_BUSY` observable dans PP/LMAC.

---

# 28. Noise-floor : registre et unités

Le PHY v6 utilise :

```text
0x60009824
```

dans :

```text
read_hw_noisefloor()
```

Une capture réelle a donné :

```text
raw = 0xD24
```

En signed 12-bit :

```text
0xD24 = -732
```

puis :

```text
-732 >> 1 = -366
```

ce qui correspond exactement au retour observé :

```text
read_hw_noisefloor() = -366
```

`libpp` applique ensuite :

```text
(value + 2) >> 2
```

soit environ :

```text
-91
```

pour cet exemple.

Cela renforce l'interprétation d'une unité interne fine, proche du quart de dB / quart de dBm équivalent, sans que le binaire fournisse explicitement le mot « dBm ».

---

# 29. Noise-floor state machine `0x60009B60`

Plusieurs fonctions utilisent :

```text
B60 bit1
```

comme état actif/busy de la procédure noise-floor.

`ram_start_noisefloor()` programme notamment :

```text
bit17
bit15
bit1
```

Le code de polling teste ensuite bit1.

Donc :

```text
B60 bit1 = bon candidat "noise measurement active"
```

mais :

```text
B60 bit1 != CCA busy
```

---

# 30. Noise-floor vs OOK

Pendant les essais RF courts :

```text
0x60009824
```

et :

```text
read_hw_noisefloor()
```

sont restés constants à travers des bursts de quelques millisecondes.

Conclusion pratique :

```text
noise-floor = utile pour bruit / baseline / occupation lente
IQ_EST E4   = bien meilleur candidat pour OOK rapide
```

---

# 31. Les registres RX IQ ne sont pas un RSSI gratuit permanent

Registres :

```text
0x60000580
0x60000584
0x60000588
0x6000058C
```

Ils sont utilisés dans :

```text
ram_rxiq_get_mis()
```

pour des calculs IQ/corrélation/calibration.

Dans un RX Wi-Fi normal, ils ont été observés à zéro pendant nos captures.

Conclusion :

```text
580..58C
```

ne doivent pas être supposés comme :

```text
live RSSI registers
```

sans déclencher/établir le contexte de mesure correspondant.

---

# 32. Pourquoi `0x600005E4` reste utile malgré cela

`E4` possède un usage clair dans :

- le chemin IQ ;
- le firmware RX OOK du dépôt ;
- une séquence avec `IQ_EST DONE`.

Donc la différence est :

```text
lecture passive de 580..58C
    -> souvent rien

déclenchement IQ_EST + attente DONE + E4
    -> mesure valide démontrée
```

---

# 33. Matrice des modulations réellement envisageables

| Modulation | TX | RX | Niveau de confiance |
|---|---|---|---|
| OOK | gate bit18 | IQ_EST/E4 | **élevé** |
| 2-ASK | digital_scale | E4 + seuil | **élevé logiciel / RF à calibrer** |
| M-ASK | plusieurs scales | E4 multi-seuils | **moyen/élevé** |
| 2-FSK | K0/K1 | CFO potentiel | **TX élevé, RX moyen** |
| M-FSK | plusieurs K | CFO multi-classes potentiel | **TX élevé, RX moyen/faible** |
| PPM | timing OOK | IQ_EST temporel | **plausible** |
| PWM | timing OOK | IQ_EST temporel | **plausible** |
| Manchester OOK | timing gate | seuil E4 | **plausible/fort** |
| AM analogique lente | scale variable | E4 | **expérimental** |
| FM discrète | K variable | CFO | **expérimental** |
| CPFSK | K variable | CFO | **phase non démontrée** |
| GFSK | trajectoire K | CFO | **non validée** |
| MSK | K + phase continue précise | ? | **non démontrée** |
| BPSK arbitraire | contrôle phase requis | ? | **non démontré** |
| QPSK arbitraire | mapper I/Q requis | ? | **non démontré** |
| QAM arbitraire | I/Q indépendants requis | ? | **non démontré** |
| OFDM arbitraire | sous-porteuses/IFFT | native Wi-Fi seulement | **non démontré hors Wi-Fi** |
| Chirp/CSS | sweep K | déchirp/détection | **expérimental uniquement** |

---

# 34. Modulations « faciles » à construire avec les primitives actuelles

## 34.1 OOK + Manchester

TX :

```text
bit -> deux demi-symboles
gate bit18
```

RX :

```text
E4 -> threshold -> Manchester decoder
```

Avantage :

- récupération d'horloge facilitée ;
- pas de DC long.

## 34.2 Pulse width modulation RF

TX :

```text
largeur du gate -> symbole
```

RX :

```text
détecter durée E4 au-dessus du seuil
```

## 34.3 PPM

TX :

```text
position du burst OOK dans une frame
```

RX :

```text
timestamp des transitions E4
```

## 34.4 M-ASK

TX :

```text
échelle numérique parmi N codes
```

RX :

```text
N clusters E4
```

N'est viable que si les distributions ne se recouvrent pas excessivement.

## 34.5 M-FSK

TX :

```text
N codes K
```

RX potentiel :

```text
N plages CFO
```

La fermeture RX dépend encore de la disponibilité d'un CFO frais hors paquet Wi-Fi.

---

# 35. Modulation hybride ASK + FSK

Les champs :

```text
K
digital_scale
```

sont indépendants dans le même tone slot.

On peut donc conceptuellement créer un alphabet 2D :

```text
(K0, A0)
(K0, A1)
(K1, A0)
(K1, A1)
...
```

Cela ressemble à une constellation :

```text
fréquence × amplitude
```

mais **pas à du QAM**, car il n'y a pas de contrôle I/Q orthogonal démontré.

Cette approche pourrait servir à :

- augmenter l'alphabet symbolique ;
- tester un modem hybride ;
- encoder fréquence + amplitude.

Elle demanderait une calibration RX à deux dimensions :

```text
CFO + E4
```

---

# 36. QPSK / QAM : ce que le silicium sait faire et ce qu'on ne contrôle pas

L'ESP8266 sait évidemment générer nativement :

- BPSK ;
- QPSK ;
- 16-QAM ;
- 64-QAM ;

dans ses modes Wi-Fi.

Mais le chemin identifié est :

```text
rate / PHY Wi-Fi
 -> mapper interne
 -> OFDM
 -> DAC / RF
```

Aucun des éléments suivants n'a été démontré :

```text
FIFO I/Q TX CPU-visible
index de constellation arbitraire
bypass mapper
écriture I/Q symbole par symbole
```

Conclusion :

> Le fait que le hardware supporte 64-QAM Wi-Fi ne transforme pas l'ESP8266 en SDR QAM programmable.

---

# 37. Peut-on faire du BPSK/QPSK en détournant le tone ?

Pas avec les connaissances actuelles.

Il manquerait un contrôle explicite de :

```text
phase accumulator
ou
I/Q sign
ou
phase offset
```

Changer K modifie une fréquence/commande NCO.

Changer le gate modifie l'enveloppe.

Changer le scale modifie l'amplitude.

Aucun de ces contrôles ne garantit un saut de phase contrôlé de :

```text
0 / π
```

ou :

```text
0 / π/2 / π / 3π/2
```

---

# 38. Rôle du PBUS

Registres :

```text
0x60000594   PBUS command
0x600005A0   PBUS busy/status
```

Format reconstruit :

```text
bits15:14   bank
bits13:5    value
bits4:2     selector
bit1        START
bit0        préservé
```

Le PBUS contrôle des éléments analogiques/RF.

Il est utilisé pour :

- XPD TX ;
- gain/power ;
- états RF.

## Important pour les modulations

Le PBUS est :

```text
plus transactionnel
plus lent
plus analogique
```

Il ne doit pas être le premier choix pour changer chaque symbole.

Pour un modem :

```text
PBUS         -> configuration lente / setup
tone slot    -> modulation rapide
```

---

# 39. TX clock et RF chain

`rom_start_tx_tone()` active :

```text
rom_set_txclk_en(1)
```

`rom_stop_tx_tone()` :

```text
clear gate
rom_set_txclk_en(0)
```

Donc pour :

- OOK rapide ;
- ASK ;
- FSK ;

le principe est :

```text
initialiser une fois
maintenir RF/TX clock
modifier seulement le champ symbole
```

Cette séparation est l'une des découvertes les plus utiles du projet.

---

# 40. Le Wi-Fi normal doit-il rester actif ?

Pour un modem autonome expérimental, le dépôt privilégie :

```text
utiliser le Wi-Fi/PHY pour l'initialisation/calibration
puis empêcher PP/LMAC/WDEV de reprendre le TX
tout en conservant RF/PLL/clocks nécessaires
```

Éteindre complètement le PHY par une API de haut niveau n'est pas équivalent à :

```text
arrêter le protocole Wi-Fi mais garder le générateur RF
```

Cette distinction est essentielle.

---

# 41. Conséquences des découvertes CCA pour les modems

## 41.1 Pour TX OOK/ASK/FSK autonome

CCA n'est pas nécessaire pour produire les symboles.

Le modem autonome peut :

```text
sortir du scheduling Wi-Fi normal
contrôler directement le tone slot
```

## 41.2 Pour un modem qui coexiste avec Wi-Fi

Le problème est plus difficile.

Le CCA est consommé par le MAC interne, mais :

```text
pas de getter BUSY fiable identifié
```

Donc une couche custom qui veut « transmettre seulement quand le canal est libre » ne possède pas encore un signal direct CPU simple.

Alternatives possibles :

- laisser le MAC Wi-Fi gérer l'accès si on reste dans son chemin ;
- construire une estimation d'occupation via IQ_EST ;
- utiliser une métrique de bruit/énergie ;
- faire une logique de canal libre purement logicielle.

---

# 42. Occupancy scanner : quelles métriques utiliser

Pour un scanner 2.4 GHz :

## Rapide

```text
IQ_EST / E4
```

Avantages :

- mesure explicitement déclenchable ;
- DONE ;
- vitesse élevée ;
- calibration relative.

## Lent / bruit de fond

```text
read_hw_noisefloor()
0x60009824
```

Avantages :

- interprétation de bruit ;
- utile pour baseline.

Inconvénient :

- pas observé comme métrique burst rapide dans nos tests.

## Événements Wi-Fi

```text
WDEV RAW bit8
```

Peut indiquer activité RX de paquets/chemin RX.

Mais ce n'est pas :

```text
détecteur général de porteuse non-Wi-Fi
```

---

# 43. Comparator RX / CCA : potentiel futur

`0x60009D68` et `0x60009B64[19:12]` montrent qu'un comparator/CCA configurable existe.

Ce bloc pourrait théoriquement être utile pour :

- seuillage hardware ;
- détecteur rapide ;
- wake/sense.

Mais deux obstacles subsistent :

1. la sortie BUSY/FREE n'est pas trouvée côté CPU ;
2. la signification physique exacte des codes n'est pas calibrée.

Donc :

```text
excellent sujet de reverse-engineering
mauvais choix pour une première implémentation modem
```

---

# 44. Récapitulatif des faux amis

| Élément | Ancienne intuition | Conclusion actuelle |
|---|---|---|
| `B00 bit28` | CCA status | **CCA enable/disable** |
| `D68` | possible CCA busy | **comparator/config** |
| `B64` | simple seuil CCA | **registre composite** |
| `0xB4` | -76 dBm | **hypothèse retirée** |
| WDEV bit8 | possible CCA event | **RX event** |
| BACKOFF `[21:12]` | compteur live | **valeur de chargement** |
| RXIQ `580..58C` | RSSI live | **calibration/IQ context** |
| noise-floor | OOK rapide | **trop lent / baseline** |
| `ic_get_rssi()` | RF live | **RSSI contexte paquet** |

---

# 45. Priorité des blocs pour chaque modulation

## OOK TX

Priorité :

```text
1. 0x600005B8 bit18
2. TX clock
3. RF chain/PBUS setup
```

## OOK RX

Priorité :

```text
1. IQ_EST CTRL 0x6000057C
2. E4 0x600005E4
3. gain fixe
4. timing
```

## ASK TX

```text
1. 0x600005B8[17:10]
2. gate actif
3. APWR fixe
```

## ASK RX

```text
1. IQ_EST/E4
2. gain fixe
3. clustering multi-niveaux
```

## FSK TX

```text
1. K = 0x600005B8[9:0]
2. RFPLL/canal fixe
3. scale fixe
4. gate actif
```

## FSK RX

```text
1. valider CFO / 0x60009800 sur CW/FSK externe
2. caractériser valid bit
3. mapper CFO_code -> fréquence
```

---

# 46. Roadmap statique recommandée — sans nouveaux tests Arduino

Puisque les essais Arduino sont arrêtés, les prochaines recherches peuvent rester 100 % statiques.

## 46.1 Fermer complètement le chemin CFO RX

Objectifs :

- désassembler `phy_get_bb_freqoffset()` ;
- identifier exactement :
  - valid bit ;
  - largeur champ ;
  - signe ;
  - scaling ;
- trouver qui écrit/latche `0x60009800` ;
- remonter la condition qui rend la mesure « fresh ».

Résultat attendu :

```text
savoir si une CW arbitraire peut produire un CFO utilisable
sans packet decode Wi-Fi
```

## 46.2 Rechercher un phase-control TX

Chercher dans :

- ROM ;
- libphy ;
- I²C PHY ;
- registres tone voisins ;

des champs liés à :

```text
phase
NCO phase
IQ rotation
CORDIC
sin/cos
txiq phase
```

Si un offset phase direct apparaît, BPSK/QPSK expérimental deviendrait beaucoup plus crédible.

## 46.3 Cartographier les autres tone slots

On connaît solidement slot1.

Il faut déterminer :

- nombre de slots ;
- adresses ;
- mux de sélection ;
- fonctionnement simultané ou exclusif.

Possibilité intéressante :

```text
plusieurs tones simultanés
 -> multi-tone
 -> FSK plus rapide
 -> comb / OFDM-like experiments
```

mais ce n'est pas démontré actuellement.

## 46.4 Comprendre l'origine du K

Chercher si `K` alimente :

- NCO ;
- LUT ;
- DDS ;
- diviseur ;
- phase increment.

Une relation statique pourrait éventuellement révéler :

```text
K -> phase increment
-> Hz
```

sans campagne RF exhaustive.

## 46.5 RX comparator

Continuer la recherche autour de :

```text
D68
B64
C28
D24
```

mais avec la question :

```text
où va la SORTIE comparator ?
```

plutôt que :

```text
quel registre de config ressemble à un status ?
```

Chercher :

- mux vers MAC ;
- signal dans FSM WDEV ;
- latch interne ;
- crossbar / interrupt raw source.

## 46.6 Gain RX déterministe

Pour ASK/M-ASK :

- isoler RF gain ;
- BB gain ;
- AGC hold ;
- saturation ;
- dynamique E4.

Objectif :

```text
métrique amplitude répétable
```

sans auto-normalisation de l'AGC.

---

# 47. Architecture modem recommandée aujourd'hui

Si l'objectif est de fabriquer un « modem expérimental ESP8266 » sur les connaissances actuelles :

## TX

```text
init PHY/RF
    |
set channel / RFPLL
    |
prepare TX RF
    |
TX clock ON
    |
tone slot normalized
    |
+------------------------------+
| gate  -> OOK                 |
| scale -> ASK                 |
| K     -> FSK                 |
+------------------------------+
```

## RX

```text
init RX RF
    |
channel fixe
    |
gain déterministe
    |
+--------------------------------+
| IQ_EST / E4 -> OOK / ASK       |
| CFO          -> FSK (à valider)|
| noise-floor  -> baseline       |
+--------------------------------+
```

---

# 48. Recommandation par ordre de maturité

## Niveau A — prêt pour développement logiciel

### TX OOK
Oui.

### TX ASK
Oui, avec calibration RF.

### TX FSK/M-FSK
Oui au niveau code/symboles ; K→Hz reste à établir.

### RX OOK
Oui comme détecteur relatif IQ_EST.

---

## Niveau B — très prometteur mais nécessite fermeture du RX

### RX ASK/M-ASK
Oui si gain fixe et niveaux E4 séparables.

### RX FSK
CFO prometteur, validation d'actualisation nécessaire.

### RX M-FSK
Même problème + classification multi-seuils CFO.

---

## Niveau C — expérimental

### hybride ASK+FSK
Briques TX présentes, RX 2D à développer.

### FM discrète / sweep
K permet des pas, mais loi/fréquence/phase inconnues.

### PPM/PWM RF
Très réalisables au-dessus d'OOK, performances à caractériser.

---

## Niveau D — non démontré

### CPFSK / MSK / GFSK conformes
Phase/filtrage non fermés.

### BPSK/QPSK arbitraire
Pas de contrôle phase symbole démontré.

### QAM arbitraire
Pas de contrôle I/Q direct.

### OFDM arbitraire
Pas d'interface IFFT/subcarrier CPU démontrée.

---

# 49. Points à ne plus poursuivre comme voies principales

Les investigations suivantes ont maintenant un rendement faible pour la modulation :

```text
chercher CCA_BUSY dans D68
chercher CCA_BUSY dans B64
utiliser WDEV bit8 comme carrier detect
lire BACKOFF[21:12] comme countdown
lire 580..58C sans déclencher IQ_EST
utiliser noise-floor comme OOK haute vitesse
```

Ces pistes peuvent rester intéressantes pour documenter le silicium, mais elles ne sont plus le chemin le plus court vers un modem.

---

# 50. Les découvertes récentes les plus importantes

En ordre d'impact :

1. **CCA master identifié** : `B00 bit28` est un contrôle, pas un status.
2. **B64 décomposé** en résultat noise-floor + param CCA + configs.
3. **D68 confirmé comparator config**, pas BUSY.
4. **aucune lecture CCA status évidente** dans libphy/libpp, même via accès calculés.
5. **WDEV bit8 validé comme RX event**, pas CCA.
6. **backoff CPU-visible = valeur de chargement**, pas countdown live.
7. **Q2 CTRL[31:30]=11 validé comme queue TX active**.
8. **noise-floor v6 localisé à `0x60009824`**.
9. **conversion noise-floor brute mieux comprise**.
10. **RXIQ regs passifs non adaptés au RSSI live**.
11. **IQ_EST/E4 confirmé comme meilleur détecteur OOK/ASK actuel**.
12. **PHY exacte = 1156**, donc corpus tardif et pertinent.
13. **`chip_v6_set_sense/get_sense` sont vides**, éliminant une fausse piste.
14. **table PHY démontre le dispatch étrange AGC→CCA**, rappelant de ne pas faire confiance aux noms SDK.
15. **FSK TX K-switching reste l'une des voies non-Wi-Fi les plus prometteuses.**

---

# 51. Conclusion générale

L'ESP8266 n'est pas un SDR généraliste.

Mais il possède plusieurs primitives RF suffisamment accessibles pour former un **modem spécialisé expérimental** :

```text
TX:
    OOK  -> gate
    ASK  -> digital scale
    FSK  -> K

RX:
    OOK/ASK -> IQ_EST / E4
    FSK     -> CFO potentiel
```

La découverte la plus importante sur le CCA est finalement négative mais très utile :

> Le CCA semble être une décision PHY consommée directement par le moteur MAC/backoff ; aucune sortie BUSY/FREE CPU-readable utilisée par le SDK n'a été trouvée.

Cela permet de recentrer le projet sur les blocs réellement mesurables.

La hiérarchie de travail recommandée devient :

```text
1. OOK TX/RX
2. ASK TX/RX
3. FSK TX
4. fermer CFO RX
5. M-FSK
6. modulation hybride amplitude/fréquence
7. recherche phase-control pour PSK
8. abandonner QAM arbitraire tant qu'aucune interface I/Q/mapping n'est trouvée
```

Le meilleur axe de reverse-engineering statique restant est probablement :

```text
CFO RX
+
tone K / NCO
+
phase accumulator
```

car fermer ces trois éléments transformerait la plateforme actuelle d'un générateur OOK/ASK/FSK expérimental en véritable modem FSK bidirectionnel bien caractérisé.

---

# 52. Référence rapide

```text
TX TONE SLOT 1
0x600005B8
    [9:0]    K / tone_control
    [17:10]  encoded digital_scale
    [18]     gate

IQ EST
0x6000057C
    bit0     enable
    bit1     start
    [16:2]   N
    bit18    mode
    bit31    done

RX METRIC
0x600005E4
    IQ_EST result used for OOK/ASK relative detection

RX BB RESULT
0x60009800
    freq-offset / EVM result area
    validity + frequency field
    exact arbitrary-tone behavior still to close

NOISE FLOOR
0x60009824
    raw v6 noise-floor field

CCA CONTROL
0x60009B00 bit28
    0 = CCA enabled
    1 = CCA disabled

NOISE/CCA COMPOSITE
0x60009B64
    [31:20] noise-floor result
    [19:12] CCA parameter
    [11:9]  noise-check config
    [8:0]   noise-floor config

RX COMPARATOR CONFIG
0x60009D68
    bit18    comparator/CCA mode/config
    [17:10] channel/calibration comparator config

WDEV
0x3FF20C18  enable
0x3FF20C1C  raw
0x3FF20C20  masked/pending
0x3FF20C24  ack

WDEV bit8
    RX event, not CCA

MAC Q2
0x3FF20D90  backoff/load
0x3FF20D94  control
    CTRL[31:30] = 11 observed during active TX queue

BACKOFF[21:12]
    load/initial value
    not demonstrated as live countdown
```

---

# 53. Statut final des hypothèses

```text
[CONFIRMÉ]
Tone gate bit18
ASK scale field
K tone-control
IQ_EST DONE
E4 usable relative RX metric
B00 bit28 CCA enable/disable
B64 composite map
D68 comparator config
WDEV bit8 RX
Q2 CTRL 11 active TX state
backoff field non-live in observed behavior

[TRÈS PROBABLE]
CCA -> internal MAC backoff direct hardware path
noise units finer than dB, quarter-dB-like representation

[À VALIDER]
CFO fresh on arbitrary external tone
K -> exact Hz law
phase continuity on K changes
M-ASK practical separability
M-FSK RX

[NON DÉMONTRÉ]
CPU-readable CCA_BUSY
hidden CCA interrupt
arbitrary BPSK/QPSK phase write
raw I/Q TX FIFO
arbitrary QAM mapping
general OFDM subcarrier control
GFSK/MSK-compliant phase behavior
```

---

## Fin du document
