import sys
import os
import gc
import time
import json
import struct
import binascii
import socket
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
    import encryption as enc
except ImportError:
    print("[johnify] encryption.py is missing. Copy it to the board next to main.py.")
    raise
try:
    import esp32
except ImportError:
    esp32 = None
try:
    import esp
except ImportError:
    esp = None

# ---------------------------------------------------------------------------
# Settings. Change these before uploading if you need to.
# ---------------------------------------------------------------------------
AP_SSID_PREFIX = "Johnify-"  # Default Wi-Fi name: this plus 4 characters from the board's MAC. Can be changed in Settings.
AP_PASSWORD = ""             # Default password. Empty means an open network, otherwise 8 or more characters. Can be changed in Settings.
WIFI_CHANNEL = 1             # Default channel (1 to 11). Must be the same on every board. Can be changed in Settings.
MAX_HOPS = 4                 # How many repeaters a message may pass through.
MAX_ROOMS = 12               # Chats a single board remembers.
MAX_MSGS = 200               # Recent messages a board keeps in memory.
MAX_TEXT = 190               # Longest message in bytes. ESP-NOW packets are limited to 250 bytes.
ROOM_LEN = 10                # Length of a chat code.
SEEN_SIZE = 256              # How many recent message IDs are remembered to spot duplicates.
TXQ_SIZE = 16                # Packets waiting to be sent.
MAX_WEB_CLIENTS = 6          # Web connections handled at once.
CONSOLE_LINES = 60           # Developer console lines kept in memory while Developer mode is on.
LED_PIN = 21                 # Status LED pin, or None for no LED. XIAO ESP32S3: 21. Many ESP32 dev boards: 2.
LED_ACTIVE_LOW = True        # True if the LED lights when the pin is low (XIAO ESP32S3). False for most dev boards.
USE_WATCHDOG = True          # Reboot automatically if the program freezes. Turn off while developing over USB.
PAGE_FILE = "johnify.html"   # The web page file. Keep it on the board next to main.py.

VERSION = "1.4.0-mp"
DEV_LEASE_MS = 8000
SHOW = ("dev", "since", "boot", "on", "polls", "clear", "confirm")
NOISY = ("/api/poll", "/api/status", "/api/log", "/api/perf")
LOG_LINES = 16
HDR = 32
HDR_FMT = "<2sB8sII12sB"
BCAST = b"\xff" * 6
BEACON_FMT = "<2sBIB"        # 'JB', version 1, board ID, flags (bit 0 = repeater). 8 bytes.
BEACON_LEN = 8
BEACON_MS = 30000            # A board announces itself this often (plus up to 5 s of random delay).
BEACON_TTL_MS = 100000       # A board not heard from for this long is no longer counted as in range.
BEACON_GRACE_MS = 40000      # After start-up, "looking for boards" is shown for this long.
BEACON_MAX = 16              # Other boards remembered at once.
HEXCH = "0123456789abcdefABCDEF"
CODE_CHARS = "ABCDEFGHIJKLMNOPQRSTUVWXYZ0123456789"
REASON = {200: "OK", 204: "No Content", 302: "Found", 400: "Bad Request",
          404: "Not Found", 500: "Internal Server Error", 503: "Service Unavailable"}

boot_hex = "%08x" % int.from_bytes(os.urandom(4), "little")
beacon_id = int.from_bytes(os.urandom(4), "little")
near = {}
next_beacon = 0
t0 = time.time()
ssid = ""
def_ssid = ""
wifi_pass = AP_PASSWORD
wifi_ch = WIFI_CHANNEL
wifi_custom = False
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
msgs = []
seen_ring = [None] * SEEN_SIZE
seen_set = set()
txq = []
LOG = [""] * LOG_LINES
LOG_MS = [0] * LOG_LINES
CON = []
PK = []
clients = {}
sta_cur = {}
dev_on = False
dev_polls = False
dev_until = 0
dev_task_on = False
con_seq = 1
busy_us = 0
cpu_pct = 0
boot_ms = time.ticks_ms()


