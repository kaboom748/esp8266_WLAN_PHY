# Serie-IP — deux PC en IPv4 sur liaison série

## Ce qui est fourni

`Serie-IP.ps1` est un script autonome avec du C# intégré. Il utilise une DLL
**Wintun native**, et non une DLL .NET ou COM. Aucun projet Visual Studio ni
compilateur à installer séparément : `Add-Type` compile le moteur intégré.

Cible de cette version : **Windows PowerShell 5.1 Desktop x64**, sur Windows
x64 Intel/AMD. Utiliser `powershell.exe`, pas `pwsh.exe`/PowerShell 7. Les deux
PC exécutent exactement le même fichier, avec des rôles A et B inversés.

C'est un prototype à valider sur le matériel. Ni l'exécution Windows/Wintun ni
la communication avec deux ports série physiques n'ont pu être testées dans
l'environnement de création. Le mode `-SelfTest` permet de tester le codec
sur la machine cible, indépendamment du pilote et du matériel.

## 1. Disposition des fichiers

Sur **chacun** des deux PC, dans un dossier local :

```text
C:\SerieIP\
  Serie-IP.ps1
  wintun.dll       <-- DLL AMD64 provenant de l'archive officielle Wintun
  LIRE-MOI.md      <-- facultatif pour l'exécution
```

La DLL n'est pas incluse dans cette livraison. Utiliser la DLL signée officielle :
https://www.wintun.net/

Le script utilise son propre dossier (`$PSScriptRoot`), même lorsqu'il est lancé
à partir d'un autre répertoire. Il vérifie le format PE AMD64 et la signature
Authenticode avant le chargement. Si la signature ne peut pas être validée,
il s'arrête : vérifier la provenance, la copie du fichier, l'horloge et les
certificats Windows. Une machine isolée peut manquer des éléments de confiance
nécessaires à cette vérification. Le script ne contourne pas ce contrôle.

## 2. Préparer les deux extrémités

La liaison série doit déjà fonctionner entre les deux PC, dans les deux sens,
sans conversion de texte ni écho local par un équipement intermédiaire. Les
ports COM réels peuvent avoir des noms différents. Fermer les terminaux série
et les autres logiciels qui utilisent ces ports.

Ouvrir **Windows PowerShell en tant qu'administrateur**, et non « Windows
PowerShell (x86) ». Se placer dans le dossier du script :

```powershell
Set-Location C:\SerieIP
$PSVersionTable
[Environment]::Is64BitProcess
.\Serie-IP.ps1 -ListPorts
.\Serie-IP.ps1 -SelfTest
```

Les deux derniers modes ne demandent pas de DLL ni de droits administrateur.
`-SelfTest` doit retourner `OK`, sinon ne pas lancer la liaison.
Il vérifie le vecteur de référence du CRC, toutes les valeurs d'octet,
1 000 trames pseudoaléatoires, le découpage des lectures, les contrôles XON/XOFF,
les CRC erronés, les débordements et la resynchronisation. Il ne teste pas
l'installation de Wintun ou le câble.

**Politique d'exécution :** le script ne modifie aucune politique PowerShell.
Lire le code avant de l'exécuter. Un fichier téléchargé peut nécessiter un
`Unblock-File -LiteralPath .\Serie-IP.ps1` après cette vérification. Cela ne
remplace pas une politique de signature imposée. Sur une machine administrée,
respecter sa politique et faire signer/autoriser le fichier si nécessaire.

## 3. Démarrer

### PC A

```powershell
.\Serie-IP.ps1 -Role A -Port COM3 -AllowPing
```

Adresse locale : `10.77.0.1`. Adresse du pair : `10.77.0.2`.

### PC B

```powershell
.\Serie-IP.ps1 -Role B -Port COM4 -AllowPing
```

Adresse locale : `10.77.0.2`. Adresse du pair : `10.77.0.1`.

