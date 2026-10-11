# AirModem ESP8266 0.5.13 — Référence technique et guide de reprise

**Version décrite :** `AirModem 0.5.13 — étape 8.6, modes 2/4/8/16-FSK manuels, IF ±6 500 Hz`.  
**Sources faisant autorité :** `modemA/modemA.ino` et `modemB/modemB.ino`, **exclusivement de la version 0.5.13**.  
**Utilisation :** documentation de reprise, diagnostic, modification et validation par un futur développeur.  
**État :** les quatre modes ont été exercés sur ESP8266 durant le développement ; la **réception bidirectionnelle 16-FSK a été observée**. Cela ne vaut pas qualification de production ni preuve de transmission binaire sans perte sur de longues durées.  
**Dernière mise à jour du document :** 11 octobre 2026.

> **IMPORTANT — NE PAS CONFONDRE AVEC LA 0.6.0 :** la 0.5.13 transmet **une seule modulation de données par trame**, fixée au moment de la compilation. Elle **n'utilise ni modulation multizone simultanée, ni Reed–Solomon, ni fragments FEC, ni adaptation automatique**. La trame 0.6.0 possède un format totalement différent. Le présent document reprend la structure de la documentation 0.6.0, mais **remplace ses descriptions par celles du code réel 0.5.13**.

## Sommaire

