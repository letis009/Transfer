# P1-MITM — USB Printer Man-in-the-Middle

## What This Program Does

`p1-mitm` sits invisibly between a POS (Point of Sale) system running Windows
and a real Epson TM-T88 USB receipt printer. From the POS system's
perspective, it is connected directly to the printer. From the real
printer's perspective, it is connected directly to the POS system. Neither end
knows the Pi is in the middle.

**Zero field configuration.** At startup `p1-mitm` reads the USB identity of the
real printer attached to its host port — VID, PID, serial, manufacturer/product
strings, and the IEEE-1284 device-ID — and **clones it onto the gadget**, so the
POS sees the exact printer that is plugged in. If no printer can be read, it
falls back to built-in TM-T88VII defaults. No per-site config file is required.

**Real status replication.** The relay is bidirectional. The real printer's
live status — DLE EOT real-time replies and unsolicited Automatic Status Back
(paper-out, cover-open, errors) — is relayed straight back to the POS, and the
printer's USB printer-class status byte is mirrored onto the gadget. If the real
printer runs out of paper, Oracle Simphony sees it.

While this transparent relay is happening, `p1-mitm` also decodes the raw ESC/POS
binary stream, extracts the human-readable receipt text, and writes it to a file
that Project 2 (p2-overlay) watches — driving a live text overlay on the IP
camera video stream. (This side-channel is independent of the print path.)

```
POS System (Windows / Oracle Simphony)
    │  USB (sees the cloned identity of the real printer)
    ▼
Pi USB-C port (OTG/gadget mode)
    │  /dev/g_printer0  (bidirectional)
    ▼
┌────────────────────────────────────────────────────────────┐
│                          p1-mitm                           │
│                                                            │
│  0. Clone real printer USB identity → gadget (or fallback) │
│  host→printer:  forward ALL bytes → /dev/usb/lp0           │
│  printer→host:  relay real status (DLE EOT / ASB) → gadget │
│  status mirror: LPGETSTATUS(real) → gadget class status    │
│  overlay:       parse ESC/POS, on GS V cut → overlay file  │
└────────────────────────────────────────────────────────────┘
    │  /dev/usb/lp0 (R/W)              │  /run/pos-overlay/overlay.txt
    ▼                                  ▼
Real Epson TM-T88             p2-overlay (inotify watcher)
(prints; reports status)      (updates video overlay text)
```

---

## Hardware Setup

### Required connections

| Port | Connection | Notes |
|------|-----------|-------|
| Pi USB-C (OTG) | → POS system USB-A port | Gadget mode — Pi appears as printer |
| Pi USB-A port | → Real Epson TM-T88VII | Host mode — Pi sends to real printer |
| Pi USB-C | DO NOT use for power | Use GPIO 5V or separate PSU |

### Pi 4B OTG port
The USB-C port on the Pi 4B is a USB 2.0 OTG port driven by the DWC2 controller.
It can operate in either host or peripheral (gadget) mode. We need peripheral mode.

### /boot/firmware/config.txt — required addition
```
dtoverlay=dwc2,dr_mode=peripheral
```

This device tree overlay tells the DWC2 USB controller to operate as a USB device
(peripheral) rather than a USB host. Without this, the USB-C port acts as a host
and the gadget subsystem is unavailable.

**After adding this line, a reboot is required.** The change is not hot-reloadable.

**Verify after reboot:**
```bash
ls /sys/class/udc/
# Expected: fe980000.usb  (or similar — this is the DWC2 UDC)

cat /sys/class/udc/*/state
# Expected: not attached  (before POS is connected)
# Expected: configured    (after POS enumerates the gadget)
```

**Failure mode:** If `/sys/class/udc/` is empty after reboot, DWC2 is not in
peripheral mode. Check that `dtoverlay=dwc2,dr_mode=peripheral` is in the
correct section of config.txt and that the file was saved correctly:
```bash
grep dwc2 /boot/firmware/config.txt
```

---

## Kernel Modules

Two kernel modules must be loaded before the gadget can be configured:

### libcomposite
The USB composite gadget framework. Provides the configfs interface under
`/sys/kernel/config/usb_gadget/`. Without this module, the configfs gadget
directory does not exist.

```bash
sudo modprobe libcomposite
# Verify:
ls /sys/kernel/config/usb_gadget/
```

