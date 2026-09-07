/**
 * p1-mitm — USB Printer Man-in-the-Middle
 *
 * Sits between a POS system and a real Epson TM-T88 receipt printer. On start
 * it clones the real printer's USB identity onto the gadget (zero field config,
 * TM-T88 fallback if no printer is readable), presents itself as that printer
 * to the POS, forwards all raw bytes to the real printer unchanged, relays the
 * real printer's live status back to the POS, and parses the ESC/POS stream to
 * extract receipt text for P2-overlay.
 *
 * Flow:
 *   POS (Windows) → USB-C OTG → /dev/g_printer0 → [this program]
 *      host→printer:  forward all bytes → /dev/usb/lp0 → Real printer
 *      printer→host:  relay real status (DLE EOT reply / ASB) → /dev/g_printer0
 *      status mirror: LPGETSTATUS(real) → GADGET_SET_PRINTER_STATUS(gadget)
 *      overlay:       parse ESC/POS → /run/pos-overlay/overlay.txt
 *
 * Usage:
 *   p1-mitm [/path/to/config]   (config optional; default /etc/p1-mitm.conf)
 *
 * Requires: root (for gadget configfs + raw device access)
 */

#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <errno.h>
#include <unistd.h>
#include <fcntl.h>
#include <signal.h>
#include <sys/ioctl.h>
#include <sys/select.h>
#include <sys/stat.h>
#include <time.h>

/*
 * usb_f_printer gadget status ioctl.
 * GET_PORT_STATUS (USB printer class control request) returns this byte.
 *   Bit 3 (0x08): PRINTER_NO_ERROR  — 1 = no error
 *   Bit 4 (0x10): PRINTER_SELECTED  — 1 = printer is online/selected
 * 0x18 = online, no error, paper present.
 * Windows and OPOS both read GET_PORT_STATUS on connect; 0x00 = offline/error
 * causes Windows to loop ESC @ (printer init) indefinitely.
 *
 * Use the kernel uapi header so the ioctl number is computed by the same
 * macros the kernel used — avoids ENOTTY from a manually-computed mismatch.
 */
#include <linux/usb/g_printer.h>
#define PRINTER_STATUS_ONLINE  0x18  /* PRINTER_SELECTED | PRINTER_NO_ERROR */

#include "config.h"
#include "gadget.h"
#include "escpos.h"
#include "overlay.h"
#include "printer.h"

#define DEFAULT_CONFIG   "/etc/p1-mitm.conf"
#define READ_BUF_SIZE    4096
#define RECEIPT_BUF_SIZE 8192

/* Seconds to wait at startup for the real printer to enumerate (it is plugged
 * in before the POS, per the field workflow) so its identity can be cloned. */
#define PRINTER_OPEN_SECS 5
/* How often to poll the real printer's class status and mirror it to the gadget. */
#define STATUS_POLL_SECS  2

/* DLE EOT status bytes */
#define DLE             0x10
#define EOT             0x04
/*
 * Real-time status response byte (DLE EOT n reply) — Epson TM-T88VII spec.
 * Bit 7: 0 (fixed)  Bit 6: 0 (paper-feed button released)
 * Bit 5: 1 (fixed)  Bit 4: 0 (print position normal / no error)
 * Bit 3: 0 (drawer pin 3 low)  Bit 2: 0 (fixed)
 * Bit 1: 1 (fixed)  Bit 0: 0 (fixed)
 * = 0b00100010 = 0x22  "Printer online, paper OK, no faults"
 *
 * NOTE: 0x12 (old value) had bit 4 set which signals "print position
 * abnormal" — OPOS interprets this as an error and rejects the printer.
 */
#define STATUS_ONLINE   0x22

/* --- Global state --- */

static volatile int g_running = 1;
static int          g_gadget_fd  = -1;
static int          g_printer_fd = -1;
/* 1 if g_printer_fd was opened O_RDWR and its bulk-IN can be read (status). */
static int          g_printer_can_read = 0;
/* Last USB printer-class status byte pushed to the gadget (change detection). */
static unsigned char g_gadget_status = PRINTER_STATUS_ONLINE;

