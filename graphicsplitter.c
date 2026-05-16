/*
 * graphicsplitter.c
 *
 * PTY launcher + graphic escape sequence splitter for IRAF.
 * - erkennt klassische ESC/CSI/OSC/DCS Sequenzen (robust gegen CR/LF/NUL und
 *   zusätzliche control separators 0x1d/0x19)
 * - erkennt Start/End Marker 0x1d 0x19 ... 0x1d 0x18 und schreibt alles dazwischen
 *   binär in gfx.log
 *
 * Build:
 *   Linux: gcc -std=c11 -O2 -Wall -Wextra -o graphicsplitter graphicsplitter.c -lutil
 *
 * Usage:
 *   GFX_PREFIX (optional) : nur OSC/DCS mit diesem Präfix abfangen
 *   GFX_DEBUG  (optional) : setze auf 1 für Debugausgaben auf stderr
 *   GFX_DUMP_ALL (optional): setze auf 1 und GFX_DUMP_PATH für Rohdump aller PTY-Bytes
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

/* capture state: between 0x1d 0x19 (start) and 0x1d 0x18 (end) */
static int capture_mode = 0;
/* small state to detect marker split across reads */
static unsigned char last_byte = 0;

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

    t.c_iflag &= ~(IGNBRK | BRKINT | PARMRK | ISTRIP | INLCR | IGNCR | ICRNL | IXON);
    t.c_oflag &= ~OPOST;
    t.c_lflag &= ~(ECHO | ECHONL | ICANON | ISIG | IEXTEN);
    t.c_cflag &= ~(CSIZE | PARENB);
    t.c_cflag |= CS8;

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
        (void)ioctl(master_fd, TIOCSWINSZ, &ws);
        if (gfx_debug) {
            fprintf(stderr, "gfx: propagated winsize to master_fd\n");
        }
    }
}

/* Helper: skip optional separators (CR LF NUL and some control separators) starting at buf[pos], return new pos */
static size_t skip_separators(const unsigned char *buf, size_t len, size_t pos) {
    while (pos < len) {
        unsigned char c = buf[pos];
        /* allow CR, LF, NUL and control separators seen in dumps (GS 0x1d, EM 0x19) */
        if (c == 0x0a || c == 0x0d || c == 0x00 || c == 0x1d || c == 0x19) pos++;
        else break;
    }
    return pos;
}

/* Detect graphic sequences (robust, tolerant to CR/LF/NUL and separators).
 * Return values:
 *  >0  : number of bytes that form a complete graphic sequence
 *   0  : not a graphic sequence (caller should forward first byte(s))
 *  -1  : need more bytes (partial sequence)
 *  -2  : sequence too long (error)
 *
 * Behavior:
 *  - Skip optional separators between ESC and next bytes.
 *  - Accept CSI even if a '?' precedes '[' after optional separators.
 *  - OSC (ESC ]) and DCS/PM/APC (ESC P / ESC ^ / ESC _) are only treated as
 *    graphic if gfx_prefix is non-NULL and the payload starts with that prefix.
 */