### usb_f_printer
The USB printer function driver. Provides:
- `/dev/g_printer0` — the character device the program reads/writes
- USB Interface Class 7 (Printer), SubClass 1, Protocol 2 — required for
  Windows to load the correct USB printer driver

```bash
sudo modprobe usb_f_printer
# Verify:
ls /dev/g_printer0
```

**Failure mode:** If `modprobe usb_f_printer` fails with "Module not found",
the module is not available for this kernel. Install the full kernel modules:
```bash
sudo apt-get install linux-modules-extra-$(uname -r)
# or on Raspbian:
sudo apt-get install raspberrypi-kernel
```

`p1-mitm` calls `gadget_load_modules()` at startup which runs these modprobes
automatically. If they fail, a warning is printed but execution continues
(the modules may already be loaded or built into the kernel).

---

## USB Gadget Configuration (gadget.c)

### What configfs is

configfs is a Linux kernel filesystem that exposes kernel configuration via
a directory tree under `/sys/kernel/config/`. For USB gadgets, creating
directories and writing to files in `/sys/kernel/config/usb_gadget/` directly
configures the USB device controller — no ioctl or special API needed.

### Directory structure created

```
/sys/kernel/config/usb_gadget/epson_printer/
├── idVendor          ← 0x04B8 (Seiko Epson Corp)
├── idProduct         ← 0x0E28 (TM-T88VII)
├── bcdDevice         ← 0x0100
├── bcdUSB            ← 0x0200
├── strings/
│   └── 0x409/        ← English language strings
│       ├── manufacturer  ← "SEIKO EPSON CORPORATION"
│       ├── product       ← "TM-T88VII"
│       └── serialnumber  ← from real printer
├── functions/
│   └── printer.0/    ← USB printer function
│       └── PNP_STRING ← IEEE 1284 identification string
├── configs/
│   └── c.1/          ← USB configuration descriptor
│       ├── bmAttributes ← 0x02 (bus powered)
│       ├── MaxPower     ← 500 (mA)
│       ├── printer.0 → ../../functions/printer.0  (symlink)
│       └── strings/0x409/
│           └── configuration ← "Printer Configuration"
└── UDC               ← Write UDC name here to activate gadget
```

### The UDC binding — activation step

Writing the UDC name to the `UDC` file is the final step that activates the
gadget. The UDC name is found by listing `/sys/class/udc/`. On Pi 4B with
DWC2 in peripheral mode, this is typically `fe980000.usb`.

After writing to UDC, the Pi's USB-C port begins advertising itself as an
Epson TM-T88VII when connected to a USB host.

**Failure mode:** If `write(UDC, udc_name)` fails with `EINVAL`, the gadget
configuration is incomplete or invalid. Most common causes:
- PNP_STRING contains a trailing space or incorrect semicolons
- VID/PID format incorrect (must be `0x04B8` not `04B8` or `0x04b8`)
- functions/printer.0 directory not properly populated
- libcomposite or usb_f_printer module not loaded

**Failure mode:** If UDC write succeeds but Windows does not recognise the
device (shows unknown device or yellow warning):
1. Check VID/PID exactly matches the real printer with `lsusb`
2. Check the PNP string with: `cat /sys/kernel/config/usb_gadget/epson_printer/functions/printer.0/PNP_STRING`
3. Try removing the device in Windows Device Manager and re-plugging

### OPOS recognition chain

OPOS (OLE for Retail POS) on Windows will only bind to the gadget if ALL
three of these match what it expects:

1. **VID=0x04B8, PID=0x0E28** — Seiko Epson Corp, TM-T88VII USB
   - To verify: plug real printer into a Linux machine → `lsusb | grep Epson`
   - PID varies by firmware: 0x0E28 is most common but may differ

2. **IEEE 1284 PNP string**:
   `MFG:EPSON;CMD:ESCPOS;MDL:TM-T88VII;CLS:PRINTER;DES:EPSON TM-T88VII;`
   - TMUSB.dll (Epson's USB driver) parses this to identify the model
   - One wrong character = OPOS shows "(none)" in port dropdown

3. **USB Class 7/SubClass 1/Protocol 2** — USB Printer Class
   - Set automatically by `usb_f_printer` kernel module
   - Cannot be overridden in configfs

---

## The Read Loop (main.c)

### How /dev/g_printer0 works

`/dev/g_printer0` is a bidirectional character device:
- **Read** from it: gets bytes the USB host (POS) sent to the printer
- **Write** to it: sends bytes back to the USB host (status responses)

The device uses a kernel ring buffer. `read()` blocks until data is available.
We wrap it in a `select()` on **both** the gadget device and the real printer
device (opened `O_RDWR`), with a 1-second timeout so the program can re-poll the
real printer's class status and check the `g_running` flag (set by
SIGTERM/SIGINT) for a clean exit.