/* --- Signal handling --- */

static void on_signal(int sig)
{
    (void)sig;
    g_running = 0;
}

/* --- DLE EOT detection and response --- */

/**
 * Scan a buffer for DLE EOT n (0x10 0x04 n) real-time status requests.
 *
 * DLE EOT is a 3-byte command: DLE(0x10) EOT(0x04) n
 *   n=1: Printer status    n=2: Offline status
 *   n=3: Error status      n=4: Paper sensor status
 *
 * We respond with STATUS_ONLINE (0x22) for all status types — this byte
 * signals: printer selected, online, no errors, paper present, no faults.
 *
 * Response MUST happen within ~200ms or OPOS marks the printer offline.
 * Done BEFORE ESC/POS parsing to minimise latency.
 *
 * Returns the number of DLE EOT sequences found and responded to.
 */
static int handle_dle_eot(const uint8_t *buf, size_t len, int gadget_fd)
{
    int count = 0;
    for (size_t i = 0; i + 2 < len; i++) {   /* need 3 bytes: DLE EOT n */
        if (buf[i] == DLE && buf[i+1] == EOT) {
            uint8_t n      = buf[i+2];        /* status type: 1..4       */
            uint8_t status = STATUS_ONLINE;
            ssize_t w = write(gadget_fd, &status, 1);
            if (w < 0) {
                if (errno != EPIPE)
                    perror("[main] DLE EOT response write");
            } else {
                printf("[main] DLE EOT n=%u → 0x%02X\n", n, status);
            }
            count++;
            i += 2;  /* Skip EOT and n — next iteration adds 1 more */
        }
    }
    return count;
}

/* --- Gadget status helpers --- */

/**
 * Set the usb_f_printer status byte via ioctl.
 *
 * This byte is returned to the USB host on every GET_PORT_STATUS class
 * control request (bRequest=0x01). Windows usbprint.sys and OPOS both
 * read this before accepting the printer as online.
 *
 * Must be called immediately after opening /dev/g_printer0. If not set,
 * the kernel defaults to 0x00 which Windows interprets as "printer error",
 * causing it to loop ESC @ (printer init) endlessly.
 */
static void set_gadget_printer_status(int fd, unsigned char status)
{
    printf("[main] GADGET_SET_PRINTER_STATUS ioctl=0x%08lX value=0x%02X\n",
           (unsigned long)GADGET_SET_PRINTER_STATUS, status);
    if (ioctl(fd, GADGET_SET_PRINTER_STATUS, (unsigned long)status) < 0) {
        fprintf(stderr, "[main] GADGET_SET_PRINTER_STATUS failed: %s\n",
                strerror(errno));
        /*
         * Fallback: read back current status to confirm what the kernel holds.
         * If GET also fails, the ioctl interface is not available on this kernel.
         */
        int cur = ioctl(fd, GADGET_GET_PRINTER_STATUS);
        if (cur < 0)
            fprintf(stderr, "[main] GADGET_GET_PRINTER_STATUS also failed: %s"
                    " — usb_f_printer ioctl not supported on this kernel\n",
                    strerror(errno));
        else
            printf("[main] current printer status from kernel: 0x%02X\n",
                   (unsigned char)cur);
    } else {
        printf("[main] printer status set to 0x%02X (online)\n", status);
    }
}

/* --- Main read loop --- */

/**
 * Open the gadget device with retry.
 *
 * /dev/g_printer0 may not exist immediately after gadget_setup()
 * because the USB host (POS system) must enumerate the device first.
 * This can take 2–5 seconds after the gadget is bound.
 *
 * Returns fd on success, -1 after max_retries.
 *
 * Failure modes:
 *   - ENOENT: usb_f_printer module not loaded, or gadget not bound
 *   - EACCES: not running as root
 *   - EBUSY:  device already opened by another process
 */
