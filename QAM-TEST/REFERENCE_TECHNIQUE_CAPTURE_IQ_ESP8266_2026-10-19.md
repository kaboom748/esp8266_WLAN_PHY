---
title: "ESP8266 — Référence d’enquête sur la capture I/Q directe"
subtitle: "Registres RFTest, adctrig, accumiq, WDEV, DMA et SRAM — état des preuves"
author: "Synthèse technique des binaires et des essais fournis"
date: "19 octobre 2026 — consolidation corrigée"
lang: fr-CA
---

# ESP8266 — Référence d’enquête sur la capture I/Q directe

**État documentaire : 19 octobre 2026.** Synthèse autonome du manuel *Cartographie détaillée de la PHY ESP8266*, des quatre bibliothèques compilées, du dump ROM et des deux firmwares RFTest transmis, complétée par les relevés matériels du projet. Cette référence ne remplace ni une fiche constructeur ni une qualification expérimentale du circuit.

> **Conclusion centrale.** Le firmware RFTest de l’ESP8266 contient (1) un mécanisme de commande de capture **`adctrig`**, (2) une adresse SRAM annoncée **`0x3FFE8000`** accompagnée du mot **`0x4000`**, (3) un lecteur mémoire **`RdMem`**, et (4) un analyseur **`accumiq`** qui décode des mots 32 bits en **deux composantes signées de 10 bits**. Ces quatre faits constituent une **piste forte de capture I/Q directe**. Ils ne prouvent **pas encore** que le circuit ADC radio écrit effectivement des échantillons temporels dans cette SRAM, ni la taille réelle du transfert, ni son fonctionnement continu.

## 1. Résumé exécutif et niveaux de preuve

| Proposition | Conclusion | Classe de preuve |
|---|---|---|
| Le Wi-Fi ESP8266 transfère des trames par DMA vers la RAM | **Établi** : descripteurs et commandes WDEV | Code compilé, manuel |
| `IQ_EST` est une FIFO RF échantillon par échantillon | **Non** : statistiques/accumulateurs de fenêtre | Manuel, codes des modems |
| Une fonction `adctrig` existe sur ESP8266 | **Établi** : symbole et corps Xtensa | `librftest.a`, deux RFTest `.bin` |
| `adctrig` pilote E20/E24 et annonce 0x3FFE8000 | **Établi au niveau logiciel** | Instructions dans les binaires liés |
| `mac_init_rf` écrit E4C = `0x03020100` | **Établi** | Instructions liées |
| `accumiq` décode 2 × 10 bits signés | **Établi** | Instructions liées, helper signé |
| `adctrig` remplit 0x3FFE8000 avec du vrai I/Q ADC | **Candidat fort, non démontré** | Pas de capture matérielle corrélée |
| La SRAM est attribuée exclusivement au dump I/Q | **Non établi** : régions RX chevauchantes | Cartographie de buffers |
| Capture continue ou FIFO circulaire accessible au CPU | **Non établi** | Aucun test de progression temporelle concluant |

**Vocabulaire utilisé ici :** *établi* = instruction, table de symbole ou comportement directement contrôlé dans un artefact identifié ; *inférence* = conclusion logique plausible mais non validée sur puce ; *inconnu* = absence de preuve dans les éléments examinés. Une absence de référence littérale ne prouve pas l’absence d’accès indirect ou de mécanisme câblé.

## 2. Artefacts étudiés et identification précise

| Source | Nature | Identifiant pertinent |
|---|---|---|
| `ESP8266_RFTest_153_20231018_26M.bin` | Firmware RFTest lié ; quartz 26 MHz | SHA-256 `cfe406ecf2702b136b9efc798b084e2192b7a75aa91d752d855771970c54ee10` |
| `ESP8266_RFTest_153_20231020_40M.bin` | Firmware RFTest lié ; quartz 40 MHz | SHA-256 `8437d35657b717a17b48dd316ffe7cbb62b05aefb172700694f033e16a1d20bc` |
| `librftest(1).a` | Objets RFTest Xtensa LX106 | SHA-256 `e99187ba983f34e00552e974c7516c42a211121484fbfe41848923135cba5414` |
| `libphy(7).a` | PHY et calibrations | Fonctions PHY/ADC, accès MMIO |
| `libpp(8).a` | WDEV/gestion des paquets | DMA RX et contrôle WDEV |
| `libnet80211(6).a` | Protocole 802.11 | Fonctions RX et état système |
| `esp8266_rom(8).bin` | Dump ROM 64 Kio | Routine de lecture des paramètres à `0x40000814` |
| `MANUEL_PHY_CARTOGRAPHIE_PARTIE_1 (2).pdf`, parties 2 et 3 | Référence documentaire corrective | Édition du 18 octobre 2026 |

