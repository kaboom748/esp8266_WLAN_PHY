# Profil Windows transparent 0.3

Fichier : airmodem-transparent.inf
Modele : AirModem4800 Transparent 0.3 - 4800 bps XON-XOFF
Identifiant : AIRMODEM4800_TRANSPARENT_V1

Ce profil remplace le profil AT pour les firmwares 0.3.0. Il utilise le moteur
Unimodem deja fourni par Windows et le pilote USB-serie deja installe.
Aucun .sys n'est fourni. Aucun peripherique Windows n'a ete installe automatiquement.

## Installation manuelle

1. Reprendre l'ESP dans VirtualHere sur le PC concerne et verifier son numero COM.
2. Fermer PuTTY, les moniteurs serie et les connexions utilisant ce COM.
3. Ouvrir `control telephon.cpl`, onglet Modems, Ajouter, selection manuelle
   sans detection automatique, puis Disque fourni et choisir cet INF.
4. Choisir le modele Transparent 0.3 et le COM exact de cet ESP.
5. Cote client, selectionner ce nouveau peripherique dans les proprietes de la
   connexion d'acces a distance. Cote serveur, le selectionner dans Connexions
   entrantes, a la place de l'ancien modele AT. Ne pas laisser les deux profils
   essayer d'utiliser le meme COM.
6. Verifier 4800 bit/s et ne pas activer le controle de flux materiel. Le profil
   annonce uniquement le controle logiciel XON/XOFF, sans compression modem.

Le profil est **NON SIGNE**, sans catalogue .cat. La validation du fichier par
SetupAPI n'est pas une signature et ne garantit pas son installation sur toutes
les politiques Windows. L'absence de nouveau .sys ne garantit pas non plus
l'acceptation du paquet. Si Windows le refuse, relever l'erreur d'installation ;
ne pas desactiver globalement les protections de signature pour ce fichier.
Une distribution signee est un travail distinct.

## Ce que le profil demande

- DCB 4800, 8N1, XON=0x11, XOFF=0x13.
- fOutX et fInX actifs, fTXContinueOnXoff actif pour garder les directions independantes.
- Pas de controle CTS/DSR, DTR ou RTS de flux.
- DeviceType=0, cable serie direct ; debit maximum DTE et DCE 4800.
- Initialisation et rearmement d'ecoute : envoi du seul XON, sans reponse attendue.
- Appel : CLIENT. Reception de CLIENT : evenement d'appel entrant.
- Reponse par le PC serveur : CLIENTSERVER. Le firmware ne fabrique aucune reponse.
- Aucune commande AT, aucun +++, aucun message CONNECT emis par les ESP.

La sequence de rearmement par XON est une precaution pour un firmware qui respecte
une pause jusqu'a reception d'un vrai XON. Son execution effective par Unimodem
et l'ecoute entrante apres plusieurs deconnexions restent a verifier sur le PC cible.

## Validation

```powershell
powershell.exe -NoProfile -ExecutionPolicy Bypass -File .\Test-Inf.ps1
```

Ce script est en lecture seule : pas d'installation, de modification de registre
ou d'ouverture du COM. Il utilise le parseur SetupAPI et BuildCommDCBW de Windows.
77 assertions ont reussi. Le journal precise NativeRasTested=false.

Pour la validation native : verifier au moniteur la vitesse reelle 4800, le XON
initial, CLIENT puis CLIENTSERVER venant du PC distant, et la negociation PPP.
L'ACCM PPP doit preserver les octets 0x11 et 0x13 dans **les deux sens**.
Fermer le moniteur s'il prend le port en exclusivite avant de lancer RAS.

La fonction Interroger le modem peut envoyer des commandes AT de diagnostic ;
ce firmware n'y repond plus. Ce n'est pas un test approprie du cable transparent.

L'ouverture du COM a provoque un reset des deux cartes dans les essais Linux via
VirtualHere, avec message ROM a 74880 bauds. Les options DTR/RTS des logiciels ne
prouvent pas l'absence d'impulsion du pilote a l'ouverture. Le comportement Windows
et un eventuel blocage de RasMan ne sont pas valides par les tests Linux.

Reference DCB : https://learn.microsoft.com/en-us/windows/win32/api/winbase/ns-winbase-dcb
Reference PPP : https://www.rfc-editor.org/rfc/rfc1662.html#section-4.2
