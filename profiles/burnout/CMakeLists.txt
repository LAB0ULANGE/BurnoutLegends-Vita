set(BURNOUT_PROFILE_DIR "${CMAKE_CURRENT_SOURCE_DIR}")

# The generated corpus is a translation of the commercial executable and is
# never committed (see .gitignore); regenerate it locally with psp_recomp.
set(BURNOUT_GENERATED_DIR "${BURNOUT_PROFILE_DIR}/generated" CACHE PATH
    "Directory holding the psp_recomp --auto output for this profile")
file(GLOB BURNOUT_GENERATED CONFIGURE_DEPENDS "${BURNOUT_GENERATED_DIR}/*.cpp")
if(NOT BURNOUT_GENERATED)
    message(WARNING "profiles/burnout/generated is empty; run psp_recomp --auto first")
endif()

set(BURNOUT_GENERATED_INLINE_LEVEL "0" CACHE STRING
    "MSVC /Ob level for the generated burnout units (0 keeps compile time low)")
if(NOT BURNOUT_GENERATED_INLINE_LEVEL MATCHES "^[0-3]$")
    message(FATAL_ERROR "BURNOUT_GENERATED_INLINE_LEVEL must be 0, 1, 2 or 3")
endif()

if(MSVC)
    if(PSPRECOMP_GENERATED_OPT_LEVEL STREQUAL "0")
        set(BURNOUT_GENERATED_MSVC_OPT "/Od")
    elseif(PSPRECOMP_GENERATED_OPT_LEVEL STREQUAL "3")
        set(BURNOUT_GENERATED_MSVC_OPT "/Ox")
    else()
        set(BURNOUT_GENERATED_MSVC_OPT "/O${PSPRECOMP_GENERATED_OPT_LEVEL}")
    endif()
    set_source_files_properties(${BURNOUT_GENERATED} PROPERTIES COMPILE_OPTIONS
        "${BURNOUT_GENERATED_MSVC_OPT};/Ob${BURNOUT_GENERATED_INLINE_LEVEL};/bigobj;${PSPRECOMP_MSVC_MP_FLAG}")
else()
    # Extra flags for the generated units only (e.g. relaxed float math).
    set(BURNOUT_GENERATED_EXTRA_FLAGS "" CACHE STRING "Additional compiler flags for the generated units")
    set(BURNOUT_GENERATED_GCC_OPTIONS "-O${PSPRECOMP_GENERATED_OPT_LEVEL};-g0")
    if(BURNOUT_GENERATED_EXTRA_FLAGS)
        list(APPEND BURNOUT_GENERATED_GCC_OPTIONS ${BURNOUT_GENERATED_EXTRA_FLAGS})
    endif()
    set_source_files_properties(${BURNOUT_GENERATED} PROPERTIES COMPILE_OPTIONS "${BURNOUT_GENERATED_GCC_OPTIONS}")
endif()

add_library(burnout_generated STATIC ${BURNOUT_GENERATED})
target_include_directories(burnout_generated PRIVATE "${BURNOUT_GENERATED_DIR}")
target_link_libraries(burnout_generated PUBLIC psprecomp_core)
psprecomp_enable_host_avx(burnout_generated)

set(BURNOUT_HOST_SOURCES
    host/burnout_hle.cpp
    host/burnout_bench.cpp
    host/burnout_run.cpp
    host/workload_stats.cpp
    host/audio_out.cpp
    host/sas_core.cpp
    host/sas_worker.cpp
    host/music.cpp
    host/trax.cpp
    host/core_load.cpp
    host/movie.cpp
    host/save_dialog.cpp
    host/psp_savedata.cpp
    host/filters.cpp
    host/filter_names.cpp
    host/bonus.cpp
    host/paints.cpp
    host/texture_pack.cpp
    host/iso_reader.cpp
    host/game_setup.cpp
    host/bonus_menu.cpp
    host/native_funcs.cpp
    host/overlay.cpp
    host/perf_counters.cpp)

# Per-frame performance counters (host/perf_counters.hpp, BRIEF_PERF_60FPS.md
# step 0): a log of average, maximum and worst 1% over 600 frames. Off: no code.
option(BURNOUT_PERF_COUNTERS "Per-frame performance counters (burnout-perf.log)" OFF)
if(BURNOUT_PERF_COUNTERS)
    # Host sources only: switching it does not rebuild the generated code.
    set_property(SOURCE ${BURNOUT_HOST_SOURCES} host/ge_gl.cpp host/main_vita.cpp host/main_pc.cpp host/main_headless.cpp
                 APPEND PROPERTY COMPILE_DEFINITIONS PERF_COUNTERS=1)
