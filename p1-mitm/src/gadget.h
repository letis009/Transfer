#pragma once

#include "config.h"

/**
 * gadget.h — USB gadget configfs setup
 *
 * Configures the Linux USB gadget subsystem (via configfs at
 * /sys/kernel/config/usb_gadget/) to impersonate an Epson TM-T88VII
 * printer on the USB Device Controller (UDC) — the USB-C OTG port
 * on Pi 4B, or the Micro-USB OTG port on CM4.
 *
 * The POS system (Windows) sees exactly one USB device: an Epson
 * TM-T88VII. All data it sends to that printer arrives at
 * /dev/g_printer0 on the Pi. The Pi forwards it to the real printer
 * at /dev/usb/lp0 and simultaneously parses the ESC/POS stream.
 *
 * CRITICAL REQUIREMENTS for OPOS compatibility:
 *   1. VID/PID must match the real Epson TM-T88VII exactly
 *   2. IEEE 1284 PNP string must match what TMUSB.dll expects
 *   3. USB class must be Printer Class (7/1/2) — set by usb_f_printer.ko
 *
 * Prerequisites (must be done before calling gadget_setup()):
 *   - Kernel modules: libcomposite, usb_f_printer
 *   - configfs mounted at /sys/kernel/config
 *   - DWC2 in peripheral mode (dwc2 dr_mode=peripheral)
 *   - /boot/firmware/config.txt: dtoverlay=dwc2,dr_mode=peripheral
 *
 * On Pi 4B: the USB-C port (J1) is the OTG port.
 * Do NOT power the Pi through this port while using it as gadget —
 * use the GPIO 5V pins or the USB-A power supply port instead.
 */

/**
 * Set up the USB gadget using values from cfg.
 *
 * Creates the full configfs directory tree under:
 *   /sys/kernel/config/usb_gadget/epson_printer/
 *
 * Returns 0 on success, -1 on failure (errno set).
 *
 * Failure modes:
 *   - libcomposite module not loaded → ENOENT on configfs operations
 *   - configfs not mounted → ENOENT
 *   - DWC2 not in peripheral mode → no UDC found → returns -1
 *   - gadget already bound (duplicate setup) → EEXIST on mkdir → handled
 */
int gadget_setup(const Config *cfg);

/**
 * Tear down the USB gadget cleanly.
 * Unbinds from UDC, removes all symlinks and directories.
 * Call this on SIGTERM/SIGINT before exit.
 *
 * Failure to teardown leaves the gadget in a bound state which
 * prevents re-setup until reboot or manual removal.
 */
void gadget_teardown(void);

/**
 * Load required kernel modules (libcomposite, usb_f_printer).
 * Uses modprobe — requires root.
 *
 * Returns 0 if modules loaded successfully or already loaded.
 * Returns -1 if modprobe fails (module not available for this kernel).
 *
 * Failure modes:
 *   - Module not available for kernel version → check apt-get install linux-modules
 *   - Missing /lib/modules/$(uname -r)/ → kernel headers not installed
 */
int gadget_load_modules(void);

/**
 * Check if the USB gadget is currently bound and active.
 * Returns 1 if bound, 0 if not.
 */
int gadget_is_bound(void);
