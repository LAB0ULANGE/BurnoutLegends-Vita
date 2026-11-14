# Journal de performance — Burnout Legends Vita, objectif 60 fps

Méthode : `BRIEF_PERF_60FPS.md` (F:\Burnout Legends). Une hypothèse = un
changement = un commit, mesuré sur la vraie Vita (même scène, même durée).
Gain < 0,3 ms ou régression visuelle : commit annulé.

## Étape 0 — Instrumentation (2026-11-12)

Module `profiles/burnout/host/perf_counters.{hpp,cpp}`, compilé seulement
avec `-DBURNOUT_PERF_COUNTERS=ON` (CMake ; `PERF_COUNTERS` sur les sources
hôte, le code généré n'est pas recompilé). Désactivé : les sites
`PERF_ONLY(...)` ne produisent aucun code.

Toutes les 600 images, une ligne par compteur dans `ux0:data/burnout-perf.log`
(PC : `burnout_perf.log`) : moyenne, maximum, 1 % pire (99e centile = 6e plus
grande valeur sur 600).

| Compteur | Thread (cœur Vita) | Mesure |
|---|---|---|
| `t_cpu_game` | émulation (0) | d'un sceDisplaySetFrameBuf au suivant, moins le temps bloqué (attente du moteur GE, du mixeur SAS, cadencement temps réel) : code recompilé + HLE |
| `t_audio` | où le mixage SAS a lieu | mixage SAS de l'image (la ligne précise « SAS worker, other core » quand il tourne sur l'autre cœur) |
| `t_ge_translate` | décodage (1) | listes GE parcourues et traduites en opérations GL (décodage + enregistrement), par image présentée |
| `t_gl_submit` | GL (2) | rejeu de l'image (appels vitaGL), moins la présentation |
| `t_swap` | GL (2) | présentation : dernier quad, vglSwapBuffers et son attente GPU / vsync |
| `draws`, `state_changes` | GL (2) | appels de dessin ; opérations d'état GL rejouées (enable/disable, blend, profondeur, stencil, masques, scissor, viewport, programme, framebuffer, textures liées et paramètres, uniformes) |
| `vertices`, `tex_uploads` | décodage (1) | sommets convertis ; envois de textures enregistrés |

Version Vita de mesure : `0.0.82-perf` (bulle habituelle BRNG00001).
Réglages de référence : 960×544 fixe par défaut, 480×272 fixe en tenant L
au lancement ; résolution dynamique coupée dans les deux cas (noté en tête
du journal).

Vérifié sur PC (course scriptée, 4 fenêtres de 600 images) : les lignes
s'écrivent, valeurs cohérentes. `t_audio` = 0 sur PC en mode scripté (son
coupé), à vérifier sur Vita.

Base de mesure : le traducteur **sans** l'intégration LWL/LWR/SWL/SWR
(commit 864d48f, fait avant ce brief, gain non mesuré) : retirée du corpus
Vita pour la base, elle sera la première hypothèse mesurée.

## Scène de référence

À fixer avec l'utilisateur : Road Rage sur Waterfront (proposée, la plus
lourde selon lui), même voiture, 60 s depuis le départ, trafic présent ;
plus une séquence de crash/takedown. Chaque scène en 960×544 puis 480×272.

## Étape 1 — Diagnostic

### Limiteur à 30 ?
Oui, dans le jeu lui-même (établi le 2026-11-12, commit 49e30fc) : le
gestionnaire de vblank 0x08855374 force 2 vblanks par image en course
(0x088553F4, tant que l'octet 0x08DCC999 vaut 0). Les menus demandent 30
(0x088360A4 a1 = 1). Retiré sur PC par l'option `fps60` (1 vblank par image
quand le pas vaut 16 ms) ; pas encore essayé sur Vita. Frame réelle une fois
le limiteur retiré sur Vita : **non mesurée**.