def uptime():
    return time.time() - t0


def now_ms():
    return time.ticks_diff(time.ticks_ms(), boot_ms)


def tag(gid):
    return binascii.hexlify(gid[:3]).decode()


def hx(b):
    return binascii.hexlify(b).decode()


def con(cat, text, hexdata=None, ms=None):
    global con_seq
    if not dev_on:
        return
    CON.append([con_seq, now_ms() if ms is None else ms, cat, text[:150], hexdata])
    if hexdata is not None:
        PK.append(con_seq)
        if len(PK) > 12:
            old = PK.pop(0)
            i = old - CON[0][0]
            if 0 <= i < len(CON) and CON[i][0] == old:
                CON[i][4] = None
    con_seq += 1
    if len(CON) > CONSOLE_LINES:
        del CON[0]


def log(msg):
    global log_head, log_count
    ms = now_ms()
    LOG[log_head] = ("%ds %s" % (uptime(), msg))[:72]
    LOG_MS[log_head] = ms
    log_head = (log_head + 1) % LOG_LINES
    log_count += 1
    print("[johnify]", msg)
    con("SYS", msg, None, ms)


def add_busy(us):
    global busy_us
    busy_us += us


def renew():
    global dev_until
    dev_until = time.ticks_add(time.ticks_ms(), DEV_LEASE_MS)


def dev_stop():
    global dev_on, dev_polls, busy_us, cpu_pct
    dev_on = False
    dev_polls = False
    del CON[:]
    del PK[:]
    clients.clear()
    sta_cur.clear()
    busy_us = 0
    cpu_pct = 0
    gc.collect()


def dev_start():
    global dev_on, busy_us, cpu_pct, dev_task_on
    if not dev_on:
        del CON[:]
        del PK[:]
        clients.clear()
        sta_cur.clear()
        busy_us = 0
        cpu_pct = 0
        dev_on = True
        n = log_count if log_count < LOG_LINES else LOG_LINES
        for i in range(n):
            idx = (log_head - n + i) % LOG_LINES
            text = LOG[idx]
            con("SYS", text.split(" ", 1)[1] if " " in text else text, None, LOG_MS[idx])
    renew()
    if not dev_task_on:
        dev_task_on = True
        asyncio.create_task(dev_task())


def fmt_mac(mac):
    return "%02x:%02x:%02x:xx:xx:xx" % (mac[0], mac[1], mac[2])


def near_count():
    now = time.ticks_ms()
    for k in [k for k in near if time.ticks_diff(now, near[k]) > BEACON_TTL_MS]:
        del near[k]
    if not near and now_ms() < BEACON_GRACE_MS:
        return -1
    return len(near)


def beacon_tick():
    global next_beacon
    now = time.ticks_ms()
    if time.ticks_diff(now, next_beacon) < 0:
        return
    next_beacon = time.ticks_add(now, BEACON_MS + int.from_bytes(os.urandom(2), "little") % 5000)
    enqueue(struct.pack(BEACON_FMT, b"JB", 1, beacon_id, 1 if repeater_on else 0), 0)


def handle_beacon(msg):
    global stat_bad
    n = len(msg)
    if n != BEACON_LEN:
        stat_bad += 1
        if dev_on:
            con("RX", "RX rejected: wrong beacon size (%d bytes)" % n, hx(msg))
        return
    _m, ver, bid, flags = struct.unpack(BEACON_FMT, msg)
    if ver != 1 or bid == beacon_id:
        return
    is_new = bid not in near
    if is_new and len(near) >= BEACON_MAX:
        oldest = None
        for k in near:
            if oldest is None or time.ticks_diff(near[oldest], near[k]) > 0:
                oldest = k
        del near[oldest]
    near[bid] = time.ticks_ms()
    if dev_on:
        con("RX", "RX beacon from board %08x%s%s" % (bid, " (repeater)" if flags & 1 else "", ", newly heard" if is_new else ""), hx(msg))


