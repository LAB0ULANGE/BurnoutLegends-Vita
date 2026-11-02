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
    host/music.cpp)

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
    target_link_libraries(burnout_vita PRIVATE burnout_generated
        SceDisplay_stub SceSysmem_stub ScePower_stub SceKernelThreadMgr_stub
        SceProcessmgr_stub SceLibKernel_stub SceAudio_stub pthread)
    vita_create_self(eboot.bin burnout_vita UNSAFE)
    vita_create_vpk(burnout-bench.vpk BRNB00001 eboot.bin
        VERSION 01.00
        NAME "Burnout recomp bench")

    # Rendering build: the same runtime with the GE translated to vitaGL, the
    # real pad and real-time pacing. Separate title so the bench stays intact.
    option(BURNOUT_VITA_GL "Also build the vitaGL rendering VPK" ON)
    if(BURNOUT_VITA_GL)
        add_executable(burnout_vita_gl host/main_vita.cpp host/boot_menu_vita.cpp host/ge_render.cpp host/ge_gl.cpp
            host/gl_context_vita.cpp ${BURNOUT_HOST_SOURCES})
        target_compile_definitions(burnout_vita_gl PRIVATE
            BURNOUT_GENERATED_OPT="-O${PSPRECOMP_GENERATED_OPT_LEVEL}" BURNOUT_GE_RENDER=1 BURNOUT_VITA_GL=1)
        target_include_directories(burnout_vita_gl PRIVATE "${BURNOUT_PROFILE_DIR}/host")
        target_link_libraries(burnout_vita_gl PRIVATE burnout_generated
            vitaGL vitashark SceShaccCgExt mathneon taihen_stub SceShaccCg_stub SceKernelDmacMgr_stub
            SceGxm_stub SceDisplay_stub SceCommonDialog_stub SceAppMgr_stub SceSysmodule_stub SceIme_stub
            SceCtrl_stub SceSysmem_stub ScePower_stub SceKernelThreadMgr_stub SceProcessmgr_stub
            SceLibKernel_stub SceAudio_stub pthread m)
        vita_create_self(eboot_gl.bin burnout_vita_gl UNSAFE)
        vita_create_vpk(burnout-gl.vpk BRNG00001 eboot_gl.bin
            VERSION 01.00
            NAME "Burnout recomp GL")
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
target_link_libraries(burnout_headless PRIVATE burnout_generated burnout_ge_render opengl32)
target_compile_definitions(burnout_headless PRIVATE BURNOUT_GE_RENDER=1 BURNOUT_GE_SOFTWARE=1)
target_include_directories(burnout_headless PRIVATE "${BURNOUT_PROFILE_DIR}/host")
if(MSVC)
    # Guest code runs on the host stack only one dispatch deep, but generated
    # units chain natively; keep the same headroom as the VCS profile.
    target_link_options(burnout_headless PRIVATE /STACK:67108864 /DEBUG)
    # Symbols for the host code (sampling profiler); generated units unchanged.
    target_compile_options(burnout_headless PRIVATE /Zi)
endif()

add_executable(burnout_opcode_census tools/opcode_census.cpp)
target_link_libraries(burnout_opcode_census PRIVATE psprecomp_core)
