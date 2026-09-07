#include "escpos.h"

#include <string.h>
#include <stdio.h>
#include <ctype.h>

/* ESC/POS control bytes */
#define ESC  0x1B
#define GS   0x1D
#define DLE  0x10
#define EOT  0x04
#define LF   0x0A
#define CR   0x0D
#define FF   0x0C  /* Form feed — treat as line break */

void escpos_init(EscPosParser *p)
{
    memset(p, 0, sizeof(*p));
    p->state = STATE_NORMAL;
}

/**
 * Commit the current line buffer as a completed line.
 * Strips trailing spaces. Empty lines are kept (blank receipt line).
 * If ESCPOS_MAX_LINES is reached, oldest lines are silently discarded.
 */
static int commit_line(EscPosParser *p)
{
    /* Trim trailing spaces */
    while (p->line_len > 0 && p->line_buf[p->line_len - 1] == ' ')
        p->line_len--;

    /* NUL-terminate */
    p->line_buf[p->line_len] = '\0';

    /* Store in receipt buffer (overwrite oldest if full) */
    size_t idx = p->line_count < ESCPOS_MAX_LINES
                 ? p->line_count
                 : ESCPOS_MAX_LINES - 1;

    memcpy(p->lines[idx], p->line_buf, p->line_len + 1);
    if (p->line_count < ESCPOS_MAX_LINES)
        p->line_count++;

    /* Reset line buffer */
    p->line_len = 0;
    memset(p->line_buf, 0, sizeof(p->line_buf));

    return ESCPOS_LINE_READY;
}

void escpos_flush_line(EscPosParser *p)
{
    if (p->line_len > 0)
        commit_line(p);
}