static int open_gadget_with_retry(const char *path, int max_retries)
{
    for (int i = 0; i < max_retries; i++) {
        int fd = open(path, O_RDWR);
        if (fd >= 0) {
            printf("[main] opened gadget device: %s (attempt %d)\n", path, i+1);
            set_gadget_printer_status(fd, PRINTER_STATUS_ONLINE);
            return fd;
        }
        if (errno == ENOENT || errno == ENODEV) {
            /*
             * ENOENT: usb_f_printer not yet created functions/printer.0
             * ENODEV: device node exists but kernel char device not yet
             *         registered — happens in the ~100ms window after
             *         gadget_setup() recreates functions/printer.0
             * Both are transient. Retry silently.
             */
            printf("[main] waiting for %s (attempt %d/%d)...\n",
                   path, i+1, max_retries);
        } else {
            fprintf(stderr, "[main] open(%s): %s\n", path, strerror(errno));
            if (errno == EACCES) return -1;  /* No point retrying permission errors */
        }
        sleep(1);
    }
    fprintf(stderr, "[main] %s not available after %d attempts\n",
            path, max_retries);
    return -1;
}

/**
 * Open the real printer device.
 *
 * /dev/usb/lp0 is the USB line printer device created by the usblp
 * kernel module when the real TM-T88VII is connected to a USB-A port.
 *
 * Passthrough is optional — if the real printer is not connected,
 * we continue parsing (for development) but do not forward bytes.
 *
 * Failure modes:
 *   - ENOENT: real printer not plugged in, or usblp module not loaded
 *   - EBUSY:  another process has the printer open (e.g. CUPS)
 *     → run: sudo systemctl disable cups && sudo systemctl stop cups
 */
/*
 * Open the real printer, preferring O_RDWR so its bulk-IN endpoint can be read
 * for status replies (DLE EOT / Automatic Status Back). If the device rejects
 * O_RDWR (unidirectional printer), fall back to O_WRONLY — printing still works,
 * but status can only come from the LPGETSTATUS poll, not the real-time relay.
 *
 * *can_read is set to 1 for O_RDWR, 0 for O_WRONLY.
 */
static int open_printer(const char *path, int *can_read)
{
    int fd = open(path, O_RDWR);
    if (fd >= 0) { *can_read = 1; return fd; }

    fd = open(path, O_WRONLY);
    if (fd >= 0) { *can_read = 0; return fd; }

    return -1;
}

/*
 * Open the real printer with a short retry window. Per the field workflow the
 * printer is attached before the POS, so it should already be enumerating; give
 * it a few seconds. Returns fd (and sets *can_read) or -1 if never available.
 */
static int open_printer_retry(const char *path, int secs, int *can_read)
{
    for (int i = 0; i < secs; i++) {
        int fd = open_printer(path, can_read);
        if (fd >= 0) {
            printf("[main] real printer open: %s (%s)\n",
                   path, *can_read ? "R/W — status relay enabled"
                                   : "write-only — status relay disabled");
            return fd;
        }
        if (i == 0)
            printf("[main] waiting for real printer at %s (%s)...\n",
                   path, strerror(errno));
        sleep(1);
    }
    fprintf(stderr, "[main] real printer %s not available — passthrough "
            "disabled, using TM-T88 fallback identity\n", path);
    *can_read = 0;
    return -1;
}

/*
 * Clone the real printer's USB identity onto the config so the gadget presents
 * exactly the attached printer to the POS (zero field configuration). Every
 * field that reads back non-empty overrides the built-in default; anything that
 * cannot be read keeps its TM-T88VII default.
 */
