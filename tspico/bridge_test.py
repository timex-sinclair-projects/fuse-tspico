#!/usr/bin/env python3
"""The TS-Pico interface against a stand-in bridge: no TS-Pico firmware or
ROM needed, so CI can run it anywhere.

    python3 tspico/bridge_test.py ./fuse

The stand-in listens on TCP, answers HELLO with version 1, reads of port
0x0f with 0x42 and of 0x0e with 0x99, and records every frame. Fuse (built
--with-null-ui --enable-automation) runs the stock TS2068 ROMs with
--tspico; its debugger types two lines, a key at a time (a code in LAST_K,
FLAGS bit 5 set, as the ROM's keyboard interrupt would):

    POKE 40000,IN 15     a status read: op 2, and 0x42 lands at 40000
    OUT 14,85            a data write: op 0, value 85

and at the end prints [40000]. Then: was HELLO first, did both accesses
arrive as frames, and did the byte the bridge sent reach the Z80?
"""

import os
import socket
import subprocess
import sys
import tempfile
import threading

PORT = 20680
STATUS, DATA = 0x42, 0x99
frames = []


def bridge(srv):
    conn, _ = srv.accept()
    conn.setsockopt(socket.IPPROTO_TCP, socket.TCP_NODELAY, 1)
    buf = b""
    while True:
        try:
            got = conn.recv(64)
        except OSError:
            break
        if not got:
            break
        buf += got
        while len(buf) >= 2:
            op, value = buf[0], buf[1]
            buf = buf[2:]
            frames.append((op, value))
            reply = {4: 1, 1: DATA, 2: STATUS}.get(op, 0)
            conn.sendall(bytes([reply]))
    conn.close()


def keys(lines, first=150, gap=10, end=200):
    """Debugger commands that type each code in `lines`, in turn."""
    codes = [13] + [c for line in lines for c in line + [13]]   # ENTER past the copyright first
    c = ["set $t 0", "set $k 0", "break time 100", "commands 1", "set $t $t+1", "continue", "end"]
    for i, code in enumerate(codes):
        c += ["break time 200 if $k==%d && $t>=%d && ([23611]&32)==0" % (i, first if i == 0 else gap),
              "commands %d" % (i + 2), "set 23560 %d" % code, "set 23611 [23611]|32",
              "set $k %d" % (i + 1), "set $t 0", "continue", "end"]
    c += ["break time 300 if $k==%d && $t==%d" % (len(codes), end),
          "commands %d" % (len(codes) + 2), "print [40000]", "continue", "end"]
    return "\n".join(c)


def main():
    fuse = sys.argv[1] if len(sys.argv) > 1 else "./fuse"
    srv = socket.socket()
    srv.setsockopt(socket.SOL_SOCKET, socket.SO_REUSEADDR, 1)
    srv.bind(("127.0.0.1", PORT))
    srv.listen(1)
    t = threading.Thread(target=bridge, args=(srv,), daemon=True)
    t.start()

    POKE, IN, OUT = 0xF4, 0xBF, 0xDF
    lines = [[POKE] + list(b"40000,") + [IN] + list(b"15"),
             [OUT] + list(b"14,85")]
    out = tempfile.mkdtemp()
    r = subprocess.run([fuse, "--machine", "ts2068", "--no-sound", "--tspico",
                        "--tspico-bridge", "tcp:127.0.0.1:%d" % PORT,
                        "--debugger-command", keys(lines),
                        "--automation-frames", "1200", "--automation-output", out],
                       capture_output=True, text=True, timeout=300)
    t.join(10)

    peek = [int(l, 16) for l in r.stdout.splitlines() if l.startswith("0x")]
    checks = [
        (r.returncode == 0, "Fuse ran (exit %d)" % r.returncode),
        (frames[:1] == [(4, 1)], "HELLO first, version 1"),
        ((2, 0) in frames, "IN 15 arrived as op 2"),
        ((0, 85) in frames, "OUT 14,85 arrived as op 0, value 85"),
        (peek == [STATUS], "the bridge's 0x%02X reached the Z80 (read back %s)"
         % (STATUS, [hex(p) for p in peek])),
    ]
    for ok, msg in checks:
        print(("  PASS  " if ok else "  FAIL  ") + msg)
    print("frames:", frames[:12])
    if not all(ok for ok, _ in checks):
        print(r.stdout[-2000:], r.stderr[-2000:])
        sys.exit(1)
    print("ALL PASS")


if __name__ == "__main__":
    main()