def track_stations():
    global sta_cur
    try:
        listing = ap.status("stations")
    except Exception:
        return
    cur = {}
    for item in listing:
        try:
            rssi = item[1] if len(item) > 1 else None
            if isinstance(rssi, bool) or not isinstance(rssi, (int, float)):
                rssi = None
            cur[bytes(item[0])] = int(rssi) if rssi is not None else None
        except Exception:
            pass
    for mac in cur:
        if mac not in sta_cur:
            con("WIFI", "device connected %s%s" % (fmt_mac(mac), " signal %d dBm" % cur[mac] if cur[mac] is not None else ""))
    for mac in sta_cur:
        if mac not in cur:
            con("WIFI", "device disconnected %s" % fmt_mac(mac))
    sta_cur = cur


async def dev_task():
    global dev_task_on, busy_us, cpu_pct
    last = time.ticks_us()
    try:
        while dev_on:
            await asyncio.sleep(1)
            if not dev_on:
                break
            if time.ticks_diff(time.ticks_ms(), dev_until) >= 0:
                dev_stop()
                break
            now = time.ticks_us()
            win = time.ticks_diff(now, last)
            last = now
            cpu_pct = min(100, busy_us * 100 // win) if win > 0 else 0
            busy_us = 0
            try:
                track_stations()
            except Exception as ex:
                log("Device tracking error: %s" % ex)
    finally:
        dev_task_on = False


def reset_name():
    try:
        rc = machine.reset_cause()
    except Exception:
        return "Other"
    for name, label in (("PWRON_RESET", "Power on"), ("HARD_RESET", "Reset button"), ("WDT_RESET", "Watchdog"),
                        ("SOFT_RESET", "Software restart"), ("DEEPSLEEP_RESET", "Deep sleep wake")):
        if getattr(machine, name, None) == rc:
            return label
    return "Other"


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


class Room:
    def __init__(self, code):
        t = time.ticks_ms() if dev_on else 0
        self.code = code
        self.last = time.time()
        self.key, self.gid = enc.derive(code)
        self.cipher = enc.Cipher(self.key)
        if dev_on:
            con("CRYPTO", "derive key for chat %s took %d ms (5000 rounds)" % (tag(self.gid), time.ticks_diff(time.ticks_ms(), t)))


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


def valid_ssid(s):
    b = s.encode()
    if len(b) < 1 or len(b) > 32 or '"' in s or "\\" in s:
        return False
    for c in b:
        if c < 32 or c == 127:
            return False
    return True


def valid_pass(s):
    if s == "":
        return True
    if len(s) < 8 or len(s) > 63:
        return False
    for c in s:
        if ord(c) < 32 or ord(c) > 126:
            return False
    return True


def nvs_text(key, size):
    buf = bytearray(size)
    n = nvs.get_blob(key, buf)
    return bytes(buf[:n]).decode()


def load_wifi():
    global ssid, wifi_pass, wifi_ch, wifi_custom
    if nvs is None:
        return
    try:
        if nvs.get_i32("wset") != 1:
            return
    except OSError:
        return
    try:
        n = nvs_text("wssid", 40)
        if valid_ssid(n):
            ssid = n
    except Exception:
        pass
    try:
        p = nvs_text("wpass", 70) if nvs.get_i32("wopen") != 1 else ""
        if valid_pass(p):
            wifi_pass = p
    except Exception:
        pass
    try:
        c = nvs.get_i32("wch")
        if 1 <= c <= 11:
            wifi_ch = c
    except Exception:
        pass
    wifi_custom = True
    log("Using saved Wi-Fi settings")


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
        log("Chat list full, replacing chat %s" % tag(rooms[old].gid))
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
    if not txq:
        return
    if not dev_on:
        _process_tx()
        return
    t = time.ticks_us()
    _process_tx()
    add_busy(time.ticks_diff(time.ticks_us(), t))


def _process_tx():
    global stat_tx, stat_txfail, tx_streak
    now = time.ticks_ms()
    for item in txq[:]:
        if time.ticks_diff(now, item[0]) < 0:
            continue
        try:
            en.send(BCAST, item[1], False)
            txq.remove(item)
            tx_streak = 0
            if item[1][:2] == b"JB":
                if dev_on:
                    con("TX", "TX beacon", hx(item[1]))
                continue
            stat_tx += 1
            blink()
            if dev_on:
                _m, h, gid, mid, dv, _n, ln = struct.unpack_from(HDR_FMT, item[1], 0)
                con("TX", "TX chat %s msg %08x dev %08x hops %d len %d" % (tag(gid), mid, dv, h, ln), hx(item[1]))
        except OSError as ex:
            item[2] += 1
            if item[2] >= 5:
                txq.remove(item)
                stat_txfail += 1
                tx_streak += 1
                log("Radio send failed (error %s)" % (ex.args[0] if ex.args else "?"))
            else:
                item[0] = time.ticks_add(now, 10)
                if dev_on:
                    con("TX", "TX busy (error %s), retry %d of 5" % (ex.args[0] if ex.args else "?", item[2]))


def handle_packet(msg):
    if not dev_on:
        _handle_packet(msg)
        return
    t = time.ticks_us()
    _handle_packet(msg)
    add_busy(time.ticks_diff(time.ticks_us(), t))


def _handle_packet(msg):
    global stat_rx, stat_bad, stat_auth, stat_relay
    n = len(msg)
    if msg[:2] == b"JB":
        handle_beacon(msg)
        return
    if n < HDR or n > 250:
        stat_bad += 1
        if dev_on:
            con("RX", "RX rejected: wrong size (%d bytes)" % n, hx(msg))
        return
    magic, hops, gid, msgid, devid, nonce, tl = struct.unpack_from(HDR_FMT, msg, 0)
    if magic != b"JF":
        stat_bad += 1
        if dev_on:
            con("RX", "RX rejected: not a Johnify packet (%d bytes)" % n, hx(msg))
        return
    if tl > MAX_TEXT or n != HDR + 12 + tl + 16:
        stat_bad += 1
        if dev_on:
            con("RX", "RX rejected: wrong length (%d bytes, text field %d)" % (n, tl), hx(msg))
        return
    key = (devid << 32) | msgid
    t = tag(gid) if dev_on else ""
    if key in seen_set:
        if dev_on:
            con("RX", "RX duplicate ignored  chat %s msg %08x dev %08x hops %d" % (t, msgid, devid, hops), hx(msg))
        return
    seen_add(key)
    stat_rx += 1
    blink()
    plen = 12 + tl
    result = "chat not known on this board"
    for r in rooms.values():
        if r.gid != gid:
            continue
        t0 = time.ticks_us() if dev_on else 0
        pt = r.cipher.open(nonce, msg[HDR:HDR + plen], msg[HDR + plen:HDR + plen + 16], enc.make_aad(gid, msgid, devid))
        if dev_on:
            con("CRYPTO", "open %s chat %s msg %08x %d B in %d us" % ("OK" if pt is not None else "FAILED", t, msgid, plen, time.ticks_diff(time.ticks_us(), t0)))
        if pt is not None:
            store_msg(r.code, cstr(pt[:12]), cstr(pt[12:12 + tl]), devid, msgid)
            result = "decrypted and stored"
            if dev_on:
                con("CHAT", "message stored for chat %s (%d bytes of text)" % (t, tl))
        else:
            stat_auth += 1
            result = "security check FAILED, dropped"
            if stat_auth % 50 == 1:
                log("Packet failed its security check (%d)" % stat_auth)
        break
    relayed = False
    if repeater_on and hops < MAX_HOPS:
        c = bytearray(msg)
        c[2] = hops + 1
        if enqueue(bytes(c), 10 + os.urandom(1)[0] % 50):
            stat_relay += 1
            relayed = True
    if dev_on:
        con("RX", "RX chat %s msg %08x dev %08x hops %d len %d: %s%s" % (t, msgid, devid, hops, tl, result, ", relay queued" if relayed else ""), hx(msg))


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
    r = ensure_room(code, True)
    if dev_on:
        con("CHAT", "phone joined chat %s" % tag(r.gid))
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
    t0 = time.ticks_us() if dev_on else 0
    try:
        sealed = r.cipher.seal(nonce, pt, enc.make_aad(r.gid, msgid, dev))
    except Exception as ex:
        log("Encryption failed (%s)" % ex)
        return fail(500, "crypto")
    pkt = struct.pack(HDR_FMT, b"JF", 0, r.gid, msgid, dev, nonce, len(tb)) + sealed
    if not enqueue(pkt, 0):
        log("Send queue full")
        return fail(503, "radio busy")
    enqueue(pkt, 30 + os.urandom(1)[0] % 40)
    if dev_on:
        con("CRYPTO", "seal chat %s msg %08x %d B text -> %d B packet in %d us" % (tag(r.gid), msgid, len(tb), len(pkt), time.ticks_diff(time.ticks_us(), t0)))
        con("CHAT", "message from a phone queued for the radio, sent twice: chat %s msg %08x (%d bytes of text)" % (tag(r.gid), msgid, len(tb)))
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
    return j({"boot": boot_hex, "msgs": out, "next": last, "near": near_count()})


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
              "repeater": repeater_on, "ssid": ssid, "ch": wifi_ch, "pw": len(wifi_pass) >= 8, "wcustom": wifi_custom, "hops": MAX_HOPS,
              "clients": clients, "rooms": len(rooms), "rx": stat_rx, "tx": stat_tx, "relayed": stat_relay,
              "rxDrop": drop, "txFail": stat_txfail, "bad": stat_bad, "auth": stat_auth, "reinit": stat_reinit,
              "near": near_count(), "heap": free, "minHeap": min_free, "up": uptime()})


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
    if code in rooms:
        tg = tag(rooms[code].gid)
        log("Left chat %s" % tg)
        con("CHAT", "phone left chat %s, board forgot it" % tg)
    elif code in pending:
        log("Left a saved chat that had not finished loading")
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
    dev_stop()
    rooms.clear()
    del pending[:]
    del msgs[:]
    del txq[:]
    near.clear()
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
        for key in ("rooms", "rep", "wset", "wssid", "wpass", "wopen", "wch"):
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


