// Streaming-style hashes for files about to be uploaded.
// SHA-256: Web Crypto when available (HTTPS/localhost — HW-backed),
// otherwise a pure-JS streaming impl so plain-HTTP contexts still work.
// CRC32 is a small lookup-table impl. Both read the file in chunks so a
// 100 MB file does not allocate a single 100 MB buffer.

const CHUNK = 1024 * 1024;  // 1 MB

const CRC32_TABLE: Uint32Array = (() => {
  const t = new Uint32Array(256);
  for (let i = 0; i < 256; i++) {
    let c = i;
    for (let k = 0; k < 8; k++) c = c & 1 ? 0xedb88320 ^ (c >>> 1) : c >>> 1;
    t[i] = c >>> 0;
  }
  return t;
})();

const toHex = (bytes: Uint8Array): string => {
  let out = '';
  for (let i = 0; i < bytes.length; i++) {
    const h = bytes[i].toString(16);
    out += h.length === 1 ? '0' + h : h;
  }
  return out;
};

const readChunk = (file: File, start: number, end: number): Promise<ArrayBuffer> =>
  new Promise((resolve, reject) => {
    const reader = new FileReader();
    reader.onload = () => resolve(reader.result as ArrayBuffer);
    reader.onerror = () => reject(reader.error);
    reader.readAsArrayBuffer(file.slice(start, end));
  });

// FIPS 180-4 SHA-256 constants
const K = new Uint32Array([
  0x428a2f98, 0x71374491, 0xb5c0fbcf, 0xe9b5dba5, 0x3956c25b, 0x59f111f1, 0x923f82a4, 0xab1c5ed5,
  0xd807aa98, 0x12835b01, 0x243185be, 0x550c7dc3, 0x72be5d74, 0x80deb1fe, 0x9bdc06a7, 0xc19bf174,
  0xe49b69c1, 0xefbe4786, 0x0fc19dc6, 0x240ca1cc, 0x2de92c6f, 0x4a7484aa, 0x5cb0a9dc, 0x76f988da,
  0x983e5152, 0xa831c66d, 0xb00327c8, 0xbf597fc7, 0xc6e00bf3, 0xd5a79147, 0x06ca6351, 0x14292967,
  0x27b70a85, 0x2e1b2138, 0x4d2c6dfc, 0x53380d13, 0x650a7354, 0x766a0abb, 0x81c2c92e, 0x92722c85,
  0xa2bfe8a1, 0xa81a664b, 0xc24b8b70, 0xc76c51a3, 0xd192e819, 0xd6990624, 0xf40e3585, 0x106aa070,
  0x19a4c116, 0x1e376c08, 0x2748774c, 0x34b0bcb5, 0x391c0cb3, 0x4ed8aa4a, 0x5b9cca4f, 0x682e6ff3,
  0x748f82ee, 0x78a5636f, 0x84c87814, 0x8cc70208, 0x90befffa, 0xa4506ceb, 0xbef9a3f7, 0xc67178f2,
]);

const rotr = (x: number, n: number) => ((x >>> n) | (x << (32 - n))) >>> 0;

class Sha256Streaming {
  private h = new Uint32Array([
    0x6a09e667, 0xbb67ae85, 0x3c6ef372, 0xa54ff53a,
    0x510e527f, 0x9b05688c, 0x1f83d9ab, 0x5be0cd19,
  ]);
  private buf = new Uint8Array(64);
  private bufLen = 0;
  private totalLen = 0;
  private w = new Uint32Array(64);

  update(data: Uint8Array): void {
    this.totalLen += data.length;
    let i = 0;
    if (this.bufLen) {
      const need = 64 - this.bufLen;
      if (data.length < need) {
        this.buf.set(data, this.bufLen);
        this.bufLen += data.length;
        return;
      }
      this.buf.set(data.subarray(0, need), this.bufLen);
      this.processBlock(this.buf, 0);
      this.bufLen = 0;
      i = need;
    }
    while (i + 64 <= data.length) {
      this.processBlock(data, i);
      i += 64;
    }
    if (i < data.length) {
      this.buf.set(data.subarray(i));
      this.bufLen = data.length - i;
    }
  }

  digest(): Uint8Array {
    const lenBits = this.totalLen * 8;
    this.buf[this.bufLen++] = 0x80;
    if (this.bufLen > 56) {
      while (this.bufLen < 64) this.buf[this.bufLen++] = 0;
      this.processBlock(this.buf, 0);
      this.bufLen = 0;
    }
    while (this.bufLen < 56) this.buf[this.bufLen++] = 0;
    // 64-bit big-endian length; values past 2^53 won't appear in practice.
    const dv = new DataView(this.buf.buffer, this.buf.byteOffset);
    dv.setUint32(56, Math.floor(lenBits / 0x100000000));
    dv.setUint32(60, lenBits >>> 0);
    this.processBlock(this.buf, 0);
    const out = new Uint8Array(32);
    const dvOut = new DataView(out.buffer);
    for (let j = 0; j < 8; j++) dvOut.setUint32(j * 4, this.h[j]);
    return out;
  }

