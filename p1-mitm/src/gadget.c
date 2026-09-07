#include "gadget.h"

#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <errno.h>
#include <unistd.h>
#include <fcntl.h>
#include <dirent.h>
#include <sys/stat.h>
#include <sys/types.h>
#include <sys/mount.h>

#define CONFIGFS_BASE  "/sys/kernel/config"
#define GADGET_BASE    CONFIGFS_BASE "/usb_gadget/epson_printer"
#define GADGET_STRINGS GADGET_BASE  "/strings/0x409"
#define GADGET_FUNC    GADGET_BASE  "/functions/printer.0"
#define GADGET_CONF    GADGET_BASE  "/configs/c.1"
#define GADGET_CSTR    GADGET_CONF  "/strings/0x409"
#define GADGET_SYMLINK GADGET_CONF  "/printer.0"
#define GADGET_UDC     GADGET_BASE  "/UDC"

/* --- Internal helpers --- */

/**
 * Write a string to a sysfs/configfs file.
 * Returns 0 on success, -1 on error.
 *
 * Many configfs attributes reject writes with EINVAL if the format
 * is wrong (e.g. hex strings must be lowercase for some attributes).
 */
static int write_file(const char *path, const char *value)
{
    int fd = open(path, O_WRONLY | O_CREAT | O_TRUNC, 0644);
    if (fd < 0) {
        fprintf(stderr, "[gadget] open(%s): %s\n", path, strerror(errno));
        return -1;
    }
    ssize_t len = (ssize_t)strlen(value);
    ssize_t written = write(fd, value, len);
    close(fd);
    if (written != len) {
        fprintf(stderr, "[gadget] write(%s, \"%s\"): %s\n",
                path, value, strerror(errno));
        return -1;
    }
    return 0;
}

/**
 * Create a directory if it does not already exist.
 * EEXIST is not treated as an error.
 */
static int make_dir(const char *path)
{
    if (mkdir(path, 0755) != 0 && errno != EEXIST) {
        fprintf(stderr, "[gadget] mkdir(%s): %s\n", path, strerror(errno));
        return -1;
    }
    return 0;
}

/**
 * Remove a directory (must be empty).
 */
static void remove_dir(const char *path)
{
    if (rmdir(path) != 0 && errno != ENOENT)
        fprintf(stderr, "[gadget] rmdir(%s): %s\n", path, strerror(errno));
}

/**
 * Find the first UDC (USB Device Controller) available on this system.
 * On Pi 4B: "fe980000.usb" (DWC2 controller)
 * On CM4:   "20980000.usb" or similar
 *
 * Returns a heap-allocated string (caller must free), or NULL.
 *
 * Failure mode: returns NULL if /sys/class/udc/ is empty, which means
 * DWC2 is not loaded or not in peripheral mode. Check:
 *   lsmod | grep dwc2
 *   cat /boot/firmware/config.txt | grep dwc2
 */
static char *find_udc(void)
{
    DIR *d = opendir("/sys/class/udc");
    if (!d) {
        fprintf(stderr, "[gadget] cannot open /sys/class/udc: %s\n",
                strerror(errno));
        return NULL;
    }
    char *udc = NULL;
    struct dirent *entry;
    while ((entry = readdir(d)) != NULL) {
        if (entry->d_name[0] == '.') continue;
        udc = strdup(entry->d_name);
        break;
    }
    closedir(d);
    if (!udc)
        fprintf(stderr, "[gadget] no UDC found — is dwc2 loaded in "
                "peripheral mode?\n");
    return udc;
}

/* --- Public API --- */

int gadget_load_modules(void)
{
    int ret = 0;

    printf("[gadget] loading kernel modules\n");

    if (system("modprobe libcomposite 2>/dev/null") != 0) {
        fprintf(stderr, "[gadget] modprobe libcomposite failed\n");
        ret = -1;
    } else {
        printf("[gadget] libcomposite loaded\n");
    }

    /* usb_f_printer provides /dev/g_printer0 and sets USB Class 7/1/2 */
    if (system("modprobe usb_f_printer 2>/dev/null") != 0) {
        fprintf(stderr, "[gadget] modprobe usb_f_printer failed\n");
        ret = -1;
    } else {
        printf("[gadget] usb_f_printer loaded\n");
    }

    return ret;
}

