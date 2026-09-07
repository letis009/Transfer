'use strict';

const net = require('net');
const host = process.argv[2] || '127.0.0.1';
const port = Number(process.argv[3] || 9100);

const slip = Buffer.concat([
  Buffer.from([0x1b, 0x40]),                        // ESC @
  Buffer.from([0x1b, 0x61, 0x01]),                  // centre
  Buffer.from('TDN TEST SLIP\n\n', 'latin1'),
  Buffer.from([0x1b, 0x61, 0x00]),                  // left
  Buffer.from('1  Flat White            42.00\n', 'latin1'),
  Buffer.from('1  Croissant             35.00\n', 'latin1'),
  Buffer.from('                        ------\n', 'latin1'),
  Buffer.from('   TOTAL                 77.00\n\n\n', 'latin1'),
  Buffer.from([0x1d, 0x56, 0x42, 0x00])             // GS V 66 0 — feed and cut
]);

const s = net.connect({ host, port }, () => {
  console.log('connected');

  let n = 1, sent = 0;
  const ask = () => { sent = Date.now(); s.write(Buffer.from([0x10, 0x04, n])); };

  s.on('data', (d) => {
    console.log(`DLE EOT ${n} -> 0x${d[0].toString(16).padStart(2, '0')}  (${Date.now() - sent} ms)`);
    if (++n <= 4) return ask();
    s.write(slip, () => { console.log('slip sent'); setTimeout(() => s.end(), 500); });
  });

  ask();
});

s.on('error', (e) => console.error(e.message));
