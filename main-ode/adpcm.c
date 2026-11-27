// GameCube DTK (streamed audio) ADPCM: 32-byte frames of 28 stereo samples. Names are ours.
#include "pico/stdlib.h"
#include "main_ode.h"

#define ADPCM_FRAME 32
#define ADPCM_SAMPLES 28

int32_t adpcm_hist_l[2];
int32_t adpcm_hist_r[2];

void adpcm_reset(void)
{
    adpcm_hist_l[0] = 0;
    adpcm_hist_l[1] = 0;
    adpcm_hist_r[0] = 0;
    adpcm_hist_r[1] = 0;
}

static inline int16_t adpcm_sample(int bits, int scale, int filter, int32_t *hist)
{
    int32_t h;
    if (filter == 1) {
        h = hist[0] * 0x3c;
    } else if (filter == 2) {
        h = hist[0] * 0x73 - hist[1] * 0x34;
    } else {
        h = 0;
        if (filter == 3)
            h = hist[0] * 0x62 - hist[1] * 0x37;
    }
    h = (h + 0x20) >> 6;
    if (h < -0x200000)
        h = -0x200000;
    if (h > 0x1fffff)
        h = 0x1fffff;
    int32_t prev = hist[0];
    int32_t cur = h + (((int16_t)(bits << 12) >> scale) << 6);
    hist[1] = prev;
    hist[0] = cur;
    cur >>= 6;
    if (cur < -0x8000)
        cur = -0x8000;
    if (cur > 0x7fff)
        cur = 0x7fff;
    return cur;
}

// One frame: header bytes for left and right (scale in the low nibble, filter in bits 4-5),
// two unused bytes, then 28 bytes of (left low nibble, right high nibble).
void adpcm_decode_frame(const uint8_t *frame, int16_t *pcm)
{
    int lscale = frame[0] & 0xf, lfilter = (frame[0] >> 4) & 3;
    int rscale = frame[1] & 0xf, rfilter = (frame[1] >> 4) & 3;
    for (int i = 0; i < ADPCM_SAMPLES; i++) {
        uint8_t b = frame[4 + i];
        pcm[i * 2] = adpcm_sample(b, lscale, lfilter, adpcm_hist_l);
        pcm[i * 2 + 1] = adpcm_sample(b >> 4, rscale, rfilter, adpcm_hist_r);
    }
}

void adpcm_decode(const uint8_t *src, uint32_t *dst, uint32_t len)
{
    for (uint32_t i = 0; i < len / ADPCM_FRAME; i++)
        adpcm_decode_frame(src + i * ADPCM_FRAME, (int16_t *)(dst + i * ADPCM_SAMPLES));
}