### Logique liée au framerate ?
Non au sens « vitesse doublée » : le jeu avance par pas de 1/60 s
(0x08A4A144 : 1/rate s à 0x08ED6AFC, 1000/rate ms à 0x08ED6B00 ; la course
règle rate = 60) et fait N pas par image (+0x32FAC de l'objet jeu, 2 en
course à 30). Mesuré sur PC : 600 pas pour 600 vblanks à 30 comme à 60
(fps60 : N = vblanks écoulés depuis l'image précédente, 0x0880F48C).
Validé en jeu par l'utilisateur sur PC (même vitesse).

### Mesures (2026-11-12, Vita, versions 0.0.82-perf et 0.0.82-perf480)

Scène : Road Rage, circuit US C1 (`Tracks/US/C1_V1`, le même dans les trois
passages), même voiture, crashs et takedowns ; fenêtres de 600 images prises
en course seulement (fenêtres 4-7, 2-5 et 3-7 des trois journaux). Moyenne des
moyennes de fenêtre ; « pire » = plus grand 1 % pire des fenêtres.
Journaux : `original/vita_logs/burnout-perf_0.0.82.log` (deux passages
960), `burnout-perf_480.log`, et `burnout-0.0.82-perf*.log` (image complète).

| Compteur | 960×544 passage 1 | 960×544 passage 2 | 480×272 |
|---|---|---|---|
| image complète (moy. des moyennes sur 300 images) | 37,1 ms | 37,9 ms | 35,4 ms |
| `t_cpu_game` (cœur 0) | 31,4 (pire 48,0) | 33,6 (51,4) | 33,6 (50,5) |
| `t_ge_translate` (cœur 1) | 25,9 (38,9) | 30,5 (45,3) | 29,6 (44,3) |
| `t_gl_submit` (cœur 2) | 18,3 (48,0) | 21,8 (58,9) | 20,1 (67,2) |
| `t_swap` (cœur 2, attente GPU / vsync) | 10,7 (30,5) | 9,6 (29,0) | **0,7** (16,0) |
| `t_audio` (SAS, autre cœur) | 3,3 (7,5) | 3,6 (8,5) | 2,9 (7,1) |
| `draws` | 347 (595) | 398 (605) | 443 (704) |
| `state_changes` | 1 179 (1 933) | 1 534 (2 202) | 1 655 (2 170) |
| `vertices` | 33 646 (60 433) | 38 172 (61 799) | 37 320 (66 328) |
| `tex_uploads` | ~0 | ~0 | ~0 |

Le passage 480 a plus de dessins (scène plus chargée : autre partie, autres
crashs), pas un effet de la résolution.

### Répartition du budget (960×544, budget 60 fps = 16,6 ms par cœur)
Les trois cœurs travaillent en chaîne ; l'image dure autant que le plus lent.
- Cœur 0, jeu recompilé + HLE : ~32 ms (**2× le budget**).
- Cœur 1, traduction GE → opérations GL : ~28 ms (**1,7×**).
- Cœur 2, appels vitaGL ~20 ms + présentation/attente GPU ~10 ms (**1,8×**).
- Son : ~3,5 ms sur le cœur du mixeur SAS.
- Aucun compteur n'est près du budget : un 60 fps demande de diviser par ~2
  les trois cœurs, pas un seul.

### 960×544 contre 480×272
4× moins de pixels : l'image passe de ~37,5 à 35,4 ms (≈ -2 ms, -6 %).
`t_swap` tombe de ~10 à 0,7 ms ; les cœurs 0 et 1 ne bougent pas
(`t_cpu_game` et `t_ge_translate` égaux, à l'écart entre passages près).
**Blocage CPU.** Le GPU coûte ~10 ms d'attente en 960 mais sur le cœur 2
seulement, qui n'est pas le plus lent : les pistes GPU (overdraw, shaders,
effets plein écran) ne rapporteraient presque rien sur l'image tant que les
cœurs 0 et 1 restent à ~30 ms. Laissées de côté.

## Étape 2 — Liste proposée (en attente de validation)

Classée par gain estimé ; estimations, pas des mesures.

| # | Cœur | Optimisation | Gain estimé | Confiance |
|---|---|---|---|---|
| 1 | 1 | Conversion des sommets (33 000-62 000 par image) : plus de géométrie statique réutilisée, le reste en NEON | 3-8 ms (cœur 1) | moyenne, après découpage des 28 ms du cœur 1 |
| 2 | 1 | Listes d'affichage identiques d'une image à l'autre : ne pas les réinterpréter (idée PPSSPP, à étudier) | 2-6 ms (cœur 1) | faible à moyenne |
| 3 | 2 | Changements d'état GL inutiles (1 200-1 800 par image pour ~400 dessins) | 1-4 ms (cœur 2) | moyenne ; recensement par type codé (`perf_state_census`, version PERF), pas encore mesuré |
| 4 | 0 | Traducteur : LWL/LWR/SWL/SWR intégrés (commit 864d48f) | 0,1-0,5 ms | bonne, risque < seuil |
| 5 | 0 | Fonctions du jeu les plus chères réécrites (collision 0x08A3C868 en tête, ~1,4 ms) | 0,5-1 ms chacune | moyenne |

Même si les cinq réussissent, le cœur 0 resterait vers 30 ms : 30 fps plus
stable, pas 60.

### Refonte à valider avant de commencer (> 1 journée) : registres invités dans les registres hôte

Constat (code ARM produit par GCC pour l'unité 0142, `restrict/u_*.s` du
2026-11-12) : chaque écriture d'un registre PSP (gpr, fpr, vfpu) devient un
stockage dans `AllegrexContext` en mémoire, chaque lecture en début de bloc un
chargement. Les accès mémoire invités 32 bits n'empêchent déjà plus GCC de
garder les valeurs (types distincts, `GuestWord`), mais les accès octet/demi-
mot (char), les appels sortants et la fin de chaque unité imposent tout de
même de ranger le contexte, et les valeurs vivantes d'un bloc à l'autre
repassent par la mémoire.

Idée : dans chaque fonction d'unité, des variables locales pour les registres
invités utilisés, chargées à l'entrée et rangées seulement aux sorties de
l'unité, autour des appels (chaînage, HLE, rt.*) et aux points où le runtime
lit le contexte. GCC peut alors les placer en registres ARM et supprimer les
allers-retours mémoire dans les boucles chaudes.

Risques : correction (chaque point où le runtime ou un autre bloc lit le
contexte doit voir les valeurs rangées) ; coût des chargements/rangements aux
entrées/sorties pour les fonctions courtes très appelées (peut annuler le gain) ;
temps de compilation (unités plus lourdes pour GCC). Vérification : empreinte
mémoire du banc PC identique au bit près, puis banc Vita (`burnout-bench`,
déterministe) pour le temps du cœur 0.

Gain : inconnu tant qu'un prototype n'est pas mesuré. C'est le seul candidat
identifié à ce jour pour viser une division du cœur 0 par ~2 ; rien ne
garantit qu'il y parvienne.

Mesures PC : le code généré est compilé sans intégration (`/Ob0`) sur PC, ses
temps ne représentent pas la Vita ; le PC sert à l'exactitude, la vitesse se
mesure sur Vita.

## Recherche du 2026-11-12 au soir (sources et journaux)

- **Cœur 1, phases échantillonnées** (journaux 0.0.82-perf, en course, % des
  échantillons du cœur 1) : 10 boucle des listes (y compris PRIM rapides et
  matrices) ~21 % ; 11 fin d'image ~12 % ; 6 enregistrement des dessins
  ~12 % ; 14 commandes d'état ~11 % ; 3 lecture des sommets ~9 % ; 4-5 index
  ~9 % ; 7-9 textures, uniformes, envoi des sommets ~16 %. La boucle est déjà
  optimisée (état répété sauté sans appel, chemins rapides matrices et PRIM).
  La conversion des sommets n'est pas le premier poste : la liste est revue.
- **vitaGL** (`tools/src/vitaGL`, README) : notre build a `NO_DEBUG`
  (`SKIP_ERROR_HANDLING`) mais aucun speedhack. `DRAW_SPEEDHACK` saute
  surtout des vérifications par attribut quand les sommets ne sont pas dans
  un VBO ; nos dessins passent déjà par des VBO (géométrie statique en
  tampons GPU, le reste copié une fois en mémoire scratch + `vglBufferData`,
  `ge_gl.cpp` ~4360). Gain attendu < 0,5 ms : écarté.
- **PortableKit** (github.com/TeamGDB/PortableKit, MIT, frère de PSPRecomp,
  `tools/codegen_main.cpp` ~1720, `docs/ARCHITECTURE.md`) : ils ont eu un
  cache de registres invités (par bloc et entre unités, « hot-register ») et
  l'ont retiré : dans des fonctions générées de ~10 000 lignes, MSVC ne
  convergeait plus (unités > 2 Go, mémoire saturée). Notre note du
  2026-10-30 (mémoire du projet) : « registres en variables locales = perte
  tant qu'un accès mémoire fait un appel » ; ce n'est plus le cas chez nous
  (table de pages). Risque principal de la refonte : la compilation.
- **Point oublié dans le budget** : en course à 30, le cœur 0 fait 2 pas de
  simulation par image ; à 60 il n'en ferait qu'un. Les ~32 ms ne sont donc
  pas le coût d'une image à 60. Mesure prévue : version PERF avec
  `ux0:data/burnout/fps60.txt` (limiteur coupé) et
  `ux0:data/burnout/single_step.txt` (un pas par image même si l'image est
  lente : la course ralentit, mais `t_cpu_game` donne le coût d'une image à 60).

## Nuit du 2026-11-12 au 13

- **Recensement des états GL (piste 3)**, PC, course scriptée (les comptes
  ne dépendent pas de la machine), par image : enable/disable ~59 (4
  inutiles), blend ~20 (1), profondeur ~16 (1), stencil ~34 (3), scissor/
  viewport ~8 (6), programmes ~75 (0), textures liées ~245 (0), uniformes
  vec4 ~200, **matrices ~1 030**. Moins de 20 changements inutiles par image :
  **piste 3 abandonnée** (gain < 0,3 ms). Les matrices (2-3 par dessin) sont
  déjà gardées par slot et envoyées au dessin si changées ; leur coût réel
  dans vitaGL reste à mesurer (piste possible pour le cœur 2).
- **Bancs Vita A/B prêts** pour la piste 4 (LWL/LWR/SWL/SWR intégrés) :
  `out/vita_opt/burnout-bench-A-base.vpk` (rapport `ux0:data/burnout-bench.txt`)
  et `burnout-bench-B-lwl.vpk` (`burnout-bench-B.txt`). Même course
  scriptée, même bulle (BRNB00001) : installer A, lancer, puis B, lancer.
  **Correction 2026-11-13** : le fichier A était une copie de B, au bit près
  (même md5, écrit `burnout-bench-B.txt`), et tous les corpus actuels ont déjà
  LWL/SWL intégrés (28 sites, unités 0021 et 0142) : la piste 4 seule n'est
  pas mesurée. B sert de référence pour C (C = B + registres en locales).
  A renommé `NE-PAS-UTILISER_bench-A-est-une-copie-de-B.vpk`.
- **0.0.83-perf** (compteurs + `fps60.txt` / `single_step.txt`) et **0.0.83**
  (jeu normal) : couleurs 4-8 dans les palettes, traducteur LWL/SWL intégré.

### Refonte du traducteur, première expérience (registres GPR en variables locales)

`profiles/burnout/tools/lower_gpr_locals.py` (expérimental, post-passe sur un
corpus Vita) : dans la fonction d'entrée de chaque unité, les registres
invités 1-31 deviennent des variables locales ; elles sont rangées dans le
contexte avant chaque `return` et avant chaque appel qui reçoit le contexte
(chaînage d'unités, fonctions natives), et relues après. FPR et VFPU non
touchés.
- Compilation GCC de l'unité 0142 : 27 s avec ou sans (pas d'explosion,
  contrairement à PortableKit sous MSVC).
- Statique (unité 0142) : plus d'accès mémoire au total (rangement de 31
  registres à chaque sortie/appel) ; la boucle chaude ne se compare pas de
  façon fiable (blocs réordonnés). Seule une mesure Vita tranche.
- Corpus complet transformé (`out/gen_regs`, 167 unités, aucun motif non
  traité) ; banc **C** : `burnout-bench-C-regs.vpk`, rapport
  `burnout-bench-C.txt`. Exactitude : empreinte RAM du rapport C = celle de A.
- **Exactitude vérifiée sur PC** (corpus PC transformé de même,
  `out/gen_pc_regs`, build `out/burnout-regs`, banc
  `race_world_tour_compact`) : empreinte RAM `3c94cc5c24524146`, identique à
  la référence. Deux erreurs trouvées ainsi avant la Vita : les `return`
  n'étaient pas rangés (caractère de contrôle à la place de `\b` dans le
  script), et `AllegrexContext::execute_signed_add` (ADD/ADDI avec trap, 24
  sites) lit et écrit `ctx.gpr` lui-même (fait sur les locales). Compilation
  MSVC du corpus transformé : 6 min, pas d'explosion. Code Vita du banc C :
  segment de code +23 % (rangements aux sorties et appels).