Remplacer COM3 et COM4 par les ports réellement raccordés. Les paramètres
par défaut sont **4 800 bauds, 8 bits, sans parité, 1 bit d'arrêt, XON/XOFF**.
DTR et RTS sont maintenus actifs, mais RTS/CTS n'est pas utilisé comme contrôle
de flux. `-NoDtr` et `-NoRts` permettent de ne pas activer ces sorties lorsque
le matériel le demande.

Les deux scripts doivent rester en cours d'exécution. « Passerelle locale
active » signifie que l'extrémité locale a démarré ; il n'existe pas de
négociation ou de confirmation automatique de présence du PC distant.

### Choisir d'autres adresses

Utiliser des adresses privées non déjà utilisées dans les réseaux des deux PC :

```powershell
# PC A
.\Serie-IP.ps1 -Role A -Port COM3 -LocalIp 10.88.10.1 -PeerIp 10.88.10.2 -AllowPing
# PC B
.\Serie-IP.ps1 -Role B -Port COM4 -LocalIp 10.88.10.2 -PeerIp 10.88.10.1 -AllowPing
```

Le script configure volontairement l'adresse locale en **/32** et ajoute
une seule route **/32 vers le pair**, directement sur l'adaptateur Wintun.
Ce choix remplace le /30 envisagé dans l'explication précédente pour ne router
que l'autre PC. Il ne configure aucune passerelle par défaut ni serveur DNS.
Il refuse une adresse déjà locale ou une route /32 existante vers le pair ;
il n'exclut pas tous les conflits possibles avec des réseaux plus larges.

## 4. Tester depuis une autre console

Pendant que la passerelle tourne sur les deux PC :

```powershell
# Sur A :
ping -4 -n 4 -l 32 -w 5000 10.77.0.2

# Sur B :
ping -4 -n 4 -l 32 -w 5000 10.77.0.1
```

Diagnostic local :

```powershell
Get-NetAdapter -Name SerieIP
Get-NetIPAddress -InterfaceAlias SerieIP -AddressFamily IPv4
Get-NetRoute -InterfaceAlias SerieIP -AddressFamily IPv4
Get-NetIPInterface -InterfaceAlias SerieIP -AddressFamily IPv4
```

`-AllowPing` crée seulement une règle entrante ICMPv4 Echo Request, limitée à
cet adaptateur, à l'IP locale et à l'IP du pair, pour les profils Windows
concernés. Le pare-feu reste actif. Une stratégie de blocage prioritaire peut
empêcher le ping malgré cette règle. Pour une autre application, autoriser
uniquement son trafic nécessaire selon la politique locale ; le script
n'ouvre pas tous les ports TCP/UDP.

La liaison transporte des paquets IPv4 ; elle ne fournit pas un service
applicatif à elle seule. Une application TCP/UDP doit être en écoute sur le
PC distant et utiliser les adresses du tunnel, avec des délais adaptés.

## 5. Arrêter et nettoyer

Appuyer sur **Q** dans la console de la passerelle, ou utiliser **Ctrl+C**.
À l'arrêt normal, y compris une erreur gérée, le script tente de :

- arrêter les échanges et fermer COM ;
- terminer la session Wintun, puis supprimer l'adaptateur qu'il a créé ;
- retirer uniquement la règle de ping créée par cette exécution.

Le paquet du pilote Wintun peut rester installé. Le script n'appelle jamais
`WintunDeleteDriver`, ne réutilise aucun adaptateur préexistant et ne modifie
pas les autres cartes.

Un arrêt forcé du processus ou une panne de Windows ne garantit pas le
nettoyage. La règle de ping est créée dans `PersistentStore` puis supprimée
à l'arrêt normal ; en cas d'arrêt forcé, elle peut subsister. Son nom unique
est affiché au lancement. Examiner les règles du script avec :

```powershell
Get-NetFirewallRule -Name 'SerieIP-Ping-*' |
    Select-Object Name, DisplayName, Enabled
```

Après avoir identifié une règle résiduelle et arrêté l'instance concernée :

```powershell
Remove-NetFirewallRule -Name 'NOM-EXACT-AFFICHE-AU-LANCEMENT' -PolicyStore PersistentStore
```

