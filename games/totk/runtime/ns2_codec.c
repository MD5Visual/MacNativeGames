/* The Switch 2's nn::codec video decoders and sync point events, served on the host (GAME_NS2; docs/ns2-port.md). Ported from
   seedds/nxnative dm/host/hle_codec_video.c (5b2d4606 and later, MIT) with its SDK protocol notes; here VP8 and VP9 through
   libvpx (as movie_host.c), not H.264 (nxnative: VideoToolbox) or AV1 (nowhere), whose Initialize fails.
   The SDK sends every call to a decoder service the host does not serve: Donkey Kong Bananza starts one for its guide movies
   after its first punch. The protocol (sdk 20.5.19 Ounce):
   - the client makes one image pool per output picture and registers each with the decoder under an index. Registering puts
     the index in the decoder's queue of acquirable buffers (8 deep), with a sync handle already signalled.
   - Acquire takes the oldest index and its sync handle (description 3604 when the queue is empty); Release gives the buffer to
     the decoder to fill and puts it back in the queue at once, with a new sync handle that signals when a picture is in it.
     The client polls nn::gs::TryWaitNativeSync (or waits on a sync point event), reads the picture's properties (3604 until
     it is filled) and the picture (ReadDecoderOutputImageRgba / Nv12, the CPU route Bananza's player takes).
   - Submit takes one packet with its time and an end-of-stream flag; 3603 when the decoder cannot take more (the client
     retries after the submission-ready event).
   Pictures go into the released buffers in release order. Packets decode on the submitting thread; pictures with no released
   buffer wait in a short queue, and a full queue refuses packets. The SDK's opaque types hold a host pointer at +0; the
   decoder's event lives in its type at +0x100. Not served: encrypted input, native image handles (the GPU route). */
#include "runtime.h"
#include "thread_compat.h"
#include <pthread.h>
#include <stdbool.h>
#include <execinfo.h>   /* TEMP probe (OoT) */
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include "vpx/vpx_decoder.h"
#include "vpx/vp8dx.h"
#include <VideoToolbox/VideoToolbox.h>

typedef uint32_t Result;
void hle_signal_system_event(void *event);   /* runtime/threads.c */
#undef LOG
#define LOG(...) do { fprintf(stderr, "[ns2-codec] " __VA_ARGS__); fputc('\n', stderr); } while (0)

#define CV_MAGIC 0x56436d44u        /* 'DmCV' */
#define CV_SYNC 0x53436d44u         /* 'DmCS': a sync handle the host made (decoder id, index, generation) */
#define CV_BUFS 32
#define CV_HOLD 4                   /* decoded pictures waiting for a released buffer */
#define CV_EVENT_OFF 0x100          /* the submission-ready SystemEventType inside the decoder type */
#define CV_R(desc) ((Result)(0x6f | ((desc) << 9)))   /* nn::codec results (module 111) */
#define CV_BUSY CV_R(3603)
#define CV_NONE CV_R(3604)

static void clear_event(void *e) {
    static void (*clear)(void *);
    if (!clear && !(clear = guest_sym("_ZN2nn2os16ClearSystemEventEPNS0_15SystemEventTypeE"))) runtime_fail("nn::os::ClearSystemEvent missing");
    clear(e);
}
static void destroy_event(void *e) {
    static void (*destroy)(void *);
    if (!destroy && !(destroy = guest_sym("_ZN2nn2os18DestroySystemEventEPNS0_15SystemEventTypeE"))) runtime_fail("nn::os::DestroySystemEvent missing");
    destroy(e);
}

/* ---------- YUV (libvpx's I420) to what the game reads; from nxnative hle_movie.c md_nv12 / md_rgba (MIT) ---------- */
static uint64_t align64(uint64_t v) { return (v + 63) & ~63ull; }
static uint64_t align128(uint64_t v) { return (v + 127) & ~127ull; }
/* The SDK's NV12 image layout (Lichii nx2_gsv.h 1589, codes 1/4): pitch = align64(w), the half-height chroma
   plane at pitch*AlignUp(h,128); one image is pitch*(AlignUp(h,128)+AlignUp(h/2,128)) — 3,440,640 for 1920x1080. */
static uint64_t nv12_pitch(uint32_t w) { return align64(w); }
static uint64_t nv12_chroma(uint32_t w, uint32_t h) { return nv12_pitch(w) * align128(h); }
static uint64_t nv12_size(uint32_t w, uint32_t h) { return nv12_pitch(w) * (align128(h) + align128(h / 2)); }
static void yuv_nv12(const vpx_image_t *im, uint8_t *dst, uint64_t pitch, uint64_t coff, int w, int h) {
    int cw = (int)im->d_w < w ? (int)im->d_w : w, ch = (int)im->d_h < h ? (int)im->d_h : h;
    if (cw < w || ch < h) { memset(dst, 16, (size_t)pitch * h); memset(dst + coff, 128, (size_t)pitch * (h / 2)); }
    for (int y = 0; y < ch; y++) memcpy(dst + (size_t)y * pitch, im->planes[0] + (size_t)y * im->stride[0], (size_t)cw);
    int cw2 = (cw + 1) / 2, ch2 = (ch + 1) / 2;
    for (int y = 0; y < ch2 && y < h / 2; y++) {
        const uint8_t *u = im->planes[1] + (size_t)y * im->stride[1], *v = im->planes[2] + (size_t)y * im->stride[2];
        uint8_t *o = dst + coff + (size_t)y * pitch;
        for (int x = 0; x < cw2 && 2 * x + 1 < w; x++) { o[2 * x] = u[x]; o[2 * x + 1] = v[x]; }
    }
}
/* By the colour space and range the picture declares (VP9 colour config; undeclared: BT.709 from 720 lines up, else BT.601). */
static void yuv_rgba(const vpx_image_t *im, uint8_t *dst, int w, int h) {
    int full = im->range == VPX_CR_FULL_RANGE, b709 = im->cs == VPX_CS_BT_709 || (im->cs == VPX_CS_UNKNOWN && h >= 720);
    double ky = full ? 1.0 : 255.0 / 219.0, kc = full ? 1.0 : 255.0 / 224.0;
    double rv = b709 ? 1.5748 : 1.402, gu = b709 ? 0.1873 : 0.3441, gv = b709 ? 0.4681 : 0.7141, bu = b709 ? 1.8556 : 1.772;
    int Y = (int)(ky * 65536), RV = (int)(rv * kc * 65536), GU = (int)(gu * kc * 65536), GV = (int)(gv * kc * 65536), BU = (int)(bu * kc * 65536), y0 = full ? 0 : 16;
    int cw = (int)im->d_w < w ? (int)im->d_w : w, ch = (int)im->d_h < h ? (int)im->d_h : h;
    if (cw < w || ch < h) for (size_t i = 0; i < (size_t)w * h; i++) ((uint32_t *)(void *)dst)[i] = 0xff000000u;
#define CL(v) ((v) < 0 ? 0 : (v) > 255 ? 255 : (v))
    for (int y = 0; y < ch; y++) {
        const uint8_t *py = im->planes[0] + (size_t)y * im->stride[0], *pu = im->planes[1] + (size_t)(y >> 1) * im->stride[1], *pv = im->planes[2] + (size_t)(y >> 1) * im->stride[2];
        uint8_t *o = dst + (size_t)y * w * 4;
        for (int x = 0; x < cw; x++) {
            int l = (py[x] - y0) * Y, u = pu[x >> 1] - 128, v = pv[x >> 1] - 128;
            int r = (l + RV * v + 32768) >> 16, g = (l - GU * u - GV * v + 32768) >> 16, b = (l + BU * u + 32768) >> 16;
            o[4 * x] = (uint8_t)CL(r); o[4 * x + 1] = (uint8_t)CL(g); o[4 * x + 2] = (uint8_t)CL(b); o[4 * x + 3] = 255;
        }
    }
#undef CL
}

typedef struct { uint16_t w, h; uint32_t fmt, mode, magic; uint32_t pad[2]; } cv_pref;   /* VideoDecoderImagePreference as the host fills it (returned in x8) */
typedef struct { uint32_t magic; uint16_t w, h; uint32_t fmt, count; vpx_image_t **img; uint8_t *mem; uint64_t size; uint32_t slot; uint8_t native; } cv_pool;
typedef struct { uint32_t magic, dec, idx, gen; } cv_sync;   /* nn::gs::NativeSyncHandle (16 bytes) */
typedef struct mav1 mav1;
enum { CV_VP8, CV_VP9, CV_AV1 };
typedef struct cv_buf { cv_pool *pool; uint32_t pidx; int reg, filled; uint32_t gen, filled_gen; uint16_t w, h; int64_t ts; uint8_t eos; } cv_buf;
typedef struct {
    uint32_t magic, id; pthread_mutex_t mu; vpx_codec_ctx_t vpx; int vpx_ok; void *event; uint64_t submits, pictures;
    int codec; uint16_t w, h;
    mav1 *av1;
    cv_buf buf[CV_BUFS];
    uint32_t fifo[8], fhead, fcount;          /* acquirable indices */
    uint32_t pend[CV_BUFS], phead, pcount;    /* released, waiting for a picture */
    vpx_image_t *held[CV_HOLD]; int64_t held_ts[CV_HOLD]; uint8_t held_eos[CV_HOLD]; int nheld;
    int eos_pending;
} cv_dec;
static const char *const k_name[] = {"VP8", "VP9", "AV1"};

