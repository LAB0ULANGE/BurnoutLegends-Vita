# Burnout Legends sur PS Vita (recompilation native)

Un portage natif de **Burnout Legends** (PSP) sur PS Vita, obtenu en recompilant
statiquement l'exécutable PSP du jeu en code ARM avec [PSPRecomp](docs/PSPRecomp.md).
Ce n'est pas un émulateur : le code du jeu tourne nativement, avec un rendu
(vitaGL), un son, des vidéos et des sauvegardes refaits pour la Vita.

*English version: [README.md](README.md)*

> **Il faut votre propre exemplaire du jeu.** Ce dépôt ne contient aucun fichier
> du jeu, aucun exécutable du disque et aucun code du jeu recompilé. Faites une
> copie de votre propre UMD.

## Fonctionnalités (0.0.1)

- Courses à 30 fps en 960x544 (la résolution dynamique tient la cadence sur les
  circuits les plus lourds ; les menus et la voiture du joueur restent en pleine
  résolution)
- Son, musiques et vidéos des menus
- Fenêtre de sauvegarde native de la Vita
- **Vos sauvegardes PSP / Adrenaline sont reprises directement** : celles déjà
  présentes sur votre carte mémoire (`ux0:pspemu`, `uma0:pspemu`, `imc0:pspemu`,
  `ur0:pspemu`, `xmc0:pspemu`) apparaissent avec la mention « - PSP ». Rien à
  copier ni à convertir, et une sauvegarde écrite par le portage se recharge dans
  Adrenaline. Une copie de vos sauvegardes Adrenaline est faite au premier
  lancement dans `ux0:data/burnout/backup_adrenaline/`.

## Version du jeu prise en charge

| | |
|---|---|
| Jeu | Burnout Legends, Europe |
| Disque | **ULES00125**, DISC_VERSION 1.01 |
| SHA-256 de l'`EBOOT.BIN` déchiffré | `89263a8f1f262971ad85761202fa6b02f4dfc86b3fbc733402ec20d0003e57c4` |

Les autres versions (US ULUS10025, autres révisions) ne sont pas encore prises en
charge : le code recompilé correspond à cet exécutable précis.

## Prérequis

- Une PS Vita / PS TV sous HENkaku / Ensō
- [VitaShell](https://github.com/TheOfficialFloW/VitaShell)
- `libshacccg.suprx` (le compilateur de shaders de la Vita) dans
  `ur0:data/external/`. Si vous ne l'avez pas, extrayez-le avec
  [ShaRKBR33D](https://github.com/Rinnegatamante/ShaRKBR33D) puis copiez-le en
  `ur0:data/external/libshacccg.suprx`.
- Votre propre UMD de Burnout Legends (ULES00125), copié en ISO
- Sur PC : [PPSSPP](https://www.ppsspp.org/) (pour déchiffrer l'exécutable du jeu)
  et Python 3 avec [PyAV](https://pypi.org/project/av/) (`pip install av`, plus
  `pip install audioop-lts` à partir de Python 3.13) pour convertir les musiques

## Installation

### 1. Préparer les fichiers du jeu sur PC

1. **Déchiffrer l'exécutable.** PPSSPP fermé, ouvrez
   `memstick/PSP/SYSTEM/ppsspp.ini` et mettez `DumpDecryptedEboots = True` dans
   la section `[General]`. Lancez le jeu une fois dans PPSSPP : l'exécutable
   déchiffré est écrit dans `memstick/PSP/SYSTEM/DUMP/ULES00125_Burnout.BIN`.
   Renommez-le `EBOOT_DECRYPTED.ELF`.
2. **Extraire le disque.** Ouvrez l'ISO avec 7-Zip et extrayez
   `PSP_GAME/PARAM.SFO` et tout le dossier `PSP_GAME/USRDIR`.
3. **Convertir les musiques** (la Vita ne sait pas décoder l'ATRAC3plus du jeu) :

   ```bash
   python profiles/burnout/tools/convert_music.py USRDIR/music music_ima
   ```

Vous obtenez :

```text
burnout/
  EBOOT_DECRYPTED.ELF
  PARAM.SFO
  USRDIR/
  music_ima/
```

### 2. Les copier sur la Vita

Copiez ce dossier `burnout` en `ux0:data/burnout/` (FTP ou USB de VitaShell).

### 3. Installer le VPK

Installez le VPK avec VitaShell et lancez **Burnout recomp GL** depuis la LiveArea.

Le premier lancement reste environ une minute sur le logo : les shaders sont
compilés pour votre console.

## Commandes

Celles de la PSP, sur les mêmes boutons. **START + SELECT** quitte le jeu.

## Problèmes connus

- De courts ralentissements peuvent arriver sur les plus grands circuits
  (Waterfront, Downtown) quand une nouvelle partie du circuit se charge.
- Environ une minute de chargement à chaque lancement (compilation des shaders).
- Seule la version ULES00125 v1.01 est prise en charge.

## Signaler un bug

Joignez le log : **`ux0:data/burnout-0.0.1.log`** (et `.log.prev`, la partie
précédente). Dites sur quel circuit et ce que vous faisiez. Le log note déjà la
mémoire utilisée et chaque image trop lente.

## Compiler soi-même

Voir [profiles/burnout/README.md](profiles/burnout/README.md) (en anglais). En
bref : déchiffrer l'exécutable, lancer le recompilateur dessus (le code généré
reste sur votre PC), puis compiler la cible Vita avec VitaSDK.

## Crédits et licences

- [PSPRecomp](https://github.com/jessicanataliagta/PSPRecomp) de Jessica Natalia
  et contributeurs (MIT) : le recompilateur et le runtime sur lesquels repose ce
  portage.
- [vitaGL](https://github.com/Rinnegatamante/vitaGL) de Rinnegatamante (OpenGL sur
  le GXM de la Vita), avec un petit patch (`profiles/burnout/tools/vitagl-*.patch`).
- Le chiffrement des sauvegardes PSP (`profiles/burnout/host/psp_savedata.cpp`)
  est porté de [PPSSPP](https://github.com/hrydgard/ppsspp) et est sous
  **GPL-2.0-or-later** ; une version Vita qui l'inclut est donc distribuée sous GPL.
- [stb_image](https://github.com/nothings/stb) (domaine public).
- Le reste du dépôt est sous licence MIT ([LICENSE](LICENSE)).

Burnout et Burnout Legends sont des marques d'Electronic Arts. Ce projet n'est
ni affilié à Electronic Arts ou Criterion Games, ni approuvé par eux.
