'use strict';

const fs = require('fs');
const path = require('path');

/** Development sink: every job lands as a .bin you can feed to escpos-dump. */
class FileBackend {
  constructor(cfg, state, log) {
    this.cfg = cfg; this.log = log;
    fs.mkdirSync(cfg.dir, { recursive: true });
    state.update({ online: true });
  }

  async print(buf) {
    const f = path.join(this.cfg.dir, `${Date.now()}.bin`);
    fs.writeFileSync(f, buf);
    this.log.info(`wrote ${f}`);
  }

  close() {}
}

module.exports = { FileBackend };
