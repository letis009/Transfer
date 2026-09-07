# transfer.md — p3-printlan → Pi 400 (`TDN-netprint`)

Record of the work done on 2026-09-07: building `simphony-printer-bridge` from
its spec, moving it to the Pi 400, and standing up a development environment
there. Written after `p3-printlan/` was removed from this repo.

> ## ⚠️ The Pi holds the only copy
>
> `p3-printlan/` no longer exists in this working tree and was **never
> committed** to git — it was untracked the whole time. The only surviving copy
> of the bridge is on the Pi at `~/p3-printlan`, where it *is* committed
> (`30a4525`, 24 files, clean tree).
>
> **Nothing here is recoverable from this repo's history.** To bring it back:
>
> ```bash
> cd /h/GitHub/TDN-USB/TDN-USB-v2      # POSIX path, not h:/...
> scp -r tdn-netprint:~/p3-printlan .
> ```
>
> Verified working — it restores all 24 files plus `.git`, so commit `30a4525`
> and its history come with it.
>
> Use the POSIX path. A Windows drive-letter destination (`h:/GitHub/...`) is
> parsed by `scp` as a *hostname* `h` and fails with
> `scp: mkdir h:/...: No such file or directory`.
>
> That Pi SD card is a single point of failure until the code is committed
> somewhere else. `p1-mitm` and `p2-overlay` were untouched throughout.

---

## 1. The target

| | |
|---|---|
| Hostname | `TDN-netprint` |
| Hardware | Raspberry Pi 400 Rev 1.0, arm64 (`aarch64`) |
| OS | Debian GNU/Linux 13 (trixie), kernel 6.18.34+rpt-rpi-v8 |
| RAM / disk | 3.7 GB / 15 GB card, 9.6 GB free |
| User | `admin` (uid 1000), in `sudo`, `dialout`, `lpadmin`, `plugdev`, `netdev` |
| sudo | password required (`admin`) — **not** passwordless |

**Both IPs given are the same machine.** `hostname -I` returns
`192.168.0.200 10.0.0.110` — the Pi is dual-homed, so these were never two
candidates to choose between. Everything below uses `192.168.0.200`.

State at first contact: `node`, `npm` and `git` were all **absent**. Port 9100
free, cups inactive, no printer attached (`lsusb` showed only the internal
keyboard and two hubs).

---

## 2. What was built

`simphony-printer-bridge` — a virtual Epson TM-T88 Ethernet printer. Oracle
Simphony talks to it as a normal **IP Printer** on TCP 9100; it relays each slip
to a real printer over Ethernet, USB or USB-CDC.

Implemented in full from `simphony-printer-bridge.md` (1430 lines): 23 files,
Node.js CommonJS, zero mandatory dependencies. The spec supplied every file
verbatim except `README.md`, which was written from its contents.

```
p3-printlan/
├── simphony-printer-bridge.md      the design doc / spec
└── simphony-printer-bridge/
    ├── README.md                   written (spec named it but gave no content)
    ├── package.json  .gitignore
    ├── config/default.json         from spec
    ├── config/dev.json             added — see §5
    ├── src/index.js  config.js  log.js
    ├── src/escpos/                 constants · scanner · status · responder
    ├── src/server/posListener.js   TCP 9100, single session, job assembly
    ├── src/spool/jobSpool.js       disk-backed FIFO, fsync + retry/backoff
    ├── src/backends/               tcp · usblp · serial · file
    ├── tools/escpos-dump.js  fake-pos.js
    ├── systemd/simphony-bridge.service
    └── test/scanner.test.js
```

The two rules the design hangs on, both preserved: the upstream socket never
blocks on the backend (status answered from cached `PrinterState` in the same
tick), and status is reported truthfully so Simphony's backup-printer failover
can actually fire.

---

## 3. Verification — what was actually run

Not "written and assumed". Every claim below was executed.

### Unit tests — 4/4 on both machines

Covers query stripping, raster payloads containing `DLE EOT` bytes *not* being
mistaken for commands, cut-based job splitting, and chunk-straddling commands.

### End-to-end on the Pi

Booted the bridge with the `file` backend and drove it as Simphony would:

```
DLE EOT 1..4 -> 0x12 0x12 0x12 0x12  (26 ms)
GS I 1       -> 0x20  (configured identity.modelId)
job relayed byte-identical to the slip (queries stripped, raster intact)
second concurrent session refused
SMOKE OK
```

### Over the LAN — the real Simphony path

Bridge on the Pi, client on this PC (`192.168.0.25`), across the network:

```
connected
DLE EOT 1 -> 0x12  (26 ms)     <- includes connection setup
DLE EOT 2 -> 0x12  (2 ms)
DLE EOT 3 -> 0x12  (1 ms)
DLE EOT 4 -> 0x12  (1 ms)
slip sent
```

Pi log: `session open from 192.168.0.25` → `job closed (cut), 153 bytes` →
`printed`. `escpos-dump` decoded it with 0 trailing bytes. **1–2 ms steady-state
status latency across the LAN** is the number that matters for failover.

### Transfer fidelity

All 24 files compared by md5 on both machines: **identical byte for byte**.

---

## 4. Setup performed on the Pi

**Toolchain** (from Debian trixie apt):

