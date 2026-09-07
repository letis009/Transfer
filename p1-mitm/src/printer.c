#include "printer.h"

#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <errno.h>
#include <unistd.h>
#include <fcntl.h>
#include <limits.h>
#include <sys/stat.h>
#include <sys/types.h>
#include <sys/ioctl.h>
#include <sys/sysmacros.h>   /* major(), minor() */
#include <linux/lp.h>        /* LPGETSTATUS (0x060b) */

/*
 * usblp IEEE-1284 device-ID ioctl.
 *
 * This is defined inside the kernel's drivers/usb/class/usblp.c but is NOT
 * exposed by any stable uapi header, so we reproduce it here (same pattern
 * the main loop uses for <linux/usb/g_printer.h>). The buffer returned starts
 * with a 2-byte big-endian length (including those 2 bytes) followed by the
 * IEEE-1284 device ID string.
 */
#define IOCNR_GET_DEVICE_ID       1
#define LPIOC_GET_DEVICE_ID(len)  _IOC(_IOC_READ, 'P', IOCNR_GET_DEVICE_ID, len)

/* --- sysfs helpers --- */

/**
 * Read a small sysfs attribute file into out (NUL-terminated, trailing
 * whitespace trimmed). Returns 0 on success, -1 on failure (out set to "").
 */
static int read_sysfs_str(const char *dir, const char *name,
                          char *out, size_t out_len)
{
    out[0] = '\0';
    char path[PATH_MAX + 64];
    snprintf(path, sizeof(path), "%s/%s", dir, name);

    int fd = open(path, O_RDONLY);
    if (fd < 0) return -1;

    ssize_t n = read(fd, out, out_len - 1);
    close(fd);
    if (n <= 0) { out[0] = '\0'; return -1; }

    out[n] = '\0';
    while (n > 0 && (out[n-1] == '\n' || out[n-1] == '\r' ||
                     out[n-1] == ' '  || out[n-1] == '\t'))
        out[--n] = '\0';
    return 0;
}

/**
 * Format a raw sysfs hex id ("04b8") as the "0x04B8" form configfs/OPOS use.
 */
static void format_hex_id(const char *raw, char *out, size_t out_len)
{
    char up[5] = {0};
    size_t j = 0;
    for (size_t i = 0; raw[i] && j < sizeof(up) - 1; i++) {
        char c = raw[i];
        if (c >= 'a' && c <= 'f') c = (char)(c - 'a' + 'A');
        up[j++] = c;
    }
    snprintf(out, out_len, "0x%s", up);
}

/**
 * Resolve the sysfs directory of the USB *device* backing a character device
 * with the given rdev (major:minor).
 *
 * /sys/dev/char/<major>:<minor> is a symlink to the device's sysfs node
 * (typically the usblp interface, e.g. .../1-1/1-1:1.0/usbmisc/lp0). We walk
 * up the parent chain until we reach the USB device node — the first ancestor
 * that has an `idVendor` attribute.
 *
 * Returns 0 and writes out_dir on success, -1 otherwise.
 */
static int resolve_usb_device_dir(dev_t rdev, char *out_dir, size_t out_len)
{
    char link[64];
    snprintf(link, sizeof(link), "/sys/dev/char/%u:%u",
             major(rdev), minor(rdev));

    char cur[PATH_MAX];
    if (!realpath(link, cur)) return -1;

    for (int depth = 0; depth < 12; depth++) {
        char probe[PATH_MAX + 16];
        snprintf(probe, sizeof(probe), "%s/idVendor", cur);
        if (access(probe, R_OK) == 0) {
            snprintf(out_dir, out_len, "%s", cur);
            return 0;
        }
        /* Climb one level up. */
        char *slash = strrchr(cur, '/');
        if (!slash || slash == cur) break;
        *slash = '\0';
        if (strcmp(cur, "/sys") == 0 || strcmp(cur, "/sys/devices") == 0)
            break;
    }
    return -1;
}

/**
 * Fetch the IEEE-1284 device-ID string from the real printer and store it,
 * stripped of the 2-byte length header, into out_pnp.
 */
static int probe_device_id(int fd, char *out_pnp, size_t out_len)
{
    char buf[1024];
    memset(buf, 0, sizeof(buf));

    if (ioctl(fd, LPIOC_GET_DEVICE_ID(sizeof(buf)), buf) < 0)
        return -1;

    unsigned len = ((unsigned)(unsigned char)buf[0] << 8) |
                    (unsigned)(unsigned char)buf[1];
    if (len < 2) return -1;
    if (len > sizeof(buf)) len = sizeof(buf);

    unsigned data_len = len - 2;
    if (data_len >= out_len) data_len = (unsigned)out_len - 1;
    memcpy(out_pnp, buf + 2, data_len);
    out_pnp[data_len] = '\0';
    return 0;
}

/* --- Public API --- */

int printer_probe_identity(int fd, const char *dev_path, PrinterIdentity *out)
{
    memset(out, 0, sizeof(*out));
    if (fd < 0) return -1;

    int have_ids = 0;

    struct stat st;
    if (fstat(fd, &st) == 0 && S_ISCHR(st.st_mode)) {
        char devdir[PATH_MAX];
        if (resolve_usb_device_dir(st.st_rdev, devdir, sizeof(devdir)) == 0) {
            char raw[64];
            if (read_sysfs_str(devdir, "idVendor", raw, sizeof(raw)) == 0 && raw[0]) {
                format_hex_id(raw, out->vid, sizeof(out->vid));
                have_ids++;
            }
            if (read_sysfs_str(devdir, "idProduct", raw, sizeof(raw)) == 0 && raw[0]) {
                format_hex_id(raw, out->pid, sizeof(out->pid));
                have_ids++;
            }
            read_sysfs_str(devdir, "serial",       out->serial,       sizeof(out->serial));
            read_sysfs_str(devdir, "manufacturer", out->manufacturer, sizeof(out->manufacturer));
            read_sysfs_str(devdir, "product",      out->product,      sizeof(out->product));
        } else {
            fprintf(stderr, "[printer] could not resolve sysfs node for %s\n",
                    dev_path ? dev_path : "(printer)");
        }
    }

    /* IEEE-1284 device-ID → PNP string (independent of the sysfs read above) */
    if (probe_device_id(fd, out->pnp, sizeof(out->pnp)) != 0)
        out->pnp[0] = '\0';

    out->valid = (have_ids >= 2);   /* need both VID and PID to trust the clone */
    return out->valid ? 0 : -1;
}

int printer_get_port_status(int fd, unsigned char *status)
{
    if (fd < 0) return -1;

    int st = 0;
    if (ioctl(fd, LPGETSTATUS, &st) < 0)
        return -1;

    *status = (unsigned char)st;
    return 0;
}
