/*
 * gtermio_buffer.c
 *
 * Shared circular buffer and wrapper functions for gtermio / gio_processdata.
 *
 * This file implements the circular graphics buffer and the small wrapper
 * functions that mirror the original gtermio macros:
 *   g_getc_wrapper, g_putc_wrapper, g_ungetc_wrapper,
 *   g_spaceleft_wrapper, g_havedata_wrapper
 *
 * The buffer size must match the value used when extracting gio_processdata.
 * If you changed SZ_GBUF in gio_processdata.c, change it here as well.
 *
 * This implementation is single-threaded (same assumptions as original).
 * It intentionally keeps semantics compatible with the original macros:
 *  - g_getc_wrapper returns -1 when no data available, otherwise 0 and sets *out_c.
 *  - g_putc_wrapper writes a byte into the buffer (wraps around).
 *  - g_ungetc_wrapper pushes one byte back (wraps around).
 *  - g_spaceleft_wrapper returns free bytes available for writing.
 *  - g_havedata_wrapper returns nonzero if buffer contains data.
 */

#include <stdlib.h>
#include <string.h>
#include <stdio.h>

#ifndef SZ_GBUF
#define SZ_GBUF 16384
#endif

/* Circular buffer storage */
unsigned char g_buf[SZ_GBUF];
unsigned char *g_top = &g_buf[SZ_GBUF]; /* end of buffer + 1 */
unsigned char *g_ip = g_buf;            /* input pointer (read) */
unsigned char *g_op = g_buf;            /* output pointer (write) */

/* Helper: advance pointer with wrap */
static inline unsigned char *advance_ptr(unsigned char *p, int n) {
    p += n;
    if (p >= g_top) p = g_buf + (p - g_top);
    return p;
}

/* g_getc_wrapper
 *  - If no data available returns -1.
 *  - Otherwise sets *out_c to next byte and returns 0.
 */
int g_getc_wrapper(int *out_c)
{
    if (g_ip == g_op) {
        /* empty */
        return -1;
    }
    *out_c = (int)*g_ip++;
    if (g_ip >= g_top) g_ip = g_buf;
    return 0;
}

/* g_putc_wrapper
 *  - Writes a byte into the buffer at g_op and advances g_op.
 *  - If buffer would overflow (no space left) this implementation
 *    overwrites oldest data (same behavior as many circular buffers).
 *    The original gtermio code ensured space before writing; keep that
 *    contract in the caller or adapt this behavior if you prefer to drop.
 */
void g_putc_wrapper(int c)
{
    g_buf[(size_t)(g_op - g_buf)] = (unsigned char)c;
    g_op = advance_ptr(g_op, 1);
    /* If we just caught up with g_ip, advance g_ip to avoid ambiguity (overwrite) */
    if (g_op == g_ip) {
        g_ip = advance_ptr(g_ip, 1);
    }
}

/* g_ungetc_wrapper
 *  - Pushes one byte back into the input stream (moves g_ip backward).
 *  - Caller must ensure there is space (i.e., not more ungetc than prior gets).
 */
void g_ungetc_wrapper(int c)
{
    /* Move g_ip back one position (wrap if needed) and store c there */
    if (g_ip == g_buf)
        g_ip = g_top - 1;
    else
        g_ip--;
    *g_ip = (unsigned char)c;
}

/* g_spaceleft_wrapper
 *  - Returns number of free bytes available for writing.
 *  - Matches semantics used in gtermio: if g_ip <= g_op then free =
 *      (g_top - g_op + g_ip - g_buf)
 *    else free = (g_ip - g_op)
 */
int g_spaceleft_wrapper(void)
{
    if (g_ip <= g_op)
        return (int)((g_top - g_op) + (g_ip - g_buf));
    else
        return (int)(g_ip - g_op);
}

/* g_havedata_wrapper
 *  - Nonzero if buffer contains data (g_ip != g_op)
 */
int g_havedata_wrapper(void)
{
    return (g_ip != g_op);
}

/* Optional utility: reset buffer (used by gtermio_hardreset) */
void g_buffer_reset(void)
{
    g_ip = g_op = g_buf;
    g_top = &g_buf[SZ_GBUF];
    memset(g_buf, 0, SZ_GBUF);
}

/* Debug helper (optional) */
#ifdef GTERMIO_BUFFER_DEBUG
#include <stdio.h>
void g_buffer_dump(void)
{
    fprintf(stderr, "g_buf: ip=%ld op=%ld spaceleft=%d havedata=%d\n",
        (long)(g_ip - g_buf), (long)(g_op - g_buf),
        g_spaceleft_wrapper(), g_havedata_wrapper());
}
#endif