### État au matin du 2026-11-13

- Banc **C corrigé** : compilation Vita arrêtée par le système (mémoire basse)
  à 96/177 unités dans `out/vita-regs` ; les objets compilés sont gardés,
  reprendre avec `cmake --build out/vita-regs -j 2 --target
  burnout-bench.vpk-vpk` (avec la ligne temporaire `BURNOUT_BENCH_SUFFIX "-C"`
  en tête du bloc du banc dans `main_vita.cpp`). L'ancien C (faux) est renommé
  `NE-PAS-UTILISER_...`.
- Variante **D** (GPR + FPR en locales, option `--fpr`) : corpus transformés
  (`out/gen_regs2`, `out/gen_pc_regs2`), pas encore compilés ni vérifiés.

### Mesure à 60 (0.0.83-perf, fps60 + single_step) et bancs de contention, 2026-11-13

- 0.0.83-perf, Road Rage US C1, **un seul pas de 1/60 s par image** :
  `t_cpu_game` 30-36 ms (fenêtres de course), comme à 30 avec deux pas.
  Le coût du cœur 0 est **par image dessinée**, pas par pas de simulation.
- Le banc sans affichage (même code de jeu) fait une image (2 pas) en
  **9,17 ms**. Écart ×3-4 avec le jeu affiché.