static cv_dec *g_cv[16];
static pthread_mutex_t g_cv_mu = PTHREAD_MUTEX_INITIALIZER;   /* lock order: g_cv_mu, then a decoder's mu, then g_spt_mu */
static cv_dec *cv_of(void *t) { cv_dec *d = t ? *(cv_dec **)t : NULL; return d && d->magic == CV_MAGIC ? d : NULL; }
static cv_pool *pool_of(const void *t) { cv_pool *p = t ? *(cv_pool *const *)t : NULL; return p && p->magic == CV_MAGIC ? p : NULL; }

/* Native image handles: (slot<<8)|picture, the slot table nn::gs hands to nvnNativeImage (as nxnative's). */
static cv_pool *g_cv_pools[256];
static uint32_t native_stride(uint32_t w) { return (w * 4 + 63) & ~63u; }

static vpx_image_t *img_copy(const vpx_image_t *im) {   /* an owned I420 copy (libvpx reuses its buffers at the next decode) */
    vpx_image_t *c = vpx_img_alloc(NULL, VPX_IMG_FMT_I420, im->d_w, im->d_h, 1);
    if (!c) return NULL;
    c->cs = im->cs; c->range = im->range;
    for (int p = 0; p < 3; p++) {
        unsigned w = p ? (im->d_w + 1) / 2 : im->d_w, h = p ? (im->d_h + 1) / 2 : im->d_h;
        for (unsigned y = 0; y < h; y++) memcpy(c->planes[p] + (size_t)y * c->stride[p], im->planes[p] + (size_t)y * im->stride[p], w);
    }
    return c;
}

/* Sync point events (nn::syncpt::sdk): the player's worker waits on one (in a MultiWait) for the sync handle of the picture it
   expects. Their library is set up by the graphics initialisation the host replaces, so CreateSyncPointEvent jumped through
   its null table. The event type holds a SystemEvent the host binds at +0 (0x30 bytes) and the awaited handle at +0x40. */
typedef struct { uint32_t magic, armed; cv_sync want; } cv_spt;
#define SPT(e) ((cv_spt *)(void *)((uint8_t *)(e) + 0x40))
static void *g_spt[64];
static pthread_mutex_t g_spt_mu = PTHREAD_MUTEX_INITIALIZER;
static bool sync_reached(const cv_sync *s);
static void spt_fire(cv_dec *d, uint32_t i) {   /* buffer i of d filled: signal the events waiting for it (decoder lock held) */
    pthread_mutex_lock(&g_spt_mu);
    for (int k = 0; k < 64; k++) { void *e = g_spt[k]; cv_spt *t = e ? SPT(e) : NULL;
        if (t && t->want.dec == d->id && t->want.idx == i && d->buf[i].filled_gen >= t->want.gen) { t->armed = 0; g_spt[k] = NULL; hle_signal_system_event(e); } }
    pthread_mutex_unlock(&g_spt_mu);
}
static void spt_unarm(void *e) { pthread_mutex_lock(&g_spt_mu); for (int k = 0; k < 64; k++) if (g_spt[k] == e) g_spt[k] = NULL; pthread_mutex_unlock(&g_spt_mu); SPT(e)->armed = 0; }

/* The GPU route the hardware decoder serves: each picture lands in the pool's own memory as tight NV12,
   which the game then copies into its textures itself (Ocarina of Time's title background reads it through
   two staging buffers). ponytail: format 3 (NV12) and one picture per pool are all any game has asked for;
   add the RGBA layout and a per-picture stride with the next one. */
static void pool_pic_write(cv_pool *p, uint32_t i, const vpx_image_t *im) {
    if (!p->mem || p->fmt != 3) return;
    if (p->native) {   // the GPU route: RGBA at the native stride, what the native image's linear texture reads
        uint32_t ns = native_stride(p->w);
        uint64_t pic = (uint64_t)ns * p->h;
        if ((uint64_t)(i + 1) * pic > p->size) return;
        uint8_t *dst = p->mem + (uint64_t)i * pic;
        if (ns == (uint32_t)p->w * 4) { yuv_rgba(im, dst, p->w, p->h); return; }   // every width asked so far is a multiple of 16
        uint8_t *tmp = malloc((size_t)p->w * p->h * 4); if (!tmp) return;          // ponytail: row scatter for wider strides; no game has hit it
        yuv_rgba(im, tmp, p->w, p->h);
        for (uint32_t y = 0; y < p->h; y++) memcpy(dst + (uint64_t)y * ns, tmp + (size_t)y * p->w * 4, (size_t)p->w * 4);
        free(tmp);
        return;
    }
    uint64_t pitch = nv12_pitch(p->w), coff = nv12_chroma(p->w, p->h), pic = nv12_size(p->w, p->h);   // the CPU route: the SDK's NV12 layout, what the game's own copies read
    if ((uint64_t)(i + 1) * pic > p->size) return;
    yuv_nv12(im, p->mem + (uint64_t)i * pic, pitch, coff, p->w, p->h);
    { static int n; if (n++ < 3) { const uint8_t *xd = p->mem + (uint64_t)i * pic; char hx[40]; for (int q = 0; q < 16; q++) sprintf(hx + q * 2, "%02x", xd[q]); LOG("TEMP: pool_pic_write #%d mem=%p i=%u first16=%s", n, (void *)p->mem, i, hx); } }   /* TEMP probe (OoT) */
}

/* Pictures and end of stream into the released buffers, in order (decoder lock held). */
static void drain(cv_dec *d) {
    while (d->pcount && (d->nheld || d->eos_pending)) {
        uint32_t i = d->pend[d->phead]; d->phead = (d->phead + 1) % CV_BUFS; d->pcount--;
        cv_buf *b = &d->buf[i];
        if (d->nheld) {
            vpx_image_t *im = d->held[0]; int64_t ts = d->held_ts[0]; uint8_t eos = d->held_eos[0];
            memmove(d->held, d->held + 1, sizeof d->held[0] * (CV_HOLD - 1)); memmove(d->held_ts, d->held_ts + 1, sizeof d->held_ts[0] * (CV_HOLD - 1));
            memmove(d->held_eos, d->held_eos + 1, CV_HOLD - 1); d->nheld--;
            b->w = (uint16_t)im->d_w; b->h = (uint16_t)im->d_h; b->ts = ts; b->eos = eos;
            if (b->pool && b->pidx < b->pool->count) { pool_pic_write(b->pool, b->pidx, im); if (b->pool->img[b->pidx]) vpx_img_free(b->pool->img[b->pidx]); b->pool->img[b->pidx] = im; } else vpx_img_free(im);
            if (!d->pictures++) LOG("%s decoder %u: first picture %ux%u into buffer %u", k_name[d->codec], d->id, b->w, b->h, i);
        } else { b->w = b->h = 0; b->ts = 0; b->eos = 1; d->eos_pending = 0; }
        if (b->eos) LOG("%s decoder %u: end of stream after %llu pictures", k_name[d->codec], d->id, (unsigned long long)d->pictures);
        b->filled = 1; b->filled_gen = b->gen;
        spt_fire(d, i);
    }
    if (d->event) { if (d->nheld < CV_HOLD) hle_signal_system_event(d->event); else clear_event(d->event); }
}