endif()

# First-launch install from the player's disc image: PPSSPP's PRX decrypter
# and libkirk (GPL-2.0), and the FFmpeg ATRAC3plus decoder (LGPL) as
# extracted by PPSSPP (at3_standalone). Compiled with stand-in headers for
# PPSSPP's Common/ (host/third_party/ppsspp_shim).
enable_language(C)
file(GLOB BURNOUT_KIRK_SOURCES "${CMAKE_CURRENT_SOURCE_DIR}/host/third_party/libkirk/*.c")
file(GLOB BURNOUT_AT3_SOURCES "${CMAKE_CURRENT_SOURCE_DIR}/host/third_party/at3/*.cpp")
add_library(burnout_thirdparty STATIC ${BURNOUT_KIRK_SOURCES} ${BURNOUT_AT3_SOURCES}
    host/third_party/PrxDecrypter.cpp)
target_include_directories(burnout_thirdparty
    PUBLIC "${CMAKE_CURRENT_SOURCE_DIR}/host" "${CMAKE_CURRENT_SOURCE_DIR}/host/third_party/ppsspp_shim"
    PRIVATE "${CMAKE_CURRENT_SOURCE_DIR}/host/third_party/at3")
if(MSVC)
    target_compile_options(burnout_thirdparty PRIVATE /w)
    target_compile_definitions(burnout_thirdparty PRIVATE _CRT_SECURE_NO_WARNINGS)
else()
    target_compile_options(burnout_thirdparty PRIVATE -w -O2)
    target_compile_definitions(burnout_thirdparty PRIVATE _GNU_SOURCE M_PI=3.14159265358979323846 M_SQRT1_2=0.70710678118654752440 M_SQRT2=1.41421356237309504880)
endif()

