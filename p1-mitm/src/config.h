#pragma once

/**
 * config.h — P1-MITM runtime configuration
 *
 * Reads /etc/p1-mitm.conf (KEY=VALUE format).
 * All string fields are heap-allocated; free via config_free().
 *
 * Configuration file: /etc/p1-mitm.conf
 */

typedef struct {
    /* USB gadget identity — must match the real Epson TM-T88VII exactly */
    char *gadget_vid;          /* Vendor ID  hex string, e.g. "0x04B8"   */
    char *gadget_pid;          /* Product ID hex string, e.g. "0x0E28"   */
    char *gadget_manufacturer; /* e.g. "SEIKO EPSON CORPORATION"          */
    char *gadget_product;      /* e.g. "TM-T88VII"                        */
    char *gadget_serial;       /* Serial number — copy from real printer  */
    char *gadget_pnp;          /* IEEE 1284 PNP string — OPOS critical    */

    /* Device paths */
    char *gadget_dev;          /* Gadget printer device, /dev/g_printer0  */
    char *printer_dev;         /* Real printer USB device, /dev/usb/lp0   */

    /* IPC with P2 overlay */
    char *overlay_file;        /* File P2 watches for receipt text        */
    int   overlay_clear_secs;  /* Seconds before clearing overlay text    */
} Config;

/**
 * Load config from file at path.
 * Returns NULL on failure.
 */
Config *config_load(const char *path);

void config_free(Config *cfg);

/** Print all config values to stdout for diagnostic purposes. */
void config_print(const Config *cfg);