Les firmwares RFTest possèdent chacun trois segments et un point d’entrée `0x401001D4` :

| Segment | Firmware 26 MHz | Firmware 40 MHz |
|---|---|---|
| Code IRAM | `0x40100000`, longueur `0xDE2C` | `0x40100000`, longueur `0xDE28` |
| Données chargées 1 | `0x3FFF8000`, longueur `0x13A` | `0x3FFF8000`, longueur `0x13C` |
| Données chargées 2 | `0x3FFF813C`, longueur `0x1A30` | `0x3FFF813C`, longueur `0x1A30` |

**Attention aux versions.** L’objet `librftest.a/mac_common.o` n’est **pas identique instruction par instruction** au firmware lié : l’objet appelle `mac_buffer_get` au prologue de `mac_init_rf`, contrairement aux débuts correspondants observés dans les deux images RFTest liées. Les conclusions destinées à une image finale doivent donc d’abord s’appuyer sur **son code lié**, puis utiliser les objets `.a` comme pièces complémentaires.

## 3. Carte des registres pertinents

| Adresse | Champ / opération constatée | Fonction et niveau de preuve |
|---|---|---|
| **`0x3FF20E20`** | `[31]` désarmé/armé selon `adctrig`; `[19]` impulsion mode 0; `[18]` scruté jusqu’à 1; `[17]` affiché | **Contrôle de capture candidat**, fonctions liées et objet `.a` |
| **`0x3FF20E24`** | `[29:16]` extrait sur 14 bits et comparé; `[13:0]` affiché | **Statut/compteur**, signification physique partielle |
| **`0x3FF20E4C`** | Écriture de **`0x03020100`** par `mac_init_rf` | Sélecteur/multiplexeur présumé, sens des octets non validé |
| `0x3FF20CC0` | Écriture de zéro en branche `adctrig` mode 6 | Voie liée au test TX, pas réception passive garantie |
| `0x3FF20004` | `[31]` gate RX; séquence sur `[31:29]` puis `[30]` dans `get_rx_buffer` | Contrôle RX WDEV et handshake ; pas une preuve d’arrêt de tout DMA |
| `0x3FF20000` | RMW `[28]` dans `get_rx_buffer` | État WDEV lié à la synchronisation RX |
| `0x3FF20008`, `0x3FF2000C`, `0x3FF20010` | Pointeurs de descripteurs RX | **DMA Wi-Fi de paquets** vérifié, distinct de l’I/Q brut |
| `0x3FF20E44` | Lectures par `phy_get_rand` et `esp_random` via base `0x3FF20E00+0x44` | Contribution à une valeur pseudo-aléatoire, pas I/Q prouvé |
| `0x3FF00024` | `(ancien & 0xFFFFFFF9) | 0x00000002` par `phy_bb_rx_cfg` | Configuration DPORT ; **pas** identifié comme attribution DRAM I/Q |
| `0x6000057C` | IQ_EST ENABLE/START/N/MODE/DONE | Estimateur de fenêtre, **pas** FIFO d’échantillons bruts |
| `0x600005DC`, `0x600005E0` | Résultats I/Q accumulés de fenêtre | Statistiques RX, résultats mémorisés |
| `0x600005E4` | Énergie de fenêtre E4 | Énergie brute relative, non dBm calibré |

**Remarque décisive :** les adresses `0x3FF20E20/E24/E4C` ne sont pas interchangeables avec les registres `0x600005xx` du générateur TONE ou d’IQ_EST. Sur ESP8266, les chemins de diagnostic RF, les registres de réception de paquets et l’estimateur IQ de fenêtre sont des sous-systèmes distincts.

