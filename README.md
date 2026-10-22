# Johnify

An encrypted group chat that runs on ESP32 boards with no internet, router, or cell service.

Each board runs its own Wi-Fi hotspot and serves a small chat web app. Phones connect to the nearest board with a browser. Boards pass messages to each other over ESP-NOW, a connectionless 2.4 GHz protocol built into the ESP32. Messages are encrypted with AES-256-GCM using a key derived from a shared chat code. Any board can also be switched into repeater mode to extend range.

There are two implementations of the same firmware. The Arduino version is a single source file. The MicroPython version is three small files in one folder:

| Version | File | Language | Boards |
|---------|------|----------|--------|
| Arduino | `arduino/Johnify/Johnify.ino` | C++ | Written for the Seeed Studio **XIAO ESP32S3**. Other ESP32-family boards supported by the Arduino ESP32 core should work. |
| MicroPython | `MicroPython/` (`main.py`, `johnify.html`, `encryption.py`) | Python | Any ESP32-family board (ESP32, S2, S3, C3, C6) running MicroPython 1.21 or newer. See [MicroPython version](#micropython-version). |

Both serve the same web app, speak the same packet format, and use the same encryption, so boards running either version can share chats and relay for each other in the same network. A test that builds packets with the actual Arduino code and the MicroPython code and exchanges them in both directions is described under [Verification](#verification).

---

## Contents

- [Features](#features)
- [Quick start](#quick-start)
- [MicroPython version](#micropython-version)
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
- [Troubleshooting](#troubleshooting)

---

## Features

- No infrastructure. Boards talk to each other directly; phones talk only to their local board.
- Chat rooms identified by a 10-character code. Anyone with the code can join. Anyone without it cannot read the traffic.
- End-to-end encryption: AES-256-GCM, with the key derived from the chat code. The key never leaves the board and the phone, and is never transmitted.
- Duplicate suppression: the same message arriving twice (retransmits, repeaters) is shown once.
- Repeater mode: a board can re-broadcast any message it hears, for any chat, up to a hop limit. Repeaters do not need to know the chat code and cannot read the messages they relay.
- Chat history is kept in the phone's browser storage. Chat codes are kept in the board's flash. Recent messages are kept in the board's RAM.
- Captive-portal behaviour: phones prompt "sign in to network" and any URL typed while connected opens the app.
- A mixed network works. Arduino boards and MicroPython boards can chat and relay for each other.
- The web app is served entirely by the board and makes no outside requests, so it works with no internet. It uses each phone's built-in fonts.

## Quick start

### Hardware

- Two or more ESP32-family boards. Two are the minimum to test; each additional board is another chat node or a repeater. The Arduino version is written for the Seeed XIAO ESP32S3, powered over USB-C or a LiPo battery. The MicroPython version runs on a wider range of boards (see below).

### Arduino IDE

1. Install the **esp32 by Espressif Systems** board package (3.x recommended; 2.x is also handled).
2. Open `arduino/Johnify/Johnify.ino`. It is the only source file.
3. Board: **XIAO_ESP32S3**.
4. Optional: set **USB CDC On Boot** to *Enabled* if you want serial output over the USB port.
5. Upload the same sketch to every board.

No third-party libraries are required. `WiFi`, `WebServer`, `DNSServer`, `Preferences`, `esp_now`, and `mbedtls` all ship with the board package.

### arduino-cli

```sh
arduino-cli core install esp32:esp32
arduino-cli compile --fqbn esp32:esp32:XIAO_ESP32S3 arduino/Johnify
arduino-cli upload  --fqbn esp32:esp32:XIAO_ESP32S3 -p /dev/ttyACM0 arduino/Johnify
```

Replace the port with your own (`COMx` on Windows).

## MicroPython version

The `MicroPython/` folder is a full port of the Arduino firmware, written for a broad range of boards rather than one. It behaves the same way from the phone's point of view: same hotspot name, same web page (byte for byte), same HTTP API, same packet format, same key derivation, same limits.

It is three files, and all three go on the board:

| File | What it is |
|------|------------|
| `main.py` | The program: Wi-Fi hotspot, web server, radio, chats, repeater. MicroPython runs this file by itself at every power-up. |
| `johnify.html` | The web app. It is read from flash and sent to phones in small pieces, so it is never held in RAM. |
| `encryption.py` | Key derivation and AES-256-GCM encryption. MicroPython has no GCM mode, so this builds it. |

Splitting it this way keeps memory use down. Each Python file is compiled separately, so the board never has to hold the whole program at once while loading it. The page never enters RAM. `encryption.py` can be precompiled (see Requirements). And `johnify.html` can be edited and re-uploaded on its own.

### Which boards

The port needs a MicroPython build that includes `espnow`, `asyncio`, `hashlib` with SHA-256, and `cryptolib` with AES. Every current ESP32-family build does.

| Chip | Examples | Notes |
|------|----------|-------|
| ESP32 | DevKitC, WROOM and WROVER modules, TTGO, many others | Usually no PSRAM. Lower `MAX_ROOMS` and `MAX_MSGS` (see below). |
| ESP32-S2 | S2 mini and similar | Single core, limited RAM. Use the small-board settings. |
| ESP32-S3 | XIAO ESP32S3, DevKitC-1, many others | With PSRAM (for example the XIAO) the defaults are fine. |
| ESP32-C3 | XIAO ESP32C3, SuperMini | Single core, limited RAM. Use the small-board settings. |
| ESP32-C6 | DevKitC-1 | Needs a MicroPython build that supports the C6. |

Not supported: the ESP8266 (not enough RAM), the Raspberry Pi Pico W and other RP2 boards (no ESP-NOW), and non-Espressif boards. The program is written for these chips and checked as described under [Verification](#verification), but it has not been run on every board in the table. If a board misbehaves, use **Copy bug report** and open an issue with the chip and MicroPython version.

### Requirements

- **MicroPython 1.21 or newer** for the `espnow` module and a current `asyncio`.
- **All three files on the board, in its top-level folder, with these exact names.** If `johnify.html` is missing the board still runs but answers every page request with a message saying so. If `encryption.py` is missing the program stops at start-up and prints a message saying so. If you rename the HTML file, set `PAGE_FILE` at the top of `main.py` to match.
- **Free RAM.** The page is not held in RAM, which is what makes small boards workable. What does use RAM is the chats (about 2 KB each) and the message history (roughly 250 to 450 bytes per message, depending on length). These are rough estimates. Suggested starting values:

| Board | `MAX_ROOMS` | `MAX_MSGS` |
|-------|------------:|-----------:|
| With PSRAM (for example a XIAO ESP32S3 on a PSRAM firmware build) | 24 | 200 |
| ESP32 or ESP32-S3 without PSRAM | 8 | 60 |
| ESP32-S2 or ESP32-C3 | 6 | 40 |

The program also protects itself. If free memory drops below about 30 KB it discards the older half of the message history, and only reboots if memory stays below about 12 KB.

- **Optional: precompile `encryption.py`.** Running `mpy-cross encryption.py` produces `encryption.mpy`, which loads faster and needs less RAM to start. Upload `encryption.mpy` and delete `encryption.py` from the board, because if both exist MicroPython uses the `.py`. Use an `mpy-cross` that matches your MicroPython version. `main.py` has to stay a `.py` file, because MicroPython only runs `main.py` at start-up.

### Install

> **The port name and number will be different on your computer.** The commands below use `/dev/ttyACM0` as a stand-in. Your serial port has a different name, and the name can change during the install, so look it up each time instead of copying the example.
>
> | System | What ports look like |
> |--------|----------------------|
> | Windows | `COM3`, `COM4`, and so on. See Device Manager, under Ports (COM & LPT). |
> | macOS | `/dev/cu.usbmodem1101`, `/dev/cu.usbserial-0001`, and similar. List them with `ls /dev/cu.*`. |
> | Linux | `/dev/ttyACM0`, `/dev/ttyUSB0`, and similar. List them with `ls /dev/ttyACM* /dev/ttyUSB*`. |
>
> The name and number change when you use a different USB socket or cable, with a different board, on a different computer, and very often when the board switches between bootloader mode (flashing) and normal mode (running). After every unplug and replug, check again. `python -m serial.tools.list_ports` lists every serial port, and `mpremote connect list` lists the boards running MicroPython.
>
> If only one board is plugged in you can leave out `--port` (esptool) or `connect <port>` (mpremote) entirely, and the tools find the board themselves.
>
> On Linux, a `Permission denied` error means your user is not allowed to use serial ports. Run `sudo usermod -aG dialout $USER`, then log out and back in.

1. Download the MicroPython `.bin` for your board from the MicroPython downloads page. Pick the exact board, or the generic build for your chip. For a board with PSRAM, such as the XIAO ESP32S3, pick the build with PSRAM support (for the generic S3 build that is `ESP32_GENERIC_S3-SPIRAM_OCT`).
2. Put the board in bootloader mode. On the XIAO, hold the **B** (boot) button while plugging in USB. Most dev boards have a BOOT button to hold while pressing EN or reset.
3. Find the port the board has in bootloader mode (see the box above), then flash it. The start address depends on the chip:

| Chip | Flash offset |
|------|--------------|
| ESP32, ESP32-S2 | `0x1000` |
| ESP32-S3, ESP32-C3, ESP32-C6 | `0x0` |

```sh
pip install esptool mpremote
esptool.py --chip esp32s3 --port /dev/ttyACM0 erase_flash
esptool.py --chip esp32s3 --port /dev/ttyACM0 --baud 460800 write_flash -z 0x0 <firmware>.bin
```

Use your own `--chip` value, offset and port. On newer versions of esptool the command is `esptool` instead of `esptool.py`.

4. Unplug the board and plug it back in without holding the button. **Find the port again: it is usually different from the one in step 3.**
5. From the root of this repository, copy the three files to the board, using your port. They must end up in the board's top-level folder with the same names:

```sh
mpremote connect /dev/ttyACM0 cp MicroPython/main.py :main.py
mpremote connect /dev/ttyACM0 cp MicroPython/johnify.html :johnify.html
mpremote connect /dev/ttyACM0 cp MicroPython/encryption.py :encryption.py
mpremote connect /dev/ttyACM0 fs ls
```

The last command lists the files on the board. You should see all three. In Thonny, open each file and use File, Save as, MicroPython device, keeping the same name.

6. Restart the board and watch its log:

```sh
mpremote connect /dev/ttyACM0 reset
mpremote connect /dev/ttyACM0 repl
```

Press Ctrl+] to leave the REPL. Log lines start with `[johnify]`, and you should see `Ready: Johnify-XXXX`. The program starts by itself on every power-up.

Settings are variables at the top of `main.py`. Set `LED_PIN` to your board's LED pin if you want the activity light (see Configuration). It is off by default because a pin number that is an LED on one board can be something else on another.

While developing over USB, set `USE_WATCHDOG = False`. The watchdog cannot be stopped once it starts, so a board you have interrupted with Ctrl-C will reboot after 30 seconds.

### Removing Johnify from a board

Deleting the files does not erase the chat list and repeater setting that Johnify keeps in the board's flash storage, because those are not files. Before you delete anything, open **Settings** and press **Reset board** to clear them, or use option 3 below, which erases everything. If you skip this and install Johnify again later, the old saved chats come back.

Use the port the board has now (see the port box in Install). Pick one option.

**Option 1: remove only Johnify's files.** `main.py` goes first on purpose. While Johnify is running its watchdog reboots the board 30 seconds after you interrupt it, and with `main.py` gone nothing restarts.

```sh
mpremote connect /dev/ttyACM0 fs rm main.py
mpremote connect /dev/ttyACM0 fs rm johnify.html
mpremote connect /dev/ttyACM0 fs rm encryption.py
mpremote connect /dev/ttyACM0 fs ls
```

If you installed the precompiled version, also run `fs rm encryption.mpy`. If the board restarts part-way through, just run the remaining commands again. The last command should list nothing from Johnify. The board then starts at the MicroPython prompt instead of running Johnify.

**Option 2: delete every file on the board.** MicroPython itself stays installed. This deletes all files, not just Johnify's, including any SD card mounted on the board. Run option 1's first command (`fs rm main.py`) first, then open the REPL with `mpremote connect /dev/ttyACM0 repl`, press Ctrl+E to enter paste mode, paste this, and press Ctrl+D to run it:

```python
import os

def wipe(path):
    for name in os.listdir(path):
        full = path.rstrip("/") + "/" + name
        if os.stat(full)[0] & 0x4000:
            wipe(full)
            os.rmdir(full)
        else:
            os.remove(full)

wipe("/")
```

Then type `os.listdir()`. It should print `[]`. Press Ctrl+] to leave the REPL.

**Option 3: erase the whole board.** This removes the files, the saved chats and settings, and MicroPython itself. Put the board in bootloader mode (see Install, step 2), find its port, and run:

```sh
esptool.py --chip esp32s3 --port /dev/ttyACM0 erase_flash
```

The board is then blank. To use it again, flash MicroPython (Install, step 3) or another firmware.

### What is the same

- Wi-Fi hotspot `Johnify-XXXX` (the name uses the same bytes of the MAC as the Arduino build), captive-portal DNS, channel 1, up to 8 phones.
- The web app, including Settings, repeater mode, diagnostics, the bug report button, leaving a chat, and resetting the board.
- The HTTP API and its error codes (`bad room`, `bad dev`, `empty`, `crypto`, `radio busy`).
- The packet layout, the key derivation (5,000 rounds of SHA-256), and AES-256-GCM with the same additional authenticated data, so packets from either version decrypt on the other.
- Duplicate suppression, the 4-hop repeater limit with random relay delay, sending each message twice, 24 chats per board, 12 per phone, 200 messages in memory, 190 bytes of text.
- Saved chat codes and the repeater setting in the board's flash (the ESP32 NVS store), written only when they change.
- Self-recovery: the radio is restarted after 5 failed sends in a row, and the board reboots on very low memory or a fatal error.

### What is different, and what is not supported

| Area | Difference |
|------|------------|
| **Files** | Three files on the board instead of one compiled image. `main.py`, `johnify.html` and `encryption.py` must all be present. |
| **Web page storage** | `johnify.html` is a normal file on the board's flash. It is read in 1 KB pieces and sent, not kept in RAM. It is the same page the Arduino build serves. |
| **AES-GCM** | MicroPython has no GCM mode, only the raw AES block function. `encryption.py` builds GCM itself in Python on top of that. The output is standard AES-256-GCM, byte for byte the same as the Arduino build, but it runs in software, so encrypting or decrypting one message takes tens of milliseconds instead of well under one. |
| **Adding a chat** | Deriving the key (5,000 SHA-256 rounds in Python) briefly blocks the board, on the order of a second. |
| **Saved chats at boot** | They load one by one in the background after the hotspot is up. A message for a chat that has not finished loading is not decrypted. On a board with many saved chats this window lasts a few seconds. |
| **Receive queue** | The Arduino build has its own 16-packet queue. Here the ESP-NOW driver's receive buffer (set to 4 KB) does that job, and the "packets dropped" counter comes from the driver's statistics. |
| **Lowest free memory** | The Arduino build reads the true low-water mark. MicroPython has no equivalent, so the value shown is the lowest reading sampled every 5 seconds. |
| **Low memory** | The message history lives in RAM that grows with use, and is trimmed under memory pressure. The Arduino build uses fixed arrays. |
| **Concurrency** | One cooperative event loop instead of FreeRTOS tasks. Crypto and key derivation block everything else for a moment. |
| **Web server** | A small built-in HTTP/1.1 server: one request per connection, request bodies up to 2 KB, at most 6 connections at once (extra ones are dropped and the page retries), 10 seconds per request. It does not support keep-alive, chunked uploads, or file uploads, none of which the app uses. |
| **Radio tuning** | Transmit power and Wi-Fi power-save are set when the firmware build allows it and skipped silently when it does not. |
| **LED** | Off unless `LED_PIN` is set. |
| **Clock** | Uptime and message ages use the board's seconds counter. |
| **Version string** | Settings shows `1.3.0-mp`, and the SDK line shows the MicroPython version instead of the ESP-IDF version. |

Nothing in the Arduino feature set is missing from the port.

---

## Using Johnify

1. On your phone, join the Wi-Fi network named `Johnify-XXXX` (the last four hex digits of the board's MAC). It is open by default.
2. Open a browser and go to `http://192.168.4.1`. Any address typed while connected also redirects there. If your phone shows a "sign in to network" prompt, it opens the same page, but a full browser is better because its storage persists.
3. Press **Create chat**. You get a 10-character code.
4. Give the code to the other person. They connect to their own board, press **Join**, and enter it.
5. Messages sent by either person appear on the other's phone.
6. The **Settings** page (the sliders icon in the top-right corner) has your display name, the repeater switch that makes that board relay traffic, live counters, and the reset button.

**Leaving a chat.** Open the chat and press **Leave**, then confirm in the dialog. The chat and its saved messages are deleted from your phone, and the board forgets the chat too: it stops keeping messages for it and removes it from its saved list. You can join again later with the code. If another phone on the same board is still in that chat, that phone re-adds it automatically within a couple of seconds.

**Resetting the board.** In Settings, press **Reset board** and confirm. The board erases every chat, message, counter, log line and setting it holds, turns repeater mode off, clears its saved data in flash, and restarts, so it behaves like freshly uploaded firmware. Your phone's saved chats, name and device ID are cleared too, because otherwise the phone would put the chats straight back. Other phones keep their own copies and re-add their chats when they next connect. The Wi-Fi network disappears for a few seconds while the board restarts. The firmware itself is not touched.

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

**Arduino.** Two tasks share the board's state under one lock. The Arduino `loop()` handles the radio: it drains the receive queue, sends queued packets, and runs the health check. A second task runs the web server and the DNS server. They are separate because the Arduino web server library waits on idle connections for up to 5 seconds (browsers open spare connections), and that must not delay the radio. Every API handler and the radio code hold the lock only while touching shared data. The ESP-NOW receive callback runs in the Wi-Fi task and only copies the packet into a queue under a spinlock.

**MicroPython.** One cooperative event loop runs the radio, the web server, the DNS server, background chat loading, and the health check as separate coroutines. Nothing runs in parallel, so no lock is needed; slow work such as key derivation briefly holds up the others.

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

Makes the board start listening for a chat code. Idempotent. Adds the room (and derives its key) if it is new. If all `MAX_ROOMS` slots are in use, the least recently used room is replaced. Only create, join, and send do this.

```json
{ "ok": true }
```

Errors: `bad room` (not exactly 10 characters of `A–Z0–9`).

### `POST /api/send` (form: `room`, `dev`, `name`, `text`)

Encrypts, stores locally, and broadcasts a message. `name` is truncated to 12 bytes and `text` to 190 bytes, on UTF-8 character boundaries.

```json
{ "ok": true, "i": "5e8c01aa" }
```

Errors: `bad room`, `bad dev`, `empty` (400), `crypto` (500), `radio busy` (503, the send queue is full and the message was not sent). The web app turns each of these into a plain-language message.

### `GET /api/poll?since=<n>&boot=<hex>&r=<code,code,...>`

Returns messages newer than sequence number `since` for the listed chats. Also registers any listed chats the board does not have yet, as long as there is a free slot (polling never replaces an existing chat).

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

If the `boot` sent by the client does not match the board's, `since` is treated as 0. This handles a phone switching boards or a board rebooting. At most 30 messages (and about 4.5 KB of JSON) are returned per call; if there are more, `next` points at the last one scanned and the client simply polls again.

### `GET /api/log`

The board's last 16 events (start-up, radio failures, full queues, restarts), oldest first. Also printed to the serial port with a `[johnify]` prefix.

```json
{ "log": ["0s Johnify 1.3.0 starting", "2s Ready: Johnify-9F3A, repeater off, 1 chats saved"] }
```

### `GET /api/status`

```json
{ "ver": "1.3.0", "chip": "ESP32-S3", "sdk": "v5.4.1", "repeater": false,
  "ssid": "Johnify-9F3A", "ch": 1, "hops": 4, "clients": 2, "rooms": 3,
  "rx": 118, "tx": 64, "relayed": 0,
  "rxDrop": 0, "txFail": 0, "bad": 3, "auth": 0, "reinit": 0,
  "heap": 214000, "minHeap": 186000, "up": 5321 }
```

### `POST /api/repeater` (form: `on=1` or `on=0`)

Switches repeater mode. The setting is saved to flash and survives reboots.

### `POST /api/leave` (form: `room`)

Makes the board forget a chat: the chat is removed from the board's table and its saved list, and the board's stored messages for it are discarded. Harmless if the board does not know the chat. Errors: `bad room`.

```json
{ "ok": true }
```

### `POST /api/reset` (form: `confirm=RESET`)

Erases all chats, messages, duplicate memory, counters, log lines and flash-saved settings, turns repeater mode off, replies, and restarts the board about 0.8 seconds later. Only `POST` is accepted, and the confirmation word must be present, so a stray link or a browser pre-fetch cannot trigger it. Errors: `post only` (405), `confirm` (400).

```json
{ "ok": true }
```

### Other paths

`/` serves the app (`Cache-Control: no-cache`, so a firmware update shows up immediately). `/favicon.ico` returns 204. Every other path replies `302` to `http://192.168.4.1/`, which together with a DNS server that answers every query with the board's address makes the captive-portal behaviour work.

## Web client

The app is a single HTML page stored inside the firmware: the `INDEX_HTML` constant in `Johnify.ino`, or the file `MicroPython/johnify.html`. Both versions serve the identical page. There are no external requests, because the phone has no internet while connected to the board.

### Design

- **Type.** Native fonts only, so nothing extra is downloaded: a built-in serif (Iowan Old Style, Palatino, or Georgia, whichever the phone has) for headlines and numerals, and the phone's own system font for everything else.
- **Colour.** Solid colours only: forest green as the dominant colour, ink black, a paper background, and a vermilion accent used sparingly for unread counts, the send button, and warnings. A dark theme follows the phone's setting.
- **Layout.** A ruled, editorial list with large serif numerals for chats, a green header band inside a chat, and an asymmetric bento grid on the Settings page.
- **Motion.** One staggered reveal when the page first loads, switched off for users who ask for reduced motion. No other animation.
- **Navigation.** The phone's back button moves between the chat list, a chat, and Settings.

### Error handling in the page

- Saved data is validated when the page loads, so corrupted or old browser storage cannot break it.
- If the browser refuses to store data, the page says so once.
- Every request has a 5-second limit. Replies that are not valid JSON (such as a captive-portal page from the phone) are treated as "not the board".
- Board errors are translated into plain sentences. Unexpected script errors show a single notice.
- The page stops polling while the tab is hidden and slows down after repeated failures.
- Confirmations (Leave, Reset) use dialogs built into the page, not the browser's own pop-ups. Phones' "sign in to network" browsers block those pop-ups silently, which would make the buttons do nothing.
- A message cannot be double-sent while the previous one is in flight, and an over-length message is blocked before it is sent.

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
- Requests time out after 5 s. A small dot next to the title is green when the last poll succeeded and red when it failed.

Because phones are plain HTTP pages, `crypto.subtle` is not available. That is why encryption happens on the board rather than in the browser.

## Configuration

Settings are `#define`s at the top of `Johnify.ino`, or variables of the same names at the top of `main.py`. These exist only in the MicroPython version: `MAX_WEB_CLIENTS`, `LED_PIN`, `LED_ACTIVE_LOW`, `USE_WATCHDOG`, and `PAGE_FILE`. `RXQ_SIZE` exists only in the Arduino version.

| Name | Default | Description |
|------|--------:|-------------|
| `AP_SSID_PREFIX` | `"Johnify-"` | Wi-Fi name prefix. The last 4 hex digits of the MAC are appended. |
| `AP_PASSWORD` | `""` | Empty for an open network. 8+ characters to require a WPA2 password. |
| `WIFI_CHANNEL` | `1` | Must match on all boards. |
| `MAX_HOPS` | `4` | Maximum repeater hops. |
| `MAX_ROOMS` | `24` | Chats stored per board. |
| `MAX_MSGS` | `200` | Messages kept in RAM per board. |
| `MAX_TEXT` | `190` | Maximum message length in bytes. Limited by the 250-byte ESP-NOW payload. |
| `ROOM_LEN` | `10` | Chat code length. The web app also assumes 10, so change both. |
| `SEEN_SIZE` | `256` | Duplicate-suppression memory. |
| `TXQ_SIZE`, `RXQ_SIZE` | `16` | Transmit and receive queue depth. |
| `MAX_WEB_CLIENTS` | `6` | MicroPython only. Web connections handled at once. |
| `LED_PIN` | `None` | MicroPython only. Pin number of a status LED, or `None`. XIAO ESP32S3: 21. Many ESP32 dev boards: 2. |
| `LED_ACTIVE_LOW` | `True` | MicroPython only. `True` if the LED lights when the pin is low (XIAO), `False` for most dev boards. |
| `USE_WATCHDOG` | `True` | MicroPython only. Reboot if the program freezes for 30 seconds. |
| `PAGE_FILE` | `"johnify.html"` | MicroPython only. The web page file, read from flash on request. Keep it on the board next to `main.py`. |

If you change `MAX_TEXT`, keep `PKT_HDR + 12 + MAX_TEXT + 16 <= 250`. A `static_assert` enforces this at compile time.

## Memory and limits

Approximate static RAM use of the Arduino version (the Arduino build also stores about 27 KB of web page in flash, not RAM; MicroPython memory is covered in its own section):

| Item | Size |
|------|-----:|
| Message ring (200 × ~232 B) | ~46 KB |
| Transmit queue (16 × ~260 B) | ~4 KB |
| Receive queue (16 × ~251 B) | ~4 KB |
| Seen-ID ring (256 × 8 B) | 2 KB |
| Rooms (24 × ~56 B) | ~1.3 KB |
| Poll response buffer | 6 KB |

Persistent storage (NVS, namespace `johnify`) holds the comma-separated list of chat codes and the repeater flag. A value is only written when it has changed, so normal use does not wear the flash. Messages are not written to flash, so they are lost on power loss; phones keep their own copies.

Other limits:

- 190 bytes of text per message, 12 bytes of name.
- Up to 8 phones per board at once (soft AP setting).
- A board holds 24 chats and a phone can have 12. If more than 24 chats are active on one board, creating or joining a new one replaces the least recently used. Polling never evicts, so a busy board cannot thrash its chat list or flash.
- Message ordering across boards is by arrival, not by a shared clock.
- No delivery guarantee. Broadcast is best-effort; double-sending reduces but does not eliminate loss.

## Reliability and diagnostics

- **The radio is never held up by the web server.** On Arduino the web server runs in its own task, so a slow or idle phone connection cannot delay sending or receiving.
- **Self-recovery.** Every 5 seconds the board checks its health. If the radio fails to send 5 packets in a row, it restarts the ESP-NOW radio on its own. If that restart fails, or if free memory stays below 20 KB for 30 seconds (12 KB on MicroPython, which first discards older messages), it reboots. If the hotspot or radio cannot start at boot, it reboots and tries again.
- **Honest errors.** If a message cannot be queued for sending, the phone is told (`radio busy`) and the message stays in the input box. Nothing is silently dropped on the way out.
- **Counters.** The board counts packets dropped because the receive queue was full (`rxDrop`), failed sends (`txFail`), rejected packets (`bad`, wrong format), failed security checks (`auth`), and radio restarts (`reinit`). They appear under Settings, Health, and the non-zero ones are highlighted.
- **Event log.** The last 16 events are kept in memory and shown in the bug report. The same lines go to the serial monitor at 115200 baud.
- **Bug report.** Settings has a **Copy bug report** button. Nothing is sent anywhere. It gathers firmware version, chip, SDK, uptime, channel, memory, counters, recent events, and the phone's browser into one block of text, selects it, and tries to copy it. If the browser blocks copying, select the text and copy it by hand. Paste it into a GitHub issue.
- **Connection warning.** If the phone cannot reach the board twice in a row, a red banner appears under the header with the Wi-Fi name to join.
- **Input cleaning.** Control characters are removed from names and messages, long text is cut on character boundaries, and a live "N left" counter appears in the message box as you near the 190-byte limit.

## Memory safety

**Arduino**

- All long-lived data is in fixed-size global arrays: the message ring, the chat table, the duplicate ring, and the two packet queues. Nothing in the radio path allocates memory.
- HTTP replies are built in fixed-size buffers with bounds-checked helpers. The poll reply is built in one 6 KB buffer and stops adding messages when less than 1,400 bytes remain, which is more than the largest possible single message after JSON escaping.
- The only dynamic memory is the short-lived Arduino `String` objects the web server library creates for request parameters. They are freed when each request ends.
- Incoming packets are length-checked before use. Each message copy is bounded by the maximum text size, and strings are always terminated.
- If a queue is full, new items are dropped instead of overwriting anything.

**MicroPython**

- The page is never loaded into RAM. It is read from flash in 1 KB pieces while sending.
- The message history is a list capped at `MAX_MSGS`, the duplicate memory is a ring with a matching set, and the transmit queue is capped, so none of them can grow without limit.
- Under memory pressure the older half of the history is dropped before anything else happens, and the board reboots only as a last resort.
- Web connections are capped, each request has a 10-second limit, and requests with oversized headers or bodies are refused.

**Web page (both versions)**

- The page keeps at most 300 messages per chat and 12 chats in browser storage, clears its timers when a screen changes, and cancels any request that takes longer than 5 seconds.

**Known limit (Arduino).** The Arduino web server library reads a whole request body into memory before the handler runs and has no size cap, so a deliberately oversized request from a device on the hotspot could use up memory. Use `AP_PASSWORD` if you do not want unknown devices on the hotspot.

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
- **Anyone on the hotspot can reset the board.** `POST /api/reset` has no login. The `POST` and confirmation word only protect against accidents. On an open hotspot, any device that joins could wipe the board. Set `AP_PASSWORD` if that matters.

This project has not had an independent security review.

Firmware version: 1.3.0.

## Troubleshooting

| Symptom | Things to check |
|---------|-----------------|
| Johnify network does not appear | Board powered? Wait a few seconds after boot. Try the serial monitor for the printed SSID. |
| Page does not load | Open `http://192.168.4.1` directly. Turn off mobile data. Some phones need the captive-portal prompt dismissed first. |
| Dot next to the title is red | The phone left the board's Wi-Fi, or the board rebooted. Reconnect. |
| Messages do not reach the other board | Both boards must use the same `WIFI_CHANNEL` and the same chat code, and be within range. Open Settings on the receiving board and watch **Packets heard** while the other board sends. If it does not rise, it is a radio problem. If it rises but nothing appears, the chat codes differ. |
| Messages disappear after a board restarts | Expected. The board's RAM history is lost. Phones keep their own copy. |
| Compile error about `esp_now_recv_info_t` | Old board package. The sketch handles 2.x and 3.x, so update the package or report the exact version. |
| Chats lost after switching browsers | History is stored per browser per device. The "sign in to network" popup uses a separate browser storage from Chrome or Safari. |
| No serial output (Arduino) | Enable **USB CDC On Boot** in the Arduino IDE Tools menu. |
| `MemoryError` or repeated "Memory low" in the log on MicroPython | Lower `MAX_ROOMS` and `MAX_MSGS`. See the table under Requirements. On a board with PSRAM, use a firmware build that has PSRAM support. |
| MicroPython page shows "Johnify web page not found" | `johnify.html` is not on the board, or is not in the top-level folder, or was renamed (then set `PAGE_FILE`). Copy it again and check with `mpremote fs ls`. |
| MicroPython prints "encryption.py is missing" and stops | Copy `encryption.py` (or `encryption.mpy`) to the board's top-level folder. |
| `mpremote` or `esptool` cannot find or open the port | The port name differs from the example, and it changes between bootloader mode and normal mode. Look it up again (see the port box under Install). Try another USB cable, since some only charge. On Linux, add your user to the `dialout` group. |
| `ImportError: no module named 'espnow'` | The MicroPython build is too old or is not an ESP32 build. Use 1.21 or newer. |
| MicroPython board reboots every 30 seconds while you work | The watchdog is on. Set `USE_WATCHDOG = False` while developing. |
| MicroPython prints `Radio error` or `Radio send failed` | Check the log in the bug report. Repeated failures restart the radio automatically. |

The status LED, where there is one, blinks on each packet sent or received. On Arduino it is the board's built-in LED. On MicroPython set `LED_PIN`.