int escpos_feed(EscPosParser *p, uint8_t byte)
{
    switch (p->state) {

    /* ------------------------------------------------------------------ */
    case STATE_NORMAL:
        if (byte == ESC) {
            p->state = STATE_ESC;
            return ESCPOS_OK;
        }
        if (byte == GS) {
            p->state = STATE_GS;
            return ESCPOS_OK;
        }
        if (byte == DLE) {
            p->state = STATE_DLE;
            return ESCPOS_OK;
        }
        if (byte == LF || byte == CR || byte == FF) {
            /* Line feed / carriage return = end of line */
            return commit_line(p);
        }
        /* Printable ASCII — append to current line */
        if (byte >= 0x20 && byte <= 0x7E) {
            if (p->line_len < ESCPOS_MAX_LINE_LEN)
                p->line_buf[p->line_len++] = (char)byte;
        }
        /* All other bytes (non-printable, >0x7E) are silently ignored */
        return ESCPOS_OK;

    /* ------------------------------------------------------------------ */
    case STATE_ESC:
        /*
         * We received ESC (0x1B). This byte is the command byte.
         * Switch on which command follows and decide how many param
         * bytes to skip.
         */
        switch (byte) {
        case 0x40: /* ESC @  — Initialize printer. No params. */
            p->state = STATE_NORMAL;
            break;
        case 0x21: /* ESC !  — Select print mode. 1 param byte. */
        case 0x2D: /* ESC -  — Underline. 1 param. */
        case 0x45: /* ESC E  — Bold on/off. 1 param. */
        case 0x47: /* ESC G  — Double-strike. 1 param. */
        case 0x4A: /* ESC J  — Print and feed n/180". 1 param. */
        case 0x4D: /* ESC M  — Select font. 1 param. */
        case 0x52: /* ESC R  — Select international charset. 1 param. */
        case 0x53: /* ESC S  — Select standard/page mode. 1 param. */
        case 0x56: /* ESC V  — Rotate 90 degrees. 1 param. */
        case 0x61: /* ESC a  — Select justification. 1 param. */
        case 0x63: /* ESC c  — Select paper sensor. 1 param (simplified). */
        case 0x7B: /* ESC {  — Upside down. 1 param. */
            p->skip_remaining = 1;
            p->state = STATE_SKIP;
            break;
        case 0x20: /* ESC SP — Character right-spacing. 1 param. */
            p->skip_remaining = 1;
            p->state = STATE_SKIP;
            break;
        case 0x64: /* ESC d  — Print and feed n lines. 1 param. Treat as LF. */
            /* We don't know n yet; skip it but also flush current line */
            escpos_flush_line(p);
            p->skip_remaining = 1;
            p->state = STATE_SKIP;
            break;
        case 0x70: /* ESC p  — Open cash drawer. 2 param bytes. */
            p->state = STATE_ESC_P;
            break;
        case 0x74: /* ESC t  — Select character code table. 1 param. */
            p->skip_remaining = 1;
            p->state = STATE_SKIP;
            break;
        case 0x75: /* ESC u  — Obsolete. 1 param. */
            p->skip_remaining = 1;
            p->state = STATE_SKIP;
            break;
        case 0x77: /* ESC w  — Double-width mode (some Epson models). 1 param. */
            p->skip_remaining = 1;
            p->state = STATE_SKIP;
            break;
        default:
            /*
             * Unknown ESC command. Rather than trying to skip unknown
             * params (risking treating data bytes as text), return to
             * NORMAL state and hope the stream re-syncs.
             * Log for debugging.
             */
            fprintf(stderr, "[escpos] unknown ESC 0x%02X — skipping\n", byte);
            p->state = STATE_NORMAL;
            break;
        }
        return ESCPOS_OK;

    /* ------------------------------------------------------------------ */
    case STATE_GS:
        /*
         * We received GS (0x1D). This byte is the GS command byte.
         */
        switch (byte) {
        case 0x21: /* GS !  — Character size. 1 param. */
        case 0x42: /* GS B  — Reverse color. 1 param. */
        case 0x68: /* GS h  — Barcode height. 1 param. */
        case 0x77: /* GS w  — Barcode width. 1 param. */
        case 0x48: /* GS H  — Barcode HRI position. 1 param. */
        case 0x66: /* GS f  — HRI font. 1 param. */
        case 0x72: /* GS r  — Transmit status. 1 param. */
            p->skip_remaining = 1;
            p->state = STATE_SKIP;
            break;
        case 0x4C: /* GS L  — Set left margin. 2 param bytes. */
        case 0x57: /* GS W  — Set print area width. 2 param bytes. */
        case 0x50: /* GS P  — Set resolution. 2 param bytes. */
            p->skip_remaining = 2;
            p->state = STATE_SKIP;
            break;
        case 0x56: /* GS V  — Paper cut — END OF RECEIPT */
            p->state = STATE_GS_V;
            break;
        case 0x6B: /* GS k  — Print barcode (variable length) */
            p->state = STATE_GS_K;
            break;
        case 0x28: /* GS (  — Extended command (GS ( L, GS ( A, GS ( E …)  */
            /*
             * Format: GS ( X  pL  pH  fn  [data...]
             *   pL + pH*256 = total bytes that follow (fn + data).
             * We must read pL and pH to know how many bytes to skip.
             * Fixed-skip (old approach) desynchronises the stream on any
             * real receipt that uses logo printing or extended commands.
             */
            p->state = STATE_GS_PAREN_PL;
            break;
        default:
            fprintf(stderr, "[escpos] unknown GS 0x%02X — skipping\n", byte);
            p->state = STATE_NORMAL;
            break;
        }
        return ESCPOS_OK;

    /* ------------------------------------------------------------------ */
    case STATE_GS_V:
        /*
         * GS V subcommand byte:
         *   0x00 (0)  — Full cut, no feed. No param.
         *   0x01 (1)  — Partial cut, no feed. No param.
         *   0x41 (65) — Full cut after feed n. 1 param byte (n).
         *   0x42 (66) — Partial cut after feed n. 1 param byte (n).
         *   0x30 (48) — Full cut (alternative). No param.
         *   0x31 (49) — Partial cut (alternative). No param.
         *
         * In all cases, this signals END OF RECEIPT.
         * Flush the current line, then signal RECEIPT_CUT.
         */
        escpos_flush_line(p);
        if (byte == 0x41 || byte == 0x42) {
            /* These have 1 param byte we need to consume first */
            p->state = STATE_GS_V_FULL;
        } else {
            p->state = STATE_NORMAL;
            return ESCPOS_RECEIPT_CUT;
        }
        return ESCPOS_OK;

    case STATE_GS_V_FULL:
        /* Consuming the n param byte after GS V 65/66 */
        p->state = STATE_NORMAL;
        return ESCPOS_RECEIPT_CUT;

    /* ------------------------------------------------------------------ */
    case STATE_DLE:
        /*
         * DLE (0x10) received. If next byte is EOT (0x04), this is a
         * printer status request. We do NOT respond here — the main loop
         * handles DLE EOT directly because it needs an immediate write().
         * We just discard this byte and return to NORMAL.
         *
         * Other DLE sequences (DLE ENQ, DLE DC4) are also discarded.
         */
        p->state = STATE_NORMAL;
        return ESCPOS_OK;

    /* ------------------------------------------------------------------ */
    case STATE_SKIP:
        /* Skipping a fixed number of parameter bytes */
        p->skip_remaining--;
        if (p->skip_remaining == 0)
            p->state = STATE_NORMAL;
        return ESCPOS_OK;

    /* ------------------------------------------------------------------ */
    case STATE_ESC_P:
        /* ESC p (cash drawer): first of 2 param bytes */
        p->state = STATE_ESC_P2;
        return ESCPOS_OK;

    case STATE_ESC_P2:
        /* ESC p: second of 2 param bytes */
        p->state = STATE_NORMAL;
        return ESCPOS_OK;

    /* ------------------------------------------------------------------ */
    case STATE_GS_K:
        /*
         * GS k — print barcode. Two formats:
         * Format 1: GS k m d1...dk NUL  (m=0..6, data terminated by NUL)
         * Format 2: GS k m n d1...dn    (m=65..73, n=data length)
         *
         * For our purposes, we discard all barcode data.
         */
        p->barcode_type = byte;
        if (byte >= 65) {
            /* Format 2: next byte is length */
            p->state = STATE_GS_K_LEN;
        } else {
            /* Format 1: data terminated by NUL — we'll catch NUL in DATA */
            p->barcode_remaining = 0;  /* 0 = NUL-terminated */
            p->state = STATE_GS_K_DATA;
        }
        return ESCPOS_OK;

    case STATE_GS_K_LEN:
        p->barcode_remaining = byte;
        p->state = STATE_GS_K_DATA;
        return ESCPOS_OK;

    case STATE_GS_K_DATA:
        if (p->barcode_remaining > 0) {
            p->barcode_remaining--;
            if (p->barcode_remaining == 0)
                p->state = STATE_NORMAL;
        } else {
            /* NUL-terminated: 0x00 ends the data */
            if (byte == 0x00)
                p->state = STATE_NORMAL;
        }
        return ESCPOS_OK;

    /* ------------------------------------------------------------------ */
    case STATE_GS_PAREN_PL:
        /* First length byte of GS ( command — store low byte */
        p->skip_remaining = byte;          /* pL */
        p->state = STATE_GS_PAREN_PH;
        return ESCPOS_OK;

    case STATE_GS_PAREN_PH:
        /* Second length byte — compute total payload length pL + pH*256 */
        p->skip_remaining += (int)byte * 256;   /* add pH*256 */
        if (p->skip_remaining > 0) {
            p->state = STATE_GS_PAREN_DATA;
        } else {
            /* Zero-length payload (unusual but valid) */
            p->state = STATE_NORMAL;
        }
        return ESCPOS_OK;

    case STATE_GS_PAREN_DATA:
        /* Skip payload bytes one at a time */
        p->skip_remaining--;
        if (p->skip_remaining <= 0)
            p->state = STATE_NORMAL;
        return ESCPOS_OK;

    } /* end switch(state) */

    return ESCPOS_OK;
}

