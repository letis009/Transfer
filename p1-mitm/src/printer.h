#pragma once

/**
 * printer.h — real (physical) printer helpers
 *
 * Everything that talks to the real Epson TM-T88 attached to the Pi's USB-A
 * host port (exposed by the kernel `usblp` driver as /dev/usb/lp0):
 *
 *   1. Identity probe — read the real printer's USB descriptors
 *      (VID/PID/serial/manufacturer/product) from sysfs and its IEEE-1284
 *      device-ID string via the usblp ioctl. p1-mitm clones these onto the
 *      USB gadget so the POS sees the exact printer that is attached, with
 *      no field configuration. If a field cannot be read, the caller keeps
 *      its built-in TM-T88 default for that field.
 *
 *   2. Port status — read the real printer's USB printer-class GET_PORT_STATUS
 *      byte (paper-empty / selected / no-error) so it can be mirrored onto the
 *      gadget, replicating the real printer's status upstream to the POS.
 *
 * This module has no dependency on the gadget or overlay code and uses only
 * libc + Linux uapi headers.
 */

/* Cloned identity of the real printer. Fields are empty strings when the
 * corresponding descriptor could not be read. `valid` is non-zero only when
 * at least the VID and PID were obtained. */
typedef struct {
    char vid[8];            /* "0x04B8"  (configfs / OPOS format)          */
    char pid[8];            /* "0x0E28"                                    */
    char serial[128];       /* iSerialNumber string                        */
    char manufacturer[128]; /* iManufacturer string                        */
    char product[128];      /* iProduct string                             */
    char pnp[512];          /* IEEE-1284 device-ID (no 2-byte length head) */
    int  valid;             /* 1 = VID+PID read successfully               */
} PrinterIdentity;

/**
 * Probe the identity of the printer behind the already-open file descriptor
 * `fd` (its device node path `dev_path` is used only for diagnostics).
 *
 * Best-effort: every field that can be read is filled; the rest stay empty.
 * Returns 0 when the identity is `valid` (VID+PID read), -1 otherwise.
 * `out` is always zero-initialised first, so the caller can safely inspect it
 * even on failure.
 */
int printer_probe_identity(int fd, const char *dev_path, PrinterIdentity *out);

/**
 * Read the real printer's USB printer-class GET_PORT_STATUS byte via the
 * usblp LPGETSTATUS ioctl on `fd`.
 *
 * Status bits (USB Printer Class 1.1):
 *   bit 5 (0x20) Paper Empty   — 1 = out of paper
 *   bit 4 (0x10) Selected      — 1 = printer selected / online
 *   bit 3 (0x08) Not Error     — 1 = no error
 *
 * Returns 0 and writes *status on success; -1 on failure (errno set).
 */
int printer_get_port_status(int fd, unsigned char *status);
