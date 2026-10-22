import struct
import hashlib

try:
    import cryptolib
except ImportError:
    import ucryptolib as cryptolib

LAST4 = (0x0000, 0x1C20, 0x3840, 0x2460, 0x7080, 0x6CA0, 0x48C0, 0x54E0,
         0xE100, 0xFD20, 0xD940, 0xC560, 0x9180, 0x8DA0, 0xA9C0, 0xB5E0)


def derive(code):
    cb = code.encode()
    h = hashlib.sha256(b"johnify-v1|" + cb).digest()
    for _ in range(5000):
        h = hashlib.sha256(h + cb).digest()
    return h, hashlib.sha256(h + b"gid").digest()[:8]


def make_aad(gid, msgid, devid):
    return gid + struct.pack("<II", msgid, devid)


def _table(h):
    m = [0] * 16
    m[8] = h
    v = h
    for i in (4, 2, 1):
        if v & 1:
            v = (v >> 1) ^ (0xE1 << 120)
        else:
            v >>= 1
        m[i] = v
    for i in (2, 4, 8):
        for j in range(1, i):
            m[i + j] = m[i] ^ m[j]
    return m


def _mul(m, x):
    z = 0
    for k in range(32):
        z = (z >> 4) ^ (LAST4[z & 15] << 112)
        z ^= m[(x >> (4 * k)) & 15]
    return z


def _ghash(m, aad, ct):
    y = 0
    for data in (aad, ct):
        for i in range(0, len(data), 16):
            blk = data[i:i + 16]
            if len(blk) < 16:
                blk = blk + bytes(16 - len(blk))
            y = _mul(m, y ^ int.from_bytes(blk, "big"))
    return _mul(m, y ^ (((len(aad) * 8) << 64) | (len(ct) * 8)))


class Cipher:
    def __init__(self, key):
        self.aes = cryptolib.aes(key, 1)
        self.tbl = _table(int.from_bytes(self.aes.encrypt(bytes(16)), "big"))

    def _ctr(self, nonce, data):
        out = []
        for i in range(0, len(data), 16):
            ks = self.aes.encrypt(nonce + struct.pack(">I", 2 + i // 16))
            chunk = data[i:i + 16]
            k = len(chunk)
            x = int.from_bytes(chunk, "big") ^ int.from_bytes(ks[:k], "big")
            out.append(x.to_bytes(k, "big"))
        return b"".join(out)

    def _tag(self, nonce, aad, ct):
        s = _ghash(self.tbl, aad, ct)
        e = int.from_bytes(self.aes.encrypt(nonce + b"\x00\x00\x00\x01"), "big")
        return (e ^ s).to_bytes(16, "big")

    def seal(self, nonce, pt, aad):
        ct = self._ctr(nonce, pt)
        return ct + self._tag(nonce, aad, ct)

    def open(self, nonce, ct, tag, aad):
        if self._tag(nonce, aad, ct) != tag:
            return None
        return self._ctr(nonce, ct)
