# Pilotage de la Vita depuis le PC (versions de test)

Prérequis : vitacompanion (devnoname120, via AutoPlugin II) sur la Vita, à
192.168.1.21 (FTP 1337, commandes 1338). Ne pas activer le FTP de VitaShell.

- `vc.py "<commandes>"` : commandes vitacompanion (`destroy`, `launch BRNG00001`,
  `press cross; wait 200ms; release cross`, `screen on`, `nosleep on`).
  Gâchettes : `l2` / `r2` (`l` / `r` ne passent pas au jeu).
- `deploy.sh` : ferme le jeu, remplace seulement `ux0:app/BRNG00001/eboot.bin`
  par `out/vita/.../eboot_gl.bin`, relance.
- `go_race.sh` : du lancement à World Tour > Compact > Interstate Loop >
  Course (Compact Type 1), puis accélère `DRIVE_S` secondes (défaut 90).
  Réglé sur la sauvegarde de l'utilisateur (accélérateur = R).
- `step.sh "<commandes>" ATTENTE NOM` : commandes, attente, capture d'écran
  (versions PERF : `ux0:data/burnout/shot.txt` → `ux0:data/burnout-frames`).

Les versions PERF gardent l'écran allumé (`sceKernelPowerTick` DEFAULT) :
les appuis simulés ne comptent pas comme activité.
