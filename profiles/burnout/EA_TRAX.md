# EA Trax : notes pour les musiques ajoutées (2026-11-14)

But : un dossier de musiques du joueur dont les morceaux **s'ajoutent** à la
liste EA Trax (menu Détails pilote > Paramètres > EA GAMES TRAX), avec titre,
artiste et album, sans toucher à la sauvegarde du jeu.

## Ce qui est établi (PC, ULES00125)

- **Fichiers** : `USRDIR/music/EATraxNN.mus` (ATRAC3plus), 21 fichiers
  (00-06, 08, 09, 12-23 ; 07, 10, 11 absents). Le chemin est fabriqué par
  0x089B0160 : `sprintf(buf, "%s%s%02i%s", "music/", "EATrax", n, ".mus")`
  (pointeurs des trois morceaux à 0x08B97E54/58/5C).
- **Table des morceaux** : 0x08B97E60, 21 fiches de 24 octets
  `{fichier n, 0, texte titre, texte album, texte artiste, réglage}`
  (ex. fiche 0 : fichier 0, 0xC01 INK, 0xBA9 SAY HELLO TO SUNSHINE, 0xBD5 FINCH, 0xF).
- **Textes** : 44 titres (0xC01-0xC2C environ), 44 albums (0xBAB-), 44
  artistes (0xBD7-) hérités de Burnout 3 ; 23 inutilisés dont
  MUSICTRACK07/10/11, MUSICALBUM07/10/11, MUSICARTIST07/10/11
  (ids 0xC03/0xC04/0xC2A, 0xBAB/0xBAC/0xBD2, 0xBD7/0xBD8/0xBFE).
- **Modèle du lecteur** : à 0x08B9805C le nombre de morceaux (21), à
  0x08B980A4 l'adresse de la table. Le jeu le recopie dans chaque lecteur
  (objet +0x6C nombre, +0xB4 table) **au démarrage** : le changer dans le
  modèle avant le lancement (copie de l'ELF, ou juste après
  `load_and_relocate` dans burnout_run.cpp) suffit pour que le lecteur
  ET le menu prennent la nouvelle table (essai : 23 lignes dans le menu,
  23 numéros mélangés par le lecteur).
- **Lecteur** (objet en jeu à 0x08DD17C8) : +0x0D liste mélangée (octets,
  place pour ~50), +0x40 position, +0x44 nombre mélangé, +0xD8 fichier en
  cours. Mélange : 0x089AFFF8 ; suivant : 0x089B00CC.
- **Réglages par morceau** (PARTOUT / ... / NON) : 21 octets à 0x08C041E0
  (= 0x08BFCDE0 + 0x7400 + i), suivis d'autres réglages du jeu (volumes…).
  Le bloc de 42 octets à +0x7400 est recopié tel quel par la sauvegarde
  (0x08812650, 0x0888D58C, 0x0888D79C, 0x08896C24, 0x08897050 : boucles de 21
  paires). 0x089AF694 recopie les 21 réglages dans les fiches (+0x14).
  **Danger** : avec plus de 21 morceaux, le menu lit et écrirait les réglages
  des lignes 22+ dans les octets suivants (essai : lignes 22-23 affichées
  « NON »). Il faut rediriger ces accès vers notre propre fichier.
- Le menu lit les textes des fiches **au démarrage** : notre table doit être
  remplie avant (au chargement), et les textes des places libres réécrits
  dès que la table des textes du jeu est chargée.

## Fait (2026-11-14, PC vérifié)

- `host/trax.cpp` : dossier `musique_perso` (Vita : ux0:data/burnout/musique_perso,
  PC : à côté du jeu, testeur : `--custom-music DIR`), MP3 triés par nom, 23 au
  plus ; titre/artiste/album lus dans les balises ID3 (v2.3/2.4 puis v1),
  sinon « Artiste - Titre.mp3 », album par défaut « MUSIQUE PERSO » ; table
  agrandie en 0x08110000 et modèle redirigé juste après le chargement ;
  textes écrits sur les 23 textes libres dès que la table des textes existe ;
  réglages des morceaux ajoutés dans `musique_perso.txt` (nom du fichier, réglage).
- `tools/patch_trax.py` : quatre accès aux réglages redirigés (commit du menu
  0x088C6D44, chargement du menu 0x088C6DF4, morceau en cours
  0x089B0370/0x089B0380, application de tous les réglages 0x089B2E10, qui
  reçoit l'adresse des réglages en paramètre).
- `burnout_hle.cpp` : `EATrax24.mus`… listés dans le dossier music (Dopen,
  Getstat) et ouverts comme une copie d'EATrax22.mus dont la taille RIFF
  (impaire, jamais celle d'un vrai fichier) désigne le MP3 ; lectures de ces
  fichiers toujours synchrones (les octets 4-7 réécrits).
- `music.cpp` : décodage MP3 (minimp3, CC0, third_party/minimp3), rééchantillonné
  au plus proche en 44,1 kHz stéréo.
- Vérifié sur PC : lignes 22-23 dans le menu avec leurs textes, réglage PARTOUT
  et modifiable, aperçu joué (bandeau EA Trax du morceau, tonalité du MP3 de test).

## Reste à faire

1. Vita : vérifier en jeu, mesurer le coût du décodage MP3 sur le cœur 0
   (DecodeData, fil d'émulation) ; s'il compte, décoder d'avance sur un autre fil.
2. En course : vérifier que les morceaux ajoutés passent dans la rotation.
3. Lettres accentuées : remplacées par la lettre simple (police du jeu).

Outils : `profiles/burnout/scripts/driver_details_menu.txt` (ouvre le menu
EA Trax), `--poke`, `--dump-ram`, `dump_function.exe`.
