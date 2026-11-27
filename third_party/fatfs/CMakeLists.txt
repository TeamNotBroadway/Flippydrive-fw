# FatFs R0.15 (elm-chan.org). Built without -fdata-sections: the original's literal pools
# address FatFs data through section anchors.
#
# The two images were built with different FatFs configurations. `fatfs` is the bootloader's
# (ffconf.h). main-ode's differs in three options, read from its code: FF_FS_LOCK 2 (dec_share
# and clear_share bound the table at 2), FF_USE_LFN 2 (no ff_memalloc path, lfnbuf/dirbuf not
# set in mount_volume) and FF_FS_NOFSINFO 0 (mount_volume loads free_clst from FSInfo).
set(FATFS_SOURCES ff.c ffunicode.c ffsystem.c)
add_library(fatfs STATIC ${FATFS_SOURCES})
add_library(fatfs_main_ode STATIC ${FATFS_SOURCES})
foreach(t fatfs fatfs_main_ode)
    target_include_directories(${t} PUBLIC ${CMAKE_CURRENT_LIST_DIR})
    target_compile_options(${t} PRIVATE -ffunction-sections -fno-data-sections)
endforeach()
target_compile_definitions(fatfs_main_ode PUBLIC FF_FS_LOCK=2 FF_USE_LFN=2 FF_FS_NOFSINFO=0)
