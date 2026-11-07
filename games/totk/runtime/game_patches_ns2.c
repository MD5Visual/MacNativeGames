/* Switch 2 games (GAME_NS2 in runtime/game_version.h; first Donkey Kong Bananza, GAME_DKB, docs/ns2-port.md): the patch file
   the Makefile builds in place of TOTK's game_patches.c. Only OoT has a guarded measured-time producer; the other games run their own frame pacing
   and output size, and the output-size and aspect hooks the frontend calls are inert. Exclusive loads take acquire, as on
   Breath of the Wild and Super Mario Bros. Wonder (games/wonder/runtime/game_patches_wonder.c): sead's spin locks, which
   Bananza's engine (sead / agl / gsys) uses too, take the lock with a plain ldxr. */
#include "game_patches.h"
#include "game_version.h"
#include <stdatomic.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <TargetConditionals.h>
#ifdef GAME_OOT
#include "oot_frame_step.h"
static _Atomic uint32_t *oot_seconds;
static OotFrameStep oot_clock;
static void oot_realtime_patch(Module *m) {
    /* v131072 main e0f3ccfaf50065ae4ea01eb5aeb13b1a only (game_version.h).
       nxnative PR1543: the original fdiv constructs 60/target without a clock.
       Keep it, then take max(original, s1 * measured seconds). x16/x17/s0 are
       dead after this site. The veneer does not change NZCV.
       Use the loader's private page and text padding, like TOTK's producer. */
    const uint64_t site = 0x6e6cc4, stub = (m->text_end + 15) & ~15ULL;
    const uint64_t data = (m->file_size + 0x3fff) & ~0x3fffULL;
    if (site + 4 > m->text_end || data + 0x4000 != m->size ||
        stub + 24 > ((m->text_end + 0x3fff) & ~0x3fffULL)) runtime_fail("OoT frame-step layout");
    int64_t pages = (int64_t)(data >> 12) - (int64_t)((stub + 4) >> 12);
    int64_t jump = (int64_t)stub - site, back = (int64_t)(site + 4) - (int64_t)(stub + 20);
    if (pages < -(1 << 20) || pages >= (1 << 20) ||
        jump < -(1 << 27) || jump >= (1 << 27) || back < -(1 << 27) || back >= (1 << 27)) runtime_fail("OoT frame-step branch range");
    uint32_t imm = (uint32_t)pages & 0x1fffff;
    uint32_t code[] = {0x1e201828, /* fdiv s8,s1,s0 */
        0x90000010 | ((imm & 3) << 29) | ((imm >> 2) << 5), /* adrp x16,data */
        0xbd400200, 0x1e210800, 0x1e204908, /* ldr s0,[x16]; fmul s0,s0,s1; fmax s8,s8,s0 */
        0x14000000 | ((uint32_t)(back >> 2) & 0x3ffffff)};
    uint32_t branch = 0x14000000 | ((uint32_t)(jump >> 2) & 0x3ffffff), old;
    memcpy(&old, m->base + site, 4);
#if TARGET_OS_IPHONE
    if (old != branch || memcmp(m->base + stub, code, sizeof code)) runtime_fail("OoT embedded frame-step mismatch");
#else
    if (old != 0x1e201828) runtime_fail("OoT frame-step original instruction mismatch");
    for (unsigned i = 0; i < sizeof code; i++) if (m->base[stub + i]) runtime_fail("OoT frame-step padding not empty");
    memcpy(m->base + stub, code, sizeof code); memcpy(m->base + site, &branch, 4);
#endif
    oot_seconds = (_Atomic uint32_t *)(void *)(m->base + data);
    atomic_store(oot_seconds, 0);
    fprintf(stderr, "[oot] checked measured frame step enabled (DM_FRAME_DELTA=0: original fixed step)\n");
}
#endif

/* No frame-step patch: these are what the frontend and the frame lock call. */
void runtime_frame_step_pin(double ms) { (void)ms; }
void runtime_frame_step_update(uint64_t now_ns) {
#ifdef GAME_OOT
    if (!oot_seconds) return;
    const char *enabled = getenv("DM_FRAME_DELTA");
    float seconds = enabled && !strcmp(enabled, "0") ? 0 : oot_frame_seconds(&oot_clock, now_ns);
    uint32_t bits; memcpy(&bits, &seconds, 4);
    atomic_store_explicit(oot_seconds, bits, memory_order_release);
#else
    (void)now_ns;
#endif
}