static Result cv_init(void) { return 0; }
static cv_pref cv_preference(uint16_t w, uint16_t h, uint32_t fmt, uint32_t mode) {
    { static int once; if (!once++) LOG("TEMP: MakeDecodeImagePreference raw args: %ux%u fmt=%u mode=%u", w, h, fmt, mode); }   /* TEMP probe (OoT): reconcile the format enum with the SDK layout codes */
    return (cv_pref){w, h, fmt, mode, CV_MAGIC, {0, 0}};
}
static Result cv_pool_size(uint64_t *size, uint64_t *align, const cv_pref *p, uint32_t n) {
    uint64_t one = p->fmt == 3 ? nv12_size(p->w, p->h) : align64((uint64_t)p->w * 4) * align128(p->h);   // the SDK's own image sizes: NV12 (codes 1/4) and RGBA (3/6); the game allocates exactly this
    *size = (one * (n ? n : 1) + 0xfff) & ~0xfffull; *align = 0x1000;
    return 0;
}
static Result cv_pool_init(void *t, void *mem, uint64_t size, const cv_pref *p, uint32_t n, uint32_t mode) {
    (void)mode; (void)size;
    cv_pool *o = calloc(1, sizeof *o); if (!o) runtime_fail("nn::codec image pool memory");
    o->magic = CV_MAGIC; o->w = p->w; o->h = p->h; o->fmt = p->fmt; o->count = n ? n : 1; o->img = calloc(o->count, sizeof *o->img);
    o->mem = mem; o->size = size;   // the game's pool memory: the hardware decoder writes each picture here, and the game copies it out itself
    for (uint32_t s = 0; s < 256; s++) if (!g_cv_pools[s]) { g_cv_pools[s] = o; o->slot = s; break; }   // a slot for the native-image handle
    if (!o->img) runtime_fail("nn::codec image pool memory");
    *(cv_pool **)t = o;
    static int once; if (!once++) LOG("image pool %ux%u format %u, %u picture(s): the host keeps the decoded pictures", p->w, p->h, p->fmt, o->count);
    LOG("TEMP: pool mem=%p size=%llu mode=%u fmt=%u", mem, (unsigned long long)size, p->mode, p->fmt);   /* TEMP probe (OoT) */
    return 0;
}
static void cv_pool_fin(void *t) {
    cv_pool *p = pool_of(t); if (!p) return;
    pthread_mutex_lock(&g_cv_mu);   /* a decoder still holding a buffer of this pool keeps it, with no pool to write into */
    for (int k = 0; k < 16; k++) if (g_cv[k]) { pthread_mutex_lock(&g_cv[k]->mu); for (int i = 0; i < CV_BUFS; i++) if (g_cv[k]->buf[i].pool == p) g_cv[k]->buf[i].pool = NULL; pthread_mutex_unlock(&g_cv[k]->mu); }
    pthread_mutex_unlock(&g_cv_mu);
    for (uint32_t i = 0; i < p->count; i++) if (p->img[i]) vpx_img_free(p->img[i]);
    if (p->slot < 256) g_cv_pools[p->slot] = NULL;   // the slot table must not keep a freed pool: a stale native-image handle would serve another decoder's memory
    free(p->img); p->magic = 0; free(p); *(cv_pool **)t = NULL;
}
static uint32_t cv_native(const void *t, uint32_t i) {
    cv_pool *p = pool_of(t); if (!p || i >= p->count || !p->slot) return 0;
    p->native = 1;   // the GPU route was asked for: pictures now go into pool memory as RGBA (the native image reads them with no upload)
    LOG("nn::codec: native image route served for pool %ux%u (handle %#x)", p->w, p->h, (p->slot << 8) | i);
    return (p->slot << 8) | i;
}

/* The NVN layer's bridge: a native image handle → the picture's memory, as a linear RGBA texture description. */
int dm_native_image(uint32_t handle, uint8_t **mem, uint64_t *size, uint32_t *w, uint32_t *h, uint32_t *stride) {
    uint32_t s = handle >> 8, i = handle & 0xff;
    cv_pool *p = s < 256 ? g_cv_pools[s] : NULL;
    if (!p || p->magic != CV_MAGIC || i >= p->count || !p->mem) return 0;
    uint32_t ns = native_stride(p->w);
    uint64_t pic = (uint64_t)ns * p->h;
    if ((uint64_t)i * pic + pic > p->size) return 0;
    *mem = p->mem + (uint64_t)i * pic; *size = pic; *w = p->w; *h = p->h; *stride = ns;
    return 1;
}
static void cv_dump_pic(const vpx_image_t *im) {   /* TEMP probe (OoT): save a decoded picture so we can see the decode itself */
    if (!im || !getenv("TOTK_CODEC_DUMP")) return;
    static int n; if (n++ % 30) return;   /* every 30th read: the file always holds a recent picture during the movie */
    FILE *f = fopen("/tmp/oot_frame.ppm", "wb");
    if (!f) return;
    fprintf(f, "P6\n%u\n%u\n255\n", im->d_w, im->d_h);
    for (unsigned r = 0; r < im->d_h; r++) for (unsigned c = 0; c < im->d_w; c++) {
        int y = im->planes[VPX_PLANE_Y][r * im->stride[VPX_PLANE_Y] + c];
        int u = im->planes[VPX_PLANE_U][(r >> 1) * im->stride[VPX_PLANE_U] + (c >> 1)];
        int v = im->planes[VPX_PLANE_V][(r >> 1) * im->stride[VPX_PLANE_V] + (c >> 1)];
        int rr = y + 1.3707 * (v - 128), gg = y - 0.3379 * (u - 128) - 0.6980 * (v - 128), bb = y + 1.7329 * (u - 128);
        fputc(rr < 0 ? 0 : rr > 255 ? 255 : rr, f); fputc(gg < 0 ? 0 : gg > 255 ? 255 : gg, f); fputc(bb < 0 ? 0 : bb > 255 ? 255 : bb, f);
    }
    fclose(f); LOG("TEMP: first decoded picture (%ux%u) written to /tmp/oot_frame.ppm", im->d_w, im->d_h);
}
static Result cv_read_rgba(const void *t, uint32_t i, uint16_t x, uint16_t y, uint16_t w, uint16_t h, void *dst, uint64_t stride, uint64_t size) {
    { static int once; if (!once++) LOG("TEMP: ReadDecoderOutputImageRgba #%u %ux%u stride %llu size %llu", i, w, h, (unsigned long long)stride, (unsigned long long)size); }
    cv_pool *p = pool_of(t); const vpx_image_t *im = p && i < p->count ? p->img[i] : NULL;
    cv_dump_pic(im);
    if (!im) { memset(dst, 0, size); return 0; }
    if (x || y || stride != (uint64_t)w * 4 || size < (uint64_t)w * h * 4) { static int once; if (!once++) LOG("[RED] ReadDecoderOutputImageRgba: region %u,%u stride %llu: only whole tight pictures are served", x, y, (unsigned long long)stride); return 0; }
    yuv_rgba(im, dst, w, h);
    return 0;
}
static Result cv_read_nv12(const void *t, uint32_t i, uint16_t x, uint16_t y, uint16_t w, uint16_t h, void *ydst, uint64_t ystride, uint64_t ysize, void *uvdst, uint64_t uvstride, uint64_t uvsize) {
    { static void *seen[8]; static int n, cnt; cnt++;   /* TEMP probe (OoT): log each distinct dst once */
      int dup = 0; for (int q = 0; q < n; q++) if (seen[q] == ydst) dup = 1;
      #pragma clang diagnostic push
      #pragma clang diagnostic ignored "-Wframe-address"
      if (!dup && n < 8) { seen[n++] = ydst; LOG("TEMP: ReadDecoderOutputImageNv12 dst=%p/%p strides %llu/%llu %ux%u (distinct #%d) caller=%p f1=%p f2=%p f3=%p", ydst, uvdst, (unsigned long long)ystride, (unsigned long long)uvstride, w, h, n, __builtin_return_address(0), __builtin_return_address(1), __builtin_return_address(2), __builtin_return_address(3)); }
      #pragma clang diagnostic pop
      if (cnt % 300 == 0) { void *fp = __builtin_frame_address(0); char buf[512]; int k = 0;   /* TEMP probe (OoT): manual frame walk — the guest frames have no host unwind info */
          for (int q = 0; q < 24 && fp && k < 480; q++) { void *nxt = *(void **)fp, *ra = *(void **)((char *)fp + 8); if (!ra) break; k += snprintf(buf + k, 512 - k, " %#llx", (unsigned long long)ra); if ((char *)nxt <= (char *)fp) break; fp = nxt; }
          LOG("TEMP: nv12 read #%d dst=%p fw:%s", cnt, ydst, buf); }   /* TEMP probe (OoT) */
    }
    cv_pool *p = pool_of(t); const vpx_image_t *im = p && i < p->count ? p->img[i] : NULL;
    { static int nnull, nok; if (!im) { if (!nnull++) LOG("TEMP: nv12 read NULL (pool=%p count=%d idx=%u) #1", (void *)p, p ? p->count : -1, i); } else nok++;
      if ((nnull + nok) % 300 == 0) LOG("TEMP: nv12 reads: %d ok, %d null", nok, nnull); }
    cv_dump_pic(im);
    if (x || y || ystride != w || uvstride != w || ysize < (uint64_t)w * h || uvsize < (uint64_t)w * h / 2) { static int once; if (!once++) LOG("[RED] ReadDecoderOutputImageNv12: region %u,%u strides %llu / %llu: only whole tight pictures are served", x, y, (unsigned long long)ystride, (unsigned long long)uvstride); return 0; }
    if (!im) { memset(ydst, 16, ysize); memset(uvdst, 128, uvsize); return 0; }
    if ((uint8_t *)uvdst == (uint8_t *)ydst + (size_t)w * h) { yuv_nv12(im, ydst, w, (uint64_t)w * h, w, h); return 0; }
    uint8_t *tmp = malloc((size_t)w * h * 3 / 2); if (!tmp) runtime_fail("nn::codec readback memory");
    yuv_nv12(im, tmp, w, (uint64_t)w * h, w, h);
    memcpy(ydst, tmp, (size_t)w * h); memcpy(uvdst, tmp + (size_t)w * h, (size_t)w * h / 2); free(tmp);
    return 0;
}

