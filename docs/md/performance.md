# Hardware and performance

P1, P2 and P3 are strict about time: every received packet must be decoded,
and our answer sent, within a fixed gap of the other station's packet. If the
computer is late, the packet counts as lost and is sent again, and the link
slows down. This page covers what computer you need and the three things
that keep Signum on time: real-time priority, the CPU clock and the CPU
health indicator.

The audio path's own delay (sound card buffers, a sound server, the radio) counts against the same
gap; how much of it Signum tolerates, and how to read yours from the log, is in
[How long an audio path Signum tolerates](radio-setup.md#how-long-an-audio-path-signum-tolerates).

## Minimum hardware

**The Raspberry Pi 4 is the slowest supported computer.** A Pi 3 or an
ESP32 is not supported.

<!-- Measured results: update this table when new measurements come in
     (e.g. a faster long P3 decode on the Pi 4). Keep "measured" and
     "not measured" apart. -->

| Computer | P1 | P2 | P3 | Measured how |
|---|---|---|---|---|
| **Raspberry Pi 4 Model B** (arm64, Debian 13, real-time priority, CPU clock held) | works | works | as **sender**: every level, long packets up to SL6. As **receiver**: every level, short and long packets up to SL6 (a long SL6 decode is done about 15 ms after the packet's last pulse, about 25 ms with a second station on the same Pi, against an answer due about 50 ms after it) | on the bench against a reference unit: P3 12,000 characters exact both ways, P2 (it sending) 12,000 exact, P1 (we sending) 9,525 exact in the run's time (all with version 0.4.0, when long packets as receiver stopped at SL4). Long SL6 as receiver: measured between two Signums on a second Pi 4 (20,000 characters exact in 40 s, long SL6 throughout), not yet against the reference unit; Signum's test suite passes there |
| **x86-64 laptop** (Intel Core i7-8550U, 4 cores / 8 threads) | all | all | all, short and long packets, SL6 | Signum's full test suite passes |
| Raspberry Pi 5, other x86 | not measured | | | expected to have more headroom than a Pi 4 |

Measured 2026-11-15 (after version 0.4.0). On the Pi 4 the whole test suite passes. A computer slower
than that is handled by the adaptive CPU features (below): if its long decodes come late, Signum asks for
short packets above the level it can keep up with, and the link still reaches SL6 with short packets.
Two stations on ONE core each (a test, not a real set-up) still fall behind on a Pi 4 in P3. P2 keeps
up even then: two Signums on one Pi 4, each held to one core of its own, or both to the same core,
carried 3,000 characters exactly in 52.5 s (measured 2026-11-16).
As P2 sender on one core, every long packet's audio is ready at its start, also with the CPU clock left to
the governor (measured 2026-11-16 on the Pi 4, and at 600 MHz).

**Memory:** Signum itself peaks at about 17 MB resident during a P3 link
with the GUI on (measured on x86; expect 15-30 MB on ARM). A headless system
with Signum and Pat fits in about 150-250 MB, so any Pi 4 is comfortable,
including the 1 GB model. Building needs more: see [Installing](install.md).

**Recommendations:**

- Run headless (no desktop) on a small computer: it leaves the most CPU for
  the modem. Use the GUI from another computer's browser.
- Keep other heavy programs off the computer during links.
- Listen mode (on by default, as on the reference unit) runs a second decoder while idle, at
  normal priority below the modem's real-time work, and pauses during links. On a Raspberry Pi 4
  (2026-11-16, one idle Signum with real-time priority, 5 minutes per case, the CPU at ~0.85-1.5 GHz
  under the `ondemand` governor; figures in % of one core, scaled to 1.5 GHz): hearing noise, the
  station alone ~8.5 %, with Listen on ~28 % (the monitor ~20 %); hearing a recorded P3 link in
  noise, ~9-12 % against ~56-60 % (the monitor ~47 %); digital silence (a muted sound card) cost the
  monitor as much as a link until fix 272 (now no more than noise). `Listen 0` (`--listen off`) saves that, for a
  quieter, cooler computer; it changes nothing on the air.
- Set up real-time priority and the CPU clock unit (below).
- A Pi 4 needs a good power supply and cooling: the GUI shows the CPU
  temperature, amber from 70 °C, red from 80 °C.

## Real-time priority

With real-time priority Linux runs Signum's audio loop (and its decoders)
before ordinary programs, so a busy desktop or a background job cannot delay
an answer. Signum asks for it at start (SCHED_FIFO, and locked memory). It
works without it, but on a radio links are less reliable.

**Turning it on (one of):**

1. **File capabilities** (what `make` does): the program gets
   `cap_sys_nice,cap_ipc_lock+ep`.

   ```
   make rt                                              # the build tree's signum
   sudo setcap cap_sys_nice,cap_ipc_lock+ep /usr/local/bin/signum   # an installed one
   ```

   `make` and `make install` do this by themselves as root or with
   password-less `sudo`. Every rebuild or reinstall clears the capabilities,
   so set them again after one (`make` does when it can). They do not work
   on a filesystem mounted `nosuid` (some home directories and USB disks).

2. **A limit for your user**, without capabilities: create
   `/etc/security/limits.d/signum.conf` with

   ```
   youruser   -   rtprio    95
   youruser   -   memlock   unlimited
   ```

   (or `@audio` for everyone in the `audio` group), then **log out and in
   again** (or reboot): limits apply only to new log-ins. Signum's audio loop
   uses priority 80 and its decoders 20, so the rtprio limit should be at
   least 80. A lower limit still gives real-time priority, at the limit
   (decoders one below it), and the log says
   `[rt] real-time priority 40, not 80: the rtprio limit ...`. Without
   `memlock unlimited` Signum does not lock memory it allocates later (a
   small memlock limit would make that memory fail), and the log says so.
   For a systemd service, set `LimitRTPRIO=95` and `LimitMEMLOCK=infinity`
   in the unit instead.

**Checking it:**

- `getcap $(command -v signum)` (or `getcap build/signum`) shows
  `cap_ipc_lock,cap_sys_nice=ep`.
- Signum's log does **not** contain
  `[rt] no real-time priority (run with CAP_SYS_NICE / rtprio for the best timing)`.
- The GUI's Status tab, **Computer** card, does not list "no real-time
  priority" under **Here**.
- `chrt -p $(pidof signum)` shows `SCHED_FIFO` and priority 80.

Not needed on the loopback (`signum-loopback`), where there is no radio's
turnaround to meet.

## CPU clock

On a Raspberry Pi (and many other computers) the CPU clock drops when the
processor is idle, a Pi 4 down to 600 MHz, and takes time to come back up.
Between packets Signum is idle for a moment, so the clock drops just before
the next decode, and P3 decodes then come late at every level. On the air
this made links fall back to the lowest P3 level and time out, while the same
links with the clock held up were flawless.

So, with a sound card (`--alsa`), **while a link is up** (calling, setting up
or linked) Signum raises each CPU's minimum clock to its maximum, and puts
back the value it found in standby and whenever it stops. Nothing else is
changed.

| `--cpu-clock` | Effect |
|---|---|
| `auto` (default) | the clock at its maximum while a link is up |
| `max` | the clock at its maximum for as long as Signum runs |
| `off` | the clock left to the system |

**It needs permission.** Writing the minimum clock
(`/sys/devices/system/cpu/cpufreq/policy*/scaling_min_freq`) needs root, or a
file your group may write. Without it Signum logs once

```
[cpu] cannot hold the CPU clock up (... is not writable): ...
```

and leaves the clock alone. With it, the log says
`[cpu] CPU clock held at its maximum while a link is up (...)`.

### Installing the CPU clock unit `signum-cpufreq.service`

The example unit makes the minimum-clock files writable for one group at
every boot. It is installed to `/usr/local/share/signum/examples/` (in the
source tree: `dist/`), not enabled.

1. Pick the group Signum runs as. For yourself, your own user group (the
   output of `id -gn`); for the example service, `signum`.
2. Copy the unit and put that group in (the unit uses `signum`):

   ```
   sudo cp /usr/local/share/signum/examples/signum-cpufreq.service /etc/systemd/system/
   sudo sed -i 's/chgrp signum/chgrp YOURGROUP/' /etc/systemd/system/signum-cpufreq.service
   ```

3. Enable and start it:

   ```
   sudo systemctl daemon-reload
   sudo systemctl enable --now signum-cpufreq
   ```

4. Check: `ls -l /sys/devices/system/cpu/cpufreq/policy*/scaling_min_freq`
   shows the group with write permission, and Signum's next start logs
   `[cpu] CPU clock held at its maximum ...`.

Alternatives: set the CPU governor to `performance` yourself (the clock then
stays high all the time), or run with `--cpu-clock off` and accept the risk.

If Signum is killed with `kill -9` it cannot put the clock back: the
minimum stays at the maximum until you write it back or restart the
computer. On a computer without cpufreq (most x86 virtual machines) there is
nothing to do.

## CPU health: OK, BUSY, STARVED

One indicator tells you whether this computer keeps up with the audio and
the decodes, so you can stop other programs or move to a faster computer.
With `CPUadapt` on (the default) Signum also leaves some CPU-bound work out
while it is not OK ([below](#adaptive-cpu-features-cpuadapt)); otherwise it
changes nothing.

| State | Lamp | Meaning |
|---|---|---|
| **OK** | green | nothing late |
| **BUSY** | yellow | some things came late in the last minute: links may slow down |
| **STARVED** | red, blinking | many things came late: links fall back to low speed levels or drop |

**What counts as late:** playback underruns and capture stalls of the audio
loop, decodes not done in time or done with less than 8 ms to spare,
"computer back-off" steps (Signum lowering the P3 level because its decodes
are late), packets rescued from the SL1 decode, and transmit audio not ready
in time (by 15 ms or more). Each counts for about a minute, fading. One
isolated event is never an alarm, the first 20 s after start are ignored, and
the state does not flap.

**Where you see it:**

- the **Computer** lamp in the GUI's title bar (hover for the reason) and the
  Status tab's **Computer** card (State, Why, Here, What to do);
- `CPU OK|BUSY|STARVED` on the TUI's top line, with the reason and the advice
  below while it is not OK;
- the console command `CPULoad`
  (`*** CPU LOAD: STARVED  (3 playback underruns ...)  APPLIES HERE: ...`);
- one log line on every change, for example
  `[cpu] STARVED: 4 playback underruns (...), 2 late decodes in the last minute. ... What to do: ...`,
  and `[cpu] OK again ...` when it has been quiet.

**What to do when it is not OK:** Signum names the causes that apply on
this computer, from:

1. **Other programs** using the CPU ("other load (load average ... )"):
   find them with `top` and stop them.
2. **No real-time priority:** set it up ([above](#real-time-priority)).
3. **A CPU clock that cannot be held up** (or `--cpu-clock off`: Signum then leaves it to the governor and the
   health text says so): install
   [the CPU clock unit](#installing-the-cpu-clock-unit-signum-cpufreqservice)
   or set the governor to `performance`.
4. When none of these applies, the computer itself is too slow for what it
   is asked: use a faster one, or set `CPUadapt` (below) and Listen mode off.

It is measured only with a sound card (`--alsa`).

## Decoding and the back-off

As P3 receiver Signum must finish each packet's decode before its answer is due. When that fails because the
computer is too slow, Signum **backs off**: it caps the receive level it asks for (and the long-packet level, below)
so the decodes get shorter, holds the cap for at least 60 s (then 120, 240, 480 s for further caps of the same
link), and lifts it after it has kept up. The log says `P3: computer back-off: ..., not above SLn`.

A single late decode is not evidence of a slow computer: a scheduling stall (another process, a CPU clock change,
the audio loop woken late) delays one decode while the work itself took a few milliseconds, and a cap for that
makes the sender, which does not follow our level requests the way we hope, send packets we no longer try to decode.
So the back-off waits for **two late or thin decodes (less than 8 ms to spare) within 10 s**, about eight packet
cycles. A computer that is too slow is late on every packet and backs off after the second, within a few seconds.
A late decode within 2 s after an audio stall or a playback underrun (the audio loop lost time, from outside the
decode) is never counted. The first is logged,
`P3: late decode treated as a hiccup (...)`, and changes nothing; the back-off itself says
`P3: second late or thin decode within 10 s (...)` before it caps. The same rule guards the long-packet level
(`P3: long packets: ...` below). `P3_BACKOFF_OLD=1` restores the old rule (every late or thin decode caps at once,
the first hold 120 s). There is no setting for this in the program: the CPU health indicator above still counts a
hiccup as a late decode.

## Adaptive CPU features (`CPUadapt`)

Some features cost more computing than others; on a slow computer their
decodes may not be done in time, and every late decode is a lost packet. One
setting covers them all:

| Setting | Effect |
|---|---|
| `CPUadapt 1` (default; `--cpu-adaptive on`; GUI: **Adaptive**) | the features are used, and switched off level by level when this computer's decodes come late |
| `CPUadapt 0` (`--cpu-adaptive off`; GUI: **Off**) | never used: no retries spent finding the limit, for a computer known to be slow |

Two features follow it.

**Long packets as P3 receiver** (the
sender's 3.75 s cycle: about 5 % more throughput at the higher levels, more
at the lower ones). Signum times every long decode. One that is late at level
n stops long packets above level n - 1; one done with less than 8 ms to spare
stops them above level n. Above that level it asks for short packets, which at
the next level up carry more than long ones a level lower. What it learns
belongs to the computer, not to the link: it is kept from link to link while
Signum runs, and one level higher is tried again only after a link whose
long decodes all had at least 16 ms to spare.

The answer to `CPUadapt` (and the GUI's hint under the setting) shows how far
long packets are used on this computer, e.g.
`P3 LONG PACKETS AS RECEIVER: UP TO LEVEL 4 (THIS COMPUTER)`. The log says
`P3: long packets: ...` whenever it changes. As sender, Signum always sends
long packets when the other station asks for them.

**The CPU fallback: P3 packets tried at the expected level only.** As P3
receiver, Signum decodes each packet at the level it expects and, in case
the sender changed level on its own, at one or two others beside it (on a
Raspberry Pi 4 core each extra level costs 120-250 ms of timing search and
7-26 ms of decode per packet). While the CPU health is BUSY or STARVED, and
for a minute after it is OK again, Signum tries only the expected level as
long as packets decode there (no failure, no level request of its own in
flight). A packet the sender did send at another level then fails once,
and from the next one every level is tried again. Nothing changes while the
computer keeps up.

Measured on a Pi 4 with each of two stations on one core (a P3 link sending
bulk text, 90 s, four runs each way): the receiver's short-packet decodes
took 53-71 ms (mean, from launch) against 126-149 ms without the fallback;
it stepped back for late decodes 4 times a run against 6-7, and ended each
run OK where without the fallback it stayed STARVED. The characters delivered
were the same (the link ran at long SL4 either way).

When a fallback is used, the log says so once,
`[cpu] fallback on (CPUadapt): P3 packets tried at the expected level only`,
and `[cpu] fallback off ...` when it is no longer used; `CPULoad` lists it
after `FALLBACKS:`, and the GUI's Computer card shows it under **Fallback**.

Looked at and left as they are (measured on the Pi 4): the P1 caller's
answer search over the frequency-offset grid (at most 5.7 ms of an audio
period, well inside the playback cushion), the waterfall (0.4 % of a core
at its fastest, 40 rows a second), the P3 re-acquisition search after a
slip (as much work as the normal one, by design), the SL1 decode (it is
what keeps a starved link up, and runs below the other decodes), and
memory ARQ (it runs only on failed packets, not seen behind any late
decode). With `CPUadapt 0` none of this is done.
