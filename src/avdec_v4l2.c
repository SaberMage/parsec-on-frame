/* avdec_v4l2.c - stand-in libavcodec.so.62 for the x86_64 Parsec client on
 * Steam Frame (running under FEX). Loaded by Parsec in place of FFmpeg's
 * libavcodec; everything it doesn't override resolves to the real library
 * (libavcodec-real.so.62). What it fixes:
 *
 * - Parsec still looks up avcodec_close(), which FFmpeg 8 removed; without it
 *   Parsec rejects FFmpeg entirely and has no decoders. avcodec_free_context()
 *   already closes the codec in FFmpeg 8, so a no-op is enough here.
 * - When Parsec asks for an H.264/HEVC decoder, hand it the V4L2 mem2mem one
 *   so decoding runs on the Snapdragon's Iris hardware decoder.
 *   Set PARSEC_HWDEC=0 to get FFmpeg's software decoders instead.
 * - Parsec opens the decoder before it knows the stream size, and the V4L2
 *   driver rejects 0x0 input buffers. Open it at 4K instead (it reconfigures
 *   to the real size on the first keyframe), and fall back to the software
 *   decoder if the hardware one still can't be opened.
 * - FFmpeg's V4L2 decoder can misread the Iris driver's end-of-sequence marker
 *   (sent around keyframes) as end of stream and return AVERROR_EOF mid-stream,
 *   which Parsec treats as fatal (error -14). On that, transparently open a
 *   fresh decoder and replay the packets since the last keyframe into it.
 *   The EOF can surface from any call (the first receive, the wait for an
 *   owed frame, or send_packet), so every one of them recovers.
 *   The same recovery kicks in if the hardware decoder stalls: no frame for
 *   1.5 s while packets keep coming. (poll_timeout.c, preloaded by parsec.sh,
 *   keeps FFmpeg from blocking forever on a stalled decoder in the first place.)
 * - FFmpeg's V4L2 decoder answers EAGAIN while a frame is still being decoded
 *   (it prefers taking more input), and Parsec moves on, so frames come out one
 *   or more packets late: 6 frames behind in practice, i.e. hundreds of ms at
 *   desktop frame rates. When a frame is owed, wait briefly for it instead.
 *
 * Diagnostics:
 * - PARSEC_AVLOG=1 logs per-second decode stats to stderr (~/.parsec/stderr.txt),
 *   including how many frames the hardware decoder currently owes ("held").
 * - PARSEC_DUMP=<file> appends the raw compressed video stream to <file>.
 * - A watchdog thread logs "[shim] WATCHDOG" to stderr if a hardware decoder
 *   call hasn't returned after 2 s (then 10 s, 30 s), naming the call.
 *
 * Build with install.sh. It must be built at -O1: the -O2 build crashes under
 * FEX. No headers are needed; the few FFmpeg/libc declarations are below. */
typedef struct AVCodec { const char *name, *long_name; int type, id; } AVCodec;

const AVCodec *avcodec_find_decoder_by_name(const char *name);
const AVCodec *av_codec_iterate(void **opaque);
int av_codec_is_decoder(const AVCodec *codec);
int av_opt_set(void *obj, const char *name, const char *val, int flags);
int av_opt_get_image_size(void *obj, const char *name, int *w, int *h);
char *getenv(const char *name);
int dprintf(int fd, const char *fmt, ...);
struct timespec { long tv_sec, tv_nsec; };
int clock_gettime(int clk, struct timespec *ts);
void *dlopen(const char *file, int mode);
void *dlsym(void *handle, const char *name);

#define AV_CODEC_ID_H264 27
#define AV_CODEC_ID_HEVC 173

int avcodec_close(void *avctx)
{
    (void)avctx;
    return 0;
}

static const AVCodec *find_software_decoder(int id)
{
    /* Same as upstream: first registered decoder for this codec id. */
    const AVCodec *c;
    void *it = 0;
    while ((c = av_codec_iterate(&it)))
        if (c->id == id && av_codec_is_decoder(c))
            return c;
    return 0;
}