/* ---------- AV1 on VideoToolbox (ported from nxnative hle_movie_av1.c, MIT; its VideoToolbox backend only -
   the AV1 decoder there is hardware on Apple M3 and A17 Pro on, and this host has one. If a stream is ever
   refused, nxnative's pattern is to fall back to dav1d; add it when that happens.)
   VideoToolbox wants a format description holding the stream's av1C record: four bytes read from the sequence
   header OBU, then that header's OBU. Every key frame carries a sequence header (AV1 5.5), so it comes from the
   stream; a changed one makes a new session. A temporal unit goes in whole, its OBUs in the low-overhead format
   the SDK's demuxer keeps them in (each with its size); it shows at most one frame, in presentation order. ---------- */
typedef struct { const uint8_t *y, *uv; size_t ystride, uvstride; int w, h, matrix, full_range; } av1_pic;
struct mav1 { uint8_t seq[512]; size_t nseq; CMVideoFormatDescriptionRef fd; VTDecompressionSessionRef vs;
              CVPixelBufferRef got, locked; int red; };

/* ---- the sequence header (AV1 5.5), read as far as the colour config ---- */
typedef struct { const uint8_t *p; size_t n, bit; int bad; } av1_bits;
static uint32_t av_rb(av1_bits *b, unsigned n) {
    uint32_t v = 0;
    if (b->bit + n > b->n * 8) { b->bad = 1; return 0; }
    while (n--) { v = v << 1 | ((b->p[b->bit / 8] >> (7 - b->bit % 8)) & 1); b->bit++; }
    return v;
}
static void av_uvlc(av1_bits *b) { unsigned z = 0; while (!b->bad && !av_rb(b, 1)) if (++z >= 32) return; if (z < 32) av_rb(b, z); }
static size_t av_leb128(const uint8_t *p, size_t n, uint64_t *v) {
    *v = 0;
    for (size_t i = 0; i < n && i < 8; i++) { *v |= (uint64_t)(p[i] & 0x7f) << (i * 7); if (!(p[i] & 0x80)) return i + 1; }
    return 0;
}
static int av_av1c(const uint8_t *obu, size_t n, uint8_t c[4], int *w, int *h) {
    if (n < 2 || (obu[0] >> 3 & 15) != 1 || !(obu[0] & 2)) return 0;   /* a sequence header with a size field */
    size_t at = obu[0] & 4 ? 2 : 1; uint64_t size;
    size_t l = at < n ? av_leb128(obu + at, n - at, &size) : 0;
    if (!l || size > n - at - l) return 0;
    av1_bits b = {obu + at + l, (size_t)size, 0, 0};
    unsigned profile = av_rb(&b, 3); av_rb(&b, 1); unsigned reduced = av_rb(&b, 1), level = 0, tier = 0;
    unsigned decoder_model = 0, delay_len = 0;
    if (reduced) level = av_rb(&b, 5);
    else {
        if (av_rb(&b, 1)) {                       /* timing_info */
            av_rb(&b, 32); av_rb(&b, 32); if (av_rb(&b, 1)) av_uvlc(&b);
            if ((decoder_model = av_rb(&b, 1))) { delay_len = av_rb(&b, 5) + 1; av_rb(&b, 32); av_rb(&b, 5); av_rb(&b, 5); }
        }
        unsigned display_delay = av_rb(&b, 1), points = av_rb(&b, 5) + 1;
        for (unsigned i = 0; i < points && !b.bad; i++) {
            av_rb(&b, 12); unsigned lv = av_rb(&b, 5), t = lv > 7 ? av_rb(&b, 1) : 0;
            if (!i) { level = lv; tier = t; }
            if (decoder_model && av_rb(&b, 1)) { av_rb(&b, delay_len); av_rb(&b, delay_len); av_rb(&b, 1); }
            if (display_delay && av_rb(&b, 1)) av_rb(&b, 4);
        }
    }
    unsigned wb = av_rb(&b, 4) + 1, hb = av_rb(&b, 4) + 1;
    *w = (int)av_rb(&b, wb) + 1; *h = (int)av_rb(&b, hb) + 1;
    if (!reduced && av_rb(&b, 1)) { av_rb(&b, 4); av_rb(&b, 3); }   /* frame ids */
    av_rb(&b, 3);                                                   /* superblocks, filter intra, intra edge filter */
    if (!reduced) {
        av_rb(&b, 4);                                               /* inter-intra, masked compound, warped, dual filter */
        unsigned order_hint = av_rb(&b, 1);
        if (order_hint) av_rb(&b, 2);                               /* jnt_comp, ref_frame_mvs */
        unsigned tools = av_rb(&b, 1) ? 2 : av_rb(&b, 1);
        if (tools && !av_rb(&b, 1)) av_rb(&b, 1);                   /* force_integer_mv */
        if (order_hint) av_rb(&b, 3);
    }
    av_rb(&b, 3);                                                   /* superres, cdef, restoration */
    unsigned hbd = av_rb(&b, 1), twelve = profile == 2 && hbd ? av_rb(&b, 1) : 0, mono = profile == 1 ? 0 : av_rb(&b, 1), cp = 2, tc = 2, mc = 2, ssx = 1, ssy = 1, pos = 0;
    if (av_rb(&b, 1)) { cp = av_rb(&b, 8); tc = av_rb(&b, 8); mc = av_rb(&b, 8); }
    if (!mono) {
        if (cp == 1 && tc == 13 && mc == 0) ssx = ssy = 0;          /* sRGB */
        else {
            av_rb(&b, 1);                                          /* colour range */
            if (profile == 1) ssx = ssy = 0;
            else if (profile == 2) { if (twelve) { ssx = av_rb(&b, 1); ssy = ssx ? av_rb(&b, 1) : 0; } else { ssx = 1; ssy = 0; } }
            if (ssx && ssy) pos = av_rb(&b, 2);
        }
    }
    if (b.bad) return 0;
    c[0] = 0x81;                                                   /* marker, version 1 */
    c[1] = (uint8_t)(profile << 5 | (level & 31));
    c[2] = (uint8_t)(tier << 7 | hbd << 6 | twelve << 5 | mono << 4 | ssx << 3 | ssy << 2 | pos);
    c[3] = 0;
    return 1;
}
static const uint8_t *av_find_seq(const uint8_t *p, size_t n, size_t *len) {
    for (size_t i = 0; i < n;) {
        if (!(p[i] & 2)) return NULL;                              /* no size field: not the low-overhead format */
        size_t hdr = p[i] & 4 ? 2 : 1; uint64_t size;
        size_t l = i + hdr < n ? av_leb128(p + i + hdr, n - i - hdr, &size) : 0;
        if (!l || size > n - i - hdr - l) return NULL;
        if ((p[i] >> 3 & 15) == 1) { *len = hdr + l + (size_t)size; return p + i; }
        i += hdr + l + (size_t)size;
    }
    return NULL;
}
static void av_on_frame(void *ref, void *src, OSStatus st, VTDecodeInfoFlags fl, CVImageBufferRef img, CMTime pts, CMTime dur) {
    (void)src; (void)fl; (void)pts; (void)dur; mav1 *a = ref;
    if (st || !img) { if (st && !a->red++) LOG("[RED] AV1: VideoToolbox decode error (%d, first case)", (int)st); return; }
    if (a->got) CVPixelBufferRelease(a->got);
    a->got = CVPixelBufferRetain(img);
}
static void av_close(mav1 *a) {
    if (a->vs) { VTDecompressionSessionWaitForAsynchronousFrames(a->vs); VTDecompressionSessionInvalidate(a->vs); CFRelease(a->vs); a->vs = NULL; }
    if (a->fd) { CFRelease(a->fd); a->fd = NULL; }
    if (a->got) { CVPixelBufferRelease(a->got); a->got = NULL; }
}
static int av_open(mav1 *a, const uint8_t *seq, size_t n) {
    av_close(a);
    uint8_t rec[4 + sizeof a->seq]; int w = 0, h = 0;
    if (n > sizeof a->seq || !av_av1c(seq, n, rec, &w, &h)) { if (!a->red++) LOG("[RED] AV1: a sequence header the host cannot read (%zu bytes)", n); return 0; }
    memcpy(rec + 4, seq, n);
    CFDataRef data = CFDataCreate(NULL, rec, (CFIndex)(4 + n));
    const void *ak[] = {CFSTR("av1C")}, *av[] = {data};
    CFDictionaryRef atoms = CFDictionaryCreate(NULL, ak, av, 1, &kCFTypeDictionaryKeyCallBacks, &kCFTypeDictionaryValueCallBacks); CFRelease(data);
    const void *ek[] = {kCMFormatDescriptionExtension_SampleDescriptionExtensionAtoms}, *ev[] = {atoms};
    CFDictionaryRef ext = CFDictionaryCreate(NULL, ek, ev, 1, &kCFTypeDictionaryKeyCallBacks, &kCFTypeDictionaryValueCallBacks); CFRelease(atoms);
    OSStatus st = CMVideoFormatDescriptionCreate(NULL, kCMVideoCodecType_AV1, w, h, ext, &a->fd); CFRelease(ext);
    if (st) { if (!a->red++) LOG("[RED] AV1: no format description (%d)", (int)st); return 0; }
    int32_t pf = kCVPixelFormatType_420YpCbCr8BiPlanarVideoRange;
    CFNumberRef num = CFNumberCreate(NULL, kCFNumberSInt32Type, &pf);
    const void *k[] = {kCVPixelBufferPixelFormatTypeKey}, *v[] = {num};
    CFDictionaryRef attrs = CFDictionaryCreate(NULL, k, v, 1, &kCFTypeDictionaryKeyCallBacks, &kCFTypeDictionaryValueCallBacks); CFRelease(num);
    VTDecompressionOutputCallbackRecord cb = {av_on_frame, a};
    st = VTDecompressionSessionCreate(NULL, a->fd, NULL, attrs, &cb, &a->vs); CFRelease(attrs);
    if (st) { if (!a->red++) LOG("[RED] AV1: VideoToolbox cannot make a session (%d)", (int)st); a->vs = NULL; return 0; }
    memcpy(a->seq, seq, n); a->nseq = n;
    static int once; if (!once++) LOG("[ns2-codec] AV1: VideoToolbox decoding %dx%d, profile %d level %d", w, h, rec[1] >> 5, rec[1] & 31);
    return 1;
}
static mav1 *av_new(void) { return calloc(1, sizeof(mav1)); }
static void av_unlock(mav1 *a) {
    if (a->locked) { CVPixelBufferUnlockBaseAddress(a->locked, kCVPixelBufferLock_ReadOnly); CVPixelBufferRelease(a->locked); a->locked = NULL; }
}
static void av_reset(mav1 *a) { av_unlock(a); av_close(a); a->nseq = 0; }
static void av_free(mav1 *a) { if (a) { av_reset(a); free(a); } }
/* one temporal unit in; 1 with *out set: a picture (NV12, valid until the next call); 0: nothing to show; -1: refused */
static int av_decode(mav1 *a, const uint8_t *data, size_t n, av1_pic *out) {
    av_unlock(a);
    size_t seqn = 0; const uint8_t *seq = av_find_seq(data, n, &seqn);
    if (seq && (seqn != a->nseq || memcmp(seq, a->seq, seqn) || !a->vs) && !av_open(a, seq, seqn)) return -1;
    if (!a->vs) return 0;                                          /* the first sequence header has not come */
    CMBlockBufferRef bb = NULL; CMSampleBufferRef sb = NULL; size_t ssz = n;
    OSStatus st = CMBlockBufferCreateWithMemoryBlock(NULL, NULL, n, NULL, NULL, 0, n, 0, &bb);
    if (!st) st = CMBlockBufferReplaceDataBytes(data, bb, 0, n);
    if (!st) st = CMSampleBufferCreateReady(NULL, bb, a->fd, 1, 0, NULL, 1, &ssz, &sb);
    if (!st) st = VTDecompressionSessionDecodeFrame(a->vs, sb, 0, NULL, NULL);   /* flags 0: synchronous */
    if (sb) CFRelease(sb);
    if (bb) CFRelease(bb);
    if (st) { if (!a->red++) LOG("[RED] AV1: decode submission failed (%d, first case)", (int)st); return -1; }
    if (!a->got) return 0;
    CVPixelBufferRef pb = a->got; a->got = NULL; a->locked = pb;
    CVPixelBufferLockBaseAddress(pb, kCVPixelBufferLock_ReadOnly);
    CFStringRef m = CVBufferCopyAttachment(pb, kCVImageBufferYCbCrMatrixKey, NULL);
    *out = (av1_pic){CVPixelBufferGetBaseAddressOfPlane(pb, 0), CVPixelBufferGetBaseAddressOfPlane(pb, 1),
                     CVPixelBufferGetBytesPerRowOfPlane(pb, 0), CVPixelBufferGetBytesPerRowOfPlane(pb, 1),
                     (int)CVPixelBufferGetWidthOfPlane(pb, 0), (int)CVPixelBufferGetHeightOfPlane(pb, 0),
                     !m || CFEqual(m, kCVImageBufferYCbCrMatrix_ITU_R_709_2) ? 2 : CFEqual(m, kCVImageBufferYCbCrMatrix_ITU_R_2020) ? 5 : 1, 0};
    if (m) CFRelease(m);
    return 1;
}
/* an NV12 picture as the owned I420 the rest of this file works in */
static vpx_image_t *nv12_copy(const av1_pic *p) {
    vpx_image_t *c = vpx_img_alloc(NULL, VPX_IMG_FMT_I420, (unsigned)p->w, (unsigned)p->h, 1);
    if (!c) return NULL;
    c->cs = p->matrix == 5 ? VPX_CS_BT_2020 : p->matrix == 1 ? VPX_CS_BT_601 : VPX_CS_BT_709;
    c->range = p->full_range;
    for (int y = 0; y < p->h; y++) memcpy(c->planes[0] + (size_t)y * c->stride[0], p->y + (size_t)y * p->ystride, (size_t)p->w);
    unsigned cw = (unsigned)(p->w + 1) / 2, ch = (unsigned)(p->h + 1) / 2;
    for (unsigned r = 0; r < ch; r++) {
        const uint8_t *uv = p->uv + (size_t)r * p->uvstride;
        uint8_t *u = c->planes[1] + (size_t)r * c->stride[1], *v = c->planes[2] + (size_t)r * c->stride[2];
        for (unsigned x = 0; x < cw; x++) { u[x] = uv[2 * x]; v[x] = uv[2 * x + 1]; }
    }
    return c;
}

