# Running Signum

## In a terminal

- `signum-start [options]`: Signum with the TUI; quitting the TUI stops
  Signum. Its log: `~/.local/state/signum/tnc.log`.
- `signum-start --no-tui [options]` or `signum [options]`: Signum in the
  foreground, its log on the terminal. Ctrl-C stops it.
- `--quiet`: no modem log on standard output.

Signum needs `--alsa DEV` (a sound card) to start; for a station that only
listens use `--ptt none`, the default.

## In the background

**A station that can key the radio stops when the program that started it
exits.** This is on purpose: no orphaned Signum is left holding a radio. It
applies to every `--ptt` but `none`. A line like

```
nohup setsid signum ... &         # the last line of a script that then exits
```

therefore stops Signum at once, and its log says
`[signum] SIGTERM (the parent process exited ...): unkeyed, exiting`.

To run it in the background, either keep a parent that stays:

```
nohup setsid bash -c 'signum --call N0CALL --alsa plughw:CARD=CODEC --ptt rts:/dev/ttyUSB0; echo "signum exited $?"' > signum.log 2>&1 &
```

(stopping that shell stops Signum), or run it as a systemd service (below).
`signum-loopback` stations (`--ptt none`) are not affected.

## As a systemd service

An example unit, `signum.service`, is installed to
`/usr/local/share/signum/examples/` (in the source tree: `dist/`). It is not
enabled. It runs Signum as the user `signum` with the groups `audio` and
`dialout`, the GUI on `127.0.0.1:8770`, and restarts it after a failure.

1. Create the user (once):

   ```
   sudo useradd --system --create-home --groups audio,dialout signum
   ```

   Signum keeps its self-calibration in that user's
   `~/.config/signum/`, so the user needs a home directory.

2. Copy and edit the unit:

   ```
   sudo cp /usr/local/share/signum/examples/signum.service /etc/systemd/system/
   sudo nano /etc/systemd/system/signum.service
   ```

   Change `ExecStart=` (or put the options in the `signum` user's
   `~/.config/signum/signum.conf`, which the command line overrides) to your call sign, sound card and PTT (the example has
   `--ptt none`, so it never keys), e.g.
   `ExecStart=/usr/local/bin/signum --call N0CALL --alsa plughw:CARD=CODEC --ptt rts:/dev/serial/by-id/... --ws 127.0.0.1:8770 --quiet`.

3. Real-time priority: the installed binary needs its capabilities
   (`sudo setcap cap_sys_nice,cap_ipc_lock+ep /usr/local/bin/signum`; `make
   install` sets them when it can). Instead, you can add `LimitRTPRIO=95` and
   `LimitMEMLOCK=infinity` under `[Service]`.

4. The CPU clock: install `signum-cpufreq.service` with the group `signum`
   ([CPU clock](performance.md#installing-the-cpu-clock-unit-signum-cpufreqservice)).
   It is ordered to run before `signum.service`.

5. Enable and start:

   ```
   sudo systemctl daemon-reload
   sudo systemctl enable --now signum
   journalctl -u signum -f          # the log (drop --quiet to see the modem's lines)
   ```

The GUI is then at http://127.0.0.1:8770/ on that computer (an SSH tunnel
from elsewhere), the pty at `/tmp/ttysignum0`. Note that systemd may give
the service a private `/tmp` if you add `PrivateTmp=yes`; then use
`--pty` with a path other programs can reach.

## Signum, rigctld and Pat as services

A station where Pat changes the radio's frequency runs three services, in
this order: `rigctld` (owns the radio's serial port and keys it), Signum
(keys through `rigctld`, [Pat changing the
frequency](connections.md#pat-changing-the-frequency-qsy)) and Pat. Examples
are installed to `/usr/local/share/signum/examples/`: `rigctld-signum.service`
and `pat-signum.service`, besides `signum.service`.

1. `rigctld-signum.service`: set the radio's port, Hamlib model, CI-V
   address and baud rate in `ExecStart=`. It listens on `127.0.0.1` only.
2. `signum.service`: as above, with `--ptt rig:127.0.0.1:4532`, and
   `After=`/`Wants=rigctld-signum.service` under `[Unit]`.
3. `pat-signum.service`: runs `pat --config FILE http` as your user, after
   Signum. Pat's web page listens where Pat's `"http_addr"` says
   (`0.0.0.0:8080` for every interface).

```
sudo cp /usr/local/share/signum/examples/{rigctld-signum,pat-signum}.service /etc/systemd/system/
sudo nano /etc/systemd/system/rigctld-signum.service /etc/systemd/system/pat-signum.service
sudo systemctl daemon-reload
sudo systemctl enable --now rigctld-signum signum pat-signum
```

**Reaching it from other computers on the LAN:** give Signum `--ws
0.0.0.0:8770` (GUI and WebSocket API) and `--telnet-bind 0.0.0.0` (the
command interpreter on port 3000), and Pat `"http_addr": "0.0.0.0:8080"`.
Anyone who can reach those ports can key your transmitter: on a network you
do not fully trust, set logins (`--web-auth`, `--ws-auth`, `--telnet-auth`,
[Logins and security](connections.md#logins-and-security)). Keep `rigctld` on
`127.0.0.1`.

Use your own `pat@.service` instead only if nothing else is attached to it:
Pat's packaged template runs `pat http` with Pat's default `config.json`.

## Stopping

Ctrl-C, `kill PID`, `systemctl stop signum`, or quitting the TUI under
`signum-start`. Signum unkeys at once and logs
`[signum] SIGTERM: unkeyed, exiting` (or SIGINT, SIGHUP). On the way out it
puts the CPU clock back.

### Exit status

| Status | Meaning |
|---|---|
| 0 | stopped by a signal (SIGTERM, SIGINT, SIGHUP) |
| 1 | an error at start-up (a port in use, a sound card, PTT or pty that cannot be opened), or a stop that took more than 3 s |
| 2 | a bad command-line option |
| 3 | the sound card was lost while running |

## Files

| Path | What |
|---|---|
| `/tmp/ttysignum0` | the host-mode serial port (`--pty`) |
| `~/.config/signum/signum.conf` | the settings file: options read at start, written by the GUI's Configure tab ([Command-line options](options.md#the-settings-file)) |
| `~/.config/signum/calibration.conf` | your timing calibration, if any (`--cal`) |
| `~/.config/signum/autocal-DEV.conf` | what self-calibration measured, one per sound card (`--autocal-state`) |
| `~/.local/state/signum/tnc.log` | the log while `signum-start` runs the TUI |
| `~/.local/state/signum/loopback-A.log`, `-B.log` | `signum-loopback`'s logs |
| `~/.local/state/signum/loopback/A/`, `.../B/` | each `signum-loopback` station's own settings file (`signum.conf`) and encryption keys (`keys/`) |

`$XDG_CONFIG_HOME` and `$XDG_STATE_HOME` replace `~/.config` and
`~/.local/state` when set. Station settings changed with commands are not
saved: each start begins from the defaults, the settings file and the command
line. The GUI's **Keep live settings at next start** writes the live ones that
have a start-up option into the settings file.
