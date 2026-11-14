// The Bonus filters' names in the game's languages (upper case, no accents:
// as the game's own menu strings).
#include "filters.hpp"

#include <cstring>

namespace burnout::filters {
namespace {

struct Names {
    const char *id;
    const char *fr, *es, *de, *it, *nl;  // nullptr: the English name
};

constexpr Names kNames[]{
    {"bw", "NOIR ET BLANC", "BLANCO Y NEGRO", "SCHWARZWEISS", "BIANCO E NERO", "ZWART-WIT"},
    {"sepia", "SEPIA", "SEPIA", "SEPIA", "SEPPIA", "SEPIA"},
    {"noir", "FILM NOIR", "CINE NEGRO", "FILM NOIR", "NOIR", "NOIR"},
    {"vintage", "RETRO", "VINTAGE", "VINTAGE", "VINTAGE", "VINTAGE"},
    {"saturated", "SATURE", "SATURADO", "GESAETTIGT", "SATURO", "VERZADIGD"},
    {"gritty", "GRANULEUX", "ASPERO", "KOERNIG", "GRANULOSO", "KORRELIG"},
    {"arthouse", "CINEMA D'AUTEUR", "CINE DE AUTOR", "ARTHOUSE", "CINEMA D'AUTORE", "ARTHOUSE"},
    {"blockbuster", "SUPERPRODUCTION", "TAQUILLAZO", "BLOCKBUSTER", "KOLOSSAL", "KASKRAKER"},
    {"negative", "NEGATIF", "NEGATIVO", "NEGATIV", "NEGATIVO", "NEGATIEF"},
    {"rainbow", "ARC-EN-CIEL", "ARCOIRIS", "REGENBOGEN", "ARCOBALENO", "REGENBOOG"},
    {"res_colours", "COULEURS REDUITES", "COLORES REDUCIDOS", "WENIGE FARBEN", "COLORI RIDOTTI", "MINDER KLEUREN"},
    {"gb_colours", "COULEURS GB", "COLORES GB", "GB-FARBEN", "COLORI GB", "GB-KLEUREN"},
    {"painted", "PEINTURE", "PINTADO", "GEMALT", "DIPINTO", "GESCHILDERD"},
    {"shadow_puppets", "OMBRES CHINOISES", "SOMBRAS CHINESCAS", "SCHATTENSPIEL", "OMBRE CINESI", "SCHADUWSPEL"},
    {"glow_worm", "VER LUISANT", "LUCIERNAGA", "GLUEHWURM", "LUCCIOLA", "GLIMWORM"},
    {"mirror", "ROIRIM", "OJEPSE", "LEGEIPS", "OIHCCEPS", "LEGEIPS"},
    {"upside_down", "A L'ENVERS", "AL REVES", "KOPFUEBER", "SOTTOSOPRA", "ONDERSTEBOVEN"},
    {"cinematic", "FORMAT CINEMA", "FORMATO CINE", "KINOFORMAT", "FORMATO CINEMA", "BIOSCOOPFORMAAT"},
    {"tv_ratio", "FORMAT TELE", "FORMATO TV", "TV-FORMAT", "FORMATO TV", "TV-FORMAAT"},
    {"spotlight", "PROJECTEUR", "FOCO", "SCHEINWERFER", "RIFLETTORE", "SCHIJNWERPER"},
    {"dot_matrix", "MATRICE DE POINTS", "MATRIZ DE PUNTOS", "PUNKTMATRIX", "MATRICE A PUNTI", "PUNTMATRIX"},
    {"felt_tip", "FEUTRE", "ROTULADOR", "FILZSTIFT", "PENNARELLO", "VILTSTIFT"},
    {"comic", "BANDE DESSINEE", "COMIC", "COMIC", "FUMETTO", "STRIP"},
    {"gritty_comic", "BD GRANULEUSE", "COMIC ASPERO", "RAUER COMIC", "FUMETTO GRANULOSO", "RUWE STRIP"},
    {"cartoony", "DESSIN ANIME", "DIBUJOS ANIMADOS", "ZEICHENTRICK", "CARTONE ANIMATO", "TEKENFILM"},
    {"chameleon", "CAMELEON", "CAMALEON", "CHAMAELEON", "CAMALEONTE", "KAMELEON"},
    {"cel", "CEL SHADING", "CEL SHADING", "CEL SHADING", "CEL SHADING", "CEL SHADING"},
    {"newspaper", "JOURNAL", "PERIODICO", "ZEITUNG", "GIORNALE", "KRANT"},
};

} // namespace

std::string localized_name(int filter, int language) {
    if (filter < 0 || filter >= static_cast<int>(all().size())) return {};
    const Filter &f = all()[static_cast<std::size_t>(filter)];
    std::string english = f.name;
    if (const std::size_t film = english.find(" Film"); film != std::string::npos) english.erase(film, 5u);
    for (const Names &n : kNames) {
        if (std::strcmp(n.id, f.id) != 0) continue;
        const char *name = language == 2 ? n.fr : language == 3 ? n.es : language == 4 ? n.de : language == 5 ? n.it : language == 6 ? n.nl : nullptr;
        return name != nullptr ? name : english;
    }
    return english;  // consoles and proper names
}

} // namespace burnout::filters