static const vpx_codec_iface_t *cv_iface(int codec) {
    if (codec == CV_VP8) return vpx_codec_vp8_dx();
    if (codec == CV_VP9) return vpx_codec_vp9_dx();
#if defined(__has_include) && __has_include("vpx/dx_av1.h")
    return vpx_codec_av1_dx();
#else
    return NULL;   /* ponytail: the homebrew libvpx bottle is built --disable-av1. If the AV1 Break proves fatal,
                      link libaom (same API shape) or VideoToolbox av01 - decided by the Break analysis. */
#endif
}
static void cv_open(cv_dec *d) {   /* (re)creates the codec state: a new stream starts at the next key frame */
    if (d->vpx_ok) vpx_codec_destroy(&d->vpx);
    if (d->codec == CV_AV1) { if (!d->av1) d->av1 = av_new(); else av_reset(d->av1); d->vpx_ok = d->av1 != NULL; if (!d->vpx_ok) LOG("[RED] AV1 decoder: no host decoder could be made"); return; }
    vpx_codec_dec_cfg_t cfg = {.threads = 2, .w = d->w, .h = d->h};
    const vpx_codec_iface_t *iface = cv_iface(d->codec);
    d->vpx_ok = iface && vpx_codec_dec_init(&d->vpx, iface, &cfg, 0) == VPX_CODEC_OK;
    if (!d->vpx_ok) LOG("[RED] %s decoder: libvpx init failed", k_name[d->codec]);
}
static Result cv_dec_init(void *t, int codec, uint16_t w, uint16_t h, uint32_t in, uint32_t out) {
    cv_dec *d = calloc(1, sizeof *d); if (!d) runtime_fail("nn::codec decoder memory");
    d->magic = CV_MAGIC; d->codec = codec; d->w = w; d->h = h; pthread_mutex_init(&d->mu, NULL);
    cv_open(d);
    pthread_mutex_lock(&g_cv_mu);
    for (int i = 0; i < 16; i++) if (!g_cv[i]) { g_cv[i] = d; d->id = (uint32_t)i + 1; break; }
    pthread_mutex_unlock(&g_cv_mu);
    if (!d->id) runtime_fail("nn::codec: more than 16 video decoders");
    d->event = (uint8_t *)t + CV_EVENT_OFF; hle_bind_system_event(d->event, 0); hle_signal_system_event(d->event);
    *(cv_dec **)t = d;
    /* The movie library dispatches its event getters on the codec kind the real Initialize leaves at [type+0x400]. */
    static const uint32_t k_nn_kind[] = {4, 0, 1};   /* nn::codec: VP8=4, VP9=0, AV1=1 */
    *(uint32_t *)((uint8_t *)t + 0x400) = k_nn_kind[codec];
    LOG("%s decoder %u: %ux%u, input format %u, output format %u (libvpx on the host)", k_name[codec], d->id, w, h, in, out);
    return 0;
}
static Result cv_dec_fin(void *t) {
    cv_dec *d = cv_of(t); if (!d) return 0;
    pthread_mutex_lock(&g_cv_mu); g_cv[d->id - 1] = NULL; pthread_mutex_lock(&d->mu); pthread_mutex_unlock(&d->mu); pthread_mutex_unlock(&g_cv_mu);
    if (d->vpx_ok) vpx_codec_destroy(&d->vpx);
    if (d->av1) av_free(d->av1);
    for (int i = 0; i < d->nheld; i++) vpx_img_free(d->held[i]);
    d->magic = 0;
    /* The submission-ready event goes with the decoder, as in the SDK: kept, it stayed bound, and the next decoder made in the
       same (cleared) memory found it "bound" and got none; nn::os aborted on it (nxnative, Bananza's second movie). */
    destroy_event(d->event);
    pthread_mutex_destroy(&d->mu); free(d); *(cv_dec **)t = NULL;
    return 0;
}
static Result cv_dec_reg(void *t, uint32_t i, void *pool, uint32_t pidx) {
    cv_dec *d = cv_of(t); if (!d || i >= CV_BUFS) { LOG("[RED] Register...DecoderBuffer index %u", i); return CV_R(2004); }
    pthread_mutex_lock(&d->mu);
    d->buf[i] = (cv_buf){.pool = pool_of(pool), .pidx = pidx, .reg = 1};
    if (pool && !d->buf[i].pool) { static int once; if (!once++) LOG("[RED] RegisterDecoderBuffer: the pool handle %p is not one of ours; its pictures will be reported blank", pool); }   // a silent black screen is the worst failure mode to debug
    if (d->fcount < 8) { d->fifo[(d->fhead + d->fcount) % 8] = i; d->fcount++; } else LOG("[RED] acquirable queue full at register %u", i);
    pthread_mutex_unlock(&d->mu);
    return 0;
}
static void drop_index(uint32_t *q, uint32_t *head, uint32_t *count, uint32_t cap, uint32_t i) {   /* removes i from a ring, keeping the others' order */
    uint32_t n = 0, tmp[CV_BUFS];
    for (uint32_t k = 0; k < *count; k++) { uint32_t v = q[(*head + k) % cap]; if (v != i) tmp[n++] = v; }
    for (uint32_t k = 0; k < n; k++) q[k] = tmp[k];
    *head = 0; *count = n;
}
static Result cv_dec_unreg(void *t, uint32_t i) {
    cv_dec *d = cv_of(t); if (!d || i >= CV_BUFS) return CV_R(2004);
    pthread_mutex_lock(&d->mu);
    d->buf[i].reg = 0; d->buf[i].filled = 0; d->buf[i].pool = NULL; d->buf[i].gen++;
    drop_index(d->fifo, &d->fhead, &d->fcount, 8, i); drop_index(d->pend, &d->phead, &d->pcount, CV_BUFS, i);
    pthread_mutex_unlock(&d->mu);
    return 0;
}
static Result cv_dec_reset(void *t) {   /* Stop / Flush: undelivered pictures and a pending end of stream are dropped; next key frame starts over */
    cv_dec *d = cv_of(t); if (!d) return 0;
    pthread_mutex_lock(&d->mu);
    for (int i = 0; i < d->nheld; i++) vpx_img_free(d->held[i]);
    d->nheld = 0; d->eos_pending = 0;
    cv_open(d);
    if (d->event) hle_signal_system_event(d->event);
    pthread_mutex_unlock(&d->mu);
    return 0;
}
static Result cv_dec_event(void **out, void *t) { cv_dec *d = cv_of(t); *out = d ? d->event : NULL; return d ? 0 : CV_R(2004); }
/* SDK 22 renamed this to a one-argument getter returning the event pointer; the movie library calls only this form. */
static void *cv_dec_event_ptr(void *t) { cv_dec *d = cv_of(t); return d ? d->event : NULL; }
static int cv_hold(cv_dec *d, vpx_image_t *c, int64_t ts) {
    if (d->nheld >= CV_HOLD) { static int n; if (n++ < 4) LOG("[RED] %s: a packet gave more pictures than the host holds: one dropped", k_name[d->codec]); vpx_img_free(c); return 0; }
    d->held[d->nheld] = c; d->held_ts[d->nheld] = ts; d->held_eos[d->nheld] = 0; d->nheld++; return 1;
}
static Result cv_dec_submit(uint64_t *out, void *t, const void *data, uint64_t size, int64_t ts, bool eos) {
    cv_dec *d = cv_of(t); if (!d) return CV_R(2004);
    pthread_mutex_lock(&d->mu);
    if (d->nheld >= CV_HOLD) { if (d->event) clear_event(d->event); pthread_mutex_unlock(&d->mu); return CV_BUSY; }
    int got = 0;
    if (size && d->vpx_ok && d->codec == CV_AV1) {
        av1_pic pic; int r = av_decode(d->av1, data, (size_t)size, &pic);
        if (r == 1) { vpx_image_t *c = nv12_copy(&pic); if (c) got += cv_hold(d, c, ts); }
        else if (r < 0) { /* av_decode logged the first refusal */ }
    } else if (size && d->vpx_ok) {
        if (vpx_codec_decode(&d->vpx, data, (unsigned)size, NULL, 0) != VPX_CODEC_OK) { static int n; if (n++ < 4) LOG("[RED] %s: %s", k_name[d->codec], vpx_codec_error(&d->vpx)); }
        vpx_codec_iter_t it = NULL; const vpx_image_t *im;
        while ((im = vpx_codec_get_frame(&d->vpx, &it))) { vpx_image_t *c = img_copy(im); if (c) got += cv_hold(d, c, ts); }
    }
    if (eos) { if (got) d->held_eos[d->nheld - 1] = 1; else d->eos_pending = 1; }
    *out = ++d->submits;
    drain(d);
    pthread_mutex_unlock(&d->mu);
    return 0;
}
static Result cv_dec_acq(uint32_t *idx, cv_sync *sync, void *t) {
    cv_dec *d = cv_of(t); if (!d) return CV_R(2004);
    pthread_mutex_lock(&d->mu);
    if (!d->fcount) { pthread_mutex_unlock(&d->mu); return CV_NONE; }
    uint32_t i = d->fifo[d->fhead]; d->fhead = (d->fhead + 1) % 8; d->fcount--;
    *idx = i; *sync = (cv_sync){CV_SYNC, d->id, i, d->buf[i].gen};
    pthread_mutex_unlock(&d->mu);
    return 0;
}
static void cv_dec_rel(void *t, uint32_t i, cv_sync sync) {
    (void)sync; cv_dec *d = cv_of(t); if (!d || i >= CV_BUFS || !d->buf[i].reg) { LOG("[RED] Release...DecoderOutputBuffer index %u", i); return; }
    pthread_mutex_lock(&d->mu);
    d->buf[i].gen++; d->buf[i].filled = 0;
    if (d->pcount < CV_BUFS) { d->pend[(d->phead + d->pcount) % CV_BUFS] = i; d->pcount++; }
    if (d->fcount < 8) { d->fifo[(d->fhead + d->fcount) % 8] = i; d->fcount++; } else LOG("[RED] acquirable queue full at release %u", i);
    drain(d);
    pthread_mutex_unlock(&d->mu);
}
static Result cv_dec_props(uint16_t *w, uint16_t *h, uint16_t *cw, uint16_t *ch, int64_t *ts, bool *eos, void *t, uint32_t i) {
    cv_dec *d = cv_of(t); if (!d || i >= CV_BUFS) return CV_R(2004);
    pthread_mutex_lock(&d->mu);
    int ok = d->buf[i].filled;
    if (ok) { *w = *cw = d->buf[i].w; *h = *ch = d->buf[i].h; *ts = d->buf[i].ts; *eos = d->buf[i].eos != 0; }
    pthread_mutex_unlock(&d->mu);
    return ok ? 0 : CV_NONE;
}
static Result cv_dec_encrypted(void) { static int once; if (!once++) LOG("[RED] an encrypted input buffer: protected streams are not served"); return CV_R(2004); }

