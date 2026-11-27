# FatFs R0.15 (elm-chan.org). Built without -fdata-sections: the original's literal pools
# address FatFs data through section anchors.
#
# The two images were built with different FatFs configurations. `fatfs` is the bootloader's
# (ffconf.h). main-ode's differs in three options, read from its code: FF_FS_LOCK 2 (dec_share
# and clear_share bound the table at 2), FF_USE_LFN 2 (no ff_memalloc path, lfnbuf/dirbuf not
# set in mount_volume) and FF_FS_NOFSINFO 0 (mount_volume loads free_clst from FSInfo).
# FF_USE_EXPAND 0 and FF_USE_STRFUNC 0 leave find_bitmap and tchar2uni one caller each, so they
# inline into create_chain and create_name as in the original, which has neither as a function.
# Compiler options: -O2 with -O3's four inlining parameters and -fno-ipa-strict-aliasing,
# found by tools/flagsweep_at.py (located functions 2463 -> 3119 windows, vs 2742 for the best
# -O3 variant). -O2 has no -fipa-cp-clone, which -O3 has: the original's dir_next keeps its
# stretch parameter instead of a .constprop clone, and ff.c's function order equals the
# original's.
set(FATFS_SOURCES ff.c ffunicode.c ffsystem.c)
add_library(fatfs STATIC ${FATFS_SOURCES})
add_library(fatfs_main_ode STATIC ${FATFS_SOURCES})
foreach(t fatfs fatfs_main_ode)
    target_include_directories(${t} PUBLIC ${CMAKE_CURRENT_LIST_DIR})
    target_compile_options(${t} PRIVATE -ffunction-sections -fno-data-sections)
endforeach()
target_compile_definitions(fatfs_main_ode PUBLIC FF_FS_LOCK=2 FF_USE_LFN=2 FF_FS_NOFSINFO=0
    FF_USE_EXPAND=0 FF_USE_STRFUNC=0)
target_compile_options(fatfs_main_ode PRIVATE -O2 --param=max-inline-insns-auto=30
    --param=early-inlining-insns=14 --param=inline-heuristics-hint-percent=600
    --param=inline-min-speedup=15 -fno-ipa-strict-aliasing)
