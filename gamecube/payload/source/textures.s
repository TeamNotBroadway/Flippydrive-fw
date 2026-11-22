# The menu's two TPL textures, as tools/gcdisc.py extracts them from the original payload
# (found on the assembler's -I path). Linked first: they start the application's .rodata.

	.section .rodata.font_tpl,"a"
	.balign 32
	.globl font_tpl
font_tpl:			# 960x16 RGBA8: one row of glyphs, ' ' to DEL
	.incbin "font.tpl"

	.section .rodata.logo_tpl,"a"
	.balign 32
	.globl logo_tpl
logo_tpl:			# 380x32 RGB5A3
	.incbin "logo.tpl"
