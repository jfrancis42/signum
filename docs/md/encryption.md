# Encryption

Signum can encrypt an ARQ link between **two Signum stations that both have it on**.
It can also, without encrypting anything, **check that the other station is who it claims to be**
([Verify](#verify-identity-check-without-encryption)). Both are off by default, and neither is in
the program unless it was built with `./configure --enable-encryption`
([Building with it](#building-with-it)).

> **WARNING: encryption is illegal on the amateur radio bands.** In the USA, 47 CFR
> 97.113(a)(4) forbids messages encoded for the purpose of obscuring their meaning, and most
> other countries have the same rule. Use Signum's encryption only where the law and your
> licence allow it (for example licensed commercial or government channels), never on amateur
> frequencies. You alone are responsible for using it lawfully.

> **NOT compatible with the AES encryption of SCS modems** (the PTC series, called "the
> reference unit" elsewhere in these documents), or with any other modem's encryption. It
> works only between two Signums.

> **The chat and sync apps override this setting.** When either app (hf-tools, the separate project with the Python SDK) makes a link, the protection
> mode chosen in that app (Off, Verify, Verify and sign, Encrypt) replaces the protection set here for
> that link, up or down. **Off in chat or sync means plain text, even if Signum is set to encrypt.** Signum's
> own setting is put back when the link ends.

Turning it on the first time asks for the acknowledgement `NOT-AMATEUR` (below), and Signum shows
both texts: on the command line, at the console and in the GUI. Once given, the acknowledgement is
stored in the station's settings file and encryption can be turned on and off at will without
typing it again ([Acknowledgement](#the-acknowledgement-is-stored)); the warning stays on the
Encryption card.

How to handle the keys (making, backing up, exchanging, verifying, replacing, rotating), with a
quick recipe and a maximum-assurance recipe: [Encryption keys](encryption-keys.md).

## Verify: identity check without encryption

`--encryption verify` (console `CRYPTO VERIFY`, the GUI's **Protection: Verify**) runs the same
capability record and the same Noise handshake as encryption, with the same keys, pre-shared keys,
trust and key-change handling, and then sends your data **in clear**. Nothing is hidden, so it needs
no `NOT-AMATEUR` acknowledgement. What it proves is that the station at the other end holds the
private key (or the pre-shared key) you trust for that call sign.

| Mode | Option | Identity of the other station | Your data | Acknowledgement |
|---|---|---|---|---|
| off (default, as the reference unit) | `--encryption off` | not checked | in clear, as typed | none |
| verify | `--encryption verify` | checked at link start | in clear, as typed, no overhead | none |
| verify + sign | `--encryption verify --sign on` | checked at link start and on every record | in clear, each record followed by a 16-byte authentication tag | none |
| encryption | `--encryption on` | checked at link start, and every record is authenticated | encrypted (and compressed) | `NOT-AMATEUR`, once |

**What verify does not protect.** The data is readable by anyone listening. With signing off (the
default, the owner's choice) the identity is checked **once, at link start**: a station that takes
the link over after the check (transmitting over the real station at the right moment) is **not
detected**. Turn signing on to close that: every record then carries a tag made from the session
keys, and a record whose tag does not check ends the link and is never passed to your programs.
With encryption on every record is already authenticated; `--sign` has no extra effect there.

**Signing, in detail.** Once its handshake is complete each side sends one frame, the AES-GCM tag
of nothing under its sending key at nonce 0 with the associated data `Signum verify v1 clear` and a
byte saying whether records are signed: it confirms the keys and marks where the clear data starts.
A signed record is up to 1024 bytes of your data in clear followed by a short system sequence
(`FT` and the tag in base64, 27 characters on the air): the tag is AES-256-GCM with no plaintext and
the record as associated data, from the session's sending key, nonce = the record counter (the same
nonce discipline and rekeying as encrypted records: a record cannot be altered, replayed, reordered
or dropped in the middle without the link ending). The receiver holds each record until its tag
has checked; a record whose tag never comes (the link ended) is discarded. A typed line goes as its
own record when the modem is about to run out of text (at a line end, or after 1 s).

**First contact** has its own policy for verify, `--verify-trust stored|tofu|verified` (console
`CRYPTO TRUST VERIFY ...`, the GUI's **Trust (verify)**), default **stored**:

| Policy | A key not stored here | A stored key, not verified | A verified key |
|---|---|---|---|
| `stored` (verify's default) | link ended | passes | passes |
| `tofu` | stored (not verified), passes | passes | passes |
| `verified` | link ended | link ended | passes |

Encryption keeps its own policy, `--encryption-trust`, default `tofu` as before (it now also takes
`stored`). A link resting on a pre-shared key alone (no station keys) proves the other station holds
that key: a key set for that call sign counts as stored and verified; the `*` key, which every
station holding it shares, does not say which station this is, so under verify it passes only with
`tofu`.

**What works with which station** (the capability record shows which kind it is):

| The other station | off | verify / verify + sign | encryption |
|---|---|---|---|
| Signum 0.5.2 or later, same mode | plain link | verified (signing if either side asks) | encrypted |
| Signum 0.5.2 or later, the other mode (verify against encryption) | | **link ended**: never weaker than either side asked, and encryption is never switched on for a station that did not ask (it needs that station's acknowledgement) | **link ended** |
| Signum with both off (any version with the peer marker) | plain link | **link ended** at once: `the other station is a Signum without the modem-layer identity check on ...` | link ended: `encryption is off at the other station` |
| Signum before 0.5.2 with encryption on | | **link ended** (it asks for encryption) | encrypted, exactly as before |
| The reference unit, another modem, a Signum with its marker off | plain link | **link ended**: after 64 characters with no record (`... modem-layer identity check unavailable`), or after `--encryption-wait` | link ended the same way |
| Any station on a P1 link | plain link | link ended (P1 cannot carry the record) | link ended |

At link start the log, `CRYPTO STATUS`, the Status tab's Encryption card and the WebSocket status
say which mode is in use (`IDENTITY VERIFIED, DATA IN CLEAR (station keys, IK, checked at link start
only)`), or why the requested one is unavailable and the link was ended. Data queued for a link that
is ended this way is discarded, not sent. Unproto is refused while verify is on (a broadcast cannot
be checked). Between two Signums with different settings the stronger request wins or the link
ends: signing is used when either side asks for it; verify against encryption ends the link.

### Verify, sign and encryption over any modem: the SDK's app layer

Everything above is Signum's **modem layer**: it needs **two Signums** and protects every host program. The
Python SDK (the separate project hf-tools) adds an **app layer** with
the same Noise handshake, keys, trust policies, tags and records, run *inside the data stream of one link*, so a
program built on it (folder sync, chat) can check who is at the other end, sign every record, or encrypt, through
**any** modem, **the reference unit included**, as long as the same program runs at both ends. It uses the
**same key directory** as the modem (below and [encryption-keys.md](encryption-keys.md)): one set of keys for
everything. Signing is off by default in both layers; encrypting needs the `NOT-AMATEUR` acknowledgement in both
(the SDK takes it as an argument, or uses the one a local Signum has stored).

| The two stations | Layer in use | Verify | Verify + sign | Encryption |
|---|---|---|---|---|
| Signum, Signum; Signum's own protection on at both | **modem layer**: nothing more is done by the program | yes | yes (`--sign on`) | yes |
| Signum, Signum; Signum's own protection off at either | **app layer** (or switch the modem layer on before the call) | yes | yes | yes |
| Signum or reference unit, a reference unit | **app layer**; the modem layer is unavailable: the other station is not a Signum | yes | yes | yes |
| Signum with its own protection on, a reference unit | Signum **ends the link** (it never runs unprotected): switch its protection off, the app layer then does it | no | no | no |
| the other end does not run the same program | neither: the app layer ends the link | no | no | no |

The program tells the user, at link start, which layer and mode are in use and what is unavailable with this peer and
why (for example: "the other station uses a reference unit: Signum's own identity check is unavailable; the app's
identity check is in use"). A protection that was asked for and cannot be had ends the link; verified data is the only
data delivered when verify is asked, and clear text is never a fallback when encryption is asked. The app layer's
protection covers that program's data only; a takeover after a start-only check is **not detected** in either layer
unless signing is on. The reference unit's **own** AES is a different, proprietary scheme: **not compatible** with
either layer, and Signum does not use or investigate it.

**signum-chat sets Signum's own protection for each link**: the mode chosen in chat wins over what was set here. For the link, the station's own protection becomes exactly what that
mode needs (Off: **off**, signing off; Verify: verify with signing off; Verify and sign: verify + sign; Encrypt: encryption; off
whenever chat's app layer does the work), with chat's trust policy, and is put back when the link is over (also on signals). **Off in
chat means no encryption at either layer**, even on a station set here to encrypt with the acknowledgement stored: the data goes on the
air as plain text, which its tests check on the bytes the other modem received. Verify and sign never hide the data either. When the
setting cannot be made (no acknowledgement, a link already up, a station that encrypts without its acknowledgement stored, which could
not be switched back on: for Off or Verify) chat makes no link; for Encrypt such a station is simply left encrypting. signum-sync follows the same rule (its options are the mode: no option is Off, plain at both layers), with the same refusals.

The app layer is not wire-compatible with the modem layer (a different prologue label, so that one cannot be mistaken
for the other; frames `K` and `T` instead of system sequences; no compression): the SDK's documentation lists every
difference, and its tests run the app layer against Signum's own C++ protection layer through a small bridge.

## The rule: never clear text

**With encryption on, Signum never sends your data in clear, and never falls back to clear
text.** A link that cannot be encrypted is ended, with the reason in the log, the console
(`CRYPTO STATUS`) and the GUI's status. Data you gave it for that link is discarded, not sent.

| What happens | What you see |
|---|---|
| The other station is not a Signum, or a Signum with encryption and its peer marker off (it sends no capability record within `--encryption-wait`, 60 s by default) | `ENCRYPTION FAILED: no capability record from the other station within 60 s ...`; the link is ended |
| The other station is a Signum with encryption off and its [peer marker](connections.md#the-peer-marker-is-the-other-station-a-signum) on (the default): its marker is a capability record without encryption | `ENCRYPTION FAILED: encryption is off at the other station` as soon as its marker arrives; at its end: `the other station asks for encryption, which is off here` |
| No common encryption version (a much newer or older Signum) | `no common encryption version (caller 1, callee 2)` |
| No common key material (no pre-shared key on both, no key pair on both) | `no common key material: ...` |
| The pre-shared keys differ | `the two stations' pre-shared keys differ (ids 3fa2 and 91c0)` |
| The other station's key is not the one you stored for it | `the other station's key is NOT the one stored for N0BBB (stored 1A2B ..., presented 7C3D ...): someone may be posing as it ...`; offers **Replace stored key…** ([A changed key](#a-changed-key-replacing-the-stored-one)) |
| Its copy of your key is wrong (IK) | at the callee: `the handshake failed: the caller's copy of this station's public key is wrong ...`; at the caller: `the other station ended encryption: ...`, and the next call to it asks for its key (XX), so the mismatch above shows with both fingerprints |
| The other station presents your own station key | `the other station presents this station's own key ...: the two stations share one key pair, probably because they use the same key directory` |
| Another Signum uses the same key directory | at start with `--encryption on`: `Refusing to start: ... the key directory ... is in use by another Signum ...; give this one its own with --key-dir DIR`; otherwise encryption cannot be turned on in that run ([Files](#files)) |
| Trust `verified` and the key is unknown or not verified | `... not verified, and trust is "verified"` |
| The handshake does not finish in time | `the handshake did not complete within 60 s` |
| A record fails its check mid-link (altered, repeated, out of order, wrong keys) | `a record failed authentication ...: the link is ended` |
| A P1 link (the other station's MYLevel 1) | `encryption needs a P2 or P3 link` |
| Rekeying, the compressor or memory fails | `rekeying failed`, `out of memory in the encryption layer`, ... |
| Unproto (`U`) while encryption is on | `*** ERROR: UNPROTO REFUSED: ENCRYPTION IS ON` (a broadcast cannot be encrypted) |
| Turning encryption on or off while a link is up | refused: end the link first |

Text the other station sends before the encrypted session is up is not passed to your host
programs (the log shows its first characters), so a program such as Pat never starts its
exchange in clear. Text your programs send before the session is up is held and sent
encrypted once it is; if the session never comes up, it is discarded. Every new link makes
new keys; a link that drops and is called again goes through the whole set-up again.

Two things do travel in clear, as on any link: the call set-up (call signs) and the
station's connect text (`Signum: P1/P2/P3 station`, `CMsg`), which the modem sends before
any program's data; and the capability record and handshake, which hold no user data.

## How it works, briefly

1. When a P2 or P3 link comes up, each Signum with encryption on sends a short **capability
   record** inside the link's own supervisor sequence (an "SI", which the reference unit and
   its kin skip silently). It says what this Signum can do: the encryption versions, whether
   it has a key pair, the id of its pre-shared key, the compression methods. The same record, without
   the encryption fields, is the [peer marker](connections.md#the-peer-marker-is-the-other-station-a-signum)
   a Signum sends with encryption off; with encryption on this record is the marker (the front
   panel's Peer lamp shows Signum).
2. From the two records both stations work out the same choice, with no extra exchange.
3. The calling station starts a **Noise** handshake (the Noise Protocol Framework, as in
   WireGuard and WhatsApp): `IK` when it already knows the other station's public key, `XX` at
   first contact (both public keys travel encrypted), `NNpsk0` with a pre-shared key only, and
   `IKpsk2` / `XXpsk3` when both have key pairs and a pre-shared key. Curve25519, AES-256-GCM,
   SHA-256. Both records are part of the handshake, so a station in the middle that changes
   them makes it fail.
4. Then your data goes in records: **compressed** (zstd with a built-in English dictionary),
   then **encrypted** with AES-256-GCM, one key per direction, fresh keys for every link
   (forward secrecy: a key stolen later does not open recorded traffic).
5. Compression comes first because encrypted bytes cannot be compressed: the modem's own
   compression would gain nothing. Signum's is better than the modem's for text anyway
   (measured). It is on by
   default and used when both Signums have it on; `--encryption-compression off` (the
   GUI's **Compression** switch, `CRYPTO COMPRESS OFF`) sends the records uncompressed, which
   cost 30-43% of the throughput on English text in the measurements.

While it sets the session up, Signum changes the link's direction by itself so that the
records and handshake messages can cross (as if the operator pressed Ctrl-Y), and once the
session is up the callee gives the turn back to the caller. After that the direction changes
exactly as on a plain link: the receiver's text waits for Ctrl-Y (or host `%O`/`%I`, or
`PDuplex 1`), as on the reference unit.

The handshake costs a few hundred bytes once per link. Each record adds 20 bytes (header,
flag, the 16-byte authentication tag).

## Setting it up

You need either **a pre-shared key** on both stations, or **a key pair** on each (and,
best, each other's public key checked). Then turn encryption on at both.

### Turning it on

Command line (or the settings file, `encryption = on` and `encryption-ack = NOT-AMATEUR`):

```
signum ... --encryption on --encryption-ack NOT-AMATEUR
```

Without `--encryption-ack NOT-AMATEUR` Signum prints the warning and does not start, unless the
acknowledgement is already stored. At the console (terminal, telnet, the GUI's Terminal tab):
`CRYPTO ON` shows the warning (or, with the acknowledgement stored, turns it on);
`CRYPTO ON NOT-AMATEUR` turns it on; `CRYPTO OFF` turns it off. Not while a link is up.

#### The acknowledgement is stored

The first time it is given, by any interface (the GUI's dialog, `CRYPTO ON NOT-AMATEUR`,
`--encryption-ack NOT-AMATEUR`), Signum writes `encryption-ack = NOT-AMATEUR` to the settings
file by itself. From then on, for this station:

- `CRYPTO ON` works without the phrase, and the GUI's **On** and **At next start: On** act at
  once, with no dialog;
- `--encryption on` starts without `--encryption-ack`;
- turning encryption off (now, or at next start) removes only `encryption = on`: the
  acknowledgement stays.

To take it back: the Configure tab's **Withdraw acknowledgement**, `CRYPTO ACK OFF`, or
`--encryption-ack none`. Each removes `encryption-ack` and `encryption` from the settings file and
turns encryption off (refused while a link is up); the next On asks for the phrase again.
`CRYPTO ACK` shows whether it is stored. Encryption is never turned on without it: with
`--no-config`, or a settings file that cannot be written, nothing is stored and the phrase is
required every time, as before. With encryption on, Signum still never sends clear text.

### A pre-shared key (the simplest)

On one station make a key and show it:

```
CRYPTO PSK GEN *
CRYPTO PSK SHOW *
```

`*` means "for every station"; use a call sign instead to have a key for one station only (a
key for the call wins over `*`). Give the block to the other operator **by a safe channel**
(in person, encrypted mail; never over the air), who stores it with
`CRYPTO IMPORT FILE path` (the block saved in a file) or `CRYPTO PSK SET * KEY` (the 44
characters of its `Key:` line). `CRYPTO PSK PHRASE * a long passphrase ...` makes a key
from a passphrase of at least 20 characters instead (PBKDF2-SHA256, 200,000 rounds); a
random key is stronger. The key's 4-character id (`CRYPTO PSK`) is the same on both
stations when the keys are the same.

### Key pairs

```
signum --keygen                       # or at the console: CRYPTO KEYGEN
```

makes this station's key pair in `~/.config/signum/keys/station.key` (mode 0600) and prints
the public key:

```
-----BEGIN SIGNUM PUBLIC KEY-----
Call: N0AAA
Fingerprint: 1A2B 3C4D 5E6F 7081 92A3 B4C5 D6E7 F809
Key: oL2b0bLqh6o6s+1f2k9sB3kZ0Zl7mJmJxQp4v1c3Z2w=
Check: 5d41402a
-----END SIGNUM PUBLIC KEY-----
```

A public key is not secret. Send it to the other operator any way you like (mail, over the
air with `CRYPTO SENDKEY`, below); they import it with `CRYPTO IMPORT FILE path` or
`CRYPTO PEER ADD N0AAA KEY`. The `Check:` line catches typing and transfer errors.

**Verify the fingerprint.** A public key that arrived by mail or over the air could have been
swapped by someone in the middle. Read the fingerprint to the other operator by a channel you
trust (telephone, in person) and compare it with theirs (`CRYPTO MYKEY` at their end), then
mark it verified: `CRYPTO PEER VERIFY N0AAA`.

### First contact without importing anything (XX)

If both stations have key pairs and neither knows the other's key, the first link exchanges
the public keys inside the encrypted handshake and stores the other's key as **not verified**
(trust on first use). The log and the status then show:

```
first contact with N0BBB: its key 1A2B ... is stored, NOT verified. Compare the SAS or the fingerprint ...
ENCRYPTED: N0BBB, Noise_XX_25519_AESGCM_SHA256, station keys, compression zen1. SAS 4821 0937 ...
```

The **SAS** (short authentication string, 8 digits) is the same at both ends only if no one
sat in the middle of this handshake. Compare it with the other operator (by voice on another
channel; reading it over the same data link proves nothing), and if it matches, run
`CRYPTO PEER VERIFY N0BBB`. Later links use the stored key (`IK`). If a stored key ever
changes, the link is refused (see the table above and the next section).

### A changed key: replacing the stored one

When a station presents a key other than the one stored for it, the link is ended and nothing
is sent. There are two possible reasons: its operator made a new key pair (a new computer, a
lost key, `KEYGEN FORCE`), or **someone else is posing as that station**. Signum cannot tell
which; only the other operator can. So:

1. Read both fingerprints. The failure names them (stored and presented); `CRYPTO STATUS`
   repeats them, and the GUI's Status tab shows **Replace stored key…** enabled.
2. Ask the other operator, by telephone or in person (never over the same radio link), whether
   they changed their key, and have them read their fingerprint (`CRYPTO MYKEY`).
3. Only if it matches the presented one, replace the stored key:
   `CRYPTO PEER REPLACE N0BBB 7C3D VERIFIED` (the new fingerprint, or at least its first 4
   digits, as the confirmation; leave out `VERIFIED` to store it not verified). In the GUI:
   **Replace stored key…** on the Status tab or **Replace…** in that station's row under Known
   stations shows the old and the new fingerprint; **New fingerprint confirmed: replace and mark
   verified** or **Replace, not verified yet**.
4. Call again: the link now uses the new key.

If the fingerprints do not match, do nothing: keep the stored key and tell the other operator.

When you called the station and it holds a new key, its answer is only that it could not read
your handshake (your copy of its key is wrong, and the handshake hides its key). Call it again:
Signum then asks for its key (XX), refuses it as above, and offers the replacement.

**Mark verified marks only one key.** "Their key: mark verified…" on the Status tab marks
the key the other station presented on this link; Verify… under Known stations marks the
stored key whose fingerprint the dialog shows. Neither ever marks a different key stored
under the same call: if the stored key changed meanwhile, it is refused. `CRYPTO PEER VERIFY`
marks the stored key and names its fingerprint (and says so when the station presented another
key on the last link). Verifying the stored key does not make a link with a changed key work:
only replacing it does.

With `--encryption-trust verified` (`CRYPTO TRUST VERIFIED`) only keys you verified are
accepted, and first contact is refused.

### Sending your public key over the air

On a link (encrypted or not), `CRYPTO SENDKEY` sends your public key block as text. A Signum
that receives a block keeps it under **received keys** and trusts nothing yet:
`CRYPTO RECEIVED` lists them, `CRYPTO ACCEPT N0AAA` (or `ACCEPT N0AAA VERIFIED` after
checking the fingerprint by another channel) makes it a known station, `CRYPTO REJECT N0AAA`
drops it. Sending a public key in clear is fine; it is not secret.

### Moving or backing up the station's key

`CRYPTO MYKEY PRIVATE` shows the private key block. Anyone who has it can pose as your station:
store it like a password. Import it on the new computer with `CRYPTO IMPORT FILE path`.

## Files

`~/.config/signum/keys/` (`$XDG_CONFIG_HOME/signum/keys`, or `--key-dir DIR`), mode 0700. **Two Signums on one
computer each need their own key directory** (`--key-dir`; `signum-loopback` does this): sharing one would give
both the same station key and they would overwrite each other's list of known stations.

**One key store for everything.** The key directory is shared by the modem, the Python SDK and the programs
built on it (folder sync, chat): the SDK reads these files directly and, while a Signum holds the lock, changes keys
through Signum's WebSocket API, never by writing the files; without Signum it takes the same lock itself. Key
management without Signum: `signum-keys` (part of the Python SDK, hf-tools).

**One Signum per key directory.** A Signum locks its key directory (the file `lock`, which holds its
process id) for as long as it runs. A second Signum on the same directory:

- with `--encryption on` refuses to start, naming the directory, the other process and `--key-dir`;
- otherwise starts with a warning, and cannot turn encryption on or change any key in that run
  (`CRYPTO STATUS`, the Status tab's Note row and the WebSocket status say why);
- `--keygen` on a directory in use is refused.

Every change re-reads the file it changes first and writes a new file that replaces the old in one
step, so a change made by hand while Signum runs is kept, and a crash never leaves half a file.
The directory holds:

| File | What | Mode |
|---|---|---|
| `station.key` | this station's private key (a text block) | 0600 |
| `peers` | known stations: call, public key, verified or not, since when | 0600 |
| `psk` | pre-shared keys: name (a call or `*`), key | 0600 |
| `received` | public keys received over the air, not yet accepted | 0600 |
| `lock` | the running Signum's process id (see above) | 0600 |

**The private key has no passphrase.** A station usually runs unattended (a service started
at boot), so a passphrase would have to be typed at every start or stored next to the key,
which protects nothing. The protection is the file mode (Signum warns at start when
`station.key` can be read by others) and the computer's own disk encryption: use full-disk
encryption on a station whose key matters.

## Commands and options

| Console | What |
|---|---|
| `CRYPTO` / `CRYPTO STATUS` | on/off, this link's state, the station's fingerprint, counts |
| `CRYPTO ON NOT-AMATEUR` / `CRYPTO OFF` | turn it on (with the acknowledgement, the first time: it is then stored) / off; not during a link. `CRYPTO ON` alone works once it is stored |
| `CRYPTO VERIFY` | the identity check without encryption (verify); no acknowledgement; not during a link. `CRYPTO OFF` turns it off, `CRYPTO ON` switches to encryption |
| `CRYPTO SIGN ON\|OFF` | verify: an authentication tag on every record (default off; from the next link) |
| `CRYPTO ACK` / `CRYPTO ACK OFF` | whether the acknowledgement is stored / withdraw it and turn encryption off (verify is not affected) |
| `CRYPTO TRUST [ENCRYPT] STORED\|TOFU\|VERIFIED` | encryption's first-contact policy: tofu (default) / stored keys only / verified keys only |
| `CRYPTO TRUST VERIFY STORED\|TOFU\|VERIFIED` | verify's: stored (default) / tofu / verified |
| `CRYPTO COMPRESS ON\|OFF` | compress the records before encrypting (default on; from the next link) |
| `CRYPTO WAIT S` | the capability record and the handshake each at most S s (10-600) |
| `CRYPTO KEYGEN [FORCE]` | make the key pair (FORCE replaces it) |
| `CRYPTO MYKEY [PRIVATE]` | the public key block (the private one with PRIVATE) |
| `CRYPTO SENDKEY` | send the public key block on the link |
| `CRYPTO PEERS` | known stations with fingerprints |
| `CRYPTO PEER ADD CALL KEY [VERIFIED]`, `PEER VERIFY\|UNVERIFY\|DELETE\|SHOW CALL` | manage them |
| `CRYPTO PEER REPLACE CALL NEWFP [VERIFIED]` | after a link failed on a changed key: store the key CALL presented (NEWFP: its fingerprint, at least the first 4 digits) |
| `CRYPTO RECEIVED`, `ACCEPT CALL [VERIFIED]`, `REJECT CALL` | keys received over the air |
| `CRYPTO IMPORT FILE PATH` | every key block in a file |
| `CRYPTO PSK`, `PSK GEN\|SHOW\|DELETE NAME`, `PSK SET NAME KEY`, `PSK PHRASE NAME words...` | pre-shared keys |

Command-line options (also settings-file keys without the `--`; [Options](options.md#encryption)):
`--encryption off|verify|on`, `--encryption-ack NOT-AMATEUR|none`, `--encryption-trust stored|tofu|verified`,
`--verify-trust stored|tofu|verified`, `--sign on|off`, `--encryption-wait S`, `--encryption-compression on|off`, `--key-dir DIR`, and the one-shot
`--keygen`, `--show-key`.

## In the GUI

Everything above can be done in the browser GUI too (through the WebSocket API's `crypto`
request: [src/web/README.md](../../src/web/README.md#encryption)). A build made without
`./configure --enable-encryption` (the default) shows none of it, in the GUI or the TUI.

**Chat and sync override the Encryption card** for their own links (above): what is set here applies to every
other link, and comes back when theirs ends.

**Configure tab, Encryption card.** At the top, in red, the legal warning; below it, in amber,
the statement that this encryption is not compatible with SCS modems; then the rule that it never
falls back to clear text. The controls:

- **Protection (now): Off / Verify / On…** turns it off, to the identity check without encryption
  ([Verify](#verify-identity-check-without-encryption), as `CRYPTO VERIFY`, at once, no dialog) or
  to encryption for this run (as `CRYPTO ON` / `CRYPTO OFF`).
  The first **On…** opens a dialog with the warning and the compatibility statement again, and a
  field where you type `NOT-AMATEUR`; its button stays disabled until you have. Cancel leaves it
  off. The acknowledgement is then stored in the settings file and On acts at once from then on.
  Refused while a link is up (the reason is shown under the card).
- **Protection at next start: Off / Verify / On…** writes `encryption = verify` or `encryption = on`
  to the settings file (On the first time with `encryption-ack = NOT-AMATEUR`, after the same dialog);
  Off removes the line. "now:" shows this run's state.
- **Sign every record: On / Off** (`--sign`, default off): verify only; from the next link.
- **Acknowledgement: Withdraw acknowledgement** removes the stored acknowledgement and turns
  encryption off (verify stays as it is); enabled only while one is stored ("stored" / "not given yet" beside it). The
  next On asks for the phrase again.
- **Compression: On / Off** (`--encryption-compression`), from the next link. Default on.
- **Trust (encryption): Stored only / First contact / Verified only** (`--encryption-trust stored|tofu|verified`, default first contact).
- **Trust (verify): Stored only / First contact / Verified only** (`--verify-trust`, default stored only).
- **Set-up timeout (s)** (`--encryption-wait`, 10-600, default 60).
- **Key directory** (`--key-dir`): applies at the next start; "now:" shows the one in use.

Signing, compression, trust and the timeout apply at once for this run; the tab's **Keep live settings
at next start** writes them to the settings file as well. It does not write `encryption = on` or `verify`:
only the next-start switch does (on: after the acknowledgement).

**This station's key pair.** The fingerprint and the public key block. **Generate key pair**
(`--keygen`; once there is one it reads **Replace key pair…** and warns that stations holding the
old key will refuse links until they get the new one). **Copy public key** and **Save public
key…** (a text file) give the block to send to other operators. **Send over the air** sends it on
the current link (`CRYPTO SENDKEY`; enabled only while a link is up). **Export private key…**
warns that the block is secret, then shows it with Copy and Save (to move or back up the station).

**Import keys.** Paste one or more key blocks, or open a file; **Import** stores public keys as
known stations (NOT verified), pre-shared key blocks as pre-shared keys, and a private key as
this station's key (only with **replace this station's key pair** ticked when it has one). The
call field is for a public block without a `Call:` line.

**Known stations.** Each station's call, fingerprint, trust (verified / NOT verified) and since
when. **Verify…** shows the fingerprint in large type with the instruction to compare it with the
other operator by telephone or in person (and, during an encrypted link with that station, its
SAS); **It matches: mark verified** stores that key (only that one) as verified. After a link
failed because the station presented another key, its row says **presented another key** and
offers **Replace…** ([A changed key](#a-changed-key-replacing-the-stored-one)); Verify… then
warns that it verifies the stored key. **Unverify**, **Export** (its public key
block) and **Delete…**. Below the table: add a station by call and the 44 characters of its `Key:`
line (stored NOT verified).

**Received over the air.** Public keys other stations sent with `CRYPTO SENDKEY`, not trusted
yet. **Review…** shows the fingerprint and the warning that anyone could have sent it, with
three choices: **Fingerprint matches: accept as verified**, **Accept, not verified yet**, and
**Reject**. **Reject** in the table drops it at once.

**Pre-shared keys.** Each key's name (a call, or `*` for every station) and id (the same at both
ends for the same key). **Export…** warns that the key is secret, then shows the block with Copy
and Save. **Delete…** asks first. Below: a **Name** (default `*`) for **Generate random**, **Set**
(from the 44 characters of a block's `Key:` line) and **From passphrase** (at least 20 characters).

### The front panel's Crypto lamps

Four lamps on the front panel show the encryption at a glance ([The front panel](gui.md#the-front-panel)):

| Lamp | Green | Amber | Red | Dark |
|---|---|---|---|---|
| **Armed** | encryption or verify on for this run (the tooltip says which) | | | off |
| **Key / Fail** | session keys agreed (encryption) or the other station's identity checked (verify) | blinking: set-up running; steady: a public key received over the air waits for review | the set-up FAILED (nothing sent in clear or unverified) | no negotiation |
| **Secure** | this link's traffic is encrypted | | | no encrypted session; also on a verified link (verify: the data goes in clear) |
| **Verified** | the other station's key is verified, or it holds the pre-shared key set for it alone | not verified (labelled Unverified) | | no session |

With verify the same four lamps are used, so the panel never changes size: Armed, Key and Verified
as with encryption, Secure dark because nothing is encrypted; the tooltips and the Status card say so.

A failure keeps Key / Fail red after the link has ended, until the next link starts (or encryption
is turned on or off), so it is not missed; the Status card's Note row then names it too. The
other station's key turns Verified green during the link as soon as you mark it verified. A link
that uses the shared `*` pre-shared key and no station keys stays amber: every station holding that
key could be this one. Hover over a lamp (tap it on a phone) for the detail. The TUI has the same
four lamps on its `CRYPTO` line.

**Status tab, Encryption card** (this link's negotiation; always the same rows):

| Row | Shows |
|---|---|
| State | off; on: no link; verify (identity check): no link; setting up: waiting for the other station's capability record; setting up: handshake (with the pattern); **ENCRYPTED**; **IDENTITY VERIFIED · data in clear**; or **FAILED: the link was ended; nothing was sent in clear** (verify: nothing unverified was sent or passed on) (red) |
| Station | the other station and who called |
| Method | the Noise pattern, the key material, the compression agreed (verify: every record signed, or checked at link start only) |
| SAS | the 8 digits to compare with the other operator by voice on another channel |
| Their key | its fingerprint, verified or NOT verified |
| Note | that another Signum holds the key directory (encryption unavailable); the reason of a failure (and, after the link, the last link's failure until the next one); data held until the session is up; "no key pair and no pre-shared key" when encryption is on with nothing to use; why compression is not used |
| Traffic | records and bytes each way (before and after compression; verify: bytes, or signed records, in clear) |
| Keys received | keys received over the air waiting for review |

**Their key: mark verified…** under it opens the same Verify dialog for the key the other station
presented on this link (enabled while an encrypted link's key is not verified). **Replace stored
key…** beside it is enabled after a link failed because the other station presented a key other
than the stored one: it shows the old and the new fingerprint and replaces the stored key after
you confirm. A failed link is only shown: there is no
button that continues or repeats it without encryption. To talk in clear, turn encryption off
yourself and call again.

**TUI.** Two lines under the tuning row: `CRYPTO` with the four lamps (Armed, Key/Fail, Secure,
Verified; in `--once` plain text G, Y, R or `.`), and `ENCRYPTION`: `OFF`, `ON no link`, `VERIFY no link`, `SETTING UP`,
`VERIFIED, IN CLEAR` (verify), `ENCRYPTED` with the call, the pattern, the SAS and whether the key is verified, `FAILED:` with
the reason (after the link: the last link's failure, until the next link); and the number of keys
received over the air waiting. The `CRYPTO` console commands
work on its command line.

## Building with it

Encryption is opt-in at build time. The default build (`./configure && make`) has no
encryption code at all: OpenSSL and zstd are not needed or linked, and there are no
encryption options, `CRYPTO` command, settings keys or WebSocket requests; the GUI hides every
encryption control (the WebSocket `hello` says `"crypto": false`). To build it in:

```
./configure --enable-encryption && make
```

That build needs OpenSSL 3 (libcrypto) and zstd:

| | Package names |
|---|---|
| Debian, Ubuntu, Linux Mint, Raspberry Pi OS | `libssl-dev libzstd-dev` |
| Fedora, RHEL | `openssl-devel libzstd-devel` |
| Arch Linux | `openssl zstd` |

`./configure --enable-encryption` stops with a message naming these packages when they
are missing (`./configure` alone builds without encryption). OpenSSL 3.0 or later is required (Debian 12, Ubuntu 22.04, Raspberry Pi OS
bookworm or newer): its Apache-2.0 licence may be combined with Signum's GPL-3.0, OpenSSL
1.1.1's may not. Nothing is built from source.
