#!/usr/bin/env bash
# install.sh — build and install p1-mitm on Raspberry Pi (Debian Trixie)
#
# Prerequisite hardware setup (do ONCE, before running this script):
#   1. Add to /boot/firmware/config.txt:
#        dtoverlay=dwc2,dr_mode=peripheral
#   2. Reboot
#   3. Verify: ls /sys/class/udc/   (should list one UDC)
#
set -e

echo "=== p1-mitm install ==="

# --- No build dependencies beyond gcc ---
echo "[1/4] Checking build tools..."
if ! command -v gcc > /dev/null; then
    sudo apt-get install -y build-essential
fi

# --- Build ---
echo "[2/4] Building..."
cd "$(dirname "$0")/.."
make clean
make

# --- Install binary + service ---
echo "[3/4] Installing binary and service..."
sudo make install

# --- Config file (OPTIONAL) ---
# p1-mitm needs NO config file: it auto-clones the attached printer's USB
# identity (VID/PID/serial/device-ID) and falls back to TM-T88VII defaults.
# We install an EXAMPLE only, for the rare case you want to pin an identity.
echo "[4/4] Installing example config (no live config required)..."
EXAMPLE_FILE="/etc/p1-mitm.conf.example"
sudo tee "$EXAMPLE_FILE" > /dev/null << 'CONF'
# p1-mitm — OPTIONAL override configuration
# -------------------------------------------------------
# Not required. With no /etc/p1-mitm.conf present, p1-mitm auto-clones the
# real printer's USB identity and uses built-in TM-T88VII defaults as fallback.
# Copy this to /etc/p1-mitm.conf and uncomment only the keys you want to PIN.

# --- Identity overrides (normally cloned from the real printer at runtime) ---
#GADGET_VID=0x04B8
#GADGET_PID=0x0E28
#GADGET_MANUFACTURER=SEIKO EPSON CORPORATION
#GADGET_PRODUCT=TM-T88VII
#GADGET_SERIAL=A1B2C3D4E5F6
#GADGET_PNP=MFG:EPSON;CMD:ESCPOS;MDL:TM-T88VII;CLS:PRINTER;DES:EPSON TM-T88VII;

# --- Device paths (rarely need changing) ---
#GADGET_DEV=/dev/g_printer0
#PRINTER_DEV=/dev/usb/lp0

# --- P2-overlay side-channel ---
#OVERLAY_FILE=/run/pos-overlay/overlay.txt
#OVERLAY_CLEAR_SECS=30
CONF
echo "  Wrote $EXAMPLE_FILE (informational only)"
if [ -f /etc/p1-mitm.conf ]; then
    echo "  Note: /etc/p1-mitm.conf exists — its values override auto-clone."
fi

# --- CUPS conflicts with direct /dev/usb/lp0 access ---
echo ""
echo "Disabling CUPS if present (it would hold /dev/usb/lp0)..."
sudo systemctl disable --now cups 2>/dev/null && echo "  CUPS disabled" \
    || echo "  CUPS not present / already disabled"

echo ""
echo "=== Hardware prerequisites ==="
echo "  Check /boot/firmware/config.txt contains:"
echo "    dtoverlay=dwc2,dr_mode=peripheral"
echo ""
grep -q "dwc2" /boot/firmware/config.txt \
    && echo "  [OK] dwc2 overlay present" \
    || echo "  [!!] dwc2 NOT found in config.txt — add it and reboot"
echo ""
echo "  Available UDCs (USB Device Controllers):"
ls /sys/class/udc/ 2>/dev/null || echo "  [!!] No UDC found — check dwc2 config"
echo ""

echo "=== Installation complete ==="
echo ""
echo "Next steps (no configuration needed — identity is auto-cloned):"
echo "  1. Connect the real printer to the Pi USB-A (host) port FIRST"
echo "  2. Connect the Pi USB-C (OTG) port to the POS system"
echo "  3. sudo systemctl enable --now p1-mitm"
echo "  4. sudo journalctl -u p1-mitm -f"
echo "     → look for 'identity CLONED' and 'R/W — status relay enabled'"
echo ""
echo "Test without real printer (parse only, TM-T88 fallback identity):"
echo "  Just run with no printer attached — passthrough is disabled and the"
echo "  built-in TM-T88VII identity is used. Journal shows 'FELL BACK'."