int escpos_feed_buf(EscPosParser *p, const uint8_t *buf, size_t len,
                    size_t *bytes_consumed)
{
    int last_ret = ESCPOS_OK;
    size_t i;
    for (i = 0; i < len; i++) {
        int ret = escpos_feed(p, buf[i]);
        if (ret == ESCPOS_RECEIPT_CUT) {
            if (bytes_consumed) *bytes_consumed = i + 1;
            return ESCPOS_RECEIPT_CUT;
        }
        if (ret == ESCPOS_LINE_READY)
            last_ret = ESCPOS_LINE_READY;
    }
    if (bytes_consumed) *bytes_consumed = len;
    return last_ret;
}

size_t escpos_get_receipt(const EscPosParser *p, char *out_buf, size_t out_len)
{
    if (out_len == 0) return 0;
    size_t written = 0;
    for (size_t i = 0; i < p->line_count && written < out_len - 1; i++) {
        size_t line_len = strlen(p->lines[i]);
        /* Skip completely blank lines at the start of the receipt */
        if (i == 0 && line_len == 0) continue;
        if (written + line_len + 1 >= out_len) break;
        if (written > 0) out_buf[written++] = '\n';
        memcpy(out_buf + written, p->lines[i], line_len);
        written += line_len;
    }
    out_buf[written] = '\0';
    return written;
}

void escpos_reset_receipt(EscPosParser *p)
{
    p->line_count = 0;
    memset(p->lines, 0, sizeof(p->lines));
}
