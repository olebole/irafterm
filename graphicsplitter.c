/* graphicsplitter.c
 *
 * PTY launcher + graphic escape sequence splitter for IRAF.
 * - runs a command under a PTY
 * - intercepts selected graphic escape sequences (configurable)
 * - appends intercepted sequences to gfx.log (binary)
 * - forwards all other bytes to the original terminal (stdout)
 * - propagates terminal window size (winsize) to the slave PTY and forwards SIGWINCH
 *
 * Build:
 *  Linux:  gcc -std=c11 -O2 -Wall -Wextra -o graphicsplitter graphicsplitter.c -lutil
 *  macOS:  gcc -std=c11 -O2 -Wall -Wextra -o graphicsplitter graphicsplitter.c
 *
 * Usage:
 *  GFX_PREFIX (optional): if set and non-empty, only sequences that start with this
 *                         prefix (after OSC/DCS/etc.) are intercepted.
 *  GFX_DEBUG  (optional): if set to 1, prints debug info to stderr about captures.
 *
 * Notes:
 *  - conservative default: do NOT intercept CSI sequences; only intercept OSC/DCS/etc.
 *    when a non-empty GFX_PREFIX is provided (avoids capturing normal terminal control).
 *  - partial sequence buffering with SEQ_MAX limit
 *  - forwards SIGWINCH, SIGINT, SIGTERM to child
 */

#define _XOPEN_SOURCE 700
#include <errno.h>
#include <fcntl.h>
#include <poll.h>
#include <pty.h>
#include <signal.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <sys/ioctl.h>
#include <sys/select.h>
#include <sys/types.h>
#include <sys/wait.h>
#include <termios.h>
#include <unistd.h>

#if defined(__APPLE__)
#include <util.h>
#endif

#define BUF_SIZE 8192
#define SEQ_MAX (64*1024) /* 64 KiB max sequence length */

static struct termios orig_term;
static int raw_fd_for_restore = -1;
static pid_t child_pid = -1;
static volatile sig_atomic_t winch_pending = 0;

/* configuration: prefix for graphic sequences and debug flag */
static const char *gfx_prefix = NULL;
static int gfx_debug = 0;

/* restore terminal attributes at exit */
static void restore_term(void) {
    if (raw_fd_for_restore >= 0) tcsetattr(raw_fd_for_restore, TCSANOW, &orig_term);
}

/* portable replacement for cfmakeraw: set terminal to raw-ish mode */
static int set_raw_terminal(int fd) {
    struct termios t;
    if (tcgetattr(fd, &t) < 0) return -1;
    orig_term = t;
    raw_fd_for_restore = fd;
    atexit(restore_term);

    /* input flags: disable parity, CR/NL translations, XON/XOFF */
    t.c_iflag &= ~(IGNBRK | BRKINT | PARMRK | ISTRIP
                   | INLCR | IGNCR | ICRNL | IXON);
    /* output flags: disable post processing */
    t.c_oflag &= ~OPOST;
    /* local flags: disable echo, canonical mode, signals, extended functions */
    t.c_lflag &= ~(ECHO | ECHONL | ICANON | ISIG | IEXTEN);
    /* control flags: set 8 bit chars */
    t.c_cflag &= ~(CSIZE | PARENB);
    t.c_cflag |= CS8;

    /* minimum number of bytes and timeout for read */
    t.c_cc[VMIN] = 1;
    t.c_cc[VTIME] = 0;

    if (tcsetattr(fd, TCSANOW, &t) < 0) return -1;
    return 0;
}

/* write all bytes reliably (handles EINTR, partial writes) */
static ssize_t write_all(int fd, const void *buf, size_t count) {
    const unsigned char *p = buf;
    size_t left = count;
    while (left > 0) {
        ssize_t w = write(fd, p, left);
        if (w < 0) {
            if (errno == EINTR) continue;
            return -1;
        }
        left -= (size_t)w;
        p += w;
    }
    return (ssize_t)count;
}

