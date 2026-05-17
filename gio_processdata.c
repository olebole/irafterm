/*
 * gio_processdata.c
 *
 * Extracted implementation of gio_processdata() using a table-driven
 * ESC-sequence scanner.  This version fixes two issues reported by the
 * compiler:
 *  - suppresses -Wmissing-braces around the e_table initializer (pragma)
 *  - avoids sizeof on an incomplete extern char[] by using a compile-time
 *    TX_BUF_SIZE constant
 *
 * Requirements at link time: the external symbols listed after the code.
 *
 * Note: gtermio.esc must still be present and contain the initializer rows
 * for the escape table (no surrounding braces required).
 */

#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <ctype.h>
#include <X11/Xlib.h>    /* for XPoint */
#include <limits.h>

/* --- Constants (match original gtermio.c) --- */
#define SZ_GBUF     16384
#define MAX_PLPTS   4096
#define MAX_QUOTA   512
#define MAX_CANDIDATES 64
#define MAX_FIELDS 6

/* Tek/text buffer size (avoid sizeof on extern) */
#define TX_BUF_SIZE 1024

/* Modes */
#define COMMAND_MODE    0
#define ALPHA_MODE      1
#define TEXT_MODE       2
#define VECTOR_MODE     3
#define MESSAGE_MODE    4
#define WIMAGE_MODE     5
#define WCMAP_MODE      6
#define WIOMAP_MODE     7
#define CURSOR_MODE     8

/* Data levels */
#define SET_BITS    0
#define CLEAR_BITS  1
#define TOGGLE_BITS 2

/* ASCII control codes */
#define ETX     '\003'
#define ENQ     '\005'
#define BEL     '\007'
#define CR      '\015'
#define CAN     '\030'
#define EM      '\031'
#define SUB     '\032'
#define ESC     '\033'
#define FS      '\034'
#define GS      '\035'
#define RS      '\036'
#define US      '\037'

/* xterm equivalents */
#define XGS        "[?38h"
#define LEN_XGS    5
#define XGE        "\003"
#define LEN_XGE    1

/* --- External buffer / wrapper declarations (must be provided at link time) --- */
extern unsigned char g_buf[];          /* circular buffer storage */
extern unsigned char *g_top;
extern unsigned char *g_ip;
extern unsigned char *g_op;

extern int g_getc_wrapper(int *out_c);   /* -1 if no data, else 0 and *out_c set */
extern void g_putc_wrapper(int c);
extern void g_ungetc_wrapper(int c);
extern int g_spaceleft_wrapper(void);
extern int g_havedata_wrapper(void);

/* --- External Gterm / OBM / helpers (must be provided at link time) --- */
extern void *gw; /* opaque Gterm widget pointer */

extern void GtDrawVector(void *gw, int x1, int y1, int x2, int y2, int mode);
extern void GtDrawPoint(void *gw, int x, int y, int mode);
extern void GtDrawPolyline(void *gw, XPoint *pts, int npts, int mode);
extern void GtClearScreen(void *gw);
extern void GtSetRaster(void *gw, int r);
extern void GtSetCursorType(void *gw, int type);
extern void GtSetTextRes(void *gw, int rows, int cols);
extern void GtGetPhysRes(void *gw, int raster, int *xres, int *yres);
extern void GtSetLogRes(void *gw, int xres, int yres);
extern void GtGetAlphaTextSize(void *gw, void *font, int *w, int *h, int *base);
extern void GtGetDialogTextSize(void *gw, char *s, int *w, int *h, int *base);
extern int  GtSelectRaster(void *gw, int dummy, int pixel, int sx, int sy, int mode, int *rx, int *ry, int *mapping);
extern void GtGetCursorPos(void *gw, int *sx, int *sy);
extern int  GtGetRaster(void *gw);
extern void GtEraseAlphaCursor(void *gw);

/* OBM / request / cursor helpers */
extern int gio_queue_request(int sx, int sy, int raster, int rx, int ry, int key, char *strval);
extern int gio_retcursor(int key, int sx, int sy, int raster, int rx, int ry, int datalen);
extern void ObmDeliverMessage(const char *msg);

/* Utility / I/O */
extern int v_write(int fd, const char *buf, int n);

