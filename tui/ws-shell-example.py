#!/usr/bin/env python3
# SPDX-License-Identifier: GPL-3.0-or-later
# Copyright (C) 2026 Jeff Francis, N0GQ
"""ws-shell-example.py: the command shell of signum over its WebSocket
(ws://HOST:PORT/shell): a raw byte stream to the TNC's command interpreter,
exactly as a serial terminal or Pat sees the pty. Binary frames both ways.

  ws-shell-example.py [URL]                 interactive: lines typed are sent with CR,
                                            everything the TNC writes is printed
  ws-shell-example.py [URL] -c "MYcall" ... each command once, print the answers, exit
  ws-shell-example.py [URL] --host          a CRC host-mode (JHOST4) demo: polls the
                                            link status (L on channel 31) and leaves
                                            host mode again (JHOST0)
  --auth USER:PASS                          the TNC's --ws-auth login

URL defaults to ws://127.0.0.1:8770/shell. Needs websocket-client
(pip install websocket-client).
"""
import base64
import sys
import threading
import time

import websocket


def open_shell(url, auth=None):
    hdr = ["Authorization: Basic " + base64.b64encode(auth.encode()).decode()] if auth else []
    return websocket.create_connection(url, header=hdr, timeout=5)


def read_for(ws, seconds, until=None):
    """Everything the TNC writes within `seconds` (or until `until` appears)."""
    out, end = b"", time.time() + seconds
    while time.time() < end:
        ws.settimeout(max(0.01, end - time.time()))
        try:
            op, data = ws.recv_data()
        except websocket.WebSocketTimeoutException:
            break
        out += data
        if until and until in out:
            break
    return out


# ---- CRC host mode (JHOST4), as Pat's driver speaks it -----------------------------------
def crc16_x25(b):
    c = 0xFFFF
    for x in b:
        c ^= x
        for _ in range(8):
            c = (c >> 1) ^ 0x8408 if c & 1 else c >> 1
    return c ^ 0xFFFF


def host_frame(ch, code, payload, toggle):
    """AA AA ch code len-1 payload crc(low, high); every AA after the header stuffed as AA 00."""
    body = bytes([ch, code | (0x80 if toggle else 0), len(payload) - 1]) + payload
    c = crc16_x25(body)
    body += bytes([c & 0xFF, c >> 8])
    return b"\xaa\xaa" + body.replace(b"\xaa", b"\xaa\x00")


def host_reply(raw):
    """(channel, code, payload) of one reply frame, or None."""
    if not raw.startswith(b"\xaa\xaa"):
        return None
    b = raw[2:].replace(b"\xaa\x00", b"\xaa")
    if len(b) < 4:
        return None
    ch, code = b[0], b[1] & 0x7F
    if code in (6, 7):                       # data with a length byte
        n = b[2] + 1
        payload, rest = b[3:3 + n], b[3 + n:]
    elif code == 0:                          # success, nothing more
        payload, rest = b"", b[2:]
    else:                                    # 1..5: text up to a NUL
        z = b.find(b"\x00", 2)
        payload, rest = b[2:z], b[z + 1:]
    if len(rest) < 2 or crc16_x25(b[:len(b) - len(rest)]) != rest[0] | rest[1] << 8:
        return None
    return ch, code, payload


def host_demo(ws):
    ws.send_binary(b"\r")
    read_for(ws, 0.5)
    ws.send_binary(b"JHOST4\r")
    print(read_for(ws, 0.5).decode("latin-1"), end="")
    toggle = False
    for _ in range(3):
        ws.send_binary(host_frame(31, 1, b"L", toggle))
        r = host_reply(read_for(ws, 0.5))
        if r:
            toggle = not toggle              # the toggle flips when a reply decodes
            ch, code, payload = r
            f = payload.decode("latin-1").split()
            state = {"0": "disconnected", "1": "link setup", "3": "disconnect request"}.get(f[5] if len(f) > 5 else "", "connected")
            print(f"ch {ch} code {code}: {payload.decode('latin-1')!r}  (link: {state})")
        else:
            print("no valid reply")
        time.sleep(0.5)
    ws.send_binary(host_frame(0, 1, b"JHOST0", toggle))
    print(repr(read_for(ws, 0.5)))


def main():
    args = sys.argv[1:]
    auth = None
    if "--auth" in args:
        i = args.index("--auth")
        auth = args[i + 1]
        del args[i:i + 2]
    cmds = []
    while "-c" in args:
        i = args.index("-c")
        cmds.append(args[i + 1])
        del args[i:i + 2]
    host = "--host" in args
    pos = [a for a in args if not a.startswith("--")]
    url = pos[0] if pos else "ws://127.0.0.1:8770/shell"
    ws = open_shell(url, auth)
    if host:
        host_demo(ws)
        return 0
    if cmds:
        ws.send_binary(b"\r")                # a prompt first
        read_for(ws, 0.5, b"cmd:")
        for c in cmds:
            ws.send_binary(c.encode("latin-1") + b"\r")
            sys.stdout.write(read_for(ws, 2.0, b"cmd:").decode("latin-1"))
        print()
        return 0

    def reader():                            # interactive: print what the TNC writes
        while True:
            try:
                op, data = ws.recv_data()
            except Exception:
                print("\n[closed]")
                return
            sys.stdout.write(data.decode("latin-1"))
            sys.stdout.flush()
    ws.settimeout(None)
    threading.Thread(target=reader, daemon=True).start()
    ws.send_binary(b"\r")
    for line in sys.stdin:
        ws.send_binary(line.rstrip("\n").encode("latin-1") + b"\r")
    return 0


if __name__ == "__main__":
    sys.exit(main())
