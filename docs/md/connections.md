# Connecting programs

Signum offers its command interpreter in four ways. Each connection gets its
own interpreter (its own terminal or host mode), and all of them act on the
one modem, so a program on the serial port and a person on telnet see the
same link.

| Connection | Default | For |
|---|---|---|
| host-mode serial port (pty) | `/tmp/ttysignum0` | Pat and other host programs; serial terminal programs |
| telnet / raw TCP | `127.0.0.1:3000` | a person with `telnet` or `nc`; programs with a plain socket |
| WebSocket command shell | `ws://127.0.0.1:8770/shell` | programs that prefer WebSockets |
| JSON WebSocket API | `ws://127.0.0.1:8770/` | the GUI, the TUI, your own front ends |

For your own programs in Python, the Python SDK (a separate project, hf-tools) drives Signum through the
host-mode port and the JSON API together (links, exact binary data, parameters, status, encryption).
| browser GUI | `http://127.0.0.1:8770/` | people ([The GUI and the TUI](gui.md)) |

Everything listens on `127.0.0.1` (this computer only) unless you say
otherwise.

## The host-mode serial port (pty)

Signum creates a pseudo-terminal and links it at `/tmp/ttysignum0`
(`--pty PATH` for another name, `--pty none` for none). To a host program it
looks like the serial port of a hardware modem of the classic class: the
terminal-mode command set with the `cmd:` prompt, and WA8DED host mode, plain
(`JHOST1`) or with CRC (`JHOST4`). The baud rate a program sets is ignored.

A person can use it too: `picocom /tmp/ttysignum0` (or `minicom`, `screen`),
then Enter for the `cmd:` prompt. See [Console commands](commands.md).

### Pat (Winlink)

In Pat's `config.json` (the `"pactor"` key and the `pactor:///` scheme are
Pat's own names for this class of modem):

```json
"pactor": {"path": "/tmp/ttysignum0", "baudrate": 57600, "rig": "", "custom_init_script": ""}
```

Then:

```
pat connect pactor:///CALL
```

- Signum answers everything Pat's driver sends: its set-up commands in
  terminal mode (`MYcall`, `PTCH 31`, `MAXE 35`, `TONES 4`, `MODE 0` and
  others), then CRC host mode (`JHOST4`) for the connection itself.
- Pat sets the host-mode channel to 31 (`PTCH 31`); Signum's default is 4.
- Pat may log an occasional "Wrong checksum in package!" warning; it comes
  from how Pat's driver polls and recovers on its next poll.
