import sys
import os
import gc
import time
import json
import struct
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

# ---------------------------------------------------------------------------
# Settings. Change these before uploading if you need to.
# ---------------------------------------------------------------------------
AP_SSID_PREFIX = "Johnify-Lite"  # Wi-Fi name is this plus 4 characters from the board's MAC
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
PAGE_FILE = "johnify.html"   # The web page file. Keep it on the board next to main.py.

VERSION = "Johnify-Lite"
LOG_LINES = 16
HDR = 32
HDR_FMT = "<2sB8sII12sB"
BCAST = b"\xff" * 6
HEXCH = "0123456789abcdefABCDEF"
CODE_CHARS = "ABCDEFGHIJKLMNOPQRSTUVWXYZ0123456789"
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


class Room:
    def __init__(self, code):
        self.code = code
        self.last = time.time()
        self.key, self.gid = enc.derive(code)
        self.cipher = enc.Cipher(self.key)


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
        pt = r.cipher.open(nonce, msg[HDR:HDR + plen], msg[HDR + plen:HDR + plen + 16], enc.make_aad(gid, msgid, devid))
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
        sealed = r.cipher.seal(nonce, pt, enc.make_aad(r.gid, msgid, dev))
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


async def send_page(w):
    try:
        f = open(PAGE_FILE, "rb")
    except OSError:
        log("Cannot open %s, the web page is missing" % PAGE_FILE)
        await respond(w, 500, "Johnify web page not found. Copy johnify.html to the board.")
        return
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
    finally:
        f.close()


async def serve(r, w):
    req = await read_request(r)
    if req is None:
        await respond(w, 400, "bad request")
        return
    method, path, q = req
    if path == "/":
        await send_page(w)
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
