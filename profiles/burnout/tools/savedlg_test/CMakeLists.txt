cmake_minimum_required(VERSION 3.16)
if(NOT DEFINED CMAKE_TOOLCHAIN_FILE)
    set(CMAKE_TOOLCHAIN_FILE "$ENV{VITASDK}/share/vita.toolchain.cmake" CACHE PATH "toolchain file")
endif()
project(savedlg_test CXX C)
include("$ENV{VITASDK}/share/vita.cmake" REQUIRED)

set(CMAKE_CXX_STANDARD 17)
add_executable(savedlg_test main.cpp)
target_link_libraries(savedlg_test
    vitaGL vitashark SceShaccCgExt mathneon taihen_stub SceShaccCg_stub SceKernelDmacMgr_stub
    SceGxm_stub SceDisplay_stub SceCommonDialog_stub SceAppUtil_stub SceAppMgr_stub SceSysmodule_stub SceIme_stub
    SceCtrl_stub SceSysmem_stub ScePower_stub SceKernelThreadMgr_stub SceProcessmgr_stub
    SceLibKernel_stub SceAudio_stub SceRtc_stub pthread m)
vita_create_self(eboot.bin savedlg_test UNSAFE)
vita_create_vpk(savedlg_test.vpk BRNT00001 eboot.bin VERSION 01.00 NAME "Burnout save test")