| | |
|---|---|
| nodejs | 20.19.2 |
| npm | 9.2.0 |
| git | 2.47.3 |
| Claude Code | 2.1.263, native installer → `~/.local/bin/claude` |

`export PATH="$HOME/.local/bin:$PATH"` appended to `~/.bashrc`.

**Git repo** initialised at `~/p3-printlan`, branch `main`, identity
`letis009 <65073218+letis009@users.noreply.github.com>`, one commit `30a4525`.

**Claude Code is installed but NOT logged in** — OAuth is interactive. Run
`claude` then `/login` on the Pi.

---

## 5. Changes made beyond the spec

Three, all deliberate.

**1. `package.json` test script → `node --test`**

The spec said `node --test test/`. That fails on Windows + Node 24 (Node treats
the directory as an entry module). The glob form `node --test "test/**/*.test.js"`
fixes Windows but fails on the Pi's Node 20 — glob expansion of `--test`
positional args only landed in Node 22. Bare `node --test` auto-discovers and is
the **only form that works on both**. Verified on each.

**2. `config/dev.json` added**

Bring-up step 2 calls for a laptop run with `backend.type = "file"`, but
`config/default.json` points every path at `/var/lib/simphony-bridge`, which is
unwritable on a dev box. Redirects spool and output under `./run/`:

```json
{
  "backend": { "type": "file", "file": { "dir": "./run/out" } },
  "spool":   { "dir": "./run/spool" },
  "log":     { "level": "debug" }
}
```

**3. `README.md` written** — the spec listed it in the scaffold without content.

---

## 6. Changes made on this PC

**SSH key auth**, replacing `admin:admin` password entry. Password login still
works; nothing was disabled.

- Generated `~/.ssh/tdn-netprint` — ed25519, **no passphrase**, comment
  `letis-pc -> TDN-netprint`
- Fingerprint `SHA256:DS+Xu27uI8zPzGWfD5U1aOseKHZ15jqO16X6qQS3Uhc`
- Public half appended to `~/.ssh/authorized_keys` on the Pi

**`~/.ssh/config` appended** (backup at `~/.ssh/config.bak-20260907`):

```
Host tdn-netprint
  HostName 192.168.0.200
  User admin
  IdentityFile ~/.ssh/tdn-netprint
  IdentitiesOnly yes
  ServerAliveInterval 30
```

`ssh tdn-netprint` now connects with no password, and the host appears by name
in the VS Code Remote Explorer.

### ⚠️ Host key change

PuTTY had a **different** host key cached for `192.168.0.200`. It was accepted
and is now `SHA256:BTQCAKYCovcHTbpRNVV2MZJIVXbqxqGAdosmMs2Z3E8`.

Benign if the Pi was re-flashed or the IP was reused by DHCP. If neither is
true, that warrants investigation before trusting the connection.

---

## 7. Working on the Pi

**Via VS Code Remote-SSH** (preferred — the extension is already installed):

`Ctrl+Shift+P` → `Remote-SSH: Connect to Host` → `tdn-netprint`, open
`/home/admin/p3-printlan`, run `claude` in the integrated terminal. That
terminal is a Pi shell, so Claude Code runs *on the Pi* and can drive real
hardware — `lsusb`, `/dev/usb/lp0`, binding :9100, `systemctl`. First connect
downloads ~100 MB of VS Code server; the Pi has working internet (verified).

**Or plain SSH:**

```bash
ssh tdn-netprint
cd ~/p3-printlan && claude
```

Running `/init` in the Pi checkout is worth it — the spec is 1430 lines and a
CLAUDE.md saves re-reading it each session.

### Syncing back

The two copies are independent git repos, so sync by patch:

```bash
ssh tdn-netprint 'cd ~/p3-printlan && git diff main' > /tmp/pi.patch
git apply /tmp/pi.patch
```

Re-push the whole tree after editing here:

```bash
tar --exclude=run --exclude=node_modules --exclude=.git -czf /tmp/p3.tgz p3-printlan/
scp /tmp/p3.tgz tdn-netprint:/tmp/ && ssh tdn-netprint 'cd ~ && tar -xzf /tmp/p3.tgz'
```

---

## 8. Open items

**`admin` is not in the `lp` group.** It has `dialout` (covers the `serial`
backend and `/dev/ttyACM0`) and `lpadmin`, but the `usblp` backend needs `lp` to
open `/dev/usb/lp0`:

```bash
sudo usermod -aG lp admin   # then log out and back in
```

**No printer attached yet.** `lsusb` shows only hubs and the internal keyboard,
and no `/dev/usb/lp0` or `/dev/ttyACM*` exist. Check `lsusb -v` when one is
connected: interface class `0x07` → `usblp` backend; CDC-ACM → `serial`.

**The capture is still the real spec.** Per §18 of the design doc, `identity.*`
(`modelId` 32 / `typeId` 2 / `firmwareId` 3) are placeholders, and the
`asbPacket` bit assignments vary by model far more than the `DLE EOT` ones do.
Both need a port-9100 capture of a live Simphony + TM-T88 pair before the `tcp`
backend goes near a production property.

**Bring-up order** from §18, for reference: capture first → `file` backend
against EMC → diff relayed jobs against the capture → `tcp` into a real printer
and pull the paper to confirm failover → `usblp`/`serial` → power-cut test
mid-job → only then the custom board.
