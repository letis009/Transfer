# simphony-printer-bridge

A virtual Epson TM-T88 Ethernet printer. Oracle Simphony talks to it as a normal
**IP Printer** on TCP 9100; it relays each slip to a real printer over Ethernet,
USB or USB-CDC.

Runtime is Node.js >= 18, CommonJS, zero mandatory dependencies. `serialport` is
optional and only loaded when `backend.type = "serial"`.

The full design document — including the ESC/POS bit maps, bring-up order and
porting notes — is `../simphony-printer-bridge.md`.

## Why a bridge and not a passthrough

Two rules the whole design hangs on:

1. **The upstream socket must never block on the backend.** Status queries are
   answered from a cached `PrinterState` in the same tick they arrive.
2. **Status is reported truthfully.** If you always answer "ready", Simphony's
   backup-printer failover never fires and slips silently vanish.

## Quick start

```bash
node --test test/                 # unit tests, no deps needed

# Run with the dev sink: every job lands as a .bin in a spool directory
BRIDGE_BACKEND=file BRIDGE_CONFIG=./config/dev.json node src/index.js

# In another shell, pretend to be Simphony
node tools/fake-pos.js 127.0.0.1 9100

# Decode what arrived
node tools/escpos-dump.js /var/lib/simphony-bridge/out/<timestamp>.bin
```

For a laptop run, drop a `config/dev.json` that moves the state directories
somewhere writable:

```json
{
  "backend": { "type": "file", "file": { "dir": "./run/out" } },
  "spool":   { "dir": "./run/spool" },
  "log":     { "level": "debug" }
}
```

## Configuration

`config/default.json` holds every key. Override it with a second JSON file
pointed at by `BRIDGE_CONFIG` — it is deep-merged over the defaults, so you only
name what changes. Three env vars shortcut the common edits:

| Variable | Effect |
|---|---|
| `BRIDGE_CONFIG` | path to the override JSON |
| `BRIDGE_BACKEND` | `tcp` \| `usblp` \| `serial` \| `file` |
| `BRIDGE_TARGET` | `backend.tcp.host` — the real printer's IP |
| `BRIDGE_LOG` | `debug` \| `info` \| `warn` \| `error` |

### Choosing a backend

| Backend | Use when | Notes |
|---|---|---|
| `tcp` | real printer on the LAN at `:9100` | polls `DLE EOT 1..4`; disables downstream ASB |
| `usblp` | `lsusb -v` shows printer class `0x07` | needs group `lp` on `/dev/usb/lp0` |
| `serial` | printer enumerates as CDC-ACM (TM-T88VI, TM-P80II default) | needs `serialport` and group `dialout` |
| `file` | bring-up and regression diffing | writes each job as a `.bin` |

## Operational notes

- **Job boundaries** are cut commands (`GS V`, legacy `ESC i` / `ESC m`). If a
  slip arrives without a cut, `listen.jobIdleMs` closes it after a quiet gap,
  and `listen.flushOnClose` closes it when the session drops.
- **The spool is disk-backed and fsynced** before a job is acknowledged, so a
  power cut mid-relay replays on boot rather than losing the slip. Retries back
  off from `spool.retryBaseMs` to `spool.retryMaxMs`.
- **Single session** mimics a real TM printer, which serves one connection at a
  time. Without it Simphony can open a second session on retry and interleave
  two jobs.
- **Identity bytes** (`identity.modelId` / `typeId` / `firmwareId`) answer
  `GS I`. Set them from a capture of the real printer you are standing in for.
- **ASB bit assignments vary between models** more than the `DLE EOT` ones do.
  Confirm `asbPacket` against your capture; if Simphony never sends `GS a n`,
  leave ASB disabled and rely on polling.

## Deployment

```bash
sudo install -d /opt/simphony-bridge /etc/simphony-bridge
sudo cp -r src config package.json /opt/simphony-bridge/
sudo cp config/default.json /etc/simphony-bridge/config.json   # then edit
sudo cp systemd/simphony-bridge.service /etc/systemd/system/
sudo useradd --system --no-create-home --shell /usr/sbin/nologin bridge
sudo systemctl enable --now simphony-bridge
journalctl -u simphony-bridge -f
```

`Group=lp` covers `/dev/usb/lp0`. For CDC-ACM add `SupplementaryGroups=dialout`.

## Simphony-side setup

EMC → **Property → Setup → Printers**, insert a record:

| Field | Value |
|---|---|
| Workstation | the workstation acting as print controller |
| Printer Type | IP Printer |
| Address | the bridge's IP (static or DHCP reservation) |
| Port | 9100 |
| Thermal printer | ticked |
| Center Logo Using | 3 1/8" (80 mm) if logo printing is on |

Then assign it as an Order Device or receipt printer as usual. Nothing else in
Simphony needs to know the bridge is not a printer.

## Layout

```
src/escpos/constants.js   opcode + command-length tables
src/escpos/scanner.js     streaming, length-aware ESC/POS scanner
src/escpos/status.js      PrinterState + DLE EOT / GS r / ESC v encoders
src/escpos/responder.js   query -> reply bytes, ASB packet builder
src/server/posListener.js TCP 9100, single session, job assembly
src/spool/jobSpool.js     disk-backed FIFO with retry/backoff
src/backends/             tcp | usblp | serial | file
tools/escpos-dump.js      annotate a captured 9100 stream
tools/fake-pos.js         pretend to be Simphony: print + poll + time
```

`constants.js`, `scanner.js` and `status.js` are pure byte logic with no I/O —
those are the three files that port to C if this ever moves to bare metal.