if(VITA)
    # PS Vita: the same headless runtime packaged as a VPK that benchmarks the
    # scripted race and writes ux0:data/burnout-bench.txt.
    include("$ENV{VITASDK}/share/vita.cmake" REQUIRED)
    add_executable(burnout_vita host/main_vita.cpp ${BURNOUT_HOST_SOURCES})
    target_compile_definitions(burnout_vita PRIVATE BURNOUT_GENERATED_OPT="-O${PSPRECOMP_GENERATED_OPT_LEVEL}")
    # Sampling profiler: needs a corpus generated with PSPRECOMP_PROFILE_LABELS=1.
    option(BURNOUT_VITA_PROFILE "Sample the generated block addresses during the window" OFF)
    if(BURNOUT_VITA_PROFILE)
        target_compile_definitions(burnout_vita PRIVATE BURNOUT_VITA_PROFILE=1)
    endif()
    target_include_directories(burnout_vita PRIVATE "${BURNOUT_PROFILE_DIR}/host")
    target_link_libraries(burnout_vita PRIVATE burnout_thirdparty)
    target_link_libraries(burnout_vita PRIVATE burnout_generated
        SceDisplay_stub SceSysmem_stub ScePower_stub SceKernelThreadMgr_stub
        SceProcessmgr_stub SceLibKernel_stub SceAudio_stub SceVideodec_stub SceAppUtil_stub SceRtc_stub SceCommonDialog_stub SceSysmodule_stub pthread)
    vita_create_self(eboot.bin burnout_vita UNSAFE)
    vita_create_vpk(burnout-bench.vpk BRNB00001 eboot.bin
        VERSION 01.00
        NAME "Burnout recomp bench")

    # Rendering build: the same runtime with the GE translated to vitaGL, the
    # real pad and real-time pacing. Separate title so the bench stays intact.
    option(BURNOUT_VITA_GL "Also build the vitaGL rendering VPK" ON)
    if(BURNOUT_VITA_GL)
        add_executable(burnout_vita_gl host/main_vita.cpp host/boot_menu_vita.cpp host/ge_render.cpp host/ge_gl.cpp
            host/gl_context_vita.cpp host/alloc_stats_vita.cpp ${BURNOUT_HOST_SOURCES})
        # Memory diagnostics: every malloc/free counted (alloc_stats_vita.cpp).
        target_link_options(burnout_vita_gl PRIVATE
            -Wl,--wrap=malloc -Wl,--wrap=free -Wl,--wrap=calloc -Wl,--wrap=realloc -Wl,--wrap=memalign)
        # A second build (other generated-code flags) installs next to this
        # one under its own title ID; the label shows on the launch screen.
        set(BURNOUT_VITA_GL_TITLE_ID "BRNG00001" CACHE STRING "Title ID of the rendering VPK")
        set(BURNOUT_VITA_GL_NAME "Burnout Legends 2.0" CACHE STRING "LiveArea name of the rendering VPK")
        set(BURNOUT_VITA_GL_LABEL "" CACHE STRING "Build label on the launch screen (empty: none)")
        set(BURNOUT_VITA_GL_FILES "burnout-gl" CACHE STRING "Report and log names under ux0:data/")
        target_compile_definitions(burnout_vita_gl PRIVATE
            BURNOUT_GENERATED_OPT="-O${PSPRECOMP_GENERATED_OPT_LEVEL}" BURNOUT_GE_RENDER=1 BURNOUT_VITA_GL=1
            BURNOUT_VITA_LABEL="${BURNOUT_VITA_GL_LABEL}" BURNOUT_VITA_FILES="${BURNOUT_VITA_GL_FILES}")
        if(BURNOUT_VITA_PROFILE)
            target_compile_definitions(burnout_vita_gl PRIVATE BURNOUT_VITA_PROFILE=1)
        endif()
        target_include_directories(burnout_vita_gl PRIVATE "${BURNOUT_PROFILE_DIR}/host")
        # The renderer's decode thread (one core) bounds the frame rate on the
        # Vita: its math calls set no errno (-O3 measured no faster than -O2).
        target_compile_options(burnout_vita_gl PRIVATE -fno-math-errno)
    target_link_libraries(burnout_vita_gl PRIVATE burnout_thirdparty)
        target_link_libraries(burnout_vita_gl PRIVATE burnout_generated
            vitaGL vitashark SceShaccCgExt mathneon taihen_stub SceShaccCg_stub SceKernelDmacMgr_stub
            SceGxm_stub SceDisplay_stub SceCommonDialog_stub SceAppMgr_stub SceSysmodule_stub SceIme_stub
            SceCtrl_stub SceSysmem_stub ScePower_stub SceKernelThreadMgr_stub SceProcessmgr_stub
            SceLibKernel_stub SceAudio_stub SceVideodec_stub SceAppUtil_stub SceRtc_stub SceCommonDialog_stub pthread m)
        vita_create_self(eboot_gl.bin burnout_vita_gl UNSAFE)
        # LiveArea icon (128x128, 8-bit palette PNG): game-derived art, kept
        # beside the game files, never in the repository.
        set(BURNOUT_VITA_ICON "${CMAKE_SOURCE_DIR}/../original/vita_icon0.png" CACHE FILEPATH "LiveArea icon0.png")
        set(BURNOUT_VITA_ICON_FILE)
        # (vita.cmake quotes the -a paths: patched locally for "Burnout Legends".)
        if(EXISTS "${BURNOUT_VITA_ICON}")
            set(BURNOUT_VITA_ICON_FILE FILE "${BURNOUT_VITA_ICON}" sce_sys/icon0.png)
        endif()
        # LiveArea background and gate image (840x500 bg.png, 280x158
        # startup.png, 8-bit palette) with their template.xml, same place.
        set(BURNOUT_VITA_LIVEAREA "${CMAKE_SOURCE_DIR}/../original/vita_livearea" CACHE PATH "LiveArea contents folder")
        if(EXISTS "${BURNOUT_VITA_LIVEAREA}/template.xml")
            foreach(item bg.png startup.png template.xml)
                list(APPEND BURNOUT_VITA_ICON_FILE FILE "${BURNOUT_VITA_LIVEAREA}/${item}" sce_sys/livearea/contents/${item})
            endforeach()
        endif()
        # Save slot icons of the native save dialog (PSP / PS VITA marked):
        # made from the game's save icon, beside the game files.
        set(BURNOUT_VITA_SAVE_ICONS "${CMAKE_SOURCE_DIR}/../original/save_icons" CACHE PATH "Save dialog icons folder")
        # The Bonus menu's font (Oswald Bold, SIL Open Font License).
        list(APPEND BURNOUT_VITA_ICON_FILE FILE "${CMAKE_CURRENT_SOURCE_DIR}/host/third_party/fonts/Oswald-Bold.ttf" fonts/Oswald-Bold.ttf
             FILE "${CMAKE_CURRENT_SOURCE_DIR}/host/third_party/fonts/OFL.txt" fonts/OFL.txt)
        foreach(item psp.png vita.png)
            if(EXISTS "${BURNOUT_VITA_SAVE_ICONS}/${item}")
                list(APPEND BURNOUT_VITA_ICON_FILE FILE "${BURNOUT_VITA_SAVE_ICONS}/${item}" save_icons/${item})
            endif()
        endforeach()
        # The Bonus menu's ready-made postcards (<filter id>.jpg, made with
        # burnout_headless --make-postcards; not in git: pictures of the game).
        set(BURNOUT_VITA_POSTCARDS "${CMAKE_SOURCE_DIR}/../original/postcards" CACHE PATH "Bonus postcards folder")
        file(GLOB BURNOUT_POSTCARD_FILES CONFIGURE_DEPENDS "${BURNOUT_VITA_POSTCARDS}/*.jpg")
        foreach(item ${BURNOUT_POSTCARD_FILES})
            get_filename_component(name "${item}" NAME)
            list(APPEND BURNOUT_VITA_ICON_FILE FILE "${item}" postcards/${name})
        endforeach()
        # The Signature Takedown photos shown on the awards screen
        # (<name>.png from Frontend.txd, made with tools/modding burnout_bake;
        # not in git: pictures of the game).
        set(BURNOUT_VITA_SIGNATURES "${CMAKE_SOURCE_DIR}/../original/signatures" CACHE PATH "Signature photos folder")
        file(GLOB BURNOUT_SIGNATURE_FILES CONFIGURE_DEPENDS "${BURNOUT_VITA_SIGNATURES}/*.png")
        foreach(item ${BURNOUT_SIGNATURE_FILES})
            get_filename_component(name "${item}" NAME)
            list(APPEND BURNOUT_VITA_ICON_FILE FILE "${item}" signatures/${name})
        endforeach()
        # The title movie with the 2.0 logo (original/title/startB.pmf, made
        # with tools/make_title_movie.py; not in git): read before the
        # player's own USRDIR/ovid/startB.pmf (burnout_hle.cpp, sceIoOpen).
        set(BURNOUT_VITA_TITLE_MOVIE "${CMAKE_SOURCE_DIR}/../original/title/startB.pmf" CACHE FILEPATH "Title movie shipped in the VPK")
        if(EXISTS "${BURNOUT_VITA_TITLE_MOVIE}")
            list(APPEND BURNOUT_VITA_ICON_FILE FILE "${BURNOUT_VITA_TITLE_MOVIE}" ovid/startB.pmf)
        endif()
        # BONUS 2.0's pictures (Polaroids, postcards, newspapers: original/bonus,
        # not in git: pictures of the game).
        set(BURNOUT_VITA_BONUS "${CMAKE_SOURCE_DIR}/../original/bonus" CACHE PATH "BONUS 2.0 pictures folder")
        file(GLOB BURNOUT_BONUS_FILES CONFIGURE_DEPENDS "${BURNOUT_VITA_BONUS}/*.png")
        foreach(item ${BURNOUT_BONUS_FILES})
            get_filename_component(name "${item}" NAME)
            list(APPEND BURNOUT_VITA_ICON_FILE FILE "${item}" bonus/${name})
        endforeach()
        vita_create_vpk(burnout-gl.vpk ${BURNOUT_VITA_GL_TITLE_ID} eboot_gl.bin
            VERSION 01.00
            NAME "${BURNOUT_VITA_GL_NAME}"
            ${BURNOUT_VITA_ICON_FILE})
    endif()
    return()
