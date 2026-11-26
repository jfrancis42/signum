# Getting started

This page takes you from a fresh build to a first link: first two stations
on one computer with no radio, then a real radio.

## 1. Try it without a radio: `signum-loopback`

`signum-loopback` starts two Signum stations on this computer, joined by
the ALSA loopback driver (`snd-aloop`) as if by two radios: what one plays,
the other hears. Nothing is keyed (`--ptt none`).

```
./signum-loopback            # start stations TESTA and TESTB
./signum-loopback status     # what runs, with the addresses
./signum-loopback stop       # stop both
./signum-loopback reset      # delete both stations' settings and keys
```

(Installed, the commands are `signum-loopback`, without `./`.)

The first start loads `snd-aloop` with `sudo modprobe snd-aloop` and asks
for your password. `stop` tries to unload it again if the script loaded it.

| | Station A | Station B |
|---|---|---|
| call | `TESTA` | `TESTB` |
| GUI | http://127.0.0.1:8771/ | http://127.0.0.1:8772/ |
| telnet | `127.0.0.1:3001` | `127.0.0.1:3002` |
| host-mode serial port (pty) | `/tmp/ttysignumA` | `/tmp/ttysignumB` |
| sound device | `plughw:CARD=Loopback,DEV=0,SUBDEV=0` | `plughw:CARD=Loopback,DEV=1,SUBDEV=0` |
| log | `~/.local/state/signum/loopback-A.log` | `~/.local/state/signum/loopback-B.log` |
| settings and keys | `~/.local/state/signum/loopback/A/` | `~/.local/state/signum/loopback/B/` |

Each station has its own settings file (what its Configure tab saves) and its own
encryption keys, so the two behave like two separate computers, and your own
`~/.config/signum` is not touched. Two stations must never share a key directory: they
would have one station key between them and overwrite each other's list of known stations,
and encryption between them then fails.

To make a link, open station A's GUI, type `TESTB` in **Link control** and
press **Connect** (or, in A's Terminal tab, type `C TESTB` and Enter). Then
type text: B's GUI shows it. The front panel shows the link climbing through
the speed levels. **Disconnect** on the front panel ends it.

Options:

- `--calls A B` uses other call signs.
- Options after `--` go to both stations, e.g.
  `./signum-loopback -- --mylevel 2` for a P2 link.
- `--bin PATH` runs another `signum` binary.

Real-time priority is not needed on the loopback; the script says when the
stations run without it. The stations run with self-calibration off
(`--no-autocal`), because there is no radio to measure.

## 2. Start Signum for a radio: `signum-start`

```
./signum-start --call N0CALL --alsa plughw:CARD=CODEC --ptt rts:/dev/ttyUSB0
```

Replace `N0CALL` with your call sign, and the sound card and PTT with yours
([Radio set-up](radio-setup.md) explains how to find them).

**An Icom IC-7300 on USB:** let Signum set the radio up and find the sound
card and PTT itself (it saves a backup of the radio's settings first and asks
you to type `YES`; it never changes the power or frequency):

```
./build/signum --configure-my-ic7300-and-overwrite-its-settings
./signum-start --call N0CALL
```

([Zero-effort set-up](radio-ic7300.md#zero-effort-set-up); the GUI has the
same as **Configure » Radio & audio » Set up my IC-7300…**.)

`signum-start` starts `signum` and the terminal front panel (TUI) together
and prints the GUI's address:

```
signum: GUI http://127.0.0.1:8770/   (log: /home/you/.local/state/signum/tnc.log)
```

- Open **http://127.0.0.1:8770/** in a browser on the same computer for the
  GUI ([The GUI and the TUI](gui.md)).
- The terminal shows the TUI. Quitting it (`/q` or Ctrl-C) stops Signum.
- While the TUI has the terminal, Signum's own log goes to
  `~/.local/state/signum/tnc.log` (or `$XDG_STATE_HOME/signum/tnc.log`).
- `--no-tui` runs Signum alone in the foreground, its log on the terminal.
- Every other option goes to `signum` ([Command-line options](options.md)).
- `--ws ADDR:PORT` and `--ws-auth USER:PASS` move the GUI and add a login;
  `signum-start` passes both to the TUI too.

You can also run `signum` directly with the same options.

### What to check on the first start

The log's first lines tell you what Signum found. Look for:

- `[rt] no real-time priority ...`: real-time priority is missing. Fine for
  a try, but set it up before working stations
  ([Real-time priority](performance.md#real-time-priority)).
- `[cal] ...`: the timing calibration in use. Without a calibration file
  Signum calibrates itself from the first links
  ([Timing calibration](radio-setup.md#timing-calibration)).
- `[ptt] rts:/dev/ttyUSB0, key-down watchdog 90 s`: the PTT method.
- `[ws] GUI ...`: the GUI's address.
- the CPU clock line: whether Signum can hold the CPU clock up during a
  link ([CPU clock](performance.md#cpu-clock)).

## 3. A first link on the air

1. Set the radio for data: USB, a 2.4 kHz or wider filter, compression and
   speech processing off, the interface's VOX keying **off**
   ([why](radio-setup.md#ptt)).
2. Set the output level: start low and follow
   [Audio levels](radio-setup.md#audio-levels).
3. Call a station you have arranged a test with: `C THEIRCALL` in the
   Terminal tab or the TUI, or **Connect** in the GUI.
4. Watch the front panel: **Connect/Call** red while calling, green once
   linked; the mode lamp (P1, P2 or P3) and the speed level light; **Quality %**
   and **Offset Hz** show how the link goes.
5. Type to send. **Disconnect** (or `D`) ends the link in an orderly way;
   pressing it again while its lamp is lit drops the link at once (`DD`).

Mail with Pat: see [Connecting programs](connections.md#pat-winlink).

## Next

- [The GUI and the TUI](gui.md)
- [Radio set-up](radio-setup.md)
- [Running Signum as a service](running.md)
