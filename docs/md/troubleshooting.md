# Troubleshooting and FAQ

Signum's log says most of what goes wrong, in plain words. Look there first:
the terminal with `signum --no-tui`, `~/.local/state/signum/tnc.log` under
`signum-start`, `journalctl -u signum` for the service, or the GUI's Log tab.

## It does not start

| Message | Cause and fix |
|---|---|
| `need --alsa DEV or --audio-in/--audio-out` | give the sound card: `--alsa plughw:CARD=...` |
| `cannot open DEV` | the sound card name is wrong, or another program holds the card (a sound server, another Signum, a digital-mode program). Check with `arecord -l`; see [Sound card](radio-setup.md#sound-card---alsa-dev) |
| `cannot configure DEV (48 kHz mono S16)` | the device does not offer that format: use `plughw:` instead of `hw:` |
| `bad --ptt ...` | the PTT spec does not parse. `civ:` needs the radio's address: `civ:/dev/ttyUSB0:94` |
| a PTT or pty error, exit status 1 | the serial port does not exist or you lack permission (`dialout` group, then log in again) |
| `[ws] bind 127.0.0.1:8770: Address already in use`, exit status 1 | another Signum (or another program) has port 8770: give `--ws 127.0.0.1:PORT` or stop the other one |
| `[telnet] ...: telnet off` | port 3000 is taken; Signum runs without telnet. `--telnet PORT` picks another |
| `[ws] no GUI directory found` | Signum cannot find its `www/` directory: give `--gui DIR`, or reinstall |
| `signum-start: ... not built (make)` | build first, or `--bin PATH` |

## No link: the other station never answers my call

- **Is the radio transmitting?** The **TX** lamp lights while Signum keys.
  If it lights but the radio does not transmit, the PTT method or port is
  wrong ([PTT](radio-setup.md#ptt)).
- **Is audio getting out?** Check the radio's power or ALC meter while
  calling. Raise the level ([Audio levels](radio-setup.md#audio-levels)).
- **Frequency:** both stations on the same dial frequency, USB.
- **Mode:** a station that allows only P1 (or only P2) still links; it just
  stays at the lower mode.
- **A long audio path:** lines `our ... burst goes out N ms late` or `p1 reply ... read after the
  decision` in the log mean the sound path (a sound server, a USB hub, a slow computer, a large
  `--prime-ms`) is longer than the answer times allow. Two Signums link with up to about 0.2 s of
  round trip on both sides together; against the reference unit keep the default path
  ([How long an audio path Signum tolerates](radio-setup.md#how-long-an-audio-path-signum-tolerates)).
  A few `TX burst queued N ms after its start` lines of under ~10 ms are normal on an IC-7300 over
  USB (its own round trip is ~47 ms) and harmless.
- **Links to a Winlink RMS set up, then stall** (receiving stuck at levels 1-2, our break-in repeated
  until the set-up times out) with `our burst goes out N ms late, whole` lines: Signum 0.5.1 before
  fix 269 sent late answers whole and late to every station, which RMS modems do not read. Update;
  as a stopgap start Signum with `P3_LATE_CUT_OLD=1` in its environment.

## The other station answers, but Signum hears nothing

This is almost always **VOX keying**: the far station's answer arrives about
0.14 s after our packet, while a VOX circuit still holds our transmitter.
Turn VOX keying off on the radio and the interface and key by `rts:`, `dtr:`,
`civ:` or `rig:`. See [Why not VOX](radio-setup.md#why-not-vox).

Also check that the receive audio reaches the sound card: the waterfall
should show the band and the far station's signal.

## The Free/Busy lamp stays red

The busy detector reads the receive audio in standby. It stays red for about
4 s after start-up (as the reference unit does), a little after each link and
for about 4 s after each signal. It turns red for a keyed or steady signal on
the station's two tones (P1, P2, a tuning carrier), and for any signal well
above the band noise (P3, other modes, voice): about 8 dB over the level the
band sits at most of the time.

- **No audio, or nearly none** (a muted or unconnected sound card, a loopback
  with nothing on it, a level of a few steps of the sound card's 16 bits): the
  lamp turns green after those 4 s; such input is taken as a free channel.
  Raise the receive level until the band noise shows on the waterfall
  ([Audio levels](radio-setup.md#audio-levels)).
- **Red with nothing heard:** a carrier on one of the station's tones (a birdie
  from a switching supply, say) keeps it red. Look at the waterfall.

## The link starts but drops, or stays at low speed

- **The Computer lamp is yellow or red:** this computer is not keeping up.
  Follow its advice ([CPU health](performance.md#cpu-health-ok-busy-starved)):
  stop other programs, set up real-time priority, hold the CPU clock up.
- **`[rt] playback underrun`** or **`the audio loop stalled`** in the log: the
  same cause. A single one after start-up is harmless; Signum keeps the link
  timing across a stall, and the packet on the air at that moment is sent
  again.
- **`[cpu] cannot hold the CPU clock up`:** install the
  [CPU clock unit](performance.md#installing-the-cpu-clock-unit-signum-cpufreqservice).
  On a Raspberry Pi this matters a lot.
- **A poor channel:** P3 needs a reasonable signal. **Quality %** low and the
  level stepping down is the protocol working as designed.
- **Timing:** the first few links on a new set-up teach self-calibration the
  audio path. `[cal]` lines in the log show it. A slow radio path (an SDR
  transceiver, a remote radio) may need `--autocal-max-ms`
  ([Timing calibration](radio-setup.md#timing-calibration)).

## "sound card clock ... ppm off" warning

`[clock] WARNING: the sound card's clock is +312 ppm off ...` in the log, `SOUND CARD CLOCK` in
the console's CPULoad answer, and a "sound card clock +312 ppm off" notice in the GUI's CPU/notices
area. The figure is the card's measured sample-rate error, from self-calibration (against the far
station after a link, or against the computer's clock before the first one).

**What a ppm is, and why it matters.** A sound card meant to run at 48,000 samples a second that runs
300 ppm (parts per million) fast really runs at 48,014.4. The audio sounds the same, but PACTOR is a
timed protocol: both stations work in 1.25-second cycles, and each answer must arrive inside a window a
few milliseconds wide. A card 300 ppm off makes this station's idea of 1.25 s wrong by 0.375 ms every
cycle, which adds up to about 22 ms a minute, enough to put answers outside the other station's window.

**What failure looks like.** The link connects, then fails early: changeover requests or packets
repeated until the link times out, "not taken" answers, or links that work for a few minutes and then
fall apart, mostly on long transfers. Nothing in the audio sounds wrong.

**Where the limit comes from.** In emulator tests, links with one station's clock 250-300 ppm off
failed early, and that was true between two emulated reference units as well, not only with Signum: it
is a limit of the protocol's timing, not a Signum weakness. At 130-200 ppm the links held.

**What Signum does about it.** It measures the error and corrects for it (below), so a card that is
off by a steady amount mostly works once measured. The warning is still given above 200 ppm because:
the correction only starts after the first measurement (until then, a first link can fail); a card this
far off usually also drifts with temperature, faster than the correction follows; and a different card
is the real cure. It warns once per change (again only if the figure moves by 50 ppm; "back within 180
ppm" when it clears).

**How the error is measured and corrected.** Self-calibration does it without you
([Timing calibration](radio-setup.md#timing-calibration)): against the computer's clock after 5 quiet
minutes, and against the far station's timing over any link of 40 s or more that another station
starts. The result is applied between links and kept per sound card in
`~/.config/signum/autocal-DEV.conf`, so it survives a restart. A computer whose clock is set by
NTP or GPS makes the first measurement better. If you have measured your card some other way,
`--audio-ppm E` (or `audio_ppm` in the settings file) sets the figure by hand.

**Fix:** another sound card, another USB port, or a different USB audio adapter. `--audio-ppm` only tells Signum the figure; it
does not make the card better.

## The radio stays keyed

Signum drops the key after `--max-key` seconds (default 90) in any case.
With `rig:` and `civ:` it checks every 0.5 s while not transmitting and
unkeys the radio again. A radio that stays keyed after Signum unkeys usually
has a second keying path (an interface's VOX or its own PTT line): turn that
off.

## Frequency offset

The **Offset Hz** readout and the tuning row show the other station's offset.
Small offsets (a few Hz to tens of Hz) are normal and followed automatically.
A sudden step during a link (one station retuning) is measured and followed
within about two failed packets, up to about ±70 Hz on P3 data and ±90-100 Hz
on P2. Beyond that, retune: both stations should be within a few tens of Hz.

## Pat cannot connect

- Pat's `"pactor"` `"path"` must be Signum's pty, default `/tmp/ttysignum0`
  ([Pat](connections.md#pat-winlink)).
- Only one program should drive the pty at a time; a terminal program left
  open on it confuses Pat.
- "Serial port busy" (EBUSY) from Pat on the pty after an earlier Pat was killed: that Pat had taken the port
  exclusively (TIOCEXCL) and died without releasing it. Signum clears such a leftover flag itself within about a
  second once no process has the pty open, and logs `[pty] cleared the exclusive flag a client left on ...`.
  A program that is still running keeps its exclusive hold.
- A "Wrong checksum in package!" warning now and then from Pat is harmless.

## Text I type waits until the other side hands over

That is the reference unit's behaviour, in every mode (P1, P2, P3). While you
receive, what you type is kept until the sending station hands the link over,
or until you press **Ctrl-Y** to break in. While you send, Signum keeps the
link after your text has gone, sending idle packets, until you press
**Ctrl-Y** ("over"). In a keyboard QSO end each turn with Ctrl-Y, as the other
operator does on a reference unit.

If you want it to happen by itself, set **Auto break-in** on (GUI: Configure,
Link parameters; `PD 1`; `--pduplex on`): Signum then breaks in whenever text
is waiting (not within `PDTimer` seconds, default 12, of the other station
breaking in) and hands over whenever its text has run out. Leave it off for
ordinary QSOs and for Pat as the calling client.

## A program answering calls hangs at the start (gateway, peer-to-peer)

The calling side speaks first only after the called side has sent its
greeting, and with Auto break-in off the called station never takes the link
to send it: both wait until the link times out. The same happens on the
reference unit.

- A host program that answers calls (a Winlink gateway, a peer-to-peer
  listener) must set **`PD 1`** at start-up, or send `%I` (break in) and `%O`
  (hand over) itself. Gateway software does this on the reference unit too:
  LinBPQ, for example, sets `PDUPLEX 1` and `PDTIMER 5` because "the node code
  relies on automatic IRS/ISS changeover".
- **Pat** sends neither (its start-up commands are `MYcall`, `PTCH 31`, `MAXE
  35`, `REM 0`, `CHOB 0`, `TONES 4`, `MARK 1600`, `SPACE 1400`, `CWID 0`,
  `CONType 3`, `MODE 0`, `DATE`, `TIME`). As the calling client it does not
  need to: the gateway it calls hands over and breaks in. Pat cannot answer
  calls in these modes (its driver has no listener), so "Pat as the called
  station" is not possible with Signum or with the reference unit.
- If you must run a program that answers calls and cannot send `PD 1`, set
  Auto break-in on in Signum's settings (`pduplex = on` in the settings file
  or `--pduplex on`). More: [Running a gateway or peer-to-peer
  listener](connections.md#running-a-gateway-or-peer-to-peer-listener-set-pduplex-1).

## The GUI does not load

- Open http://127.0.0.1:8770/ on the same computer as Signum. From another
  computer use an SSH tunnel or `--ws 0.0.0.0:8770` with logins
  ([Logins and security](connections.md#logins-and-security)).
- `--no-ws` turns the GUI off.
- The pill top right says `disconnected · retrying` when the page cannot
  reach Signum: Signum stopped, or the address changed.

## No real-time priority

The log line `[rt] no real-time priority` means the capabilities are missing
(a rebuild clears them) or the filesystem is mounted `nosuid`. See
[Real-time priority](performance.md#real-time-priority).

## The IC-7300 set-up

| Message | Cause and fix |
|---|---|
| `No IC-7300 found` | the radio is off, its USB cable is out, or it is not an IC-7300 (`ls /dev/serial/by-id/` shows no `...IC-7300_<serial>...` link; `lsusb` shows 10c4:ea60 for its serial chip) |
| `More than one IC-7300` | add `--ic7300-serial N` (the serial numbers are listed) |
| `no CI-V answer from the radio` | the radio is off, or **USB Serial Function** is "RTTY Decode" (MENU » SET » Connectors: set it to CI-V by hand), or another program holds the port and answers in its place |
| `the radio did not answer at the port's present rate` | the port's chip has a rate the radio does not use (the radio's CI-V USB Baud Rate was changed). Allow Signum to set the port's speed (the command line asks; the GUI's second checkbox), or unplug and replug the USB cable and run it again |
| `the radio is transmitting` | wait until it is not; nothing was changed |
| `could not read ...: nothing changed` | the radio stopped answering part-way; run it again |
| a setting `read back ..., not ...` or `the radio refused it (NG)` | the set-up went on with the rest and says so at the end; the backup puts everything back (`--restore-ic7300-settings FILE`). Report the item and your radio's firmware version |
| `Not confirmed: standard input is not a terminal` | run it in a terminal and type `YES`, or (scripts only) add `--i-understand-this-overwrites-my-ic7300-settings` |
| `note: ... also has /dev/ttyUSB0 open` | another program (a rig-control program, a running Signum) uses the port too; the set-up asks again when it loses an answer. Stop that program if the set-up fails |

## FAQ

**Does Signum work with a hardware P1/P2/P3 modem on the other end?**
Yes. Signum was tested against the reference hardware on the bench (P3
calling and called, P2 called, P1 calling) and on the air. Its defaults match
the reference unit's.

**Can Signum talk to another Signum?** Yes, in every mode.

**Does Signum control my radio's frequency?** No. Use the radio, Pat's
rig control, or another program. Signum only keys the transmitter.

**Which programs can use it?** Pat (tested). Other programs written for
hardware modems with the classic terminal-mode commands and WA8DED/CRC host
mode should work, on the pty, telnet or the WebSocket shell.

**Can I run it on a Raspberry Pi?** A Pi 4 or faster
([Minimum hardware](performance.md#minimum-hardware)).

**Can I use it without a desktop?** Yes: run it headless and open the GUI from
another computer through an SSH tunnel, or use the TUI over SSH.

**Is there a mailbox (store and forward)?** No. Use Pat or another host
program for mail.

**Why does Signum show other stations' text while idle?** Listen mode is on
by default, as on the reference unit: it decodes other links on the
frequency. `Listen 0` (or `--listen off`, or the GUI's Configure tab) turns it
off; that saves some CPU (about a fifth of one Raspberry Pi 4 core while idle)
and changes nothing on the air.

**Where are my settings saved?** Commands change settings only until Signum
stops. Put what you always want on the command line (or in the systemd
unit). Only self-calibration is kept, in `~/.config/signum/`.

**Does it work with VOX?** No; see [Why not VOX](radio-setup.md#why-not-vox).