static ssize_t detect_graphic_sequence(const unsigned char *buf, size_t len) {
    if (len == 0) return -1;
    if (buf[0] != 0x1B) return 0; /* not ESC */

    if (len < 2) return -1;

    size_t pos = 1;
    pos = skip_separators(buf, len, pos);
    if (pos >= len) return -1;

    unsigned char second = buf[pos];

    /* optional '?' before '[' */
    if (second == '?') {
        size_t pos2 = pos + 1;
        pos2 = skip_separators(buf, len, pos2);
        if (pos2 >= len) return -1;
        if (buf[pos2] == '[') {
            for (size_t i = pos2 + 1; i < len; ++i) {
                unsigned char c = buf[i];
                if (c >= 0x40 && c <= 0x7E) return (ssize_t)(i + 1);
                if (i + 1 >= SEQ_MAX) return -2;
            }
            return -1;
        }
    }

    /* CSI: ESC [ */
    if (second == '[') {
        for (size_t i = pos + 1; i < len; ++i) {
            unsigned char c = buf[i];
            if (c >= 0x40 && c <= 0x7E) return (ssize_t)(i + 1);
            if (i + 1 >= SEQ_MAX) return -2;
        }
        return -1;
    }

    /* OSC: ESC ] ... BEL or ESC '\' */
    if (second == ']') {
        if (!gfx_prefix) return 0;
        size_t pref_len = strlen(gfx_prefix);
        size_t payload_pos = pos + 1;
        payload_pos = skip_separators(buf, len, payload_pos);
        if (payload_pos + pref_len > len) return -1;
        if (memcmp(buf + payload_pos, gfx_prefix, pref_len) != 0) return 0;
        for (size_t i = payload_pos + pref_len; i < len; ++i) {
            if (buf[i] == 0x07) return (ssize_t)(i + 1);
            if (i + 1 < len && buf[i] == 0x1B && buf[i+1] == '\\') return (ssize_t)(i + 2);
            if (i + 1 >= SEQ_MAX) return -2;
        }
        return -1;
    }

    /* DCS / PM / APC: ESC P / ESC ^ / ESC _ ... ESC '\' */
    if (second == 'P' || second == '^' || second == '_' ) {
        if (!gfx_prefix) return 0;
        size_t pref_len = strlen(gfx_prefix);
        size_t payload_pos = pos + 1;
        payload_pos = skip_separators(buf, len, payload_pos);
        if (payload_pos + pref_len > len) return -1;
        if (memcmp(buf + payload_pos, gfx_prefix, pref_len) != 0) return 0;
        for (size_t i = payload_pos + pref_len; i < len; ++i) {
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

    const char *env_pref = getenv("GFX_PREFIX");
    if (env_pref && env_pref[0] == '\0') env_pref = NULL;
    gfx_prefix = env_pref;
    const char *dbg = getenv("GFX_DEBUG");
    gfx_debug = (dbg && (dbg[0] == '1' || dbg[0] == 'y' || dbg[0] == 'Y')) ? 1 : 0;
    if (gfx_debug) {
        fprintf(stderr, "gfx: GFX_PREFIX=%s GFX_DEBUG=%d\n", gfx_prefix ? gfx_prefix : "(null)", gfx_debug);
    }

    int in_fd = STDIN_FILENO;
    int out_fd = STDOUT_FILENO;

    if (isatty(in_fd)) {
        if (set_raw_terminal(in_fd) < 0) perror("set_raw_terminal(stdin)");
    } else {
        int devtty = open("/dev/tty", O_RDWR | O_NOCTTY);
        if (devtty >= 0) {
            if (set_raw_terminal(devtty) < 0) perror("set_raw_terminal(/dev/tty)");
            close(devtty);
        }
    }

    int master_fd;
    pid_t pid = forkpty(&master_fd, NULL, NULL, NULL);
    if (pid < 0) {
        perror("forkpty");
        return 1;
    }

    if (pid == 0) {
        execvp(argv[1], &argv[1]);
        perror("execvp");
        _exit(127);
    }

    child_pid = pid;
    propagate_winsize(master_fd, in_fd);

    int flags = fcntl(master_fd, F_GETFL, 0);
    fcntl(master_fd, F_SETFL, flags | O_NONBLOCK);
    flags = fcntl(in_fd, F_GETFL, 0);
    fcntl(in_fd, F_SETFL, flags | O_NONBLOCK);

    struct sigaction sa;
    memset(&sa, 0, sizeof(sa));
    sa.sa_handler = forward_signal;
    sigaction(SIGINT, &sa, NULL);
    sigaction(SIGTERM, &sa, NULL);

    struct sigaction sa_winch;
    memset(&sa_winch, 0, sizeof(sa_winch));
    sa_winch.sa_handler = sigwinch_handler;
    sigaction(SIGWINCH, &sa_winch, NULL);

    FILE *gfx = fopen("gfx.log", "ab");
    if (!gfx) {
        perror("fopen gfx.log (continuing without gfx logging)");
    }

    FILE *dumpf = NULL;
    const char *dump_env = getenv("GFX_DUMP_ALL");
    if (dump_env && (dump_env[0]=='1' || dump_env[0]=='y' || dump_env[0]=='Y')) {
        const char *dump_path = getenv("GFX_DUMP_PATH");
        if (!dump_path) dump_path = "/tmp/pty_dump.bin";
        dumpf = fopen(dump_path, "ab");
        if (!dumpf) perror("fopen pty_dump.bin");
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

        if (fds[0].revents & POLLIN) {
            ssize_t n = read(master_fd, inbuf, sizeof(inbuf));
            if (n <= 0) break;

            if (dumpf && n > 0) {
                fwrite(inbuf, 1, (size_t)n, dumpf);
                fflush(dumpf);
            }

            /* --- Marker based capture: start at 0x1d 0x19, end at 0x1d 0x18 --- */
            size_t pos = 0;
            if (!capture_mode) {
                for (size_t i = 0; i < (size_t)n; ++i) {
                    if (last_byte == 0x1d && inbuf[i] == 0x19) {
                        capture_mode = 1;
                        size_t write_from = i + 1;
                        if (write_from < (size_t)n && gfx) {
                            fwrite(inbuf + write_from, 1, (size_t)n - write_from, gfx);
                            if (gfx_debug) fprintf(stderr, "gfx: marker start (split) found, wrote %zu bytes\n", (size_t)n - write_from);
                            fflush(gfx);
                        }
                        pos = (size_t)n;
                        last_byte = inbuf[(size_t)n - 1];
                        break;
                    }
                    if (inbuf[i] == 0x1d && i + 1 < (size_t)n && inbuf[i+1] == 0x19) {
                        capture_mode = 1;
                        size_t write_from = i + 2;
                        if (write_from < (size_t)n && gfx) {
                            fwrite(inbuf + write_from, 1, (size_t)n - write_from, gfx);
                            if (gfx_debug) fprintf(stderr, "gfx: marker start found at %zu, wrote %zu bytes\n", i, (size_t)n - write_from);
                            fflush(gfx);
                        }
                        pos = (size_t)n;
                        last_byte = inbuf[(size_t)n - 1];
                        break;
                    }
                }
                if (pos == 0) last_byte = inbuf[(size_t)n - 1];
            } else {
                int end_found = 0;
                for (size_t i = 0; i < (size_t)n; ++i) {
                    if (last_byte == 0x1d && inbuf[i] == 0x18) {
                        capture_mode = 0;
                        pos = i + 1;
                        end_found = 1;
                        if (gfx_debug) fprintf(stderr, "gfx: end marker (split) found at chunk boundary\n");
                        break;
                    }
                    if (inbuf[i] == 0x1d && i + 1 < (size_t)n && inbuf[i+1] == 0x18) {
                        if (i > 0 && gfx) {
                            fwrite(inbuf, 1, i, gfx);
                            if (gfx_debug) fprintf(stderr, "gfx: end marker found at %zu, wrote %zu bytes\n", i, i);
                            fflush(gfx);
                        }
                        capture_mode = 0;
                        pos = i + 2;
                        end_found = 1;
                        break;
                    }
                    last_byte = inbuf[i];
                }
                if (!end_found) {
                    if (gfx) {
                        fwrite(inbuf, 1, (size_t)n, gfx);
                        if (gfx_debug) fprintf(stderr, "gfx: capture active, wrote %zd bytes\n", n);
                        fflush(gfx);
                    }
                    last_byte = inbuf[(size_t)n - 1];
                    continue;
                }
            }

            /* remaining bytes (pos < n) are processed normally */
            while (pos < (size_t)n) {
                if (seq_len > 0) {
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
                        continue;
                    } else if (det == -2) {
                        if (gfx_debug) fprintf(stderr, "gfx: sequence too long, dropping\n");
                        seq_len = 0;
                        continue;
                    } else {
                        if (write_all(out_fd, seqbuf, 1) < 0) { }
                        if (seq_len > 1) memmove(seqbuf, seqbuf + 1, seq_len - 1);
                        seq_len -= 1;
                        continue;
                    }
                }

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
                    size_t tocopy = (size_t)n - pos;
                    if (tocopy > SEQ_MAX) tocopy = SEQ_MAX;
                    memcpy(seqbuf, inbuf + pos, tocopy);
                    seq_len = tocopy;
                    pos += tocopy;
                    continue;
                } else if (det == -2) {
                    pos += 1;
                    continue;
                } else {
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

        if (fds[1].revents & POLLIN) {
            ssize_t n = read(in_fd, stdinbuf, sizeof(stdinbuf));
            if (n <= 0) break;
            (void)write_all(master_fd, stdinbuf, (size_t)n);
        }

        pid_t w = waitpid(child_pid, &status, WNOHANG);
        if (w == child_pid) break;
    }

    if (gfx) fclose(gfx);
    if (dumpf) fclose(dumpf);
    close(master_fd);
    restore_term();
    waitpid(child_pid, &status, 0);
    return 0;
}