## 4. Fonctions et adresses dans les firmwares liés

| Fonction | RFTest 26 MHz | RFTest 40 MHz | Rôle observé |
|---|---|---|---|
| `mac_init_rf` | **`0x4010CE08`** | **`0x4010D730`** | Prépare les registres WDEV/RX et écrit le MUX E4C |
| `adctrig` | **`0x4010CF58`** | **`0x4010D87C`** | Programme E20, attend E20[18], examine E24, affiche base/taille |
| `accumiq` | **`0x401002E8`** | **`0x401002E8`** | Lit RAM et calcule des sommes d’échantillons 10 bits |
| Helper signé 10 bits | `0x4010D998` | `0x4010D994` | Conversion 10 bits non signés → signés |
| `rx_buffer_init` | `0x4010CC7C` | `0x4010D5A4` | Configure des descripteurs de paquets RX |
| Gestionnaire console `accumiq` | `0x40101C7B` | `0x40101C7B` | Lit les deux paramètres, puis appelle `accumiq` |
| Gestionnaire console `adctrig` | `0x40101C5D` | `0x40101C5D` | Récupère quatre paramètres et appelle `adctrig` |
| Lecteur console `RdMem` | Zone `0x401019F5` | Même chemin de lecture repéré | Lecture de mots 32 bits en RAM |
| Parseur de paramètres ROM | **`0x40000814`** | **`0x40000814`** | Analyse du texte et stockage d’arguments dans BSS |

Ces adresses **`0x401xxxxx` dépendent strictement de ces deux firmwares** ; ne pas les considérer comme points d’entrée ROM portables. Les adresses ROM `0x400xxxxx` relèvent d’un autre espace d’exécution. Les prototypes des fonctions de la ROM doivent être conservés avec leur provenance.

## 5. `adctrig` : reconstruction de la commande de capture

### 5.1 Paramètres et séquence logique

L’objet `mac_common.o` décrit une fonction de **259 octets** (section `.text.adctrig`, entrée relative `+0x48`). Ses paramètres arrivent, selon l’ABI Xtensa observée, dans `a2`, `a3`, `a4` et `a5` ; notons-les ici **`p0`, `mode`, `p2`, `p3`**. Le mot de commande est assemblé par des **additions et décalages**, non par une API nommée de sélection d’échantillons :

```text
ctrl := (mode << 28) + (p2 << 20) + p0 + 2
si (p3 & 1) == 0 : ctrl := ctrl + 0x8000
E20[31] := 0
E20 := ctrl
E20[31] := 1
si mode == 0 : impulsion E20[19] := 1, puis := 0
si mode == 6 : écriture dans E20CC0 et appel de test_tx_frame
attendre tant que E20[18] == 0
attendre 10000 microsecondes
comparaison E24[29:16] avec p0 + 2
si échec : message d’erreur (sans nettoyage systématique E20[31] montré)
si succès : effacer E20[31], afficher statut, indicateur, 0x3FFE8000, 0x4000
```

Il s’agit d’un **pseudocode d’analyse**, pas d’un pilote Arduino exécutable. Dans la construction originale, certains champs peuvent se recouvrir si l’appelant choisit des arguments inattendus ; le rapport ne les transforme donc pas en champs matériels définitivement indépendants.

### 5.2 Preuve corrigée du test `p0 + 2` — à utiliser à la place des V3/V6/V7

Les rapports intermédiaires ont divergé sur ce point. La vérification des octets **dans les deux firmwares liés** et **dans l’objet d’origine** donne sans ambiguïté :

| Action | 26 MHz | 40 MHz | Octets pertinents |
|---|---|---|---|
| `addi a12,a12,2` **après l’attente DONE** | **`0x4010CFF8`** | **`0x4010D91C`** | **`C2 CC 02`** |
| Extraction `E24[29:16]` | `0x4010D009` | `0x4010D92D` | Instruction `extui ...,16,14` |
| Comparaison `a12` vs champ extrait | **`0x4010D00C`** | **`0x4010D930`** | `beq a12,a2,...` |

