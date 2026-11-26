# Command-line options

`signum --help` prints the same list; `man signum` has it too.
`signum-start` passes every option except its own (`--no-tui`, `--bin PATH`)
to `signum`. Every option here except the test ones can also be set on the
GUI's Configure tab ([The GUI](gui.md#configure-tab)), which keeps it in the
[settings file](#the-settings-file).

## Station

| Option | Default | What it does |
|---|---|---|
| `--call CALL` | `N0CALL` | your call sign (MYcall) |
| `--mylevel 1\|2\|3` | 3 | the highest mode: P1, P2 or P3 (MYLevel) |
| `--tones LO,HI` | `1400,1600` | the FSK tone pair (TONES 4) |
| `--listen [on\|off]` | on | Listen mode at start (Listen): other stations' links decoded and shown while idle, as on the reference unit; `off` saves CPU ([Modes](modes.md#listen-mode-monitoring)); a bare `--listen` is `on` |
| `--p3-unproto-rx` | off | Listen mode also prints P3 unproto (U 30-41); the reference unit cannot |
| `--peer-marker on\|off` | on | the Signum peer marker (PEERmark): a short system sequence on each P2/P3 link that the reference unit skips silently, so that two Signums recognise each other (the front panel's Peer lamps); never sent on P1; `off` sends nothing extra. On by default by the owner's decision: the reference unit has no such marker ([The peer marker](connections.md#the-peer-marker-is-the-other-station-a-signum)) |
| `--alc-warn on\|off` | on | warn when the radio's ALC acts while we send P2/P3 (ALCwarn): reads the ALC meter through rigctld (`--ptt rig:`, Hamlib `l ALC`) while keyed on a P2/P3 link and logs `[alc]` once per link when the ALC squashes the PSK peaks, with a summary at the link's end. Warns only: never changes TX gain or power. `off` reads nothing ([ALC warning](radio-ic7300.md#alc-warning)) |
| `--cpu-adaptive on\|off` | on | adaptive CPU features (CPUadapt) ([details](performance.md#adaptive-cpu-features-cpuadapt)) |
| `--pduplex on\|off` | off | automatic changeover in every mode (PDuplex): off breaks in only on the sender's changeover, Ctrl-Y or a disconnect and hands over only on Ctrl-Y, as the reference unit; on also breaks in whenever text is waiting and hands over whenever our text has run out ([Modes](modes.md#how-a-link-works-arq)) |
| `--pdtimer S` | 12 | with `--pduplex on`: seconds after the other station broke in before ours may (PDTimer, 2-30) |
| `--tune-tone S` | 0 (off) | tune for up to S seconds (1-10) before each new call we place, for an automatic antenna tuner: the radio's own tuner cycle through rigctld, or a steady tone (TUnetone) ([Tuning tone](radio-setup.md#tuning-tone)) |
| `--tune-method auto\|tone\|radio` | auto | how: `auto` = the radio's own tune (`G TUNE` through rigctld, `--ptt rig:`, the rig must list the TUNE VFO op), else the tone; `tone` = always the tone; `radio` = the radio's tune, the tone with the reason in the log when impossible. The radio's tune transmits at the radio's RF POWER, not the data level ([The radio's own tune cycle](radio-setup.md#the-radios-own-tune-cycle---tune-method)) |
| `--tune-hz HZ` | 1500 | the tuning tone's frequency, 300-2700 Hz (not used by the radio's tune) |
| `--tune-db DB` | 0 | the tuning tone's level against the data level, -30 to 0 dB (0 = the power of the P1 call packets) |

## Audio and radio

| Option | Default | What it does |
|---|---|---|
| `--alsa DEV` | (required) | the sound card, e.g. `plughw:CARD=CODEC` ([Radio set-up](radio-setup.md#sound-card---alsa-dev)) |
| `--ptt SPEC` | `none` | `rts:TTY`, `dtr:TTY`, `civ:TTY:ADDR[:BAUD]`, `rig[:HOST[:PORT]]`, `vox` (do not use), `none` ([PTT](radio-setup.md#ptt)) |
| `--rts TTY` | | the same as `--ptt rts:TTY` |
| `--max-key S` | 90 | key-down watchdog: the key is dropped after S seconds of continuous transmitting (the GUI takes 1-600 and changes it while running) |
| `--tx-gain DB` | 0 | output level in dB on top of PSKAmpl ([Audio levels](radio-setup.md#audio-levels)); the GUI takes -40 to 20 and changes it while running (from the next link when one is up) |
| `--cpu-clock auto\|max\|off` | auto | hold the CPU clock at its maximum while a link is up / always / never ([CPU clock](performance.md#cpu-clock)) |

## Icom IC-7300 set-up

One action each; Signum does it and exits ([Zero-effort set-up](radio-ic7300.md#zero-effort-set-up)).
None of them changes the radio's frequency; only `--ic7300-power` changes its transmit power (RF POWER, and the drive: the USB MOD Level and Signum's TX gain).

| Option | What it does |
|---|---|
| `--detect-ic7300` | find every IC-7300 (USB serial port, sound card, CI-V rate and address); print its settings and what the set-up would change. Reads only |
| `--backup-ic7300-settings` | save the radio's settings to `~/.config/signum/ic7300-backup-SERIAL-TIME.conf`. Changes nothing |
| `--configure-my-ic7300-and-overwrite-its-settings` | reconfigure the radio for Signum over CI-V, each setting read back, after a backup and your typed `YES`; then write `alsa`, `ptt`, `tx-gain`, `prime-ms`, `no-autocal` into the settings file |
| `--configure-my-ic7300-for-an-external-modem-and-overwrite-its-settings` | the same, for a hardware modem on the radio's ACC socket instead of Signum on USB: DATA MOD = ACC, USB SEND and USB Keying OFF so nothing on USB can key the radio, ACC MOD Level 50 %, the rest as for Signum (so either command brings the radio back from the other). Does not touch the settings file; Signum cannot key the radio until the Signum set-up is run again ([details](radio-ic7300.md#using-the-radio-with-an-external-modem-instead)) |
| `--restore-ic7300-settings FILE` | write a backup file back to its radio, after your typed `YES` (a backup of `--ic7300-power` puts RF POWER back too; the USB MOD Level is in every backup) |
| `--ic7300-power W` | set the **drive** and **RF POWER** so that our P2/P3 PSK peaks reach W watts (1-100) with the ALC at rest: the TX gain the model names for W, chosen so the ALC stays at rest even on a hot radio (0.5 dB steps, never a peak above W; -7 dB for 20 W, -7.5 dB for 25 W); USB MOD Level written back to 100 % (the set-up's) if it differs; RF POWER a limit 1.1 dB above W. The TX gain is saved in the settings file (`tx-gain`; old and new printed; `--no-config`: printed only). Backup first (it restores RF POWER and the USB MOD Level); never transmits ([Setting the power](radio-ic7300.md#setting-the-power)) |
| `--measure` | with `--ic7300-power`: **transmits** short steady tones (1.5 s each, 12 s in all; RF POWER is the final one from the first tone and only the TX gain steps up from low, the tone never above about 0.82 × W) to find the TX gain that brings P2/P3 peaks to W with the ALC at rest, then sets RF POWER and saves that TX gain. A separate watchdog process unkeys the radio if Signum dies or hangs while keyed. Asks you to type `YES`; command line only: Signum must not be running (it needs the sound card, which a running Signum and so its console and GUI hold) |
| `--ic7300-serial N` | which radio, when more than one is plugged in |
| `--i-understand-this-overwrites-my-ic7300-settings` | for scripts: confirm without typing `YES` |

## Timing calibration

| Option | Default | What it does |
|---|---|---|
| `--cal FILE` | `~/.config/signum/calibration.conf` | the calibration file |
| `--no-autocal` | | no self-calibration |
| `--autocal-state FILE` | `~/.config/signum/autocal-DEV.conf` | where self-calibration keeps what it measured |
| `--autocal-max-ms N` | 45 | hardware round-trip readings above N ms are refused (the environment variable `AUTOCAL_HW_MAX_MS=N` does the same; the option wins) |
| `--audio-ppm E` | | the sound card's rate error in ppm (else `AUDIO_PPM` in the environment, else the file, else self-calibration) |
| `--rt-latency S` | | a direct timing override instead of the file's (advanced) |
| `--prime-ms MS` | 40 | the playback buffer ahead of the sound card (advanced) |

See [Timing calibration](radio-setup.md#timing-calibration).

## Connections

| Option | Default | What it does |
|---|---|---|
| `--pty PATH` | `/tmp/ttysignum0` | the host-mode serial port; `none` for none. A link left by an earlier run is replaced; any other existing file at PATH is not (Signum stops with a message) |
| `--telnet PORT` | 3000 | the command interpreter on TCP; `0` = off |
| `--telnet-bind ADDR` | `127.0.0.1` | its address; `0.0.0.0` for every interface |
| `--telnet-auth PASS` | open | a password prompt before the interpreter attaches |
| `--ws [ADDR:]PORT` | `127.0.0.1:8770` | the GUI, the WebSocket API and `/shell` |
| `--no-ws` | | none of them |
| `--web-auth USER:PASS` | open | a login for the GUI pages |
| `--ws-auth USER:PASS` | open | a login for the WebSocket API and `/shell` |
| `--web-auth-file F`, `--ws-auth-file F` | | the same, read from the first line of F that is not a `#` comment |
| `--gui DIR` | the installed or nearby `www/` | the directory served over HTTP |

See [Connecting programs](connections.md).

## Encryption

Between two Signums only, off by default, **illegal on the amateur bands**; only in a build made
with `./configure --enable-encryption` (not the default). See [Encryption](encryption.md).

| Option | Default | What it does |
|---|---|---|
| `--encryption off\|verify\|on` | off | verify: check the other Signum's identity at link start (the same keys and handshake as encryption), then send the data in clear; no acknowledgement; a link where the check cannot be made is ended ([Verify](encryption.md#verify-identity-check-without-encryption)). on: encrypt every link; a link that cannot be encrypted is ended, never sent in clear. Needs `--encryption-ack` the first time (it is then stored) |
| `--encryption-ack NOT-AMATEUR` | | the acknowledgement that this station is not on the amateur bands; without it `--encryption on` refuses to start. Given once, it is stored in the settings file (`encryption-ack`) and not needed again; `none` withdraws it, removes `encryption` too and starts with encryption off (nothing is stored with `--no-config`) |
| `--encryption-trust stored\|tofu\|verified` | tofu | encryption's first-contact policy: tofu: a new station's key is stored unverified at first contact; stored: only a key already stored here; verified: only keys you verified |
| `--verify-trust stored\|tofu\|verified` | stored | the same for verify |
| `--sign on\|off` | off | verify: a 16-byte authentication tag on every record (data still in clear; a bad tag ends the link); off: the identity is checked at link start only and a takeover after it is not detected. No extra effect with encryption on |
| `--encryption-wait S` | 60 | the capability record and the handshake, each at most S s (10-600) |
| `--encryption-compression on\|off` | on | compress before encrypting (zstd with a dictionary), used when both Signums have it on |
| `--key-dir DIR` | `~/.config/signum/keys` | the keys (environment `SIGNUM_KEY_DIR` instead of the default: the tests use it so that they never touch yours). One Signum per directory: a second one on the same directory refuses to start with `--encryption on` or `verify`, and otherwise runs with encryption unavailable ([Encryption](encryption.md#files)) |
| `--keygen` | | make this station's key pair (if none), print the public key, exit |
| `--show-key` | | print the public key and its fingerprint, exit |

## Output

| Option | What it does |
|---|---|
| `--quiet` | no modem log on standard output |
| `--version` | version and licence |
| `--help` | the option list |

## The settings file

| Option | Default | What it does |
|---|---|---|
| `--config FILE` | `~/.config/signum/signum.conf` | the settings file (`SIGNUM_CONFIG=FILE` in the environment does the same) |
| `--no-config` | | no settings file (`SIGNUM_CONFIG=none` the same) |

Signum reads the settings file at start, **before** the command line, so an
option on the command line wins over the same option in the file. A missing
file is no error. One option per line, as `option = value`, the long option
without its `--`; flags (`p3-unproto-rx`, `no-autocal`, `no-ws`, `quiet`)
and on/off options (`listen`, `peer-marker`, `cpu-adaptive`, `pduplex`) take `on` or `off`. `#` starts a comment line. An unknown option is
reported at start and skipped.

```
# ~/.config/signum/signum.conf
call = N0CALL
alsa = plughw:CARD=CODEC
ptt = rts:/dev/ttyUSB0
listen = off
tune-tone = 3
```

Every option of the tables above except `--rts` (use `ptt = rts:TTY`), the
`--*-auth-file` forms (use `web-auth` / `ws-auth`), the one-shot `--keygen` and
`--show-key`, the test options and `--config` itself can go in the file
(`encryption = on` needs `encryption-ack = NOT-AMATEUR`, which Signum writes the first time it is given). The start log names the file and the
options taken from it (`[config] settings file ...`; logins shown as `***`).

The GUI's Configure tab writes this file: a setting marked "applies at next
start" is saved there as you change it (emptied, its line is removed and the
default applies again), and **Keep live settings at next start** saves the
current live settings that have a start-up option (call sign, highest level,
tones, Listen, CPU features, auto break-in and its hold, tuning tone, TX gain, watchdog). Other lines and
comments are kept. The file is written with mode 0600, since it may hold
logins. Anyone who can use the GUI or the WebSocket API can change it; use
logins (`--web-auth`, `--ws-auth`) when the GUI listens beyond this computer.

A flag set in the file cannot be turned off on the command line; start with
`--no-config` (or another `--config FILE`) to ignore the file.
The test suite never reads it, and `signum-loopback` gives each of its stations a settings file of its own (below).

## For tests and development

| Option | What it does |
|---|---|
| `--audio-in F --audio-out F` | instead of a sound card, a lockstep audio link through files or FIFOs (two instances talk to each other; used by the test suite) |
| `--pace` | the FIFO link at real time (else as fast as the peer) |
| `--duration S` | FIFO link: stop after S seconds of audio |
| `--rx-noise RMS` | FIFO link: noise added to what we hear |

## `signum-start`

```
signum-start [--no-tui] [--bin PATH] [signum options...]
```

| Option | What it does |
|---|---|
| `--no-tui` | Signum alone in the foreground, its log on the terminal |
| `--bin PATH` | the `signum` program to run (default: the build tree's, else the installed one) |

## `signum-loopback`

```
signum-loopback [start] [--bin PATH] [--calls A B] [-- signum options...]
signum-loopback stop
signum-loopback status
signum-loopback reset
```

Each station keeps its own settings file and encryption keys in
`~/.local/state/signum/loopback/A/` and `.../B/` (`signum.conf` and `keys/`), never your
`~/.config/signum`. `reset` deletes both stations' files (stop them first).

See [Getting started](getting-started.md#1-try-it-without-a-radio-signum-loopback).