- Banc S (B + charge sur les cœurs 1-2, `ux0:data/burnout/stress.txt`),
  même course, même empreinte `0b86387cdb031e18` :

| Charge sur les cœurs 1-2 | ms/image (flip moy.) | p99 |
|---|---|---|
| aucune (banc B) | 9,17 | 9,30 |
| `cpu` (calcul en registres) | 9,11 | 9,17 |
| `mem` (copies de 4 Mio en boucle) | **15,28** | 15,46 |

  → seul le trafic mémoire des autres cœurs ralentit le cœur 0 (+67 % au
  pire cas). Cela n'explique pas tout l'écart (15 contre ~30 ms) : reste du
  travail propre au jeu affiché sur le cœur 0 (prépasse 2,7 + copie des
  listes 2,4 + attente du front GE 2-4 + interruptions 6,4 ms par image dans
  le journal) et la scène (Road Rage contre la course du banc).
- **Même course que le banc, avec affichage** (0.0.83-perfb, World Tour
  Compact 1re épreuve, conduite manuelle, 30 i/s, 960x544) : charge du cœur 0
  64-70 % de 33,3 ms → **~21-23 ms occupés par image**, contre **9,17 ms**
  sans affichage (banc B). Détail par image : user_main 16-25 ms, interruptions
  3,3, prépasse des listes 0,7, copie des listes (350 Kio) 2,0, attente du
  front GE 0,2-2,4. Le jeu tient 30 sur cette course (cœurs à ~70 %).
- **Correction de l'instrument** : `t_cpu_game` = intervalle entre flips moins
  les attentes repérées ; l'attente de la vblank suivante quand le jeu est en
  avance n'est pas retirée. Il surestime donc le travail quand il reste de la
  marge (Compact : 26-28 affiché pour ~22 occupé). La charge par cœur
  (`core load`) est la mesure fiable. Road Rage : 77-92 % → 26-31 ms occupés.
- **Piste 1 (copie des listes hors du cœur 0), recensement Vita**
  (0.0.83-perfc, Compact) : en course, **100 % des listes** sont réécrites
  par le jeu pendant leur marche, 20-37 Kio sur ~345 Kio, répartis sur des
  dizaines de pages différentes des deux tampons (0x08400000 et 0x08500000,
  1 Mio chacun, en alternance). Pas de petite zone fixe à copier seule : la
  copie complète reste nécessaire. Gain possible restant (copie partagée
  entre deux cœurs) < 1 ms : **piste 1 abandonnée** sous cette forme.
- **Temps des imports sur le cœur 0** (0.0.83-perfd, Compact, par image) :
  sceDisplayWaitVblankCB 8,5-11,9 ms (**attente** : marge à 30 i/s, pas du
  travail) ; **sceGeListEnQueue 2,8-3,4 ms, entièrement dans l'interruption
  vblank** (le gestionnaire 0x08A37B94 met la liste en file : attente du
  front + prépasse 0,7 + copie 2,0) ; sceAudioOutputPannedBlocking 1,2-2,2
  (attente du son) ; sceIoReadAsync 0,5-1,2 ; sceAtracDecodeData 0,3-1,0 ;
  __sceSasCore 0,4. L'« anomalie » de l'interruption = la mise en file des
  listes, déjà connue. La copie (2,0 ms pour ~350 Kio) est ~4× plus lente
  que la même copie mesurée à froid (0,46-0,64 ms pour 400 Kio).
- **Mesure Vita de la copie partagée** (0.0.83-perfe, Compact, fenêtres de
  course alternées, 4 de chaque) : copie 1,87 → **1,25 ms/image** ;
  sceGeListEnQueue 2,86 → **2,25 ms** (hors 1re fenêtre) ; charge du cœur 0
  67,8 % → 60,8 % (bruitée, scène différente d'une fenêtre à l'autre).
  **Gain retenu ~0,6 ms** (> 0,3) : copie partagée **activée par défaut**.
- Bilan Compact : ~22 ms occupés = imports ~5 ms + code du jeu ~17 ms (9 ms
  sans affichage). Le surcoût du code du jeu (~8 ms) ne vient pas d'un
  travail en plus (PC : 5,5 contre 5,7 ms, même course) : **contention
  mémoire** (cœurs 1-2 et GPU).
