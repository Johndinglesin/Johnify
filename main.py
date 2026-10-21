import sys
import os
import gc
import time
import json
import struct
import socket
import hashlib
import network
import machine

try:
    import espnow
except ImportError:
    print("[johnify] This MicroPython build has no espnow module. Use MicroPython 1.21 or newer for an ESP32-family board.")
    raise

try:
    import asyncio
except ImportError:
    import uasyncio as asyncio
try:
    import cryptolib
except ImportError:
    import ucryptolib as cryptolib
try:
    import esp32
except ImportError:
    esp32 = None

# ---------------------------------------------------------------------------
# Settings. Change these before uploading if you need to.
# ---------------------------------------------------------------------------
AP_SSID_PREFIX = "Johnify-"  # Wi-Fi name is this plus 4 characters from the board's MAC
AP_PASSWORD = ""             # Empty means an open network. Use 8 or more characters to set a password.
WIFI_CHANNEL = 1             # Must be the same on every board, or they will not hear each other.
MAX_HOPS = 4                 # How many repeaters a message may pass through.
MAX_ROOMS = 24               # Chats a single board remembers.
MAX_MSGS = 200               # Recent messages a board keeps in memory.
MAX_TEXT = 190               # Longest message in bytes. ESP-NOW packets are limited to 250 bytes.
ROOM_LEN = 10                # Length of a chat code.
SEEN_SIZE = 256              # How many recent message IDs are remembered to spot duplicates.
TXQ_SIZE = 16                # Packets waiting to be sent.
MAX_WEB_CLIENTS = 6          # Web connections handled at once.
LED_PIN = None               # Status LED pin, or None for no LED. XIAO ESP32S3: 21. Many ESP32 dev boards: 2.
LED_ACTIVE_LOW = True        # True if the LED lights when the pin is low (XIAO ESP32S3). False for most dev boards.
USE_WATCHDOG = True          # Reboot automatically if the program freezes. Turn off while developing over USB.
PAGE_FILE = "main.py"        # File the web page is read from. Leave as is unless you rename this file.

VERSION = "1.3.0-mp"
LOG_LINES = 16
HDR = 32
HDR_FMT = "<2sB8sII12sB"
BCAST = b"\xff" * 6
HEXCH = "0123456789abcdefABCDEF"
CODE_CHARS = "ABCDEFGHIJKLMNOPQRSTUVWXYZ0123456789"
LAST4 = (0x0000, 0x1C20, 0x3840, 0x2460, 0x7080, 0x6CA0, 0x48C0, 0x54E0,
         0xE100, 0xFD20, 0xD940, 0xC560, 0x9180, 0x8DA0, 0xA9C0, 0xB5E0)
REASON = {200: "OK", 204: "No Content", 302: "Found", 400: "Bad Request",
          404: "Not Found", 500: "Internal Server Error", 503: "Service Unavailable"}

boot_hex = "%08x" % int.from_bytes(os.urandom(4), "little")
t0 = time.time()
ssid = ""
ap_ip = "192.168.4.1"
ap = None
sta = None
en = None
nvs = None
led = None
led_off_at = None
restart_at = None
wdt = None
repeater_on = False
saved_rooms = ""
stat_rx = 0
stat_tx = 0
stat_relay = 0
stat_txfail = 0
stat_bad = 0
stat_auth = 0
stat_reinit = 0
rxdrop_base = 0
tx_streak = 0
radio_err_at = -100
low_mem_count = 0
min_free = 1 << 30
web_clients = 0
next_seq = 1
seen_pos = 0
log_head = 0
log_count = 0

rooms = {}
pending = []
RES = {}
msgs = []
seen_ring = [None] * SEEN_SIZE
seen_set = set()
txq = []
LOG = [""] * LOG_LINES


def uptime():
    return time.time() - t0


def log(msg):
    global log_head, log_count
    LOG[log_head] = ("%ds %s" % (uptime(), msg))[:72]
    log_head = (log_head + 1) % LOG_LINES
    log_count += 1
    print("[johnify]", msg)


def blink():
    global led_off_at
    if led is not None:
        led.value(0 if LED_ACTIVE_LOW else 1)
        led_off_at = time.ticks_add(time.ticks_ms(), 60)


def norm_room(s):
    s = s.strip().upper()
    if len(s) != ROOM_LEN:
        return None
    for c in s:
        if c not in CODE_CHARS:
            return None
    return s


def parse_hex8(s):
    if len(s) != 8:
        return None
    for c in s:
        if c not in HEXCH:
            return None
    return int(s, 16)


def to_int(s):
    try:
        return int(s)
    except ValueError:
        return 0


def clean(b, keep_newline):
    return bytes(32 if (c < 32 and not (keep_newline and c == 10)) else c for c in b)


def cut_utf8(b, n):
    if len(b) <= n:
        return b
    while n > 0 and (b[n] & 0xC0) == 0x80:
        n -= 1
    return b[:n]


def cstr(b):
    b = b.split(b"\x00")[0]
    try:
        return b.decode()
    except UnicodeError:
        return ""


def gcm_table(h):
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


def gcm_mul(m, x):
    z = 0
    for k in range(32):
        z = (z >> 4) ^ (LAST4[z & 15] << 112)
        z ^= m[(x >> (4 * k)) & 15]
    return z


def ghash(m, aad, ct):
    y = 0
    for data in (aad, ct):
        for i in range(0, len(data), 16):
            blk = data[i:i + 16]
            if len(blk) < 16:
                blk = blk + bytes(16 - len(blk))
            y = gcm_mul(m, y ^ int.from_bytes(blk, "big"))
    return gcm_mul(m, y ^ (((len(aad) * 8) << 64) | (len(ct) * 8)))


