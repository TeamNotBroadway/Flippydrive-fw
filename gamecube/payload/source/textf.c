#include <stdarg.h>
#include <stdio.h>

#include "gfx.h"

void draw_textf(int x, int y, texture_t font, u32 color, f32 scale, const char *fmt, ...) {
    char buf[256];
    va_list args;
    va_start(args, fmt);
    vsnprintf(buf, sizeof(buf), fmt, args);
    va_end(args);
    draw_text(x, y, buf, color, font, scale);
}