endif()

# GE renderer: the VCS profile's software rasterizer, driven by the Burnout GE
# command stream, with the GPU backend stubbed out.
set(BURNOUT_VCS_HOST "${BURNOUT_PROFILE_DIR}/../vcs/host")
add_library(burnout_ge_render STATIC
    "${BURNOUT_VCS_HOST}/ge_renderer.cpp"
    "${BURNOUT_VCS_HOST}/vcs_config.cpp"
    "${BURNOUT_VCS_HOST}/vcs_camera_input.cpp"
    "${BURNOUT_VCS_HOST}/vcs_vehicle_input.cpp"
    "${BURNOUT_VCS_HOST}/vcs_project2dfx.cpp"
    "${BURNOUT_VCS_HOST}/vcs_project2dfx_lights.cpp"
    "${BURNOUT_VCS_HOST}/vcs_fps_overlay.cpp"
    "${BURNOUT_VCS_HOST}/framebuffer_capture.cpp"
    host/ge_gpu_backend_none.cpp)
target_include_directories(burnout_ge_render PUBLIC "${BURNOUT_VCS_HOST}")
target_link_libraries(burnout_ge_render PUBLIC psprecomp_core)
psprecomp_enable_host_avx(burnout_ge_render)

add_executable(burnout_headless host/main_headless.cpp host/ge_render.cpp host/ge_gl.cpp host/gl_context_win.cpp
    ${BURNOUT_HOST_SOURCES}
    $<$<BOOL:${WIN32}>:host/sampling_profiler_win.cpp>)