- Pat's `"rig"` setting is for frequency control through Hamlib, which Pat
  does itself; Signum does not control the radio's frequency. For Pat to
  change the frequency (QSY) see [Pat changing the frequency](#pat-changing-the-frequency-qsy).
- Signum's own tests run a full Winlink exchange between two Pats over two
  Signums (one message each way).

### Pat changing the frequency (QSY)

Pat changes the radio's frequency through Hamlib's `rigctld`. If Pat logs
`Unable to get frequency from rig ...` or reports a QSY failure, no `rigctld`
is answering at the address in Pat's `hamlib_rigs`.

**A radio with one USB serial port (the IC-7300 and most Icom radios) must
have ONE program on that port.** Do not start `rigctld` on the port Signum
keys with `rts:`: a second program opening the port, and setting its speed,
raises RTS for an instant, and a radio with USB SEND = RTS transmits. Let
`rigctld` own the port, keying with the RTS line exactly as Signum would,
and give Signum `--ptt rig` so it keys through `rigctld`:

1. `rigctld` on the radio's port, localhost only (anything that can reach it
   can key the radio). IC-7300 (Hamlib model 3073, CI-V address 94h; use the
   radio's CI-V USB Baud Rate):

   ```
   rigctld -m 3073 -r /dev/serial/by-id/usb-Silicon_Labs_CP2102_USB_to_UART_Bridge_Controller_IC-7300_<serial>-if00-port0 \
           -s 115200 -c 0x94 -P RTS -p /dev/serial/by-id/...same port... \
           -T 127.0.0.1 -t 4532 --set-conf=dtr_state=OFF
   ```

   `-P RTS -p` keys with the port's RTS line (the radio keeps USB SEND = RTS);
   `dtr_state=OFF` keeps DTR low (USB Keying (CW) may be DTR on some radios).

2. Signum keys through it: `--ptt rig:127.0.0.1:4532` (or `ptt =
   rig:127.0.0.1:4532` in `signum.conf`). Signum sends only `T 1` / `T 0`.

3. Pat's `config.json`, the rig named in `"pactor"`:

   ```json
   "hamlib_rigs": {"radio": {"network": "tcp", "address": "127.0.0.1:4532", "VFO": ""}},
   "pactor": {"path": "/tmp/ttysignum0", "baudrate": 57600, "rig": "radio", "custom_init_script": ""}
   ```

   Give the address as `127.0.0.1`, not `localhost`: Pat tries `::1` first,
   where a `rigctld` bound to `127.0.0.1` does not answer.

Start `rigctld` before Signum (Signum's keying needs it) and Pat after
both. As systemd services: [Signum, rigctld and Pat as
services](running.md#signum-rigctld-and-pat-as-services). The first open of
the port after the radio is switched on still raises the lines once, before
`rigctld` can clear them (see [PTT](radio-setup.md#ptt)).

Keying through `rigctld` adds a localhost round trip (about a millisecond on
a Pi 4) to each key-up and key-down. It has not been measured yet with
PACTOR links on the bench; `rts:` straight from Signum stays the recommended
keying when nothing else needs the port.

### Other host programs

Any program written for a hardware modem with the classic terminal-mode
commands and WA8DED/CRC host mode should work on the pty, the telnet port or
`/shell`. Only Pat is tested. Differences from the reference firmware that a
program might notice are listed in [Console commands](commands.md#differences-from-the-reference-firmware).

### Running a gateway or peer-to-peer listener: set `PDuplex 1`

If your station **answers calls** unattended (a Winlink gateway, a mailbox,
a peer-to-peer listener), turn on automatic changeover: **`PDuplex 1`**
(`PD 1` for short).

- **Off by default**, as on the reference unit. With `PD 0` Signum breaks
  in only when the sending station hands over, or when someone presses
  Ctrl-Y (host mode `%I`), and hands over only on Ctrl-Y (`%O`). An
  unattended station presses nothing, so after a call it never takes the
  link to send its greeting, and the caller waits until the link times out,
  unless the calling operator presses **Ctrl-Y** to hand over.
- **With `PD 1`** Signum breaks in by itself whenever text is waiting and
  hands over by itself whenever its text has run out, in every mode.
  `PDTimer` (2-30 s, default 12) is how long it holds back from breaking in
  after the other station broke in.
- **Real gateways do this on the reference unit too:** gateway software sends
  `PD 1` at start-up (LinBPQ, for example, sets `PDUPLEX 1` and `PDTIMER 5`).
  A program that sends `PD 1` itself, or drives `%I` / `%O` itself, needs
  nothing in Signum.
- **For a program that does not**, set it in Signum: GUI **Configure »
  Link parameters » Auto break-in** (and **Keep live settings at next
  start**), `pduplex = on` in the [settings file](options.md#the-settings-file)
  (with `pdtimer = 5` if you want the gateways' value), or `--pduplex on`.
  `PD` alone shows the setting.
- **The calling side does not need it:** Pat as a calling client works with
  Signum at `PD 0`, because the gateway it calls hands over and breaks in.
  Pat cannot answer calls in these modes (its driver has no listener).
  Leave it off for keyboard QSOs too, where each operator ends a turn with
  Ctrl-Y.

More: [Modes, how a link works](modes.md#how-a-link-works-arq),
[Troubleshooting](troubleshooting.md#a-program-answering-calls-hangs-at-the-start-gateway-peer-to-peer).

### The peer marker: is the other station a Signum?

On every P2 and P3 link Signum sends a short **peer marker**, so that two
Signums recognise each other. The front panel's **Peer** lamps show the
result ([The GUI](gui.md#the-front-panel)): **Signum** (green) or **Other**
(amber).

- **What goes on the air:** one system-information sequence of the P1/P2/P3
  byte stream, `0x1C` `X` `FE1;pSignum;v0.5.1` and a space (the version is
  this Signum's). The protocol defines such sequences for exactly this: a
  receiver skips one it does not know. The reference unit skips it silently:
  it shows nothing, gives nothing to its host program and changes nothing in
  the link. It is plain ASCII, carries no message and hides nothing: any
  monitor that decodes the link can read it, and this page documents it.
- **When:** once per link, P2 and P3 only, never on P1 (other P1 equipment
  may print it). Signum sends it only while it holds the sending direction
  anyway: as the caller at once, otherwise at its first turn, or just before
  its first text. So it never causes a changeover or a break-in and never
  delays a disconnect you ask for while receiving. It costs about 22
  characters per link.
- **With encryption on,** the encryption's capability record does the same
  job ([Encryption](encryption.md#how-it-works-briefly)), and no separate
  marker is sent.
- **On by default** (the owner's decision): the reference unit has no such
  marker, so this is a deliberate difference from it. **Turn it off** with
  `PEERmark 0` (console, `/shell`, telnet), `--peer-marker off` or
  `peer-marker = off` in the [settings file](options.md#the-settings-file),
  or the GUI's **Configure » Peer marker**. It takes effect at the next
  link. Off, Signum sends nothing extra; it still shows **Signum** when the
  other station identifies itself (its own marker, or an encrypted link),
  and never shows **Other**.
- **"Other" is an inference, not a proof:** with the marker on, a Signum
  sends it before its own text, so when the other station has sent 64
  characters with no marker, it is not a Signum with its marker on. That is
  most likely the reference unit or another P1/P2/P3 modem, but an older
  Signum (before the marker existed) or a Signum with its marker off looks
  the same. If a marker arrives later after all, the lamp turns to Signum.
  Until 64 characters have come both lamps stay dark, so a link where the
  other station sends nothing stays unknown.
- **Not tested:** other manufacturers' modems whose firmware is not
  published. The protocol's rule says they skip the sequence too. If a
  station ever shows text like `FE1;pSignum` after a link with you, turn
  the marker off.

## Telnet / raw TCP (port 3000)

```
telnet 127.0.0.1 3000
nc 127.0.0.1 3000
```

Each TCP client gets its own attachment to the command interpreter, exactly
as the pty: terminal mode and `JHOST1`/`JHOST4` host mode.

| Option | Effect |
|---|---|
| `--telnet PORT` | listen on PORT (default 3000); `0` turns it off |
| `--telnet-bind ADDR` | the address (default `127.0.0.1`; `0.0.0.0` for every interface) |
| `--telnet-auth PASS` | a `Password:` prompt before the interpreter attaches (3 tries) |

- If the default port 3000 is taken, Signum says so and runs without
  telnet. A port you give with `--telnet` that cannot be opened stops it.
- A plain socket gets exactly the bytes a serial port would. A real telnet
  client (one that negotiates) gets its typed characters echoed by Signum,
  since the interpreter itself does not echo.
- Host mode is binary-clean: from `JHOST1`/`JHOST4` until `JHOST0` the
  connection carries raw frames with no telnet processing either way. Type
  the password (if any) before entering host mode.

## The WebSocket command shell: `ws://127.0.0.1:8770/shell`

A raw byte stream to the command interpreter over a WebSocket, exactly as a
program on the pty gets it: terminal mode and `JHOST1`/`JHOST4` host mode.

- Signum sends binary frames holding exactly the bytes a serial port would
  carry. Send binary frames (text frames are accepted too). Frame boundaries
  mean nothing; end command lines with CR.
- Closing the WebSocket is like unplugging a serial cable.

```python
import websocket                                   # pip install websocket-client
ws = websocket.create_connection("ws://127.0.0.1:8770/shell")
ws.send_binary(b"MYcall\r")
print(ws.recv())                                   # b'MYcall\r\nMycall: >N0CALL<\r\ncmd: ' (in one or more frames)
```

`ws-shell-example.py` (in `tui/` of the source tree, installed beside the
TUI) is a complete client: interactive, one-shot commands
(`-c "MYcall" -c "MYLevel 2"`), and a CRC host-mode demo (`--host`).

## The JSON WebSocket API: `ws://127.0.0.1:8770/`

The GUI and the TUI use this API; your own programs can too. Every frame is
one JSON object with a `"type"`: Signum pushes the front panel, received
text, link events, the log, Listen-mode text, the settings and the waterfall;
clients send commands, connect / disconnect, text, and settings changes.

```
→ {"type":"command","id":1,"line":"MYcall"}
← {"type":"result","id":1,"ok":true,"text":"Mycall: >N0AAA<\r\n"}
→ {"type":"connect","id":2,"call":"N0BBB"}
← {"type":"link","up":true,"call":"N0BBB"}
→ {"type":"send","id":3,"text":"HELLO\r"}
→ {"type":"disconnect","id":4}
```

The full protocol (every message and field) is in
[src/web/README.md](../../src/web/README.md) in the source tree.

| Option | Effect |
|---|---|
| `--ws [ADDR:]PORT` | where the GUI, the API and `/shell` listen (default `127.0.0.1:8770`; `0.0.0.0:8770` for every interface) |
| `--no-ws` | none of them |
| `--gui DIR` | serve the GUI from another directory |

## Logins and security

Signum is open by default, but only to programs on the same computer.
Anyone who can reach its ports can control it **and key your radio**.

| Option | Protects |
|---|---|
| `--web-auth USER:PASS` | the GUI pages (HTTP Basic login in the browser) |
| `--ws-auth USER:PASS` | the JSON API and `/shell` |
| `--web-auth-file F`, `--ws-auth-file F` | the same, read from the first line of file F that is not a `#` comment (Signum warns if others can read the file) |
| `--telnet-auth PASS` | the telnet port (a password prompt) |

- Listening on another address than 127.0.0.1 without both web logins makes
  Signum print a warning at start.
- There is no encryption (no TLS). Across a network you do not trust, use an
  SSH tunnel instead of opening the ports:
  `ssh -L 8770:127.0.0.1:8770 -L 3000:127.0.0.1:3000 radio-pi`.
- With `--ws-auth`, programs log in with an `Authorization: Basic` header,
  `?auth=BASE64(USER:PASS)` on the URL, or (JSON API only) an
  `{"type":"auth","user":...,"pass":...}` message first. The TUI takes
  `--auth USER:PASS`.

## Two Signums on one computer

Give each its own pty, WebSocket port and telnet port, e.g.
`--pty /tmp/ttysignum1 --ws 127.0.0.1:8771 --telnet 3001`. A WebSocket port
in use stops the second one. `signum-loopback` does this for you.