/* forward signal to child process */
static void forward_signal(int sig) {
    if (child_pid > 0) kill(child_pid, sig);
}

/* SIGWINCH handler: set a flag to be handled in the main loop */
static void sigwinch_handler(int sig) {
    (void)sig;
    winch_pending = 1;
}

/* propagate current terminal window size from in_fd (or /dev/tty) to master_fd */
static void propagate_winsize(int master_fd, int in_fd) {
    struct winsize ws;
    int got = 0;

    if (in_fd >= 0 && ioctl(in_fd, TIOCGWINSZ, &ws) == 0) {
        got = 1;
    } else {
        int t = open("/dev/tty", O_RDWR | O_NOCTTY);
        if (t >= 0) {
            if (ioctl(t, TIOCGWINSZ, &ws) == 0) got = 1;
            close(t);
        }
    }

    if (got) {
        /* set winsize on master PTY (propagates to slave) */
        (void)ioctl(master_fd, TIOCSWINSZ, &ws);
        if (gfx_debug) {
            fprintf(stderr, "gfx: propagated winsize to master_fd\n");
        }
    }
}

/* Detect graphic sequences (conservative, prefix-based).
 * Return values:
 *  >0  : number of bytes that form a complete graphic sequence
 *   0  : not a graphic sequence (caller should forward first byte(s))
 *  -1  : need more bytes (partial sequence)
 *  -2  : sequence too long (error)
 *
 * Behavior:
 *  - CSI (ESC [) is NOT treated as graphic (forwarded).
 *  - OSC (ESC ]) and DCS/PM/APC (ESC P / ESC ^ / ESC _) are only treated as
 *    graphic if gfx_prefix is non-NULL and the payload starts with that prefix.
 *  - Empty gfx_prefix is treated as unset (see initialization in main).
 */
static ssize_t detect_graphic_sequence(const unsigned char *buf, size_t len) {
    if (len < 2) return -1;
    if (buf[0] != 0x1B) return 0;
    unsigned char second = buf[1];

    /* CSI: ESC [ -> do not intercept */
    if (second == '[') return 0;

    /* OSC: ESC ] ... BEL or ESC ] ... ESC '\' */
    if (second == ']') {
        if (!gfx_prefix) return 0; /* conservative: forward normal OSC */
        size_t pref_len = strlen(gfx_prefix);
        if (len < 2 + pref_len) return -1;
        if (memcmp(buf + 2, gfx_prefix, pref_len) != 0) return 0; /* not our OSC */
        for (size_t i = 2 + pref_len; i < len; ++i) {
            if (buf[i] == 0x07) return (ssize_t)(i + 1); /* BEL */
            if (i + 1 < len && buf[i] == 0x1B && buf[i+1] == '\\') return (ssize_t)(i + 2);
            if (i + 1 >= SEQ_MAX) return -2;
        }
        return -1;
    }

    /* DCS / PM / APC / vendor private strings: ESC P / ESC ^ / ESC _ ... ESC '\' */
    if (second == 'P' || second == '^' || second == '_' ) {
        if (!gfx_prefix) return 0; /* conservative: do not intercept */
        size_t pref_len = strlen(gfx_prefix);
        if (len < 2 + pref_len) return -1;
        if (memcmp(buf + 2, gfx_prefix, pref_len) != 0) return 0; /* not our DCS */
        for (size_t i = 2 + pref_len; i < len; ++i) {
            if (i + 1 < len && buf[i] == 0x1B && buf[i+1] == '\\') return (ssize_t)(i + 2);
            if (i + 1 >= SEQ_MAX) return -2;
        }
        return -1;
    }

    return 0;
}

