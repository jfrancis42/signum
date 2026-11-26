# Console commands and host mode

The command interpreter is the same on every connection: the serial port
(pty), telnet, the WebSocket shell, the GUI's Terminal tab and the TUI's
command line. It follows the classic terminal-mode command set of hardware
modems of this class, with the same answers, so host programs and operators
used to that set feel at home.

## Terminal mode

- Press Enter for the `cmd:` prompt. A command line ends with CR (Enter).
- Commands may be shortened to their capital letters: `MYcall` can be typed
  `MY`, `MYLevel` as `MYL`, `Connect` as `C`. Case does not matter.
- A command without a value shows the current value; with one, sets it.
- An unknown command answers `*** ERROR: PSE TYPE HELP`; a value out of
  range, `*** BAD ARGUMENT`.
- The interpreter does not echo what you type (the GUI and telnet clients
  echo it locally).
- `Help` lists the commands.

### During a link (converse mode)

From `C CALL` until the link ends, what you type is sent to the other
station.

| Key | Does |
|---|---|
| text, Enter | sent on the link |
| Esc | one command line (`cmd:`), e.g. `MYL`, `CPUL`, `D` |
| Ctrl-D | orderly disconnect (QRT): what is queued is sent, then the link ends |
| Ctrl-Y | while receiving: break in (take the sending direction); while sending: change over (hand it over once what you typed so far has gone) |