target_link_libraries(burnout_headless PRIVATE burnout_generated burnout_ge_render opengl32 winmm mfplat mfuuid ole32)
target_compile_definitions(burnout_headless PRIVATE BURNOUT_GE_RENDER=1 BURNOUT_GE_SOFTWARE=1)
target_include_directories(burnout_headless PRIVATE "${BURNOUT_PROFILE_DIR}/host")
target_link_libraries(burnout_headless PRIVATE burnout_thirdparty)
if(MSVC)
    # Guest code runs on the host stack only one dispatch deep, but generated
    # units chain natively; keep the same headroom as the VCS profile.
    target_link_options(burnout_headless PRIVATE /STACK:67108864 /DEBUG)
    # Symbols for the host code (sampling profiler); generated units unchanged.
    target_compile_options(burnout_headless PRIVATE /Zi)
endif()

# The playable PC build (main_pc.cpp): a window, pad and keyboard, sound.
# Its pictures and font are copied next to it; the game files are made at
# its first launch from the player's own disc image.
if(WIN32)
    # Its icon: the LiveArea icon made into pc_icon.ico beside the game files
    # (game-derived art, not in the repository); none when it is missing.
    set(BURNOUT_PC_ICON "${CMAKE_SOURCE_DIR}/../original/pc_icon.ico" CACHE FILEPATH "Windows icon of the PC build")
    set(BURNOUT_PC_RC)
    if(EXISTS "${BURNOUT_PC_ICON}")
        file(WRITE "${CMAKE_CURRENT_BINARY_DIR}/burnout_pc.rc" "1 ICON \"${BURNOUT_PC_ICON}\"\n")
        set(BURNOUT_PC_RC "${CMAKE_CURRENT_BINARY_DIR}/burnout_pc.rc")
    endif()
    add_executable(burnout_pc host/main_pc.cpp host/ge_render.cpp host/ge_gl.cpp host/gl_context_win.cpp host/sampling_profiler_win.cpp ${BURNOUT_HOST_SOURCES} ${BURNOUT_PC_RC})
    set_target_properties(burnout_pc PROPERTIES OUTPUT_NAME burnout_legends)
    target_link_libraries(burnout_pc PRIVATE burnout_generated burnout_ge_render opengl32 winmm mfplat mfuuid ole32 burnout_thirdparty)
    target_compile_definitions(burnout_pc PRIVATE BURNOUT_GE_RENDER=1 BURNOUT_GE_SOFTWARE=1)
    target_include_directories(burnout_pc PRIVATE "${BURNOUT_PROFILE_DIR}/host")
    if(MSVC)
        # No console window: main() through the windows subsystem.
        target_link_options(burnout_pc PRIVATE /STACK:67108864 /SUBSYSTEM:WINDOWS /ENTRY:mainCRTStartup)
    endif()
    set(BURNOUT_PC_PICTURES "${CMAKE_SOURCE_DIR}/../original")
    add_custom_command(TARGET burnout_pc POST_BUILD
        COMMAND ${CMAKE_COMMAND} -E make_directory "$<TARGET_FILE_DIR:burnout_pc>/fonts"
        COMMAND ${CMAKE_COMMAND} -E copy_if_different "${CMAKE_CURRENT_SOURCE_DIR}/host/third_party/fonts/Oswald-Bold.ttf"
                "${CMAKE_CURRENT_SOURCE_DIR}/host/third_party/fonts/OFL.txt" "$<TARGET_FILE_DIR:burnout_pc>/fonts/"
        COMMAND ${CMAKE_COMMAND} -E copy_directory "${BURNOUT_PC_PICTURES}/postcards" "$<TARGET_FILE_DIR:burnout_pc>/postcards"
        COMMAND ${CMAKE_COMMAND} -E copy_directory "${BURNOUT_PC_PICTURES}/signatures" "$<TARGET_FILE_DIR:burnout_pc>/signatures")
endif()

add_executable(burnout_opcode_census tools/opcode_census.cpp)
target_link_libraries(burnout_opcode_census PRIVATE psprecomp_core)