int runtime_output_res(unsigned *w, unsigned *h) { (void)w; (void)h; return 0; }
int runtime_render_res(unsigned *w, unsigned *h) { (void)w; (void)h; return 0; }   /* no larger scene size for the heaps (romfs.c) */
uint64_t runtime_output_res_extra_bytes(void) { return 0; }
uint64_t runtime_output_size_current(void) { return (uint64_t)1080 << 32 | 1920; }
int runtime_output_res_toggle(void) { return 0; }
int runtime_output_mode_cycle(void) { return 0; }
int runtime_aspect_toggle(void) { return 0; }
float runtime_aspect_now(void) { return 0; }
int runtime_aspect_step(void) { return -1; }   /* no aspect setting: the shortcut list leaves it out */
void runtime_aspect_view(double w, double h) { (void)w; (void)h; }
int runtime_output_modes(void) { return 0; }
int runtime_output_mode(void) { return OUTPUT_MODE_OFF; }
int runtime_output_mode_set(int mode) { (void)mode; return 0; }
int runtime_output_native(int mode, unsigned *w, unsigned *h) { (void)mode; (void)w; (void)h; return 0; }
int runtime_aspect_set_ratio(float r) { (void)r; return 0; }
/* TOTK's VR and camera-distance hooks (zeldaVR, the settings panel) are left to their weak defaults (zeldaVR/gfx/vr_defaults.c):
   no sites in a Switch 2 game, so VR and the camera row stay off. */
float runtime_aspect_ratio(void) { return 0; }
float runtime_aspect_start(void) { return 0; }

/* Exclusive loads with acquire (TOTK_EXCL_ACQUIRE=0: as shipped): every plain ldxr / ldxp that opens a real exclusive window
   (a store-exclusive on the same base register within 16 instructions) gets the acquire bit. Why: game_patches_wonder.c. */
static int excl_ld(uint32_t w) { return (w & 0x3ffffc00u) == 0x085f7c00u || (w & 0xbfff8000u) == 0x887f0000u; }   /* ldxr{b,h} / ldxp */
static int excl_st(uint32_t w) { return (w & 0x3fe07c00u) == 0x08007c00u || (w & 0xbfe00000u) == 0x88200000u; }   /* st{l}xr{b,h} / st{l}xp */
static unsigned excl_acquire(uint8_t *code, size_t n) {
    unsigned done = 0; uint32_t *w = (uint32_t *)(void *)code; size_t words = n / 4;
    for (size_t i = 0; i < words; i++) {
        if (!excl_ld(w[i])) continue;
        uint32_t rn = (w[i] >> 5) & 31;
        for (size_t j = i + 1; j < words && j <= i + 16; j++) if (excl_st(w[j]) && ((w[j] >> 5) & 31) == rn) { w[i] |= 0x8000u; done++; break; }
    }
    return done;
}

void runtime_game_patches(Module *m) {
    if (strcmp(m->name, "main")) runtime_fail("game patches expect the main module");
#ifdef GAME_OOT
    oot_realtime_patch(m);
#endif
    if (getenv("TOTK_PCHTXT") || getenv("TOTK_ASPECT") || (getenv("TOTK_OUTPUT_RES") && strcmp(getenv("TOTK_OUTPUT_RES"), "0")))
        fprintf(stderr, "[totk] %s: TOTK_PCHTXT, TOTK_ASPECT and TOTK_OUTPUT_RES are not supported; ignored\n", GAME_NAME);
    const char *a = getenv("TOTK_EXCL_ACQUIRE");
    if (!a || strcmp(a, "0")) {
        unsigned t = excl_acquire(m->base, m->text_end), p = excl_acquire(m->base + m->patch_off, m->file_size - m->patch_off);
        fprintf(stderr, "[totk] exclusive loads with acquire: %u in the code, %u in the rewriter's patch area (TOTK_EXCL_ACQUIRE=0: as shipped)\n", t, p);
    } else fprintf(stderr, "[totk] exclusive loads as shipped (TOTK_EXCL_ACQUIRE=0)\n");
}

/* The game paces itself (its own present interval and frame-rate controller): no host frame limit. */
int runtime_game_fps_limit(void) { return 0; }