static int is_v4l2m2m(const AVCodec *c)
{
    const char *n = c ? c->name : 0;
    for (; n && *n; n++)
        if (n[0] == 'v' && n[1] == '4' && n[2] == 'l' && n[3] == '2')
            return 1;
    return 0;
}

static void hw_track(void *avctx, const AVCodec *codec);

int avcodec_open2(void *avctx, const AVCodec *codec, void **options)
{
    static int (*real_open2)(void *, const AVCodec *, void **);
    if (!real_open2)
        real_open2 = (int (*)(void *, const AVCodec *, void **))
            dlsym(dlopen("libavcodec-real.so.62", 2 /* RTLD_NOW */), "avcodec_open2");

    if (!is_v4l2m2m(codec))
        return real_open2(avctx, codec, options);

    int w = 0, h = 0;
    av_opt_get_image_size(avctx, "video_size", &w, &h);
    if (w <= 0 || h <= 0)
        av_opt_set(avctx, "video_size", "3840x2160", 0);

    int ret = real_open2(avctx, codec, options);
    if (ret >= 0)
        hw_track(avctx, codec);
    if (ret < 0) {
        const AVCodec *sw = find_software_decoder(codec->id);
        if (sw)
            ret = real_open2(avctx, sw, options);
    }
    return ret;
}

const AVCodec *avcodec_find_decoder(int id)
{
    const AVCodec *c = 0;
    const char *hw = getenv("PARSEC_HWDEC");

    if (!hw || hw[0] != '0') {
        if (id == AV_CODEC_ID_H264)
            c = avcodec_find_decoder_by_name("h264_v4l2m2m");
        else if (id == AV_CODEC_ID_HEVC)
            c = avcodec_find_decoder_by_name("hevc_v4l2m2m");
        if (c)
            return c;
    }

    return find_software_decoder(id);
}

/* Optional decode timing (PARSEC_AVLOG=1). */
static void *real_sym(const char *name)
{
    return dlsym(dlopen("libavcodec-real.so.62", 2 /* RTLD_NOW */), name);
}

static double now_ms(void)
{
    struct timespec ts;
    clock_gettime(1 /* CLOCK_MONOTONIC */, &ts);
    return ts.tv_sec * 1e3 + ts.tv_nsec / 1e6;
}

static long held_now;
static int avlog = -1;
static long n_pkt, n_frm, n_again, n_err, n_waits, n_timeouts;
static double wait_max;
static int dump_fd = -2;
int open(const char *path, int flags, ...);
long write(int fd, const void *buf, unsigned long n);
static double t_window, send_ms, recv_ms, recv_max;

static void maybe_report(void)
{
    double t = now_ms();
    if (!t_window)
        t_window = t;
    if (t - t_window < 1000)
        return;
    dprintf(2, "[avlog] held %ld | pkts %ld frames %ld backlog %ld eagain %ld err %ld | send avg %.2f ms | recv avg %.2f max %.2f ms | waits %ld max %.1f ms, timeouts %ld\n",
            held_now, n_pkt, n_frm, n_pkt - n_frm, n_again, n_err,
            n_pkt ? send_ms / n_pkt : 0, n_frm ? recv_ms / n_frm : 0, recv_max,
            n_waits, wait_max, n_timeouts);
    n_pkt = n_frm = n_again = n_err = n_waits = n_timeouts = 0;
    send_ms = recv_ms = recv_max = wait_max = 0;
    t_window = t;
}

/* Hardware decoder contexts we manage: Parsec's context, the replacement we
 * decode on after an EOF recovery, and the last packet sent (to resend). */
#define GOP_MAX 128

typedef struct {
    void *orig, *repl;
    void *gop[GOP_MAX];   /* packets since the last keyframe, for replay */
    int gop_n, gop_overflow;
    const AVCodec *codec;
    int flushing;
    long owed;      /* packets sent that have not produced a frame yet */
    int timeouts;   /* consecutive waits that gave up */
    double last_frame_ms;  /* when the decoder last produced a frame */
    int pkts_since_frame;
} HwCtx;

