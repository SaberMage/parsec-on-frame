/* poll_timeout.c - LD_PRELOADed into the x86_64 Parsec client.
 *
 * FFmpeg's V4L2 mem2mem decoder waits for a decoded frame with
 * poll(fd, -1), i.e. forever. If the hardware decoder stalls, that call never
 * returns, the thread holding Parsec's video pipeline hangs, and the rest of
 * Parsec freezes behind it (it then also ignores SIGTERM). This caps infinite
 * polls on V4L2 video devices (char major 81) at 1 s and reports a timeout,
 * which FFmpeg treats as "no buffer yet" (EAGAIN upstream). The libavcodec
 * shim then recovers the decoder if the stall persists.
 *
 * Build: see install.sh. No headers needed. */
typedef unsigned long nfds_t;
struct pollfd { int fd; short events, revents; };
struct stat_buf { unsigned long st_dev, st_ino, st_nlink; unsigned st_mode, st_uid, st_gid, pad0;
                  unsigned long st_rdev; long rest[12]; }; /* x86_64 struct stat layout */

void *dlsym(void *handle, const char *name);
int fstat(int fd, void *buf);
int *__errno_location(void);

#define RTLD_NEXT ((void *)-1l)
#define S_IFMT 0170000
#define S_IFCHR 0020000
#define V4L2_MAJOR 81
#define SLICE_MS 100
#define CAP_MS 1000
#define ETIMEDOUT 110

static int is_v4l2(int fd)
{
    struct stat_buf st;
    if (fstat(fd, &st) != 0 || (st.st_mode & S_IFMT) != S_IFCHR)
        return 0;
    unsigned long dev = st.st_rdev;
    unsigned major = ((dev >> 8) & 0xfff) | ((unsigned)(dev >> 32) & ~0xfffu);
    return major == V4L2_MAJOR;
}

int poll(struct pollfd *fds, nfds_t nfds, int timeout)
{
    static int (*real_poll)(struct pollfd *, nfds_t, int);
    if (!real_poll)
        real_poll = (int (*)(struct pollfd *, nfds_t, int))dlsym(RTLD_NEXT, "poll");

    if (timeout >= 0 || nfds != 1 || !is_v4l2(fds[0].fd))
        return real_poll(fds, nfds, timeout);

    for (int waited = 0; waited < CAP_MS; waited += SLICE_MS) {
        int ret = real_poll(fds, nfds, SLICE_MS);
        if (ret != 0)
            return ret;
    }
    *__errno_location() = ETIMEDOUT; /* anything but EINTR, which FFmpeg retries */
    return 0;
}
