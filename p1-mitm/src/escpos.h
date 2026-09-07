#pragma once

#include <stdint.h>
#include <stddef.h>

/**
 * escpos.h — ESC/POS protocol state machine parser
 *
 * ESC/POS (Epson Standard Code for Point of Sale) is a binary protocol
 * used by Epson receipt printers. The POS system sends a mix of:
 *   - Printable ASCII characters (0x20–0x7E) — receipt text
 *   - Control bytes (LF=0x0A, CR=0x0D) — line terminators
 *   - Escape sequences (ESC = 0x1B followed by command byte + params)
 *   - GS sequences   (GS  = 0x1D followed by command byte + params)
 *   - DLE sequences  (DLE = 0x10 followed by 0x04 = status request)
 *
 * This parser extracts only the printable text content, discarding all
 * formatting commands. It does NOT need to understand every command
 * in full — it only needs to know how many bytes each command consumes
 * so it can skip them without treating param bytes as text.
 *
 * The receipt boundary is detected by the paper cut command (GS V).
 * When a cut is detected, the accumulated receipt lines are considered
 * complete and ready for overlay display.
 *
 * IMPORTANT: The DLE EOT (0x10 0x04) status request is NOT handled by
 * this parser. It is handled directly in the main read loop because
 * it requires an immediate write-back response and must not be delayed
 * by parser processing. This parser simply discards DLE sequences.
 */

/* Maximum characters per receipt line (Epson TM-T88VII: 42 chars @ 80mm) */
#define ESCPOS_MAX_LINE_LEN  48

/* Maximum lines accumulated per receipt before auto-flush */
#define ESCPOS_MAX_LINES     32

/* Return codes from escpos_feed() */
#define ESCPOS_OK            0   /* Byte consumed, no complete line yet   */
#define ESCPOS_LINE_READY    1   /* A complete text line was produced      */
#define ESCPOS_RECEIPT_CUT   2   /* GS V cut detected — receipt complete  */

/**
 * Parser state machine states.
 * Normal flow: STATE_NORMAL → collect text → LF → line complete.
 * Command flow: STATE_NORMAL → ESC/GS/DLE → STATE_CMD → skip params.
 */
typedef enum {
    STATE_NORMAL = 0,  /* Collecting printable bytes into current line    */
    STATE_ESC,         /* Received 0x1B, waiting for command byte         */
    STATE_GS,          /* Received 0x1D, waiting for command byte         */
    STATE_DLE,         /* Received 0x10, waiting for 0x04                 */
    STATE_SKIP,        /* Skipping N remaining parameter bytes            */
    STATE_GS_V,        /* Received GS V, waiting for subcommand byte      */
    STATE_GS_V_FULL,   /* GS V 65/66: waiting for 1 param byte then cut  */
    STATE_ESC_P,       /* ESC p (cash drawer): skip 2 param bytes        */
    STATE_ESC_P2,      /* ESC p: skip second param byte                  */
    STATE_GS_K,          /* GS k (barcode): skip variable-length data      */
    STATE_GS_K_LEN,      /* GS k (format 2): reading length byte           */
    STATE_GS_K_DATA,     /* GS k: skipping barcode data bytes              */
    STATE_GS_PAREN_PL,   /* GS ( : waiting for pL (payload length low)     */
    STATE_GS_PAREN_PH,   /* GS ( : waiting for pH (payload length high)    */
    STATE_GS_PAREN_DATA, /* GS ( : skipping pL + pH*256 payload bytes      */
} EscPosState;

typedef struct {
    EscPosState state;

    /* Current line buffer */
    char   line_buf[ESCPOS_MAX_LINE_LEN + 1];
    size_t line_len;

    /* Accumulated receipt lines */
    char   lines[ESCPOS_MAX_LINES][ESCPOS_MAX_LINE_LEN + 1];
    size_t line_count;

    /* Generic skip counter for multi-byte param sequences */
    int    skip_remaining;

    /* GS k barcode: data length and bytes remaining */
    int    barcode_type;
    int    barcode_remaining;
} EscPosParser;

/**
 * Initialise a parser. Must be called before first use.
 */
void escpos_init(EscPosParser *p);

/**
 * Feed a single byte into the parser.
 *
 * Returns:
 *   ESCPOS_OK           — byte consumed, nothing to report
 *   ESCPOS_LINE_READY   — a line was completed; read it from p->lines[p->line_count-1]
 *                         (line_count incremented; if line_count == MAX_LINES, also flush)
 *   ESCPOS_RECEIPT_CUT  — GS V paper cut detected; call escpos_get_receipt() and
 *                         escpos_reset_receipt() to retrieve and clear lines
 */
int escpos_feed(EscPosParser *p, uint8_t byte);

/**
 * Feed a buffer of bytes into the parser.
 * Calls escpos_feed() for each byte.
 * If ESCPOS_RECEIPT_CUT is returned for any byte, stops and returns
 * ESCPOS_RECEIPT_CUT immediately (remaining bytes in buf are NOT consumed).
 * Returns ESCPOS_LINE_READY if any line was produced (but no cut yet).
 * Returns ESCPOS_OK if nothing significant happened.
 */
int escpos_feed_buf(EscPosParser *p, const uint8_t *buf, size_t len,
                    size_t *bytes_consumed);

/**
 * Flush the current partial line (if any) as a complete line.
 * Used when GS V is detected mid-buffer — ensures the last line
 * before the cut is not lost.
 */
void escpos_flush_line(EscPosParser *p);

/**
 * Build a single overlay string from accumulated receipt lines.
 * Joins lines with '\n'. Writes into out_buf (max out_len bytes).
 * Returns the number of bytes written (excluding NUL terminator).
 */
size_t escpos_get_receipt(const EscPosParser *p, char *out_buf, size_t out_len);

/**
 * Clear accumulated receipt lines. Call after escpos_get_receipt().
 * Does NOT reset the state machine — parsing continues.
 */
void escpos_reset_receipt(EscPosParser *p);