### Step 1: DLE EOT status — replicated from the real printer

OPOS continuously polls the printer's status by sending `DLE EOT` (0x10 0x04 n).
If the printer does not respond within approximately 200–500ms, OPOS transitions
the printer object to the `offline` state and stops sending print jobs.

**When a real printer is attached** we do not fake this reply. The `DLE EOT` is
passed straight through to the real printer (Step 2), which answers in real time
on its bulk-IN endpoint; the main loop reads that reply from `/dev/usb/lp0` and
relays it byte-for-byte to `/dev/g_printer0`, so the POS receives the printer's
genuine status. The same path relays the printer's unsolicited Automatic Status
Back (ASB) — the bytes it sends on its own when paper runs out or the cover
opens — so paper-out reaches Simphony without polling. This is
`relay_printer_status()`.

Independently, every ~2 s the loop reads the real printer's USB printer-class
port status with the `LPGETSTATUS` ioctl and, on change, mirrors it onto the
gadget via `GADGET_SET_PRINTER_STATUS` (`poll_and_mirror_status()`), so the
generic Windows `usbprint.sys`/OPOS enumeration read also reflects real state.

**Fallback (no real printer):** `handle_dle_eot()` answers locally with a
synthetic "online" byte (`0x22`) so OPOS does not drop the port during bench
testing or a printer unplug. This local responder is used **only** when
`g_printer_fd < 0`; when a printer is present it stays silent so the host never
receives a duplicate status byte.

**Failure mode:** If status is not relayed within the OPOS timeout:
- OPOS status shows `StatusUpdateEvent` with `offline` state
- Check the journal for `relayed N status byte(s) printer→POS` lines
- Confirm the printer opened R/W: journal shows `R/W — status relay enabled`

### Step 2: Passthrough to real printer

Every byte read from the gadget is written to `/dev/usb/lp0` unchanged.
This includes all ESC/POS commands, formatting bytes, and barcode data —
the real printer needs the complete stream, not just the text portions.

The write uses a loop to handle partial writes (unlikely on a local device
but correct practice). If the real printer is disconnected (`EPIPE` or `EIO`),
the printer fd is closed and set to -1. The program continues running —
receipt parsing and overlay updates continue even without a physical printer.
This allows development and testing without a real printer attached.

**Failure mode:** `/dev/usb/lp0` does not exist
- The real printer is not plugged in, or the `usblp` module is not loaded
- `usblp` is loaded automatically by udev when a USB printer is plugged in
- If it conflicts with another driver: `sudo modprobe usblp`

**Failure mode:** `/dev/usb/lp0` exists but is busy (`EBUSY`)
- CUPS (Common Unix Printing System) has claimed the device
- Disable CUPS: `sudo systemctl disable --now cups`

### Step 3: ESC/POS parsing

Each byte is fed to the state machine parser (`escpos_feed_buf()`). The parser
returns `ESCPOS_RECEIPT_CUT` when a paper cut command is detected. At that
point, `escpos_get_receipt()` builds the overlay text and `overlay_write()` is
called to update the file.

---

## ESC/POS Parser (escpos.c)

### What ESC/POS is

ESC/POS is a binary protocol. A typical receipt stream looks like:
```
ESC @               ← Initialize (reset printer)
ESC a 1             ← Center alignment
"ACME STORE"        ← Printable text
LF                  ← Print and advance line
ESC a 0             ← Left alignment
"Item 1     R25.00" ← Printable text
LF
"Total      R25.00" ← Printable text
LF
ESC d 3             ← Feed 3 lines
GS V 65 0           ← Partial cut (end of receipt)
```

The challenge is that ESC/GS command bytes have a variable number of
parameter bytes. Treating a parameter byte as printable text produces garbage.
The parser uses a state machine to track exactly which bytes are parameters
and which are text.

### State machine states

