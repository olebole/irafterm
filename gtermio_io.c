/*
 * gtermio_io.c
 *
 * Extracted I/O core from gtermio.c (XGterm).
 * Contains:
 *  - circular graphics buffer and macros
 *  - marker/control byte definitions
 *  - gio_ptyinput(): filter pty input into text vs graphics
 *
 * Notes:
 *  - This module expects the drawing/processing routine gio_processdata()
 *    to be provided by the rest of the gterm code (extern).
 *  - External functions used here (v_write, Obm*, Gt* etc.) are declared
 *    as extern; link against your original object files.
 */

#include <string.h>
#include <stdlib.h>
#include <ctype.h>
#include <unistd.h>
#include <stdio.h>
#include <sys/ioctl.h>

/* Size limiting definitions (kept from original) */
#define SZ_GBUF     16384
#define GB_MINSPACE 2048
#define GB_BIGSPACE 12288

/* ASCII control codes used by the protocol */
#define ETX     '\003'
#define ENQ     '\005'
#define BEL     '\007'
#define CR      '\015'
#define CAN     '\030'
#define EM      '\031'
#define SUB     '\032'
#define ESC     '\033'
#define FS      '\034'
#define GS      '\035'   /* group separator: graphics start in protocol */
#define RS      '\036'
#define US      '\037'

/* xterm equivalents used by original code */
#define XGS        "[?38h"   /* xterm GS escape sequence (after ESC) */
#define LEN_XGS    5
#define XGE        "\003"    /* xterm graphics terminator (ETX) */
#define LEN_XGE    1

/* Circular graphics buffer and helpers */
static unsigned char g_buf[SZ_GBUF];
static unsigned char *g_top = &g_buf[SZ_GBUF];
static unsigned char *g_ip = g_buf;   /* input pointer (read from) */
static unsigned char *g_op = g_buf;   /* output pointer (write to) */

#define g_getc(c)   (g_ip == g_op ? ((c)=0, -1) : ((c) = *g_ip++, g_ip >= g_top ? *(char *)(g_ip = g_buf) : 0))
#define g_putc(c)   (*g_op++ = (c), ((g_op >= g_top) ? g_op = g_buf : 0))
#define g_ungetc(c) (g_ip = ((g_ip == g_buf) ? g_top - 1 : g_ip - 1))
#define g_spaceleft (g_ip <= g_op ? (g_top - g_op + g_ip - g_buf) : (g_ip - g_op))
#define g_havedata  (g_ip != g_op)
#define g_mark(ip)  ((ip) = g_ip)
#define g_reset(ip) (g_ip = (ip))
#define g_equal(ip) ((ip) == g_ip)

/* External hooks / functions (implementations live in the original project) */
extern int v_write(int fd, const char *buf, int n); /* write helper used in original */
extern int gio_processdata(void); /* process graphics data from g_buf (original state machine) */

/* State variables (kept names from original for easy integration) */
static int gio_graphicsenabled = 0;  /* are we in graphics mode? */
static int gio_enabled = 1;          /* overall enable flag */
static int pty_fd = -1;              /* pty file descriptor (set by caller) */
static int pty_stop = 0;             /* XOFF state on pty */

/* trailer codes used by some sequences */
static int trailer1 = '\r';
static int trailer2 = -1;

/*
 * gio_ptyinput
 *
 * Called by the low-level pty reader with a buffer of bytes read from the
 * pseudoterminal. This routine:
 *  - scans for graphics start (GS or ESC XGS) when not in graphics mode
 *  - when graphics mode is entered, copies graphics bytes into the circular
 *    buffer g_buf (filtering NULs), and returns the remaining non-graphics
 *    bytes to be forwarded to the text widget.
 *
 * Parameters:
 *  - notused: legacy arg (kept for compatibility)
 *  - ttybuf: pointer to the buffer read from the pty
 *  - nchars: number of bytes in ttybuf
 *
 * Returns:
 *  - number of bytes of non-graphics data that should be forwarded to the
 *    text terminal (i.e., length of the returned text in ttybuf).
 *
 * Behavior mirrors the original gtermio.c implementation.
 */