Dans `librftest.a/mac_common.o`, la même addition se trouve à `.text.adctrig+0xEC`, après la boucle d’attente à `+0xE1..+0xE6`, et avant la comparaison à `+0x100`. **La valeur attendue du champ `[29:16]` est donc `p0 + 2`.** Les conclusions précédentes indiquant `p0` seul doivent être considérées **périmées**.

### 5.3 Particularités et risques

- Le bit **E20[18] peut déjà valoir 1 au repos** dans nos relevés Arduino : attendre uniquement la valeur haute **ne prouve pas la fraîcheur** d’une acquisition.
- La boucle observée sur E20[18] ne montre **aucun timeout local**.
- L’activation E20[31] n’est **pas effacée sur tous les chemins d’échec**.
- Le **mode 6** appelle `test_tx_frame` : il ne doit pas être présumé passif.
- Aucune écriture directe de l’adresse `0x3FFE8000` dans un registre DMA de destination n’a été retrouvée **dans le corps d’`adctrig`**.
- Le format temporel, la taille exacte, l’ordre des échantillons et la connexion ADC restent inconnus.

### 5.4 Arguments de l’affichage de succès

Dans le chemin de succès 26 MHz, le firmware prépare le format textuel `0x%x,0x%x,0x%x,0x%x` avec les valeurs suivantes :

| Champ affiché | Origine | Preuve 26 MHz |
|---|---|---|
| 1 | `E24[13:0]` | `0x4010D01B`, `0x4010D041` |
| 2 | `E20[17]` | `0x4010D026`, `0x4010D03E` |
| 3 | **`0x3FFE8000`** | littéral chargé à **`0x4010D038`** |
| 4 | **`0x4000`** | littéral chargé à **`0x4010D02D`** |

La version 40 MHz possède des chargements équivalents près de `0x4010D95C` (base) et `0x4010D951` (constante). La valeur `0x4000` = 16 384 décimal. **16 Kio** est une hypothèse de longueur en octets **compatible avec les lecteurs** ; **64 Kio** serait l’hypothèse de 16 384 mots de 32 bits. L’unité de la **capture matérielle** n’est pas établie.

## 6. `accumiq` : lecture RAM et décodage de deux nombres 10 bits

### 6.1 Chaîne d’appels et provenance du pointeur

Le firmware distingue nettement la **table des arguments** de la **zone d’échantillons candidate** :

```text
Texte reçu sur console RFTest
     ↓
Parseur ROM, entrée 0x40000814
     ↓
Arguments en RAM BSS :
   26 MHz 0x3FFF9DC8  |  40 MHz 0x3FFF9DC4
     ↓
Gestionnaire « accumiq » 0x40101C7B
     ↓
accumiq(adresse, longueur_octets) 0x401002E8
     ↓
Lectures 32 bits à adresse, adresse+4, adresse+8, ...
```

Le parseur ROM effectue des écritures de paramètres (notamment instruction à `0x400008DD` et incrément à `0x400008EE`). La table **contient le pointeur et la longueur transmis par la commande**, pas les échantillons. La routine qui compare les noms de commande vers `0x4010B03C` est de type `strcmp`, **pas** un producteur radio.

### 6.2 Instructions de la boucle mémoire

| Adresse commune aux deux images | Instruction | Conséquence |
|---|---|---|
| `0x40100308` | `memw` | Barrière d’ordre CPU |
| **`0x4010030B`** | **`l32i a5,a12,0`** | Lecture directe de la RAM pointée |
| `0x40100311` | `extui ...,0,10` | Extraction des bits `[9:0]` |
| `0x4010031F` | `extui ...,10,10` | Extraction des bits `[19:10]` |
| `0x40100337` | `addi.n a12,a12,4` | Avance de quatre octets |

Le décodeur d’origine était désaligné près de cette boucle ; la séquence **`memw` puis `l32i`** est désormais vérifiée à partir des octets sources. `accumiq` **ne commande ni E20/E24/E4C, ni ADC ni DMA** : c’est un **consommateur RAM**.

### 6.3 Format des mots

