# Projet AirModem REV7 — Modem radio 300 bauds sur ESP8266

## 1. Résumé du projet

Le projet **AirModem REV7** est un modem radio conçu autour de deux **ESP8266** qui crée une liaison série transparente à **300 bauds** entre deux équipements. Le but est de se rapprocher du comportement d'un ancien modem 300 bauds, mais sur une liaison radio **2,4 GHz**, avec une robustesse améliorée grâce à la correction d'erreurs **FEC**.

Le système fonctionne avec :

- **RS232/TTL à 300 bauds, 8N1**
- **Aucun contrôle de flux** : pas de **XON/XOFF**, pas de **RTS/CTS**
- **Octets `0x11` et `0x13` totalement transparents**
- **Radio 2-FSK**
- **TDD (Time Division Duplex)** maître/esclave
- **Aucune retransmission radio (pas d'ARQ dans la REV7)**
- **Correction d'erreurs Reed–Solomon** + **CRC16/CRC32**

Le **Master** émet en permanence sa référence temporelle. Le **Slave** se synchronise automatiquement sur le Master et répond dans son créneau TDD.

---

## 2. Objectif fonctionnel

L'objectif principal est de fournir un **pont série radio transparent** pour :

- du texte interactif,
- des commandes série,
- des tests transparents en binaire,
- éventuellement une liaison **PPP over Serial** ou d'autres protocoles série.

Le système est orienté vers la **simplicité de transport** et la **robustesse physique**, plus que vers un débit élevé.

---

## 3. Architecture générale

### 3.1 Rôles

Le système utilise deux firmwares proches :

- **Master A**
- **Slave B**

Le Master :

- génère l'horloge TDD,
- émet ses balises de synchronisation,
- envoie ses trames dans le premier demi-cycle,
- écoute le Slave dans le second demi-cycle.

Le Slave :

- cherche la synchronisation du Master,
- se verrouille sur le cycle TDD,
- émet dans son propre demi-cycle,
- conserve l'alignement tant que les trames valides continuent d'arriver.

### 3.2 Liaison série

Le côté série travaille en :

- **300 bauds**
- **8 bits de données**
- **pas de parité**
- **1 bit d'arrêt**
- **aucun contrôle de flux**

Cela donne une capacité maximale de :

```text
300 bit/s / 10 bits par caractère = 30 octets/s
```

Donc le flux série physique ne peut pas dépasser **30 octets/s** par direction.

---

## 4. Paramètres radio REV7

### 4.1 Paramètres principaux

La REV7 TDD300 utilise les paramètres suivants :

- **Modulation** : 2-FSK
- **Débit symbole radio** : **3840 symboles/s**
- **Échantillonnage** : **30720 échantillons/s**
- **8 échantillons par symbole**
- **CPU ESP8266** : **160 MHz recommandé**

Comparaison très importante :

- **RS232** : 300 bauds
- **Radio brute** : 3840 bauds

La radio est donc **12,8 fois plus rapide** en débit brut que le port série. Cette marge sert à transporter :

- la synchronisation,
- les CRC,
- la correction d'erreurs FEC,
- le calendrier TDD,
- la garde et le traitement.

---

## 5. TDD — calendrier temporel

### 5.1 Principe

La REV7 fonctionne en **TDD 300 ms** :

- **0 à 150 ms** : émission du **Master A** vers le Slave B
- **150 à 300 ms** : émission du **Slave B** vers le Master A

Puis le cycle recommence.

### 5.2 Trames par cycle

Dans la REV7 :

- **1 trame radio par sens et par cycle**
- donc **1 trame A→B toutes les 300 ms**
- et **1 trame B→A toutes les 300 ms**

### 5.3 Débit utile maximal

Chaque trame transporte jusqu'à **10 octets utiles**.

Donc la capacité théorique utile par direction est :

```text
10 octets / 0,300 s = 33,33 octets/s
```

C'est légèrement supérieur aux **30 octets/s** de la liaison série.

### 5.4 Marge disponible

La marge théorique entre la capacité RF utile et la capacité série est :

```text
33,33 - 30 = 3,33 octets/s
```

Cette marge est **faible**. Elle sert à absorber les petits retards et à garder le système viable à 300 bauds. En revanche, elle est trop petite pour intégrer confortablement des retransmissions fréquentes sans revoir le format radio.

---

## 6. Structure d'une trame REV7

La trame REV7 conserve le format radio robuste introduit en REV6.

### 6.1 Taille radio

Une trame occupe :

- **344 symboles radio au total**
- à **3840 symboles/s**
- soit environ **89,6 ms** de temps radio utile

Avec les gardes et la gestion interne, on est proche de **92,7 ms** par émission radio.

### 6.2 Découpage logique

La trame peut être résumée ainsi :

1. **Préambule / synchronisation**
   - **24 symboles**
2. **Bloc de contrôle 2-FSK**
   - **208 symboles**
   - contient les champs de contrôle
   - protégé par **CRC16**
3. **Bloc données / protection**
   - **112 symboles**
   - contient :
     - **10 octets utiles**
     - **CRC32** des données
     - **parité Reed–Solomon RS(18,14)**

### 6.3 Contrôle et données

Le code source indique :

- **24 octets de contrôle**
- suivis d'un **CRC16** pour sécuriser l'en-tête,
- **10 octets de charge utile**,
- **CRC32** pour valider les données,
- **FEC Reed–Solomon raccourci RS(18,14)**, capable de corriger jusqu'à **2 octets erronés** dans son bloc protégé.

---

## 7. Stratégie de robustesse

### 7.1 Ce que fait la REV7

La REV7 combine :

- **synchronisation 2-FSK robuste**,
- **CRC16** pour valider l'en-tête,
- **CRC32** pour valider les données,
- **Reed–Solomon RS(18,14)** pour corriger les erreurs d'octets,
- **verrouillage TDD** côté Slave,
- **mode transparent** côté série.

### 7.2 Ce que la REV7 ne fait pas

La REV7 **n'implémente pas de retransmission automatique**.

Donc :

- si le FEC corrige la trame : les données passent,
- si le CRC32 reste invalide : la trame est **rejetée**,
- les octets de cette trame sont **perdus définitivement**.

C'est donc un modem **sans ARQ**, au sens modem radio.

---

## 8. Comportement par rapport à un modem 300 bauds classique

Le comportement général ressemble à celui d'un ancien modem 300 bauds :

- débit série faible,
- pas de retransmission au niveau du modem,
- transport transparent des octets,
- possibilité d'utiliser des protocoles supérieurs comme PPP/TCP.

Mais il existe des différences importantes :

| Point | Modem téléphonique 300 bauds | AirModem REV7 |
|---|---|---|
| Support | Ligne téléphonique | Radio 2,4 GHz |
| Duplex | Full-duplex | Half-duplex TDD |
| FEC | Généralement absent | Reed–Solomon |
| CRC interne | Pas au niveau flux brut | Oui |
| Retransmission | Non | Non |
| Gestion d'erreur | un caractère peut être faux | une trame peut être rejetée |

Donc l'AirModem REV7 ne livre pas volontairement des données douteuses : il préfère les **rejeter**.

---

## 9. Benchmarks et validation

Un outil **WebSerial** a été préparé pour tester le modem en mode transparent.

### 9.1 Fonctions du benchmark

Le banc d'essai HTML permet :

- de connecter les ports A et B,
- de faire des tests **A→B**, **B→A** et bidirectionnels,
- de vérifier les **octets 0x11/0x13** en transparence,
- de lancer un **flux brut** proche de 30 octets/s,
- de réaliser des **transferts de fichiers**,
- de mesurer pertes, corruption, latence et CRC.

### 9.2 Résultats observés

Les essais montrent :

- certains petits fichiers passent avec **CRC identique**,
- des fichiers plus longs peuvent perdre des octets,
- sur des essais de 793 octets, des pertes de l'ordre de **2 à 3 %** ont été observées,
- ce comportement est cohérent avec une liaison **sans retransmission**.

Autrement dit :

- le modem est **fonctionnel**,
- la robustesse RF est correcte,
- mais l'absence d'ARQ empêche de garantir un transfert de fichier parfait.

---

## 10. Buffers série

Le projet utilise des buffers pour la partie série et la partie radio. D'après les constantes du code :

- **BUFFER_BYTES = 2048**
- buffers TX/RX de taille significative,
- capacité utile RF légèrement supérieure au débit série.

En théorie, les buffers **ne devraient pas se remplir** en régime nominal si la radio reste proche de sa capacité prévue et si les trames sont correctement reçues.

Mais en pratique :

- une perte de trame RF enlève immédiatement des octets utiles,
- une série de pertes peut réduire temporairement le débit effectif sous 30 octets/s,
- sans contrôle de flux, le système doit absorber cette différence avec ses buffers,
- si la situation dure, les buffers peuvent finir par être stressés.

---

## 11. Pourquoi les fichiers perdent des données

La cause principale est simple :

1. le navigateur envoie un flux brut,
2. le modem le convertit en trames radio de **10 octets utiles**,
3. certaines trames RF deviennent irrécupérables,
4. la REV7 les rejette,
5. le fichier reçu présente des manques ou une divergence CRC.

Sans retransmission, même un faible taux de trames perdues finit par endommager un transfert de fichier.

---

## 12. Pistes d'amélioration

### 12.1 ARQ sélectif

La première amélioration logique serait d'ajouter :

- **ACK sélectifs**,
- **numéros de séquence**,
- **retransmission sélective** des blocs perdus.

Cela permettrait de rendre les fichiers fiables.

### 12.2 FEC renforcé

La seconde piste est de renforcer encore le FEC, en réutilisant le temps radio disponible, sans réduire la charge utile de 10 octets si possible.

### 12.3 TDD plus court

Une autre piste consiste à réduire le cycle TDD pour améliorer la réactivité interactive :

- moins d'attente avant émission,
- meilleure sensation de « temps réel » au clavier,
- mais contrainte plus forte sur le CPU et les gardes radio.

### 12.4 Séparation diagnostics / données

Pendant les essais, les diagnostics ont parfois partagé le même port série 300 bauds, ce qui gêne fortement les mesures. En mode final, les diagnostics doivent être :

- désactivés,
- ou séparés du port de données.

---

## 13. Conclusion

Le projet **AirModem REV7** constitue un **modem radio 300 bauds transparent**, robuste et fonctionnel, reposant sur :

- **ESP8266**,
- **2-FSK**,
- **TDD maître/esclave**,
- **Reed–Solomon RS(18,14)**,
- **CRC16/CRC32**,
- **aucun contrôle de flux série**,
- **aucune retransmission radio**.

Les principaux points à retenir sont :

1. **La liaison série à 300 bauds fonctionne.**
2. **La radio fonctionne à 3840 symboles/s.**
3. **Le débit utile RF est de 33,33 octets/s par sens.**
4. **La charge utile par trame est de 10 octets.**
5. **La REV7 reste sans retransmission, donc les fichiers peuvent perdre des octets.**
6. **Pour des transferts fiables, il faut ajouter soit un ARQ, soit un protocole de retransmission au-dessus du modem.**

En l'état, le modem est très intéressant comme **pont série radio transparent**, comme **équivalent moderne d'un modem 300 bauds sans retry**, ou comme support expérimental pour du **PPP**, du **texte interactif** et des essais de robustesse radio.

