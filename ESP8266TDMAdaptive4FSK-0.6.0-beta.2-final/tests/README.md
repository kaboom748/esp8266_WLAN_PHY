# Controles Logiciels

La beta.2 ajoute les regressions d'adaptation de la 16-FSK beta.4, avec
les durees de trame et de pilote 4-FSK conservees : 54 replis adjacents,
18 sequences de 300 secondes simulees de reessais repetes, descente en
escalier lors d'une panne totale, conservation des buffers et de la
calibration, et retour explicite a la base par pause/run.
Les tests de controleur et TDM utilisent AddressSanitizer/UndefinedBehaviorSanitizer.

run_host_tests.sh execute les suites historiques (codec, acquittements,
controle de debit, calcul de phase, TDM et journal) et fsk4_test.cpp.

La suite 4-FSK utilise les memes Fsk4.h et FskFrame.h que le firmware.
Elle couvre le mapping Gray, les deux orientations de frequence, les
longueurs 0..32, les erreurs binaires simples, les trames tronquees,
le rejet CRC, la limite de corrections, la calibration par groupes,
les frequences ambigues, et le vote temporel sur donnees synthetiques
pour les huit periodes de symbole. AddressSanitizer et UBSan sont actifs.

Le modele TDM utilise la duree 4-FSK et controle l'ordre des messages,
les acquittements, le backpressure, les pertes, les redemarrages, les
changements de debit et l'absence de recouvrement TX dans le modele.

Aucune suite n'utilise un port serie ou les ESP reels.
Aucune de ces suites ne constitue une validation RF ou une mesure du CPU.