static void clone_identity_from_printer(Config *cfg, int printer_fd)
{
    PrinterIdentity id;
    if (printer_probe_identity(printer_fd, cfg->printer_dev, &id) != 0) {
        printf("[main] real printer identity not readable — FELL BACK to "
               "TM-T88 defaults: vid=%s pid=%s\n",
               cfg->gadget_vid, cfg->gadget_pid);
        return;
    }

    #define CLONE(field, src) \
        if ((src)[0]) { free(cfg->field); cfg->field = strdup(src); }
    CLONE(gadget_vid,          id.vid);
    CLONE(gadget_pid,          id.pid);
    CLONE(gadget_serial,       id.serial);
    CLONE(gadget_manufacturer, id.manufacturer);
    CLONE(gadget_product,      id.product);
    CLONE(gadget_pnp,          id.pnp);
    #undef CLONE

    printf("[main] identity CLONED from real printer: vid=%s pid=%s serial=%s\n",
           cfg->gadget_vid, cfg->gadget_pid, cfg->gadget_serial);
    if (id.pnp[0])
        printf("[main] cloned PNP: %s\n", cfg->gadget_pnp);
}

/*
 * Poll the real printer's USB printer-class status and, on change, mirror it to
 * the gadget so Windows/OPOS enumeration reads reflect real paper/error state.
 * No-op when there is no real printer to read from.
 */
static void poll_and_mirror_status(void)
{
    if (g_printer_fd < 0) return;

    unsigned char real;
    if (printer_get_port_status(g_printer_fd, &real) != 0) return;

    if (real != g_gadget_status) {
        set_gadget_printer_status(g_gadget_fd, real);
        g_gadget_status = real;
        printf("[main] real status changed → 0x%02X "
               "(paper_empty=%d selected=%d no_error=%d)\n",
               real, !!(real & 0x20), !!(real & 0x10), !!(real & 0x08));
    }
}

/*
 * Relay bytes the real printer sends on its bulk-IN endpoint (DLE EOT real-time
 * status replies and unsolicited Automatic Status Back on paper-out/cover-open)
 * straight to the gadget, so the POS receives the real printer's actual status.
 * Called only when the printer fd is readable. Handles disconnect.
 */
static void relay_printer_status(void)
{
    uint8_t sbuf[256];
    ssize_t sn = read(g_printer_fd, sbuf, sizeof(sbuf));

    if (sn > 0) {
        ssize_t wtot = 0;
        while (wtot < sn) {
            ssize_t w = write(g_gadget_fd, sbuf + wtot, (size_t)(sn - wtot));
            if (w < 0) {
                if (errno == EINTR) continue;
                if (errno != EPIPE)
                    perror("[main] relay status → gadget");
                break;
            }
            wtot += w;
        }
        printf("[main] relayed %zd status byte(s) printer→POS:", sn);
        for (ssize_t i = 0; i < sn && i < 8; i++)
            printf(" %02X", (unsigned char)sbuf[i]);
        printf("\n");
    } else if (sn < 0) {
        if (errno == EINTR || errno == EAGAIN) return;
        if (errno == EPIPE || errno == EIO || errno == ENODEV) {
            printf("[main] real printer disconnected (status read) — "
                   "falling back to local online status\n");
            close(g_printer_fd);
            g_printer_fd = -1;
            g_printer_can_read = 0;
            /* Keep the POS online so it does not drop the port; the local DLE
             * EOT responder now answers. */
            set_gadget_printer_status(g_gadget_fd, PRINTER_STATUS_ONLINE);
            g_gadget_status = PRINTER_STATUS_ONLINE;
        } else {
            perror("[main] read printer status");
        }
    }
    /* sn == 0: no data this cycle — ignore */
}

/* --- Entry point --- */