1. [Résumé opérationnel](#1-résumé-opérationnel)
2. [Sources, empreintes et compatibilité](#2-sources-empreintes-et-compatibilité)
3. [Configuration et paramètres fixes](#3-configuration-et-paramètres-fixes)
4. [Architecture et chaîne de traitement](#4-architecture-et-chaîne-de-traitement)
5. [TDD déterministe maître/esclave](#5-tdd-déterministe-maîtreesclave)
6. [Constellations 2/4/8/16-FSK et recentrage](#6-constellations-24816-fsk-et-recentrage)
7. [Trames : chronogramme et formats par mode](#7-trames--chronogramme-et-formats-par-mode)
8. [Format binaire, CRC et identification du mode](#8-format-binaire-crc-et-identification-du-mode)
9. [Acquisition, réception partielle et limites](#9-acquisition-réception-partielle-et-limites)
10. [Liaison logique, ACK et retransmissions](#10-liaison-logique-ack-et-retransmissions)
11. [Calibration fréquentielle A↔B](#11-calibration-fréquentielle-ab)
12. [UART 19 200, fenêtres série et XON/XOFF](#12-uart-19-200-fenêtres-série-et-xonxoff)
13. [Diagnostics et interprétation](#13-diagnostics-et-interprétation)
14. [Cartographie du code](#14-cartographie-du-code)
15. [Tests, preuves et limites de validation](#15-tests-preuves-et-limites-de-validation)
16. [Risques connus et points à surveiller](#16-risques-connus-et-points-à-surveiller)
17. [Procédure de modification et qualification](#17-procédure-de-modification-et-qualification)
18. [Ce qui a été retiré du dossier 0.6.0](#18-ce-qui-a-été-retiré-du-dossier-060)
19. [Fiche de passation](#19-fiche-de-passation)

---

## 1. Résumé opérationnel

AirModem relie deux ports série via les fonctions radio/IQ spécifiques de deux **ESP8266**. Le système est **half-duplex TDD** : A impose l'horloge temporelle, B recherche A, s'y verrouille puis émet dans le demi-cycle opposé. Un mode « câble série » établit automatiquement une session et transmet les octets du PC, avec contrôle de flux **XON/XOFF** et retransmission de paquets non acquittés.

**Principes de la 0.5.13 :**

- **4 800 symboles physiques/s** et **280 ms par cycle TDD**, quel que soit le mode.
- Choix **manuel** de `AIRMODEM_DATA_FSK_MODE` = `2`, `4`, `8` ou `16` dans **chaque sketch** ; A et B peuvent sélectionner des modes **différents dans chaque direction**.
- Une trame ne mélange **pas plusieurs modulations de données** : le mode du TX reste fixe pour tout son bloc de données. Seuls le SYNC, et selon le mode le contrôle, utilisent 2-FSK.
- Le RX tente les formats **4, 2, 8 et 16-FSK** dans cet ordre, puis ne retient un contrôle que si ses champs et son CRC16 sont valides ; il ne doit livrer les données que si le CRC32 complet est valide.
- Un **SYNC à deux tons** est inséré au milieu de la forme d'onde ; il utilise les niveaux `−3` et `+3` de la grille historique 4-FSK.
- IF cible **±6 500 Hz**, espace nominal **≈1 280 Hz** entre tons 8/16-FSK voisins (unité de niveau ≈640 Hz).
- L'UART utilisateur est **19 200 bauds, 8N1**, XON/XOFF ; la priorité temporelle reste à la radio.
- Les diagnostics textuels partagent le même UART et sont **actifs par défaut** ; les désactiver sur A et B pour PPP ou tout flux binaire transparent.

**À ne pas attendre de cette version :** Reed–Solomon, reconstruction d'une trame partiellement reçue, redondance multi-trames, sélection automatique de modulation, modulation multizone fixe, récupération de morceaux indépendants d'un même paquet. Une erreur CRC32 entraîne le rejet de **toute la charge utile** du paquet concerné ; les retransmissions classiques assurent la récupération éventuelle.

## 2. Sources, empreintes et compatibilité

### Arborescence de référence

```text
AirModem_0.5.13_STAGE8_6_16FSK_IF6500_INO/
├── modemA/
│   └── modemA.ino       # Firmware A, maître TDD
├── modemB/
│   └── modemB.ino       # Firmware B, esclave TDD
└── REFERENCE_TECHNIQUE.md
```

Les deux `.ino` sont **autonomes et monolithiques** : ils incorporent les anciennes sections `.h`/`.cpp`, délimitées par `// ===== ... =====`. Ils doivent être placés dans leurs dossiers homonymes lors de l'ouverture dans l'IDE Arduino. Ils utilisent néanmoins **Arduino ESP8266**, les interfaces SDK `user_interface.h` et les registres/entrées radio ESP8266 : ce ne sont pas des sources C++ génériques portables.

**SHA-256 des deux sources étudiées** (pour identifier précisément cette révision) :

```text
modemA/modemA.ino  a126aea9b2e99ce5b660ea23d09b8c8b16e9f2e6a913a4529c59b4f610244b00
modemB/modemB.ino  0e2a59a117754278dc862c0977a92f4c8ed106345a05aa2aa460cc3c5681f082
```

À la comparaison textuelle, **la seule différence A/B est le rôle final** : `Firmware modem(1)` pour A, `Firmware modem(2)` pour B, ainsi que le nom associé dans `#line`. Une modification fonctionnelle commune doit être reportée dans **les deux** fichiers.

### Compatibilité sur le lien RF

- Les deux cartes doivent utiliser **cette famille de protocole 0.5.13**. Les anciennes versions (en particulier 0.3.0, 0.4.x et le multizone 0.6.0) ne sont pas garanties compatibles.
- En 0.5.13, le mode émis **peut différer entre A et B** ; le RX cherche les quatre formes de paquet, avec magic/version et CRC appropriés.
- Les trames 2/4/8/16 ont le **même nombre de symboles et le même SYNC temporel**, mais **différents formats binaires**. Ne pas changer un `#define` de mode sans conserver les mêmes fonctions d'encodage/décodage des deux côtés.
- **La référence opérationnelle est le couple de fichiers ci-dessus**, pas le nom d'une étape ou une archive d'une branche expérimentale voisine.

## 3. Configuration et paramètres fixes

### Configuration au début de chaque `.ino`

```cpp
#define AIRMODEM_DIAGNOSTICS 1
#define AIRMODEM_DATA_FSK_MODE 16
#define AIRMODEM_IF_TARGET_HZ 6500
#define AIRMODEM_UART_BAUD 19200UL
```

| Paramètre | Valeur 0.5.13 | Note |
|---|---:|---|
| Modulation TX manuelle | `16` par défaut | Choix `2`, `4`, `8`, `16` au moment de compiler |
| Adaptation automatique | **Absente** | Aucun basculement de mode après démarrage |
| Reed–Solomon | **Absent** | CRC + retransmission classique seulement |
| Radio | Canal **6** par défaut (`Phy::Radio`) | Accès radio direct ESP8266 |
| Fréquence CPU requise par `Radio::begin()` | **160 MHz** | `begin()` échoue si CPU ≠ 160 MHz |
| Échantillonnage IQ | **38 400 échantillons/s** | `SAMPLE_RATE` |
| Cadence des symboles | **4 800 symboles/s** | `SYMBOL_RATE` |
| Échantillons/symbole | **8** | `SPS` |
| Durée d'un symbole | **208,333 µs** | Constante sur tous les modes |
| TDD | **280 ms** | `CYCLE = 280000 µs` |
| Demi-cycle A/B | **140 ms** | `HALF = 140000 µs` |
| Symboles/trame | **344** | 320 données/contrôle + 24 SYNC |
| Durée des symboles | **71,667 ms** | `344/4800` |
| Échantillons TX mis en forme | **2 848** | `(344+12) × 8` |
| Durée TX nominale avec mise en forme | **74,167 ms** | Hors temps de préparation RF |
| RX synchronisé | **3 200 échantillons** | **83,333 ms** |
| Recherche RX non verrouillée B | **4 096 échantillons** | **106,667 ms** |
| Filtre | RRC **81 coefficients**, facteur `β=0,2` | `RRC_TAPS=81` |
| IF cible | **±6 500 Hz** | Selon signe de réception/calibration |
| Séparation nominale des tons voisins 8/16-FSK | **≈1 280 Hz** | La valeur instantanée dépend de `unitHz` |
| UART PC | **19 200 bauds 8N1** | XON/XOFF logiciel |
| Diagnostics | Activés (`1`) | Mélangés au flux de données série |

### Débit radio brut et charge utile maximale

| Mode choisi | Bits/symbole | Débit radio brut **pendant TX** | Données utilisateur par paquet, max. |
|---|---:|---:|---:|
| 2-FSK | 1 | 4 800 bit/s | **10 octets** |
| 4-FSK | 2 | 9 600 bit/s | **50 octets** |
| 8-FSK | 3 | 14 400 bit/s | **38 octets** |
| 16-FSK | 4 | 19 200 bit/s | **50 octets** |

> **ATTENTION :** le débit brut de 19 200 bit/s en 16-FSK ne constitue **ni** un débit net UART, **ni** 19 200 bit/s de données utilisateur continues. Le cycle TDD, le SYNC, les 26 octets de contrôle robuste (8/16-FSK), les CRC et les retransmissions réduisent le débit utile. La limite de 50 octets de données en 16-FSK est celle du **protocole logique**, même si 112 symboles de 4 bits fournissent 56 octets bruts.

## 4. Architecture et chaîne de traitement

```text
 PC A                        RADIO TDD HALF-DUPLEX                         PC B
19 200 8N1                                                                19 200 8N1
XON/XOFF                                                                  XON/XOFF
   │                                                                          │
   ▼                                                                          ▼
┌───────────────────────┐                               ┌───────────────────────┐
│ ESP8266 / MODEM A      │                               │ ESP8266 / MODEM B      │
│ UART + tampons         │                               │ UART + tampons         │
│ session / seq / ACK    │  ─────────────────────────►   │ session / seq / ACK    │
│ PHY 2/4/8/16 (TX fixe)│  ◄─────────────────────────   │ PHY 2/4/8/16 (TX fixe)│
│ calendrier TDD maître │                               │ calendrier TDD esclave│
│ mesure CFO + demandes │                               │ mesure CFO + demandes │
└───────────────────────┘                               └───────────────────────┘
```

**Couches logiques actives :**

1. **Radio ESP8266** : programmation directe du ton RF, gating et capture I/Q ; le code ne transporte pas les données dans une pile Wi-Fi/IP normale.
2. **PHY** (`AirModem::Phy`) : génération de signal filtré, SYNC 2-FSK, trame au mode manuel, démodulation fréquentielle signée, CRC16 et CRC32.
3. **Link** (`AirModem::Link`) : découverte de session câble, `seq/ack`, crédits de réception et retransmission **stop-and-wait**.
4. **Modem + Firmware** : tampon UART, XON/XOFF, priorités TDD, diagnostics et calibration réciproque.

Chaque rôle possède **son propre flux série entrant et sortant**. Les modulations A→B et B→A peuvent différer, mais **il n'existe pas de classification des octets utilisateur par priorité**.

## 5. TDD déterministe maître/esclave

### Horloge et créneaux

```text
Repères nominaux       0 ms           140 ms          280 ms          420 ms          560 ms
                       |---------------|---------------|---------------|---------------|
A (maître)             TX + garde      RX + garde      TX + garde      RX + garde
B (verrouillé)         RX + garde      TX + garde      RX + garde      TX + garde
```

Chaque colonne de **140 ms est un créneau**, pas 140 ms de transmission continue. La forme d'onde TX nominale dure ≈74,167 ms. Le temps restant sert aux marges radio, au calcul et au port série.

**Rôle A — maître :**

- Fixe son propre calendrier TX à `epoch + n × 280 ms` et **émet même lorsqu'il n'a aucune donnée utile** (trames de contrôle/IDLE).
- Ne recale **jamais** sa grille temporelle sur B. Si la préparation échoue ou qu'un créneau est expiré, incrémente un compteur de créneaux manqués ; ne tente pas des TX rapprochés pour « rattraper ».
- Écoute B dans le demi-cycle opposé.

**Rôle B — esclave :**

- **Avant verrouillage**, reste en acquisition **RX seule** sur captures étendues de 4 096 échantillons. Il ne transmet pas à l'aveugle.
- Quand `Signal::acquire()` accepte un **contrôle valide**, B peut retrouver l'origine temporelle de A, puis programmer son émission **140 ms après la référence de A**.
- Une erreur de données CRC32 avec contrôle CRC16 valide peut servir à conserver la phase TDD : les octets corrompus ne sont pas livrés.
- Après **six recherches RX programmées manquées**, B enlève `LOCK` et revient en RX de recherche. A continue son calendrier TX normalement.

**Nuance du code :** un commentaire historique indique « full CRC » pour la première acquisition de B, mais **`Firmware::receive()` renvoie vrai dès lors que `Signal::acquire()` fournit un contrôle CRC16-validé**, même lorsque `got.dataValid == false`. Le fonctionnement effectif doit être jugé sur le code, pas sur ce commentaire.

### Priorité radio et UART

L'ordonnanceur `Firmware::poll()` examine les échéances RF **avant** d'accorder du temps au port série. Constantes pertinentes :

```cpp
HALF                = 140000;  // µs
CYCLE               = 280000;  // µs
PREPARE             = 26000;   // µs en mode 8/16, 20000 en mode 2/4
MIN_TX_LEAD_US      = 2800;
UART_XOFF_LEAD_US   = 30000;
UART_PRE_GUARD_US   = 11000;
UART_TX_DRAIN_US    = 8500;
UART_SAFETY_US      = 2500;
```

- XOFF est demandé **à l'avance** ; les écritures UART et les diagnostics sont confinés aux périodes autorisées.
- `beforeTxQuiet()` vérifie la disponibilité du tampon TX série et le temps depuis la dernière écriture ; **il n'appelle pas `Serial.flush()` bloquant** avant le TX.
- Des erreurs restent possibles si le PC ignore XOFF, si l'UART n'est pas vidé ou si les délais RF sont dépassés. Les compteurs TX/guard/durée doivent être mesurés sur matériel.

## 6. Constellations 2/4/8/16-FSK et recentrage

### Niveaux **réellement implémentés**

| Mode | Niveaux signés utilisés pour encoder les bits de données | Bits/symbole |
|---|---|---:|
| 2-FSK | **−3, +3** | 1 |
| 4-FSK | **−3, −1, +1, +3** | 2 |
| 8-FSK | **−7, −5, −3, −1, +1, +3, +5, +7** | 3 |
| 16-FSK | **−15, −13, …, −1, +1, …, +13, +15** | 4 |
| **SYNC universel** | **−3, +3**, tous modes | 1 symbole de SYNC par position connue |

Le niveau est multiplié par une estimation `unitHz`, d'environ **640 Hz/unité** nominalement. Les **tons voisins de 8/16-FSK sont séparés de 2 unités**, soit ≈1 280 Hz. Un **mapping Gray** est utilisé pour les tons 8 et 16, avec conversion inverse côté émission afin de réduire le nombre de bits différents entre deux tons adjacents.

**Point à retenir :** le SYNC 2-FSK ne choisit **pas** les deux extrêmes du 16-FSK (±15). Il choisit **±3**, historiquement extrêmes de la grille 4-FSK. L'étiquette de diagnostic `SYNC=2FSK-EXTREMES` fait référence à cette grille historique, et **non** à toute la constellation 16-FSK.

### Positions fréquentielles nominales (IF +6 500 Hz)

| Mode | Ton minimum | Ton maximum | Observation |
|---|---:|---:|---|
| 2-FSK | +4 580 Hz | +8 420 Hz | Paire ±3 |
| 4-FSK | +4 580 Hz | +8 420 Hz | Tons ±1, ±3 |
| 8-FSK | +2 020 Hz | +10 980 Hz | Tous positifs à IF positive |
| 16-FSK | **−3 100 Hz** | **+16 100 Hz** | **3 tons négatifs** à IF positive |

Les trois tons 16-FSK négatifs sont **−3 100, −1 820 et −540 Hz**. Le discriminateur utilise une fréquence **signée** issue des phases I/Q ; ramener ces valeurs à leur valeur absolue détruirait l'information de symbole.

À 38 400 échantillons/s, les bornes numériques de Nyquist sont **−19 200 / +19 200 Hz**. Le ton 16-FSK nominal le plus haut à +16 100 Hz n'en est distant que de **3 100 Hz**. Ces chiffres sont les **centres des tons**, pas la bande totale occupée : les transitions RRC, les filtres RF et les dérives d'oscillateurs demandent de la marge. La bande utile réellement disponible doit être **mesurée au SDR et sur le démodulateur**.

La cible ±6 500 Hz est un **asservissement fréquentiel effectif**, pas une simple translation logicielle d'une capture IQ déjà acquise. Le filtrage et la qualité des tons proches de zéro ou de Nyquist doivent rester surveillés.

## 7. Trames : chronogramme et formats par mode

### Trame et SYNC communs

```text
Symboles                   0                     160   184                    344
                           |----------------------|SYNC|-----------------------|
Durées                     0 ms              33,333  38,333                71,667 ms
SYNC = 24 symboles, durée 5 ms. 344 symboles × 208,333 µs = 71,667 ms.
```

- `SYNC_AT = 160`, `SYNC = 0xD3916AC5F027` (mot de référence historique servant à former 24 signes ±3).
- **Aucun préambule radio séparé supplémentaire** dans ce chemin actif. Le SYNC est **central** (indices **160..183**).
- L'émetteur effectue un façonnage RRC dans **2 848 échantillons**, pour une durée TX nominale de **74,167 ms**.
- Le RX recherche le SYNC et reconstitue 320 symboles hors SYNC selon un des quatre modes ; la **durée radio est identique** même si le volume binaire utile diffère.

### Agencement précis des quatre modes

| Mode de DATA | Indices de symboles (hors SYNC 160..183) | Structure effectivement modulée | Total brut hors SYNC |
|---|---|---|---:|
| **2-FSK** | 0..159 et 184..343 | 320 bits 2-FSK du paquet compact | **40 octets** |
| **4-FSK** | 0..159 et 184..343 | 320 × 2 bits 4-FSK (paquet 80 octets) | **80 octets** |
| **8-FSK** | `0..159` : CTRL 2-FSK ; `184..231` : CTRL 2-FSK ; `232..343` : DATA 8-FSK | 208 bits CTRL + 112 × 3 bits DATA | **26 + 42 = 68 octets** |
| **16-FSK** | `0..159` : CTRL 2-FSK ; `184..231` : CTRL 2-FSK ; `232..343` : DATA 16-FSK | 208 bits CTRL + 112 × 4 bits DATA | **26 + 56 = 82 octets** |

**Visualisation du mode 16-FSK :**

```text
SYMBOLES        0                  160      184       232                       344
                |------------------|--------|---------|--------------------------|
MODULATION      CTRL 2-FSK (160)  SYNC(24) CTRL2(48)      DATA 16-FSK (112)
DURÉE           33,333 ms          5 ms     10 ms             23,333 ms
                <------------------ 71,667 ms au total ------------------------>
```

**Attention : la phrase « CTRL est toujours en 2-FSK » serait inexacte pour la 0.5.13.** En mode **4-FSK**, les **24 octets d'en-tête** sont dans le bloc ordinaire 4-FSK (blanchi/whitened), protégés par un CRC16 logique, sans section de contrôle physique 2-FSK distincte. Le mode **2-FSK** est entièrement 2-FSK ; les modes **8 et 16** possèdent les **208 symboles de contrôle 2-FSK** dédiés. Tous les modes utilisent le SYNC 2-FSK. **Le message de démarrage codé `CTRL=2FSK-CRC16` est donc trompeur quand `AIRMODEM_DATA_FSK_MODE=4` : se fier à la PHY réellement construite plutôt qu’à cette étiquette.**

Le mot SYNC de 48 bits **n'est pas envoyé comme 48 bits inchangés dans tous les formats** : la fonction `syncExtreme()` lit chaque dibit de la constante historique et n'en conserve que le **signe**, émis sur un des tons ±3.

## 8. Format binaire, CRC et identification du mode

### Champs communs des 24 octets d'en-tête

Tous les modes décrivent les mêmes champs logiques, en **little-endian** pour les valeurs de plusieurs octets :

| Octets du bloc logique | Champ | Taille |
|---|---|---:|
| 0..1 | Magic **`A4 80`** | 2 octets |
| 2 | Version radio du format : **3** logique/4-FSK, **4** 2-FSK, **5** 8-FSK, **6** 16-FSK | 1 |
| 3 | Rôle : 1=A, 2=B | 1 |
| 4..7 | Identifiant de boot (`uint32`) | 4 |
| 8..11 | Identifiant de session (`uint32`) | 4 |
| 12 | Type de paquet : Idle/Open/Ring/Accept/Data/Hangup/Busy/CableOpen | 1 |
| 13 | Longueur utilisateur (limitée par le mode) | 1 |
| 14..15 | `seq` (séquence) | 2 |
| 16..17 | `ack` | 2 |
| 18..19 | `credit` (espace libre RX annoncé) | 2 |
| 20..21 | `txHz` (`int16`) | 2 |
| 22..23 | `tuneHz` (`int16`, `32767` = **NO_TUNE**) | 2 |

### Tableau des formats physiques

| Mode | Bloc de contrôle / en-tête | Données et remplissage | Vérification |
|---|---|---|---|
| **2-FSK** — 40 octets | 0..23 : en-tête, octet version `04` | 24..33 : max. 10 octets | CRC16 en **34..35** sur 0..23 ; CRC32 en **36..39** sur 0..35 |
| **4-FSK** — 80 octets | 0..23 : en-tête, octet version `03` | 24..73 : max. 50 octets | CRC16 en **74..75** sur 0..23 ; CRC32 en **76..79** sur 0..75 |
| **8-FSK** — 68 octets | 0..23 : en-tête version `05`, CRC16 en **24..25** sur 0..23 ; **26 octets** encodés en 2-FSK | 26..63 : max. 38 octets encodés en 8-FSK | CRC32 en **64..67** sur 0..63 |
| **16-FSK** — 82 octets | 0..23 : en-tête version `06`, CRC16 en **24..25** sur 0..23 ; **26 octets** encodés en 2-FSK | 26..75 : max. 50 octets ; **76..77 : remplissage zéro** | CRC32 en **78..81** sur 0..77 |

Les buffers sont initialisés à zéro avant la copie des données : les octets inutilisés n'ont pas de contenu indéterminé. Les CRC :

- **CRC16 de contrôle** (`ctrlCrc16`) : polynôme `0x1021`, initialisation `0xFFFF`, octets 0..23 selon le mode ; **détection**, pas correction.
- **CRC32 de données/paquet** : forme réfléchie du polynôme `0xEDB88320`, init. `0xFFFFFFFF` et inversion finale ; couvre les intervalles listés au tableau.
- **Blanchiment (whitening) : uniquement le paquet historique 4-FSK de 80 octets**, LFSR 9 bits initialisé à `0x1FF`, appliqué avant insertion du SYNC et inversé au RX. Les formats compacts 2-FSK et robustes 8/16-FSK ne réutilisent pas ce whitening de 80 octets.

Pour les modes 2/8/16, le décodeur transpose l'en-tête validé vers une représentation **canonique interne de version 3** pour que le même `Link` puisse traiter les quatre modulations. Cela n'autorise pas à mélanger arbitrairement les formats sur le canal radio.

**Rejet et livraison :** un CRC16 valide permet de reconnaître un contrôle exploitable et éventuellement de retrouver la phase ; **seul le CRC32 valide** autorise le traitement du paquet complet par `Link::incoming()` et l'éventuelle livraison d'octets au port série.

## 9. Acquisition, réception partielle et limites

### RX : chemin actif

1. `Radio::capture()` acquiert les I/Q de la fenêtre TDD.
2. `demodulate()` enlève une composante DC, mesure les différences de phase I/Q signées, puis recentre circulairement le discriminateur pour atténuer les discontinuités près de Nyquist.
3. `Signal::acquire()` applique un filtrage adapté RRC, recherche une corrélation avec les **24 signes du SYNC** sur différentes positions temporelles et estime `offsetHz` et `unitHz`.
4. Il essaie successivement les paquets **4-FSK, 2-FSK, 8-FSK, 16-FSK**. Les quatre voies partagent la cadence des symboles et le SYNC.
5. Il valide les champs du contrôle et son CRC16. Il tente le CRC32 indépendant pour la charge utile ; si une trame complète est disponible, elle est préférée à un candidat contrôle-seul.
6. La réception d'un contrôle valide peut fournir une origine temporelle à B ; les données sont traitées par `Link` **seulement si `dataValid` est vrai**.

**Sur le 8-FSK** : le code possède une **seconde tentative de classification** avec estimation de fréquence raffinée depuis les 208 symboles de contrôle connus + 24 SYNC. Cette récupération est propre au chemin 8-FSK (`eightRefinedRecovery`). **Ne pas prétendre que cette correction raffinée existe déjà pour le 16-FSK**.

**Sur le 16-FSK** : le décodage des 112 symboles rapides utilise les niveaux signés `−15..+15` impairs et la conversion Gray. Les tons négatifs à IF positive sont nécessaires ; une représentation en valeur absolue échouerait. Une corruption du CRC32 fait rejeter la charge utile, même si le contrôle a été reçu.

### Trois notions distinctes

| Observation | Ce qu'elle démontre | Ce qu'elle ne démontre pas |
|---|---|---|
| `SYNC-CANDIDATE-ONLY` | Une corrélation a été observée | CRC16/contrôle valide ou réception de données |
| `CTRL-ONLY` | Champs de contrôle/CRC16 reconnus sans données CRC32-valides | Que les octets utilisateur ont été livrés |
| `RX` (compteur complet) | Paquets CRC32-valides acceptés par la PHY | Absence de perte finale, ni intégrité d'un long transfert |

**Limite structurante :** le contrôle reçu sans données permet une synchronisation temporelle et, dans certains cas, une calibration, mais **ne permet pas de récupérer une fraction des données** : la 0.5.13 ne possède **aucun système de fragments indépendants**. Le paquet doit être retransmis en entier pour être livré.

## 10. Liaison logique, ACK et retransmissions

`AirModem::Link` maintient un protocole **stop-and-wait** avec une seule charge utile en attente d'ACK (`pending`, `attempts`, `txSeq`, `rxSeq`, `peerCredit`).

- Les deux côtés établissent automatiquement le mode **câble** (`CableOpen`), avec A prioritaire pour résoudre les demandes simultanées.
- Les types de paquet sont `Idle`, `Open`, `Ring`, `Accept`, `Data`, `Hangup`, `Busy`, `CableOpen` ; les états de session sont `Idle`, `Dialing`, `Ringing`, `Answering`, `Connected`, `Closing`.
- Chaque paquet de données porte `seq`, l'ACK distant dans `ack` et le crédit RX (`credit`). Les octets restent en attente tant qu'un ACK attendu n'est pas reçu et les tentatives sont répétées sur les cycles suivants.
- Le RX rejette les séquences inattendues, ignore les doublons déjà vus et **ne livre jamais la charge utile d'une trame CRC32 invalide**.
- Les plafonds de charge utile par **paquet TX** sont ceux du tableau de la section 3 ; ce paramétrage ne varie pas à l'exécution.
- `connectTimeout = 60000 ms`, `lossTimeout = 15000 ms`, limite de tentatives de paquet **64** (les conditions exactes sont dans `Link::outgoing()`).
- **Sans RS**, une perte répétée peut retarder la livraison, déclencher `Retries` ou un timeout de session. Il n'y a pas de garantie absolue de conservation des données pendant une interruption prolongée.

**Cas important pour la reprise :** changer `AIRMODEM_DATA_FSK_MODE` nécessite une **recompilation** (et éventuellement des capacités utiles différentes). Cela ne constitue pas un mode négocié en temps réel. Les deux sens peuvent être asymétriques, mais les paquets en attente ne sont pas redimensionnés automatiquement pendant une transmission.

## 11. Calibration fréquentielle A↔B

La boucle bidirectionnelle d'asservissement est une fonction centrale de cette version. Elle ne synchronise pas le **temps TDD** (phase de A) : elle ajuste la **position fréquentielle** des tons émis.

| Paramètre de calibration | Valeur du code |
|---|---:|
| IF cible en valeur absolue | **6 500 Hz** |
| Tolérance de centrage (`CAL_DEADBAND_HZ`) | ±450 Hz |
| Variation max. d'une demande calculée | **1 200 Hz** |
| Variation max. appliquée depuis une trame complète | **600 Hz/trame** |
| Variation max. depuis contrôles 8-FSK seuls cohérents | **300 Hz/réception** |
| Bornes absolues de correction TX (`txHz`) | ±12 000 Hz |
| Mesure d'offset admise | `|offsetHz|` de **1 000** à **18 750 Hz** |
| Unité de fréquence admise pour la calibration | `|unitHz|` de **100** à **5 000 Hz** |
| Sentinelle `NO_TUNE` | **32767** = pas de demande ; **ne jamais l'appliquer** |

### Boucle sur une trame complète valide

```text
A émet : annonce txHz_A, tuneHz_A
                   ↓
B mesure : offsetHz_B, unitHz_B
                   ↓
B calcule une demande ABSOLUE pour le TX de A,
  à partir du txHz_A annoncé, de la cible ±6500 Hz et du signe du discriminateur
                   ↓
B transmet tuneHz_B dans son prochain paquet
                   ↓
A accepte la demande uniquement si la trame est validée,
  borne la variation à 600 Hz et annonce son nouveau txHz_A
                   ↓
Boucle réciproque dans l'autre sens
```

- `tuneRequest(transmitted, received, unit)` calcule la correction distante souhaitée **en coordonnées absolues relatives au `txHz` annoncé par l'émetteur**, afin d'éviter l'accumulation des ordres anciens.
- `boundedTxCorrection()` limite la variation appliquée, avec garde sur `NO_TUNE` et les bornes ±12 kHz.
- Le RX n'accepte pas une correction RF distante sur une mesure aberrante ; la cible choisit `+6500` ou `−6500` selon le signe de `received`.
- **Trame CRC32 valide :** mise à jour normale de la calibration, des ACK et de la liaison logique.
- **Contrôle CRC16 seul :** pour le chemin **8-FSK uniquement**, le code peut appliquer une correction réduite après **deux contrôles cohérents**, même `boot`, moins de 3 secondes entre mesures et écarts d'offset inférieurs à 2 400 Hz. **Ce mécanisme n'est pas activé pour le 16-FSK contrôle-seul** dans cette version, bien que son timing TDD puisse être conservé.

Dans les diagnostics, `OFFSET-HZ` ou `RAW-OFFSET` proche de ±6 500 Hz **n'est pas une erreur à annuler** : la valeur cible est ±6 500 Hz. Le bon indicateur de convergence est `ERR-HZ`, proche de **zéro**. `TX-APPLIED` n'a pas non plus vocation à revenir à zéro.

## 12. UART 19 200, fenêtres série et XON/XOFF

### Chemin série

- `Serial.setRxBufferSize(2048)` puis `Serial.begin(19200, SERIAL_8N1)` ; les deux ESP8266 se connectent à leurs PC au **même débit UART fixe**.
- 19 200 bauds 8N1 = **10 bits/caractère**, soit **≈520,833 µs par caractère**, **1 920 octets/s** théoriques côté UART.
- Tampons logiques `Link`: **2 048 octets TX + 2 048 octets RX** ; tampon préconnexion câble : **1 024 octets** (`cablePending`). Ce sont des capacités du code, pas un engagement de débit sur la radio.
- `Modem::updateFlow()` émet **XOFF `0x13`** si l'ordonnanceur le demande ou que la file atteint **512 octets** ; **XON `0x11`** quand l'ordonnanceur le permet et que le cumul redescend à **256 octets ou moins**.
- La boucle traite au plus **12 caractères UART reçus** et **8 caractères UART émis** par appel, en respectant les budgets temporels. Les diagnostics sont drainés par très petites rafales, seulement quand l'UART peut servir.
- **XON/XOFF est logiciel** : le PC peut encore envoyer des octets en vol après XOFF. Le firmware garde une réserve, mais un dépassement reste possible, notamment si un pilote ignore le contrôle de flux.

### Transparence binaire : précautions indispensables

Le firmware **intercepte `0x11` et `0x13` comme des caractères de contrôle XON/XOFF locaux** : ils ne sont donc pas des données utilisateur ordinaires dans le flux entrant. Un protocole transportant ces octets (PPP ou autre) doit les **échapper ou les gérer explicitement**. Ne pas affirmer une transparence octet par octet pour tout flux arbitraire sans cette précaution.

**Les diagnostics sont actifs par défaut et écrits sur le même `Serial` que les données.** Ils rendent le flux invalide pour PPP ou un test de hachage binaire. Pour des essais transparents, régler `AIRMODEM_DIAGNOSTICS 0` **dans les deux sketches**, puis recompiler/flasher les deux modems.

### Démarrage à l'ouverture du port COM

Le code peut émettre `[BOOT]` et `[RESET-REASON=...]`. Un diagnostic `External System` suggère un reset externe. Certaines interfaces USB-série ESP8266 relient **RTS/DTR** à la circuiterie de programmation automatique ; l'ouverture du port COM peut modifier ces lignes. **XON/XOFF ne provoque pas directement le reset**. Le motif rapporté par l'ESP8266 ne prouve pas à lui seul que PuTTY est responsable. Tester séparément les ouvertures de ports A et B.

## 13. Diagnostics et interprétation

Les diagnostics sont mis en file (FIFO de **384 caractères**), écrits **sans blocage**, et peuvent être perdus sous charge. Le compteur `DIAG-DROPS` ne signifie pas que des octets utilisateur ont été perdus. Les messages périodiques sont produits à environ **5 s**, avec des détails alternés.

| Diagnostic | Interprétation correcte |
|---|---|
| `[BOOT]` | Initialisation du firmware, rôle et mode TX compilé |
| `[RESET-REASON]` | Raison générale fournie par l'ESP8266 (watchdog, externe, etc.) |
| `[DIAG][PHY=nFSK-4800]` | **Mode TX sélectionné**, pas les quatre modes en parallèle |
| `[VITESSE-MAX]` | Débit **brut** nominal 4 800/9 600/14 400/19 200 bit/s selon mode |
| `TX` | Émissions RF achevées et comptabilisées comme réussies |
| `RX` | Paquets PHY dont **CRC32 complet est valide** |
| `ABORTS` | Somme `txAbort + captureAbort` ; consulter le détail pour les séparer |
| `LOCK` | Synchronisation locale TDD ; pour A (maître) **toujours 1 dans l'affichage**, sans prouver que B répond |
| `SYNC-MISS` | Réceptions programmées manquées selon le compteur diagnostic |
| `SYNC-CANDIDATE-ONLY` | Corrélation de SYNC détectée, mais pas de contrôle exploitable |
| `CTRL-ONLY` | En-tête/CRC16 valides, **CRC32 DATA invalide** |
| `OFFSET-HZ` | Fréquence estimée reçue ; cible ±6 500 Hz après convergence |
| `ERR-HZ` | Écart résiduel à la cible IF ; chercher une valeur proche de zéro |
| `TX-APPLIED` | Valeur de correction locale réellement appliquée au TX |
| `REMOTE-REQ` / `LOCAL-REQ` | Demandes fréquentielles reçue et émise |
| `NO_TUNE=32767` | Pas de mesure/commande de correction exploitable |
| `TX-TRY` / `TX-FAIL` / `TX-ABORT` | Distinguer préparation refusée, garde et incident RF |
| `TX-SLOT-MISSED` / `TX-GUARD-*` | Dépassements de budget TDD ou UART non vidé |
| `PREP-MAX` / `DEMOD-MAX` / `ACQUIRE-MAX` | Durées maximales des traitements, **à corréler au budget TDD** |
| `[R8] CTRL-OK/DATA-OK/DATA-FAIL` | **Compteurs propres au chemin 8-FSK**, pas un compteur générique 16-FSK |
| `shadowFrames / shadowMatch / shadowMismatch` | **Statistiques internes** d’observation passive huit tons. Elles sont mises à jour en mémoire, mais **aucune ligne `[FSK8-SHADOW]` n’est imprimée par `diagnosticTick()` dans cette version**. |
| `UART-ERRORS` | Erreurs série détectées ; à investiguer séparément des CRC radio |
| `XON` / `XOFF` | Commandes de contrôle de flux produites, pas quantité de données envoyées |
| `TX-QUEUE` / `RX-QUEUE` | Octets en attente côté liaison logique |
| `DIAG-DROPS` | Rapports de diagnostic éliminés pour préserver le timing |

**Pièges d'interprétation :**

- `LOCK=1` sur A peut apparaître avec **RX=0** ; le maître possède sa propre horloge.
- Des caractères visibles dans PuTTY prouvent une activité, **pas l'intégrité d'un long flux binaire**.
- Un CRC16 de contrôle valide ne prouve **pas** que la charge utile a été récupérée.
- Des offsets à `±6500 Hz` sont voulus ; c'est `ERR-HZ` qu'il faut ramener près de zéro.
- `R8` peut rester à zéro dans une réception 16-FSK parfaitement fonctionnelle ; ces compteurs ne décrivent pas le décodeur 16-FSK.

## 14. Cartographie du code

Les deux sketches ont la même organisation : les séparateurs `// ===== ... =====` permettent de retrouver les anciennes responsabilités sans avoir à extraire les modules en fichiers séparés.

| Section fusionnée / fonction | Responsabilité réelle dans la 0.5.13 |
|---|---|
| `ModemProtocol.h` | Constantes, `Packet`, CRC16/CRC32, paquets 2/4/8/16, mapping Gray, calibration |
| `ModemLink.h` / `ModemLink.cpp` | Sessions, câble automatique, ACK, séquences, crédits RX, retransmissions |
| `AirModem.h` / `AirModem.cpp` | Tampon préconnexion, XON/XOFF, octets série entrants et sortants |
| `ModemSignal.h` / `ModemSignal.cpp` | `makeFrame()`, `waveform()`, `demodulate()`, `Signal::acquire()`, filtre RRC, SYNC |
| `ModemRadio.h` / `ModemRadio.cpp` | Génération RF sur registres ESP8266, capture IQ, préparation TX, statistiques de retard |
| `ModemFirmware.h` / `ModemFirmware.cpp` | Machine TDD A/B, garde UART, calibration, diagnostics et configuration |
| `FskPhase.h` | Calcul de phase fréquentielle signée utilisé au décodage |
| `RH_ESP8266FSK.*` | **Partiellement actif** : initialisation et basculement des chemins RF TX/RX appelés par `Phy::Radio`. Son ancien framing/poll générique n’est pas la construction de trames AirModem 0.5.13. |
| `Fsk4.*`, `FskCodec.*`, `FskFrame.*` | Code de compatibilité/historique incorporé : **ne décrit pas le format de trame actif** créé par `Phy::makeFrame()`. |
| `setup()` / `loop()` | Instanciation du rôle et appel continu à `Firmware::poll()` |

**Points d'entrée à consulter avant toute modification :**

```text
MODE TX manuel                 AIRMODEM_DATA_FSK_MODE
IF / calibration              AIRMODEM_IF_TARGET_HZ ; tuneRequest ; boundedTxCorrection
Encodage des quatre modes     encodeTwo / encode / encodeEight / encodeSixteen
Mapping des tons              symbol ; syncExtreme ; ungray8 ; ungray16
Forme d'onde                  Signal::waveform
Acquisition                   Signal::acquire
Horaires radio                Firmware::poll ; Firmware::transmit ; Firmware::receive
Protections UART              Firmware::serial ; beforeTxQuiet ; Modem::updateFlow
ACK / retransmissions         Link::incoming ; Link::outgoing
Affichage                     Firmware::diagnosticTick
```

**Ne pas déduire du nom `FskFrame.h` historique que le chemin actif utilise son ancien préambule/CRC16**. Le chemin effectif de la 0.5.13 est celui de `Phy::makeFrame()` puis `Radio::prepare()`.

## 15. Tests, preuves et limites de validation

### Ce qui a été vérifié dans l'environnement de développement

Les tests hôte ont été relancés sur les sections du code 0.5.13 :

| Test | Résultat observé | Portée de la preuve |
|---|---|---|
| Décodage IQ 2-FSK | **54/54** | Conditions IQ synthétiques |
| Décodage IQ 4-FSK | **54/54** | Conditions IQ synthétiques |
| Décodage IQ 8-FSK | **54/54** | Conditions IQ synthétiques |
| Décodage IQ 16-FSK | **54/54** | Conditions IQ synthétiques, **tons signés** |
| Gray 16 et paquets 16-FSK | **1 020** contrôles encodage/décodage/CRC | Logique des octets et rejets |
| Comparaison A/B | Différence limitée au rôle final | Cohérence des sources |

Ces essais IQ comprennent plusieurs offsets de fréquence simulés, inversions du discriminateur, retards de capture et niveaux de bruit. **Ils ne reproduisent pas l'intégralité du matériel RF** (filtre analogique, dérive réelle, scheduler Xtensa, saturation de buffers, watchdogs, alimentation).

### Ce qui a été observé sur les ESP8266 durant le développement

- Les modes **2-FSK et 4-FSK** ont été confirmés fonctionnels par l'utilisateur à l'étape 7.
- La chaîne **8-FSK** a finalement échangé des trames bidirectionnelles après les corrections de grille et de calibration.
- En **0.5.13 16-FSK à ±6 500 Hz**, une capture a montré `LOCK=1` et `ABORTS=0` sur A et B, des compteurs RX/TX progressant et des erreurs `ERR-HZ` d'environ **−136 et −132 Hz** ; des caractères apparaissaient dans les terminaux.

**Ces constats sont des essais initiaux**, pas un procès-verbal de qualification. L'intégrité d'une longue transmission binaire bidirectionnelle, avec et sans erreurs RF, n'a pas été démontrée. Aucune certification réglementaire de spectre ou d'émissions n'est impliquée.

### Vérifications restant à faire sur le matériel

1. Compiler avec **la vraie chaîne Arduino ESP8266**, relever la version du core, le CPU, les warnings, et l'occupation RAM/IRAM/flash.
2. Vérifier au SDR les **16 centres fréquentiels** et la largeur occupée, en particulier les **3 tons négatifs** dans le modèle IQ à IF positive et le ton haut nominal à +16,1 kHz.
3. Vérifier sur deux cartes les **quatre modes**, y compris configurations asymétriques A=16/B=4 et A=2/B=16.
4. Mesurer l'erreur de calibration au démarrage, le temps de convergence, les corrections alternées et la fréquence résiduelle après plusieurs minutes.
5. Mesurer `PREP-MAX`, `ACQUIRE-MAX`, `TX-SLOT-MISSED`, `TX-GUARD-UART`, `TX-ABORT` en forte charge UART et à signal dégradé.
6. Transmettre **un flux numéroté et vérifié octet par octet**, d'abord A→B puis B→A et simultanément, avec **diagnostics désactivés**.
7. Tester XON/XOFF lorsque le PC envoie plus vite que le débit radio utile ; valider le traitement des octets 0x11/0x13 échappés.
8. Tester coupures radio de durée croissante, timeout de session (**15 s** en Connected), reboot et ouverture de PuTTY/COM (DTR/RTS).
9. Consigner les résultats par mode : taux de paquets CRC32-valides, débit net livré, délai de livraison, pertes finales, comportement des tampons.

**Critère de stabilité proposé :** aucun octet corrompu, perdu ou dupliqué dans des transferts contrôlés ; aucune collision half-duplex ni émission hors créneau ; TDD et calibration stables sur des heures ; ressources CPU/RAM dans les limites réelles. `LOCK=1` ne suffit pas.

## 16. Risques connus et points à surveiller

1. **16-FSK proche des limites du discriminateur :** avec IF positive +6,5 kHz, centres de −3,1 à +16,1 kHz, marge de centres seulement **3,1 kHz** vis-à-vis de +19,2 kHz. Le spectre réel peut dépasser ces centres.
2. **Contrôle 4-FSK non émis en 2-FSK :** ce mode a CRC16 de contrôle, mais son acquisition de champs dépend de la réception 4-FSK de ces octets ; ne pas présumer l'indépendance d'un contrôle 2-FSK dans les quatre formats.
3. **Calibration contrôle-seul inégale :** l'asservissement limité depuis deux CRC16-valides est spécifique à **8-FSK** ; aucune équivalence automatique dans le RX 16-FSK.
4. **Pas de FEC :** CRC16/CRC32 détectent mais ne reconstruisent pas les données ; la retransmission d'un paquet complet reste nécessaire.
5. **Stop-and-wait / taille fixe par mode :** les changements manuels ne sont pas des changements dynamiques sûrs au milieu d'une session sans reflash/reconnexion.
6. **Longueur utilisateur en 16-FSK plafonnée à 50 octets**, malgré 56 octets bruts dans la portion rapide. Ne pas annoncer le débit brut comme débit utile.
7. **Diagnostics sur UART transparent :** les chaînes `[DIAG]`, `[CAL]`, etc., sont mélangées aux octets utilisateur ; incompatibles avec PPP non échappé et les transferts de référence.
8. **Les octets UART 0x11 et 0x13** servent à XON/XOFF ; le transport n'est pas intégralement transparent sans échappement applicatif.
9. **Risque de reset USB-série :** l'ouverture du port COM peut agir sur DTR/RTS ; examiner les circuits de la carte et `RESET-REASON`, sans attribuer automatiquement tout reboot au watchdog.
10. **Contrainte de temps CPU/IRAM :** `waveform()`, `Signal::acquire()` et les chemins RF avec interruptions contraintes exigent une validation compilée Xtensa et un relevé de ressources.
11. **Code monolithique et historique** : éviter de modifier le mauvais décodeur (`FskFrame.h`/pilote historique) ; tests de non-régression et comparaison A/B obligatoires.
12. **Une absence de réception n'interdit pas le TX de A**, mais B en recherche est RX seule. Le compteur `LOCK=1` de A est local et ne prouve pas un paquet reçu.
13. **Déconnexions après timeout :** `lossTimeout = 15000 ms`, tentatives d'ACK limitées ; ne pas promettre « aucune coupure » en toute circonstance.
14. **Aucun RS / multizone / adaptation automatique :** ne pas importer implicitement les hypothèses ou compteurs de la documentation 0.6.0.

## 17. Procédure de modification et qualification

### Avant d'éditer

1. Créer une copie immuable des deux `.ino` **0.5.13** et conserver leurs SHA-256 ci-dessus.
2. Choisir **une seule modification fonctionnelle** (ex. seuil de calibration ou décodeur 16-FSK), sans toucher simultanément au TDD et à l'UART.
3. Noter si la modification casse le format radio (taille, SYNC, version, CRC, tone map, acquisition), le comportement d'ACK ou seulement les diagnostics.
4. Mettre à jour **A et B en parallèle** ; vérifier par `diff` qu'ils ne diffèrent que par le rôle, sauf différence explicitement voulue.

### Ordre conseillé de validation

1. Tests unités des CRC, encodages et longueurs de données 0..maximum.
2. Simulation IQ aux quatre modes ; offsets ±, inversion, dérive, erreurs sélectives, SYNC et CRC.
3. Tests de compatibilité croisée : A=2/B=16, A=16/B=4, A=8/B=4, etc.
4. Compilation réelle ESP8266 / rapport RAM/IRAM/flash, vérification des erreurs et warnings.
5. Mesure des créneaux TX/RX à l'instrument, pas uniquement par les journaux.
6. Transfert binaire contrôlé avec diagnostics OFF, contrôle de flux effectif, plusieurs durées et niveaux de signal.
7. Mise à jour de ce `.md` : empreintes, paramètres, mesures, limites nouvelles et procédure de retour arrière.

### Discipline de livraison

- Livrer **deux fichiers séparés** `modemA.ino` et `modemB.ino`, **pas de ZIP**.
- Accompagner les changements de source d'un résultat de tests **honnête** : « compilé sur ESP8266 », « testé IQ hôte », « vérifié sur cartes » sont des qualifications **différentes**.
- Ne pas remplacer une version confirmée par une branche expérimentale sans conserver un retour arrière.
- Ne pas renommer un ancien format binaire tout en gardant ses magic/version inchangés si les champs ne sont plus compatibles.

## 18. Ce qui a été retiré du dossier 0.6.0

Cette section sert de **contrôle anti-confusion** lorsque le prochain technicien reçoit aussi la documentation de la 0.6.0.

| Ancien sujet du dossier 0.6.0 | État réel de la 0.5.13 |
|---|---|
| « Quatre zones FSK successives dans chaque trame » | **Supprimé** : une seule modulation DATA par trame, fixe par compilation |
| Contrôle multizone 16 octets, magic `C7 61` | **Absent** : en-têtes `A4 80`, versions 3/4/5/6 selon mode |
| Trame multizone 128 CTRL + 88 DATA 2-FSK + 24 4-FSK + 32 8-FSK + 48 16-FSK | **Absent** : les frontières changent suivant le **mode manuel** |
| Reed–Solomon `RS(255,20)` et `GF(256)` | **Absent** |
| Fragments de 4 octets / CRC8 par cellule | **Absents** |
| 9 fragments par émission (2/1/2/4) | **Absent** |
| Accumulation sur plusieurs cycles et reconstruction à 20 fragments | **Absentes** |
| Identifiants de génération, index `start`, ACK de génération FEC | **Absents** : seulement `seq/ack/credit` du `Link` historique |
| Deux couches de retransmission, FEC + Link | **Absent** : **Link stop-and-wait seulement** |
| `Multi::Engine` / `makeMultiFrame()` | **Absents du chemin 0.5.13** |
| `AIRMODEM_MULTIZONE=1` | **Non présent** ; choisir `AIRMODEM_DATA_FSK_MODE` |
| Timeout 60 s généralisé du multizone | **Non applicable** : `lossTimeout=15 s` en 0.5.13 |
| Test matériel de la 0.6.0 manquant | **Non transposable** : la 0.5.13 possède ses propres essais initiaux sur cartes |

Les parties réutilisables du dossier précédent sont **l'architecture ESP8266, les règles TDD, les précautions UART/XON-XOFF, la calibration de fréquence et la discipline de tests** ; seules les valeurs vérifiées directement dans la 0.5.13 sont reprises ici.

## 19. Fiche de passation

**Référence à maintenir :** `AirModem 0.5.13 — étape 8.6`, fichiers A/B identifiés par SHA-256.  
**Produit :** liaison série radio TDD half-duplex ESP8266 ; **A maître**, **B esclave**.  
**Modulation :** **2/4/8/16-FSK MANUEL**, un mode de données par trame, fixé à la compilation (16-FSK par défaut) ; RX multi-format.  
**Radio :** **4 800 symboles/s**, 8 échantillons/symbole, SYNC central **24 symboles 2-FSK ±3**, IF **±6 500 Hz**, espacement voisin ≈1 280 Hz.  
**Temps :** cycle **280 ms**, demi-cycle **140 ms**, trame **344 symboles = 71,667 ms**, forme d'onde nominale ≈74,167 ms.  
**Données :** 10 / 50 / 38 / 50 octets utiles maximum selon 2/4/8/16-FSK. CRC16 contrôle et CRC32 paquet, **sans Reed–Solomon**.  
**Liaison :** session câble automatique, ACK séquentiel et retransmissions **stop-and-wait** ; timeout liaison connectée **15 s**.  
**UART :** **19 200 8N1 XON/XOFF**, radio prioritaire ; diagnostics sur **même port**, actifs par défaut et à désactiver pour PPP.  
**Validation :** essais hôte 54 IQ par mode et 1 020 cas de paquet 16-FSK ; essais radio initiaux en 16-FSK, **pas de certification longue durée**.  
**À conserver :** les deux sketches originaux et cette documentation, avec leurs versions/hachages.

**Priorité du prochain développeur :** ne pas confondre cette version manuelle à trame monomode avec le multizone RS 0.6.0 ; **préserver le TDD et la calibration** ; valider sur ESP8266 la chaîne complète, les ressources et l'intégrité du flux série **avant** toute modification profonde.