| State | Meaning | Transition |
|-------|---------|-----------|
| `STATE_NORMAL` | Collecting printable chars | ESC→STATE_ESC, GS→STATE_GS, DLE→STATE_DLE, LF→commit line |
| `STATE_ESC` | Received 0x1B | Next byte is command → determines params |
| `STATE_GS` | Received 0x1D | Next byte is command → determines params |
| `STATE_DLE` | Received 0x10 | Next byte discarded → STATE_NORMAL |
| `STATE_SKIP` | Skipping N param bytes | Decrements counter → STATE_NORMAL at 0 |
| `STATE_GS_V` | GS V received | Next byte is cut subcommand |
| `STATE_GS_V_FULL` | GS V 65/66 | One more param byte → RECEIPT_CUT |

### Paper cut detection

`GS V` (0x1D 0x56) is the paper cut command. It has four variants:
- `GS V 0`  — full cut, no feed
- `GS V 1`  — partial cut, no feed
- `GS V 65 n` — full cut after feeding n lines
- `GS V 66 n` — partial cut after feeding n lines

All four signal end-of-receipt. The Epson TM-T88VII uses `GS V 65 0` or
`GS V 1` most commonly. The parser handles all four.

**Before signalling RECEIPT_CUT**, `escpos_flush_line()` is called to commit
any partial line in the line buffer. Without this, the last line of the receipt
(before the cut command) would be lost if it was not followed by a LF.

### Unknown commands

Unknown ESC/GS commands log a warning and return to STATE_NORMAL without
skipping any bytes. This is a "best effort" approach — an unknown command
may cause the following parameter bytes to be misinterpreted as text,
producing garbage characters in the overlay. If this happens:
1. Check the garbage characters' ASCII values
2. Look up which ESC/POS command they correspond to
3. Add a handler for that command in the `STATE_ESC` or `STATE_GS` switch

### GS k (barcode) handling

Barcode commands have two formats with different length semantics:
- Format 1 (type 0–6): data terminated by 0x00 (NUL)
- Format 2 (type 65–73): 1 byte length followed by that many data bytes

Both formats are handled by the `STATE_GS_K` / `STATE_GS_K_LEN` /
`STATE_GS_K_DATA` state sequence.

---

## Overlay File Writer (overlay.c)

### Why atomic rename?

A naive `fopen(path, "w"); fwrite(text); fclose()` creates a race condition:
P2-overlay's inotify fires on `open()` (or `write()`) before the file is
complete. P2 reads a partial file.

The solution is write-to-temp-then-rename:
1. Write complete text to `overlay.txt.tmp`
2. `rename("overlay.txt.tmp", "overlay.txt")` — atomic on Linux (same filesystem)

inotify fires on the rename, so P2 reads a complete, consistent file every time.

### Auto-clear via SIGALRM

`alarm(N)` schedules a SIGALRM after N seconds. Each new receipt resets the
timer: `alarm(clear_secs)` cancels any pending alarm and sets a new one.

The SIGALRM handler writes a single space to the overlay file, which P2
interprets as empty text (textoverlay trims whitespace).

**Failure mode:** SIGALRM handler uses only async-signal-safe functions
(`open()`, `write()`, `close()`). Do NOT call `printf()`, `malloc()`, or any
non-async-safe function inside the signal handler — this causes undefined
behaviour. The handler in this code uses only safe primitives.

---

## Configuration File (/etc/p1-mitm.conf) — OPTIONAL

**The config file is optional and normally not present in the field.** With no
file, `p1-mitm` runs entirely on built-in defaults and auto-clones the attached
printer's identity, so nothing needs configuring. The `GADGET_*` identity keys
below are **overridden at runtime by the cloned values** whenever a real printer
can be read; they only take effect as a fallback (no printer readable) or if you
deliberately want to pin an identity. A missing/unreadable file is logged and the
program continues.

| Key | Default | Description |
|-----|---------|-------------|
| `GADGET_VID` | `0x04B8` | Fallback vendor ID (cloned from printer when present) |
| `GADGET_PID` | `0x0E28` | Fallback product ID (cloned when present) |
| `GADGET_MANUFACTURER` | `SEIKO EPSON CORPORATION` | Fallback string (cloned when present) |
| `GADGET_PRODUCT` | `TM-T88VII` | Fallback string (cloned when present) |
| `GADGET_SERIAL` | `A1B2C3D4E5F6` | Fallback serial (cloned when present) |
| `GADGET_PNP` | (full string) | Fallback IEEE-1284 device-ID (cloned when present) |
| `GADGET_DEV` | `/dev/g_printer0` | Gadget printer character device |
| `PRINTER_DEV` | `/dev/usb/lp0` | Real printer device (passthrough + status, opened R/W) |
| `OVERLAY_FILE` | `/run/pos-overlay/overlay.txt` | Shared with p2-overlay |
| `OVERLAY_CLEAR_SECS` | `30` | Seconds of inactivity before text clears |