int nanosleep(const struct timespec *req, struct timespec *rem);
int pthread_create(void *thread, const void *attr, void *(*fn)(void *), void *arg);

/* Watchdog: which hardware decoder call is in progress, and since when. */
static const char *volatile busy_what;
static volatile double busy_since;
static volatile long busy_owed;

static void busy_begin(const char *what, long owed)
{
    busy_owed = owed;
    busy_what = what;
    busy_since = now_ms();
}

static void busy_end(void)
{
    busy_since = 0;
}

static void *watchdog(void *arg)
{
    struct timespec tick = { 0, 500000000 };
    double reported_since = 0;
    int level = 0;
    const double limits[] = { 2000, 10000, 30000 };
    (void)arg;
    for (;;) {
        nanosleep(&tick, 0);
        double since = busy_since;
        if (!since || since != reported_since) {
            reported_since = since;
            level = 0;
        }
        if (since && level < 3 && now_ms() - since > limits[level]) {
            dprintf(2, "[shim] WATCHDOG: stuck in %s for %.0f s (frames owed %ld)\n",
                    busy_what, (now_ms() - since) / 1000, busy_owed);
            level++;
        }
    }
    return 0;
}

static HwCtx hw_ctxs[8];

static HwCtx *hw_find(void *avctx)
{
    for (int i = 0; i < 8; i++)
        if (hw_ctxs[i].orig == avctx)
            return &hw_ctxs[i];
    return 0;
}

void *av_packet_alloc(void);
void av_packet_free(void **pkt);
int av_packet_ref(void *dst, const void *src);
void av_packet_unref(void *pkt);
void *avcodec_alloc_context3(const AVCodec *codec);

static int (*real_send)(void *, const void *);
static int (*real_recv)(void *, void *);
static void (*real_free)(void **);

static void hw_track(void *avctx, const AVCodec *codec)
{
    HwCtx *h = hw_find(avctx);
    if (!h)
        h = hw_find(0);
    if (!h)
        return;
    static int watchdog_started;
    if (!watchdog_started) {
        unsigned long tid;
        watchdog_started = pthread_create(&tid, 0, watchdog, 0) == 0;
    }
    h->orig = avctx;
    h->codec = codec;
    h->repl = 0;
    h->flushing = 0;
    h->owed = 0;
    h->timeouts = 0;
    h->last_frame_ms = now_ms();
    h->pkts_since_frame = 0;
    h->gop_n = 0;
    h->gop_overflow = 0;
}

/* Does this Annex B packet contain an IDR/IRAP picture? Runs on every packet,
 * so it stops at the first slice NAL (right after the small parameter-set and
 * SEI NALs) instead of scanning the whole picture, which is slow under FEX. */
static int is_keyframe(int codec_id, const unsigned char *d, int n)
{
    for (int i = 0; i + 3 < n; i++) {
        if (d[i] || d[i + 1] || d[i + 2] != 1)
            continue;
        int b = d[i + 3];
        if (codec_id == AV_CODEC_ID_H264) {
            int type = b & 0x1f;
            if (type >= 1 && type <= 5) /* coded slice */
                return type == 5;
        } else {
            int type = (b >> 1) & 0x3f;
            if (type <= 31) /* VCL NAL */
                return type >= 16 && type <= 23;
        }
        i += 3;
    }
    return 0;
}

static void gop_push(HwCtx *h, const void *pkt)
{
    /* AVPacket: data at +24, size at +32 */
    if (is_keyframe(h->codec->id, *(const unsigned char **)((const char *)pkt + 24), *(const int *)((const char *)pkt + 32))) {
        for (int i = 0; i < h->gop_n; i++)
            av_packet_unref(h->gop[i]);
        h->gop_n = 0;
        h->gop_overflow = 0;
    }
    if (h->gop_n == GOP_MAX) {
        h->gop_overflow = 1;
        return;
    }
    if (!h->gop[h->gop_n])
        h->gop[h->gop_n] = av_packet_alloc();
    av_packet_ref(h->gop[h->gop_n++], pkt);
}

