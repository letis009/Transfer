'use strict';

const { dleEot, escV, escU, gsR } = require('./status');

/**
 * Build the reply for an intercepted query. Returns null when the command
 * takes no reply.
 *
 * identity.modelId / typeId / firmwareId should be set from your capture of a
 * real TM-T88 so Simphony sees the ID it expects.
 */
function respond(item, state, identity) {
  switch (item.q) {
    case 'DLE_EOT': return Buffer.from([dleEot(item.n, state)]);
    case 'ESC_v':   return Buffer.from([escV(state)]);
    case 'ESC_u':   return Buffer.from([escU(state)]);
    case 'GS_r':    return Buffer.from([gsR(item.n, state)]);
    case 'GS_I':
      if (item.n === 1 || item.n === 49) return Buffer.from([identity.modelId]);
      if (item.n === 2 || item.n === 50) return Buffer.from([identity.typeId]);
      if (item.n === 3 || item.n === 51) return Buffer.from([identity.firmwareId]);
      return null;
    default:
      return null;
  }
}

/** 4-byte Automatic Status Back packet, sent unsolicited when state changes. */
function asbPacket(state) {
  let b1 = 0x10;                                   // fixed bit4
  if (state.drawerHigh) b1 |= 1 << 2;
  if (!state.online || state.coverOpen) b1 |= 1 << 3;

  let b2 = 0x00;
  if (state.cutterError)        b2 |= 1 << 3;
  if (state.unrecoverableError) b2 |= 1 << 5;
  if (state.autoRecoverError)   b2 |= 1 << 6;

  let b3 = 0x00;
  if (state.paperNearEnd) b3 |= 0b00000011;
  if (state.paperEnd)     b3 |= 0b00001100;

  return Buffer.from([b1, b2, b3, 0x00]);
}

module.exports = { respond, asbPacket };
