'use strict';

const fs = require('fs');
const { EscPosScanner } = require('../src/escpos/scanner');

const file = process.argv[2];
if (!file) { console.error('usage: escpos-dump <file.raw|file.bin>'); process.exit(1); }

const scanner = new EscPosScanner();
const items = scanner.push(fs.readFileSync(file));

const QNAMES = { DLE_EOT: 'DLE EOT (real-time status)', GS_I: 'GS I (printer ID)',
                 GS_r: 'GS r (transmit status)', ESC_v: 'ESC v (paper sensor)',
                 ESC_u: 'ESC u (peripheral status)' };

let jobs = 1, bytes = 0;
for (const it of items) {
  switch (it.kind) {
    case 'data':
      bytes += it.buf.length;
      console.log(`  data  ${String(it.buf.length).padStart(6)} bytes  ` +
                  `${JSON.stringify(it.buf.toString('latin1').replace(/[^\x20-\x7e]/g, '.').slice(0, 60))}`);
      break;
    case 'query':
      console.log(`  QUERY ${QNAMES[it.q] || it.q}  n=${it.n}`);
      break;
    case 'realtime':
      console.log(`  RT    cmd=0x${it.cmd.toString(16)} n=${it.n}  [${it.raw.toString('hex')}]`);
      break;
    case 'asb':
      console.log(`  ASB   GS a ${it.n}`);
      break;
    case 'cut':
      console.log(`--- end of job ${jobs++} (${bytes} bytes) ---`);
      bytes = 0;
      break;
  }
}
console.log(`\n${scanner.buf.length} trailing bytes unparsed`);