static void hw_release(HwCtx *h)
{
    if (!real_free)
        real_free = (void (*)(void **))real_sym("avcodec_free_context");
    if (h->repl)
        real_free(&h->repl);
    for (int i = 0; i < GOP_MAX; i++)
        if (h->gop[i])
            av_packet_free(&h->gop[i]);
    h->gop_n = 0;
    h->orig = 0;
}

/* Replace a hardware decoder that hit a spurious EOF. */
static void *hw_reopen(HwCtx *h)
{
    static int (*real_open2)(void *, const AVCodec *, void **);
    if (!real_open2)
        real_open2 = (int (*)(void *, const AVCodec *, void **))real_sym("avcodec_open2");
    if (!real_free)
        real_free = (void (*)(void **))real_sym("avcodec_free_context");

    void *c = avcodec_alloc_context3(h->codec);
    if (!c)
        return 0;
    av_opt_set(c, "video_size", "3840x2160", 0);
    if (real_open2(c, h->codec, 0) < 0) {
        real_free(&c);
        return 0;
    }
    if (h->repl)
        real_free(&h->repl);
    h->repl = c;
    dprintf(2, "[shim] reopened the hardware decoder\n");
    return c;
}

void *av_frame_alloc(void);
void av_frame_free(void **frame);
static int hw_recover(HwCtx *h, void **pc, void *frame);

int avcodec_send_packet(void *avctx, const void *pkt)
{
    if (!real_send)
        real_send = (int (*)(void *, const void *))real_sym("avcodec_send_packet");
    if (avlog < 0) {
        const char *e = getenv("PARSEC_AVLOG");
        avlog = e && e[0] == '1';
    }

    HwCtx *h = hw_find(avctx);
    void *c = h && h->repl ? h->repl : avctx;
    if (h) {
        if (pkt) {
            gop_push(h, pkt);
        } else {
            h->flushing = 1;
        }
    }

    if (dump_fd == -2) {
        const char *d = getenv("PARSEC_DUMP");
        dump_fd = d ? open(d, 01 | 0100 | 02000 /* O_WRONLY|O_CREAT|O_APPEND */, 0644) : -1;
    }
    if (dump_fd >= 0 && pkt) /* AVPacket: data at +24, size at +32 */
        write(dump_fd, *(void **)((char *)pkt + 24), *(int *)((char *)pkt + 32));

    double t = avlog ? now_ms() : 0;
    if (h)
        busy_begin("avcodec_send_packet", h->owed);
    int ret = real_send(c, pkt);
    if (ret == -541478725 /* AVERROR_EOF */ && h && pkt && !h->flushing) {
        /* The packet is already in the GOP buffer, so the replay feeds it in. */
        void *scratch = av_frame_alloc();
        if (scratch) {
            hw_recover(h, &c, scratch);
            av_frame_free(&scratch);
        }
        h->owed = 0;
        ret = 0;
    }
    if (h)
        busy_end();
    if (h && pkt && ret >= 0) {
        h->owed++;
        h->pkts_since_frame++;
    }
    if (avlog) {
        send_ms += now_ms() - t;
        n_pkt++;
        maybe_report();
    }
    return ret;
}

void av_frame_unref(void *frame);

/* Wait up to 20 ms for the frame a just-sent packet will produce. */
static int recv_wait(void *c, void *frame)
{
    struct timespec nap = { 0, 250000 };
    double give_up = now_ms() + 20;
    int ret = real_recv(c, frame);
    while (ret == -11 && now_ms() < give_up) {
        nanosleep(&nap, 0);
        ret = real_recv(c, frame);
    }
    return ret;
}

/* Feed the packets since the last keyframe into a fresh decoder; hand back
 * the newest frame. */
static int gop_replay(HwCtx *h, void *c, void *frame)
{
    int got = -11, frames = 0, i;
    for (i = 0; i < h->gop_n; i++) {
        if (real_send(c, h->gop[i]) < 0)
            continue;
        if (got == 0)
            av_frame_unref(frame);
        got = recv_wait(c, frame);
        if (got == -541478725)
            got = -11;
        if (got == 0)
            frames++;
        else if (!frames && i >= 5)
            break; /* the new decoder isn't producing anything either */
    }
    dprintf(2, "[shim] replayed %d of %d packets since the last keyframe (%d frames)\n",
            i < h->gop_n ? i + 1 : h->gop_n, h->gop_n, frames);
    return got;
}