int
gio_ptyinput(int notused, char *ttybuf, int nchars)
{
    char *itop = ttybuf + nchars;
    char *op, *ip = ttybuf;
    char ch;

    (void) notused;

    if (!gio_enabled || nchars <= 0)
        return nchars;

    /* If in text mode, scan quickly for graphics start; if none, return all */
    if (!gio_graphicsenabled) {
        while (ip < itop && *ip != GS &&
               !(*ip == ESC && strncmp(ip + 1, XGS, LEN_XGS) == 0))
            ip++;
        if (ip >= itop)
            return nchars;   /* no graphics start in this packet */
        else
            op = ip;         /* op points to first graphics char to be removed */
    } else {
        op = ttybuf;
    }

    /* If the gio buffer is low on space, call the processing routine to free space */
    if (g_spaceleft < (GB_MINSPACE > nchars ? GB_MINSPACE : nchars)) {
        while (g_spaceleft < (GB_BIGSPACE > nchars ? GB_BIGSPACE : nchars))
            if (gio_processdata())
                break;
    }

    /* Process the packet: copy graphics bytes into g_buf, filter NULs.
     * When in graphics mode, look for CAN (end) or ESC XGE (xterm terminator).
     * When not in graphics mode, look for GS or ESC XGS to enter graphics mode.
     */
    while (ip < itop) {
        if (gio_graphicsenabled) {
            while (ip < itop) {
                if ((ch = *ip++) == CAN) {
                    /* CAN ends graphics mode; push CAN into buffer and exit graphics */
                    g_putc(ch);
                    gio_graphicsenabled = 0;
                    break;
                } else if (ch == ESC && strncmp(ip, XGE, LEN_XGE) == 0) {
                    /* Treat ESC + XGE (ETX) as graphics terminator */
                    ip += LEN_XGE;
                    trailer1 = '\r';
                    ch = CAN;
                    g_putc(ch);
                    gio_graphicsenabled = 0;
                    break;
                } else if (ch) {
                    /* normal graphics byte: append to circular buffer */
                    g_putc(ch);
                }
            }
        } else {
            while (ip < itop) {
                ch = *ip++;
                if (ch == GS) {
                    /* GS starts graphics mode */
                    g_putc(GS);
                    gio_graphicsenabled = 1;
                    break;
                } else if (ch == ESC && strncmp(ip, XGS, LEN_XGS) == 0) {
                    /* ESC + XGS (xterm start) */
                    ip += LEN_XGS;
                    trailer1 = -1; /* special trailer handling in original */
                    g_putc(GS);
                    gio_graphicsenabled = 1;
                    break;
                } else if (ch) {
                    /* non-graphics byte: copy to output region */
                    *op++ = ch;
                }
            }
        }
    }

    /* Return the number of non-graphics bytes (op - ttybuf) to be forwarded */
    return (op - ttybuf);
}

/* Minimal helper to set the pty fd used by this module. Call this from your
 * pty manager after opening the pty.
 */
void gtermio_set_pty_fd(int fd)
{
    pty_fd = fd;
}

/* Expose a small API to query whether graphics data is pending in the buffer */
int gtermio_have_graphics_data(void)
{
    return g_havedata;
}

/* Note:
 * - The real implementation calls gio_processdata() to decode g_buf and draw.
 * - The original code also used v_write(pty_fd, ...) to send responses back to
 *   the client; v_write is declared extern above and should be provided by the
 *   rest of the program (it is a small wrapper around write()).
 *
 * Integration:
 * - Link this module with the original gterm drawing code that implements
 *   gio_processdata(), Gt* widget calls, and OBM callbacks.
 */
