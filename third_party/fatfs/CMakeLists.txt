# FatFs R0.15 (elm-chan.org), unmodified except ffconf.h. Built without -fdata-sections:
# the original's literal pools address FatFs data through section anchors.
add_library(fatfs STATIC ff.c ffunicode.c ffsystem.c)
target_include_directories(fatfs PUBLIC ${CMAKE_CURRENT_LIST_DIR})
target_compile_options(fatfs PRIVATE -ffunction-sections -fno-data-sections)