def h_wifi(q):
    global restart_at
    if q.get("__method") != "POST":
        return fail(405, "post only")
    if q.get("confirm") != "APPLY":
        return fail(400, "confirm")
    if nvs is None:
        return fail(500, "storage")
    if q.get("default") == "1":
        for key in ("wset", "wssid", "wpass", "wopen", "wch"):
            try:
                nvs.erase_key(key)
            except OSError:
                pass
        try:
            nvs.commit()
        except Exception:
            return fail(500, "storage")
        out = {"ok": True, "ssid": def_ssid, "ch": WIFI_CHANNEL, "pw": len(AP_PASSWORD) >= 8}
    else:
        n = q["ssid"].strip() if "ssid" in q else ssid
        p = q["pass"] if "pass" in q else wifi_pass
        c = to_int(q["ch"]) if "ch" in q else wifi_ch
        if not valid_ssid(n):
            return fail(400, "bad ssid")
        if not valid_pass(p):
            return fail(400, "bad pass")
        if c < 1 or c > 11:
            return fail(400, "bad channel")
        try:
            nvs.set_blob("wssid", n.encode())
            if p:
                nvs.set_blob("wpass", p.encode())
            nvs.set_i32("wopen", 0 if p else 1)
            nvs.set_i32("wch", c)
            nvs.set_i32("wset", 1)
            nvs.commit()
        except Exception:
            log("Could not save Wi-Fi settings")
            return fail(500, "storage")
        out = {"ok": True, "ssid": n, "ch": c, "pw": bool(p)}
    log("Wi-Fi settings changed, restarting")
    dev_stop()
    restart_at = time.ticks_add(time.ticks_ms(), 800)
    return j(out)