```text
bit : 31                  20 19                10 9                 0
      ┌─────────────────────┬─────────────────────┬─────────────────────┐
      │ 12 bits non exploités│   composante B (10b)│  composante A (10b) │
      └─────────────────────┴─────────────────────┴─────────────────────┘
```

Avec `w` un mot de 32 bits :

```text
A10 =  w        & 0x3FF
B10 = (w >> 10) & 0x3FF
sign10(x) = x si x < 512 ; sinon x - 1024
A = sign10(A10)
B = sign10(B10)
SA += A ; SB += B ; Energie += A*A + B*B
```

Les deux champs signés appartiennent à l’intervalle **−512 à +511**. L’ordre physique **I/Q** de A et B n’est pas démontré indépendamment ; les noms A et B sont préférés. Les 12 bits `[31:20]` sont **ignorés par cette fonction**, pas déclarés nuls ou réservés. Le calcul utilise des sommes entières dont les dépassements de capacité doivent être étudiés avant une capture longue.

### 6.4 Différence avec `RdMem`

`RdMem` parcourt également des mots 32 bits à partir d’une adresse console et d’une longueur en **octets** ; elle affiche les mots. `accumiq` calcule des sommes/énergie sur ces mots. La présence des deux commandes rend un flux de travail « acquérir → inspecter → calculer » plausible, mais **leur existence ne suffit pas à prouver que le même producteur les alimente**.

## 7. Origine de la mémoire : producteurs documentés et producteur candidat

### 7.1 Un producteur DMA certain : le RX Wi-Fi normal

`rx_buffer_init` initialise des listes de descripteurs d’environ 12 octets, fait progresser les adresses de données par blocs de 256 octets et écrit des pointeurs WDEV (`0x3FF20008`, ainsi que des chemins voisins). Le transfert DMA **de paquets** vers RAM est donc un fait du corpus. Il ne transporte pas, pour autant, du I/Q ADC brut démontré.

### 7.2 Le producteur candidat : moteur commandé par `adctrig`

`adctrig` active le contrôle E20, attend le statut E20[18], lit E24 et annonce la base `0x3FFE8000`. `mac_init_rf` configure E4C. **La chaîne est cohérente avec une capture de diagnostic**. Mais aucune instruction CPU ne montre le moteur écrivant explicitement les échantillons à l’adresse indiquée. Une destination câblée, un chemin indirect ou une reconfiguration mémoire ailleurs restent possibles.

### 7.3 Ce que le code ne prouve pas

- Le choix physique de l’ADC, la fréquence et la largeur des échantillons réellement capturés.
- La relation directe et temporelle entre les couples A/B et le signal radio.
- Le mécanisme exact de propriété SRAM (bascule, partage ou transfert de banque).
- Une capture circulaire ou une alimentation en continu.
- Une absence certaine d’accès E20/E24/E4C en ROM ou par calcul non reconnu par le décodeur partiel.

## 8. Implantation SRAM, adresses RX et collisions potentielles

Dans les deux firmwares liés, `mac_init_rf` associe des régions RX à la base `0x3FFE8000` :

| Zone RX | Début | Fin exclusive | Taille |
|---|---|---|---:|
| Entrées RX | `0x3FFEA000` | `0x3FFEA300` | 768 octets |
| Entrées secondaires | `0x3FFEA300` | `0x3FFEA400` | 256 octets |
| Données AMPDU | `0x3FFEA400` | `0x3FFED400` | 12 288 octets |
| Descripteurs secondaires | `0x3FFED400` | `0x3FFED580` | 384 octets |
| Descripteurs entrées RX | `0x3FFED700` | `0x3FFEDB80` | 1 152 octets |
| Descripteurs AMPDU | `0x3FFEEB00` | `0x3FFEED40` | 576 octets |

Les six régions totalisent **15 424 octets** (plages non nécessairement contiguës). L’objet `librftest.a/mac_common.o` contient aussi des demandes d’allocation `COMMON`, notamment `txbuffer` (20 480 octets) et `RX_ampdu_buff0_buf` (16 384 octets) ; ces tailles ne définissent pas directement la disposition des symboles dans les firmwares liés.

