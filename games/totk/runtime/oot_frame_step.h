/* Measured step, adapted from seedds/nxnative PR1543, c254ba96 (MIT).
   Only OoT's checked 60/target producer consumes it; this is not a host FPS cap. */
#pragma once
#include <stdint.h>
#include <math.h>
enum { OOT_FRAME_SAMPLES = 8 };
typedef struct { uint64_t last; double samples[OOT_FRAME_SAMPLES]; unsigned n, i; } OotFrameStep;
static inline float oot_frame_seconds(OotFrameStep *s, uint64_t now) {
    uint64_t previous = s->last; s->last = now;
    if (!previous || now <= previous) { s->n = s->i = 0; return 0; }
    double dt = (double)(now - previous) * 1e-9, total = 0;
    s->samples[s->i] = fmin(dt, 0.1);
    s->i = (s->i + 1) % OOT_FRAME_SAMPLES;
    if (s->n < OOT_FRAME_SAMPLES) s->n++;
    for (unsigned i = 0; i < s->n; i++) total += s->samples[i];
    double mean = total / s->n;
    return mean * 60 < 1.1 ? 0 : (float)fmin(mean, 4.0 / 60);
}