int main(int argc, char **argv) {
    if (argc < 2) {
        fprintf(stderr, "Usage: %s <cmd> [args...]\n", argv[0]);
        return 2;
    }

    /* configure which sequences to treat as graphic (optional) */
    const char *env_pref = getenv("GFX_PREFIX");
    if (env_pref && env_pref[0] == '\0') {
        /* treat empty string as unset to avoid matching everything */
        env_pref = NULL;
    }
    gfx_prefix = env_pref;
    const char *dbg = getenv("GFX_DEBUG");
    gfx_debug = (dbg && (dbg[0] == '1' || dbg[0] == 'y' || dbg[0] == 'Y')) ? 1 : 0;
    if (gfx_debug) {
        fprintf(stderr, "gfx: GFX_PREFIX=%s GFX_DEBUG=%d\n", gfx_prefix ? gfx_prefix : "(null)", gfx_debug);
    }

    /* prefer to set raw on stdin if it's a tty, otherwise try /dev/tty */
    int in_fd = STDIN_FILENO;
    int out_fd = STDOUT_FILENO;

    if (isatty(in_fd)) {
        if (set_raw_terminal(in_fd) < 0) perror("set_raw_terminal(stdin)");
    } else {
        int devtty = open("/dev/tty", O_RDWR | O_NOCTTY);
        if (devtty >= 0) {
            if (set_raw_terminal(devtty) < 0) perror("set_raw_terminal(/dev/tty)");
            close(devtty);
        } else {
            /* not a tty; continue without raw mode */
        }
    }

    int master_fd;
    pid_t pid = forkpty(&master_fd, NULL, NULL, NULL);
    if (pid < 0) {
        perror("forkpty");
        return 1;
    }

    if (pid == 0) {
        /* child: exec target program */
        execvp(argv[1], &argv[1]);
        perror("execvp");
        _exit(127);
    }

    /* parent */
    child_pid = pid;

    /* propagate initial winsize to the slave PTY so child sees correct size */
    propagate_winsize(master_fd, in_fd);

    /* set non-blocking on master and stdin */
    int flags = fcntl(master_fd, F_GETFL, 0);
    fcntl(master_fd, F_SETFL, flags | O_NONBLOCK);
    flags = fcntl(in_fd, F_GETFL, 0);
    fcntl(in_fd, F_SETFL, flags | O_NONBLOCK);

    /* forward signals to child */
    struct sigaction sa;
    memset(&sa, 0, sizeof(sa));
    sa.sa_handler = forward_signal;
    sigaction(SIGINT, &sa, NULL);
    sigaction(SIGTERM, &sa, NULL);

    /* SIGWINCH: set flag and handle in main loop */
    struct sigaction sa_winch;
    memset(&sa_winch, 0, sizeof(sa_winch));
    sa_winch.sa_handler = sigwinch_handler;
    sigaction(SIGWINCH, &sa_winch, NULL);

    /* open gfx log (binary append) */
    FILE *gfx = fopen("gfx.log", "ab");
    if (!gfx) {
        perror("fopen gfx.log (continuing without gfx logging)");
    }

    unsigned char inbuf[BUF_SIZE];
    unsigned char stdinbuf[BUF_SIZE];
    unsigned char seqbuf[SEQ_MAX];
    size_t seq_len = 0;

    struct pollfd fds[2];
    fds[0].fd = master_fd; fds[0].events = POLLIN;
    fds[1].fd = in_fd;     fds[1].events = POLLIN;

    int status;
    while (1) {
        /* handle pending window-size change */
        if (winch_pending) {
            winch_pending = 0;
            propagate_winsize(master_fd, in_fd);
            if (child_pid > 0) kill(child_pid, SIGWINCH);
        }

        int r = poll(fds, 2, -1);
        if (r < 0) {
            if (errno == EINTR) {
                pid_t w = waitpid(child_pid, &status, WNOHANG);
                if (w == child_pid) break;
                continue;
            }
            break;
        }

        /* data from child (master PTY) */
        if (fds[0].revents & POLLIN) {
            ssize_t n = read(master_fd, inbuf, sizeof(inbuf));
            if (n <= 0) break;

            size_t pos = 0;
            while (pos < (size_t)n) {
                if (seq_len > 0) {
                    /* append to partial sequence buffer */
                    size_t want = (size_t)n - pos;
                    size_t tocopy = (SEQ_MAX - seq_len < want) ? (SEQ_MAX - seq_len) : want;
                    memcpy(seqbuf + seq_len, inbuf + pos, tocopy);
                    seq_len += tocopy;
                    pos += tocopy;
                    ssize_t det = detect_graphic_sequence(seqbuf, seq_len);
                    if (det > 0) {
                        if (gfx) {
                            fwrite(seqbuf, 1, det, gfx);
                            if (gfx_debug) {
                                fprintf(stderr, "gfx: captured (partial-buffer) %zd bytes, first: ", det);
                                for (size_t k = 0; k < (size_t)det && k < 8; ++k) fprintf(stderr, "%02x ", seqbuf[k]);
                                fprintf(stderr, "\n");
                            }
                        }
                        size_t remain = seq_len - det;
                        if (remain) memmove(seqbuf, seqbuf + det, remain);
                        seq_len = remain;
                        continue;
                    } else if (det == -1) {
                        continue; /* need more bytes */
                    } else if (det == -2) {
                        /* too long: drop and reset */
                        if (gfx_debug) fprintf(stderr, "gfx: sequence too long, dropping\n");
                        seq_len = 0;
                        continue;
                    } else {
                        /* not a sequence: flush first byte */
                        if (write_all(out_fd, seqbuf, 1) < 0) { /* ignore */ }
                        if (seq_len > 1) memmove(seqbuf, seqbuf + 1, seq_len - 1);
                        seq_len -= 1;
                        continue;
                    }
                }

                /* no partial sequence: detect starting at inbuf+pos */
                ssize_t det = detect_graphic_sequence(inbuf + pos, (size_t)n - pos);
                if (det > 0) {
                    if (gfx) {
                        fwrite(inbuf + pos, 1, (size_t)det, gfx);
                        if (gfx_debug) {
                            fprintf(stderr, "gfx: captured %zd bytes, first: ", det);
                            for (size_t k = 0; k < (size_t)det && k < 8; ++k) fprintf(stderr, "%02x ", inbuf[pos + k]);
                            fprintf(stderr, "\n");
                        }
                    }
                    pos += (size_t)det;
                    continue;
                } else if (det == -1) {
                    /* partial: copy remainder into seqbuf */
                    size_t tocopy = (size_t)n - pos;
                    if (tocopy > SEQ_MAX) tocopy = SEQ_MAX;
                    memcpy(seqbuf, inbuf + pos, tocopy);
                    seq_len = tocopy;
                    pos += tocopy;
                    continue;
                } else if (det == -2) {
                    pos += 1; /* skip one byte */
                    continue;
                } else {
                    /* not a graphic sequence: forward as a block until next ESC or end */
                    size_t rem = (size_t)n - pos;
                    size_t j = 0;
                    while (j < rem && inbuf[pos + j] != 0x1B) ++j;
                    if (j == 0) j = 1;
                    if (write_all(out_fd, inbuf + pos, j) < 0) break;
                    pos += j;
                }
            }
            if (gfx) fflush(gfx);
        }

        /* data from user terminal -> forward to child */
        if (fds[1].revents & POLLIN) {
            ssize_t n = read(in_fd, stdinbuf, sizeof(stdinbuf));
            if (n <= 0) break;
            (void)write_all(master_fd, stdinbuf, (size_t)n);
        }

        /* check child exit */
        pid_t w = waitpid(child_pid, &status, WNOHANG);
        if (w == child_pid) break;
    }

    if (gfx) fclose(gfx);
    close(master_fd);
    restore_term();
    waitpid(child_pid, &status, 0);
    return 0;
}
