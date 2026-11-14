# Carte des fonctions du jeu

Les fonctions de Burnout Legends (ULES00125) que nous avons comprises, et
celles réécrites à la main en C (`host/native_funcs.cpp`, branchées par
`tools/patch_native.py`).

- **Coût** : part du temps du code du jeu dans la mesure Vita du
  2026-11-12 (Road Rage, Waterfront, `burnout-mesure-profile.txt`,
  `tools/profile_report.py`).
- **États** : comprise, réécrite, vérifiée PC (mode `PSPRECOMP_BURNOUT_NATIVE_CHECK` :
  aucune différence sur une course), testée Vita (en jeu, gain mesuré).

| Adresse | Nom | Ce qu'elle fait | Coût | État |
|---|---|---|---|---|
| 0x08A3C868 | `collision_sphere_decor` | Hitbox sphère d'une voiture (centre, rayon) contre les triangles du décor. Boîte de la sphère contre la zone de l'arbre du circuit (min/max), puis pour chaque triangle : 3 coins lus (n° sur 1 octet, positions en entiers 16 bits × échelle + décalage), normale (produit vectoriel, normalisée), distance sphère-triangle. Commence par appeler 0x08A3B7E8. | 4,5 % | comprise |
| 0x08A3FCD4 | `boite_visible` | Une boîte (8 coins) est-elle dans le champ de vision ? 0 dehors, 1 en partie, 2 dedans. | 1,2 % (2026-11-08) | réécrite (2026-11-09, commit 5e3964b), vérifiée PC |

## À identifier (les plus coûteuses de la mesure)

| Adresse | Coût | Notes |
|---|---|---|
| 0x088486D8 | 3,8 % | |
| 0x0897FD60 | 3,7 % | |
| 0x0884AB9C | 2,9 % | |
| 0x08A3D2DC | 2,7 % | voisine de `collision_sphere_decor` |
| 0x08A99388 | 2,0 % | |
| 0x08A998FC | 2,0 % | |
| 0x08A3EC60 | 1,8 % | |
| 0x08978F18 | 1,7 % | |
| 0x0898076C | 1,5 % | |

`collision_voiture_voiture` (rivaux et circulation, takedowns) : pas encore trouvée.