/* Other helpers from original gtermio */
extern void pl_decodepts(void);
extern void gio_retenq(void);

/* Globals used by the state machine (from gtermio.c) */
extern int gio_mode;
extern int gio_datalevel;
extern int trailer1;
extern int trailer2;
extern int wait_cursor;
extern int workstation_open;
extern int tx_leftmargin;
extern int tx_charbase;
extern int cur_x, cur_y;
extern int tek_xres, tek_yres;
extern int win_xres, win_yres;
extern int pl_npts, pl_op, pl_pointmode, pl_areamode;
extern int sl_x, sl_y, sl_charwidth, sl_charheight, sl_charbase;
extern char tx_buf[]; /* polytext buffer (extern) */
extern int tx_len;

/* Imaging/colormap globals */
extern int wi_encoding, wi_raster, wi_x1, wi_y1, wi_nx, wi_ny, wi_bp;
extern int wc_map, wc_first, wc_ncolors;

/* Cursor constants */
extern int GtIdleCursor;
extern int GtBusyCursor;

/* --- ESC table-driven scanner --- */

struct _esc {
    char e_tag;                      /* integer code for escseq */
    char e_seq[MAX_FIELDS+1];        /* the sequence itself (chars) */
};

/* Suppress missing-braces warning around the included initializer rows */
#if defined(__GNUC__)
#pragma GCC diagnostic push
#pragma GCC diagnostic ignored "-Wmissing-braces"
#endif

static struct _esc e_table[] = {
#include "gtermio.esc"
    { 0, {0,0,0,0,0,0,0} }
};

#if defined(__GNUC__)
#pragma GCC diagnostic pop
#endif

static struct _esc *e_cand1[MAX_CANDIDATES];
static struct _esc *e_cand2[MAX_CANDIDATES];
static struct _esc **e_pcand = e_cand1;
static struct _esc **e_acand = e_cand2;
static int e_npcand = 0, e_nacand = 0;
static int e_charno = 0;
static int scanok = 0;

/* Forward declarations for scanner helpers */
static int startscan(void);
static int getint(int *value);
static int endscan(void);

/* --- Tek coordinate decoding helpers --- */

static int read_tek_coord(int *out)
{
    int b1, b2;
    if (g_getc_wrapper(&b1) < 0) return -1;
    if (g_getc_wrapper(&b2) < 0) { g_ungetc_wrapper(b1); return -1; }
    if (b1 < 32 || b2 < 32) {
        g_ungetc_wrapper(b2);
        g_ungetc_wrapper(b1);
        return -1;
    }
    *out = ((b1 - 32) << 6) | (b2 - 32);
    return 0;
}

static int read_tek_coord_pair(int *x, int *y)
{
    if (read_tek_coord(x) < 0) return -1;
    if (read_tek_coord(y) < 0) return -1;
    return 0;
}

/* --- ESC scanner implementation --- */

static int startscan(void)
{
    struct _esc *ep;
    e_pcand = e_cand1;
    e_acand = e_cand2;
    e_npcand = e_nacand = 0;
    e_charno = 0;
    scanok = 1;

    for (ep = e_table; ep->e_tag != 0 || ep->e_seq[0] != 0; ep++) {
        if (ep->e_seq[0]) {
            if (e_npcand < MAX_CANDIDATES)
                e_pcand[e_npcand++] = ep;
        }
    }
    return e_npcand;
}

static int getint(int *value)
{
    int ch;
    if (g_getc_wrapper(&ch) < 0) return -1;

    e_nacand = 0;
    for (int i = 0; i < e_npcand; ++i) {
        struct _esc *ep = e_pcand[i];
        unsigned char expected = (unsigned char) ep->e_seq[e_charno];
        if (expected == 0) continue;
        if (expected == (unsigned char)ch) {
            if (e_nacand < MAX_CANDIDATES)
                e_acand[e_nacand++] = ep;
        }
    }

    struct _esc **tmp = e_pcand;
    e_pcand = e_acand;
    e_acand = tmp;
    e_npcand = e_nacand;
    e_charno++;

    if (e_npcand == 0) scanok = 0;
    *value = ch;
    return ch;
}

