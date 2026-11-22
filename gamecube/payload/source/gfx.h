#include <gccore.h>

typedef struct {
    GXTexObj obj;
    u32 unused;
    u16 pad_width, pad_height;  // rounded up to a multiple of 4
    u16 width, height;
} texture_t;

texture_t texture_load(TPLFile *tpl, s32 id);
void draw_texture(int x, int y, texture_t tex);
void draw_text(int x, int y, const char *str, u32 color, texture_t font, f32 scale);
void draw_rect(int x, int y, int w, int h, u32 color);
void draw_textf(int x, int y, texture_t font, u32 color, f32 scale, const char *fmt, ...);