**Calculs géométriques, conditionnels à l’interprétation de `0x4000` :**

| Hypothèse de dump | Plage présumée | Chevauchement de plages RX |
|---|---|---:|
| 16 384 **octets** = 16 Kio | `[0x3FFE8000,0x3FFEC000)` | **8 192 octets** |
| 16 384 **mots** = 64 Kio | `[0x3FFE8000,0x3FFF8000)` | **15 424 octets** |

Il s’agit d’une **intersection d’adresses**, non d’une preuve que le DMA Wi-Fi et un dump I/Q écrivent simultanément. La mémoire pourrait être réutilisée séquentiellement. Mais elle **ne peut pas être déclarée libre** sans vérifier la séquence et le placement final.

### Contrôle RX WDEV retrouvé

`wDevDisableRx` (dans `libpp.a`) efface `0x3FF20004[31]` et met à jour un indicateur logiciel. `wDevEnableRx` rétablit ce bit. `get_rx_buffer` suit un handshake plus développé : efface `[31:29]` de `0x3FF20004`, efface le bit 28 de `0x3FF20000`, positionne `0x3FF20004[30]`, puis attend que le matériel efface ce bit. Aucun de ces corps ne démontre **à lui seul** qu’il n’y a plus d’écritures DMA en cours ou que la SRAM est réattribuée au dump. Certaines boucles ne sont pas bornées.

## 9. Résultats des essais matériels disponibles

L’utilisateur a testé un ESP8266EX identifié `0x00D60EE8` avec un **quartz 26 MHz** (détecté par esptool) et des sketches Arduino de diagnostic. Les relevés suivants proviennent de ces essais, pas d’une capture I/Q RFTest :

| Essai | Conditions | Résultat |
|---|---|---|
| Préqualification V1 | `WIFI_OFF`, lecture seule de E20/E24/E4C | `E20=0x00040000`, `E24=0`, `E4C=0` |
| Préqualification V2 | `WIFI_OFF` puis `STA_IDLE` | Mêmes valeurs dans les deux états |
| RAMIQ 80 Kio | 160 pages de 512 octets | 154 statiques, 6 intermittentes, 0 très dynamiques |
| RAMIQ 96 Kio | 192 pages de 512 octets | **183 statiques, 9 intermittentes, 0 très dynamiques** |
| RFTest 26 MHz flashé | Sortie ROM de chargement visible, puis console illisible/muette | Démarrage initial visible, **pas de dialogue RFTest confirmé** |

Le balayage RAM a été effectué **sans `adctrig`** et avec un firmware **Arduino différent de RFTest** : son absence de candidat I/Q ne permet donc **pas** d’exclure une capture ponctuelle dans RFTest. Les pages dynamiques appartiennent majoritairement au logiciel et à la pile, sans preuve de flux ADC. Le bit E20[18] observé haut au repos interdit de confondre sa valeur avec une preuve d’acquisition fraîche.

## 10. Points non résolus et ordre de recherche recommandé

| Priorité | Question ouverte | Preuve minimale à obtenir |
|---|---|---|
| **P1** | Que sélectionnent réellement les quatre octets E4C ? | Traces d’initialisation/variation et lien matériel à la source de capture |
| **P1** | Quelle logique écrit dans la SRAM `0x3FFE8000` ? | Moteur identifié + progression de destination + preuve d’écriture fraîche |
| **P1** | Qui possède la RAM durant le dump ? | Contrôle d’attribution et arrêt complet des accès RX DMA ordinaires |
| **P1** | `0x4000` est-il octets, mots ou seulement contrat d’affichage ? | Observation de bornes exactes et lecture cohérente d’un bloc non corrompu |
| **P2** | Quel est le véritable protocole DONE ? | Transition fraîche, statut et compteur cohérents, timeout et récupération |
| **P2** | Quel est l’ordre physique I/Q et la cadence ? | Signaux RF étalonnés, horodatages et analyse temporelle |
| **P2** | Rafale unique ou buffer circulaire ? | Deux acquisitions comparées + suivi du pointeur et de l’état |
| **P3** | Pourquoi RFTest 26M est muet après le boot ROM ? | Analyse du routage UART, PHY-init, secteurs de calibration et console |