- **Copie des listes partagée entre cœur 0 et cœur 2** (`ge_front_copy_split`,
  chaque cœur copie une moitié ; le front est inactif à ce moment-là) :
  exactitude PC vérifiée (course Compact rendue, `--async-ge`, empreinte
  `7aa828f82c773ab5` identique, 4 captures identiques à l'octet). Version
  Vita **0.0.83-perfe** : alterne coupée/active à chaque rapport (300 images),
  le journal donne « copy X ms/flip (split on|off) ». À mesurer.
- **Dessin coupé une fenêtre sur deux** (0.0.83-perff, Compact, 12 paires de
  fenêtres de course) : `user_main` **17,97 ms avec dessin, 17,54 sans** ;
  le GPU et le fil GL ne ralentissent pas le code du jeu (hypothèse
  « contention par le GPU » écartée). Par contre, avec dessin, **le cœur 2
  est à 98-99 %** (fil GL + front GE + son) et l'émulation attend le front GE
  11-15 ms par image (sans dessin : 0,4-0,8 ms, cœur 2 ~60 %).
- **Biais du banc** : le banc sans affichage tourne **sans son** (SAS refusé,
  le jeu ne gère alors pas ses voix) ; la comparaison 9,17 contre ~22 ms
  inclut donc le travail du son. À mesurer sur PC (son on/off).
- **Son, mesure PC** (course Compact rendue, `--sound-dump` + musique) :
  `user_main` 5,8-6,0 ms avec son, 6,0-6,1 sans. Le son ne pèse pas sur le
  code du jeu ; le biais du banc est négligeable.
- État : code du jeu sur le cœur 0 ~18 ms (Vita, affichage) contre ~9 ms
  (banc) ; ni le GPU (perff), ni le son (PC), ni un travail en plus (PC) ne
  l'expliquent. Reste : trafic mémoire/cache du cœur 1 (décodage des listes,
  lit ce que le cœur 0 vient d'écrire) et du cœur 2. Pour 60 i/s il faut en
  plus diviser par ~2 la charge du cœur 2 (98-99 % avec dessin, à 30 i/s).
- **Chemin de dessin du fil GL (cœur 2)**, 0.0.83-perfg, Compact, par image :
  hachage de la géométrie statique **7,2-8,1 ms pour ~550 Kio** (~70 Mo/s,
  attente mémoire ligne par ligne), copie vers la mémoire GPU 1,7-2,0 ms
  (~185 Kio), expansion des indices 0,5-0,7, uniformes 1,2-1,9, appels
  vitaGL (dessins et états) 7,4-8,7 ms ; fil GL occupé 33-35 ms ; cœur 2
  98-99 %.
- **Préchargement dans le hachage** (`__builtin_prefetch` 256 o d'avance,
  même résultat) : 0.0.83-perfh, hachage **2,4-2,9 ms** (−5 ms), fil GL
  occupé 22-27 ms, cœur 2 79-85 %, attente du front GE par l'émulation
  11-15 → 0,2-0,4 ms. **Gardé.** (Le recensement des réécritures de perfc
  tournait encore sur le cœur 2 dans perfd-perfg : coupé, `ge_front` 32 % →
  14 % ; ce n'est pas un gain du jeu, un biais de mesure en moins.)
- **Détail du fil GL et de vitaGL** (0.0.83-perfj, Compact ; chiffres gonflés
  par l'instrumentation) : glDrawElements de vitaGL **17-19 µs par dessin**
  (~380/image) : programme de fragments 1,3-1,6, textures 1,4-1,7, attributs
  1,5-1,6, programme de sommets 2,4-2,7, uniformes 2,2-2,4, flux 1,1-1,2,
  **reste (surtout sceGxmDraw) 7-8,4**. sceGxmDraw émet tout l'état en
  attente : incompressible. Contourner vitaGL ne rendrait que ~2 ms/image
  (et non la majeure partie) : à revoir avant d'y investir des jours. Op GL
  par image : Draw 21-23 ms, BindTexture 1,9-2,2 (~220), UniformMatrix 1,2
  (~650), Present 1-1,4, UseProgram 0,7, Uniform4 0,45.
- **Biais d'instrumentation** : le recensement des états GL (une table par
  op, ~1 700 op/image) et les chronos tournaient dans toutes les versions
  PERF ; désormais seulement avec `ux0:data/burnout/gl_detail.txt`.
  0.0.83-perfk = mesure propre du fil GL (sans ces outils).
- **Mesure propre** (0.0.83-perfk, Compact, sans outils de détail) : fil GL
  occupé **19,7-24,3 ms/image** (upload 7,7-8,8, dessins/états 5,7-7,6,
  autres 4,0-4,9, présentation 1,4-4,0) ; cœur 2 **69-75 %** ; cœur 1
  75-81 % ; **cœur 0 69-85 %** (`user_main` 17-24 ms) : le cœur 0 redevient
  le plus chargé.
- **Cœur 0 : divisions 64 bits natives** : les deux fonctions les plus
  chaudes après les crochets du profil Vita (0x08A99388 `__udivdi3` 2,7 %,
  0x08A998FC `__umoddi3` 2,8 % du code du jeu ; appelées entre autres par la
  conversion nombre → chiffres 0x08A39C48) remplacées par une division
  native (`patch_native.py` FUNCTIONS64, `native_funcs.cpp`). PC : mode de
  vérification, 65 536+ appels de chaque, **0 écart** ; empreinte
  `7aa828f82c773ab5` et 4 captures identiques. Version 0.0.83-perfl.
- **Banc D** (même VPK, `ux0:data/burnout/no_native.txt` pour couper) :
  divisions natives coupées **10,28 ms/image** (p99 10,52), actives
  **9,25 ms** (p99 9,43), empreinte `0b86387cdb031e18` identique :
  **−1,03 ms (−10 %) sur le code du jeu**. Gardées.
- **Mode 60 réel** (0.0.83-perfl + `fps60.txt`, sans pas unique, Compact) :
  compteur VSH du joueur **~40 i/s**, saccadé (images de 1 ou 2 vblanks).
  Charge : **cœur 1 95-97 % (fil de décodage saturé)**, cœur 2 79-89 %,
  cœur 0 77-85 % (attend le décodage 3,6-5,7 ms/image). Fil de décodage
  (échantillons) : boucle de lecture de la liste 24 %, indices 14-19 %,
  enregistrement des dessins 14 %, commandes d'état 9-11 %, textures 13 %.
- 0.0.83-perfm : préchargement dans la boucle de lecture de la liste
  (256 o d'avance) : **aucun gain** (compteur VSH toujours ~40 i/s, la
  boucle garde 23-26 % des échantillons du fil de décodage). **Retiré.**
- **Ce qui borne le mode 60** : le fil GL est occupé **~25 ms par image**
  (perfl en mode 60 : 24,6-25,9 ms) → plafond ~40 i/s, exactement ce que le
  compteur affiche. Le fil de décodage (cœur 1) attend ensuite en fin
  d'image (phase 11 : 19-22 % des échantillons avec perfm). Pour 60 i/s il
  faut ramener le fil GL de ~25 à < 16,7 ms, et les deux autres cœurs aussi.
- **Optimisation c en mode 60** (0.0.83-perfn, hachage une image sur 4) :
  compteur toujours ~40 i/s, **le décor clignote par moments** (retour du
  joueur) ; fil GL « occupé » 26,5-28,4 ms/image alors que le cœur 2 n'est
  qu'à 72-84 % : le fil GL **attend** dans vitaGL (GPU ou anneaux GXM).
  **Rejetée**, retour à b. Test suivant : mode 60 en 480x272 (L au
  lancement) pour savoir si le GPU borne.
- **Nuit du 2026-11-13** (autonomie, tests Vita au réveil) :
  - profileur PC sur le fil de décodage et le fil GL
    (`PSPRECOMP_PROFILE_THREAD=decode|gl`, headless `--profile`) : pas de
    point chaud unique (décodage : boucle 13 %, fast_prim 12 %, flush 8 %,
    ge_command 8 % de son temps actif) ;
  - appels GL par dessin (pointeurs d'attributs, liaisons) : vitaGL ne fait
    que quelques écritures par appel, gain < 1 ms : non fait ;
  - boucles VFPU du jeu en natif : sémantique délicate (préfixes, matrices
    transposées) pour ~0,15 ms chacune : non fait ;
  - présentation en mode 60 : 2-9 ms/image (0,6-2 à 30) ; triple tampon,
    pas d'attente de vblank ; ~3 scènes par image (1024x544, 960x544,
    128x544).
  - Prêts sur la Vita : `0.0.83-perfo480` (480x272 forcé) et `0.0.83-perfp`
    (960x544, interrupteurs `no_fxaa.txt`, `offscreen_1x.txt` posés) ;
    `fps60.txt` posé. But : savoir si le GPU borne le mode 60.
- Écart affiché/sans affichage sur le cœur 0 (~13 ms) : contention mémoire
  (jusqu'à +6 ms, banc S), travail HLE du rendu sur le cœur 0 (copie +
  prépasse + attentes, ~3-5 ms), reste ~3-4 ms non attribué.

### Tests GPU du 2026-11-13 après-midi (0.0.83-perfo480 et perfp, fps60)
- **perfo480** (480×272 forcé) : present 0,5 ms (GPU quasi libre), pourtant 24-26 ms/image (~40 i/s). Charge : cœur 0 80 %, **cœur 1 98-100 %** (decode), cœur 2 83-86 % (gl 54 %, ge_front 18 %, sas_mix 8 %).
- **perfp** (960, FXAA coupé, effets 1x) : present 3,4-6,6 ms, 26-29 ms/image ; cœur 1 89-96 %.
- **Conclusion : la puce graphique n'est pas le frein.** Le frein est le processeur, d'abord le **décodage des listes (cœur 1, saturé)**, puis le fil GL (envoi des sommets 9-10 ms, appels de dessin 6-7 ms).
- Budget à 60 : ~65 ms de calcul par image (c0 ~20, c1 ~24, c2 ~21) pour 3 × 16,7 = 50 ms disponibles. Il faut retirer ~25 % **et** rééquilibrer, le cœur 1 en priorité.
- Compteur VSH (utilisateur) : perfo480 **49-50 i/s en pointe**, perfp 960 allégé **42 max**. Le GPU coûte donc quelques i/s en 960 (attente present 3-6 ms), mais la moyenne reste bornée par le CPU (cœur 1 à 100 % même en 480).
- Fichiers no_fxaa.txt et offscreen_1x.txt retirés de la Vita ; fps60.txt laissé.

### Cœur 1 contre cœur 2 (0.0.83-perfq et perfr, 2026-11-13 soir, mode 60, 960)
- **perfq** (phases du décodage détaillées, aucune autre modification) : la
  phase 5 « 24 % » de perfo480 était un artefact (le chemin rapide des PRIM
  héritait de la phase). Vraie répartition du fil de décodage : chemin rapide
  des PRIM 14-17 %, boucle 12-16 %, `flush` 27-30 %, commandes d'état 9-15 %,
  **triangles ombrés à plat (copies par triangle) 7-10 %**, attente du fil GL
  dans `submit_frame` **10-17 %** : le cœur 1 attendait le cœur 2.
- **perfr** : empreinte de la géométrie statique calculée par le fil de
  décodage au lieu du fil GL (PC : 5 captures identiques). Envoi des sommets
  du fil GL 9,5 → 6,3 ms, cœur 2 83-87 → 70-74 % ; mais cœur 1 **98-100 %**,
  image 26-27 → 27-29 ms ; compteur VSH : « l'impression que ça a baissé ».
  **Rejeté** (le hachage coûte plus que l'attente libérée). Les deux fils sont
  maintenant à égalité vers 26 ms : il faut alléger les deux.

- **perfs** (ombrage plat : triangles d'une seule couleur indexés, PC 5
  captures identiques, sommets -3,7 %) : phase « plat » du fil de décodage
  7-14 % → 2-3 %, cœur 1 98-100 % → 82-91 %, attente du fil GL 14-27 %.
  Image inchangée (~27 ms) : le cœur 2 borne. **Gardé** (cœur 1 libéré).
- **perft** : interrupteur `front_core0.txt` (fil ge_front sur le cœur 0 ;
  « h » : devant l'émulation). Testé avec « h » (émulation priorité 160,
  ge_front 150) : **cœur 2 83-92 → 65-72 %**, cœur 0 80-93 → 92-97 %,
  cœur 1 88-99 % ; image 24,4-27,4 ms (avant 25,8-29). Attente du front par
  l'émulation 4,5 → 0,1-1,6 ms, mais « interrupts » 2,5-3,7 → 6,8-7,4 ms
  (le front préempte l'émulation pendant les tranches d'interruption) et
  copie des listes 1,3 → 2,2 ms (les deux moitiés sur le même cœur). Le fil
  GL attend maintenant la puce graphique 6,5-10 ms (960) : bornes cœur 0,
  cœur 1 et GPU. Compteur VSH : **moyenne ~37, pointes 42, chutes à 15**.
  Les à-coups du journal (lectures de fichiers 100-500 ms pendant le
  streaming de la piste) existent aussi dans perfq/perfr/perfs (16-27 par
  session), pas propres à perft. **Rejeté** (pas de gain moyen, cœur 0 plein) ;
  `front_core0.txt` retiré de la Vita.

### Pilotage à distance et cache d'état GXM (0.0.83-perfu/perfv, 2026-11-13 soir)
- vitacompanion + NoLockScreen installés par l'utilisateur ; `tools/vita_remote/`
  (deploy, go_race synchronisé sur le journal, step/captures, measure) : mesures
  Vita sans intervention. Le jeu rejoue la dernière épreuve choisie (le test
  tourne maintenant sur la poursuite d'Airport Terminal, voiture de police).
- vitaGL, coût d'un dessin (mode détaillé, ~440 dessins/image) : avant 18,6-18,7 µs
  (programme de sommets 2,4, envoi final dont sceGxmDraw 8,3-8,8).
- **Cache d'état GXM** dans vitaGL (programme de sommets non re-patché, programmes
  et textures non renvoyés s'ils sont ceux du dessin précédent) : même version,
  même épreuve, `no_state_cache.txt` pour couper : **coupé 18,8-20,0 µs, actif
  16,8-18,4 µs** (programme de sommets 2,9 → 1,7) : **~1,5 µs/dessin, ~0,65 ms
  par image sur le cœur 2**. Captures : mêmes objets, écarts de contours dus au
  mouvement de la caméra. **Gardé.**

### Vérification du décor une image sur N, corrigée (0.0.83-perfw)
- Cause du clignotement de l'option c : un morceau que le jeu réécrit (couleurs
  de la route) n'était revu qu'une image sur 4 jusqu'à 3 changements comptés
  (~12 images d'escalier). Correction : un morceau vu changer une fois est
  vérifié à chaque image ensuite (adresses gardées après éviction).
- PC (course Compact, N = 4) : 4 captures sur 5 diffèrent, faiblement (aucun
  pixel à plus de 60/765 d'écart ; 0,02 à 3 % des pixels, sur la route) : la
  couleur d'un morceau peut arriver 1 à 3 images en retard, **une seule fois**
  par morceau (~0,1 morceau par image).
- Vita (poursuite Airport, même version, `probe_every.txt` 1 puis 4) : hachage
  **2,6-2,8 → 1,1 ms/image** (~1,6 ms sur le cœur 2), 36-96 morceaux « en
  retard » sur la session. **En attente du jugement visuel de l'utilisateur**
  (règle : pas de perte de qualité cachée) ; défaut inchangé (1).

- **Road Rage, piste la plus lourde (utilisateur, perfz, mode 60)** : 36-39 ms/image,
  cœur 1 85-96 %, attente GPU jusqu'à 18,7 ms : pire cas, référence pour le 60.

### Lectures d'horloge du fil GL (0.0.83-perfx)
- Une lecture d'horloge coûte 0,87 µs sur la Vita ; le fil GL en faisait 3 par
  dessin (temps des dessins et de l'envoi des sommets, statistiques seulement),
  **versions des joueurs comprises** : ~1,2 ms/image de travail sur le cœur 2.
- Correction : un dessin sur 16 chronométré, compté 16 fois (tous avec le mode
  détaillé, ou `time_every_draw.txt` pour comparer).
- Vita, même version, même scène (poursuite Airport, fenêtre 2700) :
  tous chronométrés gl 55,1 %, cœur 2 85 % ; 1 sur 16 **gl 48,8 %, cœur 2 79 %**
  (~1,6 ms/image). Le cœur 1 (décodage) est alors à 100 % et borne l'image
  (26,4-26,5 ms). **Gardé** (aucun effet sur l'image).
- Mode détaillé, découpe de l'envoi des sommets (ms/image) : entrée 1,8 (surtout
  les horloges), hachage des index 1,2-1,3, recherche dans le cache 0,9-1,0,
  liaison des tampons 0,4-0,5.

### Où part le temps du cœur 1 (0.0.83-perfy, chronos précis, 1 dessin sur 16)
- Les histogrammes de phases sont **biaisés** : l'échantillonneur tourne sur le
  cœur 2 et ne passe que quand ce cœur est libre. Mesures directes à la place :
  le décodage n'attend ni le fil GL (0,0 ms/image) ni les listes (0,1) : le
  cœur 1 est vraiment plein en course.
- Par image (poursuite Airport, ~440 dessins) : lecture des listes ~12-15 ms ;
  `flush` ~11-13 ms = cible 1,1 + matrices 2,4 + éclairage 1,0 + textures 3,4
  + autres réglages ~3,1 + sommets et enregistrement 2,2.
- Préchargement dans la sonde des textures en RAM : phase textures 10,3 → 9,3 %
  (bruit), image 26,5 → 26,0 : sous le seuil, **retiré**.
- Les captures d'écran à distance bloquent le jeu 1 à 15 s (encodage PNG) :
  fenêtres de mesure prises avant la capture.

### Nuit du 2026-11-13 au 14 : lectures asynchrones, cœur système (branche perf-core1-cache)
- **sceIoReadAsync vraiment asynchrone** (fil à part, PollAsync rend 1 tant
  qu'elle court ; `sync_io.txt` pour couper). Avant : lecture bloquante dans
  l'appel (~8 ms/flip en moyenne en Road Rage, lectures de dizaines de ms).
  Sur la Vita, std::thread n'existe pas (« Enable multithreading to use
  std::thread » : le jeu s'arrêtait) : pthread. PC : course chargée normalement.
- **Cœur système (3)** : l'utilisateur a installé CapUnlocker. Le mixage SAS et
  la sortie audio s'y placent s'ils y tournent vraiment (sceKernelGetCpuId),
  sinon restent sur leur cœur ; `no_system_core.txt` pour l'interdire.
  Journal : « [audio] output thread on the system core (3) », « [sas] mixer
  thread on the system core (3) ».
- **Mesure Silver Lake, Éliminateur, mode 60 (perfz + ces deux changements)** :
  cœur 0 70 %, cœur 1 82-84 %, **cœur 2 53 %** (gl 36 %, ge_front 16 %),
  cœur 3 ~11 % (sas 8 %, audio 3 %) ; image 28,8-30,1 ms ; fil GL : envoi des
  sommets 5,3-5,8, dessins 5,8-5,9, **attente de présentation 16,5-16,7 ms**
  (puce graphique, Silver Lake très chargé en feuillages) → borne GPU sur ce
  circuit en 960, puis cœur 1 (~24 ms de travail) et cœur 0 (~20 ms).
- **Même circuit en 480×272** (perfo480, Silver Lake Éliminateur, piste
  C3_V2) : attente de présentation **0,5 ms**, image 24-25 ms, **cœur 1
  98-99 %**, cœur 0 79-86 %, cœur 2 66 %. Conclusion : à Silver Lake en 960 la
  puce graphique fait attendre ~16 ms/image ; en 480 le cœur 1 (~24 ms de
  traduction) borne. Le 60 demande les deux : moins de travail GPU en 960
  (ou une résolution plus basse, à décider avec l'utilisateur) et ~8 ms de
  moins sur le cœur 1.
- Outils : captures à distance sans relevé des dessins (il figeait le jeu des
  secondes), menus synchronisés sur le journal (`boot_to_menu.sh`), écran
  rallumé à chaque appui (touches ignorées écran éteint).

## Hypothèses et mesures

| Date | Hypothèse | Changement | ms avant | ms après | Décision |
|---|---|---|---|---|---|
| 2026-11-13 | Géométrie statique vérifiée une image sur 4 (option c) | `kOptimisations` c | ~40 i/s | ~40 i/s, clignotements | **Rejeté** |
| 2026-11-13 | Préchargement dans la boucle de lecture des listes (cœur 1) | `ge_run` | ~40 i/s | ~40 i/s | **Rejeté** (aucun gain) |
| 2026-11-13 | __udivdi3/__umoddi3 du jeu en divisions natives (cœur 0) | `patch_native.py` FUNCTIONS64 | banc 10,28 | banc 9,25 | **Gardé** (−1,03 ms, −10 %) |
| 2026-11-13 | Préchargement dans le hachage de la géométrie statique (fil GL) | `fnv` + `__builtin_prefetch` | hachage 7,2-8,1 | hachage 2,4-2,9 | **Gardé** (−5 ms cœur 2) |
| 2026-11-13 | Copie des listes partagée cœurs 0 et 2 | `ge_front_copy_split` | enqueue 2,86 (copie 1,87) | enqueue 2,25 (copie 1,25) | **Gardée** (~0,6 ms, PC identique) |
| 2026-11-13 | Registres GPR invités en variables locales C++ (moins d'accès à `ctx`) | `lower_gpr_locals.py`, banc C vs B (Vita, banc sans affichage, même course, empreinte `0b86387cdb031e18` identique) | 9,17 (flip moy.), p99 9,30 | **12,07**, p99 12,32 (+31 %) | **Rejetée** : rangement/relecture des 31 registres à chaque sortie et appel coûte plus que le gain ; code +23 %. Variante D (FPR en plus) non poursuivie sous cette forme. |
| 2026-11-13 | Empreinte de la géométrie statique sur le fil de décodage (cœur 1) | `record(Op::Draw)` + `fnv` | image 26-27 ms | 27-29 ms, cœur 1 100 % | **Rejeté** |
| 2026-11-13 | Ombrage plat : triangles uniformes indexés au lieu de copiés | `decode_prim` | fil de décodage plat 7-14 % | 2-3 %, cœur 1 -8 points | **Gardé** (image inchangée, cœur 2 borne) |
| 2026-11-13 | Fil ge_front sur le cœur 0, devant l'émulation | `front_core0.txt` « h » | ~40 i/s | ~37 (pointes 42, chutes 15), cœur 2 -20 points, cœur 0 97 % | **Rejeté** |
| 2026-11-13 | Cache d'état GXM dans vitaGL | `vglEnableStateCache` | 18,8-20,0 µs/dessin | 16,8-18,4 µs/dessin (~0,65 ms/image cœur 2) | **Gardé** |
| 2026-11-13 | Décor vérifié 1 image sur 4, morceaux changeants à chaque image | `probe_every.txt`, `g_geometry_ever_changed` | hachage 2,6-2,8 ms | 1,1 ms (~1,6 ms cœur 2) | **Rejeté** : l'utilisateur voit les feux des voitures clignoter (Road Rage, piste la plus lourde, ~250 morceaux en retard) ; confirmé : sans lui, plus de clignotement (cache d'état GXM et autres changements hors de cause) |
| 2026-11-13 | Fil GL : 1 dessin sur 16 chronométré | `draw()`, `time_every_draw.txt` | gl 55,1 %, cœur 2 85 % | gl 48,8 %, cœur 2 79 % (~1,6 ms) | **Gardé** |
| 2026-11-13 | Raccourci de bind_texture (même état de texture que le flush précédent) | `g_texture_memo` (PC 5 captures identiques) | textures 3,31 ms/image | 3,31 ms/image | **Rejeté** (aucun gain) |