def h_dev(q):
    global dev_polls
    if q.get("__method") != "POST":
        return fail(405, "post only")
    if "on" in q:
        if q["on"] == "1":
            dev_start()
        else:
            dev_stop()
    if dev_on:
        renew()
        if "polls" in q:
            dev_polls = q["polls"] == "1"
        if q.get("clear") == "1":
            del CON[:]
            del PK[:]
    return j({"on": dev_on, "polls": dev_polls})


def h_console(q):
    since = to_int(q.get("since", "0"))
    if q.get("boot", "") != boot_hex:
        since = 0
    if not dev_on:
        return j({"on": False, "polls": False, "boot": boot_hex, "lost": 0, "lines": [], "next": since, "more": False})
    renew()
    newest = con_seq - 1
    if since < 0 or since > newest:
        since = 0
    first = since + 1
    window = newest - CONSOLE_LINES + 1 if newest > CONSOLE_LINES else 1
    lost = 0
    if first < window:
        lost = window - first
        first = window
    last = first - 1
    out = []
    size = 0
    for l in CON:
        if l[0] < first:
            continue
        if len(out) >= 40 or size > 6000:
            break
        d = {"s": l[0], "t": l[1], "c": l[2], "x": l[3]}
        size += len(l[3]) + 60
        if l[4]:
            d["h"] = l[4]
            size += len(l[4])
        out.append(d)
        last = l[0]
    more = bool(CON) and CON[-1][0] > last
    return j({"on": True, "polls": dev_polls, "boot": boot_hex, "lost": lost, "lines": out, "next": last, "more": more})


