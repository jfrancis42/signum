# Encryption keys: how to handle them

This page covers everything you do with Signum's encryption keys: making them, keeping them
safe, giving them to other stations, checking them, and what to do when something does not
match. [Encryption](encryption.md) describes the feature itself, the failure messages and every
control. Two ready-made recipes are at the end: [quick set-up](#recipe-1-quick-set-up-non-paranoid)
and [maximum assurance](#recipe-2-maximum-assurance-paranoid).

> **WARNING: encryption is illegal on the amateur radio bands.** In the USA, 47 CFR
> 97.113(a)(4) forbids messages encoded for the purpose of obscuring their meaning, and most
> other countries have the same rule. Use Signum's encryption only where the law and your
> licence allow it, never on amateur frequencies. It works only between two Signums, and is
> **not** compatible with the AES encryption of SCS modems.

Console commands below are typed at the Signum console (terminal, telnet, the GUI's terminal
or the TUI). GUI controls are on the **Configure** tab (cards *Encryption*, *This station's key
pair*, *Import keys*, *Known stations*, *Pre-shared keys*) and the **Status** tab (card
*Encryption*).

## The kinds of key

| Key | What it is | Secret? |
|---|---|---|
| **Station key pair** | one per station: a private key (stays on your computer) and a public key (given to others). Proves which station is at the other end | the private key is; the public key is not |
| **Known stations** | the public keys of other stations, each marked *verified* or *NOT verified* | no, but the list shows whom you talk to |
| **Pre-shared key (PSK)** | a secret both stations hold, named after one station's call or `*` (every station) | yes |
| **Session keys** | made fresh for every link and thrown away when it ends | never stored |

A link can use station keys, a pre-shared key, or both. With both, an attacker needs both to
break in.

## Making the station key pair

- Console: `CRYPTO KEYGEN`. GUI: **Generate key pair**. Command line: `signum --keygen`.
- `CRYPTO MYKEY` (GUI: the block under *This station's key pair*) shows the public key block and
  its **fingerprint**, 8 groups of 4 characters such as `1A2B 3C4D 5E6F 7081 92A3 B4C5 D6E7 F809`.
  The fingerprint is what people compare; the whole key is too long to read aloud.
- Make it once. `CRYPTO KEYGEN FORCE` (GUI: **Replace key pair…**) makes a new one, and every
  station that stored your old public key will then refuse your links until it replaces it.

## Where the keys live

- In the key directory: `~/.config/signum/keys/` (or `$XDG_CONFIG_HOME/signum/keys`, or the
  one given with `--key-dir DIR`; the GUI's *Key directory* setting). Directory mode 0700, files
  0600: only your user can read them. Signum warns at start when `station.key` can be read by
  others.
- `station.key` is your private key, `peers` the known stations, `psk` the pre-shared keys,
  `received` keys that arrived over the air and wait for you, `lock` the running Signum's
  process id.
- **The private key has no passphrase.** A station usually runs unattended, so a passphrase
  would have to sit next to the key anyway. The protection is the file mode and your disk:
  **use full-disk encryption** (LUKS on Linux, chosen when installing) on any computer whose
  key matters, so a stolen computer or disk does not give the key away.

- **One key store for everything.** The same directory serves the modem, the Python SDK and the programs built
  on it (folder sync, chat, the SDK's app-layer protection over any modem, a reference unit included; all in the separate project hf-tools): the
  station key, the known stations and their trust levels, the pre-shared keys. While a Signum runs it holds
  the directory's lock and the SDK changes keys through Signum's API; on a station with **only
  reference-unit hardware** no Signum runs, and the key commands of this document are available as
  `signum-keys` (`python -m signum_sdk.keys`, part of the Python SDK in hf-tools): `keygen`, `show`,
  `peers`, `peer add|verify|unverify|delete|replace`, `accept`/`reject` for received keys, `import`, `psk ...`.
  The reference unit's own AES keys are a different scheme and not part of this.

## Backing up and moving to a new computer

- **Back up the private key:** `CRYPTO MYKEY PRIVATE` or GUI **Export private key…** (it warns
  first), then **Save**. Anyone with this block can pose as your station: store it like a
  password, offline (a USB stick in a drawer, or printed on paper in a safe), never by plain
  mail or over the air.
- **Back up everything:** copy the whole key directory while Signum is stopped. The `psk` file
  is as secret as the private key.
- **New computer, or restoring:** before you make a key there, import the saved block:
  `CRYPTO IMPORT FILE /path/to/block.txt`, or GUI *Import keys* (paste or open the file, then
  **Import**). If that computer already has a key pair, tick **replace this station's key
  pair** in the GUI. Or copy the saved key directory into place (mode 0700, files 0600) before
  starting Signum. Your fingerprint stays the same, so the other stations notice nothing.
- If you lose the private key with no backup, make a new one and treat it as a
  [changed key](#when-a-key-changed): every partner must replace your old one.

## Two stations on one computer

Each Signum needs **its own key directory** (`--key-dir`; `signum-loopback` does this for its
two stations). A Signum locks its key directory while it runs. A second one started on the same
directory refuses to start if `--encryption on` is set, and otherwise runs without being able to
turn encryption on or change any key; the message names the directory and `--key-dir`. Two
stations sharing one directory would share one station key; if two stations ever present the
same key to each other, the link fails with "the two stations share one key pair".

## Giving your public key to others

A public key is not secret, but it must arrive **unchanged**: someone who swaps it in transit
can sit in the middle of your links. There are two ways, with different strength:

**Out of band (stronger).** Send the public key block by any channel you like: mail, a USB
stick, a file on a shared drive, a QR code made with any QR tool, or in person.
GUI: **Copy public key** / **Save public key…**; console: `CRYPTO MYKEY`. The other operator
imports it (`CRYPTO IMPORT FILE path`, GUI *Import keys*, or `CRYPTO PEER ADD CALL KEY` with the
44 characters of the `Key:` line). Imported keys are stored **NOT verified** until the fingerprint
is checked (next section). The block's `Check:` line catches typing and copying errors, not
deliberate changes.

**Over the air (easier).** Two ways:

- **First contact (trust on first use, TOFU):** with both stations holding key pairs and
  neither knowing the other's, the first link exchanges the public keys inside the encrypted
  handshake and stores them **NOT verified**. Later links insist on that same key.
- **`CRYPTO SENDKEY`** (GUI **Send over the air**) sends your public key block on the current
  link. The receiving Signum keeps it under *Received over the air*, trusting nothing:
  **Review…** shows the fingerprint, then **Fingerprint matches: accept as verified**,
  **Accept, not verified yet**, or **Reject** (console: `CRYPTO RECEIVED`,
  `CRYPTO ACCEPT CALL [VERIFIED]`, `CRYPTO REJECT CALL`).

Over the air, a station in the middle on the very first link could substitute its own keys.
Verifying (next section) is what rules that out.

## Verifying a key

Verifying means you made sure the key you hold really is the other operator's. It is the step
that turns encryption against eavesdroppers into encryption against impostors too. There are
two ways; do at least one once per station.

**Fingerprint.** Read your fingerprint to the other operator (and theirs to you) by a channel
you already trust: telephone with a voice you know, in person, a signed letter. Never over the
same radio link. If it matches what you have stored: GUI **Verify…** under *Known stations*,
then **It matches: mark verified**; console `CRYPTO PEER VERIFY CALL`. This checks the key
itself, once, and protects every later link.

**SAS (short authentication string).** Each encrypted link shows 8 digits, such as `4821 0937`,
on the Status tab (*SAS* row), in `CRYPTO STATUS` and in the log. Both ends show the same digits
only if no one sat in the middle of **this** handshake. Compare them by voice on another channel
during the link. If they match: **Their key: mark verified…** on the Status tab. That marks the
key the other station presented on this link, and only that key.

| | Fingerprint | SAS |
|---|---|---|
| Checks | the stored key is the operator's | nobody sat in the middle of this one link |
| When | once, when you get the key (and after any change) | during a link |
| Protects against | a key swapped in transit or at first contact; every later impostor without the private key | a station in the middle of this link, including one that holds a pre-shared key both sides use |
| Needs | a trusted channel, any time | a second channel open during the link |

After a key is verified, the panel's **Verified** lamp is green on links with that station; amber
(labelled *Unverified*) means the key on this link is not verified.

## Trust settings

Encryption and verify (the identity check without encryption, [Verify](encryption.md#verify-identity-check-without-encryption))
use the same keys, the same key directory and the same known stations; each has its own
first-contact policy, with the same three choices:

- **First contact allowed (`tofu`, encryption's default).** A new station's key is stored at the first
  link, NOT verified; later links insist on that key. Simple, and safe against everyone who was
  not in the middle of that first link. Verify afterwards to close that gap.
- **Stored only (`stored`, verify's default).** Only a station whose key is already stored here
  (imported, accepted, or stored at an earlier first contact; verified or not) passes; a station
  you have never met is refused. For verify this is the default: an identity check that accepts any
  new key would check nothing on the first link.
- **Verified only (`verified`).** Only keys you have verified are used; first contact and
  unverified keys are refused. You must exchange and verify keys before the first link.

A pre-shared key set for one call sign counts as stored and verified for that station. The shared
`*` key says only that the other station is one of those holding it: verify accepts it alone only
with `tofu`.

GUI: *Encryption* card, **Trust (encryption)** and **Trust (verify)**: Stored only / First contact /
Verified only. Console: `CRYPTO TRUST TOFU|STORED|VERIFIED` (encryption) and
`CRYPTO TRUST VERIFY STORED|TOFU|VERIFIED`. Command line or settings file:
`--encryption-trust stored|tofu|verified` and `--verify-trust stored|tofu|verified`
(**Keep live settings at next start** writes them).

## When a key changed

If a station presents a key other than the one you stored for it, Signum ends the link and
sends nothing. The failure names both fingerprints (stored and presented), and the GUI's
**Replace stored key…** (Status tab) and **Replace…** (that station's row under *Known
stations*) become available. Two explanations are possible:

1. its operator made a new key pair (new computer, lost key, a deliberate change), or
2. **someone is posing as that station.**

Signum cannot tell which. Never replace the key blindly:

1. Contact the other operator by a channel you trust (not the radio link) and ask whether they
   changed their key.
2. Have them read their new fingerprint (`CRYPTO MYKEY` at their end) and compare it with the
   **presented** fingerprint in the failure.
3. Only if it matches: **Replace stored key…** shows the old and the new fingerprint; choose
   **New fingerprint confirmed: replace and mark verified** (or **Replace, not verified yet** if
   you could not compare it yet). Console: `CRYPTO PEER REPLACE CALL NEWFP VERIFIED`, where NEWFP
   is the new fingerprint or at least its first 4 characters.
4. Call again.

If they did **not** change their key, someone else answered as that station: keep the stored
key, and tell the operator.

"Mark verified" never helps here: it marks the stored (old) key, not the one presented, and
Signum refuses to mark a key verified when the stored key is another one.

If you called and the other station has a new key, its answer is only that it could not read
your handshake (your stored copy of its key is wrong). Call once more: Signum then asks for its
key and shows the mismatch with both fingerprints, as above.

## Pre-shared keys

A pre-shared key is a secret that both stations hold. It needs no key pairs and is the simplest
start, but anyone holding it can read the traffic it protects, and the key has to reach the
other station secretly.

- **Per station or for everyone.** A key named with a call (`CRYPTO PSK GEN N0BBB`) is used only
  for links with that station; one named `*` for every station that has no key of its own. With
  `*`, every holder of the key looks the same: the Verified lamp stays amber because the key does
  not show which station is at the other end. Prefer per-station keys.
- **Random or from a passphrase.** **Generate random** (`CRYPTO PSK GEN NAME`) makes a strong key
  that must be copied exactly (Export… shows the block). **From passphrase** (`CRYPTO PSK PHRASE
  NAME words...`, at least 20 characters) lets both operators type the same sentence instead: easy
  to pass by voice, but only as strong as the passphrase. Use a long, unusual sentence.
- **Giving it to the other station.** Only by a safe channel: in person, on a USB stick handed
  over, or encrypted mail. Never over the air and never by plain mail. GUI **Export…** warns that
  it is secret; the other station uses **Set** (with the `Key:` line) or imports the block.
- **The id.** Each pre-shared key has a 4-character id (shown in the table and by `CRYPTO PSK`),
  the same at both ends for the same key; if the ids differ, the link is refused with both ids.
  The id is sent in clear at the start of a link; it identifies the key, but cannot reveal it.
- **Rotating.** Make a new key, give it to the other station by a safe channel, and both set it
  (the same name replaces the old one). Until both have done so, links fail with "the
  pre-shared keys differ". Rotate on a schedule you choose, at once if the key may have leaked,
  and whenever someone who held a `*` key should no longer have it.
- **On top of key pairs.** With key pairs at both ends and a pre-shared key for that station, the
  link uses both (patterns `IKpsk2` and `XXpsk3`): an attacker needs your private key **and** the
  pre-shared key.

## A compromised key, and removing stations

There is no way to revoke a key over the air: Signum has no central list. If your private key
may have been copied (a stolen unencrypted disk, a leaked backup):

1. Make a new key pair (`CRYPTO KEYGEN FORCE`, GUI **Replace key pair…**).
2. Tell every partner by a trusted channel, give them the new public key and its fingerprint.
3. They replace your old key (the replace steps above, comparing the fingerprint you gave them),
   or delete it and import the new one.
4. Also change every pre-shared key that was in the same key directory.

Until a partner has replaced your key, whoever holds the old private key can pose as you to
that partner.

To stop trusting a station: **Delete…** in its row under *Known stations* (`CRYPTO PEER DELETE
CALL`), and delete any pre-shared key named after it (`CRYPTO PSK DELETE CALL`). With trust
*first contact allowed*, a deleted station starts again from first contact at its next link;
with *verified only*, it is refused until you add and verify a key.

## What verify protects, and what it does not

Verify (`--encryption verify`) proves at link start that the other station holds the key you
trust for its call sign: someone using that call sign without the key is refused, and nothing of
yours is sent to them. It does **not** hide anything: your data goes in clear, readable by anyone.
With signing off (the default) the check is made once: a station that takes the link over after
the check is **not detected**. With `--sign on` every record carries a tag from the session keys,
so a taken-over, altered, repeated or reordered record ends the link and is never passed on.

## What encryption protects, and what it does not

Protected: the **content** of everything your programs send over the link (messages, mail,
files), against reading and against undetected change. Each link makes new session keys from
fresh random values, so a key stolen later does not decrypt a recording of an earlier link.

Not protected, visible to anyone listening:

- **Call signs** and the link set-up, and the connect text Signum sends first.
- **That** an encrypted link took place, **when**, **how long** and **how much** was sent: timing
  and volume can reveal a lot (traffic analysis). Compression changes the sizes, but does not
  hide them.
- The **capability record** at the start of each link: that the station is a Signum with
  encryption on, whether it has a key pair, and the id of its pre-shared key.
- Your **location**, by direction finding.

Not protected against: someone who has your private key or your pre-shared key, a computer that
is already compromised, and an impostor at first contact on a key you never verified.

And again: **it is illegal on the amateur bands**, wherever amateur rules forbid encoding to
obscure meaning.

## Recipe 1: quick set-up (non-paranoid)

For two stations that want privacy from casual listeners with little effort:

1. Both stations: **Generate key pair** (`CRYPTO KEYGEN`).
2. Both stations: turn encryption on (**Encryption (now): On…**, type `NOT-AMATEUR` the first time; and
   **Encryption at next start: On…** to keep it). Leave trust at **First contact**.
3. Connect. The first link exchanges the keys (first contact) and shows a SAS on the Status tab.
4. Once, during that first link, phone the other operator and compare the 8 SAS digits. If they
   match, press **Their key: mark verified…** and confirm. They do the same at their end.
5. From now on links use the stored keys, and the Verified lamp is green at both ends. If a
   link ever fails because a key changed, follow [When a key changed](#when-a-key-changed).
6. Back up the private key once (**Export private key…**, **Save**) to a USB stick kept apart.

## Recipe 2: maximum assurance (paranoid)

1. **Computers.** Full-disk encryption on every station computer; the key directory is mode
   0700 (Signum sets it). One key directory per Signum.
2. **Keys.** Each station makes its key pair (`CRYPTO KEYGEN`) and backs up the private key
   offline at once (paper or a USB stick in a safe, never on a networked machine).
3. **Trust verified only.** Set **Trust: Verified only** (`--encryption-trust verified`, kept with
   **Keep live settings at next start**), so no link ever uses a key you did not check.
4. **Exchange keys in person**, or by any channel followed by a fingerprint comparison in person
   or by telephone with a voice you know. Import the block (*Import keys*), then **Verify…** and
   **It matches: mark verified**. Do this before the first link.
5. **A per-station pre-shared key on top.** For each partner, **Generate random** with the
   partner's call as the name, hand the block over in person (USB stick), and have them set it
   under your call. Links then use station keys and the pre-shared key together (`IKpsk2`); check
   the *Method* row on the Status tab. Do not use a `*` key.
6. **Every session:** compare the SAS by voice on another channel. A mismatch means someone is in
   the middle: end the link at once.
7. **Any key mismatch** (a "NOT the one stored" failure, a pre-shared key id that differs, a key
   received over the air that you did not expect): do not replace or accept anything until the
   other operator confirms the new fingerprint in person or by a voice call you placed. Treat
   an unexplained mismatch as an attack and tell the operator.
8. **Rotation.** Agree a schedule, for example: pre-shared keys every three months, station
   keys every year, and both at once after any doubt (a lost backup, a computer repaired by
   someone else, a departed operator). Rotate with the steps in
   [A compromised key](#a-compromised-key-and-removing-stations), including the fingerprint
   check, and delete old backups after the change.
9. **Remove** stations you no longer work (**Delete…**, and their pre-shared key).
10. Remember what is never hidden: call signs, times, durations and amounts of traffic.