/* One family of entry points per codec (the SDK's Vp8 / Vp9DecoderType share the layout). */
#define CV_FAMILY(tag, codec) \
    static Result tag##_work(uint64_t *size, uint64_t *align, uint64_t bs, uint16_t w, uint16_t h) { (void)bs; (void)w; (void)h; *size = 0x1000; *align = 0x1000; return 0; } \
    static Result tag##_init(void *t, void *work, uint64_t wsize, uint64_t bs, uint16_t w, uint16_t h, uint32_t refs, uint32_t in, uint32_t out) { (void)work; (void)wsize; (void)bs; (void)refs; return cv_dec_init(t, codec, w, h, in, out); } \
    static Result tag##_init2(void *t, uint64_t bs, uint16_t w, uint16_t h, uint32_t refs, uint32_t in, uint32_t out) { (void)bs; (void)refs; return cv_dec_init(t, codec, w, h, in, out); } \
    __attribute__((unused)) static Result tag##_fin(void *t) { return cv_dec_fin(t); } \
    __attribute__((unused)) static Result tag##_reg(void *t, uint32_t i, void *pool, uint32_t pidx) { return cv_dec_reg(t, i, pool, pidx); } \
    __attribute__((unused)) static Result tag##_unreg(void *t, uint32_t i) { return cv_dec_unreg(t, i); } \
    __attribute__((unused)) static Result tag##_start(void *t) { (void)t; return 0; } \
    __attribute__((unused)) static Result tag##_reset(void *t) { return cv_dec_reset(t); } \
    __attribute__((unused)) static Result tag##_event(void **out, void *t) { return cv_dec_event(out, t); } \
    __attribute__((unused)) static Result tag##_submit(uint64_t *out, void *t, const void *data, uint64_t size, int64_t ts, bool eos) { return cv_dec_submit(out, t, data, size, ts, eos); } \
    __attribute__((unused)) static Result tag##_acq(uint32_t *idx, cv_sync *sync, void *t) { return cv_dec_acq(idx, sync, t); } \
    __attribute__((unused)) static void tag##_rel(void *t, uint32_t i, cv_sync sync) { cv_dec_rel(t, i, sync); } \
    __attribute__((unused)) static Result tag##_props(uint16_t *w, uint16_t *h, uint16_t *cw, uint16_t *ch, int64_t *ts, bool *eos, void *t, uint32_t i) { return cv_dec_props(w, h, cw, ch, ts, eos, t, i); }
