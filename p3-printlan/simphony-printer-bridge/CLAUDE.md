# CLAUDE.md

This file provides guidance to Claude Code (claude.ai/code) when working with code in this repository.

## What this is

A Node.js daemon that impersonates an Epson TM-T88 Ethernet thermal printer. Oracle
Simphony connects to it on TCP 9100 as a normal *IP Printer*; the bridge assembles
each slip and relays it to a real printer over TCP / USB (`usblp`) / USB-CDC
(`serial`) / a file sink. CommonJS, Node >= 18, zero mandatory dependencies
(`serialport` is optional and loaded only when `backend.type = "serial"`).

The authoritative design document is `../simphony-printer-bridge.md` (~1430 lines,
section-numbered). It holds the ESC/POS bit maps (§5–§8), the bring-up order (§18),
and the porting notes for the production SoC board (§19). Consult it before
changing any byte-level logic.

## Commands

```bash
node --test                    # run all unit tests
node --test test/scanner.test.js   # run one test file

# Run the bridge against a local file sink (jobs land as .bin under ./run/out)
BRIDGE_BACKEND=file BRIDGE_CONFIG=./config/dev.json node src/index.js

node tools/fake-pos.js 127.0.0.1 9100    # drive it as Simphony would: poll + slip + cut
node tools/escpos-dump.js <file.bin>     # decode a captured or relayed 9100 stream
```

There is no build step, no linter, and nothing to `npm install` for the default
(`tcp` / `usblp` / `file`) backends.

**Use bare `node --test`, not `node --test test/` or a glob.** The bare form is the
only one that auto-discovers tests on both the Pi (Node 20) and the Windows dev box
(Node 24) — see `../transfer.md` §5.

### Configuration

`config/default.json` holds every key. `BRIDGE_CONFIG=<file.json>` is deep-merged
over it (name only what changes). Env shortcuts: `BRIDGE_BACKEND`, `BRIDGE_TARGET`
(= `backend.tcp.host`), `BRIDGE_LOG`. `config/default.json` points state dirs at
`/var/lib/simphony-bridge` (needs root / systemd `StateDirectory`);
`config/dev.json` redirects them under `./run/` (gitignored) for a dev box.

## Architecture

The data path is a one-way pipeline with `PrinterState` cross-cutting it:

```
PosListener ──▶ EscPosScanner ──▶ (job buffer) ──▶ JobSpool ──▶ Backend
 TCP 9100        parse/classify      assemble        disk FIFO    tcp|usblp|serial|file
   ▲                                                                  │
   └────────────── PrinterState ◀── ingestReply ◀── DLE EOT poll ─────┘
        (query replies + ASB pushed back upstream)
```

Two invariants the whole design defends (README "Why a bridge"):

1. **The upstream socket never blocks on the backend.** Status queries are answered
   in the same tick from a cached `PrinterState`, never by awaiting the printer.
2. **Status is reported truthfully.** Always answering "ready" stops Simphony's
   backup-printer failover from ever firing, so slips silently vanish.

### The scanner is the load-bearing part (`src/escpos/scanner.js` + `constants.js`)

A streaming, length-aware ESC/POS parser. To find where the next command starts it
must know the exact length of the current one; mis-measuring a single command
desyncs the rest of the stream (the session is dropped on desync). `constants.js`
has the fixed-length tables (`ESC_FIXED` / `GS_FIXED` / `FS_FIXED`); `scanner.js`
has the variable-length cases (raster images, NUL-terminated forms, `GS ( fn pL pH`,
etc.) and buffers a command that straddles a chunk boundary (`NEED_MORE`).

`push(chunk)` returns an ordered list of typed items:

| kind | meaning | relayed downstream? |
|---|---|---|
| `data` | printable bytes | yes, verbatim |
| `cut` | `GS V` / `ESC i` / `ESC m` — job boundary | the cut bytes stay in the preceding `data` |
| `query` | `DLE EOT`, `GS I`, `GS r`, `ESC u/v` | no — answered locally from `PrinterState` |
| `realtime` | `DLE ENQ` / `DLE DC4` | no — surfaced as an event and dropped |
| `asb` | `GS a n` | no — records the ASB mask; the bridge generates ASB itself |

### Job assembly (`src/server/posListener.js`)

`data` items accumulate into a job buffer. The job closes and is emitted to the
spool on a `cut`, on `listen.jobIdleMs` of quiet, or on socket close when
`listen.flushOnClose` is set. `listen.singleSession` refuses a second concurrent
connection — a real TM printer serves one at a time, and without this Simphony can
open a second session on retry and interleave two jobs. When an ASB mask is set,
`PrinterState` 'change' events push a 4-byte ASB packet up the live socket.

### Spool (`src/spool/jobSpool.js`)

Disk-backed FIFO. A job is `fsync`ed and atomically renamed into place before it is
acknowledged, so a power cut mid-relay replays on boot instead of losing the slip.
Delivery is in filename order with exponential backoff from `spool.retryBaseMs` to
`spool.retryMaxMs`.

### Backends (`src/backends/`)

Factory in `index.js` keyed on `cfg.backend.type`. Each implements `print(buf)` and
`close()`. Only `tcp` also polls the real printer (`DLE EOT 1..4`) and feeds
`PrinterState.ingestReply`. `tcp` disables downstream ASB (`GS a 0`) on connect,
because unsolicited 4-byte ASB packets are otherwise indistinguishable from 1-byte
`DLE EOT` replies on the same socket.

### PrinterState (`src/escpos/status.js`)

Starts pessimistic (`online = false` until a backend proves otherwise).
`dleEot` / `escV` / `escU` / `gsR` encode state into reply bytes; `ingestReply`
decodes a backend's reply byte back into state. `src/escpos/responder.js` maps an
intercepted `query` item to its reply and builds the ASB packet.

`constants.js`, `scanner.js`, and `status.js` are pure byte logic with no I/O —
these three are the files that port to C if this moves to the bare-metal board.

## Before touching a real printer

- The ESC/POS bit maps in `status.js` and `responder.js` (`asbPacket`), and the
  `identity.modelId` / `typeId` / `firmwareId` in `config/default.json`, are
  **TM-T88-family placeholders**. They must be verified against a port-9100 capture
  of the actual Simphony + printer pair before the `tcp` backend goes near a
  production property (spec §18). No such capture exists yet.
- ASB bit assignments vary between models far more than the `DLE EOT` ones do. If
  Simphony never sends `GS a n`, leave ASB disabled and rely on polling.

## Repo context

This project is a subtree of the `Transfer` repo at
`p3-printlan/simphony-printer-bridge/`. `transfer.md` at the repo root is the
handoff record: the target Pi (`TDN-netprint`, 192.168.0.200), toolchain, SSH
setup, the verification runs that were actually executed, and the deliberate
deviations from the spec.