Si un adaptateur `SerieIP` subsiste, l'instance suivante refuse de le modifier.
Examiner son origine dans Windows, ou utiliser un autre nom dédié :

```powershell
.\Serie-IP.ps1 -Role A -Port COM3 -AdapterName SerieIP2 -AllowPing
```

La fermeture peut dépendre du délai d'écriture si le pilote série reste
bloqué. Le script ne libère pas la session ou la DLL tant qu'un thread les
utilise : il signale une fermeture incomplète plutôt que de risquer un accès
à de la mémoire libérée. Utiliser une console dédiée à la passerelle.

## 6. Fonctionnement et format filaire

```text
Windows IPv4 -> Wintun -> moteur C# intégré -> série
série -> moteur C# intégré -> Wintun -> Windows IPv4
```

Trois threads C# distincts lisent Wintun, écrivent sur la série et lisent la
série. Aucun callback de thread secondaire n'appelle un scriptblock PowerShell.
Les pointeurs de réception Wintun sont copiés et immédiatement libérés, sans
attendre la transmission série.

Format propriétaire **version 1**, identique sur les deux PC :

```text
7E | ECHAPPER(01 | 01 | longueur_BE16 | paquet_IPv4 | CRC_BE16) | 7E
```

Le premier octet indique la version, le deuxième le type IPv4. La longueur
compte seulement les octets du paquet IP. Le CRC couvre version, type,
longueur et paquet, avant l'échappement. Le CRC est **CRC-16/CCITT-FALSE** :
polynôme `0x1021`, initialisation `0xFFFF`, aucune réflexion, XOR final `0`.
Le résultat est transmis octet fort en premier. Vecteur `123456789` : `0x29B1`.

Tous les octets `00..1F`, `7D`, `7E`, `91` et `93` sont encodés par `7D` suivi
de l'octet XOR `20`. Cela protège notamment les valeurs de données `11` et
`13` de XON/XOFF, y compris lorsqu'elles apparaissent dans l'en-tête ou le CRC.
Le pilote série reste responsable des vrais caractères de contrôle de flux.

Le décodeur conserve les trames incomplètes entre les lectures et les pauses,
rejette les CRC/longueurs invalides et se resynchronise au délimiteur suivant.
Une taille maximale borne sa mémoire. Il ne faut pas connecter cette version
à une extrémité PPP, SLIP ou un autre protocole : ce cadrage n'en implémente
ni l'en-tête complet ni la négociation.

## 7. Réglages et limites

| Paramètre | Défaut | Rôle |
|---|---:|---|
| `-BaudRate` | 4800 | Débit série, identique des deux côtés |
| `-Mtu` | 576 | MTU IPv4, identique des deux côtés, 576 à 1500 |
| `-QueuePackets` | 4 | Trames IP en attente côté utilisateur, 1 à 64 |
| `-WriteTimeoutSeconds` | 30 | Délai maximal d'une écriture ou de l'attente de vidage |
| `-DurationSeconds` | 0 | Arrêt automatique après cette durée ; 0 = jusqu'à arrêt manuel |
| `-AdapterName` | SerieIP | Nom dédié, ne doit pas préexister |
| `-NoDtr`, `-NoRts` | absents | Désactiver l'activation permanente de ces sorties |
| `-AllowPing` | absent | Ajouter la règle ICMP ciblée puis la retirer |

Avec 4 800 bit/s et 8N1 : `4800 / 10 = 480 octets/s` au maximum sur la ligne,
avant cadrage, échappement et pauses. Un paquet IP de 576 octets demande au
moins 1,2 seconde, puis s'ajoutent les surcoûts. Une file de quatre paquets
pleins représente déjà plusieurs secondes. Ne pas gonfler les files pour
masquer une saturation. Le moteur attend également que la file de sortie
signalée par le pilote série se vide entre deux trames ; certains matériels
ont leurs propres buffers non visibles du logiciel.

