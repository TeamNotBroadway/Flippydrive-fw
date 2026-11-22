// The GameCube disc image the drive serves (gamecube/disc), followed by its length. The
// names are ours.
    .section .flash_blob, "a"
    .balign 16
    .globl gc_disc
gc_disc:
    .incbin DISC_FILE
gc_disc_end:
    .balign 16
    .globl gc_disc_size
gc_disc_size:
    .word gc_disc_end - gc_disc
