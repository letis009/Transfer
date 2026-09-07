'use strict';

const fs = require('fs');
const path = require('path');
const { load } = require('./config');
const { create: createLog } = require('./log');
const { PrinterState } = require('./escpos/status');
const { PosListener } = require('./server/posListener');
const { JobSpool } = require('./spool/jobSpool');
const backends = require('./backends');

const cfg = load();
const log = createLog(cfg.log.level);
const state = new PrinterState();
const backend = backends.create(cfg.backend, state, log);
const spool = new JobSpool({ cfg: cfg.spool, backend, log });

const listener = new PosListener({ cfg: cfg.listen, state, identity: cfg.identity, log });

listener.on('job', (buf) => spool.enqueue(buf));
listener.on('error', (e) => { log.error(`listener: ${e.message}`); process.exit(1); });

state.on('change', (s) =>
  log.info(`state: online=${s.online} cover=${s.coverOpen} paper=${s.paperEnd ? 'END' : 'ok'} err=${s.error}`));

// Optional: keep the raw upstream stream for offline analysis with escpos-dump.
if (cfg.log.rawCapture) {
  fs.mkdirSync(cfg.log.rawCaptureDir, { recursive: true });
  const f = fs.createWriteStream(path.join(cfg.log.rawCaptureDir, `${Date.now()}.raw`));
  listener.on('raw', (c) => f.write(c));
}

listener.start();
spool.pump();

for (const sig of ['SIGINT', 'SIGTERM']) {
  process.on(sig, () => {
    log.info(`${sig} — shutting down`);
    listener.stop();
    backend.close();
    process.exit(0);
  });
}