CV_FAMILY(vp9, CV_VP9)
CV_FAMILY(vp8, CV_VP8)
/* AV1: VideoToolbox decodes it (the homebrew libvpx bottle is --disable-av1), so the opening movie plays; the game
   Breaks on a failed Initialize rather than skipping the movie, so failing cleanly is not an option. */
CV_FAMILY(av1, CV_AV1)
#undef CV_FAMILY
/* H264: same - no host decoder; the SDK's own Initialize reaches the gs/decoder service and Breaks here. */
static Result h264_init(void *t) { *(void **)t = NULL; static int once; if (!once++) LOG("[RED] H264 decoder: no H264 decoder on the host, Initialize fails"); return CV_R(2004); }

/* ---------- nn::codec::AacDecoder: the movie library's audio track (nn::movie::AudioDecoder wraps it) ----------
   The SDK's AAC client reaches the gsv codec service like its video clients do, so it fails here and nn::movie aborts
   on the null event. The game hands us the stream's rate and channels at Initialize and reads them back to set up its
   audio pipeline, so answer those and decode silence: the frames keep the movie's audio-video pacing.
   ponytail: real AAC through AudioToolbox AudioConverter (synthesized ASConfig) when the movie needs to be heard. */
struct aac_dec { void *t; int rate, ch; };
static struct aac_dec g_aac[8];
static struct aac_dec *aac_of(void *t) { for (int i = 0; i < 8; i++) if (g_aac[i].t == t) return &g_aac[i]; return NULL; }
static Result aac_init(void *t, int rate, int ch, void *work, uint64_t size) {
    (void)work; (void)size;
    struct aac_dec *a = aac_of(t);
    if (!a) for (int i = 0; i < 8; i++) if (!g_aac[i].t) { a = &g_aac[i]; break; }
    if (!a) { LOG("[RED] AacDecoder: more than 8 live decoders"); return CV_R(2004); }
    a->t = t; a->rate = rate; a->ch = ch;
    LOG("AAC decoder %p: %d Hz, %d ch (the host decodes silence for now)", t, rate, ch);
    return 0;
}
static Result aac_decode(void *t, uint64_t *consumed, int *samples, int16_t *out, uint64_t out_size, const void *data, uint64_t size) {
    (void)data;
    struct aac_dec *a = aac_of(t); if (!a) return CV_R(2002);
    int n = 1024;                                     /* one AAC frame */
    uint64_t bytes = (uint64_t)n * (a->ch > 0 ? a->ch : 2) * 2;
    if (bytes > out_size) { n = (int)(out_size / ((a->ch > 0 ? a->ch : 2) * 2)); bytes = out_size; }
    memset(out, 0, bytes);
    if (consumed) *consumed = size; if (samples) *samples = n;
    return 0;
}
static int aac_rate(const void *t) { struct aac_dec *a = aac_of((void *)t); return a ? a->rate : 0; }
static int aac_ch(const void *t) { struct aac_dec *a = aac_of((void *)t); return a ? a->ch : 0; }
static int aac_objtype(const void *t) { return aac_of((void *)t) ? 2 : 0; }   /* AAC-LC */
static void aac_fin(void *t) { for (int i = 0; i < 8; i++) if (g_aac[i].t == t) g_aac[i] = (struct aac_dec){0}; }

/* The host's sync handles signal when their buffer holds the picture of that release; a handle the host did not make reads as
   signalled. */
static bool sync_reached(const cv_sync *s) {
    if (s->magic != CV_SYNC) { static int once; if (!once++ && (s->magic || s->dec)) LOG("[RED] a sync handle the host did not make (%#x): signalled", s->magic); return true; }
    if (!s->gen) return true;
    pthread_mutex_lock(&g_cv_mu);
    cv_dec *d = s->dec && s->dec <= 16 ? g_cv[s->dec - 1] : NULL;
    bool r = true;
    if (d && s->idx < CV_BUFS) { pthread_mutex_lock(&d->mu); r = d->buf[s->idx].filled && d->buf[s->idx].filled_gen >= s->gen; pthread_mutex_unlock(&d->mu); }
    pthread_mutex_unlock(&g_cv_mu);
    return r;
}
static bool gs_try_wait(const cv_sync *s) { return sync_reached(s); }
static Result spt_create(void *e) { memset(e, 0, 0x58); hle_bind_system_event(e, 0); SPT(e)->magic = CV_SYNC; return 0; }
static void spt_holder(void *holder, void *e) {
    static void (*init)(void *, void *);
    if (!init && !(init = guest_sym("_ZN2nn2os25InitializeMultiWaitHolderEPNS0_19MultiWaitHolderTypeEPNS0_15SystemEventTypeE"))) runtime_fail("nn::os::InitializeMultiWaitHolder missing");
    init(holder, e);
}
static Result spt_request(void *e, const cv_sync *s) {
    spt_unarm(e); clear_event(e);
    SPT(e)->want = *s; SPT(e)->armed = 1;
    pthread_mutex_lock(&g_spt_mu); int k = 0; while (k < 64 && g_spt[k]) k++; if (k < 64) g_spt[k] = e; pthread_mutex_unlock(&g_spt_mu);
    if (k == 64) { LOG("[RED] nn::syncpt: more than 64 armed events: signalled at once"); hle_signal_system_event(e); return 0; }
    if (sync_reached(s)) { spt_unarm(e); hle_signal_system_event(e); }
    return 0;
}
static void spt_cancel(void *e) { spt_unarm(e); clear_event(e); }
static void spt_destroy(void *e) { spt_unarm(e); destroy_event(e); SPT(e)->magic = 0; }

