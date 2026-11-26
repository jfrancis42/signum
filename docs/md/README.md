# Signum user guide

Signum is a P1/P2/P3 HF data modem that runs on a Linux computer with a
sound card. This guide covers everything a station operator meets, from
installing it to working stations and finding faults.

| Page | What it covers |
|---|---|
| [Installing](install.md) | package lists for Debian, Ubuntu, Linux Mint, Fedora and Arch, building with or without encryption, `make install`, what goes where, real-time priority set-up |
| [Getting started](getting-started.md) | trying it with no radio (`signum-loopback`), `signum-start`, a first link on the air |
| [The GUI and the TUI](gui.md) | the browser GUI: front panel, every tab, card and setting; the terminal front panel |
| [Connecting programs](connections.md) | the host-mode serial port and Pat, running a gateway or peer-to-peer listener (`PDuplex 1`), telnet (port 3000), the WebSocket shell and API (port 8770), logins and security |
| [Radio set-up](radio-setup.md) | the sound card, PTT (RTS, DTR, CI-V, rigctld) and why not VOX, audio levels, tones, the tuning (the radio's own tune cycle or a tone) for an automatic tuner, timing calibration |
| [Icom IC-7300](radio-ic7300.md) | the IC-7300 with its built-in USB sound card and USB keying: the zero-effort set-up (Signum configures the radio itself, after a backup; command line or GUI), and the same for a hardware modem on the ACC socket instead (flip back and forth), the same by hand menu by menu, keying by RTS rather than CI-V, every menu setting, the Linux side, Signum's settings, setting the levels step by step |
| [Modes and speed levels](modes.md) | P1, P2 and P3 in plain terms, ARQ links, speed levels, Listen mode, unproto |
| [Console commands and host mode](commands.md) | every command Signum understands, converse-mode keys, host mode, differences from the reference firmware |
| [Hardware and performance](performance.md) | minimum hardware (measured), real-time priority, the CPU clock unit, the CPU health lamp, adaptive CPU features |
| [Running Signum](running.md) | foreground and background, the systemd service, stopping, exit status, files |
| [Command-line options](options.md) | every option of `signum`, `signum-start` and `signum-loopback` |
| [Encryption](encryption.md) | encryption between two Signums (off by default, illegal on the amateur bands): keys, pre-shared keys, fingerprints, what you see when it cannot be used |
| [Encryption keys](encryption-keys.md) | handling the keys: making, backing up, exchanging and verifying them (fingerprint and SAS), trust settings, a changed key, pre-shared keys and rotation, a compromised key, what is and is not protected; a quick recipe and a maximum-assurance recipe |
| [Troubleshooting and FAQ](troubleshooting.md) | common problems and what to do |

For developers: the WebSocket protocol in
[src/web/README.md](../../src/web/README.md) and the PDF
toolchain in [pdf/README.md](../pdf/README.md).

## Quick reference

| What | Default |
|---|---|
| GUI | http://127.0.0.1:8770/ |
| JSON WebSocket API | `ws://127.0.0.1:8770/` |
| WebSocket command shell | `ws://127.0.0.1:8770/shell` |
| telnet (command interpreter) | `127.0.0.1:3000` |
| host-mode serial port (for Pat) | `/tmp/ttysignum0` |
| calibration file | `~/.config/signum/calibration.conf` (optional) |
| log under `signum-start` | `~/.local/state/signum/tnc.log` |
| key-down watchdog | 90 s |
| tones | 1400 / 1600 Hz |
| highest mode | P3 |
