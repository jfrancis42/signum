# Radio set-up

Signum needs three things from the station: a sound card connected to the
radio's audio, a way to key the transmitter (PTT), and correct audio levels.
It does not tune the radio or change its mode; set the frequency on the radio
or with your own rig-control program.

For the **Icom IC-7300** (built-in USB sound card and USB keying) every
setting on the radio, in Linux and in Signum is on its own page:
[Icom IC-7300](radio-ic7300.md). Signum can also set an IC-7300 up by
itself over CI-V, after a backup of the radio's settings:
`signum --configure-my-ic7300-and-overwrite-its-settings` ([Zero-effort
set-up](radio-ic7300.md#zero-effort-set-up)). It is the one radio Signum
changes; for any other radio the settings below are yours to make.

## The radio

- **Mode:** USB (upper sideband, or USB-D / DATA mode), as for other HF data.
- **Filter:** at least 2.4 kHz wide. P3 at its higher levels uses 18 carriers
  120 Hz apart, about 2.2 kHz of the passband, centred on 1500 Hz. P1 and P2
  use the tone pair 1400 / 1600 Hz.
- **Off:** speech compression and processing, noise reduction, noise blanker
  on the receive audio if it distorts, and any VOX.
- **AGC:** as you would for other data modes.

## Sound card (`--alsa DEV`)

Signum uses one ALSA device for both receive (capture) and transmit
(playback), at 48 kHz, one channel, 16-bit. Name it with `--alsa`.

Find your radio's sound card:

```
arecord -l          # capture devices, e.g. "card 2: CODEC [USB Audio CODEC]"
aplay -l            # playback devices
```

Then use the card's name with the `plughw` plug-in, which converts the rate
and channels if the card needs it:

```
--alsa plughw:CARD=CODEC          # many radios' built-in USB codec
--alsa plughw:CARD=Device         # a typical USB sound adapter
--alsa plughw:CARD=CODEC,DEV=0    # a particular device of the card
```

Names (`CARD=CODEC`) stay the same when USB devices are plugged in a different
order; card numbers (`plughw:2`) can change.

If two radios have the same card name, `arecord -L` lists the full names
ALSA knows, and `/dev/snd/by-id/` shows which is which.

**"cannot open DEV":** the device does not exist, or another program (a
sound server such as PulseAudio or PipeWire, another Signum, a digital-mode
program) holds it. ALSA `hw`/`plughw` devices can be used by one program at a
time. Close the other program, or have your sound server leave that card
alone. **"cannot configure DEV (48 kHz mono S16)"**: the device does not
offer that format; use `plughw:` rather than `hw:`.

**If the sound card disappears** while Signum runs (a USB cable pulled, the
radio switched off), Signum unkeys and exits with status 3.

## PTT

| SPEC | Keys the radio by |
|---|---|
| `rts:/dev/ttyUSB0` | the RTS line of a serial port (Icom USB with **USB SEND** set to RTS; any interface that keys on RTS). `--rts TTY` is the same |
| `dtr:/dev/ttyUSB0` | the DTR line (Icom USB SEND = DTR; interfaces wired to DTR) |
| `civ:/dev/ttyUSB0:94` | Icom CI-V "TX on/off". The radio's CI-V address in hex is required (IC-7300: `94`, IC-7610: `98`, IC-9700: `A2`). Optional baud rate after it, default 19200: `civ:/dev/ttyUSB0:94:115200`; it must match the radio's CI-V USB setting. On the bench (IC-7300) it keyed links but received worse than `rts:`: [below](#civ-on-the-bench) |
| `rig` or `rig:HOST:PORT` | Hamlib's `rigctld` (`T 1` / `T 0`), default `127.0.0.1:4532`. Works with any radio Hamlib supports |
| `none` | never key (receive only, tests). The default |
| `vox` | nothing: the radio keys itself on the audio. **Do not use** (below) |

- Use a stable port name from `/dev/serial/by-id/` instead of `/dev/ttyUSB0`,
  which can change between boots and replugs.
- Your user needs access to the serial port (usually the `dialout` group:
  `sudo usermod -aG dialout $USER`, then log in again) and to the sound card
  (the `audio` group on some systems).
- With `rig`, start `rigctld` for your radio first, for example
  `rigctld -m 3073 -r /dev/ttyUSB0 -s 19200` (model 3073 is the IC-7300 in
  Hamlib; `rigctl -l` lists them). Signum uses its own connection only to key
  and unkey, and to read the frequency for the front panel's **Radio kHz**
  readout (`f`, read only, every 2 s in standby in place of a PTT-state poll,
  never during a call or a link); other programs (Pat, a logger) can share the
  same `rigctld` for frequency control.
- RTS and DTR are held low whenever Signum is not transmitting, and opening
  the port does not raise them. Linux raises both lines for an instant at
  every open of a serial port that is set to a speed, before any program can
  clear them: a radio that keys on RTS or DTR (an Icom with **USB SEND** set
  to RTS or DTR) transmits for 15-20 ms each time. Signum therefore uses a
  USB serial port at speed **B0** and leaves it there when it exits; a USB
  serial chip keeps its real baud rate at B0, so CI-V still works. When the
  port was not at B0 (the first open after the radio or cable was plugged in,
  or another program set a speed), that one open still raises the lines and
  Signum says so in its log (`[ptt] ... was not at speed B0`); later opens do
  not. With `civ:`, if the radio does not answer at the chip's last rate,
  Signum sets your baud rate once, which also raises the lines for an instant
  (logged). Programs that set a speed on the port (`stty`, terminal programs,
  `rigctld`, also Signum's own rig control) still raise the lines when they
  open it.
- The key goes down while the modem transmits and comes up only after the
  queued audio has played. A stop signal (Ctrl-C, `kill`) unkeys at once.
- **Key-down watchdog:** if the transmitter stays keyed for more than
  `--max-key S` seconds in a row (default 90), the key is dropped. It is
  always on.
- **Stuck key check:** with `rig` and `civ`, Signum asks the radio every
  0.5 s while it is not transmitting whether it is still keyed, and unkeys it
  again if so. This catches a second keying path (for example an interface's
  own PTT) holding the radio after Signum's unkey.

### `civ:` on the bench

On the development bench (two IC-7300s, bench-measured 2026-11-16) `civ:`
keyed whole P3 links, and Signum sent 12,000 characters as fast as with
`rts:`. As the receiving station, though, 5 of 7 links failed or lost many
packets where `rts:` was clean in 5 of 5: the radio did not always leave
transmit on the CI-V "TX off" (Signum's stuck-key check caught it twice)
and was then deaf to the other station's next packet. The bench ran it with
the radio's USB SEND at RTS (held low by Signum) and a second program
reading the same CI-V port; `civ:` with USB SEND = OFF and the port to
itself has not been measured. Until it has, prefer `rts:` (or `dtr:`) where
the radio offers it. Details: [Icom IC-7300, Keying](radio-ic7300.md#keying-rts-or-civ).

### Why not VOX

P1, P2 and P3 turn around fast: the other station's answer starts about
**0.14 s** after our packet ends. A VOX circuit holds the transmitter for its
hang time after the audio stops, usually longer than that, so the answer
arrives while we are still transmitting and is never heard. The result: the
far station answers every call and Signum hears none.

Key by `rts:`, `dtr:`, `civ:` or `rig:`. Turn VOX keying off on the radio
and on the interface (many sound-card interfaces key the radio on audio). With
two keying paths, a radio can ignore the unkey on one while the other still
holds it. `--ptt vox` is accepted but prints a warning at start.

## Audio levels

### Transmit

The default output level is the reference unit's own setting, **PSKAmpl 140**
(mV peak-to-peak on its output). On a sound card that is a PSK peak of about
**-17 dBFS**, which drives many interfaces too softly to show any ALC.

Raise it in either of two ways:

- `--tx-gain DB` on the command line, on top of PSKAmpl: `--tx-gain 12` is
  about -5 dBFS.
- the `PSKAmpl` command (or **PSK level** in the GUI's Configure tab): `PSKA 400`
  is about -8 dBFS, the most, `PSKA 1000`, about -0.3 dBFS. Do not push past
  full scale with both together.

The FSK used for calls and link set-up follows at a fixed ratio.

Set it as for any data mode: on a P3 link, at the power you mean to run,
raise the level until the radio shows a little ALC, then back off until it
shows none (or as the radio's manual says for data). The default stays the
reference unit's value on purpose. A step-by-step way with the tuning tone
as a steady test signal: [Icom IC-7300, Transmit level](radio-ic7300.md#transmit-level).

### Receive

Set the radio's or the interface's receive audio so that band noise shows as
a moderate background on the waterfall (Status tab), and the strongest
signals do not clip. Avoid AGC pumping on strong signals. The front panel's
**RX audio** lamps help: green OK; amber Low (RMS under -60 dBFS: turn it
up); amber High (peaks above -3 dBFS) or red Clip (full scale reached: turn
it down). Aim for band noise around -40 to -30 dBFS; the lamps' tooltip
shows the peak and RMS ([The GUI](gui.md#receive-audio-level)).

### Tones

`--tones LO,HI` (default `1400,1600`, the same as `TONES 4`) sets the FSK
tone pair; leave it unless you know the other station uses another pair. The
console commands `TOnes`, `MARk` and `SPAce` change it while Signum runs.

## Tuning tone

An automatic antenna tuner needs a few seconds of steady carrier to tune. A
P1/P2/P3 call is short bursts with gaps in between, which many tuners cannot
tune on. With the tuning on (`--tune-tone S`, S above 0), Signum tunes before
each new call it places, so the tuner can tune before the call goes out:

1. You connect (`C CALL`, host-mode `C`, or **Connect** in the GUI).
2. Signum tunes, one of two ways (the next section): its own **steady tone**
   (the radio keyed, a sine for the set time with a 10 ms ramp at each end, so
   no key clicks), or the **radio's own tune cycle** through rigctld.
3. 0.3 s after that the call goes out exactly as without the option.

Only the first call attempt of a new connect is tuned for: not the call's
own repeats, not when another station calls us, not unproto. The station
you call never hears it as anything (it comes before the call), so it has no
effect on compatibility. It is **off by default**, as the reference unit has
no such tone.

| Setting | Option | Console | Range | Default |
|---|---|---|---|---|
| duration | `--tune-tone S` | `TUnetone S` | 0-10 s (0 = off) | 0 |
| frequency | `--tune-hz HZ` | `TUnetone S HZ` | 300-2700 Hz | 1500 (the middle of the P3 signal) |
| level | `--tune-db DB` | `TUnetone S HZ DB` | -30 to 0 dB against the data level | 0 |
| method | `--tune-method M` | `TUnetone auto`, `TUnetone tone`, `TUnetone radio` (or `TUnetone S HZ DB M`) | auto, tone, radio | auto |

The GUI has the same settings in the Configure tab (**Tuning tone**). `TUnetone`
alone shows them (`... METHOD AUTO`); the method word may come anywhere in the
arguments.

### The radio's own tune cycle (`--tune-method`)

Most radios with an internal or attached tuner can start the tuner themselves
(the IC-7300's TUNE key). Hamlib's rigctld offers it as the `TUNE` VFO operation
(`G TUNE`), and Signum uses it instead of the tone:

| Method | What happens |
|---|---|
| `auto` (default) | The radio's tune when Signum has rigctld control (`--ptt rig[:HOST[:PORT]]`) and the rig lists `TUNE` among its VFO ops; otherwise the tone. |
| `radio` | The radio's tune if possible. If it is not possible, the log says why and the tone is sent instead: never silence. |
| `tone` | Always the tone (the behaviour before 0.7). |

How the radio's tune runs (only with a duration above 0; `TUnetone` stays 0 =
off by default):

1. A connection of its own to the same rigctld (the one `--ptt rig:` uses). Once
   per connection Signum reads `\dump_caps` and keeps whether the `VFO Ops:`
   line contains `TUNE`; a new connection (rigctld restarted, perhaps for
   another radio) asks again.
2. `t` must read 0: a radio that is already transmitting is not tuned, the tone
   follows. A rig that cannot report PTT at all (rigctld answers `RPRT -11`) is
   tuned anyway, without the early end below.
3. `G TUNE`. Any answer but `RPRT 0` counts as unsupported **for this attempt**
   (it is not remembered): the log says what rigctld said, and the tone follows.
4. Signum waits, in silence, up to the duration (S seconds), reading `t` every
   0.1 s. When the radio has keyed and has been back in receive for 0.3 s, the
   wait ends at once. A radio that never reports its tune as PTT (the IC-7300
   may not) is waited for the full S seconds, so set S to the longest cycle
   the tuner needs (the IC-7300's takes 1 to 4 s, longer on a bad match).
5. 0.3 s later the call goes out.

Signum does **not** key during the radio's tune (its own PTT stays off, and the
PTT's stuck-key check is paused so it does not unkey the radio's carrier). The
radio transmits at **its own RF POWER setting**, not at Signum's data level, and
`--tune-db` and `--tune-hz` do not apply to it. An abort during the wait (`D`,
`DD`, QRT, the GUI's Disconnect) drops the call exactly as with the tone; the
radio finishes the cycle it was told to start, because a tuner stops itself.

The log (`[tune]` lines, the GUI's Log) says which method was used and why, for
example `tune: radio (TUNE supported), up to 5 s ...`, `tune: tone (no rigctld
control: the PTT is not rig)`, `tune: radio requested but not possible (...); using
the tone`. A radio tune never starts without the owner's go-ahead: it transmits.

**The level.** The tone follows the transmit level you set (`PSKAmpl`,
`--tx-gain`) and can never be louder than the data level. At the default
0 dB it has exactly the peak and the average power of our P1 FSK, which is
what the call itself sends a moment later, so the tuner tunes at the power
the call will run at. Measured on our own output at the default level
(PSKAmpl 140, `--tx-gain 0`; sample values, full scale 32767):

| Signal | Peak | Average (RMS) | Average against the tone |
|---|---|---|---|
| tuning tone, 0 dB | 4,400 | 3,111 | 0 dB |
| P1 FSK (calls, P1 links) | 4,400 | 3,111 | 0 dB |
| P2 data | ~4,950 | 2,213 | -3.0 dB |
| P3 data | ~5,440 | ~1,780 (1,580-2,590) | about -4.8 dB |

So the tone's peak stays below the P2 and P3 data peaks (it drives no more
ALC than the data does), while its average power is the call's: about 3 dB
more than P2 data and about 5 dB more than P3 data, held for the whole tone
instead of in bursts. If your radio is set so that P3 peaks reach full power,
the tone gives about two thirds of that power, steadily. If the tuner's
manual asks for less power while tuning, or the amplifier should not see a
long carrier, lower it: `-10` is a tenth of the power, `-13` a twentieth.

**Safety.** The tone keys the radio through the same PTT as everything else
and is covered by the key-down watchdog (`--max-key`). It is never keyed
longer than the set time. `D`, `DD`, `QRT`, the QRT key (Ctrl-D), Esc then
`D`, host-mode `D` and the GUI's **Disconnect** stop it at once (the radio
unkeys within about 50 ms, the audio already queued in the sound card), and
the call is not made. Signum has no busy-channel check before a call (nor
does the reference unit): listen before you connect. The log shows a
`[tune]` line when the tone starts, ends or is stopped.

## Timing calibration

A measured sound-card clock error above 200 ppm raises a warning (log, console, GUI; [Troubleshooting](troubleshooting.md#sound-card-clock--ppm-off-warning)).

P1, P2 and P3 expect each station's answer within a few milliseconds of a
set moment. Signum must therefore know how long the audio takes from the
modem to the antenna and back (the sound card's buffers, the codec, the
radio). It learns this by itself.

### Self-calibration (the default)

With a sound card and no calibration file, Signum measures:

- **The hardware round trip** (sound card + radio): when we call another
  station, from when its answers arrive. Until three readings agree, it uses
  a 20 ms guess, what a typical radio with a built-in USB codec measured
  (20-22 ms). A station that is only ever called keeps the guess.
- **The sound card's clock error** (in ppm): when another station calls us,
  from that station's packet timing over a link of 40 s or more; before any
  such link, against the computer's clock after 5 quiet minutes.

What it learns is applied between links, never during one, and kept in
`~/.config/signum/autocal-DEV.conf` (one file per sound card; DEV is the
`--alsa` name with other characters as `_`). The log shows each step as
`[cal]` lines.

Readings of the hardware round trip above **45 ms** are refused: another
Signum answers some 60 ms later than the reference unit, and its answers
must not be mistaken for hardware delay. If your radio path is known to be
slower than a plain transceiver's (an SDR transceiver, a remote station),
raise the bound with `--autocal-max-ms N`, and then calibrate only against
stations that are not Signum, or write the value into the calibration file.

`--no-autocal` turns self-calibration off. `--autocal-state FILE` keeps it
elsewhere.

### How long an audio path Signum tolerates

Each station's **audio round trip** is its software part (the playback buffer, `--prime-ms`, 40 ms by
default) plus its hardware part (sound card, codec, radio). A USB sound server (PipeWire, PulseAudio),
a USB hub or a slow Raspberry Pi can make either part longer. Check yours in the log, a few seconds
after start and after a call:

- `[rt] software round trip 43.31 ms` (at 5 s): the software part (default prime: about 43 ms);
- `[cal] self-calibration: hardware round trip 21.50 ms (confirmed by 3 readings)`: the hardware part;
- `[cal] self-timing: round trip ... ms`: the sum Signum times itself with.

P1, P2 and P3 leave each station a fixed time between hearing a packet and answering it, and
Signum cannot answer what it has not heard yet. With a longer path it answers earlier by what its
path adds, as long as it can. When it cannot, a P3 answer loses the part of its lead-in that is
already past (`TX burst queued 4.9 ms after its start: its first part not sent`); a few ms of the
lead-in cost nothing. Only when the answer's first pulse itself is past, and only once the other
station is known to be a Signum (its peer marker read on this link), does it go whole and late
instead: `our burst goes out 28 ms late, whole: its instant had passed`. Against a Winlink RMS or
any other modem a late whole answer broke every link on the air (2026-11-19), so it is never sent
to them; `P3_LATE_WHOLE=1` sends it to every station, `P3_LATE_CUT_OLD=1` never. Its own looks for the far end's answers wait
longer too, and read an answer from the part already heard.

- **Against the reference unit** (whose timing is fixed): the default path (round trip 43 ms software
  plus a radio's 20-25 ms) is what Signum is tested with. A longer path makes Signum's answers late
  in the reference unit's eyes; keep the path short (no sound server between Signum and the sound
  card, `--prime-ms` at its default).
- **Between two Signums:** both stations' round trips together must stay under about **0.21 s**
  (measured on the ALSA loopback: links at 103 ms on each side, a P1 call that is never answered at
  113 ms on each side). The protocol fixes this: in a 1.25 s cycle a P3 packet takes 0.85 s and the
  answer 0.23 s, which leaves about 0.17 s for both turnarounds; a late answer can still be read from
  its first 12 of 21 steps, which gains the rest. At about 0.2 s together, links set up but may
  switch to long packets slowly; a few repeated packets are normal.

If calls to another Signum are never answered, or a P3 set-up repeats until it times out, and the
log shows `goes out ... late` lines or `read after the decision` lines, the audio path is too long:
use the sound card directly (`plughw:...`, not `default` or `pipewire`), keep `--prime-ms` at its
default, and check that the computer keeps up (docs/md/performance.md).

### The calibration file

Values in a calibration file always win over self-calibration. The default
file is `~/.config/signum/calibration.conf` (`--cal FILE` for another). A
commented sample is installed as `/usr/local/etc/signum/calibration.conf`
(`dist/calibration.conf` in the source tree). Lines are `key = number`:

| Key | Meaning |
|---|---|
| `hw_latency_s` | the hardware round trip in seconds (e.g. `0.022`); Signum then works out its own timing from it |
| `audio_ppm` | the sound card's sample-rate error in ppm (also `--audio-ppm E` or the `AUDIO_PPM` environment variable) |
| `rt_latency_s`, `reply_adjust_s` | direct timing overrides (advanced; `--rt-latency S` the same) |

For most stations self-calibration is enough: make a few calls to a station
running a hardware modem of the reference class, and Signum fills in the
rest.

## Checklist for a first on-air test

1. `signum-start --call YOURCALL --alsa plughw:CARD=... --ptt ...` with
   real-time priority set up ([how](performance.md#real-time-priority)).
2. The log shows no `cannot open`, and the PTT line names your method.
3. In standby, the **Free/Busy** lamp turns red when someone transmits on
   the frequency, and the waterfall shows the band.
4. Low power first. Call a station you have arranged a test with and set the
   output level on the link.
