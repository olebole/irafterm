/* v_write.c
 * Robust write wrapper used in xgterm/gtermio.
 * Returns number of bytes written or -1 on unrecoverable error.
 */

#include <unistd.h>
#include <errno.h>
#include <sys/types.h>

ssize_t v_write_raw(int fd, const void *buf, size_t count) {
    const unsigned char *p = buf;
    size_t left = count;
    while (left > 0) {
        ssize_t w = write(fd, p, left);
        if (w < 0) {
            if (errno == EINTR) continue;
            if (errno == EAGAIN || errno == EWOULDBLOCK) return -2; /* caller can retry */
            return -1;
        }
        left -= (size_t)w;
        p += w;
    }
    return (ssize_t)count;
}

/* Original code expects int v_write(int fd, const char *buf, int n) */
int v_write(int fd, const char *buf, int n) {
    ssize_t r = v_write_raw(fd, buf, (size_t)n);
    if (r >= 0) return (int)r;
    return -1;
}
