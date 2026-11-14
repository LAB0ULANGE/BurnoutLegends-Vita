# Comparaison : ports Vita de THUG / THUG2 et Burnout Legends (recomp)

Date : 2026-11-11. Étude en lecture seule.

Références clonées (`git clone --depth 1`) dans `F:\Burnout Legends\refs\` :

- `thug-vita-port` (THUG 1, noté **T1**)
- `thug2-vita` (THUG 2, noté **T2**)

Les deux ports partagent le même backend (`Code/Gfx/Vita/`, `vita/`). T2 est une version plus récente du même code. Quand rien n'est précisé, le constat vaut pour les deux.

Rappel de contexte :

- THUG part du **code source** du moteur (kisak-thug, PC). Le backend Vita voit les maillages, les matériaux et la caméra.
- Ton projet part du **binaire PSP**. Le renderer ne voit qu'un flux de commandes GE.
- THUG vise **60 fps**. Burnout tourne à **30 fps** sur PSP, et ton renderer vise ces 30 fps (commentaire de `update_dynamic_resolution`, `ge_gl.cpp:3716`).

Légende des chemins :

- `T1:` = `refs/thug-vita-port/`
- `T2:` = `refs/thug2-vita/`
- `host/` = `PSPRecomp/profiles/burnout/host/`

---

## 1. Tableaux point par point

### 1.1 Initialisation de vitaGL (mémoire, pools, MSAA, résolution, vsync)

| Sujet | THUG Vita | Mon projet | Écart |
|---|---|---|---|
| Appel d'init | `vglInitExtended(0, 960, 544, 6 Mo, SCE_GXM_MULTISAMPLE_NONE)` dans `vita_vgl_init_tot()` (`T1:Code/Gfx/Vita/p_nx.cpp:563`, `T2:…/p_nx.cpp:589-592`). T2 utilise `vglInitWithCustomThreshold` quand il joue son logo vidéo, pour laisser le phycont à sceAvPlayer. | `vglInitWithCustomSizes(0, 960, 544, ram, cdram, phycont, 0, MSAA_NONE)` dans `create_context()` (`host/gl_context_vita.cpp:60`). Tailles calculées depuis `sceKernelGetFreeMemorySize` : ram ≤ 32 Mio, cdram = libre − 4 Mio, phycont ≤ 16 Mio (l. 28-30). | Approche équivalente. Ton dimensionnement est plus explicite. |
| Pool legacy (glBegin/glEnd) | 0 (premier argument). Commentaire : « vertex arrays uniquement » (`T1:p_nx.cpp:102-104`). | 0 aussi (`gl_context_vita.cpp:60`). | Identique. |
| Anneaux de commandes GXM | Agrandis avant l'init : `vglSetVertexBufferSize(8 Mo)`, `vglSetFragmentBufferSize(2 Mo)`, `vglSetVDMBufferSize(512 Ko)`, `vglSetParamBufferSize(32 Mo)` (`T1:p_nx.cpp:559-562`). Raison mesurée : l'anneau de sommets déborde avec les uniformes d'os, et sceGxm attend le GPU au milieu des dessins (« 40 à 450 ms/s pour un même nombre », l. 553-558). | Seul le param buffer est réglable (`set_param_buffer_mib`, `gl_context_vita.cpp:52-57`), et `kParamBufferMiB = 0` (`main_vita.cpp:126`) garde le défaut 16 Mio. Les anneaux sommets / fragments / VDM ne sont **pas** modifiés. | **Écart notable.** Tes dessins envoient beaucoup d'uniformes (`u_lighting[30]`, 3 matrices, ~20 slots : `ge_gl.cpp:56-72`, `sync_uniforms` l. 4012). Le même débordement est possible. Non mesuré chez toi. |
| Scènes par render target | Non trouvé (aucun appel à `vglSetupRenderTargetScenesNum`). | `vglSetupRenderTargetScenesNum(1, 8)` (`gl_context_vita.cpp:44`), car le jeu revient 5-6 fois par image sur son framebuffer. | Spécifique à ton cas (multi-passes du jeu PSP). |
| Mémoire cachée | `vglUseCachedMem` jamais appelé (commentaire `T1:p_world_render.cpp:4353`). | Option `cached_gpu_memory`, désactivée par défaut (`boot_menu_vita.hpp:16`). | Même choix par défaut. |
| MSAA | Aucun (`SCE_GXM_MULTISAMPLE_NONE`). | Aucun. FXAA léger en post-traitement (`BURNOUT_FXAA 2`, `main_vita.cpp:86-103`). | Tu paies une passe plein écran ; THUG non. |
| Résolution | 960x544 fixe. Résolution dynamique : non trouvé. | 960x544, **résolution dynamique** jusqu'à 720x408 (`kDynamicRes`, `main_vita.cpp:124` ; `update_dynamic_resolution`, `ge_gl.cpp:3703`), avec la *valid region* de ton vitaGL patché. | Tu es en avance sur ce point. |
| Vsync | Activée : `vglSwapBuffers(GL_FALSE)` avec l'intervalle par défaut de vitaGL (1). Option 30 fps : `eglSwapInterval(0, 2)` dans `FixerCadence()` (`T1:p_nx.cpp:1406-1412`, `T2:p_nx.cpp:1593`). | `vglWaitVblankStart(GL_FALSE)` (`gl_context_vita.cpp:66`) : le swap n'attend pas la vblank. Le rythme vient de l'émulation (vblanks PSP, voir 1.8). | Philosophie opposée (voir 1.8). |
| Tas newlib | `_newlib_heap_size_user = 192 Mio`, `sceLibcHeapSize = 4 Mio`, pile principale 4 Mio (`T1:vita/src/vita_runtime.cpp`). | 208 Mio, pile principale 8 Mio (`main_vita.cpp:60-66`). | Comparable. |

### 1.2 Soumission de la géométrie

| Sujet | THUG Vita | Mon projet | Écart |
|---|---|---|---|
| VBO | Toute la géométrie du décor va en VBO `GL_STATIC_DRAW` **au chargement** (`T1:p_world_render.cpp:1731-1803`, `1929-2114`). | VBO statiques **par cache** : clé = pointeur invité + taille + hachage des index, hachage complet des sommets à chaque image, éviction à 300 images ou au-delà de 24 Mio (`StaticGeometry`, `draw_body`, `ge_gl.cpp:4062-4220`). Les autres dessins sont copiés dans la mémoire *scratch* de vitaGL (`vglAllocFromScratch` + `vglBufferData`, l. 4240-4256). | Même idée, adaptée au flux GE. Le hachage par image a un coût CPU que THUG n'a pas. |
| Fusion des dessins | Maillages fusionnés en « lots » par clé d'état complète (`g_vita_lots`, `T1:p_world_render.cpp:507-525`). Mesure : 4437 maillages → 1189 lots, « monde 22,0 → 13,8 ms ». Lots multi-passes, z-bias et billboards aussi (l. 895-940). | Regroupement des PRIM consécutifs de même état dans un lot (`g_batch_runs`, `ge_gl.cpp:701-706`). Fusion des *cut-outs* en espace monde : test, désactivé (`kMergeCutouts`, `main_vita.cpp:118-119`). | THUG fusionne au chargement, avec la connaissance de la scène. Toi, tu ne peux fusionner que ce que le flux GE rend consécutif. |
| Appels de dessin par image | Avant fusion : 2374 appels, 7,5 µs chacun (`T1:p_world_render.cpp:509-512`). Après : 500 à 1300 lots en zone dense (`T1:p_shader_decor.cpp:1644-1646`). | ~450 dessins par image (commentaire `ge_gl.cpp:1916`). Compteur « draws/frame » dans le rapport (l. 6300). | Ton nombre d'appels est déjà bas. |
| Chemin de dessin | **GXM direct** pour les lots du décor, actif par défaut (`g_vita_gxm_direct = true`, `T1:p_shader_decor.cpp:1698`). États fragment et dessins **précalculés** (`sceGxmSetPrecomputedFragmentState`, `sceGxmDrawPrecomputed`, l. 2095-2102 ; `g_vita_gxm_pre = true`, `p_world_render.cpp:3540`). Raison : un dessin vitaGL coûte ~15 µs CPU (l. 1644-1651). | Tout passe par vitaGL : `glVertexAttribPointer` ×4 puis `glDrawElements` à chaque dessin (`ge_gl.cpp:4262-4284`). | **Écart principal sur le coût CPU par dessin.** |
| Skinning | Sur GPU par défaut (`g_vita_skin_gpu = true`, `T1:p_NxModel.cpp:209` ; `ShaderPeau*`, `p_shader_decor.cpp:741`). Mesure : 2,3 ms de skinning CPU évités. | Sur CPU, dans le thread de décodage, avec un cache par adresse de sommets (`skin`, `g_skin_cache`, `ge_gl.cpp:3065-3260`). | Applicable chez toi (voir section 2). |

### 1.3 Shaders

| Sujet | THUG Vita | Mon projet | Écart |
|---|---|---|---|
| Principe | Shaders **générés** par combinaison de matériau (jusqu'à 4 passes), comme le moteur Xbox. Un shader « uber » a été mesuré : 16,7 → 41-46 ms par image, GPU saturé (`T1:p_shader_decor.cpp:283-289`). | Un shader uber, plus des **variantes** par clé de 28 bits (`variant_defines`, `use_program`, `ge_gl.cpp:3924-3990`). Repli sur l'uber si une variante échoue. | Même conclusion des deux côtés : des variantes plutôt qu'un uber. |
| Nombre de variantes | Table de 512 variantes au plus, jamais vidée (`MAX_VARIANTES`, `T1:p_shader_decor.cpp:333-336`). Recherche par table de hachage (l. 443-454). Environ 60 variantes dans New Jersey (l. 97). | 41 clés connues dans `kKnownVariants` (`ge_gl.cpp:3992-4001`), plus celles notées dans `shader_variants.txt`. Recherche par `unordered_map`. | Ordres de grandeur comparables. |
| Moment de compilation | Au **chargement du niveau** : `ShaderMateriauPrecompiler()` pour chaque lot (`T1:p_world_render.cpp:2313-2335`, `p_shader_decor.cpp:636-637`). Sans cela, la première apparition gelait une image 160-180 ms. | Au **démarrage** : `kKnownVariants` et `compile_remembered_variants()` (`ge_gl.cpp:4003`, appel l. 5363). Une variante nouvelle est compilée en course, puis notée dans `ux0:data/burnout/shader_variants.txt` pour le lancement suivant (`remember_variant`, l. 3943). | Toi : tout au démarrage, plus un apprentissage. THUG : par niveau, car il connaît les matériaux à l'avance. |
| Cache sur la carte mémoire | **Par shader** : binaire GXP via `vglGetShaderBinary` / `glShaderBinary`, nommé par un hachage FNV du source, dans `ux0:data/thug/shd/` (`T1:p_shader_decor.cpp:95-170`). Toujours actif. Mesure : 270-340 ms par variante sans cache (17 s pour 60 variantes). | **Par programme** : `glGetProgramBinary` / `glProgramBinary`, clé = hachage des sources + defines + `__DATE__ __TIME__` (`cache_path`, `load_cached`, `store_cached`, `ge_gl.cpp:969-1036`). **Désactivé par défaut** (`shader_cache{}`, `boot_menu_vita.hpp:15`). Un crash au rechargement de programmes de filtres est noté (`main_vita.cpp:559-561`). | THUG met en cache le **shader** et relinke. Toi, le **programme** lié, et ce chemin a déjà planté. |
| Compilateur | vitaShaRK + `libshacccg.suprx` (`T1:vita/CMakeLists.txt:319-327`). | Pareil (`profiles/burnout/CMakeLists.txt:135`). | Identique. |
| Discard | Le `discard` n'apparaît que si le matériau a un seuil alpha, car il empêche le rejet précoce (`T1:p_shader_decor.cpp:287-289`). | Même règle : pas de discard quand l'alpha minimal de la texture et des sommets ne peut pas échouer au test (`g_batch_min_alpha`, `ge_gl.cpp:697-702` ; ~38 ms GPU mesurés). | Même technique, déjà en place chez toi. |

### 1.4 Textures

| Sujet | THUG Vita | Mon projet | Écart |
|---|---|---|---|
| Formats source | Fichiers Xbox : DXT1, DXT5, 8 bits palettisé, 16 bits, 32 bits (`T1:Code/Gfx/Vita/p_NxTexture.cpp`). | Formats PSP : 16 bits (5650 / 5551 / 4444), 8888, CLUT4/8/16/32 (`texture_bits`, `color16`, `clut_color`, `ge_gl.cpp:1291-1328`). DXT : « not used by this game so far » (l. 1535). | Formats imposés par les données de chaque jeu. |
| Format envoyé au GPU | **DXT1 gardé compressé** (`glCompressedTexImage2D`, `T1:p_NxTexture.cpp:647-656`). DXT5 décodé en RGBA8 par logiciel, car vitaGL perdait l'alpha (l. 276-285). T2 : DXT5 natif en option (`thaw_dxt5`, `T2:p_NxTexture.cpp:858-864`). Le reste en RGBA8. | **Tout en RGBA8888** (`Op::TexImage`, `Op::TexImageIndexed`, `ge_gl.cpp:5240-5276`). Un chemin palettisé `GL_COLOR_INDEX8_EXT` + `glColorTable` existe, seulement en mode sonde (`Op::TexImageP8`, l. 5278-5284 ; `upload_mode == 1`, l. 1599). | THUG économise la bande passante sur la majorité du décor (DXT1). Toi, chaque texel coûte 4 octets, même ceux qui en faisaient 0,5 ou 1 sur PSP. |
| Mipmaps | T1 : désactivés par défaut (`g_vita_mip = 0`, `T1:p_NxTexture.cpp:64`) car le chemin compressé avec mips plante vitaGL. T2 : `g_vita_mip = 1` (bilinéaire + mip le plus proche), chaîne régénérée par `glGenerateMipmap` pour DXT5/ARGB ; DXT1 au niveau 0 seul (`T2:p_NxTexture.cpp:84-92`, `680-700`). | Activés par défaut (`g_mipmaps`, `ge_gl.cpp:898`) via `glGenerateMipmap`, à partir du niveau 0, sauf pour les textures réécrites souvent (`ge_gl.cpp:1546-1560`). Trilinéaire désactivé (`kTrilinear`, `main_vita.cpp:89-93` : +10-16 ms GPU mesurés). Pas de mips pour les PNG non puissance de deux (l. 4779-4782). | Approche proche de T2. |
| Moment du chargement | Au **chargement du niveau** (`p_scene_load.cpp`, `p_NxTexture.cpp`). | **Au premier bind** dans l'image, puis vérification par hachage à chaque image (`bind_texture`, `probe_hash`, `fnv`, `ge_gl.cpp:1330-1366`). Les textures réécrites à chaque image (film du menu) tournent dans un anneau de 6 textures (`TextureRing`, l. 3786-3800). | Imposé par la recomp : le jeu PSP charge ses textures quand il veut. |

### 1.5 Changements d'état GL redondants

| Sujet | THUG Vita | Mon projet | Écart |
|---|---|---|---|
| États fixes | Chemin GXM direct : on ne change le programme que si la variante change (`T1:p_shader_decor.cpp:1648-1651`). `g_vita_vpr = true` : programme de sommets partagé, re-poses sautées (l. 1745). | Cache côté enregistrement : `set_capability`, `set_blend_func`, `set_depth`, `set_stencil`, `set_color_mask`, `bind_texture_object`, `set_sampling` (`ge_gl.cpp:1130-1215`). `glUseProgram` seulement si le programme change (`use_program`, l. 3985-3988). | Équivalent. |
| Uniformes | Dernières valeurs gardées (`c_pose`, `fog_pose`), renvoi seulement si elles changent (`T1:p_shader_decor.cpp:325-331`). | Versions par slot et par programme (`sync_uniforms`, `ge_gl.cpp:4012-4023` ; `uniform_changed`, l. 1751). | Équivalent. |
| Tri pour limiter les changements | Lots opaques triés par clé de variante puis texture, une fois au chargement (`g_vita_tri_lots`, `cmp_ordre_lots`, `T1:p_world_render.cpp:937-958`). | Pas de tri par programme. Test de réordonnancement des *cut-outs* : désactivé (`kReorder`, `main_vita.cpp:117`). | Voir section 2 : limité par l'ordre du flux GE. |

### 1.6 Culling et tri avant le GPU

| Sujet | THUG Vita | Mon projet | Écart |
|---|---|---|---|
| Frustum culling | CPU, par maillage, contre les plans du tronc (`g_vita_cull = true`, `T1:p_world_render.cpp:278`). Un seul parcours par image (`g_vita_prepasse`, l. 485-505 : ~11 ms de coût fixe supprimés). Culling par plage dans les lots (`g_vita_cull_plages`, l. 3538). | Émulation du culling du jeu : commande GE `BBOX` testée sur CPU (`bbox_visible`, `ge_gl.cpp:5658-5700`), puis saut `BJUMP` (`g_cull`, l. 5702-5713). | THUG fait son propre culling. Toi, tu exécutes celui que le jeu PSP a déjà prévu. |
| Occlusion | Module présent, désactivé (`g_vita_occlusion = false`, `T1:p_occlusion.cpp:37`). | Non trouvé. | – |
| Tri en profondeur | Translucides marqués « triés » rangés du plus loin au plus proche (`g_vita_tri_profondeur`, `T1:p_world_render.cpp:474-483`). | Ordre du flux GE conservé (le jeu PSP trie déjà). | Pas applicable tel quel. |
| Réduction de charge | Particules sur GPU (`g_vita_particules = 4`), budgets de particules (`T1:p_NxNewParticle.cpp:71`, `284-285`). | Éclaircissement du feuillage lointain (`kThinFoliage`, désactivé) ; budget de dessins en test (`kDrawBudget = 0`) (`main_vita.cpp:121`, `134`). | – |

### 1.7 Threads

| Thread | THUG Vita | Mon projet |
|---|---|---|
| Principal | Logique + soumission du rendu. Affinité : non trouvé. | Émulation (code recompilé), épinglé au cœur 0 (`main_vita.cpp:505`). |
| Rendu, partie CPU | `thug_listes` : construction des listes de dessin (culling) sur les cœurs 1-2, priorité `0x10000100 - 0x20` (au-dessus des threads audio), pile 64 Ko (`T1:p_world_render.cpp:3142-3190`). Actif par défaut. | `decode` (front-end GE asynchrone, `async_ge` vrai par défaut) : pthread, pile 4 Mio, cœur 1 (`worker_entry`, `ge_render.cpp:479-519`). |
| Soumission GPU | `thug_lots` : enregistrement des lots opaques dans une liste de commandes GXM différée, cœurs 1-2 (`T1:p_shader_decor.cpp:2863-2925`). **Désactivé par défaut** (`g_vita_lots_mt = false`, l. 2871 ; `g_vita_listes_gxm = false`, l. 2736). | `gl` : rejoue les images enregistrées, cœur 2, 2 images en file au plus (`gl_thread_entry`, `submit_frame`, `ge_gl.cpp:5425-5450`). |
| Audio | `thug_sfx` (mixeur) priorité **64** (la plus haute en utilisateur), grain 512 (`T1:Code/Gel/SoundFX/Vita/p_sfx.cpp:487-494`). `thug_pcm` et `thug_voix` : priorité `0x10000100` (`p_music.cpp:882`, `1042`). Affinité : aucune (masque 0). | Sortie audio priorité 64, grain 512, port BGM (`audio_out.cpp:30`, `81`, `130-145`). Mixeur SAS priorité 80 (`sas_worker.cpp:60`). Cœur 2 par défaut (`kSoundCore2`, `main_vita.cpp:137`, `489`). |
| Autres | Chargement : synchrone (non trouvé de thread de chargement). T2 : thread de barre de chargement (`T2:p_nx_managers.cpp:820`). Serveur de debug TCP. | Échantillonneur de phases (cœur 2, `ge_render.cpp:527`), lecture de musique (`music.cpp:132`), préchargement des films et packs de textures en priorité basse sur le cœur 2 (`detached_thread.hpp:16-35`), `keep_awake`. |

### 1.8 Rythme d'image et pas de temps

| Sujet | THUG Vita | Mon projet | Écart |
|---|---|---|---|
| Cible | 60 fps. 30 fps en option : `eglSwapInterval(0, 2)` (`FixerCadence`, `T1:p_nx.cpp:1404-1412`). | 30 fps du jeu PSP. | – |
| Qui cadence | La vsync de `vglSwapBuffers`. L'attente active du moteur sur le compteur de vblanks est retirée (`T1:Code/Gel/MainLoop/Mainloop.cpp:483-494`). | L'émulation : à chaque vblank PSP, attente jusqu'à l'heure prévue (59,94 Hz). Remise à zéro si on a plus de 500 ms de retard (`burnout_run.cpp:129-140`). Le swap n'attend pas la vblank (`gl_context_vita.cpp:64-66`). | THUG s'aligne sur l'écran. Toi, sur une horloge murale. |
| Pas de temps | Durée d'image en vsync **fractionnaires**, lissée sur 4 images comme le moteur Xbox, plafonnée à 4 vsync (`OncePerRender`, `T2:Code/Sys/Vita/p_timer.cpp:116-180`). | Le jeu PSP gère son propre pas de temps à partir des vblanks émulés. | Le pas de temps n'est pas à toi : c'est le code du jeu. |
| Images en retard | Image > 50 ms tracée avec sa ventilation (`[ACC]`, voir 1.10). | Résolution dynamique ; saut d'image en option (`frame_skip`, `skip_when_behind`, tous deux faux par défaut, `boot_menu_vita.hpp:13-18`). | – |

### 1.9 Options de build et horloges

| Sujet | THUG Vita | Mon projet | Écart |
|---|---|---|---|
| CPU / FPU | `-mfpu=neon -mcpu=cortex-a9` partout (`T1:vita/CMakeLists.txt:86-93`). | Mêmes options via `CMAKE_CXX_FLAGS` dans la commande de build (`profiles/burnout/README.md:60`). | Identique. |
| Niveau d'optimisation | **Pas d'optimisation globale** : le moteur plante en -O1/-O2. -O2 fichier par fichier sur les fichiers chauds, validé lot par lot (`THUG_HOT_SOURCES`, `T1:vita/CMakeLists.txt:110-228`). Mesure du lot 1 : 96,5 → 42,9 ms par image. | Code généré : -O3 (`PSPRECOMP_GENERATED_OPT_LEVEL=3`) + options anti-pression de registres (`-fno-gcse`, `-fira-loop-pressure`, …) (`README.md:60` ; `profiles/burnout/CMakeLists.txt:28-35`). Hôte : -O2 (`CMAKE_CXX_FLAGS_RELEASE`). | Tu es déjà optimisé partout. |
| -ffast-math | Global, mais **annulé** (`-fno-fast-math`) sur les fichiers en -O2, car il a cassé le culling (`T1:vita/CMakeLists.txt:120-135`). | Non. Seulement `-fno-math-errno` sur la cible GL (`profiles/burnout/CMakeLists.txt:131-132`). | Prudence comparable. |
| LTO | Non trouvé. | Option `PSPRECOMP_LTO`, OFF par défaut (`CMakeLists.txt:21`, `85-95`) ; absente de la commande Vita du README. | Rien à copier. |
| vitaGL | Celui du vitasdk, ou une version recompilée « avec options de vitesse » (`THUG_VITAGL_DIR`, `T1:vita/CMakeLists.txt:291-298`). Les options exactes : non trouvé. | vitaGL 2bdbe89 recompilé avec ton patch (valid region, depth keep, stats de scènes) (`tools/vitagl-2bdbe89-valid-region.patch`). Options de compilation de vitaGL : non trouvé dans le dépôt. | Comparable. |
| Horloges | ARM 444, bus 222, GPU 222, crossbar 166, au démarrage (`T1:Code/Sk/Main.cpp:295-298`). Commande `clk ARM GPU` pour changer à chaud (`T1:Code/Sys/SIO/Vita/p_siodev.cpp:2402-2422`). | Mêmes valeurs (`run_pass`, `main_vita.cpp:243-248`). | Identique, sauf le changement à chaud. |
| Overclock au-delà de 444 | Non (commentaire : il faudrait un plugin). | Non. | – |

### 1.10 Outils de mesure et de test

| Outil | THUG Vita | Mon projet |
|---|---|---|
| Cycle build → console | `make send` : tue l'app (`destroy` sur le port 1338 de vitacompanion), envoie seulement `eboot.bin` par FTP (`curl -T`, port 1337), relance l'app. `make kill`. IP dans `vita/tools/vita_ip.txt` (`T1:vita/CMakeLists.txt:396-426`). | Non trouvé dans le dépôt (aucun script FTP ou vitacompanion). Envoi du VPK à la main. |
| Pilotage à distance | Serveur TCP port 9990 (`T2:vita/src/vita_dbgsrv.c`) : `state` (image, fps, temps), `shot` / `burst N` (captures zlib depuis la mémoire), `hold MS touches` (manette et sticks injectés), `frames N`, `idle`, `log POS` (lecture du journal). Le client PC (`vctl.py`) n'est **pas** publié (`T2:docs/building.md`, « Development loop »). | Script d'entrées par vblank (`race.txt`, `scripts/race_*.txt`, `kDefaultScript`, `main_vita.cpp:149-161`, `522-533`). Captures aux vblanks choisis et combo L+R+BAS (`main_vita.cpp:290-296`, `540-548`). |
| Réglages à chaud | Fichier `ux0:data/thug/inject.txt` ou commandes TCP : codes de 3 lettres pour basculer chaque optimisation sans recompiler (`gxd 0/1`, `fus 0/1`, `mip N`, `clk`, `fps 30/60`…) (`T1:Code/Sys/SIO/Vita/p_siodev.cpp:72`, `160`). Mesures A/B à chaud. | Interrupteurs `constexpr` dans `main_vita.cpp:112-139` (un build par essai) ; variables d'environnement sur PC seulement. |
| Ventilation d'image | `[ACC]` : horodatage de chaque étape de l'image ; toute image > 50 ms est journalisée par étape (logique, ombre, monde, 2D, swap…) (`T1:p_nx.cpp:643-665`). `[PERF]` toutes les 5 s. | Rapport toutes les 300 images : thread GL (dessins, upload, état, present), géométrie statique, compteurs GPU, cibles (`ge_gl.cpp:6260-6380`). Charge par cœur (`core_load.hpp`). Phases échantillonnées (`ge_render.cpp:473-477`). |
| Profileur | Instrumentation GCC `-finstrument-functions` sur les fichiers suspects (`vita_prof.c`, `-DTHUG_PROFILE=ON`). Défaut signalé par l'auteur : pile globale, faux en multithread. | Échantillonneur des étiquettes de blocs du code généré, sur le cœur 2 (`BURNOUT_VITA_PROFILE`, `main_vita.cpp:165-205`, `508-512`). |
| Espions | `--wrap` sur `glClear`, `glBindFramebuffer`, `glFinish`, `glReadPixels`… et sur `sceGxmDraw`, `sceShaccCgCompileProgram` (T2) (`vita_glspy.c`). Traceur d'allocations par appelant et par niveau (`vita_memspy.c`). | `--wrap` sur malloc/free/calloc/realloc/memalign (`alloc_stats_vita.cpp`, `profiles/burnout/CMakeLists.txt:113-115`). Sondes GPU par modes (`kGpuProbe`, `kAllProbe`, `kProbeSet`). |
| Journal | Double canal : `sceClibPrintf` + fichier `ux0:data/thug/thug.log` avec tampon 16 Ko et anneau de 2 Mo pour le serveur (`vita_log.c`). Chien de garde : plantage volontaire après 45 s sans log (build de dev). | Un log par build, `stderr` non bufferisé, ancien log gardé en `.prev` (`main_vita.cpp:465-472`). |

---

## 2. Techniques applicables, classées

### 2.1 Classement

Gain : estimation qualitative, **à mesurer**. Rien ici n'a été mesuré sur ton projet.

| # | Technique (source THUG) | Gain attendu | Difficulté | Pourquoi chez toi |
|---|---|---|---|---|
| 1 | **Agrandir les anneaux GXM** : `vglSetVertexBufferSize(8 Mo)`, `vglSetFragmentBufferSize(2 Mo)`, `vglSetVDMBufferSize(512 Ko)`, avant `vglInitWithCustomSizes` (`T1:p_nx.cpp:553-562`). | Moyen, possiblement fort | Très faible (4 lignes dans `create_context`) | Tes uniformes par dessin sont lourds (`u_lighting[30]` + matrices). Si l'anneau de sommets déborde, sceGxm attend le GPU au milieu de l'image. Coût : quelques Mo de RAM. À tester en A/B avec ton rapport « GL thread ». |
| 2 | **Réglages à chaud par fichier ou TCP** au lieu de `constexpr` (`inject.txt`, `p_siodev.cpp`). | Fort sur la vitesse de travail, pas sur les fps | Faible | Tes ~25 interrupteurs (`main_vita.cpp:112-139`) coûtent chacun un build et un envoi. Un fichier lu à chaque seconde permettrait l'A/B dans la même session et la même scène. |
| 3 | **Chemin GXM direct pour les dessins en VBO statique** (`p_shader_decor.cpp:1642-2100`) : `sceGxmSetVertexProgram` / `SetFragmentProgram` / `sceGxmDraw` sans repasser par vitaGL ; états précalculés pour ce qui ne change pas. | Fort sur le thread GL (THUG : ~15 µs → quelques µs par dessin) | Élevée (cohabitation avec l'état de vitaGL, `scene_reset`, invalidation des uniformes) | À n'envisager que si le thread GL (cœur 2) limite. Ton CMake dit que c'est le thread de **décodage** qui limite (`profiles/burnout/CMakeLists.txt:129-130`). Vérifier d'abord avec `core_load`. |
| 4 | **Skinning sur GPU** (`ShaderPeau*`, `p_shader_decor.cpp:741-910`) : sommets de repos, poids et index en VBO statique ; seules les matrices d'os changent. | Moyen (THUG : 2,3 ms CPU évités) | Moyenne | Ton skinning tourne sur le thread de décodage, celui qui limite. Les matrices d'os sont dans les commandes GE `BONE`. Limite : jusqu'à 8 poids par sommet sur PSP, et beaucoup d'uniformes (d'où le point 1). Commencer par lire `skin_ms` dans ton rapport (`ge_gl.cpp:807`). |
| 5 | **Cache de shaders par shader** (`vglGetShaderBinary` / `glShaderBinary`, `p_shader_decor.cpp:95-170`) au lieu de `glProgramBinary`. | Fort au chargement et sur les gels de première apparition ; nul en régime établi | Faible | Ton cache de programmes est éteint et a déjà planté. Le format de THUG relinke à chaque lancement, ce qui évite de recharger un programme lié. Clé : hachage du source + defines. |
| 6 | **Textures 16 bits ou palettisées côté GPU** au lieu de tout en RGBA8888. | Moyen sur la bande passante GPU (2 à 8 fois moins d'octets par texel) | Moyenne | Pas une technique de THUG, mais la même idée que leur DXT1 gardé compressé. Tes textures PSP sont en 16 bits ou CLUT. Ton chemin `GL_COLOR_INDEX8_EXT` existe déjà en mode sonde. Le support exact des formats 16 bits par ton vitaGL est à vérifier : non trouvé dans les deux dépôts. Attention au filtrage des textures palettisées. |
| 7 | **Traceur d'à-coups `[ACC]`** (`p_nx.cpp:643-665`) : horodatage des étapes ; seules les images > 50 ms sont journalisées. | Indirect (diagnostic) | Très faible | Ton rapport fait des moyennes sur 300 images. Un à-coup isolé (compilation de variante, upload de texture) s'y dilue. |
| 8 | **Priorité des workers au-dessus du défaut** (`0x10000100 - 0x20`, `p_world_render.cpp:3172-3180`). THUG a mesuré des images de 600 ms quand un thread de même priorité tournait sur le même cœur. | Faible à moyen (à-coups) | Très faible | Ton thread GL partage le cœur 2 avec l'échantillonneur, la musique, le préchargement et parfois le son. Vérifier les priorités de `decode` et `gl` (pthread par défaut). |
| 9 | **Changement d'horloge à chaud** (`clk 333 111`, `p_siodev.cpp:2402-2422`). | Diagnostic : CPU ou GPU limitant ? | Très faible | Si baisser le GPU ne change rien, le GPU ne limite pas. Utile avec le point 2. |
| 10 | **Tri des opaques par variante** (`g_vita_tri_lots`, `p_world_render.cpp:937-958`). | Faible | Élevée chez toi | Seulement pour des opaques contigus, même cible, test de profondeur actif, sans effet de bord sur le stencil. Ton test `kReorder` va dans ce sens. |
| 11 | **Swap avec vsync, intervalle 2** (`eglSwapInterval(0, 2)`). | Régularité de l'image à 30 fps, pas de fps en plus | Faible à essayer | Aujourd'hui l'émulation se cale sur une horloge murale et le swap n'attend pas. À tester seulement si des saccades de présentation sont visibles. Risque : attente sur le thread GL. |

### 2.2 Ce qui ne s'applique pas à une recomp

| Technique THUG | Pourquoi elle ne s'applique pas |
|---|---|
| Fusion des maillages en lots par grille, au chargement (`g_vita_lots`) | Le renderer ne voit ni le décor ni ses matériaux. Il voit des PRIM dans l'ordre du flux GE. Fusionner deux PRIM non consécutifs change l'ordre de dessin, donc le mélange et le stencil. |
| Frustum culling par maillage, liste construite sur un autre cœur (`thug_listes`) | Le graphe de scène n'existe pas dans le flux GE. Le seul culling visible est celui du jeu (`BBOX` / `BJUMP`), que tu exécutes déjà. Un culling en plus devrait deviner les limites des objets. |
| Précompilation des shaders par niveau (`ShaderMateriauPrecompiler`) | La recomp ne connaît pas les matériaux d'un niveau avant de les voir. Ton équivalent : `kKnownVariants` + `shader_variants.txt`. |
| Shaders générés par matériau Xbox (4 passes, modes de combinaison) | Ta clé de variante vient de l'état GE, pas d'un matériau. Le principe (variantes plutôt qu'uber) est déjà le tien. |
| Textures DXT1 compressées, chaînes de mips du fichier | Les données sont celles de la PSP : pas de DXT. Le format envoyé au GPU reste ton choix (point 6). |
| Tri des translucides en profondeur | Le jeu PSP émet déjà ses translucides dans son ordre. Les retrier changerait l'image. |
| Pas de temps lissé en vsync fractionnaires (`OncePerRender`) | Le pas de temps est calculé par le code du jeu à partir des vblanks émulés. |
| Optimisation -O2 fichier par fichier | Ton code généré compile déjà en -O3 sans plantage. Le problème de THUG (code de 2003 mal défini) n'est pas le tien. |
| Particules sur GPU, budgets de particules | Les particules arrivent comme des PRIM déjà calculés par le CPU PSP. |

---

## 3. Le harnais de test de THUG

### 3.1 Ce qu'il fait

D'après le README de T1 (section « How it was made ») et le code publié :

1. **Build et envoi rapide.** `make send` tue l'app, envoie seulement `eboot.bin` par FTP, puis la relance via vitacompanion (ports 1337 et 1338). Le VPK n'est installé qu'une fois (`T1:vita/CMakeLists.txt:396-426`).
2. **Pilotage.** Un serveur TCP dans le jeu (port 9990, `vita_dbgsrv.c`) reçoit des commandes texte d'une seule connexion persistante. Il remplace une boucle plus lente « inject.txt par FTP → BMP sur la carte → rapatriement FTP » (`vita_dbgsrv.h`).
   - `hold` injecte boutons et sticks pendant N ms.
   - `frames N` et `idle` attendent un état.
   - `shot` et `burst` renvoient des captures compressées (zlib), prises en mémoire juste avant le swap.
   - `log POS` lit le journal depuis une position.
   - Toute autre ligne passe à l'analyseur de `inject.txt` : réglages à chaud, déblocages, zoom, horloges.
3. **Mesures.** `[PERF]` toutes les 5 s, `[ACC]` pour les images longues, bilans `[2D]` sur 120 images, espions `--wrap` sur GL, GXM et malloc, profileur par instrumentation.
4. **Sécurité.** Chien de garde (plantage volontaire après 45 s sans log) et log flushé, pour lire le log même après un crash.
5. **Build public séparé.** `THUG_RELEASE` retire serveur, chien de garde, raccourcis et logs verbeux.

Le client PC (`vctl.py`) et les outils de comparaison avec xemu **ne sont pas publiés**. On ne connaît le protocole que par le code de la console.

### 3.2 Ce que tu pourrais reprendre

| Élément | Intérêt pour toi | Effort |
|---|---|---|
| `make send` / `make kill` (eboot seul + vitacompanion) | Un envoi en quelques secondes au lieu d'un VPK. Ton VPK porte aussi les données de la cible (`vita_create_vpk`, `profiles/burnout/CMakeLists.txt:192`) : seul `eboot_gl.bin` changerait. Il faut vitacompanion sur la console. Ta règle de nettoyage des anciens VPK reste valable pour les installations complètes. | Faible : une cible CMake `add_custom_target` avec `curl` et `nc` (ou PowerShell sous Windows). |
| Fichier de réglages lu à chaud | Remplace les `constexpr` de test de `main_vita.cpp`. Un essai = un fichier modifié, pas un build. | Faible à moyen. |
| Serveur TCP minimal (`state`, `shot`, `log`, `hold`) | Lire le log et capturer l'écran sans FTP. `hold` complète ton script `race.txt` pour des scènes reproductibles. | Moyen. Le code console est publié et lisible ; le client PC est à écrire. |
| Traceur `[ACC]` | Explique une image longue isolée. | Très faible. |
| Espion `--wrap` sur `sceGxmDraw` et `sceShaccCgCompileProgram` (T2) | Compte les dessins réels et voit chaque compilation de shader en cours de jeu. Tu as déjà le mécanisme `--wrap` pour malloc. | Faible. |
| Écriture du journal avec tampon 16 Ko + anneau | Ton `stderr` est non bufferisé (`main_vita.cpp:472`). THUG a mesuré qu'un flush par ligne coûte des accès carte. Ton propre commentaire note des gels de 150-190 ms dus à des écritures carte (`burnout_run.cpp:121-123`). | Faible, mais attention à ne pas perdre la fin du log en cas de crash. |

---

## 4. Points « non trouvé »

- THUG : options exactes de recompilation de vitaGL (`THUG_VITAGL_DIR`).
- THUG : `vglSetupRenderTargetScenesNum`, résolution dynamique, LTO, thread de chargement (T1).
- THUG : affinité du thread principal.
- THUG : client PC du serveur de debug (non publié).
- Mon projet : script d'envoi FTP / vitacompanion.
- Mon projet : options de compilation de ton vitaGL patché.
- Mon projet : culling par occlusion.
- Les deux : preuve que vitaGL accepte les textures 16 bits (5650 / 5551 / 4444) sans conversion.
