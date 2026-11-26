# Signum front ends: web GUI, WebSocket API, command shell, TUI

One port (default `127.0.0.1:8770`) serves three things:

| URL | What |
|---|---|
| `http://HOST:PORT/` | the browser GUI (`www/index.html`; its tab icon is the front panel's lit green lamp: `www/favicon.svg`, PNG fallbacks `favicon-32.png` and `apple-touch-icon.png`) |
| `ws://HOST:PORT/` | the JSON WebSocket API (the GUI and the TUI use it) |
| `ws://HOST:PORT/shell` | the command shell: a raw byte stream to the command interpreter |

Code: `src/web/frontend.cpp` (the `frontend_*` hooks of `src/tnc_core.hpp`),
`src/web/ws_server.hpp` (the server: HTTP, the WebSocket upgrade, binary
frames and close), `www/index.html`, `tui/signum-tui`,
`tui/ws-shell-example.py`, `signum-start`.

## Running

```
signum-start --call N0CALL --alsa plughw:CARD=CODEC --ptt rts:/dev/ttyUSB0
```

starts the TNC and the terminal front panel, and prints the GUI's URL; the
TNC's own output goes to `~/.local/state/signum/tnc.log`. `--no-tui`
runs the TNC alone in the foreground. Every other option goes to
`signum`. The TUI on its own: `tui/signum-tui [ws://HOST:PORT/]`.

| Option | Meaning |
|---|---|
| (none) | on, `127.0.0.1:8770`: this machine only |
| `--ws [ADDR:]PORT` | another address or port (`0.0.0.0:8770`: every interface) |
| `--no-ws` | no GUI, no WebSocket |
| `--web-auth USER:PASS` | a login for the GUI pages (HTTP Basic) |
| `--ws-auth USER:PASS` | a login for the WebSocket API and `/shell` |
| `--web-auth-file F`, `--ws-auth-file F` | the same from a file (its first line that is not a `#` comment; a warning if others can read it) |
| `--gui DIR` | the directory served over HTTP (default: `www/` beside the binary's directory) |

**Security.** Open by default, but only on 127.0.0.1. On another address
anyone who can reach the port can control the TNC and key the radio, and the
TNC says so at start unless both logins are set. There is no TLS: across an
untrusted network use an SSH tunnel (`ssh -L 8770:127.0.0.1:8770 host`).
With `--ws-auth`, a client logs in with an `Authorization: Basic` header on
the upgrade request, `?auth=BASE64(USER:PASS)` on the URL, or (JSON API
only) an `auth` message first. `/shell` takes only the header or `?auth=`;
without them it writes one line and closes (code 1008).

## The command shell: `ws://HOST:PORT/shell`

A WebSocket client on `/shell` is attached to the TNC's command interpreter
exactly as a program on the pty is: its own interpreter state, terminal mode
with the `cmd:` prompt, `JHOST1`/`JHOST4` host mode (the CRC frames of the
classic host-mode interface, as Pat's driver sends them), `C CALL`, `D`, converse mode, received
text and link messages. It acts on the one modem the pty also uses.

- **TNC to client:** binary frames, the bytes exactly as the TNC would send
  them on its serial port.
- **Client to TNC:** binary frames, the bytes as typed or framed. Text frames
  are accepted too: their UTF-8 is mapped back to bytes (U+0000-U+00FF to one
  byte each), so plain ASCII commands can be sent either way.
- Frame boundaries mean nothing: a command may arrive in pieces, and one
  frame may hold several answers. End lines with CR.
- Closing the WebSocket detaches the client (as unplugging a serial cable).

```python
import websocket                                   # pip install websocket-client
ws = websocket.create_connection("ws://127.0.0.1:8770/shell")
ws.send_binary(b"MYcall\r")
print(ws.recv())                                   # b'MYcall\r\nMycall: >N0CALL<\r\ncmd: ' (in one or more frames)
```

`tui/ws-shell-example.py` is a complete client: interactive (`ws-shell-example.py
[URL]`: lines typed go out with CR, everything the TNC writes is printed),
one-shot commands (`-c "MYcall" -c "MYLevel 2"`), and a CRC host-mode demo
(`--host`: `JHOST4`, three `L` polls on channel 31 decoded, `JHOST0`), with
the frame encoder and decoder (CRC-16/X.25, low byte first, `AA` stuffed as
`AA 00`).

The web GUI's Terminal tab is the same kind of client, over the JSON API
(`term_open`, below).

## The JSON API: `ws://HOST:PORT/`

Every frame is one JSON object with a `"type"`;
requests may carry an `"id"`, echoed in the answer. Link text is 8-bit:
bytes 0x80-0xFF and control characters travel as `\u00XX` escapes (map code
points U+0000-U+00FF to one byte each; send text the same way).

### From the TNC

| type | fields | when |
|---|---|---|
| `hello` | `proto` (1), `server` (the version), `mycall`, `streams`, `subscribed`, `requests`, `shell` (`"/shell"`), `crypto` (true when built with encryption: see Encryption below); or only `"auth": "required"` | on connect |
| `panel` | `panel`: the front panel: `text` (the TNC's display text), `leds` {name: off/green/red/yellow}: the reference unit's nine LEDs, all still sent although the GUI and TUI draw only Traffic/Error, Send/CHO and Connected/Mail (as Connect/Call); Send/CHO is green on a P1 link too while we send, `busy` (the channel busy detector: `busy` / `free` in standby, Listen mode too; `-` while a link is up or being set up. The reference unit's own indication is Traffic/Error green in standby with Listen off, and status 0xF7 on channel 254; green = free, red = busy is the intended lamp), `mode` {plevel 0-3 (P1/P2/P3), level (the TX level, kept for older front ends), levels, `tx` / `rx` (the speed level each way: this station's sending level, the other station's; P1: 1 = 100 Bd, 2 = 200 Bd; 0 none; P2 has one level, so its `tx` is the level last sent at or now expected), `tx_long` / `rx_long` (long packets that way; false in standby and on P1)}, `audio` (the receive audio level, standby too: {`state`: `low` (RMS under -60 dBFS) / `ok` / `high` (a peak above -3 dBFS) / `clip` (a sample at 32000 or more in the last 2 s) / `none` (not measured yet), `peak_dbfs` (the peak of the last 2 s), `rms_dbfs` (the RMS of the last second); full scale 32768 = 0 dBFS; samples while we transmit not counted; src/modem/signum/rx_meter.hpp, docs/md/gui.md "Receive audio level"}), `freq_khz` (the radio's frequency in kHz from the PTT's rig control, `--ptt rig:`: read every 2 s in standby, never during a call or a link, the last reading kept then; null with no rig control or no answer for 10 s), `tune_led` (0-14, 7 = no offset, -1 none), `freq_led` (`red` past 10 Hz), `bar`, `detail` {mycall, remote, role, state (standby/calling/connected), afc_hz, unsent, unacked, rx_bytes, tx_text, link_s, plevel, speed, ptt, `qrt` (true while a `D`/`disconnect` is in progress: the link or call not yet ended), `pk_ok` / `pk_err` / `pk_rep` (this link's packet checks on P2/P3: good, nothing decoded, a repeat), `quality` (% good among the last 16 checks; -1 none yet, and on P1), cs_*, margin_*, `snr_db` (the received S/N in dB in 3 kHz over the last 10 s of a link, -9 to 99: 99 when the gaps are digital silence; null when not measurable, and in standby; docs/md/gui.md "Received S/N"), `snr_src` (where the S/N reading comes from: `gaps` = the window's own gaps, `ref` = the held noise reference, used while we send; null with no reading), `snr_ref_s` (the age in seconds of the held noise reference; null when there is none; used up to 900 s) and `snr_ref_kind` (`idle` = measured idle in standby, `gaps` = in this link's gaps; null with none; docs/md/gui.md "Received S/N"), `far` (how the far end hears us: the meaning of the other station's last control signal while we send, a list of `ok`, `repeat`, `faster`, `slower`, `length` (switch long/short packets); empty while we receive, in standby, when no answer was heard or the last is older than two long cycles (P1: 1 s)), `rx_cpm` / `tx_cpm` (characters a minute received / sent and acknowledged, over the last 60 s of the link, from its start while shorter; 0 in standby)}, `cpu` (CPU health, information only: {`state`: `ok` / `busy` / `starved`, `level` 0-2, `reason` (what was late in the last minute, empty when ok), `applies` (list: the standing causes here: no real-time priority, a CPU clock that cannot be held up, a load average above the core count), `advice` (empty when ok), `counts` {underrun, stall, late, thin, backoff, sl1, tx_late: events in the last minute}}; sent whenever the state changes and, while not ok, when the reason changes (at most once a second); always `ok` without real-time audio (FIFO link); the same text is in the log as `[cpu] ...` lines and the `CPULoad` command; see docs/md/performance.md "CPU health"), `cpu_temp_c` (the CPU temperature in °C, from the system's thermal zone (the CPU package / SoC zone, else the first): a multi-core package sensor (the hottest core) sampled every 0.5 s and averaged over the last 10 s, a single SoC sensor (a Pi) as read; published every 2 s; null when not available; a new panel message when it changes by 0.1), `peer` (the Peer lamps, src/peer_id.hpp: {`lamp`: `signum` (the other station sent Signum's peer marker or encryption record on this link) / `other` (the marker on here, and 64 characters from the other station with no marker: an inference) / `off`, `version` (the other Signum's, from its record; empty otherwise), `marker` (1/0: the switch now, `PEERmark`), `link` (0 standby, 1 P1, 2 P2, 3 P3)}), `alc` (the ALC warning, src/alc_warn.hpp: {`state` `off` / `n/a` (no rigctld PTT, or the radio gives no ALC reading) / `ok` / `warn`, `max` (highest ALC of this link's bursts, 0-1), `bursts`, `over` (bursts at or above `limit`, 0.50), `on_link`, and for `warn` the `mode` (2/3), `level` and `gain` (TX gain, dB) it was given at}; not drawn on the front panel, the Status tab's Drive (ALC) card shows it), `t` | on every change (at most 20 a second), and after `hello` |
| `rx` | `text`: received link text; `backlog: true` on the first after `hello` (the last 4 KB) | as it arrives |
| `link` | `up`, `call`, `link` (the link's details, as `link_status` gives them: `role` caller/callee, `plevel`, and on the end `reason`, e.g. `QRT received`, `QRT acknowledged`, `timeout`) | link up / down |
| `link_logged` | `call` | stream `link`: a link ended (ask `links` again) |
| `log` | `t` (s), `text`: the modem's log | stream `log` (subscribe to it), and per `log_get` |
| `monitor` | `t`, `who`, `text` | listen mode: other stations' QSOs |
| `config` | `station`: the TNC's settings {mycall, mylevel, tone_lo, tone_hi, mode, listen, maxerr, txdelay, pska, fska, cpu_adaptive (1 adaptive, the default / 0 never: the `CPUadapt` command), p3_long_ceiling (read only: the highest level long packets are asked for as P3 receiver on this computer, 6 = all, 0 = none, as measured), tune_s, tune_hz, tune_db, tune_method (the tuning before a new call: seconds 0-10 with 0 = off, the default, the tone's length or the longest wait for the radio's own tune cycle; the tone's 300-2700 Hz and -30 to 0 dB; `"auto"` (default) / `"tone"` / `"radio"`: the radio's own tune through rigctld when possible, else the tone: the `TUnetone` command), maxup, maxdown, maxtry, maxsum, hostch (`PTChn`), tx_gain (dB on top of PSKAmpl, -40 to 20: `--tx-gain`, no command), max_key (the key-down watchdog, 1-600 s: `--max-key`, no command), pduplex (0 the default / 1: break-in on waiting text and changeover on an empty queue, every mode, the `PDuplex` command), pdtimer (2-30 s, default 12: `PDTimer`), peer_marker (1 the default / 0: the Signum peer marker on P2/P3 links, from the next link: `PEERmark`, `--peer-marker`), alc_warn (1 the default / 0: warn when the radio's ALC acts while we send P2/P3: `ALCwarn`, `--alc-warn`), cmsg (`CMsg`), status (`STatus`), escchr, qrtchr, bkchr, chochr (the converse-mode keys), urep (the unproto repetitions, 1-5: `U *n`), alsa, ptt, pty (read only: this run's)} | after `hello`, on `config_get`, and within a second of any change from anywhere (pty, shell, GUI) |
| `startup` | `startup`: the settings file ({`file` (its path; "" with `--no-config`), `read` (it was read at this start), `values` {option: value, as the file holds them now}, `running` {option: value, what this run was started with, from the file and the command line}, `options` [every option the file takes]}; logins as `USER:***`, `***`) | after `hello`, per `startup_get`, and after every `startup_set` (stream `config`) |
| `term` | `data`: output for the web terminal | after `term_open` |
| `result` | `id`, `ok` (false on an `ERROR` answer), `text` (the answer as the terminal shows it, no prompt) | per request |
| `error` | `id`, `error` | a request not understood, or not logged in |
| `pong` | `id` | per `ping` |
| `waterfall_info` | `id`, `on`, `rate`, `bin_hz` (9.375), `bins` (427), `db0` (-127.5), `db_step` (0.5) | per `waterfall` |
| `waterfall` | `t`, `tx`, `lo`, `hi`, `afc`, `row` (base64, one byte per bin from 0 Hz; level `db0 + byte * db_step` dBFS) | to clients that asked, `rate` rows a second |
| `links` | `id`, `kept` (true), `links`: [{id, start, end (unix s), open, call, role, modem, plevel, level_max, level_last, rx_bytes, tx_bytes}] | per `links`: this run's links, newest first (not kept across runs) |
| `about` | `id`, `version`, `built` | per `about` |
| `link_status` | `id`, `link` (see the request) | per `link_status` |
| `ic7300` | `event`: `detect` (`radios`: [{serial, port, dev, alsa, card, answered, addr, baud (0 = not measured), how, needs_b0_exit, error, changes, users (other programs with the port open), warning (the warning's text), settings [[option, value]] (what the settings file would get), rows [{key, name, now, new, change}]}]), `step` (`stage`, `text`, `ok`: 1 good, 0 failed, -1 information), `done` (`op`, `ok`, `written`, `verified`, `failed`, `backup`, `error`, and after a set-up `settings` [[option, value, why not written or ""]]), `backups` (`dir`, `files` [{path, serial, items, rf_power (a backup of `ic7300_power`: its restore writes RF POWER too)}], newest first) | the IC-7300 set-up (src/radio/ic7300.hpp; docs/md/radio-ic7300.md "Zero-effort set-up"): `detect`, `step`, `done` on stream `config` to every client; `backups` per `ic7300_backups` |

### To the TNC

| type | fields | does |
|---|---|---|
| `command` | `line` | one terminal command line (`MYcall`, `MYLevel 2`, `MAXUp 5`, `TONES 4`); the answer in `result`. `JHOST` is refused (host mode belongs to a stream client: `/shell` or `term_open`) |
| `connect` | `call` | as `C CALL` |
| `disconnect` | | as `D` |
| `changeover` | `break_in_only` (optional) | the host's Ctrl-Y (host mode `%O`): break in while receiving, change over while sending; with `break_in_only` only a break-in (`%I`). `result` ok false: nothing to do now (no link) |
| `link_status` | | a `link_status` message: `link` {`state` standby/calling/connected, `call` (the last one in standby), `role`, `plevel`, `level` (the speed level), `unsent` (bytes not yet sent, with those the encryption layer holds), `unacked`, `held` (bytes the encryption layer holds), `rx_bytes`, `tx_bytes` (this link), `link_s`, `reason` (why the last link ended; empty while one is up)} |
| `abort` | | as `DD` |
| `send` | `text` | text for the link (CR = new line) |
| `config_get` | | a `config` message |
| `config_set` | `station`: {key: value, ...}, keys from `config.station` | each applied as its terminal command; the answers in `result` |
| `startup_get` | | a `startup` message |
| `startup_set` | `options`: {option: value, or null / "" to remove it} | writes those options to the settings file (src/start_config.hpp: checked as the command line would take them; other lines kept; 0600); `result` is ok, or names what was refused (nothing of a refused option is written). They apply at the next start |
| `term_open` | | a stream client of the interpreter for this connection (as `/shell`, carried in `term` messages) |
| `term` | `data` | bytes typed into it |
| `term_close` | | detaches it (closing the connection does too) |
| `subscribe` | `streams`: any of `panel`, `rx`, `link`, `log`, `event`, `config`, `monitor` | replaces this client's subscriptions (default: all but `log`) |
| `get` | | the current `panel` at once |
| `log_get` | | the log kept so far (the last 500 lines), as `log` messages |
| `links` | `limit` (default 50) | the `links` message |
| `waterfall` | `on` (default true), `rate` (1-40 rows/s, default 10) | starts or stops the receive waterfall for this client |
| `auth` | `user`, `pass` | logs in (`--ws-auth`) |
| `ping` | | `pong` |
| `about` | | the `about` message |
| `ic7300_detect` | `profile` (optional: `external`) | finds every IC-7300 and reads its settings (no change): `ic7300` `detect`, then `done`. With `profile: "external"` the rows' `new` and `changes` are those of the external-modem set-up (and `settings` is empty); the same `profile` goes with `ic7300_backup` and `ic7300_setup`, and `done` and each radio carry `profile` (`signum`/`external`). Like every `ic7300_*` operation: refused while a link is up or the station is transmitting (opening the radio's port clears RTS/DTR, which our PTT shares) or while another one runs; the station's PTT is held unkeyed until `done` |
| `ic7300_backup` | `serial` | saves that radio's settings to a backup file (no change) |
| `ic7300_setup` | `serial`, `understood` (must be `true`), `allow_b0_exit` (optional), `profile` (optional: `external`) | the set-up: backup, writes, each read back, then the settings file (`alsa`, `ptt`, ...; **not** with `profile: "external"`, the set-up for a hardware modem on the radio's ACC socket: DATA MOD ACC, USB SEND and USB Keying OFF, Signum's settings file untouched); `step` messages, then `done`. Refused without `understood: true` |
| `ic7300_restore` | `file`, `understood` (must be `true`) | writes a backup file back to its radio (found by the serial number in the file) |
| `ic7300_backups` | | the `ic7300` `backups` message |
| `ic7300_power` | `watts` (1-100, the wanted P2/P3 peak), `serial` (optional with one radio) | The drive and RF POWER for that peak: the model's TX gain for that peak, chosen so that the ALC stays at rest on a hot radio too (20 W: -7 dB, 25 W: -7.5 dB), USB MOD Level 255 (written when it differs), RF POWER just above the peaks, using the live PSKAmpl (src/radio/ic7300_power.hpp; the model, never transmits; not during a link): `step` messages, then `done` with `op` `power`, `ok`, `lines` (the report), `before`, `after` (RF POWER /255), `backup`, `error`, and when OK `tx_gain_before`, `tx_gain` (the TX gain it chose, applied at once) and `tx_gain_saved` ("" or why the settings file was not written) |

```
→ {"type":"command","id":1,"line":"MYcall"}
← {"type":"result","id":1,"ok":true,"text":"Mycall: >N0AAA<\r\n"}
→ {"type":"connect","id":2,"call":"N0BBB"}
← {"type":"link","up":true,"call":"N0BBB"}
→ {"type":"send","id":3,"text":"HELLO\r"}
→ {"type":"disconnect","id":4}
← {"type":"link","up":false,"call":"N0BBB"}
```

## Encryption

Between two Signums only, off by default; **illegal on the amateur bands** (docs/md/encryption.md).
A build made without `./configure --enable-encryption` (the default) has none of this: `hello` carries `"crypto": false`,
there is no `crypto` stream or request (a `crypto` request is answered with an `error`), and the
GUI must show no encryption control at all. An encryption build: `"crypto": true`, the stream
`crypto` (subscribed by default) and the request `crypto`.

**From the TNC:** `{"type":"crypto","crypto":STATUS}` after `hello` and whenever anything changes
(a key added, a link's state, a setting; at most once per audio period). `STATUS`:

| Field | What |
|---|---|
| `available` | true |
| `on` | encryption on (applies to the next link; refused while one is up) |
| `ack` | the acknowledgement string the user must type to turn it on: `"NOT-AMATEUR"` |
| `ack_stored` | the acknowledgement is stored in the settings file: `set` with `on` then needs no `ack` |
| `warning` | the legal warning, to show whenever the user turns it on (and on the card) |
| `not_compatible` | the text saying it is not compatible with the AES of SCS modems |
| `no_plain` | the text saying it never falls back to clear text |
| `trust` | encryption's first-contact policy: `"tofu"` (default), `"stored"` or `"verified"` |
| `protection` | `"off"`, `"verify"` (identity check, data in clear) or `"on"` (encryption) |
| `sign` | verify: a tag on every record asked for (default false) |
| `verify_trust` | verify's first-contact policy: `"stored"` (default), `"tofu"` or `"verified"` |
| `compress` | compression before encryption offered (default true; `--encryption-compression`) |
| `wait_s` | the capability record and handshake timeout, s |
| `key_dir` | where the keys are |
| `key_dir_locked`, `key_dir_error` | whether this Signum holds the key directory's lock (one Signum per directory); if not, why (another Signum uses it: encryption cannot be turned on and the keys cannot be changed) |
| `have_key`, `fingerprint`, `public_block` | this station's key pair: exists, its fingerprint (`"1A2B 3C4D ..."`), its public key block (to show, copy, export) |
| `peers` | `[{call, fingerprint, verified, since (unix s)}]`: known stations |
| `received` | `[{call, fingerprint, when}]`: public keys received over the air, waiting to be accepted or rejected |
| `psks` | `[{name, id}]`: pre-shared keys (name: a call or `*`; id: 4 hex digits, equal at both ends for equal keys); never the keys |
| `link` | this link: `state` (`idle`, `plain` (encryption off), `waiting` (for the peer's capability record), `handshake`, `secure` (encrypted), `verified` (verify: the identity checked, the data in clear; never `secure`), `failed`), `protection` (`encrypt`, `verify`, or empty), `signed` (verify: every record carries a tag), `trust_level` (once up: `verified`, `stored`, `new` (stored at this first contact), `psk` (a pre-shared key set for that station), `psk-any` (the `*` pre-shared key)), `peer_kind` (`signum` once its record came, `other` after 64 characters with none, else empty), `reason` (why it failed), `call`, `role`, `held` (bytes waiting for the session), `record_ours`, `record_peer` (the capability records), `peer` ({product, version} or null), `features` ([{name: `encryption`/`compression` (verify: `verify`/`sign`), ours, theirs, agreed, ok, why}]), `pattern` (`IK`, `XX`, `NNpsk0`, `IKpsk2`, `XXpsk3`), `mode` (`pre-shared key`, `station keys`, `station keys + pre-shared key`), `sas` (8 digits to compare by voice), `peer_fingerprint`, `peer_verified`, `peer_key` (`known, verified` / `known, NOT verified` / `new: stored, NOT verified (trust on first use)`; `peer_verified` follows a verify or unverify made during the link), `psk` (the pre-shared key used: the station's own name, `*`, or empty), `last_failed` (`{call, reason}` of the last link's failure while `idle`, until the next link starts or encryption is turned on or off; else null), `changed_key` (`{call, old, new}`: a station presented a key other than the stored one and the link failed; the fingerprints; kept until the stored key is replaced or deleted or a later link with it succeeds; else null), counters `tx_records`, `rx_records`, `tx_bytes`, `tx_compressed`, `tx_chars` (characters handed to the modem), `rx_bytes` |

**To the TNC:** `{"type":"crypto","id":..,"op":OP, ...}`, answered by
`{"type":"crypto_result","id":..,"ok":bool,"text":"...", ...}` (`text`: what was done, or why not):

| op | fields | reply extras / effect |
|---|---|---|
| `status` | | `crypto`: STATUS |
| `set` | `on` (bool) with `ack` (must be `"NOT-AMATEUR"` to turn on), `protection` (`off`/`verify`/`on`; `on` with `ack` as `on`), `sign` (bool), `trust`, `verify_trust` (`stored`/`tofu`/`verified`), `wait_s` (10-600), `compress` (bool) | `on` refused while a link is up (`ack` not needed once `ack_stored`; a valid `ack` is stored in the settings file); this run only (the settings file: `startup_set` with `encryption` (`on`/`verify`), `encryption-ack`, `encryption-trust`, `verify-trust`, `sign`, `encryption-wait`, `encryption-compression`, `key-dir`) |
| `ack_withdraw` | | removes `encryption-ack` and `encryption` from the settings file and turns encryption off (refused while a link is up) |
| `keygen` | `force` (replace an existing pair) | `fingerprint` |
| `export` | `what`: `public`, `private`, `peer` (+`call`), `psk` (+`name`) | `block`: the text block (show `private`/`psk` only after a warning: they are secret) |
| `import` | `text` (one or more blocks, pasted), `call` (for a public block without a Call line), `force` (replace this station's key with an imported private key) | public keys become known stations (unverified), a private key this station's key, PSK blocks pre-shared keys |
| `peer_add` | `call`, `key` (44 base64 characters), `verified` | |
| `peer_verify` | `call`, `verified` (default true), `fingerprint` (optional: mark verified only if the stored key has this fingerprint; the GUI always sends the one it showed) | after the user compared the fingerprint or the SAS by another channel; refused (nothing marked) when the stored key is another one |
| `peer_replace` | `call`, `fingerprint` (the new key's, or at least its first 4 digits), `verified` | only after a link failed because `call` presented a key other than the stored one (`link.changed_key`): the stored key becomes the presented one |
| `peer_delete` | `call` | |
| `received_accept` | `call`, `verified` | a key received over the air becomes a known station |
| `received_reject` | `call` | |
| `send_key` | | sends this station's public key block on the current link (in clear on a plain link) |
| `psk_gen` | `name` | `psk_id` (the key's id; `id` is the request's) |
| `psk_set` | `name`, `key` | `psk_id` |
| `psk_phrase` | `name`, `phrase` (20+ characters) | `psk_id` |
| `psk_delete` | `name` | |

The console command `CRYPTO ...` does the same on every stream client (docs/md/encryption.md).

```
→ {"type":"crypto","id":7,"op":"set","on":true,"ack":"NOT-AMATEUR"}
← {"type":"crypto_result","id":7,"ok":true,"text":"ok"}
← {"type":"crypto","crypto":{"available":true,"on":true,...,"link":{"state":"idle",...}}}
```

## The GUI

`http://HOST:PORT/`, any browser, no external files. The CPU temperature is
in the title bar, and beside it the Computer lamp shows the `cpu` field (green
OK, yellow busy, red starved and blinking; the reason and the advice in its
tooltip and the Status tab's Computer card). Two seven-segment clocks (UTC and the browser's local time,
HH:MM:SS, 24-hour, drawn from the browser's clock, not from the WebSocket)
sit beside the Disconnect button. The front panel stays at the top: a fixed hardware-style
panel (docs/md/gui.md "The front panel") with the two-line display, the seven-segment
readouts, the lamps at fixed places, the tuning row and the Disconnect button
(`disconnect`; pressed again while `detail.qrt` is true or within 2 s, `abort`).
Tabs:

- **Status:** connect (disconnect is the panel's button); the decoding card (always shown, five rows, a dash while there is no link, so nothing below it moves); the receive waterfall; this run's links; received
  text and a terminal command line (or text to send).
- **Configure:** every user option (docs/md/gui.md "Configure tab"). Live settings: a key the
  TNC reports in `config` is set with `config_set`, any other with its command.
  Next-start settings (cards marked "applies at next start") are written with
  `startup_set` and show this run's value ("now: ..."). **Keep live settings at
  next start** writes the live ones that have a start-up option. Every field
  carries `data-opt`: the options and commands it stands for. Standing rule: a
  new user option gets a field here and an entry in `CONFIG_OPTIONS` in the
  same change.
- **Encryption** (only when `hello.crypto` is true; docs/md/encryption.md "In the GUI"): the
  Status tab's Encryption card (this link: state, method, SAS, the other key, the failure reason) and
  the Configure tab's Encryption cards (on/off now and at next start, both behind the warning dialog
  and the typed `NOT-AMATEUR`; compression, trust, timeout, key directory; key pair, import, known
  stations, received keys, pre-shared keys). The GUI subscribes to `crypto` and uses `crypto`
  requests; a failed link is only shown, never continued in clear. The front panel's **Crypto** lamps
  (Armed, Key / Fail, Secure, Verified; docs/md/gui.md "The front panel") are drawn from the same status
  (`crLampStates`); each lamp's tooltip has the detail, and a tap, click or Enter shows it in a box
  (`[data-tip]`, also the Computer lamp).
- **IC-7300 set-up:** the Radio & audio card has **Set up my IC-7300…**,
  **Set up my IC-7300 for an external modem…** and **Restore IC-7300 settings…**: a dialog driven by the `ic7300_*` requests
  (the warning, the checkbox that enables Reconfigure, each step as it comes).
- **Terminal:** the command interpreter as on the serial port (`term_open`); the
  interpreter does not echo typed characters, so the page echoes them locally
  (a "Local echo" switch, on by default, remembered per browser; Enter shows a
  new line only if the interpreter's answer does not start with one). Esc, Ctrl-D
  and Ctrl-Y buttons.
- **Log:** the modem's log.

`#status`, `#config`, `#terminal`, `#log` on the URL open that tab.

## The TUI

`tui/signum-tui [ws://HOST:PORT/] [--auth USER:PASS]`: the same panel in
a terminal (curses): the same lamps, labels, readouts and fixed layout, the
CPU temperature and the CPU health (`CPU OK|BUSY|STARVED`, with the reason and the advice on two lines below while it is not OK) on the top line, and the received text, with a command line (and, in a build with encryption, a `CRYPTO` lamp line, as the GUI's Crypto lamps, and an `ENCRYPTION` line: off, on,
setting up, ENCRYPTED with the SAS, or FAILED with the reason, also the last link's failure until the next link):
a terminal command and Enter (settings too, e.g. `CPUadapt 0`), `>text` sends link text, `/c CALL`, Ctrl-D the
Disconnect button (`D`; again while in progress, `DD`), `/d`, `/dd`, `/q`. `--once` prints one plain frame and exits. Python standard library
only.