static int endscan(void)
{
    if (!scanok || e_npcand == 0) return 0;
    for (int i = 0; i < e_npcand; ++i) {
        struct _esc *ep = e_pcand[i];
        if ((int)strlen(ep->e_seq) == e_charno) {
            return (int)ep->e_tag;
        }
    }
    return 0;
}

/* --- Minimal helpers for polyline/text/message handling --- */

static XPoint pl_points[MAX_PLPTS];
static int pl_count = 0;

static void flush_polyline(void)
{
    if (pl_count > 0 && gw) {
        GtDrawPolyline(gw, pl_points, pl_count, gio_datalevel);
    }
    pl_count = 0;
}

static void flush_textline(void)
{
    if (tx_len > 0) {
        tx_len = 0;
        /* tx_buf is extern; clear first char if available */
        if (TX_BUF_SIZE > 0) {
            /* can't safely write to tx_buf[] length unknown at compile time,
             * but clearing first byte is safe if buffer exists */
            if (tx_buf) tx_buf[0] = '\0';
        }
    }
}

static void enqueue_message(const char *msg, int n)
{
    if (n <= 0) return;
    char *tmp = (char *)malloc(n + 1);
    if (!tmp) return;
    memcpy(tmp, msg, n);
    tmp[n] = '\0';
    ObmDeliverMessage(tmp);
    free(tmp);
}