def h_perf(q):
    if not dev_on:
        return j({"on": False})
    renew()
    free = gc.mem_free()
    alloc = gc.mem_alloc()
    temp = None
    try:
        temp = esp32.mcu_temperature()
    except Exception:
        temp = None
    fs_total = fs_free = None
    try:
        st = os.statvfs("/")
        fs_total = st[1] * st[2]
        fs_free = st[1] * st[3]
    except Exception:
        pass
    flash = None
    try:
        flash = esp.flash_size()
    except Exception:
        pass
    now = time.time()
    cl = [{"ip": ip, "age": now - clients[ip][0], "n": clients[ip][1]} for ip in clients]
    sta = [{"mac": fmt_mac(m), "rssi": sta_cur[m]} for m in sta_cur]
    u = os.uname()
    try:
        mhz = machine.freq() // 1000000
    except Exception:
        mhz = None
    return j({"on": True, "temp": temp, "cpu": cpu_pct, "mhz": mhz, "cores": None, "chip": u.machine[:40],
              "heapTotal": free + alloc, "heapFree": free, "heapMax": None, "psramTotal": None, "psramFree": None,
              "flash": flash, "fsTotal": fs_total, "fsFree": fs_free, "up": uptime(), "reset": reset_name(),
              "sta": sta, "clients": cl})


ROUTES = {"/api/hello": h_hello, "/api/join": h_join, "/api/send": h_send, "/api/poll": h_poll,
          "/api/status": h_status, "/api/log": h_log, "/api/repeater": h_repeater,
          "/api/leave": h_leave, "/api/reset": h_reset, "/api/wifi": h_wifi,
          "/api/dev": h_dev, "/api/console": h_console, "/api/perf": h_perf}


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


async def send_page(w):
    try:
        f = open(PAGE_FILE, "rb")
    except OSError:
        log("Cannot open %s, the web page is missing" % PAGE_FILE)
        msg = "Johnify web page not found. Copy johnify.html to the board."
        await respond(w, 500, msg)
        return 500, len(msg)
    try:
        size = os.stat(PAGE_FILE)[6]
        w.write(("HTTP/1.1 200 OK\r\nContent-Type: text/html; charset=utf-8\r\nContent-Length: %d\r\nCache-Control: no-cache\r\nConnection: close\r\n\r\n"
                 % size).encode())
        while True:
            chunk = f.read(1024)
            if not chunk:
                break
            w.write(chunk)
            await w.drain()
        return 200, size
    finally:
        f.close()


