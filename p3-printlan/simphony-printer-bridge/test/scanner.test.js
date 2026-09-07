'use strict';

const test = require('node:test');
const assert = require('node:assert');
const { EscPosScanner } = require('../src/escpos/scanner');

const kinds = (items) => items.map((i) => i.kind);

test('strips and reports a real-time status query', () => {
  const s = new EscPosScanner();
  const items = s.push(Buffer.from([0x41, 0x10, 0x04, 0x01, 0x42]));
  assert.deepStrictEqual(kinds(items), ['data', 'query', 'data']);
  assert.strictEqual(items[1].n, 1);
  assert.strictEqual(Buffer.concat([items[0].buf, items[2].buf]).toString(), 'AB');
});

test('does not mistake image payload for a real-time command', () => {
  // GS v 0 m=0 x=2 y=1 -> 2 payload bytes that happen to be DLE EOT
  const raster = Buffer.from([0x1d, 0x76, 0x30, 0x00, 0x02, 0x00, 0x01, 0x00, 0x10, 0x04]);
  const s = new EscPosScanner();
  const items = s.push(raster);
  assert.deepStrictEqual(kinds(items), ['data']);
  assert.strictEqual(items[0].buf.length, 10);
});

test('splits jobs on cut and keeps the cut bytes', () => {
  const s = new EscPosScanner();
  const items = s.push(Buffer.concat([
    Buffer.from('hi\n', 'latin1'),
    Buffer.from([0x1d, 0x56, 0x42, 0x00]),
    Buffer.from('next\n', 'latin1')
  ]));
  assert.deepStrictEqual(kinds(items), ['data', 'cut', 'data']);
  assert.strictEqual(items[0].buf.length, 3 + 4);
});

test('holds a command that straddles a chunk boundary', () => {
  const s = new EscPosScanner();
  assert.deepStrictEqual(kinds(s.push(Buffer.from([0x10, 0x04]))), []);
  assert.deepStrictEqual(kinds(s.push(Buffer.from([0x02]))), ['query']);
});