/* Replace a hardware decoder that hit a spurious EOF (or stalled) and replay
 * the packets since the last keyframe into the new one. Returns the newest
 * frame (0) if Parsec still needs one, else AVERROR(EAGAIN). Used wherever an
 * EOF can surface outside a deliberate flush. */
static int hw_recover(HwCtx *h, void **pc, void *frame)
{
    long was_owed = h->owed;
    int ret = -11; /* AVERROR(EAGAIN) */
    h->last_frame_ms = now_ms();
    h->pkts_since_frame = 0;
    h->owed = 0;
    busy_begin("EOF recovery (reopening the decoder)", was_owed);
    *pc = hw_reopen(h);
    busy_begin("EOF recovery (replaying packets)", was_owed);
    if (*pc && !h->gop_overflow)
        ret = gop_replay(h, *pc, frame);
    if (ret == 0 && !was_owed) { /* Parsec already has this picture */
        av_frame_unref(frame);
        ret = -11;
    }
    return ret;
}

int avcodec_receive_frame(void *avctx, void *frame)
{
    if (!real_recv)
        real_recv = (int (*)(void *, void *))real_sym("avcodec_receive_frame");

    HwCtx *h = hw_find(avctx);
    void *c = h && h->repl ? h->repl : avctx;

    double t = avlog > 0 ? now_ms() : 0;
    if (h)
        busy_begin("avcodec_receive_frame", h->owed);
    int ret = real_recv(c, frame);

    int stalled = ret == -11 && h && !h->flushing && h->pkts_since_frame >= 3 &&
                  now_ms() - h->last_frame_ms > 1500;
    if (stalled)
        dprintf(2, "[shim] hardware decoder stalled (no frame for %.1f s, %d packets); reopening it\n",
                (now_ms() - h->last_frame_ms) / 1000, h->pkts_since_frame);
    if ((ret == -541478725 /* AVERROR_EOF */ || stalled) && h && !h->flushing)
        ret = hw_recover(h, &c, frame);

    /* A frame is owed but still decoding: wait for it (~2 ms normally). */
    if (ret == -11 && h && h->owed > 0 && !h->flushing) {
        busy_begin("avcodec_receive_frame (waiting for an owed frame)", h->owed);
        double w = now_ms();
        ret = recv_wait(c, frame);
        w = now_ms() - w;
        n_waits++;
        if (w > wait_max)
            wait_max = w;
        if (ret == -11)
            n_timeouts++;
        if (ret == -11 && ++h->timeouts >= 3) {
            /* That packet evidently produced no frame; stop waiting for it. */
            h->owed--;
            h->timeouts = 0;
        }
        /* The spurious EOF can also surface here, while waiting for the frame. */
        if (ret == -541478725 /* AVERROR_EOF */)
            ret = hw_recover(h, &c, frame);
    }
    if (ret == 0 && h) {
        if (h->owed > 0)
            h->owed--;
        h->timeouts = 0;
        h->last_frame_ms = now_ms();
        h->pkts_since_frame = 0;
    }
    if (h) {
        held_now = h->owed;
        busy_end();
    }

    if (avlog > 0) {
        double d = now_ms() - t;
        if (ret == 0) {
            n_frm++;
            recv_ms += d;
            if (d > recv_max)
                recv_max = d;
        } else if (ret == -11) {
            n_again++;
        } else {
            n_err++;
        }
    }
    return ret;
}

void avcodec_free_context(void **pavctx)
{
    if (!real_free)
        real_free = (void (*)(void **))real_sym("avcodec_free_context");
    HwCtx *h = pavctx ? hw_find(*pavctx) : 0;
    if (h && *pavctx)
        hw_release(h);
    real_free(pavctx);
}