**While receiving, text you type waits** (every mode, as on the reference
unit): it goes out when the other station hands the link over, or at once
when you press Ctrl-Y (`BKchr`). **While sending, Signum hands the link over
only when you press Ctrl-Y** (`CHOchr`), once what you typed so far has gone;
when its text runs out without it, it keeps the link and sends idle packets.
With `PDuplex 1` Signum does both by itself: it breaks in whenever text is
waiting and hands over whenever its text has run out (see below). Details:
[Modes](modes.md#how-a-link-works-arq).

Link messages: `*** NOW CALLING CALL`, `*** CONNECTED TO CALL`,
`*** NO RESPONSE FROM CALL`, `*** DISCONNECTED AT ...`,
`*** TIMEOUT - DISCONNECTED AT ...`, then the stand-by text `*** STBY >>`.

## Commands

### Links

| Command | What it does |
|---|---|
| `Connect CALL` | calls CALL. `C` alone calls the last call again. `C !CALL` uses the long path |
| `Disconnect`, `Qrt` | orderly disconnect (also stops a call in progress, and the tuning tone before it) |
| `DD` | disconnect at once |
| `Unproto [mode]`, `U *n` | an unproto transmission; `*n` sets the repetitions (1-5, default 2). See [Unproto](modes.md#unproto-broadcast-no-arq) |
| `Listen 0\|1` | Listen mode: decode other stations' QSOs while idle (default 1, as the reference unit) |

### Station settings

| Command | Values | Default | Notes |
|---|---|---|---|
| `MYcall [CALL]` | call sign | `--call`, else `N0CALL` | |
| `MYLevel [1-3]` | highest mode: 1 = P1, 2 = P2, 3 = P3 | 3 (`--mylevel`) | also shows the level of the current or last link |
| `MOde [0-2]` | compression: 0 ASCII, 1 Huffman, 2 Huffman or PMC | 2 | |
| `TOnes [0\|1\|2\|4]` | FSK tones: 0 = 1200/1400, 1 = 2100/2300, 2 = the `MARk`/`SPAce` pair, 4 = 1400/1600 | 4 (`--tones`) | |
| `MARk [Hz]`, `SPAce [Hz]` | 300-3000 | 1600, 1400 | used with `TOnes 2` |
| `MAXErr [30-255]` | link timeout: bad cycles before a link or a call is given up; 255 = never | 70 | |
| `TXDelay [1-31]` | × 5 ms: the FSK lead-in after keying | 4 (20 ms) | |
| `PSKAmpl [10-1000]` | output level, mV peak-to-peak on the reference unit | 140 | [Audio levels](radio-setup.md#audio-levels) |
| `CMsg [0-2]` | the connect text sent when we are called and the link is P3 (0 = none) | 1 | the text is `Signum: P1/P2/P3 station` |
| `PTChn [1-31]` | the host-mode channel of the modem | 4 | Pat sets 31 |
| `STatus [0-2]` | status byte reporting; 2 = sent by itself on every change | 1 | RS (0x1E) asks for the status byte |
| `ESCchr`, `QRTChr`, `BKchr`, `CHOchr` | the converse-mode keys (ASCII codes) | 27 (Esc), 4 (Ctrl-D), 25 (Ctrl-Y), 25 | |
| `PDuplex [0\|1]` | automatic changeover, every mode: 0 = break in only on the sender's changeover, Ctrl-Y or a disconnect, and hand over only on Ctrl-Y (the reference unit); 1 = also break in whenever text is waiting and hand over whenever our text has run out (what a mailbox or gateway host program sets) | 0 (`--pduplex`) | answer `*** PACTOR DUPLEX: 0`; `PD` for short |
| `PDTimer [2-30]` | with `PDuplex 1`: seconds after the other station broke in during which ours waits for a changeover or Ctrl-Y | 12 (`--pdtimer`) | answer `*** PACTOR DUPLEX BREAKIN TIME: 12`; `PDT` for short |

Settings given at start-up (`--call`, `--mylevel`, `--tones`, `--listen`, `--pduplex`, `--pdtimer`)
can be changed with these commands while Signum runs. Commands do not save
anything: Signum starts from its defaults, the [settings file](options.md#the-settings-file)
and the command line each time (the GUI's Configure tab can keep the live
settings in the settings file). Every setting here is also on the GUI's
Configure tab.

### Signum's own

These do not exist on the reference unit (none of its commands starts with
`CP`, `CR`, `PE` or `TU`):

| Command | What it does |
|---|---|
| `CPULoad` | whether this computer keeps up: `*** CPU LOAD: OK`, `BUSY` or `STARVED`, with what was late in the last minute and which causes apply here. See [CPU health](performance.md#cpu-health-ok-busy-starved) |
| `CPUadapt [0\|1]` | adaptive CPU features: 1 (default) or 0 (never). The answer also shows how far long P3 packets are used on this computer. See [CPUadapt](performance.md#adaptive-cpu-features-cpuadapt) |
| `CRYPTO [...]` | encryption between two Signums (off by default; **illegal on the amateur bands**): `CRYPTO ON NOT-AMATEUR` (the phrase only the first time: it is then stored), `VERIFY` (identity check without encryption, no acknowledgement), `SIGN ON|OFF`, `TRUST [ENCRYPT|VERIFY] STORED|TOFU|VERIFIED`, `OFF`, `ACK OFF` (withdraw it), `STATUS`, keys, pre-shared keys. The full word only (no abbreviation); absent from a `--no-encryption` build. See [Encryption](encryption.md#commands-and-options) |
| `ALCwarn [0\|1]` | the ALC warning: 1 (default) reads the radio's ALC meter through rigctld while we send P2/P3 and warns (once per link, in the log and on the GUI's Drive (ALC) card) when the ALC squashes the PSK peaks; 0 does not read it. `ON` and `OFF` are accepted too; `ALC` is enough. The answer is `*** ALC WARNING (0=OFF/1=ON): 1  (ON)`, with this link's numbers once bursts were read, or `(NOT AVAILABLE (THE PTT IS NOT RIG, A RIGCTLD))` / `(NOT AVAILABLE (THE RADIO GIVES NO ALC READING))`. Warns only. See [ALC warning](radio-ic7300.md#alc-warning) |
| `PEERmark [0\|1]` | the Signum peer marker: 1 (default) sends it on each P2/P3 link, 0 does not; from the next link. `PEER` is enough. The answer is `*** PEER-MARKER: 1`; during a link it also names the other station: `*** PEER-MARKER: 1  (OTHER STATION: SIGNUM 0.5.1)`, `(OTHER STATION: NOT A SIGNUM (NO MARKER IN ITS FIRST 64 CHARACTERS))`, `(OTHER STATION: NOT KNOWN YET)`, `(OTHER STATION: NOT KNOWN (MARKER OFF))` or `(THIS LINK: P1, NO MARKER)`. The same as the front panel's Peer lamps. See [The peer marker](connections.md#the-peer-marker-is-the-other-station-a-signum) |
| `ICPower [W]` | the IC-7300's drive and RF POWER for P2/P3 peaks of W watts (1-100): the model's TX gain for W, chosen so that the ALC stays at rest even on a hot radio (20 W gives -7 dB, 25 W -7.5 dB), USB MOD Level 100 % (written if it differs), RF POWER just above the peaks, with this station's PSKAmpl (the model; never transmits; not during a link). The TX gain chosen applies at once and is saved in the settings file. It works in the background: `ICP 20` answers `*** IC-7300 POWER: WORKING ON 20.0 W PEAKS (TX GAIN -7.0 DB, PSKAMPL 140); ICPOWER ALONE SHOWS THE RESULT`; `ICPower` alone then shows `*** IC-7300 POWER: SET 87/255 FOR 20.0 W PEAKS (WAS 51/255)` or `NOT SET (NOTHING CHANGED)` with the expected peaks and P1 power, the TX gain old → new and whether the settings file took it. `ICPower W MEASURE` is refused (measuring needs the sound card: `signum --ic7300-power W --measure` with Signum stopped). See [Setting the power](radio-ic7300.md#setting-the-power) |
| `TUnetone [S [HZ [DB]]] [auto\|tone\|radio]` | the tuning before each new call we place: S seconds (0 = off, the default; 1-10; the longest wait for the radio's own tune, the length of the tone), HZ the tone's frequency (300-2700, default 1500), DB its level against the data level (-30 to 0, default 0), and the method (default `auto`: the radio's own tune cycle through rigctld when the PTT is `rig:` and the rig supports TUNE, else the tone; `tone`; `radio`: the radio's tune, the tone when impossible). The method word may stand anywhere: `TU 5 radio`, `TU tone`. `TU` alone shows the setting: `*** TUNING TONE BEFORE A NEW CALL: 0 (OFF), 1500 HZ, 0 DB, METHOD AUTO`. See [Tuning tone](radio-setup.md#tuning-tone) |

### Information

| Command | What it does |
|---|---|
| `Version` | the program and version |
| `Help` | the command list |
| `TIme`, `DAte` | the computer's clock (setting them is accepted and ignored) |
| `RESTart` | the start-up banner and the stand-by text (ends a link) |

### Kept for compatibility, no effect

Host programs send these in their start-up scripts. Signum accepts them,
keeps the value and reports it, but they change nothing:

`MAXUp`, `MAXDown`, `MAXTry`, `MAXSum` (Signum uses the reference firmware's
own speed-change rules), `FSKAmpl` (the FSK level follows `PSKAmpl`),
`CSDelay`, `CYcle`, `CHOBell` (there is no changeover bell), `REMote`,
`CWid`, `CONType`, `Box`, `ARX`, `LFignore`, `BC`, `ADdlf`, `TR`.

## Host mode

Host programs switch the interpreter into WA8DED host mode:

- `JHOST4`: CRC host mode (the one Pat uses): framed packets with a CRC; a
  frame with a bad CRC is answered with a request to resend.
- `JHOST1`: the same frames without CRC.
- `JHOST0` (as a host-mode command) leaves host mode.

In host mode Signum answers `L` and `G` (channel 255 lists the channels with
output, channel 254 gives the status), `C`, `D`, `DD`, `I`, `%T`, `@B`,
`%L`, `@T2`, and every terminal-mode parameter command, as the reference
firmware does. `%O` is the host's Ctrl-Y (break in while receiving, change
over while sending) and `%I` a break-in only; both in every mode, answered
with an empty OK. Data bytes in host mode are always data: a 0x19 in a host
program's data is sent, never taken as Ctrl-Y. In host mode `U` sets the connect text (it never starts an
unproto transmission), as on the reference unit.

**Terminal commands in host mode with `#`:** `#` and a terminal command line (`#MYL 2`, `#PD 1`, `#DD`)
is carried out and answered with an empty OK whatever it was (its answer text is not sent; a query shows
nothing), as on the reference unit (LinBPQ sends `#DD` and `#MYL n`). `#U n` starts an unproto
transmission whose text then comes as data frames, `#D` ends it; during it the status byte on channel
254 says unproto, as on the reference unit. The Python SDK (a separate project, hf-tools) sets parameters this way on both.

## Differences from the reference firmware

Signum's answers were checked byte for byte against recordings of the
reference firmware. Where they differ:

- **Version and Help** are Signum's own texts. There is no evaluation-copy
  line before `*** STBY >>`, and no licence, show, system-test or packet-radio
  menus.
- **Terminal commands sent as host-mode commands** (e.g. `TONES 4`) are
  carried out and their value answered with code 1; the reference firmware
  reads them as WA8DED one-letter commands. An unknown host-mode command gets
  code 2 `INVALID COMMAND`.
- **P3 unproto:** with `--p3-unproto-rx`, P3 unproto (modes 30-41) is also
  received in Listen mode, which the reference unit cannot do (off by
  default).
- **The peer marker** (`PEERmark`, on by default, owner's decision): on P2/P3 links Signum sends one
  system-information sequence that the reference unit skips silently (nothing shown, nothing done), so that
  two Signums recognise each other. The reference unit sends no such thing. `PEERmark 0` turns it off.
- **Host-mode `U`:** our default connect text is our own.
- **`L`'s call-tries field** stays 0.
- **`%T`** answers the bytes sent and not yet acknowledged; the reference unit's manual describes the
  bytes sent and confirmed on this link.
- **`%V`** (the short version) is not provided: `V` gives Signum's version.
- **Packet radio:** channel 0 (the packet monitor) is not provided.
- `Quit` answers `*** ERROR: PSE TYPE HELP`, as the reference firmware does.
