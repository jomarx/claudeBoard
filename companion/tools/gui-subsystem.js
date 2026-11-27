// Marks a Windows exe as a GUI app (subsystem 2) instead of a console app (3), so
// starting it opens no console window. Only the two-byte Subsystem field in the PE
// optional header changes; pkg's payload offsets are untouched.

const fs = require('node:fs');

const file = process.argv[2];
if (!file) { console.error('usage: node tools/gui-subsystem.js <exe>'); process.exit(1); }
const fd = fs.openSync(file, 'r+');
const read = (pos, n) => { const b = Buffer.alloc(n); fs.readSync(fd, b, 0, n, pos); return b; };

if (read(0, 2).toString('latin1') !== 'MZ') throw new Error('not an exe');
const pe = read(0x3c, 4).readUInt32LE(0);
if (read(pe, 4).toString('latin1') !== 'PE\0\0') throw new Error('no PE header');
const optional = pe + 4 + 20;                 // after the signature and COFF header
const magic = read(optional, 2).readUInt16LE(0);
if (magic !== 0x10b && magic !== 0x20b) throw new Error('unknown optional header');
const at = optional + 68;                     // Subsystem, same offset in PE32 and PE32+
const was = read(at, 2).readUInt16LE(0);
const b = Buffer.alloc(2); b.writeUInt16LE(2, 0);
fs.writeSync(fd, b, 0, 2, at);
fs.closeSync(fd);
console.log(`${file}: subsystem ${was} -> 2 (GUI)`);