int gadget_setup(const Config *cfg)
{
    printf("[gadget] setting up USB gadget (Epson TM-T88VII emulation)\n");

    /* Tear down any leftover gadget from a previous run before starting fresh.
     * If the gadget directory already exists with attributes bound, writes to
     * those attributes return EACCES. Always start from a clean slate. */
    gadget_teardown();

    /* Mount configfs if not already mounted */
    if (mount("none", CONFIGFS_BASE, "configfs", 0, NULL) != 0) {
        if (errno != EBUSY) {
            fprintf(stderr, "[gadget] mount configfs: %s\n", strerror(errno));
            /* Not fatal — it may already be mounted */
        }
    }

    /* Create gadget root */
    if (make_dir(GADGET_BASE) != 0) return -1;

    /* VID/PID — MUST match real printer exactly for OPOS/Windows recognition */
    if (write_file(GADGET_BASE "/idVendor",  cfg->gadget_vid)  != 0) return -1;
    if (write_file(GADGET_BASE "/idProduct", cfg->gadget_pid)  != 0) return -1;
    if (write_file(GADGET_BASE "/bcdDevice", "0x0100")         != 0) return -1;
    if (write_file(GADGET_BASE "/bcdUSB",    "0x0200")         != 0) return -1;

    /* English strings */
    if (make_dir(GADGET_STRINGS) != 0) return -1;
    if (write_file(GADGET_STRINGS "/manufacturer", cfg->gadget_manufacturer) != 0) return -1;
    if (write_file(GADGET_STRINGS "/product",      cfg->gadget_product)      != 0) return -1;
    if (write_file(GADGET_STRINGS "/serialnumber", cfg->gadget_serial)       != 0) return -1;

    /* Printer function — usb_f_printer sets class=7, subclass=1, protocol=2 */
    if (make_dir(GADGET_FUNC) != 0) return -1;

    /*
     * IEEE 1284 PNP string — OPOS ADK parses this to identify the printer model.
     * TMUSB.dll will ONLY bind if this matches exactly what it expects.
     * No trailing space before the final semicolon. No newline at end.
     */
    if (write_file(GADGET_FUNC "/pnp_string", cfg->gadget_pnp) != 0) return -1;

    /* Configuration */
    if (make_dir(GADGET_CONF)  != 0) return -1;
    if (make_dir(GADGET_CSTR)  != 0) return -1;
    if (write_file(GADGET_CSTR    "/configuration", "Printer Configuration") != 0) return -1;
    /*
     * bmAttributes: bit 7 MUST always be set (USB spec §9.6.3).
     * 0x80 = bus-powered, no self-power, no remote wakeup.
     * Writing any value with bit 7 clear returns EINVAL from configfs.
     */
    if (write_file(GADGET_CONF    "/bmAttributes",  "0x80") != 0) return -1;
    if (write_file(GADGET_CONF    "/MaxPower",       "500") != 0) return -1;

    /* Link function into configuration */
    if (symlink(GADGET_FUNC, GADGET_SYMLINK) != 0 && errno != EEXIST) {
        fprintf(stderr, "[gadget] symlink(%s → %s): %s\n",
                GADGET_FUNC, GADGET_SYMLINK, strerror(errno));
        return -1;
    }

    /* Bind to UDC — this activates the gadget */
    char *udc = find_udc();
    if (!udc) return -1;

    printf("[gadget] binding to UDC: %s\n", udc);
    int rc = write_file(GADGET_UDC, udc);
    free(udc);
    if (rc != 0) return -1;

    /*
     * Give the kernel ~300ms to register /dev/g_printer0 after UDC bind.
     * Without this, open() on the char device returns ENODEV during the
     * brief window between UDC bind and kernel char device registration.
     */
    usleep(300000);

    printf("[gadget] USB gadget active — waiting for host enumeration\n");
    printf("[gadget] POS system should see TM-T88VII on next plug-in\n");
    return 0;
}

void gadget_teardown(void)
{
    /* If gadget doesn't exist at all, nothing to do */
    struct stat st;
    if (stat(GADGET_BASE, &st) != 0) return;

    printf("[gadget] tearing down USB gadget\n");

    /*
     * Unbind from UDC by writing a newline.
     * Writing "" (0 bytes) is a no-op in configfs — the kernel
     * needs at least a newline to trigger the unbind handler.
     * Ignore errors here: UDC file may not exist if never bound.
     */
    int udc_fd = open(GADGET_UDC, O_WRONLY);
    if (udc_fd >= 0) {
        write(udc_fd, "\n", 1);
        close(udc_fd);
        /* Give the kernel a moment to complete the unbind */
        usleep(200000);  /* 200ms */
    }

    /* Remove symlink before removing the config directory */
    if (unlink(GADGET_SYMLINK) != 0 && errno != ENOENT)
        fprintf(stderr, "[gadget] unlink(%s): %s\n", GADGET_SYMLINK, strerror(errno));

    /* Remove directories in reverse creation order */
    remove_dir(GADGET_CSTR);
    remove_dir(GADGET_CONF);
    remove_dir(GADGET_FUNC);
    remove_dir(GADGET_STRINGS);
    remove_dir(GADGET_BASE);

    /* Verify cleanup succeeded */
    if (stat(GADGET_BASE, &st) == 0)
        fprintf(stderr, "[gadget] WARNING: %s still exists after teardown — "
                "may need reboot\n", GADGET_BASE);
    else
        printf("[gadget] teardown complete\n");
}

int gadget_is_bound(void)
{
    char buf[256] = {0};
    int fd = open(GADGET_UDC, O_RDONLY);
    if (fd < 0) return 0;
    ssize_t n = read(fd, buf, sizeof(buf) - 1);
    close(fd);
    /* UDC file contains the UDC name if bound, or empty/newline if not */
    return (n > 1);
}
