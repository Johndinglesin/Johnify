# Johnify

An encrypted group chat that runs on ESP32 boards with no internet, router, or cell service.

Each board runs its own Wi-Fi hotspot and serves a small chat web app. Phones connect to the nearest board with a browser. Boards pass messages to each other over ESP-NOW, a connectionless 2.4 GHz protocol built into the ESP32. Messages are encrypted with AES-256-GCM using a key derived from a shared chat code. Any board can also be switched into repeater mode to extend range.

Written for the Seeed Studio **XIAO ESP32S3**. It should work on other ESP32-family boards with ESP-NOW and mbedTLS, but only the XIAO is targeted.

---

## Contents

- [Features](#features)
- [Quick start](#quick-start)
- [Using Johnify](#using-johnify)
- [Architecture](#architecture)
- [Packet format](#packet-format)
- [Cryptography](#cryptography)
- [Duplicate suppression and repeaters](#duplicate-suppression-and-repeaters)
- [Radio notes](#radio-notes)
- [HTTP API](#http-api)
- [Web client](#web-client)
- [Configuration](#configuration)
- [Memory and limits](#memory-and-limits)
- [Security model](#security-model)
- [Troubleshooting](#troubleshooting)
- [Repository layout](#repository-layout)

---

## Features

- No infrastructure. Boards talk to each other directly; phones talk only to their local board.
- Chat rooms identified by a 10-character code. Anyone with the code can join. Anyone without it cannot read the traffic.
- End-to-end encryption: AES-256-GCM, with the key derived from the chat code. The key never leaves the board and the phone, and is never transmitted.
- Duplicate suppression: the same message arriving twice (retransmits, repeaters) is shown once.
- Repeater mode: a board can re-broadcast any message it hears, for any chat, up to a hop limit. Repeaters do not need to know the chat code and cannot read the messages they relay.
- Chat history is kept in the phone's browser storage. Chat codes are kept in the board's flash. Recent messages are kept in the board's RAM.
- Captive-portal behaviour: phones prompt "sign in to network" and any URL typed while connected opens the app.

## Quick start

### Hardware

- One or more Seeed XIAO ESP32S3 boards, each powered over USB-C or a LiPo battery. Two boards are the minimum to test; each additional board is another chat node or a repeater.

### Arduino IDE

1. Install the **esp32 by Espressif Systems** board package (3.x recommended; 2.x is also handled).
2. Open `Johnify/Johnify.ino`. It is the only source file.
3. Board: **XIAO_ESP32S3**.
4. Optional: set **USB CDC On Boot** to *Enabled* if you want serial output over the USB port.
5. Upload the same sketch to every board.

No third-party libraries are required. `WiFi`, `WebServer`, `DNSServer`, `Preferences`, `esp_now`, and `mbedtls` all ship with the board package.

### arduino-cli

```sh
arduino-cli core install esp32:esp32
arduino-cli compile --fqbn esp32:esp32:XIAO_ESP32S3 Johnify
arduino-cli upload  --fqbn esp32:esp32:XIAO_ESP32S3 -p /dev/ttyACM0 Johnify
```

Replace the port with your own (`COMx` on Windows).

## Using Johnify

1. On your phone, join the Wi-Fi network named `Johnify-XXXX` (the last four hex digits of the board's MAC). It is open by default.
2. Open a browser and go to `http://192.168.4.1`. Any address typed while connected also redirects there. If your phone shows a "sign in to network" prompt, it opens the same page, but a full browser is better because its storage persists.
3. Press **Create chat**. You get a 10-character code.
4. Give the code to the other person. They connect to their own board, press **Join**, and enter it.
5. Messages sent by either person appear on the other's phone.
6. The **Repeater** page has a switch to make that board relay traffic, plus live counters.

If the page does not load, turn off mobile data on the phone. Some phones route browser traffic over cellular when the Wi-Fi network has no internet.

## Architecture

```
  Phone A                Board A                         Board B                Phone B
 ┌────────┐  HTTP     ┌──────────────┐   ESP-NOW      ┌──────────────┐  HTTP  ┌────────┐
 │ browser│──────────▶│ web server   │   broadcast    │ web server   │◀───────│ browser│
 │  app   │◀──────────│ encrypt/     │───────────────▶│ decrypt/     │───────▶│  app   │
 │        │  poll     │ decrypt      │   (ch. 1)      │ encrypt      │  poll  │        │
 └────────┘           │ dedup, store │◀───────────────│ dedup, store │        └────────┘
                      └──────┬───────┘                └──────────────┘
                             │ optional
                             ▼
                      ┌──────────────┐
                      │ Board C      │   repeater: re-broadcasts what it hears,
                      │ (repeater)   │   cannot read chats it does not know
                      └──────────────┘
```

Each board runs both Wi-Fi modes at once (`WIFI_AP_STA`):

- The **access point** serves phones.
- The **station** interface is never connected to a network. ESP-NOW uses it as the radio path for sending.

The main loop is single-threaded. It services DNS, HTTP, the receive queue, and the transmit queue in turn. The only code that runs outside the loop is the ESP-NOW receive callback, which only copies the packet into a queue.

### Life of a message

1. The phone `POST`s `/api/send` with room code, device ID, name, and text.
2. The board derives (or looks up) the room's key and group ID, generates a random message ID and nonce, and builds a packet.
3. The name and text are encrypted with AES-256-GCM. The packet header is authenticated as additional data.
4. The message ID is added to the board's seen-list, and the decrypted message is stored in the board's message ring so other phones on this same board can read it.
5. The packet is queued twice: immediately, and again after a random 30–70 ms delay.
6. Other boards receive it. The receive callback copies it to a queue. The loop pulls it out and checks magic bytes and length.
7. If its `(devId, msgId)` was seen before, it is dropped. Otherwise it is recorded.
8. If a stored room's group ID matches, the board decrypts the body. If the tag verifies, the message is stored in the ring.
9. If repeater mode is on and `hops < MAX_HOPS`, the board queues a copy with `hops + 1` after a random 10–60 ms delay.
10. Phones poll their board every 1.5 s and pick up new messages.

## Packet format

ESP-NOW payloads are limited to 250 bytes. Johnify packets are packed structs, little-endian (native to the ESP32), with a 32-byte header followed by the encrypted body.

| Offset | Size | Field | Notes |
|-------:|-----:|-------|-------|
| 0 | 2 | `magic` | ASCII `JF`. Used to ignore unrelated ESP-NOW traffic. |
| 2 | 1 | `hops` | Incremented by each repeater. Not authenticated. |
| 3 | 8 | `gid` | Group ID: first 8 bytes of SHA-256(key ‖ `"gid"`). |
| 11 | 4 | `msgId` | Random per message. |
| 15 | 4 | `devId` | Random ID assigned to the sending phone by its board. |
| 19 | 12 | `nonce` | Random AES-GCM nonce, generated per message. |
| 31 | 1 | `len` | Text length in bytes (0–190). |
| 32 | 12 + `len` | ciphertext | AES-256-GCM of `name[12]` ‖ `text[len]`. The name is zero-padded to 12 bytes. |
| 44 + `len` | 16 | tag | GCM authentication tag. |

Total size is `60 + len` bytes, at most 250.

Fields in the header are readable by anyone listening. The sender's display name and the message text are inside the ciphertext.

## Cryptography

### Key derivation

```
seed = SHA-256( "johnify-v1|" ‖ code )
h    = seed
repeat 5000 times:  h = SHA-256( h ‖ code )
key  = h                                  (32 bytes)
gid  = SHA-256( key ‖ "gid" )[0..8]       (8 bytes)
```

The key and group ID are computed once per chat when the board first sees the code (or at boot for saved chats), then cached in RAM. The 5000-round loop takes well under a second on the ESP32-S3.

This is an iterated hash, not a standard password KDF such as PBKDF2 or Argon2. It was chosen because it needs only the one-shot SHA-256 call, which is stable across mbedTLS versions. See the security model for what that means in practice.

### Chat codes

Codes are 10 characters from a 31-character alphabet (`A–Z` and `2–9` without `I`, `L`, `O`, `0`, `1`). That is about 49.5 bits of entropy when generated by the app. Generation uses `crypto.getRandomValues` and reduces each byte modulo 31, which has a very small bias that does not materially affect strength.

### Encryption

- Cipher: AES-256-GCM via mbedTLS.
- Nonce: 12 random bytes from the ESP32 hardware RNG (`esp_fill_random`), per message. The nonce is sent in the clear.
- Additional authenticated data (16 bytes): `gid` ‖ `msgId` ‖ `devId`. Changing any of these makes decryption fail. `hops` is left out because repeaters must change it.
- A packet is only accepted if the GCM tag verifies. Forged or modified packets are dropped silently.

### Group ID

The group ID lets a board find the right key without trying every key. It is a one-way hash of the key, so it does not reveal the code or the key. An attacker who captures a packet can still use the group ID or the tag to check guesses offline; see below.

### Why ESP-NOW's built-in encryption is not used

ESP-NOW has its own CCMP encryption, but it only works for unicast peers with pre-shared keys per pair. Broadcast cannot use it, and broadcast is how this project reaches every board nearby without pairing. Johnify therefore encrypts at the application layer.

## Duplicate suppression and repeaters

Every message is identified by the pair `(devId, msgId)`, packed into a 64-bit key. Each board holds the last `SEEN_SIZE` (256) keys in a ring buffer.

A packet is handled in this order:

1. Check the key against the ring. If found, drop it.
2. Otherwise add the key to the ring. This happens before any decryption, so repeaters, which cannot decrypt, also stop duplicates.
3. Decrypt if the chat is known.
4. Relay if repeater mode is on.

A board that sends a message adds its own key to the ring first, so a repeater echoing the message back is ignored.

Loop and storm protection in a repeater network comes from three things working together:

- the duplicate ring,
- the `MAX_HOPS` limit (default 4),
- a random 10–60 ms delay before relaying, which reduces collisions when several repeaters hear the same packet.

Originating boards send each message twice, 30–70 ms apart, because broadcast frames are not acknowledged. The second copy is dropped by every receiver that got the first.

## Radio notes

- **Channel.** All boards must use the same Wi-Fi channel (`WIFI_CHANNEL`, default 1). The access point fixes the radio to that channel, and ESP-NOW peers are added with `channel = 0` so they follow the radio. Boards on different channels cannot hear each other.
- **Interface.** The broadcast peer is registered on `WIFI_IF_STA`. The soft AP and ESP-NOW share the radio, so ESP-NOW traffic and phone Wi-Fi traffic compete for airtime.
- **Power save.** Wi-Fi sleep is disabled (`WiFi.setSleep(false)`) so the radio is always listening.
- **TX power.** Set to the maximum (19.5 dBm).
- **Receive path.** The ESP-NOW callback runs in the Wi-Fi task. It only copies the packet into a 16-entry ring under a spinlock. All parsing, crypto and storage happen in `loop()`. If the ring is full, new packets are dropped.
- **Send path.** `esp_now_send` is non-blocking. If it returns an error (for example, driver queue full), the packet is retried after 10 ms, up to 5 times.
- **Range.** Typical ESP-NOW range is on the order of 100–200 m in open air and much less indoors. These are rough numbers; antenna, orientation, enclosure and interference all matter.
- **Core version.** The receive callback signature differs between Arduino-ESP32 2.x and 3.x. The sketch selects the right one with `ESP_ARDUINO_VERSION_MAJOR`.

## HTTP API

The web app is the only intended client, but the API is small and can be used directly. All responses are JSON unless noted and are sent with `Cache-Control: no-store`. Errors are `400` with a plain-text message.

### `GET /api/hello?dev=<8 hex>`

Registers a phone. If `dev` is missing or invalid, the board generates a random non-zero 32-bit ID.

```json
{ "dev": "a1b2c3d4", "ssid": "Johnify-9F3A" }
```

### `POST /api/join` (form: `room`)

Makes the board start listening for a chat code. Idempotent. Adds the room (and derives its key) if it is new. If all `MAX_ROOMS` slots are in use, the least recently used room is replaced.

```json
{ "ok": true }
```

Errors: `bad room` (not exactly 10 characters of `A–Z0–9`).

### `POST /api/send` (form: `room`, `dev`, `name`, `text`)

Encrypts, stores locally, and broadcasts a message. `name` is truncated to 12 bytes and `text` to 190 bytes, on UTF-8 character boundaries.

```json
{ "ok": true, "i": "5e8c01aa" }
```

Errors: `bad room`, `bad dev`, `empty`, `crypto`.

### `GET /api/poll?since=<n>&boot=<hex>&r=<code,code,...>`

Returns messages newer than sequence number `since` for the listed chats. Also registers any listed chats the board does not have yet.

```json
{
  "boot": "07c4e19b",
  "msgs": [
    { "i": "5e8c01aa", "d": "a1b2c3d4", "r": "K7M2QX9TBA", "n": "Sam", "x": "hello", "a": 3 }
  ],
  "next": 42
}
```

| Field | Meaning |
|-------|---------|
| `boot` | Random ID chosen at each board start. |
| `i`, `d` | Message ID and sender device ID. Together they identify the message. |
| `r` | Chat code. |
| `n`, `x` | Display name and text. |
| `a` | Age of the message in seconds, by the board's clock. |
| `next` | The value the client should send as `since` next time. |

If the `boot` sent by the client does not match the board's, `since` is treated as 0. This handles a phone switching boards or a board rebooting. At most 30 messages are returned per call; if there are more, `next` points at the last one scanned and the client simply polls again.

### `GET /api/status`

```json
{ "repeater": false, "ssid": "Johnify-9F3A", "clients": 2, "rooms": 3,
  "rx": 118, "tx": 64, "relayed": 0, "up": 5321 }
```

### `POST /api/repeater` (form: `on=1` or `on=0`)

Switches repeater mode. The setting is saved to flash and survives reboots.

### Other paths

`/` serves the app. `/favicon.ico` returns 204. Every other path replies `302` to `http://192.168.4.1/`, which together with a DNS server that answers every query with the board's address makes the captive-portal behaviour work.

## Web client

The app is a single HTML page stored as a string inside `Johnify.ino` (the `INDEX_HTML` constant), which the board serves from flash. It uses only system fonts and has no external requests, because the phone has no internet while connected to the board.

### Saved state (`localStorage`, key `jf`)

| Key | Contents |
|-----|----------|
| `dev` | Device ID the board assigned to this phone. |
| `name` | Display name. |
| `rooms` | Array of joined chat codes. |
| `msgs` | Per-chat message history (capped at 300 per chat). |
| `unread` | Per-chat unread counts. |
| `since`, `boot` | Position in the current board's message list. |

### Sync protocol

- The client polls `/api/poll` every 1.5 s, plus once when the tab becomes visible again.
- Incoming messages are de-duplicated against saved history by `devId + msgId`, so repeated or re-fetched messages never show twice.
- When a chat is created or joined, the client fetches that chat's history with `since=0`, so a phone joining late sees what the board still holds.
- Message times shown in the UI are computed as `now - age` when the message is first received. The board has no real-time clock, so times are approximate for old messages.
- Requests time out after 5 s. The header indicator shows **Linked** or **No link** based on whether the last poll succeeded.

Because phones are plain HTTP pages, `crypto.subtle` is not available. That is why encryption happens on the board rather than in the browser.

## Configuration

Settings are `#define`s at the top of `Johnify.ino`.

| Name | Default | Description |
|------|--------:|-------------|
| `AP_SSID_PREFIX` | `"Johnify-"` | Wi-Fi name prefix. The last 4 hex digits of the MAC are appended. |
| `AP_PASSWORD` | `""` | Empty for an open network. 8+ characters to require a WPA2 password. |
| `WIFI_CHANNEL` | `1` | Must match on all boards. |
| `MAX_HOPS` | `4` | Maximum repeater hops. |
| `MAX_ROOMS` | `16` | Chats stored per board. |
| `MAX_MSGS` | `200` | Messages kept in RAM per board. |
| `MAX_TEXT` | `190` | Maximum message length in bytes. Limited by the 250-byte ESP-NOW payload. |
| `ROOM_LEN` | `10` | Chat code length. The web app also assumes 10, so change both. |
| `SEEN_SIZE` | `256` | Duplicate-suppression memory. |
| `TXQ_SIZE`, `RXQ_SIZE` | `16` | Transmit and receive queue depth. |

If you change `MAX_TEXT`, keep `PKT_HDR + 12 + MAX_TEXT + 16 <= 250`. A `static_assert` enforces this at compile time.

## Memory and limits

Approximate static RAM use:

| Item | Size |
|------|-----:|
| Message ring (200 × ~232 B) | ~46 KB |
| Transmit queue (16 × ~260 B) | ~4 KB |
| Receive queue (16 × ~251 B) | ~4 KB |
| Seen-ID ring (256 × 8 B) | 2 KB |
| Rooms (16 × ~56 B) | ~1 KB |

Persistent storage (NVS, namespace `johnify`) holds the comma-separated list of chat codes and the repeater flag. Messages are not written to flash, so they are lost on power loss; phones keep their own copies.

Other limits:

- 190 bytes of text per message, 12 bytes of name.
- Up to 8 phones per board at once (soft AP setting).
- A board holds 16 chats. Using more evicts the least recently used.
- Message ordering across boards is by arrival, not by a shared clock.
- No delivery guarantee. Broadcast is best-effort; double-sending reduces but does not eliminate loss.

### What is protected

- **Confidentiality of name and text** against anyone who does not have the chat code.
- **Integrity**: modified or forged packets fail the GCM tag check and are dropped.
- **Key secrecy on the air**: the code and key are never transmitted, and the group ID does not reveal them.
- **Repeaters** relay without being able to read anything.

### What is not protected

- **Metadata is visible.** A listener can see that packets are Johnify packets, their size and timing, the opaque group ID (so which packets belong to the same chat), the random device ID, and hop counts.
- **Device IDs are stable.** A phone keeps its ID, so a listener can tell that packets with the same ID came from the same phone, even though they cannot read the contents.
- **The chat code is the password.** Anyone who has it can read and write. There are no individual accounts, no revocation, and no forward secrecy; if a code leaks, past captured traffic for that chat can be decrypted.
- **Offline guessing.** An attacker who captures one packet can test candidate codes offline, using the group ID or the GCM tag as the check. Each guess costs about 5,000 SHA-256 operations. With a 49.5-bit code and that stretching, the work to exhaust the space is roughly 2^62 hash operations. That is out of reach for casual attackers but not a margin you should rely on against a well-funded one. A longer code or a real memory-hard KDF would improve this.
- **Replay.** GCM does not stop an attacker from re-broadcasting a captured packet. The duplicate ring only remembers 256 recent messages, so a replay after enough newer traffic would be accepted again by a board. Phones de-duplicate by message ID in their saved history, which hides most replays in the UI.
- **Phone to board link.** The phone talks to the board over plain HTTP on the board's Wi-Fi. The board sees the message in plain text before encrypting it, which is expected, but anyone connected to an open Johnify hotspot could observe that HTTP traffic. Set `AP_PASSWORD` if that matters.
- **Physical access.** Chat codes are stored in the board's flash in plain text.
- **Denial of service.** Anyone can jam 2.4 GHz or flood ESP-NOW with junk packets.

This project has not had an independent security review.

## Troubleshooting

| Symptom | Things to check |
|---------|-----------------|
| Johnify network does not appear | Board powered? Wait a few seconds after boot. Try the serial monitor for the printed SSID. |
| Page does not load | Open `http://192.168.4.1` directly. Turn off mobile data. Some phones need the captive-portal prompt dismissed first. |
| Header shows **No link** | The phone left the board's Wi-Fi, or the board rebooted. Reconnect. |
| Messages do not reach the other board | Both boards must use the same `WIFI_CHANNEL` and the same chat code, and be within range. Open the Repeater page on the receiving board and watch **Packets heard** while the other board sends. If it does not rise, it is a radio problem. If it rises but nothing appears, the chat codes differ. |
| Messages disappear after a board restarts | Expected. The board's RAM history is lost. Phones keep their own copy. |
| Compile error about `esp_now_recv_info_t` | Old board package. The sketch handles 2.x and 3.x, so update the package or report the exact version. |
| Chats lost after switching browsers | History is stored per browser per device. The "sign in to network" popup uses a separate browser storage from Chrome or Safari. |
| No serial output | Enable **USB CDC On Boot** in the Arduino IDE Tools menu. |

The onboard LED blinks on each packet sent or received.

## Repository layout

```
Johnify/
├── Johnify.ino   firmware (radio, crypto, HTTP API) and the embedded web app
└── README.md     this file
```

Arduino requires the sketch folder name to match the main `.ino` file name, so keep the folder named `Johnify`.

## Testing status

Beta: V.0.1.1