/* --- Main processing function --- */
/* Returns 1 if more work remains (buffer still has data), 0 otherwise. */
int gio_processdata(void)
{
    int quota = 0;

    if (!g_havedata_wrapper()) return 0;

    while (g_havedata_wrapper()) {
        if (++quota > MAX_QUOTA) return 1; /* yield, more work remains */

        switch (gio_mode) {
        case COMMAND_MODE: {
            int ch;
            if (g_getc_wrapper(&ch) < 0) return 0;

            if (ch == GS) {
                gio_mode = VECTOR_MODE;
                pl_count = 0;
                continue;
            } else if (ch == US) {
                gio_mode = ALPHA_MODE;
                continue;
            } else if (ch == CAN) {
                gio_mode = COMMAND_MODE;
                continue;
            } else if (ch == ESC) {
                int rc;
                startscan();
                while (1) {
                    int v;
                    rc = getint(&v);
                    if (rc < 0) {
                        /* incomplete sequence: yield to caller */
                        return 1;
                    }
                    int tag = endscan();
                    if (tag) {
                        switch (tag) {
                        case ESC_SETTEXTMODE:
                            gio_mode = TEXT_MODE;
                            break;
                        case ESC_ENQUIRE:
                            gio_retenq();
                            break;
                        case ESC_READCURSOR:
                            gio_retcursor(SUB, 0,0,0,0,0,0);
                            break;
                        case ESC_WINCURSOR:
                            gio_retcursor(SUB, 0,0,0,0,0,0);
                            break;
                        case ESC_CLEARSCREEN:
                            if (gw) GtClearScreen(gw);
                            break;
                        case ESC_SETCURSOR:
                            /* not implemented here */
                            break;
                        case ESC_RASTERINIT:
                            /* not implemented here */
                            break;
                        case ESC_WRITEPIXELS:
                            gio_mode = WIMAGE_MODE;
                            break;
                        case ESC_WRITECMAP:
                            gio_mode = WCMAP_MODE;
                            break;
                        case ESC_WRITEIOMAP:
                            gio_mode = WIOMAP_MODE;
                            break;
                        case ESC_SETRESET:
                            /* not implemented here */
                            break;
                        case ESC_SETRESIZE:
                            /* not implemented here */
                            break;
                        default:
                            break;
                        }
                        break;
                    }
                    if (!scanok) break;
                }
                continue;
            } else if (ch == ENQ) {
                gio_retenq();
                continue;
            } else if (ch == SUB) {
                gio_retcursor(SUB, 0,0,0,0,0,0);
                continue;
            } else {
                g_ungetc_wrapper(ch);
                return 0;
            }
            break;
        } /* COMMAND_MODE */

        case ALPHA_MODE: {
            int ch;
            if (g_getc_wrapper(&ch) < 0) return 0;
            if (ch == GS || ch == CAN || ch == ESC || ch == US) {
                g_ungetc_wrapper(ch);
                gio_mode = COMMAND_MODE;
                continue;
            }
            if (gw) {
                GtDrawPoint(gw, cur_x, cur_y, gio_datalevel);
            }
            cur_x += 8;
            continue;
        }

        case TEXT_MODE: {
            int ch;
            if (g_getc_wrapper(&ch) < 0) return 0;
            if (ch == GS || ch == CAN) {
                flush_textline();
                gio_mode = COMMAND_MODE;
                continue;
            }
            /* avoid sizeof(tx_buf) on extern; use TX_BUF_SIZE */
            if (tx_len < (int)(TX_BUF_SIZE - 1)) {
                if (tx_buf) {
                    tx_buf[tx_len++] = (char)ch;
                    tx_buf[tx_len] = '\0';
                } else {
                    /* if tx_buf not available, just increment length guard */
                    if (tx_len < TX_BUF_SIZE - 1) tx_len++;
                }
            }
            continue;
        }

        case VECTOR_MODE: {
            int peek;
            if (g_getc_wrapper(&peek) < 0) return 0;
            if (peek == CAN || peek == GS || peek == ESC || peek == US) {
                g_ungetc_wrapper(peek);
                flush_polyline();
                gio_mode = COMMAND_MODE;
                continue;
            }
            g_ungetc_wrapper(peek);
            int tx, ty;
            if (read_tek_coord_pair(&tx, &ty) < 0) {
                return 1;
            }
            int winx = (tx * win_xres + tek_xres/2) / tek_xres;
            int winy = win_yres - 1 - ((ty * win_yres + tek_yres/2) / tek_yres);

            if (pl_count < MAX_PLPTS) {
                pl_points[pl_count].x = winx;
                pl_points[pl_count].y = winy;
                pl_count++;
            } else {
                flush_polyline();
                pl_points[0].x = winx;
                pl_points[0].y = winy;
                pl_count = 1;
            }
            cur_x = winx;
            cur_y = winy;
            continue;
        }

        case MESSAGE_MODE: {
            int ch2;
            char *mbuf = NULL;
            int mlen = 0;
            while (g_getc_wrapper(&ch2) >= 0) {
                if (ch2 == CAN || ch2 == GS || ch2 == ESC || ch2 == US) {
                    g_ungetc_wrapper(ch2);
                    break;
                }
                char *tmp = (char *)realloc(mbuf, mlen + 1);
                if (!tmp) { free(mbuf); return 0; }
                mbuf = tmp;
                mbuf[mlen++] = (char)ch2;
            }
            if (mlen > 0) enqueue_message(mbuf, mlen);
            free(mbuf);
            gio_mode = COMMAND_MODE;
            continue;
        }

        case WIMAGE_MODE: {
            int ch2;
            while (g_getc_wrapper(&ch2) >= 0) {
                if (ch2 == CAN) { gio_mode = COMMAND_MODE; break; }
                if (ch2 == ESC) {
                    int a;
                    if (g_getc_wrapper(&a) < 0) { g_ungetc_wrapper(ESC); return 1; }
                    if (a == XGE[0]) {
                        gio_mode = COMMAND_MODE;
                        continue;
                    } else {
                        g_ungetc_wrapper(a);
                    }
                }
                /* pixel bytes consumed (not stored here) */
            }
            continue;
        }

        case WCMAP_MODE: {
            int needed = wc_ncolors * 3;
            int got = 0;
            int ch2;
            while (got < needed && g_getc_wrapper(&ch2) >= 0) {
                got++;
            }
            if (got >= needed) {
                gio_mode = COMMAND_MODE;
            } else {
                return 1;
            }
            continue;
        }

        case WIOMAP_MODE: {
            int ch2;
            while (g_getc_wrapper(&ch2) >= 0) {
                if (ch2 == CAN) { gio_mode = COMMAND_MODE; break; }
            }
            continue;
        }

        case CURSOR_MODE: {
            int ch2;
            if (g_getc_wrapper(&ch2) < 0) return 0;
            gio_retcursor(ch2, 0,0,0,0,0,0);
            gio_mode = COMMAND_MODE;
            continue;
        }

        default:
            gio_mode = COMMAND_MODE;
            continue;
        } /* switch */
    } /* while */

    return (g_havedata_wrapper() ? 1 : 0);
}