def peer_of(w):
    try:
        p = w.get_extra_info("peername")
        return str(p[0]) if isinstance(p, tuple) else str(p)
    except Exception:
        return "?"


def note_client(ip):
    c = clients.get(ip)
    if c is None:
        if len(clients) >= 8:
            oldest = None
            for k in clients:
                if oldest is None or clients[k][0] < clients[oldest][0]:
                    oldest = k
            del clients[oldest]
        c = clients[ip] = [0, 0]
    c[0] = time.time()
    c[1] += 1


def req_args(q):
    parts = []
    for k in q:
        if k == "__method":
            continue
        v = q[k]
        parts.append("%s=%s" % (k, v[:12]) if k in SHOW else "%s=<%dB>" % (k, len(v.encode())))
    return "&".join(parts)


async def serve(r, w):
    on = dev_on
    t0 = time.ticks_us() if on else 0
    req = await read_request(r)
    if req is None:
        await respond(w, 400, "bad request")
        return
    method, path, q = req
    code = 200
    size = 0
    work = 0
    if path == "/":
        code, size = await send_page(w)
    else:
        fn = ROUTES.get(path)
        if fn is None:
            if path == "/favicon.ico":
                code = 204
                await respond(w, 204, "")
            else:
                code = 302
                await respond(w, 302, "", extra="Location: http://%s/\r\n" % ap_ip)
        else:
            q["__method"] = method
            w0 = time.ticks_us() if on else 0
            try:
                code, body, ctype = fn(q)
            except Exception as ex:
                log("Handler error: %s" % ex)
                code, body, ctype = 500, "error", "text/plain"
            if on:
                work = time.ticks_diff(time.ticks_us(), w0)
            size = len(body)
            await respond(w, code, body, ctype)
    if on and dev_on:
        add_busy(work)
        if path != "/api/console":
            ip = peer_of(w)
            note_client(ip)
            if dev_polls or path not in NOISY:
                us = time.ticks_diff(time.ticks_us(), t0)
                args = req_args(q)
                con("HTTP", "%s %s %s%s%s -> %d %dB %d.%d ms" % (ip, method, path, "?" if args else "", args, code, size, us // 1000, (us % 1000) // 100))


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
            beacon_tick()
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
                t = time.ticks_us() if dev_on else 0
                rooms[code] = Room(code)
                if dev_on:
                    add_busy(time.ticks_diff(time.ticks_us(), t))
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
    global ap, sta, ssid, def_ssid, ap_ip, led, repeater_on, wdt, next_beacon
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
    def_ssid = ssid
    load_wifi()
    if wifi_pass and len(wifi_pass) < 8:
        log("Wi-Fi password under 8 characters, network left open")
    opts = {"essid": ssid, "channel": wifi_ch}
    if len(wifi_pass) >= 8:
        opts["authmode"] = network.AUTH_WPA2_PSK
        opts["password"] = wifi_pass
    else:
        opts["authmode"] = network.AUTH_OPEN
    try:
        ap.config(max_clients=8, **opts)
    except (ValueError, OSError, TypeError):
        ap.config(**opts)
    ap.active(True)
    ap_ip = ap.ifconfig()[0]
    tuning = [{"channel": wifi_ch}, {"txpower": 19.5}]
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
    next_beacon = time.ticks_add(time.ticks_ms(), 2000 + int.from_bytes(os.urandom(2), "little") % 3000)
    log("Ready: %s, repeater %s, %d chats saved" % (ssid, "on" if repeater_on else "off", len(rooms)))
    log("Open http://%s" % ap_ip)
    try:
        os.stat(PAGE_FILE)
    except OSError:
        log("Cannot find %s. Copy johnify.html to the board." % PAGE_FILE)
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