def gcm_ctr(room, nonce, data):
    out = []
    n = len(data)
    for i in range(0, n, 16):
        ks = room.aes.encrypt(nonce + struct.pack(">I", 2 + i // 16))
        chunk = data[i:i + 16]
        k = len(chunk)
        x = int.from_bytes(chunk, "big") ^ int.from_bytes(ks[:k], "big")
        out.append(x.to_bytes(k, "big"))
    return b"".join(out)


def gcm_tag(room, nonce, aad, ct):
    s = ghash(room.tbl, aad, ct)
    e = int.from_bytes(room.aes.encrypt(nonce + b"\x00\x00\x00\x01"), "big")
    return (e ^ s).to_bytes(16, "big")


def gcm_seal(room, nonce, pt, aad):
    ct = gcm_ctr(room, nonce, pt)
    return ct + gcm_tag(room, nonce, aad, ct)


def gcm_open(room, nonce, ct, tag, aad):
    if gcm_tag(room, nonce, aad, ct) != tag:
        return None
    return gcm_ctr(room, nonce, ct)


class Room:
    def __init__(self, code):
        self.code = code
        self.last = time.time()
        cb = code.encode()
        h = hashlib.sha256(b"johnify-v1|" + cb).digest()
        for _ in range(5000):
            h = hashlib.sha256(h + cb).digest()
        self.key = h
        self.gid = hashlib.sha256(h + b"gid").digest()[:8]
        self.aes = cryptolib.aes(h, 1)
        self.tbl = gcm_table(int.from_bytes(self.aes.encrypt(bytes(16)), "big"))


def make_aad(gid, msgid, devid):
    return gid + struct.pack("<II", msgid, devid)


def nvs_open():
    global nvs
    if esp32 is None:
        log("Storage unavailable, settings will not be saved")
        return
    try:
        nvs = esp32.NVS("johnify")
    except Exception:
        log("Storage unavailable, settings will not be saved")


def save_rooms():
    global saved_rooms
    s = ",".join(list(rooms) + [c for c in pending if c not in rooms])
    if s == saved_rooms or nvs is None:
        return
    try:
        nvs.set_blob("rooms", s.encode())
        nvs.commit()
        saved_rooms = s
    except Exception:
        log("Could not save chat list to flash")


def load_rooms():
    global saved_rooms
    if nvs is None:
        return
    try:
        buf = bytearray(400)
        n = nvs.get_blob("rooms", buf)
        saved_rooms = bytes(buf[:n]).decode()
    except Exception:
        return
    for part in saved_rooms.split(","):
        code = norm_room(part)
        if code and code not in pending and len(pending) < MAX_ROOMS:
            pending.append(code)


def ensure_room(code, evict):
    r = rooms.get(code)
    if r is not None:
        r.last = time.time()
        return r
    if len(rooms) >= MAX_ROOMS:
        if not evict:
            return None
        old = None
        for c in rooms:
            if old is None or rooms[c].last < rooms[old].last:
                old = c
        log("Chat list full, replacing %s..." % old[:5])
        del rooms[old]
    r = Room(code)
    rooms[code] = r
    save_rooms()
    return r


def store_msg(room, name, text, dev, mid):
    global next_seq
    if len(msgs) >= MAX_MSGS:
        del msgs[0]
    msgs.append((next_seq, mid, dev, time.time(), room, name, text))
    next_seq += 1


def seen_add(k):
    global seen_pos
    old = seen_ring[seen_pos]
    if old is not None:
        seen_set.discard(old)
    seen_ring[seen_pos] = k
    seen_set.add(k)
    seen_pos = (seen_pos + 1) % SEEN_SIZE


def enqueue(pkt, delay):
    if len(txq) >= TXQ_SIZE:
        return False
    txq.append([time.ticks_add(time.ticks_ms(), delay), pkt, 0])
    return True


def process_tx():
    global stat_tx, stat_txfail, tx_streak
    if not txq:
        return
    now = time.ticks_ms()
    for item in txq[:]:
        if time.ticks_diff(now, item[0]) < 0:
            continue
        try:
            en.send(BCAST, item[1], False)
            txq.remove(item)
            stat_tx += 1
            tx_streak = 0
            blink()
        except OSError as ex:
            item[2] += 1
            if item[2] >= 5:
                txq.remove(item)
                stat_txfail += 1
                tx_streak += 1
                log("Radio send failed (error %s)" % (ex.args[0] if ex.args else "?"))
            else:
                item[0] = time.ticks_add(now, 10)


def handle_packet(msg):
    global stat_rx, stat_bad, stat_auth, stat_relay
    n = len(msg)
    if n < HDR or n > 250:
        stat_bad += 1
        return
    magic, hops, gid, msgid, devid, nonce, tl = struct.unpack_from(HDR_FMT, msg, 0)
    if magic != b"JF" or tl > MAX_TEXT or n != HDR + 12 + tl + 16:
        stat_bad += 1
        return
    key = (devid << 32) | msgid
    if key in seen_set:
        return
    seen_add(key)
    stat_rx += 1
    blink()
    plen = 12 + tl
    for r in rooms.values():
        if r.gid != gid:
            continue
        pt = gcm_open(r, nonce, msg[HDR:HDR + plen], msg[HDR + plen:HDR + plen + 16], make_aad(gid, msgid, devid))
        if pt is not None:
            store_msg(r.code, cstr(pt[:12]), cstr(pt[12:12 + tl]), devid, msgid)
        else:
            stat_auth += 1
            if stat_auth % 50 == 1:
                log("Packet failed its security check (%d)" % stat_auth)
        break
    if repeater_on and hops < MAX_HOPS:
        c = bytearray(msg)
        c[2] = hops + 1
        if enqueue(bytes(c), 10 + os.urandom(1)[0] % 50):
            stat_relay += 1


def j(obj, code=200):
    return (code, json.dumps(obj), "application/json")


def fail(code, m):
    return (code, m, "text/plain")


def h_hello(q):
    dev = parse_hex8(q.get("dev", ""))
    if not dev:
        dev = 0
        while dev == 0:
            dev = int.from_bytes(os.urandom(4), "little")
    return j({"dev": "%08x" % dev, "ssid": ssid})


def h_join(q):
    code = norm_room(q.get("room", ""))
    if code is None:
        return fail(400, "bad room")
    ensure_room(code, True)
    return j({"ok": True})


def h_send(q):
    code = norm_room(q.get("room", ""))
    if code is None:
        return fail(400, "bad room")
    dev = parse_hex8(q.get("dev", ""))
    if dev is None:
        return fail(400, "bad dev")
    text = clean(q.get("text", "").encode(), True).strip()
    if not text:
        return fail(400, "empty")
    name = clean(q.get("name", "").encode(), False).strip()
    if not name:
        name = b"User"
    r = ensure_room(code, True)
    nb = cut_utf8(name, 12)
    tb = cut_utf8(text, MAX_TEXT)
    if not tb:
        return fail(400, "empty")
    pt = nb + bytes(12 - len(nb)) + tb
    msgid = int.from_bytes(os.urandom(4), "little")
    nonce = os.urandom(12)
    try:
        sealed = gcm_seal(r, nonce, pt, make_aad(r.gid, msgid, dev))
    except Exception as ex:
        log("Encryption failed (%s)" % ex)
        return fail(500, "crypto")
    pkt = struct.pack(HDR_FMT, b"JF", 0, r.gid, msgid, dev, nonce, len(tb)) + sealed
    if not enqueue(pkt, 0):
        log("Send queue full")
        return fail(503, "radio busy")
    enqueue(pkt, 30 + os.urandom(1)[0] % 40)
    seen_add((dev << 32) | msgid)
    store_msg(code, cstr(nb), cstr(tb), dev, msgid)
    return j({"ok": True, "i": "%08x" % msgid})


def h_poll(q):
    since = to_int(q.get("since", "0"))
    if q.get("boot", "") != boot_hex:
        since = 0
    if since < 0 or since > next_seq - 1:
        since = next_seq - 1
    codes = []
    for part in q.get("r", "").split(","):
        c = norm_room(part)
        if c is not None and c not in codes and ensure_room(c, False) is not None:
            codes.append(c)
        if len(codes) >= MAX_ROOMS:
            break
    first = since + 1
    if msgs and msgs[0][0] > first:
        first = msgs[0][0]
    last = first - 1
    out = []
    size = 0
    now = time.time()
    for m in msgs:
        if m[0] < first:
            continue
        if m[4] not in codes:
            last = m[0]
            continue
        if len(out) >= 30 or size > 4500:
            break
        out.append({"i": "%08x" % m[1], "d": "%08x" % m[2], "r": m[4], "n": m[5], "x": m[6], "a": now - m[3]})
        size += len(m[6]) + len(m[5]) + 90
        last = m[0]
    return j({"boot": boot_hex, "msgs": out, "next": last})


def h_status(q):
    global min_free
    free = gc.mem_free()
    if free < min_free:
        min_free = free
    try:
        clients = len(ap.status("stations"))
    except Exception:
        clients = 0
    try:
        drop = en.stats()[4] - rxdrop_base
    except Exception:
        drop = 0
    u = os.uname()
    return j({"ver": VERSION, "chip": u.machine[:40], "sdk": "MicroPython " + u.release,
              "repeater": repeater_on, "ssid": ssid, "ch": WIFI_CHANNEL, "hops": MAX_HOPS,
              "clients": clients, "rooms": len(rooms), "rx": stat_rx, "tx": stat_tx, "relayed": stat_relay,
              "rxDrop": drop, "txFail": stat_txfail, "bad": stat_bad, "auth": stat_auth, "reinit": stat_reinit,
              "heap": free, "minHeap": min_free, "up": uptime()})


def h_log(q):
    cnt = log_count if log_count < LOG_LINES else LOG_LINES
    return j({"log": [LOG[(log_head - cnt + i) % LOG_LINES] for i in range(cnt)]})


def h_repeater(q):
    global repeater_on
    repeater_on = q.get("on") == "1"
    if nvs is not None:
        try:
            nvs.set_i32("rep", 1 if repeater_on else 0)
            nvs.commit()
        except Exception:
            log("Could not save repeater setting")
    log("Repeater mode %s" % ("on" if repeater_on else "off"))
    return j({"repeater": repeater_on})


def h_leave(q):
    code = norm_room(q.get("room", ""))
    if code is None:
        return fail(400, "bad room")
    if code in rooms or code in pending:
        log("Left chat %s..." % code[:5])
    rooms.pop(code, None)
    if code in pending:
        pending.remove(code)
    msgs[:] = [m for m in msgs if m[4] != code]
    save_rooms()
    return j({"ok": True})


def h_reset(q):
    global repeater_on, next_seq, seen_pos, stat_rx, stat_tx, stat_relay, stat_txfail, stat_bad, stat_auth
    global stat_reinit, rxdrop_base, tx_streak, low_mem_count, min_free, log_head, log_count, saved_rooms, restart_at
    if q.get("__method") != "POST":
        return fail(405, "post only")
    if q.get("confirm") != "RESET":
        return fail(400, "confirm")
    rooms.clear()
    del pending[:]
    del msgs[:]
    del txq[:]
    seen_set.clear()
    for i in range(SEEN_SIZE):
        seen_ring[i] = None
    for i in range(LOG_LINES):
        LOG[i] = ""
    next_seq = 1
    seen_pos = 0
    log_head = 0
    log_count = 0
    stat_rx = stat_tx = stat_relay = stat_txfail = stat_bad = stat_auth = stat_reinit = 0
    tx_streak = 0
    low_mem_count = 0
    min_free = 1 << 30
    repeater_on = False
    try:
        rxdrop_base = en.stats()[4]
    except Exception:
        rxdrop_base = 0
    if nvs is not None:
        for key in ("rooms", "rep"):
            try:
                nvs.erase_key(key)
            except OSError:
                pass
        try:
            nvs.commit()
        except Exception:
            pass
    saved_rooms = ""
    restart_at = time.ticks_add(time.ticks_ms(), 800)
    return j({"ok": True})


ROUTES = {"/api/hello": h_hello, "/api/join": h_join, "/api/send": h_send, "/api/poll": h_poll,
          "/api/status": h_status, "/api/log": h_log, "/api/repeater": h_repeater,
          "/api/leave": h_leave, "/api/reset": h_reset}


def unquote(s):
    s = s.replace("+", " ")
    if "%" not in s:
        return s
    b = s.encode()
    out = bytearray()
    i = 0
    while i < len(b):
        c = b[i]
        if c == 37 and i + 2 < len(b):
            try:
                out.append(int(b[i + 1:i + 3].decode(), 16))
                i += 3
                continue
            except ValueError:
                pass
        out.append(c)
        i += 1
    try:
        return bytes(out).decode()
    except UnicodeError:
        return ""


def parse_qs(s):
    d = {}
    for pair in s.split("&"):
        if pair:
            k, _, v = pair.partition("=")
            d[unquote(k)] = unquote(v)
    return d


async def read_request(r):
    line = await r.readline()
    if not line or len(line) > 600:
        return None
    parts = line.split()
    if len(parts) < 2:
        return None
    method = parts[0].decode()
    target = parts[1].decode()
    clen = 0
    for _ in range(40):
        h = await r.readline()
        if h in (b"\r\n", b"\n", b""):
            break
        if h[:15].lower() == b"content-length:":
            try:
                clen = int(h[15:].strip())
            except ValueError:
                return None
    else:
        return None
    if clen < 0 or clen > 2048:
        return None
    path, _, query = target.partition("?")
    q = parse_qs(query)
    if clen:
        body = await r.readexactly(clen)
        try:
            q.update(parse_qs(body.decode()))
        except UnicodeError:
            pass
    return method, path, q


async def respond(w, code, body, ctype="text/plain", extra=""):
    b = body.encode() if isinstance(body, str) else body
    w.write(("HTTP/1.1 %d %s\r\nContent-Type: %s\r\nContent-Length: %d\r\nCache-Control: no-store\r\n%sConnection: close\r\n\r\n"
             % (code, REASON.get(code, "OK"), ctype, len(b), extra)).encode())
    if b:
        w.write(b)
    await w.drain()


def scan_resources():
    RES.clear()
    try:
        f = open(PAGE_FILE, "rb")
    except OSError:
        log("Cannot open %s, the web page will be missing" % PAGE_FILE)
        return
    try:
        name = kind = None
        start = size = 0
        while True:
            line = f.readline()
            if not line:
                break
            if line.startswith(b"#@"):
                if name:
                    RES[name] = (kind, start, size)
                parts = line[2:].split()
                name = parts[0].decode() if parts and parts[0] != b"end" else None
                kind = parts[1].decode() if len(parts) > 1 else None
                start = f.tell()
                size = 0
            elif name:
                size += len(line[2:].rstrip(b"\r\n")) + 1
        if name:
            RES[name] = (kind, start, size)
    except Exception as ex:
        log("Web page data is damaged (%s)" % ex)
        RES.clear()
    finally:
        f.close()


async def send_resource(w, name, ctype, cache):
    info = RES.get(name)
    if info is None:
        await respond(w, 500, "Johnify web page not found. Keep the complete main.py on the board.")
        return
    kind, start, size = info
    w.write(("HTTP/1.1 200 OK\r\nContent-Type: %s\r\nContent-Length: %d\r\nCache-Control: %s\r\nConnection: close\r\n\r\n"
             % (ctype, size, cache)).encode())
    f = open(PAGE_FILE, "rb")
    try:
        f.seek(start)
        buf = b""
        while True:
            line = f.readline()
            if not line or line.startswith(b"#@"):
                break
            buf += line[2:].rstrip(b"\r\n") + b"\n"
            if len(buf) >= 1024:
                w.write(buf)
                buf = b""
                await w.drain()
        if buf:
            w.write(buf)
            await w.drain()
    finally:
        f.close()


async def serve(r, w):
    req = await read_request(r)
    if req is None:
        await respond(w, 400, "bad request")
        return
    method, path, q = req
    if path == "/":
        await send_resource(w, "page", "text/html; charset=utf-8", "no-cache")
        return
    fn = ROUTES.get(path)
    if fn is None:
        if path == "/favicon.ico":
            await respond(w, 204, "")
        else:
            await respond(w, 302, "", extra="Location: http://%s/\r\n" % ap_ip)
        return
    try:
        q["__method"] = method
        code, body, ctype = fn(q)
    except Exception as ex:
        log("Handler error: %s" % ex)
        code, body, ctype = 500, "error", "text/plain"
    await respond(w, code, body, ctype)


async def client(r, w):
    global web_clients
    if web_clients >= MAX_WEB_CLIENTS:
        try:
            w.close()
        except Exception:
            pass
        return
    web_clients += 1
    try:
        await asyncio.wait_for(serve(r, w), 10)
    except OSError:
        pass
    except Exception as ex:
        if type(ex).__name__ != "TimeoutError":
            log("Request failed: %s" % ex)
    finally:
        web_clients -= 1
        try:
            w.close()
            await w.wait_closed()
        except Exception:
            pass


def dns_reply(q, ip):
    if len(q) < 12 or (q[2] & 0x80):
        return None
    i = 12
    while i < len(q) and q[i] != 0:
        i += q[i] + 1
    end = i + 5
    if end > len(q):
        return None
    qtype = (q[i + 1] << 8) | q[i + 2]
    head = q[0:2] + b"\x81\x80" + q[4:6] + (b"\x00\x01" if qtype == 1 else b"\x00\x00") + b"\x00\x00\x00\x00"
    r = head + q[12:end]
    if qtype == 1:
        r += b"\xc0\x0c\x00\x01\x00\x01\x00\x00\x00\x3c\x00\x04" + ip
    return r


async def dns_task():
    s = socket.socket(socket.AF_INET, socket.SOCK_DGRAM)
    s.setblocking(False)
    s.bind(("0.0.0.0", 53))
    ip = bytes(int(x) for x in ap_ip.split("."))
    while True:
        try:
            data, addr = s.recvfrom(256)
        except OSError:
            await asyncio.sleep_ms(30)
            continue
        try:
            reply = dns_reply(data, ip)
            if reply:
                s.sendto(reply, addr)
        except Exception:
            pass
        await asyncio.sleep_ms(0)


def start_now():
    global en
    if en is None:
        en = espnow.ESPNow()
        try:
            en.config(rxbuf=4096)
        except Exception:
            pass
    en.active(True)
    try:
        en.get_peer(BCAST)
    except OSError:
        en.add_peer(BCAST, channel=0, ifidx=network.STA_IF, encrypt=False)


async def radio_task():
    global led_off_at, radio_err_at
    while True:
        try:
            while True:
                mac, msg = en.recv(0)
                if mac is None:
                    break
                handle_packet(msg)
                await asyncio.sleep_ms(0)
            process_tx()
        except Exception as ex:
            if time.time() - radio_err_at >= 10:
                radio_err_at = time.time()
                log("Radio error: %s" % (ex.args[0] if ex.args else ex))
        if led_off_at is not None and time.ticks_diff(time.ticks_ms(), led_off_at) >= 0:
            led.value(1 if LED_ACTIVE_LOW else 0)
            led_off_at = None
        if restart_at is not None and time.ticks_diff(time.ticks_ms(), restart_at) >= 0:
            machine.reset()
        await asyncio.sleep_ms(5)


async def loader_task():
    while pending:
        code = pending.pop(0)
        if code not in rooms and len(rooms) < MAX_ROOMS:
            try:
                rooms[code] = Room(code)
            except Exception as ex:
                log("Could not load a saved chat (%s)" % ex)
        if wdt is not None:
            wdt.feed()
        await asyncio.sleep_ms(100)


async def health_task():
    global low_mem_count, min_free, tx_streak, stat_reinit
    while True:
        await asyncio.sleep(5)
        gc.collect()
        free = gc.mem_free()
        if free < min_free:
            min_free = free
        if free < 30000 and len(msgs) > 20:
            del msgs[:len(msgs) // 2]
            gc.collect()
            free = gc.mem_free()
            log("Memory low, dropped old messages")
        if free < 12000:
            low_mem_count += 1
            if low_mem_count >= 6:
                log("Memory very low, restarting")
                time.sleep_ms(200)
                machine.reset()
        else:
            low_mem_count = 0
        if tx_streak >= 5:
            tx_streak = 0
            stat_reinit += 1
            log("Radio keeps failing, restarting it")
            try:
                en.active(False)
                time.sleep_ms(50)
                start_now()
            except Exception:
                log("Radio restart failed, rebooting")
                time.sleep_ms(200)
                machine.reset()
        if wdt is not None:
            wdt.feed()


def setup():
    global ap, sta, ssid, ap_ip, led, repeater_on, wdt
    if LED_PIN is not None:
        try:
            led = machine.Pin(LED_PIN, machine.Pin.OUT)
            led.value(1 if LED_ACTIVE_LOW else 0)
        except Exception:
            led = None
            log("Could not use LED pin %s" % LED_PIN)
    log("Johnify %s starting" % VERSION)
    nvs_open()
    if nvs is not None:
        try:
            repeater_on = nvs.get_i32("rep") == 1
        except Exception:
            repeater_on = False
    load_rooms()
    sta = network.WLAN(network.STA_IF)
    sta.active(True)
    ap = network.WLAN(network.AP_IF)
    ap.active(True)
    mac = sta.config("mac")
    ssid = "%s%02X%02X" % (AP_SSID_PREFIX, mac[4], mac[5])
    if AP_PASSWORD and len(AP_PASSWORD) < 8:
        log("AP_PASSWORD under 8 characters, network left open")
    opts = {"essid": ssid, "channel": WIFI_CHANNEL}
    if len(AP_PASSWORD) >= 8:
        opts["authmode"] = network.AUTH_WPA2_PSK
        opts["password"] = AP_PASSWORD
    else:
        opts["authmode"] = network.AUTH_OPEN
    try:
        ap.config(max_clients=8, **opts)
    except (ValueError, OSError, TypeError):
        ap.config(**opts)
    ap.active(True)
    ap_ip = ap.ifconfig()[0]
    tuning = [{"channel": WIFI_CHANNEL}, {"txpower": 19.5}]
    pm_none = getattr(network.WLAN, "PM_NONE", None)
    if pm_none is not None:
        tuning.append({"pm": pm_none})
    for opts in tuning:
        try:
            sta.config(**opts)
        except Exception:
            pass
    try:
        start_now()
    except Exception as ex:
        log("Radio failed to start (%s), rebooting" % ex)
        time.sleep(2)
        machine.reset()
    log("Ready: %s, repeater %s, %d chats saved" % (ssid, "on" if repeater_on else "off", len(rooms)))
    print("[johnify] Open http://%s" % ap_ip)
    scan_resources()
    if USE_WATCHDOG:
        try:
            wdt = machine.WDT(timeout=30000)
        except Exception as ex:
            log("Watchdog unavailable (%s)" % ex)


async def main():
    asyncio.create_task(radio_task())
    asyncio.create_task(dns_task())
    asyncio.create_task(health_task())
    asyncio.create_task(loader_task())
    await asyncio.start_server(client, "0.0.0.0", 80)
    while True:
        await asyncio.sleep(3600)


if __name__ == "__main__":
    try:
        setup()
        asyncio.run(main())
    except Exception as ex:
        sys.print_exception(ex)
        print("[johnify] Fatal error, rebooting")
        time.sleep(3)
        machine.reset()


#@page text
#|<!DOCTYPE html>
#|<html lang="en"><head><meta charset="utf-8">
#|<meta name="viewport" content="width=device-width,initial-scale=1,viewport-fit=cover">
#|<meta name="color-scheme" content="light dark">
#|<meta name="theme-color" content="#12402f">
#|<title>Johnify</title>
#|<style>
#|:root{--paper:#efe9da;--card:#f9f5ea;--ink:#15130e;--mute:#6a6454;--rule:#d6cdb8;
#|  --pri:#12402f;--pritx:#f6f1e2;--acc:#e8431a;--acctx:#15130e;--acct:#b8330f;
#|  --serif:"Iowan Old Style","Palatino Linotype",Palatino,Georgia,serif;--sans:system-ui,-apple-system,"Segoe UI",sans-serif}
#|@media(prefers-color-scheme:dark){:root{--paper:#0f110d;--card:#171a14;--ink:#efe9d9;--mute:#9b9584;--rule:#2c2f25;
#|  --pri:#1d5c45;--pritx:#f4efdf;--acc:#ff5b2e;--acctx:#15130e;--acct:#ff7d55}}
#|*{box-sizing:border-box;-webkit-tap-highlight-color:transparent}
#|html,body{margin:0;height:100%}
#|body{display:flex;flex-direction:column;height:100dvh;background:var(--paper);color:var(--ink);font:500 16px/1.45 var(--sans)}
#|button,input,textarea{font:inherit;color:inherit}
#|svg{display:block}
#|:focus-visible{outline:3px solid var(--acc);outline-offset:2px}
#|header{flex:none;display:flex;align-items:center;gap:8px;padding:10px 14px;padding-top:max(10px,env(safe-area-inset-top));border-bottom:2px solid var(--ink)}
#|.mark{flex:1;min-width:0;display:flex;align-items:center;gap:8px}
#|.word{font:400 30px/1 var(--serif);letter-spacing:-.01em}
#|.dot{width:8px;height:8px;border-radius:50%;background:var(--mute)}
#|.dot.on{background:#2e8b57}.dot.off{background:var(--acc)}
#|.btn{border:2px solid var(--ink);background:transparent;border-radius:8px;padding:9px 12px;font:700 13px/1 var(--sans);cursor:pointer;white-space:nowrap}
#|.btn:active{transform:translateY(1px)}
#|.btn.pri{background:var(--pri);border-color:var(--pri);color:var(--pritx)}
#|.btn.warn{background:var(--acc);border-color:var(--acc);color:var(--acctx)}
#|.btn.wide{width:100%;padding:14px;font-size:14px}
#|.icon{flex:none;width:38px;height:38px;border:2px solid var(--ink);border-radius:8px;background:none;display:grid;place-items:center;cursor:pointer}
#|.icon.on{background:var(--ink);color:var(--paper)}
#|@media(max-width:380px){.word{font-size:28px}.btn{padding:9px 9px;font-size:12px}.icon{width:34px;height:34px}}
#|#warn{flex:none;padding:8px 14px;background:var(--acc);color:var(--acctx);font:700 13px/1.35 var(--sans)}
#|#warn[hidden]{display:none}
#|#view{flex:1;min-height:0;display:flex;flex-direction:column}
#|.scr{flex:1;min-height:0;display:flex;flex-direction:column}
#|.list{flex:1;overflow-y:auto;display:flex;flex-direction:column;padding:16px 14px 24px}
#|.lhead{display:flex;align-items:flex-end;justify-content:space-between;border-bottom:2px solid var(--ink);padding-bottom:6px}
#|.lhead h1{margin:0;font:400 50px/.9 var(--serif);letter-spacing:-.02em}
#|.lhead .n{font:800 13px var(--sans);letter-spacing:.1em;color:var(--acct)}
#|.row{display:grid;grid-template-columns:46px 1fr auto;gap:8px;align-items:center;padding:14px 0;border-bottom:1px solid var(--rule);cursor:pointer}
#|.row:active{background:var(--card)}
#|.idx{font:400 32px/1 var(--serif);color:var(--acct)}
#|.rt{min-width:0}
#|.rcode{font:800 17px var(--sans);letter-spacing:.1em}
#|.rprev{color:var(--mute);font-size:14px;white-space:nowrap;overflow:hidden;text-overflow:ellipsis}
#|.row.un .rprev{color:var(--ink);font-weight:700}
#|.rside{text-align:right;min-width:44px}
#|.rtime{font:600 12px var(--sans);color:var(--mute)}
#|.badge{display:inline-block;margin-top:4px;min-width:22px;padding:2px 7px;border-radius:4px;background:var(--acc);color:var(--acctx);font:800 12px var(--sans);text-align:center}
#|.empty{margin-top:auto;padding-top:20px}
#|.empty h1{margin:0 0 14px;font:400 clamp(48px,16vw,76px)/.9 var(--serif);letter-spacing:-.03em}
#|.lead{margin:0 0 24px;color:var(--mute);max-width:30ch}
#|.steps{list-style:none;margin:0;padding:0;border-top:2px solid var(--ink)}
#|.steps li{display:grid;grid-template-columns:54px 1fr;align-items:baseline;padding:12px 0;border-bottom:1px solid var(--rule)}
#|.steps b{font:400 40px/1 var(--serif);color:var(--acct)}
#|.steps span{font-weight:700}
#|.chead{flex:none;display:flex;align-items:center;gap:8px;padding:8px 12px 8px 6px;background:var(--pri);color:var(--pritx)}
#|.chead .icon{border-color:transparent;color:var(--pritx)}
#|.cmid{flex:1;min-width:0;display:flex;flex-direction:column;align-items:flex-start;background:none;border:0;padding:0;text-align:left;cursor:pointer}
#|.clab{font:800 10px var(--sans);letter-spacing:.16em;text-transform:uppercase;opacity:.75}
#|.ccode{font:800 19px/1.1 var(--sans);letter-spacing:.12em}
#|.leave{background:none;border:0;padding:6px 4px;font:700 13px var(--sans);text-decoration:underline;text-underline-offset:3px;cursor:pointer}
#|.encl{flex:none;display:flex;align-items:center;gap:6px;padding:6px 14px;border-bottom:1px solid var(--rule);font:700 11px var(--sans);letter-spacing:.04em;color:var(--mute)}
#|.msgs{flex:1;min-height:0;overflow-y:auto;display:flex;flex-direction:column;gap:8px;padding:14px}
#|.hint{margin:auto;color:var(--mute);font:400 24px var(--serif)}
#|.b{max-width:82%;padding:9px 12px 7px;border-radius:12px;word-break:break-word}
#|.b.g{margin-top:-5px}
#|.b.me{align-self:flex-end;background:var(--pri);color:var(--pritx);border-bottom-right-radius:3px}
#|.b.them{align-self:flex-start;background:var(--card);border:1.5px solid var(--ink);border-bottom-left-radius:3px}
#|.who{margin-bottom:2px;font:800 11px var(--sans);letter-spacing:.08em;text-transform:uppercase}
#|.tx{white-space:pre-wrap}
#|.tm{margin-top:2px;font:600 11px var(--sans);opacity:.6;text-align:right}
#|.comp{flex:none;display:flex;align-items:flex-end;gap:8px;padding:10px 12px;padding-bottom:max(10px,env(safe-area-inset-bottom));border-top:2px solid var(--ink)}
#|.inbox{flex:1;position:relative}
#|textarea{display:block;width:100%;min-height:44px;max-height:120px;resize:none;border:2px solid var(--ink);border-radius:8px;background:var(--card);padding:10px 12px;font:500 16px/1.35 var(--sans)}
#|.cnt{position:absolute;right:2px;top:-21px;font:800 11px var(--sans);color:var(--mute)}
#|.cnt.over{color:var(--acct)}
#|.send{flex:none;width:48px;height:44px;border:2px solid var(--ink);border-radius:8px;background:var(--acc);color:var(--acctx);display:grid;place-items:center;cursor:pointer}
#|.send:disabled{opacity:.5}
#|.page{flex:1;overflow-y:auto;padding:16px 14px 30px}
#|.ptitle{margin:0 0 14px;padding-bottom:6px;border-bottom:2px solid var(--ink);font:400 50px/.9 var(--serif);letter-spacing:-.02em}
#|.bento{display:grid;grid-template-columns:1.35fr 1fr;gap:10px}
#|.tile{min-width:0;padding:14px;border:2px solid var(--ink);border-radius:10px;background:var(--card)}
#|.tile.dark{background:var(--pri);border-color:var(--pri);color:var(--pritx)}
#|.tile.wide{grid-column:1/-1}
#|.tile.tall{grid-row:span 2;display:flex;flex-direction:column;justify-content:space-between}
#|.tile h3{margin:0 0 6px;font:400 28px/1 var(--serif)}
#|.tile p{margin:0 0 12px;font-size:14px;opacity:.85}
#|.tl{font:800 10px var(--sans);letter-spacing:.14em;text-transform:uppercase;opacity:.7}
#|.num{font:400 38px/1 var(--serif);letter-spacing:-.02em;overflow-wrap:anywhere}
#|.tall .num{font-size:58px}
#|.kv{display:flex;justify-content:space-between;gap:12px;padding:8px 0;border-top:1px solid var(--rule);font-size:14px}
#|.kv span{color:var(--mute)}
#|.kv b{font-weight:700;text-align:right;word-break:break-all}
#|.kv b.warnv{color:var(--acct)}
#|.rrow{display:flex;align-items:flex-start;justify-content:space-between;gap:14px}
#|.switch{position:relative;flex:none;width:60px;height:34px}
#|.switch input{position:absolute;inset:0;width:100%;height:100%;margin:0;opacity:0;cursor:pointer;z-index:1}
#|.knob{position:absolute;inset:0;border:2px solid currentColor;border-radius:8px}
#|.knob:after{content:"";position:absolute;top:4px;left:4px;width:22px;height:22px;border-radius:4px;background:currentColor;transition:transform .15s}
#|.switch input:checked+.knob{background:var(--acc);border-color:var(--acc);color:var(--acctx)}
#|.switch input:checked+.knob:after{transform:translateX(26px)}
#|.field{width:100%;margin:2px 0 12px;padding:6px 0;border:0;border-bottom:2px solid var(--ink);border-radius:0;background:none;font:700 18px var(--sans);outline:none}
#|.rep{width:100%;height:170px;margin-top:12px;font:500 12px/1.4 ui-monospace,Menlo,Consolas,monospace}
#|#modal,#cmodal{position:fixed;inset:0;display:flex;align-items:center;justify-content:center;padding:20px;background:rgba(10,9,6,.6)}
#|#modal[hidden],#cmodal[hidden]{display:none}
#|.box{width:100%;max-width:340px;padding:20px;border:2px solid var(--ink);border-radius:10px;background:var(--paper)}
#|.box h3{margin:0 0 4px;font:400 36px/1 var(--serif)}
#|.box p{margin:0;color:var(--mute);font-size:14px}
#|.box input{width:100%;margin:14px 0;padding:12px;border:2px solid var(--ink);border-radius:8px;background:var(--card);text-align:center;font:800 20px var(--sans);letter-spacing:.14em;text-transform:uppercase;outline:none}
#|.acts{display:flex;gap:8px}.acts .btn{flex:1;padding:13px}
#|#toast{position:fixed;left:50%;bottom:96px;transform:translateX(-50%);max-width:90%;padding:10px 16px;border-radius:6px;background:var(--ink);color:var(--paper);font:700 13px/1.35 var(--sans);text-align:center;opacity:0;pointer-events:none;transition:opacity .2s}
#|#toast.on{opacity:1}
#|.boot .rise{animation:rise .6s cubic-bezier(.2,.7,.2,1) both;animation-delay:calc(var(--i,0) * 60ms)}
#|@keyframes rise{from{opacity:0;transform:translateY(14px)}}
#|@media(prefers-reduced-motion:reduce){.boot .rise{animation:none}}
#|</style></head><body class="boot">
#|<header>
#| <div class="mark"><span class="word">Johnify</span><span id="dot" class="dot"></span></div>
#| <button id="bCreate" class="btn pri">Create chat</button>
#| <button id="bJoin" class="btn">Join</button>
#| <button id="bSet" class="icon" aria-label="Settings"></button>
#|</header>
#|<div id="warn" hidden role="alert"></div>
#|<main id="view"></main>
#|<div id="modal" hidden><div class="box" role="dialog" aria-modal="true" aria-labelledby="mt">
#| <h3 id="mt">Join a chat</h3>
#| <p>Type the 10-character code you were given.</p>
#| <input id="jc" maxlength="12" placeholder="CODE" autocapitalize="characters" autocomplete="off" spellcheck="false" aria-label="Chat code">
#| <div class="acts"><button id="jx" class="btn">Cancel</button><button id="jg" class="btn pri">Join</button></div>
#|</div></div>
#|<div id="cmodal" hidden><div class="box" role="alertdialog" aria-modal="true" aria-labelledby="ct" aria-describedby="cp">
#| <h3 id="ct"></h3>
#| <p id="cp"></p>
#| <div class="acts" style="margin-top:16px"><button id="cn" class="btn">Cancel</button><button id="cy" class="btn warn"></button></div>
#|</div></div>
#|<div id="toast" role="status"></div>
#|<script>
#|const $=s=>document.querySelector(s);
#|const el=(t,c,x)=>{const e=document.createElement(t);if(c)e.className=c;if(x!=null)e.textContent=x;return e};
#|const bytes=s=>new TextEncoder().encode(s).length;
#|const fmtUp=s=>{s=Math.max(0,s|0);const d=Math.floor(s/86400),h=Math.floor(s%86400/3600),m=Math.floor(s%3600/60);return(d?d+'d ':'')+h+'h '+m+'m'};
#|const fmtTime=t=>{try{return new Date(t).toLocaleTimeString([],{hour:'numeric',minute:'2-digit'})}catch(e){return''}};
#|const fmtCode=c=>c.slice(0,5)+' '+c.slice(5);
#|const ROOMRE=/^[A-Z0-9]{10}$/;
#|const ICON={
#|  back:'<path d="M15 5l-7 7 7 7"/>',
#|  lock:'<rect x="5" y="11" width="14" height="9" rx="2"/><path d="M8 11V8a4 4 0 0 1 8 0v3"/>',
#|  up:'<path d="M12 19V5M6 11l6-6 6 6"/>',
#|  sliders:'<path d="M4 7h9M17 7h3M4 17h3M11 17h9"/><circle cx="15" cy="7" r="2"/><circle cx="9" cy="17" r="2"/>'};
#|const svg=(n,s)=>'<svg viewBox="0 0 24 24" width="'+s+'" height="'+s+'" fill="none" stroke="currentColor" stroke-width="2.2" stroke-linecap="round" stroke-linejoin="round" aria-hidden="true">'+ICON[n]+'</svg>';
#|const DARK=matchMedia('(prefers-color-scheme:dark)');
#|const PAL=['#b8330f','#2f6b52','#8a6a12','#3c5a7a','#6b4a2b'],PALD=['#ff7d55','#6cc09a','#e0c060','#7fb0d8','#d0a070'];
#|const colorOf=s=>{let h=0;for(const c of s)h=(h*31+c.charCodeAt(0))>>>0;return(DARK.matches?PALD:PAL)[h%5]};
#|
#|let S={};
#|try{S=JSON.parse(localStorage.getItem('jf')||'{}')}catch(e){S={}}
#|(function(){
#|  if(!S||typeof S!=='object'||Array.isArray(S))S={};
#|  S.rooms=(Array.isArray(S.rooms)?S.rooms:[]).filter(c=>typeof c==='string'&&ROOMRE.test(c)).slice(0,12);
#|  if(!S.msgs||typeof S.msgs!=='object')S.msgs={};
#|  for(const k of Object.keys(S.msgs))if(!S.rooms.includes(k)||!Array.isArray(S.msgs[k]))delete S.msgs[k];
#|  if(!S.unread||typeof S.unread!=='object')S.unread={};
#|  S.since=Number.isInteger(S.since)&&S.since>=0?S.since:0;
#|  for(const k of['name','boot','dev','ssid'])if(typeof S[k]!=='string')S[k]='';
#|})();
#|let storeOK=true;
#|const save=()=>{try{localStorage.setItem('jf',JSON.stringify(S))}catch(e){
#|  if(storeOK){storeOK=false;setTimeout(()=>toast('This browser cannot save chats. History is lost when you close the page.'),0)}}};
#|
#|let cur=null,view='list',busy=false,again=false,fails=0,sending=false,stT=null;
#|const MSG={'bad room':'That chat code is not valid.','bad dev':'Your phone ID is missing. Reload the page.','empty':'Type a message first.',
#|  'crypto':'The board could not encrypt that message.','radio busy':'The radio is busy. Try again in a moment.'};
#|const why=e=>e&&e.net?(e.message==='timeout'?'The board is not answering.':'Cannot reach the board. Check your Wi-Fi.'):((e&&MSG[e.message])||'Board error: '+((e&&e.message)||'unknown'));
#|
#|function toast(t){const e=$('#toast');e.textContent=t;e.classList.add('on');clearTimeout(toast.t);toast.t=setTimeout(()=>e.classList.remove('on'),3000)}
#|function ask(title,text,okLabel){
#|  return new Promise(res=>{
#|    const m=$('#cmodal'),y=$('#cy'),n=$('#cn');
#|    $('#ct').textContent=title;$('#cp').textContent=text;y.textContent=okLabel;m.hidden=false;
#|    const done=v=>{m.hidden=true;y.onclick=n.onclick=m.onclick=null;res(v)};
#|    y.onclick=()=>done(true);n.onclick=()=>done(false);m.onclick=e=>{if(e.target===m)done(false)};
#|    n.focus()})}
#|function setLink(ok){
#|  $('#dot').className='dot '+(ok?'on':'off');$('#dot').title=ok?'Connected to the board':'Not connected';
#|  if(ok){fails=0;$('#warn').hidden=true;return}
#|  if(++fails>=2){const w=$('#warn');w.textContent='No connection to the board. Join the Wi-Fi network '+(S.ssid||'Johnify-XXXX')+' and keep this page open.';w.hidden=false}}
#|
#|async function api(p,o,post){
#|  const q=new URLSearchParams(o||{}),c=new AbortController(),t=setTimeout(()=>c.abort(),5000);
#|  try{
#|    const r=post?await fetch(p,{method:'POST',body:q,signal:c.signal}):await fetch(p+'?'+q,{signal:c.signal});
#|    if(!r.ok){const e=new Error((await r.text()).trim().slice(0,80)||'HTTP '+r.status);e.board=true;throw e}
#|    try{return await r.json()}catch(e){const n=new Error('offline');n.net=true;throw n}
#|  }catch(e){
#|    if(e.board||e.net)throw e;
#|    const n=new Error(e.name==='AbortError'?'timeout':'offline');n.net=true;throw n;
#|  }finally{clearTimeout(t)}}
#|
#|async function hello(){const h=await api('/api/hello',{dev:/^[0-9a-f]{8}$/.test(S.dev)?S.dev:''});S.dev=h.dev;S.ssid=h.ssid||S.ssid;save();return h}
#|function gen(){
#|  const A='ABCDEFGHJKMNPQRSTUVWXYZ23456789',a=new Uint8Array(10);
#|  if(window.crypto&&crypto.getRandomValues)crypto.getRandomValues(a);else for(let i=0;i<10;i++)a[i]=Math.random()*256;
#|  return[...a].map(x=>A[x%A.length]).join('')}
#|function copy(t){let ok=false;try{const a=document.createElement('textarea');a.value=t;document.body.appendChild(a);a.select();ok=document.execCommand('copy');a.remove()}catch(e){}
#|  toast(ok?'Code copied':'Code: '+t)}
#|
#|async function joinRoom(raw){
#|  const code=(raw||'').toUpperCase().replace(/[^A-Z0-9]/g,'');
#|  if(code.length!==10){toast('A chat code is 10 letters and numbers.');return null}
#|  if(!S.rooms.includes(code)&&S.rooms.length>=12){toast('You can have up to 12 chats. Leave one first.');return null}
#|  try{if(!S.dev)await hello();await api('/api/join',{room:code},true)}catch(e){toast(why(e));return null}
#|  if(!S.rooms.includes(code)){S.rooms.push(code);S.msgs[code]=S.msgs[code]||[];save()}
#|  try{const j=await api('/api/poll',{since:0,boot:'',r:code});ingest(j.msgs)}catch(e){}
#|  return code}
#|
#|function ingest(ms){
#|  if(!Array.isArray(ms))return;
#|  let changed=false;
#|  for(const m of ms){
#|    if(!m||typeof m.r!=='string'||typeof m.d!=='string'||typeof m.i!=='string'||!S.rooms.includes(m.r))continue;
#|    const L=S.msgs[m.r]=S.msgs[m.r]||[],id=m.d+m.i;
#|    if(L.some(x=>x.id===id))continue;
#|    L.push({id,dev:m.d,name:String(m.n||''),text:String(m.x||''),t:Date.now()-(Number(m.a)||0)*1000,mine:m.d===S.dev});
#|    L.sort((a,b)=>a.t-b.t);if(L.length>300)L.shift();
#|    changed=true;
#|    if(!(view==='chat'&&cur===m.r)&&m.d!==S.dev)S.unread[m.r]=(S.unread[m.r]||0)+1}
#|  if(changed){save();if(view==='chat')drawMsgs();else if(view==='list')render()}}
#|
#|async function poll(){
#|  if(busy){again=true;return}
#|  busy=true;
#|  try{
#|    if(!S.dev)await hello();
#|    const j=await api('/api/poll',{since:S.since,boot:S.boot,r:S.rooms.join(',')});
#|    if(typeof j.boot!=='string'||!Number.isInteger(j.next))throw Object.assign(new Error('offline'),{net:true});
#|    S.boot=j.boot;S.since=j.next;ingest(j.msgs);save();setLink(true)}
#|  catch(e){setLink(false)}
#|  busy=false;
#|  if(again){again=false;poll()}}
#|async function tick(){if(!document.hidden)await poll();setTimeout(tick,fails>=3?4000:1500)}
#|
#|async function send(){
#|  const box=$('#txt');if(!box||!cur||sending)return;
#|  const t=box.value.trim();if(!t)return;
#|  if(bytes(t)>190){toast('Too long. The limit is 190 bytes.');return}
#|  sending=true;const sb=$('#sendb');if(sb)sb.disabled=true;
#|  box.value='';box.style.height='auto';count();
#|  try{
#|    if(!S.dev)await hello();
#|    await api('/api/send',{room:cur,dev:S.dev,name:S.name||('User-'+S.dev.slice(0,4)),text:t},true);
#|    poll()}
#|  catch(e){box.value=t;count();toast(why(e))}
#|  sending=false;if(sb)sb.disabled=false}
#|function count(){
#|  const b=$('#txt'),c=$('#cnt');if(!b||!c)return;
#|  const left=190-bytes(b.value);
#|  c.hidden=left>40;c.textContent=left>=0?left+' left':(-left)+' too long';c.className='cnt'+(left<0?' over':'')}
#|
#|async function leaveRoom(code){
#|  S.rooms=S.rooms.filter(x=>x!==code);delete S.msgs[code];delete S.unread[code];save();
#|  if(view==='chat'&&cur===code)nav('list');
#|  for(let n=0;busy&&n<20;n++)await new Promise(r=>setTimeout(r,50));
#|  try{await api('/api/leave',{room:code},true);toast('You left the chat.')}
#|  catch(e){toast('You left the chat on this phone, but the board could not be reached.')}}
#|async function resetBoard(){
#|  if(!await ask('Reset this board?','This erases every chat, message and setting on this board and on this phone, then restarts the board. It cannot be undone.','Reset board'))return;
#|  try{await api('/api/reset',{confirm:'RESET'},true)}catch(e){toast(why(e));return}
#|  S.rooms=[];S.msgs={};S.unread={};S.since=0;S.boot='';S.dev='';S.name='';save();
#|  toast('Board reset. It restarts in a few seconds.');nav('list')}
#|function nav(v,c){
#|  if(v===view&&c===cur)return;
#|  try{history.pushState({v,c:c||null},'')}catch(e){}
#|  view=v;cur=c||null;if(v==='chat'){S.unread[c]=0;save()}render()}
#|function back(){if(history.state&&history.state.v&&history.state.v!=='list')history.back();else nav('list')}
#|addEventListener('popstate',e=>{
#|  const s=e.state||{v:'list'};view=s.v||'list';cur=s.c||null;
#|  if(view==='chat'&&!S.rooms.includes(cur)){view='list';cur=null}
#|  render()});
#|try{history.replaceState({v:'list',c:null},'')}catch(e){}
#|
#|function render(){
#|  $('#bSet').classList.toggle('on',view==='settings');
#|  clearInterval(stT);
#|  const v=$('#view');v.innerHTML='';
#|  if(view==='chat'&&!S.rooms.includes(cur)){view='list';cur=null}
#|  if(view==='list')drawList(v);else if(view==='chat')drawChat(v);else drawSettings(v)}
#|function backBtn(){const b=el('button','icon');b.innerHTML=svg('back',22);b.setAttribute('aria-label','Back');b.onclick=back;return b}
#|const rise=(e,i)=>{e.classList.add('rise');e.style.setProperty('--i',i);return e};
#|
#|function drawList(v){
#|  const scr=el('div','scr'),list=el('div','list');
#|  if(!S.rooms.length){
#|    const e=el('div','empty');
#|    e.append(rise(el('h1',0,'No chats yet.'),0),rise(el('p','lead','Johnify sends messages from board to board by radio. No internet, no router.'),1));
#|    const ol=el('ol','steps');
#|    ['Press Create chat to get a 10-character code.','Give the code to the other person.','They press Join and type it in.']
#|      .forEach((x,i)=>{const li=el('li');li.append(el('b',0,String(i+1)),el('span',0,x));ol.append(rise(li,i+2))});
#|    e.append(ol);list.append(e);scr.append(list);v.append(scr);return}
#|  const h=el('div','lhead');h.append(el('h1',0,'Chats'),el('span','n',String(S.rooms.length).padStart(2,'0')));
#|  list.append(rise(h,0));
#|  S.rooms.slice().reverse().forEach((code,i)=>{
#|    const L=S.msgs[code]||[],last=L[L.length-1],r=el('div','row'+(S.unread[code]?' un':'')),mid=el('div','rt'),side=el('div','rside');
#|    r.setAttribute('role','button');r.tabIndex=0;
#|    mid.append(el('div','rcode',fmtCode(code)),el('div','rprev',last?(last.mine?'You: ':'')+last.text:'No messages yet'));
#|    if(last)side.append(el('div','rtime',fmtTime(last.t)));
#|    if(S.unread[code])side.append(el('span','badge',S.unread[code]));
#|    r.append(el('div','idx',String(i+1).padStart(2,'0')),mid,side);
#|    r.onclick=()=>nav('chat',code);
#|    r.onkeydown=e=>{if(e.key==='Enter'||e.key===' '){e.preventDefault();nav('chat',code)}};
#|    list.append(rise(r,i+1))});
#|  scr.append(list);v.append(scr)}
#|
#|function drawChat(v){
#|  const scr=el('div','scr'),head=el('div','chead'),mid=el('button','cmid');
#|  mid.append(el('span','clab','Chat code'),el('span','ccode',fmtCode(cur)));mid.onclick=()=>copy(cur);mid.setAttribute('aria-label','Copy chat code');
#|  const lv=el('button','leave','Leave');
#|  lv.onclick=async()=>{const code=cur;
#|    if(await ask('Leave this chat?','It is removed from this phone, its saved messages are deleted, and the board stops keeping it. You can join again later with the code '+fmtCode(code)+'.','Leave'))leaveRoom(code)};
#|  head.append(backBtn(),mid,lv);
#|  const enc=el('div','encl');enc.innerHTML=svg('lock',12)+'<span>END-TO-END ENCRYPTED. TAP THE CODE TO COPY IT.</span>';
#|  const m=el('div','msgs');m.id='msgs';
#|  const f=el('div','comp'),ib=el('div','inbox'),t=el('textarea'),cn=el('span','cnt');
#|  t.id='txt';t.rows=1;t.placeholder='Write a message';t.setAttribute('aria-label','Message');cn.id='cnt';cn.hidden=true;
#|  t.oninput=()=>{t.style.height='auto';t.style.height=Math.min(t.scrollHeight,120)+'px';count()};
#|  t.onkeydown=e=>{if(e.key==='Enter'&&!e.shiftKey){e.preventDefault();send()}};
#|  const s=el('button','send');s.id='sendb';s.innerHTML=svg('up',22);s.setAttribute('aria-label','Send');s.onclick=send;
#|  ib.append(cn,t);f.append(ib,s);scr.append(head,enc,m,f);v.append(scr);drawMsgs(true)}
#|
#|function drawMsgs(force){
#|  const m=$('#msgs');if(!m)return;
#|  const stick=force||m.scrollHeight-m.scrollTop-m.clientHeight<90;
#|  m.innerHTML='';
#|  const L=S.msgs[cur]||[];
#|  if(!L.length)m.append(el('div','hint','Nothing here yet.'));
#|  let prev=null;
#|  for(const x of L){
#|    const grp=prev&&prev.dev===x.dev&&x.t-prev.t<120000;
#|    const b=el('div','b '+(x.mine?'me':'them')+(grp?' g':''));
#|    if(!x.mine&&!grp){const w=el('div','who',x.name||'?');w.style.color=colorOf(x.dev||'');b.append(w)}
#|    b.append(el('div','tx',x.text),el('div','tm',fmtTime(x.t)));
#|    m.append(b);prev=x}
#|  if(stick)m.scrollTop=m.scrollHeight}
#|
#|async function report(box){
#|  try{
#|    const[s,l]=await Promise.all([api('/api/status'),api('/api/log')]);
#|    box.value=['Johnify bug report','Firmware: '+s.ver+'   Chip: '+s.chip+'   SDK: '+s.sdk,
#|      'Uptime: '+fmtUp(s.up)+'   Channel: '+s.ch+'   Max hops: '+s.hops+'   Repeater: '+(s.repeater?'on':'off'),
#|      'Memory: '+Math.round(s.heap/1024)+' KB free, lowest '+Math.round(s.minHeap/1024)+' KB',
#|      'Packets: heard '+s.rx+', sent '+s.tx+', relayed '+s.relayed,
#|      'Problems: dropped '+s.rxDrop+', send failures '+s.txFail+', rejected '+s.bad+', failed checks '+s.auth+', radio resets '+s.reinit,
#|      'Phones: '+s.clients+'   Chats on board: '+s.rooms,'Browser: '+navigator.userAgent,'','Recent events:']
#|      .concat(l.log.length?l.log:['(none)']).join('\n');
#|    box.hidden=false;box.focus();box.select();
#|    let ok=false;try{ok=document.execCommand('copy')}catch(e){}
#|    toast(ok?'Bug report copied. Paste it where you need it.':'Could not copy automatically. Select the text and copy it.')
#|  }catch(e){toast(why(e))}}
#|
#|function drawSettings(v){
#|  const scr=el('div','scr'),head=el('div','chead'),ttl=el('div','cmid');
#|  ttl.append(el('span','clab','Johnify'),el('span','ccode','Settings'));ttl.style.cursor='default';ttl.lastChild.style.letterSpacing='.04em';
#|  head.append(backBtn(),ttl);
#|  const page=el('div','page'),grid=el('div','bento');
#|  page.append(rise(el('h1','ptitle','Settings'),0),grid);
#|
#|  const rep=el('div','tile dark wide'),rr=el('div','rrow'),rl=el('div');
#|  rl.append(el('h3',0,'Repeater'),el('p',0,'Re-sends every message this board hears from other boards, for any chat, to bridge boards too far apart to hear each other. A repeater cannot read what it passes on.'));
#|  const sw=el('label','switch'),cb=el('input');cb.type='checkbox';cb.setAttribute('aria-label','Repeater mode');sw.append(cb,el('span','knob'));
#|  cb.onchange=async()=>{try{await api('/api/repeater',{on:cb.checked?1:0},true);toast(cb.checked?'Repeater on':'Repeater off')}
#|    catch(e){cb.checked=!cb.checked;toast(why(e))}};
#|  rr.append(rl,sw);rep.append(rr);
#|
#|  const stat=(cls,label)=>{const t=el('div','tile '+cls),n=el('div','num','-');t.append(el('div','tl',label),n);return[t,n]};
#|  const[tHeard,nHeard]=stat('tall','Packets heard'),[tSent,nSent]=stat('','Sent'),[tRel,nRel]=stat('','Relayed');
#|
#|  const prof=el('div','tile wide');
#|  const nm=el('input','field');nm.placeholder='Your name';nm.maxLength=12;nm.value=S.name;nm.setAttribute('aria-label','Display name');
#|  nm.oninput=()=>{S.name=nm.value.trim();save()};
#|  const kvl=(k,x)=>{const d=el('div','kv');d.append(el('span',0,k),el('b',0,String(x)));return d};
#|  prof.append(el('div','tl','Profile'),nm,kvl('Device ID',S.dev||'-'));
#|
#|  const net=el('div','tile wide'),health=el('div','tile wide');
#|  net.append(el('h3',0,'This board'));health.append(el('h3',0,'Health'));
#|
#|  const rst=el('div','tile wide'),rb=el('button','btn wide warn','Reset board');
#|  rst.append(el('h3',0,'Start over'),el('p',0,'Erases every chat, message and setting on this board and on this phone, then restarts the board so it behaves like freshly uploaded firmware. Other phones keep their own copies.'),rb);
#|  rb.onclick=resetBoard;
#|  const bug=el('div','tile wide'),bt=el('button','btn wide','Copy bug report'),box=el('textarea','rep');
#|  box.readOnly=true;box.hidden=true;bt.onclick=()=>report(box);
#|  bug.append(bt,box);
#|
#|  [rep,tHeard,tSent,tRel,prof,net,health,bug,rst].forEach((t,i)=>grid.append(rise(t,i+1)));
#|  scr.append(head,page);v.append(scr);
#|
#|  const rows=(host,list)=>{[...host.querySelectorAll('.kv')].forEach(x=>x.remove());
#|    for(const[k,x,bad]of list){const d=kvl(k,x);if(bad)d.lastChild.className='warnv';host.append(d)}};
#|  const update=async()=>{
#|    try{
#|      const s=await api('/api/status');cb.checked=!!s.repeater;
#|      nHeard.textContent=s.rx;nSent.textContent=s.tx;nRel.textContent=s.relayed;
#|      rows(net,[['Wi-Fi network',s.ssid],['Phones connected',s.clients],['Chats stored',s.rooms],['Channel',s.ch],['Uptime',fmtUp(s.up)]]);
#|      rows(health,[['Firmware',s.ver],['Free memory',Math.round(s.heap/1024)+' KB',s.heap<30000],['Packets dropped (busy)',s.rxDrop,s.rxDrop>0],
#|        ['Failed sends',s.txFail,s.txFail>0],['Rejected packets',s.bad+s.auth],['Radio restarts',s.reinit,s.reinit>0]]);
#|    }catch(e){}};
#|  update();stT=setInterval(update,2500)}
#|
#|$('#bSet').innerHTML=svg('sliders',22);
#|$('#bSet').onclick=()=>{if(view==='settings')back();else nav('settings')};
#|$('#bCreate').onclick=async()=>{const c=await joinRoom(gen());if(c){nav('chat',c);toast('Chat created. Share the code.')}};
#|const openJoin=()=>{$('#modal').hidden=false;$('#jc').value='';$('#jc').focus()},closeJoin=()=>{$('#modal').hidden=true};
#|$('#bJoin').onclick=openJoin;
#|$('#jx').onclick=closeJoin;
#|$('#modal').onclick=e=>{if(e.target===$('#modal'))closeJoin()};
#|$('#jg').onclick=async()=>{const c=await joinRoom($('#jc').value);if(c){closeJoin();nav('chat',c)}};
#|$('#jc').onkeydown=e=>{if(e.key==='Enter')$('#jg').click();if(e.key==='Escape')closeJoin()};
#|addEventListener('keydown',e=>{if(e.key==='Escape'&&!$('#cmodal').hidden)$('#cn').click()});
#|document.addEventListener('visibilitychange',()=>{if(!document.hidden)poll()});
#|let errShown=false;
#|addEventListener('error',()=>{if(!errShown){errShown=true;toast('Something went wrong. Reload the page if it keeps happening.')}});
#|addEventListener('unhandledrejection',()=>{if(!errShown){errShown=true;toast('Something went wrong. Reload the page if it keeps happening.')}});
#|
#|render();setTimeout(()=>document.body.classList.remove('boot'),1400);
#|hello().catch(()=>{});tick();
#|</script></body></html>
#@end