### Finding the real VID/PID

Connect the real Epson TM-T88VII to a USB port on the Pi (while p1-mitm is
NOT running) and run:
```bash
lsusb
# Look for: Bus 001 Device 003: ID 04b8:0e28 Seiko Epson Corp.

lsusb -v -d 04b8: 2>/dev/null | grep -E "idVendor|idProduct|iSerial"
```

The serial number in `iSerial` should be copied to `GADGET_SERIAL` — some
Windows OPOS implementations track the serial number and may reject a mismatch.

---

## Build and Install

```bash
# On Pi — no external library dependencies
cd ~/p1-mitm
make
sudo make install
```

### First-time hardware setup (one time only)

```bash
# 1. Add to /boot/firmware/config.txt:
echo "dtoverlay=dwc2,dr_mode=peripheral" | sudo tee -a /boot/firmware/config.txt

# 2. Reboot
sudo reboot

# 3. Verify UDC available
ls /sys/class/udc/

# 4. Disable CUPS if installed (conflicts with /dev/usb/lp0)
sudo systemctl disable --now cups 2>/dev/null || true
```

### Running

```bash
# Start manually (for testing)
sudo p1-mitm

# Or via systemd
sudo systemctl enable --now p1-mitm
sudo journalctl -u p1-mitm -f
```

---

## Testing Without Real Printer

During development, the real printer (`/dev/usb/lp0`) is optional. Set
`PRINTER_DEV` to a path that does not exist — the passthrough will be silently
disabled and the program runs in parse-only mode.

To simulate a POS print job without a POS system, you can write raw ESC/POS
bytes directly to the gadget device:

```bash
# Simple test — write text + paper cut to gadget (simulates POS system)
printf "\x1b\x40Item 1     R25.00\x0aTotal      R25.00\x0a\x1dV\x41\x00" \
  | sudo tee /dev/g_printer0 > /dev/null

# Then check the overlay file:
cat /run/pos-overlay/overlay.txt
```

---

## Failure Mode Quick Reference

| Symptom | Most Likely Cause | Fix |
|---------|-----------------|-----|
| `/sys/class/udc/` empty | DWC2 not in peripheral mode | Add `dtoverlay=dwc2,dr_mode=peripheral` to config.txt and reboot |
| `/dev/g_printer0` not found | `usb_f_printer` not loaded or gadget not bound | `sudo modprobe usb_f_printer` |
| Windows shows "Unknown Device" | Clone failed & fallback PID wrong | Check journal `identity CLONED`/`FELL BACK`; verify with `lsusb` |
| Identity not cloned (`FELL BACK`) | printer absent/opened after POS, or `/dev/usb/lp0` busy | Attach printer BEFORE the Pi; free the device from CUPS |
| OPOS port dropdown shows "(none)" | PNP string mismatch | Check cloned/`GADGET_PNP` device-ID string |
| OPOS marks printer offline | Status not relayed | Journal `relayed N status byte(s)`; confirm `R/W — status relay enabled` |
| Paper-out not seen by POS | Printer opened write-only, or ASB disabled by POS | Confirm R/W open; ASB is enabled by the POS via `GS a` (passed through) |
| Real printer not printing | `/dev/usb/lp0` busy | `sudo systemctl stop cups` |
| Overlay shows garbage text | Unknown ESC/POS command interpreted as text | Check journal for `unknown ESC/GS` warnings |
| Overlay text never appears | `overlay.txt` not being written | Check `overlay_write()` log in journal |

---

## File Structure

```
p1-mitm/
├── Makefile
├── README.md                 ← this file
├── src/
│   ├── main.c               — bidirectional relay, status mirror, identity clone, orchestration
│   ├── printer.c / printer.h — real-printer identity probe + status ioctls (usblp)
│   ├── config.c / config.h  — optional /etc/p1-mitm.conf parser + defaults
│   ├── gadget.c / gadget.h  — USB configfs gadget setup
│   ├── escpos.c / escpos.h  — ESC/POS state machine parser
│   └── overlay.c / overlay.h — atomic file writer + SIGALRM clear
└── deploy/
    ├── p1-mitm.service      — systemd unit
    └── install.sh           — build + install script
```
