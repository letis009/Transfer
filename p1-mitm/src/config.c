#include "config.h"

#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <errno.h>

#define MAX_LINE 1024

static char *strdup_trim(const char *s)
{
    while (*s == ' ' || *s == '\t') s++;
    size_t len = strlen(s);
    while (len > 0 && (s[len-1] == '\n' || s[len-1] == '\r' ||
                       s[len-1] == ' '  || s[len-1] == '\t'))
        len--;
    char *out = malloc(len + 1);
    if (!out) return NULL;
    memcpy(out, s, len);
    out[len] = '\0';
    return out;
}

Config *config_load(const char *path)
{
    Config *cfg = calloc(1, sizeof(Config));
    if (!cfg) return NULL;

    /*
     * Defaults. These are also the TM-T88VII fallback identity used when
     * auto-clone cannot read the real printer, AND the complete configuration
     * when no config file is present — so the device runs with zero field
     * configuration.
     */
    cfg->gadget_vid          = strdup("0x04B8");
    cfg->gadget_pid          = strdup("0x0E28");
    cfg->gadget_manufacturer = strdup("SEIKO EPSON CORPORATION");
    cfg->gadget_product      = strdup("TM-T88VII");
    cfg->gadget_serial       = strdup("A1B2C3D4E5F6");
    cfg->gadget_pnp          = strdup("MFG:EPSON;CMD:ESCPOS;MDL:TM-T88VII;"
                                      "CLS:PRINTER;DES:EPSON TM-T88VII;");
    cfg->gadget_dev          = strdup("/dev/g_printer0");
    cfg->printer_dev         = strdup("/dev/usb/lp0");
    cfg->overlay_file        = strdup("/run/pos-overlay/overlay.txt");
    cfg->overlay_clear_secs  = 30;

    /*
     * The config file is optional. If it is absent (or unreadable), keep the
     * built-in defaults and carry on — this is the normal field case where the
     * printer identity is auto-cloned and nothing needs configuring.
     */
    FILE *f = fopen(path, "r");
    if (!f) {
        fprintf(stderr, "[config] %s not read (%s) — using built-in defaults\n",
                path, strerror(errno));
        return cfg;
    }

    char line[MAX_LINE];
    while (fgets(line, sizeof(line), f)) {
        if (line[0] == '#' || line[0] == '\n' || line[0] == '\r') continue;
        char *eq = strchr(line, '=');
        if (!eq) continue;
        *eq = '\0';
        const char *key = line;
        const char *val = eq + 1;

        #define SET(field, key_name) \
            if (strcmp(key, key_name) == 0) { \
                free(cfg->field); cfg->field = strdup_trim(val); continue; }

        SET(gadget_vid,          "GADGET_VID")
        SET(gadget_pid,          "GADGET_PID")
        SET(gadget_manufacturer, "GADGET_MANUFACTURER")
        SET(gadget_product,      "GADGET_PRODUCT")
        SET(gadget_serial,       "GADGET_SERIAL")
        SET(gadget_pnp,          "GADGET_PNP")
        SET(gadget_dev,          "GADGET_DEV")
        SET(printer_dev,         "PRINTER_DEV")
        SET(overlay_file,        "OVERLAY_FILE")

        #undef SET

        if (strcmp(key, "OVERLAY_CLEAR_SECS") == 0) {
            char *v = strdup_trim(val);
            cfg->overlay_clear_secs = v ? atoi(v) : 30;
            free(v);
        }
    }
    fclose(f);
    return cfg;
}

void config_free(Config *cfg)
{
    if (!cfg) return;
    free(cfg->gadget_vid);
    free(cfg->gadget_pid);
    free(cfg->gadget_manufacturer);
    free(cfg->gadget_product);
    free(cfg->gadget_serial);
    free(cfg->gadget_pnp);
    free(cfg->gadget_dev);
    free(cfg->printer_dev);
    free(cfg->overlay_file);
    free(cfg);
}

void config_print(const Config *cfg)
{
    printf("[config] VID             : %s\n", cfg->gadget_vid);
    printf("[config] PID             : %s\n", cfg->gadget_pid);
    printf("[config] Manufacturer    : %s\n", cfg->gadget_manufacturer);
    printf("[config] Product         : %s\n", cfg->gadget_product);
    printf("[config] Serial          : %s\n", cfg->gadget_serial);
    printf("[config] PNP string      : %s\n", cfg->gadget_pnp);
    printf("[config] Gadget device   : %s\n", cfg->gadget_dev);
    printf("[config] Printer device  : %s\n", cfg->printer_dev);
    printf("[config] Overlay file    : %s\n", cfg->overlay_file);
    printf("[config] Overlay clear   : %ds\n", cfg->overlay_clear_secs);
}