**Feuille de route statique** : continuer les références `base+offset` et les chemins de contrôle dans les `.bin` **liés** (prioritaires sur les `.a` si divergents) ; analyser les fonctions autour des écritures E4C/WDEV ; reconstruire les conditions d’appel du gestionnaire `adctrig` ; cartographier le placement de BSS, descripteurs, pile et heap à l’exécution ; rechercher un éventuel contrôleur SRAM non nommé. **Ne pas** transférer sur ESP8266 les adresses ou masques de l’ESP32 par simple analogie.

**Garde-fou expérimental** : ne pas exécuter le mode TX d’`adctrig` ni lancer un dump qui pourrait écraser le firmware tant que les tailles, l’exclusivité SRAM et les chemins de sortie à timeout ne sont pas démontrés. Utiliser des essais radio réglementaires, courts et maîtrisés seulement après ces préconditions.

## 11. Où retrouver les preuves

Les références ci-dessous sont des fichiers de cette étude, non des spécifications du constructeur :

1. **Manuel** : *Cartographie détaillée de la PHY ESP8266*, édition corrective 18 octobre 2026, notamment « Lecture technique TX » (p. 4–10), « Lecture technique RX » (p. 11–15), fiches WDEV/MMIO (à partir de p. 39), « Séquences et dépendances » (p. 303–309), « Inconnues et contradictions » (p. 310–314).
2. **RFTest 26 MHz et 40 MHz** : binaires `.bin` identifiés par leurs SHA-256 ci-dessus ; ils font foi pour les adresses `0x401xxxxx` et les instructions du code lié.
3. **Désassemblage d’origine** : `ESP8266_adctrig_assembleur_annote.txt`, notamment `.text.adctrig+0xE9..+0x100` (addition `p0+2`) et `+0x123..+0x13D` (affichage des résultats).
4. **Accumulateur** : `ESP8266_accumiq_audit_cible_20261019.md`, code de `0x401002E8`, lecture `0x4010030B`, extraction `0x40100311`/`0x4010031F`.
5. **SRAM/DMA** : `ESP8266_IQ_SRAM_AUDIT_v3/RAPPORT_AUDIT_SRAM_RFTEST_v3.md` et `ESP8266_IQ_SRAM_AUDIT_v4/RAPPORT_V4_MEMOIRE_DMA.md`.
6. **Traçage croisé** : `ESP8266_IQ_TRACE_V6/RAPPORT_ACCUMIQ_PRODUCTEUR_V6.md` et `ESP8266_IQ_TRACE_V7/RAPPORT_V7_ACCUMIQ_SRAMPATH.md`. Les affirmations V6/V7 « statut comparé à p0 » sont **remplacées** par la correction binaire de la présente consolidation.
7. **Validation de la correction** : octets liés `C2 CC 02` à `0x4010CFF8` (26M) et `0x4010D91C` (40M), instruction `addi a12,a12,2` ; dans les deux cas, `beq a12,a2` suit l’extraction du statut.

## 12. Conclusion à transmettre aux prochaines équipes

Le **DMA de paquets Wi-Fi** de l’ESP8266 est établi. **Le moteur candidat de capture de diagnostic `adctrig`** est identifié, ses registres de commande E20/E24 et son initialisation E4C sont localisés, et sa branche de succès fournit une adresse `0x3FFE8000` et une quantité `0x4000`. **`accumiq`**, qui consomme une adresse et une longueur saisies dans la console, décode bien deux valeurs signées de 10 bits par mot 32 bits en RAM.

L’ensemble est **plus qu’une simple analogie avec ESP32**, puisqu’il provient directement du code RFTest ESP8266. **Ce n’est toutefois pas encore une démonstration de transfert d’échantillons ADC RF bruts vers SRAM.** Le chaînon physique d’écriture, la propriété de la mémoire, la longueur/cadence et la fraîcheur restent les critères bloquants. Toute prochaine version de l’enquête doit préserver les adresses et les preuves binaires ci-dessus et ne pas transformer ces inconnues en caractéristiques garanties du silicium.