Cette version transporte seulement **IPv4 unicast entre les deux adresses
configurées**. Les autres sources/destinations, IPv6 et les tailles excessives
sont rejetées. Elle ne transporte pas Ethernet/MAC, ARP, les broadcasts ou
le multicast et n'offre pas de pont LAN, de partage Internet ou de routage
vers un troisième réseau. Elle ne désactive pas IPv6 globalement dans Windows.

Il n'y a **ni chiffrement ni authentification du pair**. Le CRC détecte des
erreurs accidentelles, pas une modification malveillante. Réserver le prototype
à une liaison de confiance et ne pas lui confier de données sensibles sans
protection applicative appropriée.

Pas d'accusé de réception ni de retransmission au niveau série. Une trame
corrompue, une file saturée ou un ring Wintun plein entraîne une perte ; TCP
peut gérer ses retransmissions, une application UDP doit gérer ses propres
besoins de fiabilité. En cas de timeout d'écriture, l'extrémité s'arrête sans
réémettre aveuglément une trame possiblement partielle. Redémarrer après avoir
corrigé le problème. Pas de reconnexion automatique dans cette version.

### Lire les statistiques

`TX/RX` comptent les paquets et octets IP transmis localement vers la série ou
injectés dans Windows. TX ne constitue pas un accusé de réception distant.
`serie TX/RX` compte les octets de cadrage remis/reçus par le logiciel, hors
caractères de contrôle gérés directement par le pilote ; ce n'est pas une
mesure électrique du fil.

`pertes file` indique la saturation de la file bornée ; `ring` indique que
Wintun ne pouvait pas accepter un paquet entrant. `trames invalides` compte
les erreurs de cadrage, taille ou CRC. `filtres TX/RX` compte les paquets ne
correspondant pas au contrat IP, y compris du bruit IPv6 local qui n'est pas
transmis sur le fil.

Si TX augmente sur A mais RX reste nul sur B, examiner COM, la liaison et les
statistiques série/CRC de B. Si RX augmente sur B sans réponse, examiner l'IP,
le pare-feu, le service destinataire et le chemin de retour.

## Contrôles réalisés lors de la création

Une transcription indépendante du codec en Python a passé 1 000 allers-retours
pseudoaléatoires, 305 positions de découpage d'une trame contenant toutes les
valeurs d'octet, les cas de CRC faux/débordement/échappement incomplet, puis
1 000 essais de bruit suivis d'une resynchronisation. Le CRC a également été
comparé à une fonction de référence indépendante. Les délimiteurs lexicaux du
C# et l'inclusion exacte de ce source dans le PS1 ont été contrôlés.

**Ces contrôles ne sont ni une compilation C# ni une exécution PowerShell.**
Restent à vérifier sur Windows : la compilation `Add-Type`, les appels Wintun,
la configuration réseau, le fonctionnement COM et les échanges entre les PC.

## Références primaires utilisées

Wintun, distribution et documentation : https://www.wintun.net/

En-tête API officiel (signatures, propriété des buffers et durée de vie) :
https://git.zx2c4.com/wintun/tree/api/wintun.h

Chargement natif par chemin explicite :
https://learn.microsoft.com/en-us/windows/win32/api/libloaderapi/nf-libloaderapi-loadlibraryexw

Échappement asynchrone à l'origine de ce mécanisme (ce script n'est pas PPP) :
https://www.rfc-editor.org/rfc/rfc1662

Configuration de l'interface et MTU :
https://learn.microsoft.com/en-us/powershell/module/nettcpip/set-netipinterface

Adresses et routes :
https://learn.microsoft.com/en-us/powershell/module/nettcpip/new-netipaddress
https://learn.microsoft.com/en-us/powershell/module/nettcpip/new-netroute

Règle de pare-feu ciblée :
https://learn.microsoft.com/en-us/powershell/module/netsecurity/new-netfirewallrule

Délai d'écriture série :
https://learn.microsoft.com/en-us/dotnet/api/system.io.ports.serialport.writetimeout