#define VP(codec, tag) \
    {"_ZN2nn5codec29Calculate" codec "DecoderWorkBufferEPmS1_mtt", tag##_work}, \
    {"_ZN2nn5codec20Initialize" codec "DecoderEPNS0_14" codec "DecoderTypeEPvmmttjNS0_23VideoDecoderInputFormatENS0_24VideoDecoderOutputFormatE", tag##_init}, \
    {"_ZN2nn5codec20Initialize" codec "DecoderEPNS0_14" codec "DecoderTypeEmttjNS0_23VideoDecoderInputFormatENS0_24VideoDecoderOutputFormatE", tag##_init2}, \
    {"_ZN2nn5codec18Finalize" codec "DecoderEPNS0_14" codec "DecoderTypeE", tag##_fin}, \
    {"_ZN2nn5codec24Register" codec "DecoderBufferEPNS0_14" codec "DecoderTypeEjPNS0_25VideoDecoderImagePoolTypeEj", tag##_reg}, \
    {"_ZN2nn5codec26Unregister" codec "DecoderBufferEPNS0_14" codec "DecoderTypeEj", tag##_unreg}, \
    {"_ZN2nn5codec15Start" codec "DecoderEPNS0_14" codec "DecoderTypeE", tag##_start}, \
    {"_ZN2nn5codec14Stop" codec "DecoderEPNS0_14" codec "DecoderTypeE", tag##_reset}, \
    {"_ZN2nn5codec15Flush" codec "DecoderEPNS0_14" codec "DecoderTypeE", tag##_reset}, \
    {"_ZN2nn5codec33Get" codec "DecoderSubmissionReadyEventEPPNS_2os15SystemEventTypeEPNS0_14" codec "DecoderTypeE", tag##_event}, \
    {"_ZN2nn5codec27Submit" codec "DecoderInputBufferEPmPNS0_14" codec "DecoderTypeEPKvmNS_8TimeSpanEb", tag##_submit}, \
    {"_ZN2nn5codec36SubmitEncrypted" codec "DecoderInputBufferEPmPNS0_14" codec "DecoderTypeEPKNS0_18EncryptionPropertyEPKvmNS_8TimeSpanEb", cv_dec_encrypted}, \
    {"_ZN2nn5codec29Acquire" codec "DecoderOutputBufferEPjPNS_2gs16NativeSyncHandleEPNS0_14" codec "DecoderTypeE", tag##_acq}, \
    {"_ZN2nn5codec29Release" codec "DecoderOutputBufferEPNS0_14" codec "DecoderTypeEjNS_2gs16NativeSyncHandleE", tag##_rel}, \
    {"_ZN2nn5codec35Get" codec "DecoderOutputBufferPropertiesEPtS1_S1_S1_PNS_8TimeSpanEPbPNS0_14" codec "DecoderTypeEj", tag##_props}

void *runtime_ns2_codec_override(const char *name) {
    static const struct { const char *name; void *fn; } map[] = {
        {"_ZN2nn5codec5video10InitializeEv", cv_init}, {"_ZN2nn5codec5video8FinalizeEv", cv_init},
        {"_ZN2nn5codec25MakeDecodeImagePreferenceEttNS0_24VideoDecoderOutputFormatENS0_9CodecModeE", cv_preference},
        {"_ZN2nn5codec31CalculateDecoderImagePoolBufferEPmS1_RKNS0_27VideoDecoderImagePreferenceEj", cv_pool_size},
        {"_ZN2nn5codec26InitializeDecoderImagePoolEPNS0_25VideoDecoderImagePoolTypeEPvmRKNS0_27VideoDecoderImagePreferenceEjNS0_9CodecModeE", cv_pool_init},
        {"_ZN2nn5codec24FinalizeDecoderImagePoolEPNS0_25VideoDecoderImagePoolTypeE", cv_pool_fin},
        {"_ZN2nn5codec36GetDecoderImagePoolNativeImageHandleEPKNS0_25VideoDecoderImagePoolTypeEj", cv_native},
        {"_ZN2nn5codec26ReadDecoderOutputImageRgbaEPKNS0_25VideoDecoderImagePoolTypeEjttttPvmm", cv_read_rgba},
        {"_ZN2nn5codec26ReadDecoderOutputImageNv12EPKNS0_25VideoDecoderImagePoolTypeEjttttPvmmS4_mm", cv_read_nv12},
        VP("Vp9", vp9), VP("Vp8", vp8), VP("Av1", av1),
        /* SDK 22's one-argument event getters (the form the Switch 2 movie library imports). */
        {"_ZN2nn5codec33GetAv1DecoderSubmissionReadyEventEPNS0_14Av1DecoderTypeE", cv_dec_event_ptr},
        {"_ZN2nn5codec33GetVp9DecoderSubmissionReadyEventEPNS0_14Vp9DecoderTypeE", cv_dec_event_ptr},
        {"_ZN2nn5codec33GetVp8DecoderSubmissionReadyEventEPNS0_14Vp8DecoderTypeE", cv_dec_event_ptr},
        {"_ZN2nn5codec34GetH264DecoderSubmissionReadyEventEPNS0_15H264DecoderTypeE", cv_dec_event_ptr},
        /* SDK 22 renamed the initializers to By{Transfer,Remote}Memory and added an OutputMode tail argument (and H264
           two more); the Switch 2 movie library calls only these. Same implementations - the extra arguments sit in
           registers the C functions never read. */
        {"_ZN2nn5codec36InitializeVp9DecoderByTransferMemoryEPNS0_14Vp9DecoderTypeEPvmmttjNS0_23VideoDecoderInputFormatENS0_24VideoDecoderOutputFormatENS0_22VideoDecoderOutputModeE", vp9_init},
        {"_ZN2nn5codec34InitializeVp9DecoderByRemoteMemoryEPNS0_14Vp9DecoderTypeEmttjNS0_23VideoDecoderInputFormatENS0_24VideoDecoderOutputFormatENS0_22VideoDecoderOutputModeE", vp9_init2},
        {"_ZN2nn5codec36InitializeVp8DecoderByTransferMemoryEPNS0_14Vp8DecoderTypeEPvmmttjNS0_23VideoDecoderInputFormatENS0_24VideoDecoderOutputFormatENS0_22VideoDecoderOutputModeE", vp8_init},
        {"_ZN2nn5codec34InitializeVp8DecoderByRemoteMemoryEPNS0_14Vp8DecoderTypeEmttjNS0_23VideoDecoderInputFormatENS0_24VideoDecoderOutputFormatENS0_22VideoDecoderOutputModeE", vp8_init2},
        {"_ZN2nn5codec36InitializeAv1DecoderByTransferMemoryEPNS0_14Av1DecoderTypeEPvmmttjNS0_23VideoDecoderInputFormatENS0_24VideoDecoderOutputFormatENS0_22VideoDecoderOutputModeE", av1_init},
        {"_ZN2nn5codec34InitializeAv1DecoderByRemoteMemoryEPNS0_14Av1DecoderTypeEmttjNS0_23VideoDecoderInputFormatENS0_24VideoDecoderOutputFormatENS0_22VideoDecoderOutputModeE", av1_init2},
        {"_ZN2nn5codec37InitializeH264DecoderByTransferMemoryEPNS0_15H264DecoderTypeEPvmmttjNS0_23VideoDecoderInputFormatENS0_24VideoDecoderOutputFormatENS0_22VideoDecoderOutputModeEPKvm", h264_init},
        {"_ZN2nn5codec35InitializeH264DecoderByRemoteMemoryEPNS0_15H264DecoderTypeEmttjNS0_23VideoDecoderInputFormatENS0_24VideoDecoderOutputFormatENS0_22VideoDecoderOutputModeEPKvm", h264_init},
        /* The movie library's audio track (nn::movie::AudioDecoder -> nn::codec::AacDecoder): the SDK client would
           reach the gsv codec service and fail, and nn::movie aborts on the null event it gets back. */
        {"_ZN2nn5codec30InitializeAacDecoderForAnyTypeEPNS0_14AacDecoderTypeEiiPvm", aac_init},
        {"_ZN2nn5codec41InitializeAacDecoderForAnyTypeWithDownmixEPNS0_14AacDecoderTypeEiiPvm", aac_init},
        {"_ZN2nn5codec20DecodeAacInterleavedEPNS0_14AacDecoderTypeEPmPiPsmPKvm", aac_decode},
        {"_ZN2nn5codec23GetAacDecoderSampleRateEPKNS0_14AacDecoderTypeE", aac_rate},
        {"_ZN2nn5codec25GetAacDecoderChannelCountEPKNS0_14AacDecoderTypeE", aac_ch},
        {"_ZN2nn5codec33GetAacDecoderMpeg4AudioObjectTypeEPKNS0_14AacDecoderTypeE", aac_objtype},
        {"_ZN2nn5codec18FinalizeAacDecoderEPNS0_14AacDecoderTypeE", aac_fin},
        {"_ZN2nn5codec26InitializeDecoderImagePoolEPNS0_25VideoDecoderImagePoolTypeEPvmRKNS0_27VideoDecoderImagePreferenceEj", cv_pool_init},
        {"_ZN2nn5codec25MakeDecodeImagePreferenceEttNS0_24VideoDecoderOutputFormatENS0_15ImageTilingModeE", cv_preference},
        {"_ZN2nn5codec29ReadDecoderOutputImageRgbaHdrEPKNS0_25VideoDecoderImagePoolTypeEjttttPvmm", cv_read_rgba},
        {"_ZN2nn2gs17TryWaitNativeSyncERKNS0_16NativeSyncHandleE", gs_try_wait},
        {"_ZN2nn6syncpt3sdk20CreateSyncPointEventEPNS1_18SyncPointEventTypeE", spt_create},
        {"_ZN2nn6syncpt3sdk25InitializeMultiWaitHolderEPNS_2os19MultiWaitHolderTypeEPNS1_18SyncPointEventTypeE", spt_holder},
        {"_ZN2nn6syncpt3sdk21RequestSyncPointEventEPNS1_18SyncPointEventTypeERKNS1_15SyncPointHandleE", spt_request},
        {"_ZN2nn6syncpt3sdk20CancelSyncPointEventEPNS1_18SyncPointEventTypeE", spt_cancel},
        {"_ZN2nn6syncpt3sdk21DestroySyncPointEventEPNS1_18SyncPointEventTypeE", spt_destroy},
    };
    for (size_t i = 0; i < sizeof map / sizeof *map; i++) if (!strcmp(name, map[i].name)) return map[i].fn;
    return NULL;
}
#undef VP
