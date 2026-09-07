#pragma once

#include <stddef.h>

/**
 * overlay.h — Receipt text file writer for P1-MITM
 *
 * Writes decoded receipt text to a file that P2-overlay watches via
 * inotify. This is the IPC bridge between Project 1 (USB MITM) and
 * Project 2 (video overlay).
 *
 * Design: atomic write using write-to-temp-then-rename pattern.
 * This prevents P2 from reading a partial file while P1 is writing.
 *
 * The clear-after-timeout uses SIGALRM + alarm() — no thread needed.
 * Each new receipt resets the alarm. When the alarm fires, the file
 * is cleared (written with empty content).
 *
 * On combined single-device deployment (future): replace file IPC
 * with a Unix domain socket to eliminate the filesystem roundtrip.
 */

/**
 * Initialise the overlay writer.
 * - Creates the directory (e.g. /run/pos-overlay/) if it does not exist.
 * - Sets the clear timeout (seconds).
 * - Installs SIGALRM handler for auto-clear.
 *
 * Must be called once before overlay_write().
 */
void overlay_init(const char *file_path, int clear_secs);

/**
 * Write receipt text to the overlay file.
 *
 * Uses atomic rename: writes to a temp file, then renames to the
 * target path. This prevents P2 (which reads via inotify) from
 * reading a partially-written file.
 *
 * Also resets the auto-clear alarm.
 *
 * text     — NUL-terminated string (may contain '\n' for multiple lines)
 * text_len — length of text (pass 0 to use strlen)
 *
 * Returns 0 on success, -1 on error.
 */
int overlay_write(const char *text, size_t text_len);

/**
 * Clear the overlay file (write empty content).
 * Called by SIGALRM handler and on clean shutdown.
 */
void overlay_clear(void);
