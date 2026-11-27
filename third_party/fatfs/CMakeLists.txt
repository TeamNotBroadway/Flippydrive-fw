# FatFs R0.15 (elm-chan.org). Built without -fdata-sections: the original's literal pools
# address FatFs data through section anchors.
#
# Both images use ffconf.h; FF_USE_EXPAND 0, FF_USE_STRFUNC 0 and FF_FS_NOFSINFO 0 make every
# located ff.c function exact in both (syms/*.fatfs.tsv, tools/lib_score.py). main-ode differs
# in two options, read from its code: FF_FS_LOCK 2 (dec_share and clear_share bound the table
# at 2) and FF_USE_LFN 2 (no ff_memalloc path, lfnbuf/dirbuf not set in mount_volume).
# ff.c starts with the vendor's #pragma GCC optimize ("O2"); see the comment there.
set(FATFS_SOURCES ff.c ffunicode.c ffsystem.c)
add_library(fatfs STATIC ${FATFS_SOURCES})
add_library(fatfs_main_ode STATIC ${FATFS_SOURCES})
foreach(t fatfs fatfs_main_ode)
    target_include_directories(${t} PUBLIC ${CMAKE_CURRENT_LIST_DIR})
    target_compile_options(${t} PRIVATE -ffunction-sections -fno-data-sections)
endforeach()
target_compile_definitions(fatfs_main_ode PUBLIC FF_FS_LOCK=2 FF_USE_LFN=2)