  private processBlock(block: Uint8Array, offset: number): void {
    const w = this.w;
    const dv = new DataView(block.buffer, block.byteOffset);
    for (let t = 0; t < 16; t++) w[t] = dv.getUint32(offset + t * 4);
    for (let t = 16; t < 64; t++) {
      const x15 = w[t - 15];
      const x2 = w[t - 2];
      const s0 = rotr(x15, 7) ^ rotr(x15, 18) ^ (x15 >>> 3);
      const s1 = rotr(x2, 17) ^ rotr(x2, 19) ^ (x2 >>> 10);
      w[t] = (w[t - 16] + s0 + w[t - 7] + s1) >>> 0;
    }
    let a = this.h[0], b = this.h[1], c = this.h[2], d = this.h[3];
    let e = this.h[4], f = this.h[5], g = this.h[6], hh = this.h[7];
    for (let t = 0; t < 64; t++) {
      const S1 = rotr(e, 6) ^ rotr(e, 11) ^ rotr(e, 25);
      const ch = (e & f) ^ (~e & g);
      const t1 = (hh + S1 + ch + K[t] + w[t]) >>> 0;
      const S0 = rotr(a, 2) ^ rotr(a, 13) ^ rotr(a, 22);
      const mj = (a & b) ^ (a & c) ^ (b & c);
      const t2 = (S0 + mj) >>> 0;
      hh = g; g = f; f = e; e = (d + t1) >>> 0;
      d = c; c = b; b = a; a = (t1 + t2) >>> 0;
    }
    this.h[0] = (this.h[0] + a) >>> 0;
    this.h[1] = (this.h[1] + b) >>> 0;
    this.h[2] = (this.h[2] + c) >>> 0;
    this.h[3] = (this.h[3] + d) >>> 0;
    this.h[4] = (this.h[4] + e) >>> 0;
    this.h[5] = (this.h[5] + f) >>> 0;
    this.h[6] = (this.h[6] + g) >>> 0;
    this.h[7] = (this.h[7] + hh) >>> 0;
  }
}

const subtleAvailable = (): boolean => {
  try {
    return !!(window.crypto && window.crypto.subtle && typeof window.crypto.subtle.digest === 'function');
  } catch {
    return false;
  }
};

// Returns lowercase 64-char hex SHA-256.
export async function sha256Hex(file: File, onProgress?: (done: number, total: number) => void): Promise<string> {
  if (subtleAvailable()) {
    try {
      // SubtleCrypto has no streaming API, so we read in chunks then
      // hand it the assembled buffer; saves nothing for memory but is fastest.
      const parts: Uint8Array[] = [];
      let done = 0;
      for (let off = 0; off < file.size; off += CHUNK) {
        const end = Math.min(off + CHUNK, file.size);
        const buf = await readChunk(file, off, end);
        parts.push(new Uint8Array(buf));
        done = end;
        onProgress?.(done, file.size);
      }
      const total = new Uint8Array(file.size);
      let cur = 0;
      for (const p of parts) { total.set(p, cur); cur += p.length; }
      const digest = await window.crypto.subtle.digest('SHA-256', total);
      return toHex(new Uint8Array(digest));
    } catch {
      // fall through to JS impl on unexpected SubtleCrypto failure
    }
  }
  // Pure-JS streaming fallback (works on plain HTTP / older browsers).
  const sha = new Sha256Streaming();
  let done = 0;
  for (let off = 0; off < file.size; off += CHUNK) {
    const end = Math.min(off + CHUNK, file.size);
    const buf = new Uint8Array(await readChunk(file, off, end));
    sha.update(buf);
    done = end;
    onProgress?.(done, file.size);
  }
  return toHex(sha.digest());
}

// Returns lowercase 8-char hex CRC32 (IEEE 802.3, same poly as zlib/PNG).
export async function crc32Hex(file: File, onProgress?: (done: number, total: number) => void): Promise<string> {
  let crc = 0xffffffff;
  let done = 0;
  for (let off = 0; off < file.size; off += CHUNK) {
    const end = Math.min(off + CHUNK, file.size);
    const buf = new Uint8Array(await readChunk(file, off, end));
    for (let i = 0; i < buf.length; i++) {
      crc = (crc >>> 8) ^ CRC32_TABLE[(crc ^ buf[i]) & 0xff];
    }
    done = end;
    onProgress?.(done, file.size);
  }
  if (file.size === 0) onProgress?.(0, 0);
  const out = (crc ^ 0xffffffff) >>> 0;
  return out.toString(16).padStart(8, '0');
}
