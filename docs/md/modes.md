# Modes and speed levels

Signum speaks three HF data modes, called P1, P2 and P3 here. They are the
three generations of one protocol family: each newer mode is faster and
needs a better channel, and every link starts the same way, so stations with
different capabilities still find each other.

## How a link works (ARQ)

A link joins two stations: the **caller** (who started it) and the
**callee**. At any moment one of them sends packets (the information sending
station) and the other answers each packet with a short **control signal**:
received, repeat, or "change speed". A packet that did not arrive is sent
again until it does (Automatic Repeat reQuest, ARQ), so what arrives is
exactly what was sent. The front panel's **Far end** lamps show what the other
station's last answer meant while you send: OK, Repeat, Faster, Slower or
Long/short ([The GUI](gui.md#the-front-panel)). The two directions take turns:

- **Break-in:** the receiving station takes the sending direction. Signum
  does this as the reference unit does, in every mode (P1, P2 and P3): when
  the sender hands the link over, when you press **Ctrl-Y**, or to end the
  link. **Text you type while receiving waits** until one of these: your
  text goes out when the other station hands over or when you press Ctrl-Y.
- **Changeover:** the sending station hands the direction over. As on the
  reference unit, Signum does this only when you press **Ctrl-Y** while
  sending (once what you typed so far has gone). When its text runs out
  without Ctrl-Y it keeps the link and sends idle packets; the other
  station's operator presses Ctrl-Y to answer. In a keyboard QSO, end each
  turn with Ctrl-Y ("over"), as with a reference unit.
- **The called station's first turn (P3):** text the called station has
  queued in the first second or so after its `CONNECTED` (until its first
  P3 burst is made) goes out at once in its first turn, after its connect
  text, with no Ctrl-Y; it then hands the link over by itself. Text typed
  after that waits as usual, and a Ctrl-Y typed while that first turn is
  going out (until the station is receiving, a few seconds after
  `CONNECTED`) is used up by it: press Ctrl-Y again. Both as on the
  reference unit. In P2 Signum sends no connect text and has no such
  first turn: an early Ctrl-Y breaks in as usual (the reference unit sends
  its connect text and what was typed by then in a first turn, and loses a
  Ctrl-Y typed while it goes out).
- **Automatic changeover (`PDuplex 1`):** GUI: Configure, Link parameters,
  "Auto break-in"; `--pduplex on`. Signum then breaks in by itself whenever
  text is waiting, except within `PDTimer` seconds (default 12) of the other
  station having broken in, and hands the link over by itself whenever its
  text has run out. This is what a mailbox or gateway program that never
  sends Ctrl-Y needs (the reference unit's manual calls it PACTOR duplex).
- **Winlink:** Pat, as the calling client, sends no changeover of its own; the
  station it calls (a Winlink gateway program) runs its modem with automatic
  changeover, which breaks in when the gateway has something to send and
  hands back when it is done. Pat over Signum therefore works as Pat over a
  reference unit, with Signum left at `PDuplex 0`. A program that answers
  calls on Signum (a gateway, a peer-to-peer listener) must set `PD 1` or
  send `%O`/`%I` itself, as it must on the reference unit. Pat cannot answer
  calls in these modes (its driver has no listener).

Because the receiver answers within a fixed, short gap (about 0.14 s after
each packet), the timing of the sound card, the computer and the PTT
matters: see [Radio set-up](radio-setup.md#ptt) and
[Hardware and performance](performance.md).

**Every link starts in P1**: the call is a P1 packet sent at 100 and
200 baud. If both stations allow P2 or P3 (`MYLevel`), they switch to the
highest mode both have during set-up. `MYLevel 1` keeps a link in P1,
`MYLevel 2` allows up to P2, `MYLevel 3` (the default) up to P3.

**Speed levels:** within a mode the link moves between speed levels by
itself: up after a run of good packets, down after bad ones, as the
reference unit does. The front panel's mode lamps show the mode (green) and
the level (amber), on two rows: TX, the level this station sends at, and RX,
the level the other station sends at. In P3 each direction has its own level,
set by how well the far end hears it, so the two often differ: a quieter
receiver at one end, more power at the other. The L/S lamp at the end of each
row shows long packets (green) or short ones (red) that way.

**Text compression:** text is packed with Huffman coding or PMC where that is
shorter than plain ASCII (`MOde 2`, the default: ASCII, Huffman or PMC,
whichever is shortest; `MOde 1` without PMC; `MOde 0` plain ASCII only).

## The modes in plain terms

| Mode | Panel | Speed levels | Signal | Typical use |
|---|---|---|---|---|
| **P1** | `100` `200` | 2: 100 and 200 baud | two-tone FSK (1400 / 1600 Hz), narrow | the call and set-up of every link; links with stations that have nothing newer; very poor channels |
| **P2** | `1` `2` `3` `4` | 4 | phase modulation on the same narrow channel | a narrow filter, older stations of the class |
| **P3** | `1` - `6` | 6 (SL1 to SL6) | up to 18 carriers, about 2.2 kHz wide | the fastest: e-mail over Winlink on a usable channel |

P2 and P3 send **short** packets (a 1.25 s cycle) or **long** packets (a
3.75 s cycle, a little more throughput at the same level). The receiving
station asks for long packets when it can decode them in time; see
[Adaptive CPU features](performance.md#adaptive-cpu-features-cpuadapt).

### What to expect (measured)

Characters delivered, with compression on, from Signum's own runs (not
nominal rates: real throughput depends on the channel and the text):

| Set-up | Result |
|---|---|
| P1, Signum calling a reference unit on the bench (clean channel) | about 24 characters a second |
| P2, a reference unit calling and sending to Signum (clean, 150 s) | 8,674 characters |
| P3, a reference unit sending 12,000 characters to Signum on a Raspberry Pi 4 (version 0.4.0: long packets up to SL4 there) | 99-108 s |
| P3, two Signums on one Raspberry Pi 4 (loopback), 20,000 characters (long packets up to SL6 as receiver) | 40 s, long SL6 |
| P3, Signum on a Raspberry Pi 4 sending 12,000 characters to a reference unit | 45-46 s (long SL6) |
| P3, two Signums on one x86 computer (loopback, no noise) | 8,000 characters in 24 s, at SL6 |

## Listen mode (monitoring)

`Listen 1` (or **Listen mode** in the GUI, or `--listen on` at start) decodes
other stations' QSOs while Signum is idle: P1, P2 and P3 links, both sides,
and unproto transmissions. The text appears in the GUI's Status tab (in
purple), on the TUI and on the serial port. Default: on (`Listen 1`), as on
the reference unit. It pauses during our own links and runs below the modem's
real-time work, so it never delays a link. Its cost while idle on a Raspberry
Pi 4: about a fifth of one core hearing noise, about half of one core hearing
a P3 link (measured 2026-11-16, [Performance](performance.md)). `Listen 0` or
`--listen off` saves that. In Listen mode the busy status byte is not given in
stand-by (as on the reference unit); the GUI's busy lamp still works.

## Unproto (broadcast, no ARQ)

Unproto sends text to anyone listening, with no answers and no repeats on
request: each packet is simply sent a set number of times. It is used for CQ
calls and bulletins.

```
U *4         set the number of repetitions (1-5; default 2)
U 3          start an unproto transmission in mode 3; then type the text
Ctrl-D       end it in an orderly way (QRT; also Esc, then D)
Esc, DD      end it at once
```

While the transmission runs, what you type is text to send, as during a
link; Esc gives one command line.

| Mode | What is sent |
|---|---|
| 1, 2 | P1 FSK, 100 / 200 baud |
| 3-6 | P2 short cycle, levels 1-4 (DBPSK, DQPSK, 8-DPSK, 16-DPSK) |
| 7-10 | P2 long cycle, levels 1-4 |
| 30-35 | P3 short packets, SL1-SL6 |
| 36-41 | P3 long packets, SL1-SL6 |

`U` alone uses the last mode (default 1). Text is packed as the reference
unit packs it.

**Receiving unproto:** in Listen mode Signum prints received unproto text as
`UNPROTO>` lines, for modes 1-10. P3 unproto (modes 30-41) is received only
with `--p3-unproto-rx`: the reference unit has no receiver for it, so the
default matches it and it is off.
