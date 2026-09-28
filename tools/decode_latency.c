/* decode_latency.c - replay an H.264/HEVC Annex B stream through the installed
 * libavcodec shim using Parsec's call pattern (per packet: send once, then
 * receive until EAGAIN, no waiting), paced at a fixed frame rate. Reports how
 * many frames the decoder holds and the packet->frame lag.
 *
 * Build (x86_64, no headers needed):
 *   clang -target x86_64-linux-gnu -O1 -fno-stack-protector -fno-omit-frame-pointer \
 *     -nostdlib -fuse-ld=lld -Wl,--dynamic-linker=/lib64/ld-linux-x86-64.so.2 \
 *     -o decode_latency tools/decode_latency.c \
 *     -L/usr/share/guestos/fex-mesa/usr/lib -l:libc.so.6
 *
 * Run under FEX (compat data dir of the Parsec shortcut, or any writable dir):
 *   STEAM_COMPAT_DATA_PATH=~/.steam/steam/steamapps/compatdata/<appid> \
 *   LD_LIBRARY_PATH=~/.local/share/parsec/lib \
 *   T3_FILE=stream.bin T3_CODEC=173 T3_FPS=30 T3_PKTS=400 \
 *   python3 ~/.steam/steam/steamapps/common/FEX-Emu/fex-compat-tool waitforexitandrun -- ./decode_latency
 *
 * T3_CODEC: 27 = H.264, 173 = HEVC. Record a real stream with PARSEC_DUMP. */
typedef struct { const char *name, *long_name; int type, id; } AVCodec;
typedef unsigned char u8;
void *dlopen(const char *, int); void *dlsym(void *, const char *); char *getenv(const char *);
int dprintf(int, const char *, ...); void exit(int); void *fopen(const char *, const char *);
unsigned long fread(void *, unsigned long, unsigned long, void *); void *malloc(unsigned long);
int atoi(const char *); int usleep(unsigned);
struct timespec { long s, ns; }; int clock_gettime(int, struct timespec *);
static double now(void) { struct timespec t; clock_gettime(1, &t); return t.s * 1e3 + t.ns / 1e6; }
#define F(ret, name, ...) ret (*name)(__VA_ARGS__) = dlsym(h, #name)
int main(void) {
  const char *file = getenv("T3_FILE"); int id = atoi(getenv("T3_CODEC")), fps = atoi(getenv("T3_FPS")), maxp = atoi(getenv("T3_PKTS"));
  void *h = dlopen("libavcodec.so.62", 2), *u = dlopen("libavutil.so.60", 2);
  F(const AVCodec *, avcodec_find_decoder, int); F(void *, avcodec_alloc_context3, const AVCodec *);
  F(int, avcodec_open2, void *, const AVCodec *, void *); F(int, avcodec_send_packet, void *, void *);
  F(int, avcodec_receive_frame, void *, void *); F(void *, av_parser_init, int); F(void *, av_packet_alloc, void);
  F(int, av_parser_parse2, void *, void *, u8 **, int *, const u8 *, int, long, long, long);
  void *(*av_frame_alloc)(void) = dlsym(u, "av_frame_alloc"); void (*av_frame_unref)(void *) = dlsym(u, "av_frame_unref");
  const AVCodec *c = avcodec_find_decoder(id); void *ctx = avcodec_alloc_context3(c);
  int r = avcodec_open2(ctx, c, 0); dprintf(2, "decoder %s open2=%d, %d fps\n", c->name, r, fps); if (r < 0) return 1;
  void *fp = fopen(file, "rb"); u8 *buf = malloc(64 << 20); long n = fread(buf, 1, 64 << 20, fp);
  void *par = av_parser_init(id), *pkt = av_packet_alloc(), *fr = av_frame_alloc();
  static double sent[100000]; int np = 0, nf = 0; double lagsum = 0, lagmax = 0, t0 = now();
  for (long off = 0; off < n && np < maxp;) {
    u8 *d; int sz; off += av_parser_parse2(par, ctx, &d, &sz, buf + off, n - off, 0, 0, 0);
    if (!sz) continue;
    double due = t0 + np * 1000.0 / fps; while (now() < due) usleep(200);
    *(u8 **)((char *)pkt + 24) = d; *(int *)((char *)pkt + 32) = sz;
    sent[np++] = now(); avcodec_send_packet(ctx, pkt);
    while (avcodec_receive_frame(ctx, fr) == 0) {
      double lag = now() - sent[nf];
      if (nf >= 30) { lagsum += lag; if (lag > lagmax) lagmax = lag; }
      nf++; av_frame_unref(fr);
    }
    if (np % 100 == 0) dprintf(2, "  after %d packets: %d frames, held %d\n", np, nf, np - nf);
  }
  dprintf(2, "RESULT %d packets -> %d frames, held at end %d, packet->frame lag avg %.1f ms max %.1f ms\n", np, nf, np - nf, lagsum / (nf - 30), lagmax);
  return 0;
}
void _start(void) { __asm__("and $-16, %rsp"); exit(main()); }