int main(int argc, char *argv[])
{
    const char *config_path = (argc > 1) ? argv[1] : DEFAULT_CONFIG;

    printf("[main] p1-mitm starting\n");
    printf("[main] config: %s\n", config_path);

    /* Load config */
    Config *cfg = config_load(config_path);
    if (!cfg) {
        fprintf(stderr, "[main] failed to load config\n");
        return EXIT_FAILURE;
    }

    /* Signal handlers */
    signal(SIGINT,  on_signal);
    signal(SIGTERM, on_signal);
    signal(SIGPIPE, SIG_IGN);  /* Ignore broken pipe — handle via write() return */

    /* Load kernel modules */
    if (gadget_load_modules() != 0) {
        fprintf(stderr, "[main] module load failed — continuing anyway\n");
        /* Non-fatal: modules may already be built into the kernel */
    }

    /*
     * Open the real printer FIRST. In the field the printer is plugged into the
     * Pi before the Pi is plugged into the POS, so it should already be present.
     * Reading it now lets us clone its exact USB identity onto the gadget (so
     * OPOS sees a compatible printer with zero field configuration) and read its
     * live status. If no printer is found we keep the TM-T88 fallback identity.
     */
    g_printer_fd = open_printer_retry(cfg->printer_dev, PRINTER_OPEN_SECS,
                                      &g_printer_can_read);
    clone_identity_from_printer(cfg, g_printer_fd);

    /* Show the effective (post-clone) configuration. */
    config_print(cfg);

    /* Set up USB gadget (impersonate the real printer) */
    if (gadget_setup(cfg) != 0) {
        fprintf(stderr, "[main] gadget setup failed\n");
        if (g_printer_fd >= 0) close(g_printer_fd);
        config_free(cfg);
        return EXIT_FAILURE;
    }

    /* Initialise overlay writer */
    overlay_init(cfg->overlay_file, cfg->overlay_clear_secs);

    /* Open gadget device (waits for USB host enumeration) */
    g_gadget_fd = open_gadget_with_retry(cfg->gadget_dev, 30);
    if (g_gadget_fd < 0) {
        fprintf(stderr, "[main] cannot open gadget device — aborting\n");
        gadget_teardown();
        config_free(cfg);
        return EXIT_FAILURE;
    }

    /* Initialise ESC/POS parser */
    EscPosParser parser;
    escpos_init(&parser);

    /* Read buffers */
    uint8_t read_buf[READ_BUF_SIZE];
    char    receipt_buf[RECEIPT_BUF_SIZE];
    time_t  last_status_poll = 0;

    printf("[main] entering read loop — waiting for print jobs\n");

    while (g_running) {
        /*
         * Periodically read the real printer's class status and mirror it to
         * the gadget (paper-out / cover-open / error → POS). Cheap; throttled.
         */
        time_t now = time(NULL);
        if (now - last_status_poll >= STATUS_POLL_SECS) {
            poll_and_mirror_status();
            last_status_poll = now;
        }

        /*
         * select() on the gadget (host → printer data) and, when available, the
         * real printer (printer → host status). 1-second timeout so we re-check
         * g_running and re-poll status without burning CPU.
         */
        fd_set rfds;
        FD_ZERO(&rfds);
        FD_SET(g_gadget_fd, &rfds);
        int maxfd = g_gadget_fd;
        if (g_printer_fd >= 0 && g_printer_can_read) {
            FD_SET(g_printer_fd, &rfds);
            if (g_printer_fd > maxfd) maxfd = g_printer_fd;
        }

        struct timeval tv = { .tv_sec = 1, .tv_usec = 0 };
        int sel = select(maxfd + 1, &rfds, NULL, NULL, &tv);

        if (sel < 0) {
            if (errno == EINTR) continue;  /* Signal received — check g_running */
            perror("[main] select");
            break;
        }
        if (sel == 0) continue;  /* Timeout — loop and check g_running */

        /* Real printer → POS: relay any status the real printer emits. */
        if (g_printer_fd >= 0 && g_printer_can_read &&
            FD_ISSET(g_printer_fd, &rfds)) {
            relay_printer_status();
        }

        if (!FD_ISSET(g_gadget_fd, &rfds))
            continue;  /* only the printer fd was ready this cycle */

        /* Data available on gadget device */
        ssize_t n = read(g_gadget_fd, read_buf, sizeof(read_buf));

        if (n < 0) {
            if (errno == EINTR) continue;
            perror("[main] read gadget");
            break;
        }
        if (n == 0) {
            /*
             * EOF on /dev/g_printer0: the USB host closed its bulk-OUT endpoint
             * (end of status-poll cycle or brief disconnect).
             *
             * DO NOT close and reopen the fd. Keeping it open is critical:
             *   1. Printer status (GET_PORT_STATUS) stays at 0x18 (online)
             *      while the fd is held. Closing resets it to 0x00 and Windows
             *      immediately sees the printer go offline.
             *   2. The usb_f_printer read() will block (via wait_event) when the
             *      host next sends data — no busy-loop, no polling needed.
             *   3. No rapid open/close cycle that races with Windows status polls.
             *
             * A brief sleep prevents a tight loop if read() returns 0 repeatedly
             * due to a kernel edge case (e.g. gadget in reset state).
             */
            usleep(50000);  /* 50ms yield — keeps CPU idle between poll cycles */
            continue;
        }

        /* Log raw bytes (up to 16) so we can see exactly what the host sends */
        printf("[main] rx %zd:", n);
        for (ssize_t i = 0; i < n && i < 16; i++)
            printf(" %02X", (unsigned char)read_buf[i]);
        if (n > 16) printf(" ...(+%zd)", n - 16);
        printf("\n");

        /*
         * STEP 1: DLE EOT status requests — FALLBACK ONLY.
         *
         * When a real printer is attached, we do NOT answer here: the request
         * is passed through to the real printer (STEP 2) and its genuine
         * real-time reply is relayed back to the POS by relay_printer_status().
         * Answering locally too would send the host a duplicate status byte.
         *
         * Only when there is no real printer do we answer locally (with a
         * synthetic "online") so OPOS does not drop the port during a printer
         * unplug or bench testing.
         */
        if (g_printer_fd < 0)
            handle_dle_eot(read_buf, (size_t)n, g_gadget_fd);

        /*
         * STEP 2: Forward all bytes to the real printer unchanged.
         * This is a transparent passthrough — the real printer receives
         * exactly what the POS system sent, including all formatting
         * commands. We are completely invisible to both ends.
         */
        if (g_printer_fd >= 0) {
            ssize_t written = 0;
            while (written < n) {
                ssize_t w = write(g_printer_fd, read_buf + written,
                                  (size_t)(n - written));
                if (w < 0) {
                    if (errno == EPIPE || errno == EIO) {
                        printf("[main] real printer disconnected (write)\n");
                        close(g_printer_fd);
                        g_printer_fd = -1;
                        g_printer_can_read = 0;
                        break;
                    }
                    perror("[main] write printer");
                    break;
                }
                written += w;
            }
        }

        /*
         * STEP 3: Feed bytes to the ESC/POS parser.
         * The parser extracts printable text, skips control sequences,
         * and signals when a paper cut (GS V) is detected.
         */
        size_t consumed = 0;
        size_t offset   = 0;

        while (offset < (size_t)n) {
            int ret = escpos_feed_buf(&parser,
                                     read_buf + offset,
                                     (size_t)n - offset,
                                     &consumed);
            offset += consumed;

            if (ret == ESCPOS_RECEIPT_CUT) {
                /*
                 * Paper cut detected = end of receipt.
                 * Build overlay text from accumulated lines and write
                 * to the overlay file. P2-overlay will detect the file
                 * change via inotify and update the video stream text.
                 */
                size_t len = escpos_get_receipt(&parser, receipt_buf,
                                                sizeof(receipt_buf));
                if (len > 0) {
                    printf("[main] receipt complete (%zu bytes, %zu lines)\n",
                           len, parser.line_count);
                    overlay_write(receipt_buf, len);
                } else {
                    printf("[main] empty receipt (cut with no text)\n");
                }
                escpos_reset_receipt(&parser);
            }
        }
    }

    /* Clean shutdown */
    printf("[main] shutting down\n");
    overlay_clear();

    if (g_gadget_fd >= 0)  close(g_gadget_fd);
    if (g_printer_fd >= 0) close(g_printer_fd);

    gadget_teardown();
    config_free(cfg);

    return EXIT_SUCCESS;
}
