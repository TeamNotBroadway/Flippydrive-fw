// Textured and flat 2D quads through GX.
#include <string.h>
#include <gccore.h>

#include "gfx.h"

// Text layout, in pixels. GCC emits variables in reverse definition order, so defining
// char_width first puts text_margin at the lower address, as in the original.
int char_width = 10;
int text_margin = 20;

texture_t texture_load(TPLFile *tpl, s32 id) {
    texture_t tex;
    TPL_GetTextureInfo(tpl, id, NULL, &tex.width, &tex.height);
    tex.pad_width = tex.width;
    if (tex.width & 3)
        tex.pad_width = (tex.width & ~3) + 4;
    tex.pad_height = tex.height;
    if (tex.height & 3)
        tex.pad_height = (tex.height & ~3) + 4;
    TPL_GetTexture(tpl, id, &tex.obj);
    return tex;
}

void draw_texture(int x, int y, texture_t tex) {
    GX_LoadTexObj(&tex.obj, GX_TEXMAP0);
    GX_SetTevOp(GX_TEVSTAGE0, GX_MODULATE);
    GX_SetVtxDesc(GX_VA_TEX0, GX_DIRECT);

    GX_Begin(GX_QUADS, GX_VTXFMT0, 4);
    GX_Position3f32(x, y, 0);
    GX_Color4u8(0xff, 0xff, 0xff, 0xff);
    GX_TexCoord2f32(0, 0);

    GX_Position3f32(x + tex.width, y, 0);
    GX_Color4u8(0xff, 0xff, 0xff, 0xff);
    GX_TexCoord2f32((f32)tex.width / tex.pad_width, 0);

    GX_Position3f32(x + tex.width, y + tex.height, 0);
    GX_Color4u8(0xff, 0xff, 0xff, 0xff);
    GX_TexCoord2f32((f32)tex.width / tex.pad_width, (f32)tex.height / tex.pad_height);

    GX_Position3f32(x, y + tex.height, 0);
    GX_Color4u8(0xff, 0xff, 0xff, 0xff);
    GX_TexCoord2f32(0, (f32)tex.height / tex.pad_height);
    GX_End();

    GX_SetTevOp(GX_TEVSTAGE0, GX_PASSCLR);
    GX_SetVtxDesc(GX_VA_TEX0, GX_NONE);
}

// The font is one row of 10-pixel glyphs for ' ' to DEL.
void draw_text(int x, int y, const char *str, u32 color, texture_t font, f32 scale) {
    GX_LoadTexObj(&font.obj, GX_TEXMAP0);
    GX_SetTevOp(GX_TEVSTAGE0, GX_MODULATE);
    GX_SetVtxDesc(GX_VA_TEX0, GX_DIRECT);

    int len = strlen(str);
    u16 tex_w = font.pad_width;
    GX_Begin(GX_QUADS, GX_VTXFMT0, len * 4);

    f32 glyph_w = 10.0f / tex_w;
    f32 cx = x;
    f32 inset_s = 0.001f / font.pad_width;
    f32 inset_t = 0.001f / font.pad_height;
    f32 top = y;
    f32 bottom = y + scale * 20;
    f32 w = scale * 10;
    for (int i = 0; i < len; i++) {
        u8 c = str[i];
        u32 g;
        if (c == '\n')
            g = 0;
        else if ((u8)(c - ' ') <= 95)
            g = c - ' ';
        else
            g = '?' - ' ';
        f32 s0 = glyph_w * g + inset_s;
        f32 s1 = s0 + glyph_w - inset_s;

        GX_Position3f32(cx, top, 0);
        GX_Color1u32(color);
        GX_TexCoord2f32(s0, 0 + inset_t);

        GX_Position3f32(cx + w, top, 0);
        GX_Color1u32(color);
        GX_TexCoord2f32(s1, 0 + inset_t);

        GX_Position3f32(cx + w, bottom, 0);
        GX_Color1u32(color);
        GX_TexCoord2f32(s1, 1 - inset_t);

        GX_Position3f32(cx, bottom, 0);
        GX_Color1u32(color);
        GX_TexCoord2f32(s0, 1 - inset_t);

        cx += w * 0.85f;
    }
    GX_End();

    GX_SetTevOp(GX_TEVSTAGE0, GX_PASSCLR);
    GX_SetVtxDesc(GX_VA_TEX0, GX_NONE);
}

void draw_rect(int x, int y, int w, int h, u32 color) {
    GX_SetTevOp(GX_TEVSTAGE0, GX_PASSCLR);
    GX_SetVtxDesc(GX_VA_TEX0, GX_NONE);

    GX_Begin(GX_QUADS, GX_VTXFMT0, 4);
    GX_Position3f32(x, y, 0);
    GX_Color1u32(color);
    GX_Position3f32(x + w, y, 0);
    GX_Color1u32(color);
    GX_Position3f32(x + w, y + h, 0);
    GX_Color1u32(color);
    GX_Position3f32(x, y + h, 0);
    GX_Color1u32(color);
    GX_End();
}
