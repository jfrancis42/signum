# Icom IC-7300

How to set up an Icom IC-7300 with Signum using only the radio's own USB
port: its built-in sound card (a USB audio codec) for receive and transmit
audio, and its built-in USB serial port for keying (RTS) and CI-V. One USB cable, no
interface box. It covers the radio's menus, the Linux side and Signum's own
settings, and how to set the levels on both sides.

The general page is [Radio set-up](radio-setup.md); this page gives the
IC-7300 values for it.

## Zero-effort set-up

Signum can set the radio up by itself, over the same USB cable. Plug the
radio in, switch it on, and run

```
signum --configure-my-ic7300-and-overwrite-its-settings
```

or, in the GUI, **Configure » Radio & audio » Set up my IC-7300…**. It finds
the radio, shows you its present settings and what it will change, warns you,
and after your confirmation saves a backup of the radio's settings, sets the
radio for Signum, checks every setting it wrote, and fills in Signum's own
settings file. Then start Signum as usual (`signum`, or `signum-start`
with your call sign) and set the transmit level ([Transmit
level](#transmit-level)): that one needs your eyes on the ALC meter.

**It never changes the transmit power (RF POWER) or the frequency, and it
never transmits.** It reads both and keeps them in the backup only.

What you need: the radio switched on with its USB cable plugged in, your
user in the `dialout` group ([The serial port](#the-serial-port)), and
**USB Serial Function = CI-V** (the factory value: with "RTTY Decode" the
port carries no CI-V and Signum cannot talk to the radio).

### The commands

| Command | What it does |
|---|---|
| `signum --detect-ic7300` | finds every IC-7300 and prints its port, sound card, CI-V rate and address, every setting below and what the set-up would change. Reads only |
| `signum --backup-ic7300-settings` | saves the radio's settings to a backup file. Changes nothing |
| `signum --configure-my-ic7300-and-overwrite-its-settings` | the set-up (below). Asks you to type `YES` |
| `signum --configure-my-ic7300-for-an-external-modem-and-overwrite-its-settings` | the same for a hardware modem on the radio's ACC socket instead of Signum on USB ([below](#using-the-radio-with-an-external-modem-instead)). Does not touch Signum's settings file. Asks you to type `YES` |
| `signum --restore-ic7300-settings FILE` | writes a backup file back to its radio, each setting read back. Asks you to type `YES` |
| `signum --ic7300-power W` | sets the drive (the TX gain W needs with the ALC at rest even on a hot radio, e.g. -7 dB for 20 W, -7.5 dB for 25 W; USB MOD Level 100 %) and RF POWER so that Signum's P2/P3 peaks reach W watts with the ALC at rest, and saves the TX gain ([Setting the power](#setting-the-power)). Never transmits; add `--measure` to measure on the air instead |
| `--ic7300-serial N` | with any of them: which radio, when more than one IC-7300 is plugged in (Signum lists their serial numbers and refuses to guess) |
| `--i-understand-this-overwrites-my-ic7300-settings` | for scripts: confirms the set-up or restore without typing `YES`. There is no other way round the confirmation; standard input that is not a terminal is never taken as a yes |
| `--config FILE`, `--no-config` | the settings file the set-up writes (default `~/.config/signum/signum.conf`) |

Each runs once and Signum exits. Do not run them while a program transmits
through the radio's port (a running Signum in a link): opening the port
clears its RTS and DTR lines, which would end that transmission. The GUI's
dialog takes care of that for its own station: it refuses while a link is up
and holds the transmitter unkeyed while it works with the radio.

### In the GUI, step by step

1. Start Signum and open the GUI, **Configure** tab, card **Radio &
   audio**. Make sure no link is up (the dialog refuses during one).
2. Press **Set up my IC-7300…**. The dialog looks for the radio at once
   and reads its settings; nothing is changed yet. It shows the serial
   number, port, sound card, CI-V address and rate, and a table: every
   setting, its present value and what Signum would set (changes in
   yellow). With two radios, choose one by its serial number.
3. Read the red warning box under the table. Tick **I understand my radio's
   current configuration will be lost**; only then can you press
   **Reconfigure the radio**.
4. Each setting appears as it is written and read back (ok / FAIL). At the
   end the dialog names the backup file and the Sound card and PTT lines it
   wrote to the settings file.
5. **Restart Signum**: the sound card and PTT apply at the next start.
   Then set the transmit level ([Transmit level](#transmit-level)).

**Set up my IC-7300 for an external modem…** opens the same dialog for a
hardware modem on the ACC socket ([below](#using-the-radio-with-an-external-modem-instead)).
**Back up only** saves a backup and changes nothing. **Restore IC-7300
settings…** lists the backup files, newest first; choose one, tick the box,
**Restore these settings**. While the dialog works with the radio, the
station holds its transmitter unkeyed.

### Keying: why `rts:` and not `civ:`

The set-up keys the radio with `rts:` (the RTS line of the radio's USB serial
port, with the radio's USB SEND = RTS). That is what every link on the
development bench ran, in both directions. Keying with CI-V commands
(`civ:`) sent fine on the bench, but as receiver 5 of 7 links failed or lost
much of their text: the radio did not always leave transmit when told to,
and was then deaf to the other station. Until that is understood, `civ:` is
not recommended. Details and numbers: [Keying](#keying-rts-or-civ).

### What it does, step by step

1. **Finds the radio.** Its USB serial port is the
   `/dev/serial/by-id/...IC-7300_<serial>...` link of a Silicon Labs CP210x
   chip (USB ID 10c4:ea60) whose USB serial string names the IC-7300 and
   carries the radio's serial number. Its sound card is the USB audio codec
   on the same USB hub inside the radio (matched in `/sys`), used as
   `plughw:CARD=<its name>`. More than one radio: you choose by serial
   number.
2. **Finds its CI-V rate and address, without keying it.** The port is
   opened with RTS and DTR held low and used at speed B0, as Signum always
   opens a radio's port ([Keying](#keying-rts-or-civ)), so the open does not
   key a radio set to USB SEND = RTS or DTR. It asks the radio's ID (CI-V
   `19 00`) at the broadcast address, which gives the CI-V address. The
   port's chip keeps the rate it last had; the rate is then measured by
   timing (600 preamble bytes, which the radio skips, before the question:
   they take 52 ms at 115200 Bd, 625 ms at 9600). On the first open after the
   radio was plugged in (Linux has raised RTS/DTR at that open already,
   unavoidably) it tries the rates the radio offers (115200, 9600, 19200,
   38400, 57600, 4800) from one to the next, which moves no line. If the
   radio does not answer at the port's present rate, finding it means leaving
   B0, which raises RTS/DTR for about a millisecond: Signum asks first.
3. **Reads every setting below** (each answer is taken only when the radio
   gives the same twice in a row: another program polling the same port can
   take bytes out of the stream), and the TX state: a radio that is
   transmitting is left alone.
4. **Shows the list and the warning** and waits for your `YES` (GUI: the
   checkbox, then **Reconfigure the radio**).
5. **Saves the backup** (next section). Nothing is changed if it cannot be
   written, or if any setting it would change could not be read.
6. **Writes each setting that differs**, then reads it back and compares;
   one more try on a mismatch. The keying items are ordered so that no two of
   USB SEND, USB Keying (CW) and USB Keying (RTTY) ever share a line (the
   radio refuses that), and **USB SEND = RTS comes last**.
7. **Reads everything again** and reports any setting that is not as wanted.
8. **Writes Signum's settings file** (only when every setting is right) and
   prints what it wrote.

### What it sets

Items `1A 05 nnnn` are the radio's **MENU » SET » Connectors** items, set
over CI-V; the numbers are Icom's (the IC-7300 manual's "Control command"
chapter) and every one was written and read back on a bench radio (bench).

| CI-V | Setting | Set to | Note |
|---|---|---|---|
| `1A 05 0079` | USB Keying (CW) | OFF | first: a control line must not key CW |
| `1A 05 0080` | USB Keying (RTTY) | OFF | |
| `1A 05 0059` | ACC/USB Output Select | AF | |
| `1A 05 0060` | ACC/USB AF Output Level | 75 % (191/255) | the bench value ([Receive level](#receive-level)) |
| `1A 05 0061` | ACC/USB AF SQL | OFF (OPEN) | |
| `1A 05 0062` | ACC/USB AF Beep/Speech... Output | OFF | |
| `1A 05 0065` | USB MOD Level | 100 % (255/255) | the bench value; the level is then set with Signum's TX gain ([Transmit level](#transmit-level)) |
| `1A 05 0066` | DATA OFF MOD | MIC, **only if it was USB** | MIC, ACC and MIC,ACC are left as they are (your microphone's routing in plain USB) |
| `1A 05 0067` | DATA MOD | USB | |
| `1A 05 0075` | CI-V USB Echo Back | OFF | (the radio reads 01 for ON, 00 for OFF; bench) |
| `1A 05 0076` | USB Serial Function | CI-V | already so, or Signum could not talk to the radio |
| `06`, `1A 06` | Mode, DATA | USB, FIL1, DATA on (USB-D) | |
| `1A 03` | IF filter width | 3.0 kHz | FIL1 of USB-D (the data mode keeps its own filter settings) |
| `16 12` | AGC | MID | |
| `16 22`, `16 40`, `16 41`, `16 48` | NB, NR, auto notch, manual notch | OFF | |
| `16 44`, `16 46`, `16 45` | COMP, VOX, monitor | OFF | |
| `0F`, `21 01`, `21 02` | Split, RIT, XIT (ΔTX) | OFF | |
| `1A 05 0078` | **USB SEND** | **RTS** | last: what lets RTS key the transmitter ([Keying](#keying-rts-or-civ)) |

**Read and kept in the backup only, never written:** the frequency, RF
POWER, AF and RF gain, preamp, ATT, TBW, ACC/USB IF Output Level, the external keypad items, CI-V Transceive, CI-V USB→REMOTE Transceive
Address, CI-V Output (for ANT), CI-V USB Port (read only over CI-V), RTTY
Decode Baud Rate. **Not reachable over CI-V at all** (Icom gives them no
`1A 05` number): CI-V Baud Rate, CI-V Address and CI-V USB Baud Rate; the
set-up finds the address and the USB rate and works with whatever they are.

A second run finds nothing to change.

### The warning

On the command line, after the list of settings:

```
============================================================================
  WARNING: THIS RECONFIGURES YOUR RADIO
============================================================================
  Signum is about to RECONFIGURE this IC-7300 (serial 02019380).
  The radio's current values of the settings below will be LOST from the radio.
  Before changing anything, Signum saves them to a backup file in ~/.config/signum/
  (ic7300-backup-02019380-<time>.conf); `signum --restore-ic7300-settings FILE` or the
  GUI's Restore button puts them back.
  It does NOT change the transmit power or the frequency, and it does not transmit.
  4 setting(s) change (the list above). Signum's own settings file is then set for this radio.
============================================================================

Type YES (in capitals) and press Enter to go ahead; anything else stops:
```

In the GUI the same text stands in a red box under the list, with the
checkbox **I understand my radio's current configuration will be lost**;
**Reconfigure the radio** stays disabled until it is ticked.

### The backup and restoring it

The backup is a plain text file,
`~/.config/signum/ic7300-backup-<serial>-<UTC time>.conf` (mode 0600): the
radio's serial number, port, CI-V address and rate, then one line per
setting with the radio's own CI-V data bytes and what they mean:

```
serial = 02019380
address = 94
baud = 115200
menu.0067 = 03    # DATA MOD: USB
menu.0078 = 02    # USB SEND: RTS
rf_power = 00 02    # RF POWER (set-ups never change it): 2/255 (never written)
...
```

`signum --restore-ic7300-settings FILE` (or **Configure » Radio & audio »
Restore IC-7300 settings…**, then the file, the checkbox and **Restore these
settings**) writes every setting the set-up may change back to the radio the
file names, each read back; it refuses a radio with another serial number,
and never writes the lines marked "never written". The one exception is a
backup taken by `--ic7300-power` ([Setting the power](#setting-the-power)):
it carries the line `restores_rf_power = yes`, and its restore puts RF POWER
back too. `--backup-ic7300-settings`
(GUI: **Back up only**) saves such a file at any time without changing
anything.

### Signum's settings file

When every setting is right, the set-up writes into
`~/.config/signum/signum.conf` (or `--config FILE`; other lines are kept):

```
alsa = plughw:CARD=CODEC
ptt = rts:/dev/serial/by-id/usb-Silicon_Labs_CP2102_USB_to_UART_Bridge_Controller_IC-7300_<serial>-if00-port0
tx-gain = 4
prime-ms = 40
no-autocal = off
```

These are the values the bench and on-air links ran with: the radio's own codec,
keying by RTS on its by-id port, TX gain +4 dB with PSK level 140 (the default;
not a settings-file option), the default 40 ms playback buffer, and
self-calibration on. Signum starts with them from then on. Your call sign is
not touched (`call = ...`).

### On the bench

(bench, 2026-11-16, both bench radios, RF POWER 2/255, nothing keyed by it)

- Found both radios and their sound cards (`CODEC`, `CODEC_R1`, matched by
  USB hub), address 94h on both; rate measured 115200 Bd (radio 1, 52 ms for
  the 600 preamble bytes) and 9600 Bd (radio 2, 625 ms). With CI-V USB Echo
  Back ON the timed question got no answer, so radio 2 showed "rate not
  measured" until its echo was off; nothing else depends on it.
- Every item it writes, written to another value and back on radio 1, each
  read back: all 49 checks right; echo back seen on the wire at 0075 = 01 and
  not at 00.
- Set-up on radio 2: 4 changes (AF Output Level 95 % → 75 %, Beep/Speech
  output ON → OFF, Echo Back ON → OFF, USB Keying (CW) DTR → OFF), all read
  back; a second look found nothing to change. Restored from its backup: all
  43 settings read back exactly as before.
- Set-up on radio 1 (already as Signum wants it): nothing to change; then
  two P3 links through the bench's harness with the settings file it wrote:
  Signum sent 12,000 characters (exact, 45.1 s) and received 12,000 from
  the reference unit (exact, 83.0 s).

### Using the radio with an external modem instead

The radio also has an ACC socket. A hardware modem on it (the reference unit
is one) takes its audio from the socket, puts its transmit audio into it, and
keys the radio with the socket's SEND line. That wants the radio set
differently from Signum's USB set-up, and the menus are a bother to flip by
hand, so there is a second set-up for it:

```
signum --configure-my-ic7300-for-an-external-modem-and-overwrite-its-settings
```

(GUI: **Configure » Radio & audio » Set up my IC-7300 for an external
modem…**, the same dialog.) It works exactly like the set-up above: finds the
radio, shows its present settings and what it will change, warns you, saves a
backup first, needs a typed `YES` (or `--i-understand-this-overwrites-my-ic7300-settings`),
takes `--ic7300-serial N` with more than one radio, writes each setting and reads
it back, never touches RF POWER or the frequency, never transmits, and a
second run finds nothing to change. The GUI refuses while a link is up.

**It does not write Signum's settings file**: Signum is not the modem now.
It prints one line saying that **Signum cannot key this radio until the
Signum set-up is run again**. To flip back:

```
signum --configure-my-ic7300-and-overwrite-its-settings      # for Signum on USB
signum --configure-my-ic7300-for-an-external-modem-and-overwrite-its-settings   # for the modem on ACC
```

Both set-ups cover the same items, so each brings the radio back from the
other; each keeps its own backup, and `--restore-ic7300-settings` still puts
a radio back as it was before either.

| Setting | Signum on USB | External modem on ACC | Why they differ |
|---|---|---|---|
| DATA MOD (`1A 05 0067`) | USB | **ACC** | the modem's audio comes in on the ACC socket |
| USB SEND (`1A 05 0078`) | RTS | **OFF** | see below |
| USB Keying (CW), (RTTY) (`0079`, `0080`) | OFF | OFF | same |
| USB MOD Level (`0065`) | 100 % | **left as it is** | not used with DATA MOD = ACC; leaving it makes flipping back and forth cost nothing |
| ACC MOD Level (`0064`) | left as it is | **50 %** (128/255) | see below |
| everything else it sets | as in [What it sets](#what-it-sets) | the same | |

The "everything else" are: mode USB, DATA on (USB-D), FIL1, 3.0 kHz; AGC MID;
NB, NR, auto and manual notch, COMP, VOX, monitor, split, RIT, XIT off;
ACC/USB Output Select AF; ACC/USB AF Output Level 75 %; ACC/USB AF SQL OFF
(OPEN); ACC/USB AF Beep/Speech... Output OFF; DATA OFF MOD MIC only if it was
USB; CI-V USB Echo Back OFF; USB Serial Function CI-V.

**Why each value** (**fact**: measured or printed in the manual;
**reasoning**: inferred, not tried on a modem with this program):

- **DATA MOD = ACC, not MIC,ACC.** *Fact:* the radio offers MIC, ACC, MIC,ACC
  and USB, and on the bench the reference unit's audio was run with MIC,ACC
  (a bench tool set radio 2 that way). *Reasoning:* with MIC,ACC the
  microphone input stays live in every data transmission, so a microphone
  (or its hum, or a room) is added to the modem's signal; with ACC alone
  nothing but the socket is heard. The modem's audio is on the socket either
  way, so ACC alone should do everything MIC,ACC did. If a radio has nothing
  plugged into the microphone jack the two behave alike. Not yet tried on the
  air with the set-up's value: check the first transmission (below).
- **USB SEND = OFF and USB Keying (CW), (RTTY) = OFF.** *Fact:* USB SEND = RTS
  (or DTR) is what makes a program that opens the radio's USB serial port
  able to key the transmitter by that port's control lines; opening a port
  for a moment can raise them. *Reasoning:* with the modem on the ACC socket
  it keys the radio by SEND, so nothing on USB should be able to: Signum, a
  stray `rigctld`, or a terminal program cannot key it through RTS/DTR while
  the modem owns the radio. The radio can still be keyed by a CI-V command
  (`1C 00 01`); a program that sends that, such as `rigctld` with PTT through
  CAT, can still do so. Nothing here sends it.
- **ACC/USB Output Select AF, AF Output Level 75 %, AF SQL OFF (OPEN), Beep
  OFF.** *Fact:* these items cover the ACC socket and the USB codec together
  (the menu items are named "ACC/USB"), so what the modem hears is what the
  menu sets. AF is the receive audio (IF is the 12 kHz intermediate frequency);
  with the squelch on, the radio mutes it between signals; with the beep and
  speech output on, the radio's announcements go into it. *Reasoning:* the
  modem wants the receive audio, always present, without announcements, so the
  values are Signum's; the 75 % is Signum's bench value, not a measured
  level for the modem, whose input is adjusted on its side. (Radio 2 read 95 %
  on 2026-11-15; that level was not chosen or measured for a modem.) Change it on the radio if the modem's own
  level indicator says so; the next set-up of either kind puts it back to 75 %.
- **ACC MOD Level: 50 %** (128/255, the factory value) by the external
  set-up; Signum's set-up does not write it (it does not use the ACC socket).
  *Fact:* it is the gain of the audio going from the socket into the
  transmitter. *Reasoning:* 50 % is the known-good value with a hardware modem
  on the ACC socket: on the bench IC-7300s, with the modem's PSKA 600 / FSKA
  270 settings, it gave 13-14 W at a 20 % power limit with the ALC at 0.14 or
  less. Adjust the modem's own PSKA/FSKA level for drive; if your modem needs
  a different ACC MOD Level, change the menu on the radio after the set-up
  (the next external set-up puts it back to 50 %). The backup records the old
  value and a restore puts it back.
- **Everything else** is the same as for Signum, for the reasons in [What
  it sets](#what-it-sets): the data mode with its filter and no processing
  (NB, NR, notches, COMP) suit any data modem; VOX and the monitor would feed
  the radio's own audio back; split, RIT and XIT move the transmit
  frequency. CI-V stays on (USB Serial Function CI-V, Echo Back OFF) so a
  program can still read the radio's frequency.

**For the first transmission**, on a real radio: set RF POWER low yourself
and key the modem into a dummy load or a quiet channel; check that the radio's
ALC meter reads what it did with the modem before, and that the modem's own
level (its PSKA/FSKA settings) is right for the radio's ACC MOD Level (50 %
after the set-up). If the transmit audio is weak or distorted, adjust the
modem's level first, then the ACC MOD Level (menu).

### What it does not do

- **The PCM playback volume** of the codec (ALSA mixer): set it once by hand
  ([The mixer](#the-mixer)).
- **The sound server**: keep PipeWire/PulseAudio off the radio's card
  ([Keep the sound server off the radio's card](#keep-the-sound-server-off-the-radios-card)).
- **The `dialout` group**, and the transmit level ([Transmit level](#transmit-level)).
- Any radio but the IC-7300. Doing the same by hand: the sections below.

**Where each value comes from.** Every recommended value is labelled:

| Label | Meaning |
|---|---|
| **bench** | read from, used on or measured with the two IC-7300s of the development test bench |
| **default** | Signum's default |
| **guidance** | general practice for HF data on this radio, not measured with Signum |

The bench radios ran at very low power (2 of 255 on the RF POWER scale)
into short test antennas on 6 m, so the bench says nothing about the levels
at full power; those steps are **guidance**.

## At a glance

| Where | Setting | Value | Label |
|---|---|---|---|
| Radio | mode | USB-D (USB, DATA on) | bench |
| Radio | filter | FIL1 at 3.0 kHz (at least 2.4 kHz) | bench |
| Radio | DATA MOD | USB | bench |
| Radio | USB MOD Level | 100 % (start; see the levels) | bench |
| Radio | ACC/USB AF Output Level | 75 % | bench |
| Radio | ACC/USB AF SQL | OFF (OPEN) | bench |
| Radio | USB SEND | **RTS** with `rts:` keying (recommended); OFF with `civ:` keying | bench / guidance |
| Radio | CI-V Address | 94h (the factory value) | bench |
| Radio | CI-V USB Port | Unlink from [REMOTE] | bench |
| Radio | VOX, COMP, NB, NR, notch, split, RIT, XIT | all OFF | bench |
| Radio | AGC | MID | bench |
| Linux | sound card | `plughw:CARD=CODEC` (the card is called "USB Audio CODEC") | bench |
| Linux | serial port | `/dev/serial/by-id/usb-Silicon_Labs_CP2102_USB_to_UART_Bridge_Controller_IC-7300_<serial>-if00-port0` | bench |
| Linux | mixer `PCM Playback Volume` | fixed (bench: 105 of 128); the codec has no capture control | bench |
| Signum | `--ptt` | `rts:<port>` (`civ:<port>:94:<baud>`: see [Keying](#keying-rts-or-civ)) | bench |
| Signum | PSK level (`PSKAmpl`) | 140 | default, bench |
| Signum | `--tx-gain` | **+4** (the set-up writes it; on the air 2026-11-19), then checked with the ALC meter | on air |
| Signum | self-calibration | on (the default); expect a hardware round trip of 20-22 ms | default, bench |

## Setting it up by hand, menu by menu

Everything the [zero-effort set-up](#zero-effort-set-up) does, done on the
radio's own screen, plus the parts it leaves to you. The values are the
same ones the set-up writes (Signum's code), and the reasons are in the
tables further down ([Menu settings](#menu-settings-menu--set--connectors),
[Operating settings](#operating-settings)). Write down your present values
first if you may want them back (the set-up's `--backup-ic7300-settings`
does that for you, and changes nothing).

**1. MENU » SET » Connectors** (press **MENU**, touch **SET**, then
**Connectors**; touch an item, choose the value, go back with **EXIT**):

| # | Item | Set to |
|---|---|---|
| 1 | ACC/USB Output Select | **AF** |
| 2 | ACC/USB AF Output Level | **75 %** |
| 3 | ACC/USB AF SQL | **OFF (OPEN)** |
| 4 | ACC/USB AF Beep/Speech... Output | **OFF** |
| 5 | ACC/USB IF Output Level | leave as it is |
| 6 | ACC MOD Level | leave as it is (**50 %** in the external-modem set-up) |
| 7 | USB MOD Level | **100 %** |
| 8 | DATA OFF MOD | **MIC** (or MIC,ACC); anything but USB |
| 9 | DATA MOD | **USB** |
| 10 | External Keypad (VOICE, KEYER, RTTY) | leave as it is (OFF) |

**2. MENU » SET » Connectors » CI-V:**

| # | Item | Set to |
|---|---|---|
| 11 | CI-V Baud Rate | leave as it is (the rear REMOTE jack only) |
| 12 | CI-V Address | **94h** (the factory value) |
| 13 | CI-V Transceive | leave as it is (ON or OFF both work) |
| 14 | CI-V USB→REMOTE Transceive Address | leave as it is (00h) |
| 15 | CI-V Output (for ANT) | **OFF** |
| 16 | CI-V USB Port | **Unlink from [REMOTE]** |
| 17 | CI-V USB Baud Rate | **115200** (used only by `civ:` keying and the set-up's rate search; any rate works with `rts:`) |
| 18 | CI-V USB Echo Back | **OFF** |

**3. Back in MENU » SET » Connectors, the USB serial items.** Set the two
USB Keying items **before** USB SEND: the radio refuses to give two of these
items the same control line, so USB SEND = RTS is refused while USB Keying
(CW) or (RTTY) is on RTS.

| # | Item | Set to |
|---|---|---|
| 19 | USB Serial Function | **CI-V** |
| 20 | RTTY Decode Baud Rate | leave as it is |
| 21 | USB Keying (CW) | **OFF** |
| 22 | USB Keying (RTTY) | **OFF** |
| 23 | USB SEND | **RTS** (for `--ptt rts:...`, recommended). Only if you key with `civ:`: **OFF** |

**4. The operating controls** (front panel; the menu has none of these):

| # | Setting | Set to |
|---|---|---|
| 24 | Mode | **USB**, then **DATA** on: the display shows **USB-D** |
| 25 | Filter | **FIL1**, width **3.0 kHz**, set while in USB-D (the data mode keeps its own filter settings) |
| 26 | AGC | **MID** |
| 27 | NB, NR, auto notch, manual notch | all **OFF** |
| 28 | COMP, VOX, monitor | all **OFF** |
| 29 | Split, RIT, XIT (ΔTX) | all **OFF** |
| 30 | TBW (SSB transmit bandwidth) | **WIDE** (guidance; the set-up only reads it) |
| 31 | RF gain, ATT, preamp | RF gain at maximum, ATT off, preamp as the band needs |
| 32 | RF POWER | what your station and the link need. Signum never sets it |

**5. Linux** (once): your user in the `dialout` group
([The serial port](#the-serial-port)); the codec's **PCM playback volume**
at a fixed value with its switch on, saved with `sudo alsactl store`
([The mixer](#the-mixer)); the desktop's sound server kept off the radio's
card ([Keep the sound server off the radio's
card](#keep-the-sound-server-off-the-radios-card)).

**6. Signum** (the settings file `~/.config/signum/signum.conf`, or the
GUI's Configure tab, or the same options on the command line):

```
call = N0CALL
alsa = plughw:CARD=CODEC
ptt = rts:/dev/serial/by-id/usb-Silicon_Labs_CP2102_USB_to_UART_Bridge_Controller_IC-7300_<serial>-if00-port0
tx-gain = 4
prime-ms = 40
```

PSK level (`PSKAmpl`) stays at its default 140 and self-calibration stays
on. With `civ:` keying (not recommended) the `ptt` line is
`civ:<port>:94:115200`, matching items 12 and 17.

**7. Check it:** `signum --detect-ic7300` reads the radio (it changes
nothing) and lists what the set-up would still change. After a complete
set-up by hand it finds nothing to change.

**8. The levels:** set the transmit level by the ALC meter
([Transmit level](#transmit-level)) and check the receive level
([Receive level](#receive-level)).

## The radio

### Menu settings (MENU » SET » Connectors)

Press **MENU**, touch **SET**, then **Connectors**. The CI-V items are in the
**CI-V** submenu of Connectors.

| Menu item | Set to | Why | Label | Bench radio 1 / radio 2 |
|---|---|---|---|---|
| ACC/USB Output Select | **AF** | the receive audio, not the 12 kHz IF | bench | AF / AF |
| ACC/USB AF Output Level | **75 %** (50-95 % all work) | the receive level; see [Receive level](#receive-level) | bench | 75 % / 95 % |
| ACC/USB AF SQL | **OFF (OPEN)** | with ON the radio's squelch mutes the USB audio, and the modem hears nothing between signals | bench | OFF / OFF |
| ACC/USB AF Beep/Speech... Output | **OFF** | with ON the radio's beeps and voice announcements go into the receive audio | guidance | OFF / ON |
| ACC/USB IF Output Level | leave | used only with Output Select = IF | bench | 50 % / 50 % |
| ACC MOD Level | leave (**50 %** for an external modem) | the ACC socket's input, not used with Signum; the external-modem set-up writes 50 %, the known-good value with a hardware modem | bench | 50 % / 50 % |
| **USB MOD Level** | **100 %** to start | the transmit audio's gain inside the radio; see [Transmit level](#transmit-level) | bench | 100 % / 100 % |
| DATA OFF MOD | **MIC** (the factory value) or MIC,ACC | the source in plain USB (data off): leave the microphone there for voice | guidance | MIC,ACC / MIC |
| **DATA MOD** | **USB** | the source in USB-D: the computer's audio | bench | USB / USB |
| External Keypad (VOICE, KEYER, RTTY) | OFF | | bench | OFF / OFF |
| CI-V » CI-V Baud Rate | any | the rear REMOTE jack only; not the USB port when the port is unlinked | guidance | (not readable) |
| CI-V » **CI-V Address** | **94h** | the factory value; `civ:` needs it | bench | 94h / 94h |
| CI-V » CI-V Transceive | ON (the factory value) or OFF | with ON the radio reports dial and mode changes by itself; Signum ignores those reports | bench | ON / ON |
| CI-V » CI-V USB→REMOTE Transceive Address | leave (00h) | | bench | 00h / 00h |
| CI-V » CI-V Output (for ANT) | OFF | | bench | OFF / OFF |
| CI-V » **CI-V USB Port** | **Unlink from [REMOTE]** | the USB port runs at its own baud rate, apart from the REMOTE jack | bench | Unlink / Unlink |
| CI-V » **CI-V USB Baud Rate** | **115200** | give Signum the same value (`civ:...:94:115200`) | bench (radio 1) | 115200 / 9600 (as used; this item cannot be read over CI-V) |
| CI-V » CI-V USB Echo Back | OFF or ON | Signum skips its own echoed commands either way | bench | OFF / ON |
| **USB Serial Function** | **CI-V** | with "RTTY Decode" the port carries no CI-V | bench | CI-V / CI-V |
| **USB SEND** | **RTS** with `rts:` keying (recommended); **OFF** with `civ:` keying | what keys the transmitter from the USB serial port's control lines ([Keying](#keying-rts-or-civ)) | bench (RTS), guidance (OFF) | RTS / RTS |
| USB Keying (CW) | **OFF** | with DTR or RTS a control line keys CW in CW mode | guidance | OFF / DTR |
| USB Keying (RTTY) | OFF | | bench | OFF / OFF |

The bench values in this table were read over CI-V from the two radios on
2026-11-15 (how: [The bench radios](#the-bench-radios)). The
[zero-effort set-up](#zero-effort-set-up) sets the ones in bold (and the
others it lists) by itself.

### Operating settings

| Setting | Set to | Label | Note |
|---|---|---|---|
| Mode | **USB-D**: USB, then **DATA** on (the display shows USB-D) | bench | P1/P2/P3 use upper sideband. USB-D takes the transmit audio from DATA MOD (USB) and keeps the microphone out |
| Filter | **FIL1, 3.0 kHz** (at least 2.4 kHz) | bench (3.0 kHz) | P3 uses up to 18 carriers 120 Hz apart, about 2.2 kHz wide, centred on 1500 Hz: roughly 450 to 2,550 Hz of audio. Check the width on the display in USB-D: the data mode keeps its own filter settings, apart from plain USB's. P1 and P2 sit on 1400 / 1600 Hz and are narrower |
| TBW (transmit bandwidth) | **WIDE** | guidance | WIDE passes the whole P3 signal. Whether the IC-7300 applies TBW in USB-D was not checked; with WIDE it does not matter. The bench radios read WIDE (CI-V `16 58` = 00: SSB transmit bandwidth WIDE, Icom's manual) |
| AGC | **MID** | bench | MID on every bench link; SLOW should do as well (guidance) |
| NB, NR, manual notch, auto notch | **OFF** | bench | each one changes the signal the modem has to decode |
| COMP (speech compressor) | **OFF** | bench | it must never act on a data signal; keep it off whatever the mode |
| VOX | **OFF** | bench | [Why not VOX](radio-setup.md#why-not-vox): the hang time swallows the other station's answer |
| Split, RIT, XIT (ΔTX) | **OFF** | bench | an offset between transmit and receive moves our signal away from where the other station listens. Signum follows small frequency offsets by itself |
| Preamp | as the band needs (P.AMP1 or off below about 20 MHz) | guidance | the bench radios run P.AMP2 on 6 m. With a strong local noise floor, turning the preamp off can improve the signal-to-noise ratio (seen on the bench) |
| ATT, RF gain | ATT off; RF gain at maximum | bench (RF gain) | |
| AF gain (the knob) | anything | bench | the USB receive audio does not follow the AF knob (the bench radios sit at the minimum and receive fine) |
| Monitor | OFF | bench | |
| RF POWER | what the link needs, never more than your amplifier or antenna takes | guidance | P3 is a multi-carrier signal: its average power is well below the peak ([levels](#transmit-level)) |
| Beep | to taste, if ACC/USB AF Beep/Speech... Output is OFF | guidance | |
| ATU (internal tuner) | as needed: press TUNE once per frequency; it remembers | guidance | the internal tuner tunes on its own carrier. Signum's [tuning tone](#tuning-tone-and-the-tuner) is for an external automatic tuner |

## Linux

### The sound card

The IC-7300's USB codec shows up as a sound card named **CODEC**
("USB Audio CODEC"):

```
$ arecord -l
card 4: CODEC [USB Audio CODEC], device 0: USB Audio [USB Audio]
```

Use it with `plughw`, which converts to the 48 kHz mono 16-bit Signum uses:

```
--alsa plughw:CARD=CODEC          # or plughw:CARD=CODEC,DEV=0: the same
```

`--alsa` takes an ALSA device name only, not a path under `/dev/snd/by-id/`.

**Two radios (or any second "USB Audio CODEC").** The second card is named
`CODEC_1`, and which radio gets which name depends on the order the USB
devices appear. Give each card a fixed name with a udev rule keyed on the USB
port the radio is plugged into (bench):

```
# /etc/udev/rules.d/70-ic7300-audio.rules
SUBSYSTEM=="sound", KERNEL=="card*", KERNELS=="1-1.2.3.4", ATTR{id}="CODEC"
SUBSYSTEM=="sound", KERNEL=="card*", KERNELS=="1-1.1.4", ATTR{id}="IC7300B"
```

`udevadm info -a /dev/snd/controlC4 | grep KERNELS` shows the USB path of a
card (the IC-7300 has a USB hub inside, so its codec sits one level deeper than
the socket you plug into). Moving the cable to another socket changes the
path.

### The mixer

The IC-7300's codec has **one** mixer control: playback volume. There is no
capture (receive) control; the receive level is set on the radio only (ACC/USB
AF Output Level). On both bench radios (bench):

```
$ amixer -c CODEC get PCM
Simple mixer control 'PCM',0
  Capabilities: pvolume pswitch pswitch-joined
  Limits: Playback 0 - 128
  Front Left: Playback 105 [82%] [-23.00dB] [on]
  Front Right: Playback 105 [82%] [-23.00dB] [on]
```

- Keep **PCM Playback Switch on** (not muted).
- The playback volume scales everything Signum sends. Pick one value, leave
  it there, and set the level with Signum's TX gain instead
  ([Transmit level](#transmit-level)). The bench runs at 105 of 128 (the
  driver reports -23 dB for it; not measured).
- Make it survive a reboot: `sudo alsactl store` after setting it (most
  distributions restore the saved state at boot). Check after replugging the
  radio: a newly seen card can start at another value.

### Keep the sound server off the radio's card

On a desktop, PipeWire or PulseAudio opens every sound card it finds,
including the radio's. Signum then gets "cannot open plughw:CARD=CODEC"
(ALSA `hw`/`plughw` devices serve one program at a time), or the desktop
plays its sounds into the transmitter, or changes the playback volume behind
your back. Tell the sound server to leave that card alone (guidance; the bench
computer runs no sound server):

- **Any of them (PipeWire with pipewire-pulse, or PulseAudio):** set the
  card's profile to off. The setting is remembered across restarts.

  ```
  pactl list cards short          # find the name, e.g. alsa_card.usb-Burr-Brown_from_TI_USB_Audio_CODEC-00
  pactl set-card-profile alsa_card.usb-Burr-Brown_from_TI_USB_Audio_CODEC-00 off
  ```

- **PipeWire with WirePlumber 0.5 or later**, to hide the card for good:

  ```
  # ~/.config/wireplumber/wireplumber.conf.d/50-radio-codec.conf
  monitor.alsa.rules = [
    {
      matches = [ { device.name = "~alsa_card.usb-Burr-Brown_from_TI_USB_Audio_CODEC*" } ]
      actions = { update-props = { device.disabled = true } }
    }
  ]
  ```

  then `systemctl --user restart wireplumber`. (Older WirePlumber 0.4 uses
  Lua files under `~/.config/wireplumber/main.lua.d/` instead.)

`fuser -v /dev/snd/*` shows which program holds a sound device.

### The serial port

The IC-7300's USB serial port is a CP2102 chip. Use its stable name, which
carries the radio's serial number:

```
$ ls /dev/serial/by-id/
usb-Silicon_Labs_CP2102_USB_to_UART_Bridge_Controller_IC-7300_0201xxxx-if00-port0
```

Your user needs the `dialout` group (`sudo usermod -aG dialout $USER`, then log
in again).

**Do not open this port with other tools while the radio can key on it.**
With USB SEND = RTS (or DTR), Linux raises both control lines for an instant
at every open of the port, before any program can lower them, and the radio
transmits for about 15-20 ms (bench: 15-23 ms per open). `stty -F`,
terminal programs and most rig-control programs open it that way. Signum
does not ([PTT](radio-setup.md#ptt)); with `civ:` keying and USB SEND = OFF
no open can key the radio at all.

## Signum

### Keying: `rts:` or `civ:`

| | `--ptt rts:PORT` | `--ptt civ:PORT:94:BAUD` |
|---|---|---|
| How | the RTS line of the USB serial port | the CI-V "TX on / TX off" command over the USB serial port |
| Radio | USB SEND = RTS | USB SEND = OFF |
| A port open by any program | keys it for 15-20 ms unless the program uses the port as Signum does | cannot key the radio |
| Signum checks the radio | no | yes: while it is not transmitting it asks every 0.5 s whether the radio is keyed, and unkeys it if so |
| The radio's answer | none | Signum sees whether the radio answers at start (logged) |
| Bench | **every bench link**; receiving 12,000 characters in P3, 5 of 5 links clean in the comparison below | sending: as fast as `rts:` (2 links); **receiving: 5 of 7 links failed or degraded** (below) |

**Use `rts:`** (bench). It is what every bench link ran, and the only way
measured to work in both directions.

**`civ:` on the bench** (bench, 2026-11-16; the radio's USB SEND left at
RTS, which Signum holds low, not the OFF this mode is meant for; the
bench's safety monitor read the same CI-V port all the time). Signum on
radio 1, the reference unit on radio 2, P3, 12,000 characters each way, the
same Signum build with `rts:` alternating as the control:

| | `rts:` | `civ:` |
|---|---|---|
| Signum sends | 45.1 s, exact | 45.8 / 52.6 s, exact |
| Signum receives | 5 of 5 links exact and held to the end, 0-2 failed packets each | 2 of 7 like that; 2 links died right after the text, 1 lost 49 packets, 1 lost 33 and never reached long packets, 1 delivered only 1,254 characters (the far station missed 23 of our acknowledgements) |
| Radio found keyed after Signum's unkey | (not checked) | twice: Signum's check saw the radio still transmitting about 1 s after its CI-V "TX off" and sent it again |

The keying itself comes on in time (every link set up at the first call; the
far station repeated nothing for a missed acknowledgement in 6 of the 7
receiving links). What fails is the receiver's side: the radio does not
always leave transmit on the CI-V "TX off", so it is deaf for the far
station's next packet, and some acknowledgements did not go out. Why is not
known yet: lost CI-V commands while a second program uses the same port
(as on the bench) is one candidate; the bench has not tried `civ:` with USB
SEND = OFF and nothing else on the port. Until it has, use `rts:`. If you
use `civ:` anyway, give Signum the port to itself (to share rig control,
use `--ptt rig` with `rigctld`, [PTT](radio-setup.md#ptt)).

Both ways, Signum opens the port with RTS and DTR low and leaves it at speed
B0, so its own start and stop never key the radio (bench, 2026-11-15: 10 of
10 starts of the previous version keyed the radio for 15-21 ms; this version 0
of 10). Only the first open after the radio was switched on or plugged in, or
after another program set a speed on the port, still raises the lines once;
Signum logs it (`[ptt] ... was not at speed B0`). With USB SEND = OFF that
does not transmit.

`civ:` details:

```
--ptt civ:/dev/serial/by-id/usb-Silicon_Labs_CP2102_USB_to_UART_Bridge_Controller_IC-7300_<serial>-if00-port0:94:115200
```

- `94` is the radio's CI-V Address (hex), `115200` its CI-V USB Baud Rate.
  Without a baud rate Signum assumes **19200** (default), which matches an
  IC-7300 only if you set it so; give it explicitly.
- If the radio does not answer, Signum logs `WARNING: no CI-V answer from 94`:
  check the address, the baud rate, USB Serial Function = CI-V, and that the
  radio is on.

### Signum's settings

| Setting | Option (settings file key) | GUI Configure tab | IC-7300 value | Label |
|---|---|---|---|---|
| sound card | `--alsa` (`alsa`) | Radio & audio » Sound card (next start) | `plughw:CARD=CODEC` | bench |
| PTT | `--ptt` (`ptt`) | Radio & audio » PTT (next start) | `rts:PORT` | bench |
| PSK level | `PSKAmpl` command | Link parameters » PSK level | **140** | default, bench |
| TX gain | `--tx-gain` (`tx-gain`) | Link parameters » TX gain (at once; from the next link when one is up) | **+4** with the IC-7300 set-up (Signum's own default is 0); check with the ALC meter | on air |
| FSK level | `FSKAmpl` command | Kept for compatibility » FSKAmpl | has no effect: the FSK follows the PSK level | default |
| key-down watchdog | `--max-key` (`max-key`) | Link parameters » Key-down watchdog | 90 s | default |
| self-calibration | on unless `--no-autocal` | Timing calibration | on | default, bench |
| playback buffer | `--prime-ms` (`prime-ms`) | Timing calibration » playback buffer | 40 ms (leave it) | default, bench |
| tuning tone | `--tune-tone`, `--tune-method`, `--tune-hz`, `--tune-db` | Tuning tone | off; [below](#tuning-tone-and-the-tuner) | default |
| ALC warning | `--alc-warn` (`alc-warn`) | ALC warning (at once) | on (needs `--ptt rig:`); [above](#alc-warning) | default |

A settings file for the IC-7300 (`~/.config/signum/signum.conf`,
[The settings file](options.md#the-settings-file)):

```
call = N0CALL
alsa = plughw:CARD=CODEC
ptt = rts:/dev/serial/by-id/usb-Silicon_Labs_CP2102_USB_to_UART_Bridge_Controller_IC-7300_<serial>-if00-port0
tx-gain = 4
```

**What the two levels do.** PSKAmpl is the reference unit's output level
setting in mV peak-to-peak (10-1000); TX gain is Signum's own, in dB on top
of it (-40 to +20). They multiply: either one moves every mode's level
together, and the FSK of calls and P1 keeps a fixed ratio to the PSK. Leave
PSKAmpl at 140 and use TX gain: it is the one Signum keeps in the settings
file (**Keep live settings at next start** in the GUI). At PSKAmpl 140 and TX
gain 0 Signum's output is (default, measured on its own output; sample
values, full scale 32767):

| Signal | Peak | dBFS (peak) | Average (RMS) | dBFS (RMS) |
|---|---|---|---|---|
| P1 FSK (every call) and the tuning tone at 0 dB | 4,400 | -17.4 | 3,111 | -20.4 |
| P2 data | ~4,950 | -16.4 | 2,213 | -23.4 |
| P3 data | ~5,440 | -15.6 | ~1,780 | -25.3 |

So **P3 has the highest peaks** (about 1.8 dB above the call's FSK) and the
**lowest average power** (about 5 dB below it). Set the level so P3's peaks
cause no ALC; the FSK then has room too. Each dB of TX gain moves every row by
one dB, up to full scale (0 dBFS); P3's peaks reach it first, at about TX
gain +15.

**Timing calibration.** Leave self-calibration on. It learns the sound
card's and the radio's delay from the first calls (bench: the IC-7300's
hardware round trip measured 20.3-21.7 ms, in a calibration file 22 ms) and
keeps it in `~/.config/signum/autocal-plughw_CARD_CODEC.conf`. Nothing on
the radio changes it, except a different sound card path
([Timing calibration](radio-setup.md#timing-calibration)).

## Setting the levels, step by step

### Transmit level

The chain is: Signum's samples (PSKAmpl, TX gain) → the codec's PCM playback
volume → the radio's USB MOD Level → the radio's ALC. Fix the middle two and
set the level with TX gain. The aim: at the power you will run, **no ALC
action on P3 peaks**. ALC on a multi-carrier P3 signal squashes its peaks and
breaks the phase and amplitude the higher P3 and P2 speed levels need: FSK
still works, PSK fails (seen in earlier bench work with these radios).

1. **Dummy load**, RF POWER at the power you mean to use (start low, say
   10 %). Meter: touch the meter on the display until it shows **ALC**.
2. Radio: USB MOD Level 100 % (bench) or the 50 % factory value; DATA MOD
   USB; mode USB-D. Linux: PCM playback volume fixed (bench: 105 of 128),
   switch on.
3. Signum: PSKAmpl 140, TX gain +4 (what the set-up writes).
4. **A steady test signal: the tuning tone.** Set **Tuning tone » Method**
   to Tone and **Duration** 10 s in the GUI (or `TUnetone 10 1500 0 tone`),
   level 0 dB (Method Tone: otherwise the radio's own tune cycle, at the
   radio's RF POWER, would run instead). Connect to any call sign
   (`C N0CALL`): Signum keys and sends a steady 1500 Hz tone for 10 s at
   exactly the call's FSK level. Watch the ALC and power meters, and **press
   Disconnect before the tone ends**, or the call goes out after it.
5. Raise TX gain in 2 dB steps (GUI field, or `--tx-gain` at the next start)
   and repeat step 4 until the ALC meter **just begins to move**. Note that
   TX gain.
6. **Set TX gain 3 dB below that.** P3's peaks are about 1.8 dB above the
   tone's, so 3 dB below the first ALC on the tone keeps them out of the ALC
   with a little margin.
7. Turn the tuning tone off again (Duration 0) unless you use it for a tuner.
8. **Check on a link** (a P3 link with a station you arranged a test with, at
   the same power): the ALC meter should stay at or near zero through P3 data.
   The power meter shows less on P3 than on the tone (P3's average is about
   5 dB lower): that is right, not a level problem. If the link does not
   rise above the low P3 speed levels while the other station's signal is
   good, lower TX gain by 2-3 dB and try again.
9. Save it: **Keep live settings at next start** in the GUI, or
   `tx-gain = N` in the settings file.

If even TX gain +15 gives no power at all, the audio is not reaching the
radio: DATA MOD not USB, mode not USB-D, the PCM switch muted, or the sound
server holding the card. If ALC appears at the lowest TX gain, lower USB MOD
Level (or the PCM volume) and start again.

**What the air showed** (2026-11-19, IC-7300 on a real antenna, RF POWER 51/255 = 20 W, USB MOD Level
100 %, PCM 105 of 128): TX gain 0 gave only 4 W on the tuning tone with the ALC at 0, and links to Winlink
RMSs failed; +4 gave 11.5 W on the tone, the ALC at 0-0.1 and SWR 1.06, and links completed. +4 keeps P3's
peaks out of the ALC, so the set-up writes `tx-gain = 4`. (That "about +7 would reach the 20 W setting" is
doubtful: on 2026-10-15 a 20 % setting held the output at 13-14 W, so at 51/255 the limit is probably about
13.5 W and the 11.5 W at +4 sat just under it: inferred, see [Setting the power](#setting-the-power).)
At a much higher RF POWER the radio may want more drive: check the ALC and power meters as above.

**What the bench shows** (bench): with Signum's defaults (PSKAmpl 140, TX
gain 0), PCM playback 105 of 128 and USB MOD Level 100 %, the radio's ALC
meter read over CI-V (command `15 13`, raw 0-255) during 15 Signum link runs
showed a median of 48-54, 90th percentile 49-69, maximum 50-79, at RF POWER
2 of 255. At so low a power
setting the radio's power control dominates the ALC reading, so these numbers
do not carry over to a normal power setting; use the steps above.

### Setting the power

```
signum --ic7300-power 20             # drive and RF POWER for P2/P3 peaks of 20 W (a model; never transmits)
signum --ic7300-power 20 --measure   # the same, measured on the air (transmits; asks for YES)
```

Console: `ICPower 20` (then `ICPower` alone for the result). GUI:
**Configure » Radio & audio » IC-7300 power**, the watts, **Set IC-7300 power**.

**What the number means.** W is the **peak** power of Signum's P2/P3 PSK
bursts: the highest P3 peaks reach it and the radio's ALC never squashes
them (squashed peaks cost P3's higher speed levels). P1 calls (FSK, a steady
envelope) read lower at the same drive; the answer tells you how much.

**What it sets: the drive and RF POWER.** Two things decide the power, and the
flag sets both:

- **the drive**: how strong Signum's audio is when it reaches the
  transmitter. Signum's **TX gain** sets it; the radio's **USB MOD Level** and
  the codec's PCM volume sit in the same chain and are kept fixed (USB MOD
  Level 100 %, what the Signum set-up writes).
- **RF POWER**: on the IC-7300 a **limit**, not a gain (below).

**ALC, in plain words.** ALC is not a setting you can turn. It is the radio's
automatic level control, and the **ALC meter shows how hard it is working** to
hold the output down. It starts working when the drive pushes the output up
against the RF POWER limit. On a steady tone that does no harm; on P2/P3
PSK it flattens the peaks, and the higher P3 speed levels fail. So the right
ALC for Signum is **at rest (zero, or nearly) on the PSK peaks**, and the
way to get there is the drive: enough to reach the power you want, with RF
POWER a little above it so the limit never touches the peaks.

**How the IC-7300 behaves, and why the drive matters.** RF POWER is a
**limit**, not a gain: below it the drive alone sets the output (TX gain,
PSKAmpl, USB MOD Level, the codec's PCM volume); at it the ALC holds the
output down and flattens PSK peaks (measured 2026-10-15: two drives 4 dB
apart read the same 41-49 W at a 50 % setting; 13 W at 20 %). So no RF POWER
setting can turn a weak drive into 20 W, and a limit set at 20 W with a
strong drive squashes the peaks. `--ic7300-power` therefore:

1. reads the radio (USB MOD Level, RF POWER, DATA MOD, the DATA mode,
   the frequency; it refuses unless DATA MOD is USB and the radio is in a
   DATA mode, i.e. after the Signum set-up);
2. chooses the drive. **Without `--measure`**: it takes the drive for a
   **hot radio**, so that the ALC stays at rest on a hot radio too (below, "Hot
   and cool radios"), with USB MOD Level 100 %. It starts from TX gain **+4 dB**,
   the default of both station IC-7300s (measured on the air 2026-11-19, the
   value the set-up writes), and **keeps it only when even a hot radio's P3
   peaks (with 0.5 dB for peaks between samples) stay under W**: since 0.7.2 that
   is never in 1-100 W (the hot figure follows RF POWER, below). Otherwise it takes the TX gain the model names (0.5 dB steps,
   rounded down: never a peak above W on a hot radio); it also does so when the
   drive would be more than 2.5 dB under W even on a hot radio. PSKAmpl: 140 on
   the command line, the running station's for the console and the GUI. **With
   `--measure`**: the TX gain found on the air (below);
3. saves a backup (marked `restores_rf_power = yes`; it holds the USB MOD Level
   too), writes the USB MOD Level back to 100 % if it was different, then RF
   POWER for a limit 1.1 dB above W, each read back, and prints the setting
   (/255 and the radio's %), the expected peaks, the P1 power, the ALC
   expectation, the assumptions, and the TX gain old → new;
4. **saves the TX gain** where `--tx-gain` and the GUI keep it: `tx-gain = N`
   in the settings file (with `--no-config` it only prints it). The console's
   `ICPower` and the GUI also apply it to the running station at once. On the
   command line it applies at Signum's next start.

If no TX gain in -40..+20 dB can give W (a very low PSKAmpl, say), nothing is
written and the answer says why.

**Hot and cool radios, and RF POWER.** The same drive gave 4.0-4.6 W of P1 at 0 dB on one IC-7300
(radio 2, 2026-11-19, RF POWER 51/255, "cool") and much more on another (radio 1, "hot"). **The
power for the same drive also depends on the RF POWER setting**: below its limit the IC-7300's output scales with
the setting, it is not only a ceiling. Radio 1 gave about 3 W of P1 at 0 dB at 18/255 (2026-11-20, `--measure`'s
first step), at least 15.6 W at 46-69/255 (2026-11-20), and **70 W at 104/255** (2026-11-23, `--measure`: 12.6 W at TX
gain -7.5 and 14.0 W at -7.0; the same evening's calls read 12.5 W at -6 and a flat 25-28 W, the radio's ceiling, at -3 and -1).
The model takes the hot G as 70 W x (setting/104)^1.8 and the cool one as
4.6 W x (setting/51)^1.8 (the exponent is **inferred**: a power law through those points; above 104/255 it is an
extrapolation that errs towards a lower drive). Without `--measure` Signum cannot tell which radio it has, so it chooses
the drive for the hot one: a cool radio then gives less than W, a hot one gives W, and **the ALC stays at rest on both**.
The answer reports P1 and the peaks as a range, cool to hot. A cool radio's lower power costs less than a hot radio's
squashed peaks (on the air, TX gain +4 against a 25 W limit on radio 1 made the RMS answer P to every break-in and held the
link at SL1; the model's old figure for 25 W at RF POWER 104, TX gain -1, was 6 dB too hot: ALC 0.45, P3 peaks to 34 W; at -6/-7
the ALC stayed at 0.03-0.08). `--measure` finds what your radio gives (25 W at RF POWER 104: TX gain -7.0, the model's -7.5).

Examples (USB MOD Level 100 %, PSKAmpl 140; the P3 peaks and P1 are cool to hot):

| W | TX gain | RF POWER | P3 peaks | P1 |
|---|---|---|---|---|
| 10 | -5.5 | 50/255 (20 %) | 1.9-8.1 W | 1.3-5.3 W |
| 20 | **-7.0** (was -2) | 87/255 (34 %) | 3.7-15.5 W | 2.4-10.1 W |
| 25 | **-7.5** (was -1) | 104/255 (41 %) | 4.5-19.0 W | 2.9-12.4 W |
| 40 | -8.0 | 151/255 (59 %) | 7.9-33.2 W | 5.1-21.7 W |
| 60 | -9.0 | 209/255 (82 %) | 11.2-47.4 W | 7.3-31.0 W |
| 100 | -8.5 | 255/255 (100 %; the limit cannot be 1.1 dB above) | 18.0-76.0 W | 11.8-49.7 W |

(RF POWER is 1.1 dB above W by the limit curve; it does not depend on the drive. 20 W: a limit of about 26 W.)

**The model and its data** (measured = read on a radio; inferred = worked out):

| Quantity | Value | Source |
|---|---|---|
| P1 watts at 0 dB drive (TX gain 0, PSKAmpl 140, USB MOD 255, PCM 105/128), a **cool** radio | 4.0-4.6 W | **measured** on the air 2026-11-19, radio 2, one band, at RF POWER 51/255: TX gain 0 → 4 W, +4 → 11.5 W, tuning tone, ALC 0-0.1 |
| the same, a **hot** radio, at RF POWER 104/255 | about 70 W | **measured** 2026-11-23, radio 1, 3591 kHz, 104/255, the radio's own Po meter, `--measure`'s steady tone: 12.6 W at TX gain -7.5, 14.0 W at -7.0 (ALC 0); the calls that evening read 12.5 W at -6 (50 W) and 25-28 W (the ceiling) at -3 and -1; the larger figure is used. At other settings G scales with RF POWER, about setting^1.8 (**inferred**: ~3 W at 18/255 on 2026-11-20, at least 15.6 W at 46-69). Earlier: | **measured** 2026-11-20, radio 1, 80/40/20 m, the radio's own Po meter: P1 at +2 dB drive read 24.8 / 24.1 / 23.4 W on 80 / 40 / 20 m (15.6 W at 0 dB). The drive is chosen from this figure; the cause of the difference between radios is **not known** |
| the radio's real limit against the limit curve, a hot radio | Po about 1.3-1.4 × the curve's watts at RF POWER 46, 1.18-1.25 × at 69 | **measured** 2026-11-20, radio 1, the radio's own Po meter. The curve errs low on this radio: a limit that keeps the ALC at rest by the curve is higher still in fact |
| the ALC meter while Po is held at the limit | 0-4 of 255 (the zone starts at 120) | **measured** 2026-11-20, radio 1: **the ALC meter does not show the limit on this radio**; do not read "ALC 0" as "no limiting" (Signum's drive choice does not rely on it) |
| P3 / P2 peak power over P1 at the same drive | 1.53 / 1.27 | **measured** on Signum's transmit audio (peaks 5,440 / 4,950 / 4,400); that the radio keeps the ratio is **inferred** (linear below the ALC) |
| Margin for peaks between samples and meter error | 0.5 dB | **inferred** |
| RF POWER → limit watts | 100 × (setting/255)^1.24 | **inferred** from two **measured** points (2026-10-15, 20 m: 20 % → 13-14 W, 50 % → 41-49 W, the radio's own meter) |
| Limit above the wanted peak | 1.1 dB (× 1.3) | **inferred**: the ALC starts a little under the limit (11.5 W at a ~13.5 W limit read 0-0.1) |
| Po meter scale (15 11), for `--measure` | 0 / 143 / 213 = 0 / 50 / 100 W | Icom's CI-V reference; **not checked** against a wattmeter |

**Expected accuracy:** the drive → watts figure rests on two readings of one
radio's meter and on the PCM volume being 105 of 128 (Signum cannot read it
over CI-V); another radio, band, antenna/tuner or PCM volume can move it by a
dB or more. The model is built to err low: an OK answer never predicts a
peak above W. **`--measure` is the recommended way** when you can transmit.

**`--measure`** (transmits; Signum must not be running, it needs the radio's
sound card): after you type `YES` (or `--i-understand-this-overwrites-my-ic7300-settings`),
and only on a frequency inside an amateur band, in a DATA mode with DATA MOD
USB, it finds the drive on the air. Each step keys over CI-V (TX on,
`1C 00 01`) while a steady 1500 Hz tone at exactly Signum's P1 level for the
TX gain being tried plays, reads Po and ALC, and unkeys (TX off, read back)
after at most 1.5 s.

- **RF POWER is the final one from the first key-down on** (since 0.7.2). The
  IC-7300's output below its limit also scales with the RF POWER setting (3 W at
  0 dB at 18/255, about 70 W at 104/255 on radio 1), so a power read at a low
  setting says nothing about the final one: the 2026-11-20 `--measure` read 1.4-1.7 W at 18/255,
  took that for the drive's power, and jumped to a drive that gave 25 W at 69/255
  (the safety stop fired). Now RF POWER is written once, at the setting the answer
  keeps (1.1 dB above W by the limit curve), and only the drive moves.
- **The drive starts low**: the TX gain 8 dB under the model's hot guess at that
  RF POWER, and steps up. A reading under 0.7 W is too coarse (the Po meter's
  steps are 0.35 W): 6 dB more drive.
- **A reading clearly under the limit with the ALC at rest** (Po at most 0.75 ×
  the corrected limit) is the drive's own power. At a fixed RF POWER the chain is linear (1 dB
  more TX gain, 1 dB more power), so the next TX gain comes from it, rounded down
  to 0.5 dB, aiming under the P1 level whose P3 peaks (plus 0.5 dB) are W, at most 6 dB up per
  step; it stops stepping when another 0.5 dB would pass that level. Usually three or four key-downs.
  **A reading more than 1.5 dB away from the previous reading's linear prediction** is a sign of the wrong
  model: it is said, steps are then at most 2 dB, and a third such reading ends the run (nothing set).
- **The ALC meter cannot be trusted to show the limit.** One IC-7300 held Po at
  about 1.3 × the model's limit with the ALC meter at 4/255 on 80 m and 0/255 on
  40 and 20 m (**measured** 2026-11-20, the radio's own Po meter). So a reading
  near the limit (above 0.75 × it) may be the drive or the limit:
  - Po above the corrected limit corrects the limit curve whatever the ALC says
    (the radio's limit there is at least the reading);
  - the drive then comes down 3 dB at the same RF POWER. A drive-bound reading falls 3 dB and lands
    clearly under the limit; a limit-bound one falls less and the drive comes
    down again (within the step and time limits, not below TX gain -40 dB).
- **What cannot be brought down** (the ALC acting clearly under the limit, or
  still at the limit with the drive at -40 dB): it
  stops, puts RF POWER and the USB MOD Level back and leaves TX gain as it was.
- When it has a reading with the ALC at rest just under the aim, it keeps the RF POWER it
  measured at and saves the TX gain it found (old → new is printed).

Limits: 1.5 s per key-down, 12 s in all, 8 steps; a Po reading more than
0.5 dB above the highest steady power allowed (0.82 × W, +0.5 dB), an error, no meter answer, Ctrl-C or
SIGTERM: unkeyed at once, RF POWER and the USB MOD Level put back, TX gain
unchanged.

**If Signum dies while the radio is keyed.** A radio keyed by CI-V stays keyed
until it hears TX off. So before the first key-down `--measure` starts a small
**watchdog process** that holds the radio's serial port, and keys only after
the watchdog has acknowledged a deadline for that key-down:

| What happens | What unkeys the radio |
|---|---|
| Normal end, an error, Ctrl-C, SIGTERM | Signum itself (TX off, read back), within the 1.5 s per key-down |
| Signum killed (SIGKILL, the OOM killer) or crashes while keyed | the watchdog, at once (it sees Signum's end of their socket close; tested: under 0.2 s) |
| Signum hangs while keyed | the watchdog, 1 s after the key-down's 1.5 s (2.5 s); Signum then stops |
| The watchdog killed | Signum keys no more (the next key-down needs its acknowledgement); the one in progress ends within its 1.5 s |
| The computer loses power | nothing in software: see below |

The watchdog sends TX off three times and lowers RTS and DTR (a radio keyed
by USB SEND = RTS drops too; the port is opened with HUPCL, so the last close
lowers them anyway). It ignores Ctrl-C (which reaches every process in the
terminal) and stays on watch until Signum has finished. Signum's
`--max-key` watchdog does not apply here: `--ic7300-power` runs and exits
before the station starts, and does not key through the PTT. **Not covered:**
the computer losing power while keyed. What the IC-7300 does when its USB host
disappears during a CI-V key-down was not checked, and Signum does not use a
radio time-out timer (whether the IC-7300 has one that CI-V can set, and how
short, was not checked against Icom's reference); if yours has one in its
menus, a short setting is a good last layer. Otherwise press TRANSMIT or
switch the radio off.

The console and the GUI offer the model only: a running Signum holds the
sound card, which `--measure` needs.

**To check the model on the air** (dummy load, the set-up's values): run
`signum --ic7300-power 20 --measure` and compare the TX gain it finds with
the model's -7 (20 W: the hot-radio choice) and its measured P1 with the model's range of 2.4-10.1 W; then on a P3 link at the RF POWER it set, the ALC should stay at
0 through P3 data and Po on P1 calls should read the predicted P1 power. A
P1 outside the model's range (G 4.0-4.6 W at 51/255 to 70 W at 104/255, scaled by RF POWER) needs new data points.

### ALC warning

`--alc-warn on|off` (default **on**; console `ALCwarn 1|0`; settings-file key `alc-warn`; the GUI's
Configure tab, **ALC warning** card) tells you when the radio's ALC is squashing the PSK peaks of
a P2/P3 link. It only **warns**: it never changes the TX gain or the radio's power (lowering the
drive is your decision).

Why: on the air on 2026-11-20 (radio 1, 80 m, a 25 W limit), TX gain +4 squashed the P3 peaks; the RMS
answered P (repeat) to every break-in and the link stayed at SL1. At TX gain -2 the same RMS answered S
and the link ran at SL3. Nothing in Signum said so. See [Setting the power](#setting-the-power) for the
drive.

How: with the PTT `--ptt rig:HOST:PORT` (rigctld), a worker thread with a connection of its own to that
rigctld reads the ALC meter (`l ALC`, 0 to 1 on Hamlib's IC-7300) every 0.25 s while we are keyed on a P2 or
P3 link, starting 0.3 s after the key-down (the audio has not begun at the key-down). The audio loop never
waits for it. A key-down on a P1 link, or with no link, is not read. Each key-down is a **burst**; its value
is the highest ALC read in it, and a burst with fewer than two readings (an acknowledgment) is not counted.

The judgement (constants in `src/alc_warn.hpp`, not options):

| What | Value | How it was chosen |
|---|---|---|
| a burst is "over" | its highest ALC is 0.50 or more | **inferred**: on radio 1 P3 at TX gain -2 read ALC 0.20-0.44 ("still a little hot") and ran at SL3 with the RMS answering S, so readings up to 0.44 are tolerated by the far end; 0.50 is just above the highest reading of a link that worked. The squashed +4 case was not logged against the meter, so where it read is not known. Another radio may need other numbers |
| the warning | 3 of the last 6 counted bursts are over | one spike (a key click, a reading at the moment of keying) cannot warn; a drive that is wrong warns on the third burst |
| how often | once per link | then one summary at the link's end |

The warning (log line `[alc] WARNING: ...`, on the console and in the GUI's log) names the mode, the
level, the TX gain and the readings; the summary reads
`[alc] link summary: 9 bursts read, highest ALC 0.44, 0 at or above 0.50 (0 %)`. The GUI's Status tab has a
**Drive (ALC)** card (this link: State, Highest, Bursts over, Worst at); it is not on the front panel. The WebSocket
panel carries `alc` {`state` off / n/a / ok / warn, `max`, `bursts`, `over`, `limit`, `on_link`, `mode`, `level`,
`gain`}.

Not available (said once in the log, nothing more is done): the PTT is not `rig:` (no rigctld to ask), no
rigctld at the address, or the radio answers `RPRT -1`, `-4` or `-11` (or three errors in a row) to `l ALC`.

Cost [inferred]: rigctld serves one command at a time on the radio's CI-V line; a read takes tens of
milliseconds, so a key-down or key-up sent during one can be late by about that. If you see timing trouble,
`--alc-warn off`. The ALC meter of an IC-7300 does not always show the limit ([above](#setting-the-power));
a quiet ALC is not proof that the peaks are untouched.

### Receive level

Signum has no receive level meter: its waterfall scales its colours to the
noise floor, so the radio's audio level does not change what it shows. Set
the level on the radio with **ACC/USB AF Output Level** (the codec has no
capture control), and check it **before starting Signum** (the card serves
one program at a time):

```
arecord -D plughw:CARD=CODEC -f S16_LE -r 48000 -c 1 -V mono -d 20 /dev/null
```

The bar shows the peak level in percent of full scale.

- On a quiet frequency, band noise should show a steady few percent: the
  bench radio reads a noise RMS of about 1,900 (-25 dBFS) at 75 % output
  (bench).
- Strong signals should stay well below 100 %; with AGC on, the radio holds
  them to a steady level. If they reach 100 %, lower the output level.
- 50-95 % on the radio worked on the bench (radio 1 at 75 %, radio 2 at 95 %).

In Signum, the **Free/Busy** lamp turns red while someone transmits on the
frequency, and the waterfall shows the signal; a signal that reaches the
modem shows as two lines at 1400 / 1600 Hz (P1, calls) or a band about
2.2 kHz wide (P3).

### Tuning tone and the tuner

The IC-7300's internal tuner tunes on its own carrier when you press TUNE and
**remembers the result per frequency**: once tuned on a frequency, the radio
reuses it, so the tune cycle is needed only when you move to a new frequency
(or the antenna changes).

With `--ptt rig:` (rigctld) Signum can press TUNE for you before each new call:
set `--tune-tone 5` (the longest wait in seconds; `--tune-method auto` is the
default and picks the radio's own tune for a rig that lists `TUNE`, as Hamlib's
IC-7300 backend does). Signum then sends `G TUNE` through rigctld and waits in
silence; the call goes out when the radio is back in receive, or after the full
wait if the radio does not report the tune as PTT. Check on first use with
`--tune-method radio`, which logs why when it cannot.

**The tune cycle transmits at the radio's RF POWER setting**, the front-panel
level, not at Signum's data level (`PSKAmpl`, `--tx-gain`), and `--tune-db`
does not apply to it. The IC-7300 tunes with a carrier at that power, so set RF
POWER to what you mean to use (or lower, say 10 W) before you connect, into a
dummy load the first time. The radio has to be on a frequency its tuner covers
and the antenna must be connected.

An **external automatic tuner** that tunes on whatever RF it sees needs a steady
carrier, and the short P1 call bursts are not one: give it Signum's tuning tone
(`--tune-tone 3 --tune-method tone`, the GUI's Tuning tone card), which sends a
steady tone before each new call we place, at the power the call will run at
([Tuning tone](radio-setup.md#tuning-tone)). Lower it with `--tune-db` if the tuner
wants less power while tuning (`-10` is a tenth). Without `--tune-method tone`,
`auto` would use the radio's tune whenever the rig has one.

## The bench radios

Read on 2026-11-15 at about 22:00 UTC from both bench IC-7300s over their
USB CI-V ports, with queries only (no setting changed, nothing keyed, the
port opened with RTS and DTR low and left at B0), the radios at 50,640 kHz.
The menu items are read with CI-V command `1A 05` and a four-digit item number:

| CI-V | Item | Radio 1 | Radio 2 |
|---|---|---|---|
| `1A 05 0059` | ACC/USB Output Select | AF | AF |
| `1A 05 0060` | ACC/USB AF Output Level | 191/255 (75 %) | 242/255 (95 %) |
| `1A 05 0061` | ACC/USB AF SQL | OFF | OFF |
| `1A 05 0062` | ACC/USB AF Beep/Speech... Output | OFF | ON |
| `1A 05 0063` | ACC/USB IF Output Level | 128/255 (50 %) | 128/255 (50 %) |
| `1A 05 0064` | ACC MOD Level | 128/255 (50 %) | 128/255 (50 %) (written by the external-modem set-up only) |
| `1A 05 0065` | USB MOD Level | 255/255 (100 %) | 255/255 (100 %) |
| `1A 05 0066` | DATA OFF MOD | MIC,ACC | MIC |
| `1A 05 0067` | DATA MOD | USB | USB |
| `1A 05 0068`-`0070` | External Keypad VOICE / KEYER / RTTY | OFF | OFF |
| `1A 05 0071` | CI-V Transceive | ON | ON |
| `1A 05 0072` | CI-V USB→REMOTE Transceive Address | 00h | 00h |
| `1A 05 0073` | CI-V Output (for ANT) | OFF | OFF |
| `1A 05 0074` | CI-V USB Port | Unlink from [REMOTE] | Unlink from [REMOTE] |
| `1A 05 0075` | CI-V USB Echo Back | OFF | ON |
| `1A 05 0076` | USB Serial Function | CI-V | CI-V |
| `1A 05 0078` | USB SEND | RTS | RTS |
| `1A 05 0079` | USB Keying (CW) | OFF | DTR |
| `1A 05 0080` | USB Keying (RTTY) | OFF | OFF |
| `19 00` | CI-V address | 94h | 94h |
| `04`, `1A 06` | mode, DATA | USB, FIL1, DATA on (USB-D) | USB, FIL1, DATA on (USB-D) |
| `1A 03` | filter width | 3.0 kHz | 3.0 kHz |
| `16 12` | AGC | MID | MID |
| `16 02` | preamp | P.AMP2 | P.AMP2 |
| `16 22`, `16 40`, `16 41`, `16 48` | NB, NR, auto notch, manual notch | all OFF | all OFF |
| `16 44`, `16 46`, `16 45` | COMP, VOX, monitor | all OFF | all OFF |
| `16 58` | TBW (00 = WIDE) | WIDE | WIDE |
| `0F`, `21 01`, `21 02` | split, RIT, XIT | all OFF | all OFF |
| `14 01`, `14 02` | AF gain, RF gain | 1/255, 255/255 | 1/255, 255/255 |
| `14 0A` | RF POWER | 2/255 | 2/255 |
| `1C 01` | internal tuner | ON | ON |
| ALSA `amixer -c` | PCM Playback Volume / Switch | 105 of 128, on | 105 of 128, on |
| (as used) | CI-V USB baud rate | 115200 | 9600 |

The item numbers and their values are Icom's (the IC-7300 manual's "Control
command" chapter, `1A 05` table), and every item the set-up writes was
written and read back on radio 1 on 2026-11-16 (each changed to another value
and back; bench). One difference from the manual: it prints CI-V USB Echo
Back (0075) as 00 = ON, 01 = OFF, but both radios echo our commands at 01 and
not at 00 (bench: radio 2 at 01 echoed, radio 1 at 00 did not; radio 1 set
to 01 began to echo and stopped again at 00). CI-V USB Port (0074) is read
only over CI-V (manual). The CI-V Baud Rate, CI-V Address and CI-V USB Baud
Rate menu items have no `1A 05` number at all. Signum's `civ:` keying works
with echo back on or off.

## A working station's values

The project's own station uses an IC-7300 with its built-in sound card and USB
keying, and Signum works very well on it. Its values go here when they are
supplied:

| Setting | Value |
|---|---|
| USB MOD Level | (to be added) |
| ACC/USB AF Output Level | (to be added) |
| USB SEND / keying | (to be added) |
| PCM playback volume | (to be added) |
| Signum `--ptt`, PSKAmpl, TX gain | (to be added) |
| RF POWER, ALC seen on P3 | (to be added) |

## IC-7300 problems

| Symptom | Cause and fix |
|---|---|
| The radio transmits for an instant when a program starts or a cable is plugged in | USB SEND = RTS or DTR and something opened the serial port at a speed (Linux raises RTS/DTR at open). Keep other programs off the port; Signum's own opens do not do it ([The serial port](#the-serial-port)) |
| Signum: `cannot open plughw:CARD=CODEC` | the sound server or another program holds the card ([above](#keep-the-sound-server-off-the-radios-card)); `fuser -v /dev/snd/*` |
| Signum keys the radio but no power comes out, or the microphone's sound is transmitted | the mode is plain USB with DATA OFF MOD = MIC: switch DATA on (USB-D) and set DATA MOD = USB |
| No receive audio, or audio only on strong signals | ACC/USB AF SQL is ON, or ACC/USB Output Select is IF |
| P1 links fine, P2/P3 stay at low speed levels or fail | ALC compression: lower TX gain ([Transmit level](#transmit-level)); also check the filter is at least 2.4 kHz and NR/NB are off |
| The RMS answers P to every break-in; on P3 we send only at SL1 | the drive is pushing into RF POWER and the ALC (or the limit) flattens the PSK peaks; the ALC meter may barely move (**measured** 2026-11-20: TX gain +4 squashed the link; TX gain -2 with RF POWER 90/255 gave SL3). Lower the TX gain, or run `signum --ic7300-power W --measure` ([Setting the power](#setting-the-power)). With `--ptt rig:` Signum's [ALC warning](#alc-warning) logs `[alc] WARNING` when the ALC is acting |
| `WARNING: no CI-V answer from 94` | the baud rate in `civ:...:94:BAUD` differs from CI-V USB Baud Rate; USB Serial Function is not CI-V; or the CI-V address was changed |
| CI-V stops answering after hours (`cp210x ... urb stopped: -32` in `dmesg`) | the radio's USB serial chip stalls now and then (seen with the IC-7300 on the bench machine); closing and opening the port again brought it back there. Restart Signum; if that does not help, replug the USB cable |
