# Installing Signum

Signum is one C++17 program (`signum`) plus two shell scripts, a browser
GUI, a terminal front panel and a man page. Install a release package, or
build it from source (the rest of this page).

## Release packages

Each [release](https://github.com/jfrancis42/signum/releases) has ready-built
files for 64-bit x86 (`amd64`) and 64-bit ARM (`arm64`, a Raspberry Pi 4 or 5
with the 64-bit OS), built without encryption:

- **Debian 12 or later, Raspberry Pi OS (64-bit), Ubuntu 22.04 or later,
  Linux Mint 21 or later:** the `.deb`.

  ```
  sudo apt install ./signum_*_arm64.deb                # or _amd64
  ```

  It installs `/usr/bin/signum` with real-time priority already set, the
  GUI, the man page, this documentation in `/usr/share/doc/signum`, and the
  example systemd units in `/usr/share/signum/examples` (not enabled).
- **Any other distribution** (Fedora, Arch, ...): the `.tar.gz`. It needs
  ALSA's library and, for real-time priority, `setcap`.

  ```
  tar xzf signum-*-linux-amd64.tar.gz
  sudo signum-*-linux-amd64/install.sh       # under /usr/local; --uninstall removes it
  ```

`SHA256SUMS` lists every file's checksum, and `gh attestation verify FILE
--repo jfrancis42/signum` checks that a file was built by the project's
release workflow. The `.debug` file holds the program's debug symbols, for a
backtrace from a crash. A 32-bit OS, and anything older than the systems
above, builds from source.

## Packages by distribution

What Signum needs, for building it on any distribution.

### What each package is for

| What | Needed for | Debian, Raspberry Pi OS | Ubuntu, Linux Mint | Fedora | Arch Linux |
|---|---|---|---|---|---|
| C++17 compiler (g++ 12 or later recommended; `configure` accepts g++ 8+) | building | `g++` | `g++` | `gcc-c++` | `gcc` |
| GNU make | building | `make` | `make` | `make` | `make` |
| pkg-config | building (optional: `configure` finds the libraries without it too) | `pkg-config` | `pkg-config` | `pkgconf-pkg-config` | `pkgconf` |
| ALSA headers and library | building and running (sound) | `libasound2-dev` | `libasound2-dev` | `alsa-lib-devel` | `alsa-lib` |
| OpenSSL 3 (libcrypto) | encryption: building and running (only with `--enable-encryption`) | `libssl-dev` | `libssl-dev` | `openssl-devel` | `openssl` |
| zstd | encryption: building and running (only with `--enable-encryption`) | `libzstd-dev` | `libzstd-dev` | `libzstd-devel` | `zstd` |
| `setcap`, `getcap` | real-time priority (recommended) | `libcap2-bin` | `libcap2-bin` | `libcap` | `libcap` |
| `aplay`, `arecord`, `amixer`, `alsactl` | finding the sound card, checking levels, the mixer (recommended) | `alsa-utils` | `alsa-utils` | `alsa-utils` | `alsa-utils` |
| Python 3 | the terminal front panel, TUI (recommended; standard library only) | `python3` | `python3` | `python3` | `python` |
| Hamlib's `rigctld` | `--ptt rig` and rig control (optional) | `libhamlib-utils` | `libhamlib-utils` | `hamlib` | `hamlib` |
| websocket-client (Python) | the example shell client `ws-shell-example.py` (optional) | `python3-websocket` | `python3-websocket` | `python3-websocket-client` | `python-websocket-client` |

On Arch the headers come with the libraries; elsewhere the `-dev` / `-devel`
package pulls in the library itself. On a computer that only **runs** a
Signum built elsewhere, the libraries are enough (Debian 13:
`libasound2t64`; Debian 12 and Ubuntu 22.04: `libasound2`; for an
encryption build also `libssl3t64 libzstd1` / `libssl3 libzstd1`), plus `libcap2-bin` and `alsa-utils`.

### One command per distribution

**Debian 12 or 13, Raspberry Pi OS (Bookworm, Trixie), Ubuntu 22.04 or
later, Linux Mint 21 or later (and LMDE 6):**

```
sudo apt install g++ make pkg-config libasound2-dev libcap2-bin alsa-utils python3
sudo apt install libhamlib-utils python3-websocket          # optional
```

**Fedora:**

```
sudo dnf install gcc-c++ make pkgconf-pkg-config alsa-lib-devel libcap alsa-utils python3
sudo dnf install hamlib python3-websocket-client             # optional
```

**Arch Linux** (and derivatives such as Omarchy, Manjaro, EndeavourOS):

```
sudo pacman -S --needed gcc make pkgconf alsa-lib libcap alsa-utils python
sudo pacman -S --needed hamlib python-websocket-client       # optional
```

**Encryption is opt-in.** The commands above build without it: no
OpenSSL or zstd linked, no `--encryption...` / `--key-dir` / `--keygen` /
`--show-key` options, no `CRYPTO` command or settings keys, no Encryption
cards in the GUI and no key files. Everything else, plain links included,
is the same. To build it in, also install `libssl-dev libzstd-dev` (Fedora
`openssl-devel libzstd-devel`, Arch `openssl zstd`) and run
`./configure --enable-encryption` ([Encryption](encryption.md#building-with-it)).
Encryption needs OpenSSL 3.0 or later (Debian 12, Ubuntu 22.04, Raspberry Pi OS
Bookworm and newer have it): OpenSSL 3 is Apache-2.0, which may be combined
with Signum's GPL-3.0; the older OpenSSL 1.1.1 licence may not.

### Real-time priority and the serial port on other distributions

The script's steps 2 and 3 are Debian-only; elsewhere do them by hand:
[Real-time priority](performance.md#real-time-priority) (the capabilities
that `make install` sets are enough on their own) and
[the CPU clock unit](performance.md#installing-the-cpu-clock-unit-signum-cpufreqservice).
Two differences to know:

- **Arch:** the serial ports belong to the group **`uucp`**, not `dialout`
  (`sudo usermod -aG uucp,audio $USER`). The package `realtime-privileges`
  creates a group `realtime` with real-time priority up to 98 and unlimited
  locked memory (checked in a container): `sudo pacman -S realtime-privileges && sudo usermod -aG
  realtime $USER`, then log in again.
- **Fedora:** the serial ports belong to `dialout`, as on Debian
  (`sudo usermod -aG dialout,audio $USER`).

### Checked how

| Distribution | How the lists above were checked |
|---|---|
| Debian 13 (Trixie), amd64 | a clean container (November 2026): both commands, `./configure` with and without encryption, a full build with encryption, `signum --version`; and the development build computer |
| Raspberry Pi OS Trixie, arm64 (Pi 4) | a full build on the Pi |
| Ubuntu 24.04, Linux Mint 22.1 | clean containers, as Debian 13 (g++ 13) |
| Fedora 45 | a clean container, as Debian 13 (g++ 16) |
| Arch Linux | a clean container, as Debian 13; and the development workstation (Omarchy) |
| Ubuntu 22.04, Linux Mint 21, LMDE 6, RHEL and its rebuilds, Manjaro, EndeavourOS | **not checked**. Ubuntu 22.04 and Mint 21 ship g++ 11 (`configure` accepts it; not built there) |

### What was installed on the development computers

Everything installed on the computers Signum was developed and tested on,
so that the same set-up can be repeated:

| Computer | Installed for Signum |
|---|---|
| Raspberry Pi 4, Raspberry Pi OS Trixie | `build-essential git libasound2-dev python3-websocket alsa-utils rsync`, plus `python3-numpy` and `chromium` (not used by Signum or its tests there); `linux-perf` (profiling); `libssl-dev libzstd-dev` (encryption) and `libbrotli-dev zlib1g-dev` (only for a compression comparison, not needed); `libhamlib-utils`; the realtime group, limits file and CPU clock unit |
| Build computer, Debian 13 | `g++ make pkg-config libasound2-dev libzstd-dev alsa-utils python3-websocket libhamlib-utils`, Pat 1.0.0 (Pat's `.deb`), and for cross builds to the Pi `crossbuild-essential-arm64 libasound2-dev:arm64 libzstd-dev:arm64`. It has no `libssl-dev`, so builds there are without encryption |
| Workstation, Arch (Omarchy) | `gcc make pkgconf alsa-lib alsa-utils openssl zstd libcap python chromium hamlib python-websocket-client`; Pat (AUR `pat-bin`); Playwright 1.62 for Python (`pip install --user`), using the system Chromium |

### Memory for building

Compiling the modem can take close to 2 GB in one
compiler process. Build on a machine with 2 GB or more (or add swap and
accept a slow build, or cross-compile) and copy the program to a smaller
one. Running it takes far less: see [Hardware and performance](performance.md).

## Build

```
./configure
make
```

`make` builds `build/signum`. You can run everything straight from the
source tree (`./signum-start`, `./signum-loopback`); installing is optional.

Plain `make` works without `./configure` too, with the defaults below.

### configure options

`./configure` is a small hand-written shell script (not autotools). It checks
for a C++17 compiler and ALSA and writes `config.mk`, which the Makefile
reads.

| Option | Default |
|---|---|
| `--prefix=DIR` | `/usr/local` |
| `--bindir=DIR` | `PREFIX/bin` |
| `--datadir=DIR` | `PREFIX/share` (the GUI and TUI go to `DATADIR/signum`) |
| `--sysconfdir=DIR` | `PREFIX/etc` (`/etc` when the prefix is `/usr`) |
| `--mandir=DIR` | `DATADIR/man` |
| `--host=TRIPLET` | none; for a cross build, `CXX` then defaults to `TRIPLET-g++` |
| `--enable-encryption` | no encryption at all: no encryption code, options, commands, settings or GUI controls; with this, encryption built in (needs OpenSSL 3 and zstd). `--no-encryption` states the default |
| `CXX=`, `CXXFLAGS=`, `LDFLAGS=`, `AR=` | `g++`, `-std=c++17 -O2 -g -Wall -Wextra -pthread`, none, `ar` (`TRIPLET-g++` / `TRIPLET-ar` with `--host`); also read from the environment |

`./configure --help` lists them. Cross-compiling for a Raspberry Pi (64-bit)
from an x86 machine, for example: `./configure --host=aarch64-linux-gnu`
(the ARM ALSA development files must be installed for the cross compiler).

## Install

```
sudo make install            # or: make install DESTDIR=/tmp/pkg  (packagers)
```

What goes where (with the default prefix `/usr/local`):

| What | Where |
|---|---|
| `signum`, `signum-start`, `signum-loopback` | `/usr/local/bin` |
| the browser GUI | `/usr/local/share/signum/www` |
| the TUI `signum-tui` and the example client `ws-shell-example.py` | `/usr/local/share/signum/tui` |
| example systemd units `signum.service`, `signum-cpufreq.service` (not enabled) | `/usr/local/share/signum/examples` |
| a commented sample `calibration.conf` | `/usr/local/etc/signum` |
| the man page `signum.1` | `/usr/local/share/man/man1` |
| `README.md`, the licence (`COPYING`) and this guide (`docs/`) | `/usr/local/share/doc/signum` |

The program has the GUI's installed path built in. It also finds a `www/`
directory beside itself or in the current directory, so the build tree works
without installing; `--gui DIR` names another one.

`sudo make uninstall` removes everything `make install` put there. Your own
files under `~/.config/signum` and `~/.local/state/signum` are left alone.

## Real-time priority (do this once)

On a radio, Signum must answer the other station within a few milliseconds
of the right moment. It asks Linux for real-time scheduling for its audio
loop and its decoders. For that the program needs the file capabilities
`cap_sys_nice,cap_ipc_lock+ep`:

- `make` and `make install` set them by themselves when they run as root or
  can use `sudo` without a password. Otherwise they print the command, for
  example `sudo setcap cap_sys_nice,cap_ipc_lock+ep build/signum`.
- `make rt` sets them on the build tree's program and asks for your `sudo`
  password.
- With `DESTDIR` set, `make install` only prints a reminder: the package must
  set them. `NO_SETCAP=1` skips the step.

Rebuilding the program clears the capabilities; `make` sets them again when
it can. Details, the alternative without capabilities, and how to check that
it worked are in [Hardware and performance](performance.md#real-time-priority).

## Next

[Getting started](getting-started.md): try Signum without a radio, then
with one.
