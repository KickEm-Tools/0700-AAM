#!/usr/bin/env python3
# Copyright (c) 2025 MUSIC Tribe
# This file is part of Kick.
# SPDX-License-Identifier: AGPL-3.0-or-later
"""A stand-in for the control-surface microcontroller, for testing the Kick app's serial link.

It plays the microcontroller's side of the protocol in docs/DEVELOPMENT.md section 8: answers
the app's handshake (EN_SYS_COMM_INIT, then comm init, then comm live), sends panel events
(buttons, pads, knobs, slider), and logs, with a hex dump, every frame the app sends back
(LEDs, pad colours, BPM, ...). Python standard library only.

    # a pseudo-terminal pair; prints the path to give the app
    Kick/tests/fake_mcu.py --pty
    #   FakeMCU: listening on /dev/ttys012
    cd Kick/build && KICK_SERIAL_DEVICE=/dev/ttys012 ./Kick.app/Contents/MacOS/Kick

    # an existing device or pty (a socat pair, a USB adapter wired to a unit's app)
    Kick/tests/fake_mcu.py --device /dev/pts/3

    # Linux: also make the pty appear as the unit's own port, for the app with no variable set
    Kick/tests/fake_mcu.py --pty --link /dev/ttymxc0

Ids and enum values are read from the tree this script lives in
(Kick/modules/contracts/dcl/dcl_contracts.h, ComProtocol/PktMgr/PktMgr.hpp), so a fork that
changes them is followed.

Steps (--steps, comma separated), run once the link is live:
    wait:SECONDS          sleep, still logging what arrives
    press:NAME / release:NAME / tap:NAME
                          a key of BtnLedSequence without the EN_ prefix (PLAY, SHIFT, BANK_A, ...)
    pad:N                 pad N (1-16) pressed at velocity 100, released 100 ms later
    padon:N / padoff:N    pad N pressed / released only
    knob:N:DELTA          knob N (0 = centre, 1-16), DELTA steps (negative = anticlockwise)
    slider:VALUE          slider touch at VALUE
    raw:HEX               bytes as given (for fault tests), e.g. raw:4D5400

Exit status 0 when the handshake completed and every step ran.
"""

import argparse
import os
import re
import select
import sys
import termios
import time
import tty

HERE = os.path.dirname(os.path.abspath(__file__))
KICK = os.path.dirname(HERE)
REPO = os.path.dirname(KICK)

T0 = time.monotonic()


def log(msg):
    print(f"FakeMCU: [{time.monotonic() - T0:8.3f}s] {msg}", flush=True)


# ---------------------------------------------------------------------------- ids from the tree

def parse_enums(path):
    """{enum name: {member: value}} for every plain C++ enum in a header."""
    text = open(path, encoding="utf-8", errors="replace").read()
    text = re.sub(r"//[^\n]*", "", text)
    text = re.sub(r"/\*.*?\*/", "", text, flags=re.S)
    enums = {}
    for m in re.finditer(r"\benum\s+(\w+)\s*\{(.*?)\}", text, flags=re.S):
        name, body = m.group(1), m.group(2)
        members, value = {}, -1
        for item in body.split(","):
            item = item.strip()
            if not item:
                continue
            if "=" in item:
                k, v = (s.strip() for s in item.split("=", 1))
                try:
                    value = int(v, 0)
                except ValueError:
                    value = members.get(v, value + 1)
            else:
                k, value = item, value + 1
            members[k] = value
        enums[name] = members
    return enums


class Ids:
    def __init__(self, contracts, pktmgr):
        c = parse_enums(contracts)
        p = parse_enums(pktmgr)
        self.pkt = p["enumPacketType"]          # EN_PKT_TYPE_SYSTEM ...
        self.dir = p["enumPacketDir"]
        self.btn = c["BtnLedSequence"]          # EN_PLAY ...
        self.sys = c["SysCmd"]
        self.syscomm = c["SysCommCmd"]
        self.btncmd = c["BtnCmd"]
        self.btnstate = c["BtnState"]
        self.ledcmd = c["LedCmd"]
        self.padcmd = c["PadCmd"]
        self.knobcmd = c["KnobCmd"]
        self.knobdir = c["KnobDirection"]
        self.slidercmd = c["SliderCmd"]
        self.padstate = c["PadButtonState"]
        self.type_name = {v: k.replace("EN_PKT_TYPE_", "") for k, v in self.pkt.items()}
        self.cmd_names = {
            self.pkt["EN_PKT_TYPE_SYSTEM"]: self.sys,
            self.pkt["EN_PKT_TYPE_BUTTON"]: self.btncmd,
            self.pkt["EN_PKT_TYPE_LED"]: self.ledcmd,
            self.pkt["EN_PKT_TYPE_PAD"]: self.padcmd,
            self.pkt["EN_PKT_TYPE_KNOBS"]: self.knobcmd,
            self.pkt["EN_PKT_TYPE_SLIDER"]: self.slidercmd,
        }
        self.btn_name = {v: k[3:] for k, v in self.btn.items() if k.startswith("EN_") and v >= 0}

    def button(self, name):
        key = "EN_" + name.upper()
        if key not in self.btn:
            raise SystemExit(f"FakeMCU: no button {name!r} in BtnLedSequence")
        return self.btn[key]

    def cmd_name(self, ptype, cmd):
        for k, v in self.cmd_names.get(ptype, {}).items():
            if v == cmd:
                return k
        return str(cmd)

    def describe(self, f):
        ptype, pdir, cmd, data = f
        s = f"{self.type_name.get(ptype, ptype)} {'req' if pdir == 0 else 'resp'} {self.cmd_name(ptype, cmd)} data={list(data)}"
        if ptype in (self.pkt["EN_PKT_TYPE_LED"], self.pkt["EN_PKT_TYPE_BUTTON"]) and data:
            s += f"  ({self.btn_name.get(data[0], data[0])})"
        return s


# ---------------------------------------------------------------------------- frames

def crc8(data):
    """CPktMgr::CrcGen: CRC-8, polynomial 0x31, initial 0xFF, MSB first, no final XOR."""
    c = 0xFF
    for b in data:
        c ^= b
        for _ in range(8):
            c = ((c << 1) ^ 0x31) & 0xFF if c & 0x80 else (c << 1) & 0xFF
    return c


def encode(ptype, pdir, cmd, data, trailer=b"\n"):
    body = bytes([ord("M"), ord("T"), ptype & 0xFF, pdir & 0xFF, cmd & 0xFF, (len(data) + 1) & 0xFF]) + bytes(d & 0xFF for d in data)
    return body + bytes([crc8(body)]) + trailer


def hexs(b):
    return " ".join(f"{x:02X}" for x in b)


class Decoder:
    """The app's frames: 'M' 'T' type dir cmd size data[size-1] crc, no trailing byte."""

    def __init__(self):
        self.buf = bytearray()
        self.bad = 0

    def feed(self, data):
        self.buf += data
        out = []
        while True:
            i = self.buf.find(b"MT")
            if i < 0:
                if self.buf:
                    junk = self.buf[:-1] if self.buf.endswith(b"M") else self.buf[:]
                    if junk:
                        log(f"rx junk {hexs(junk)}")
                        self.bad += 1
                    self.buf = self.buf[len(junk):]
                return out
            if i > 0:
                log(f"rx junk {hexs(self.buf[:i])}")
                self.bad += 1
                del self.buf[:i]
            if len(self.buf) < 6:
                return out
            size = self.buf[5]
            if size == 0:
                log(f"rx bad frame (size 0) {hexs(self.buf[:6])}")
                self.bad += 1
                del self.buf[:2]
                continue
            total = 6 + (size - 1) + 1
            if len(self.buf) < total:
                return out
            frame = bytes(self.buf[:total])
            del self.buf[:total]
            if crc8(frame[:-1]) != frame[-1]:
                log(f"rx bad CRC {hexs(frame)} (expected {crc8(frame[:-1]):02X})")
                self.bad += 1
                continue
            out.append(((frame[2], frame[3], frame[4], list(frame[6:-1])), frame))


# ---------------------------------------------------------------------------- the link

class Link:
    def __init__(self, args, ids):
        self.ids = ids
        self.slave = None
        if args.pty:
            self.fd, self.slave = os.openpty()
            tty.setraw(self.slave)  # raw before the app opens it, so nothing is echoed
            self.path = os.ttyname(self.slave)
            if args.link:
                if os.path.islink(args.link):
                    os.unlink(args.link)
                os.symlink(self.path, args.link)
                log(f"{args.link} -> {self.path}")
        else:
            self.path = args.device
            self.fd = os.open(self.path, os.O_RDWR | os.O_NOCTTY)
        tty.setraw(self.fd)
        if not args.pty:
            attrs = termios.tcgetattr(self.fd)
            speed = getattr(termios, "B230400", None)
            if speed is not None:
                attrs[4] = attrs[5] = speed
                termios.tcsetattr(self.fd, termios.TCSANOW, attrs)
        self.rx_capture = open(args.capture + ".app-to-mcu.bin", "wb") if args.capture else None
        self.tx_capture = open(args.capture + ".mcu-to-app.bin", "wb") if args.capture else None
        self.decoder = Decoder()
        self.received = []

    def send(self, ptype, cmd, data, note=""):
        frame = encode(ptype, 0, cmd, data)
        os.write(self.fd, frame)
        if self.tx_capture:
            self.tx_capture.write(frame)
            self.tx_capture.flush()
        log(f"tx {self.ids.describe((ptype, 0, cmd, data))}{'  ' + note if note else ''}\n         | {hexs(frame)}")

    def send_raw(self, data):
        os.write(self.fd, data)
        if self.tx_capture:
            self.tx_capture.write(data)
        log(f"tx raw | {hexs(data)}")

    def poll(self, timeout):
        r, _, _ = select.select([self.fd], [], [], timeout)
        if not r:
            return []
        try:
            data = os.read(self.fd, 4096)
        except OSError as e:  # EIO: the other end of a pty is gone
            log(f"read: {e}")
            time.sleep(timeout)
            return []
        if self.rx_capture:
            self.rx_capture.write(data)
            self.rx_capture.flush()
        frames = self.decoder.feed(data)
        for f, raw in frames:
            log(f"rx {self.ids.describe(f)}\n         | {hexs(raw)}")
            self.received.append((time.monotonic() - T0, f))
        return [f for f, _ in frames]


# ---------------------------------------------------------------------------- the script

def main():
    ap = argparse.ArgumentParser(description=__doc__, formatter_class=argparse.RawDescriptionHelpFormatter)
    where = ap.add_mutually_exclusive_group(required=True)
    where.add_argument("--pty", action="store_true", help="create a pseudo-terminal pair and print the slave path")
    where.add_argument("--device", help="use this existing serial device or pty")
    ap.add_argument("--link", help="with --pty: also make this path a symlink to the slave (e.g. /dev/ttymxc0)")
    ap.add_argument("--steps", default="wait:2,press:SHIFT,wait:0.5,pad:1,wait:0.5,release:SHIFT,wait:0.5,tap:PLAY,wait:2,tap:STOP,wait:1",
                    help="what to do once the link is live (see above)")
    ap.add_argument("--retry", type=float, default=1.0, help="seconds between handshake retries (default 1)")
    ap.add_argument("--timeout", type=float, default=60.0, help="give up on the handshake after this many seconds")
    ap.add_argument("--linger", type=float, default=1.0, help="keep logging this long after the steps")
    ap.add_argument("--capture", help="write the raw bytes of each direction to PREFIX.app-to-mcu.bin / PREFIX.mcu-to-app.bin")
    ap.add_argument("--contracts", default=os.path.join(KICK, "modules/contracts/dcl/dcl_contracts.h"))
    ap.add_argument("--pktmgr", default=os.path.join(REPO, "ComProtocol/PktMgr/PktMgr.hpp"))
    args = ap.parse_args()

    ids = Ids(args.contracts, args.pktmgr)
    link = Link(args, ids)
    log(f"listening on {link.path}")

    SYSTEM = ids.pkt["EN_PKT_TYPE_SYSTEM"]
    COMM_INIT = ids.sys["EN_SYS_COMM_INIT"]
    CHECK = ids.sys["EN_SYS_CHECK_COMM_STATUS"]

    # Handshake, as input_events.cpp and KickHome::handleCommand expect:
    #   app -> SYSTEM EN_SYS_COMM_INIT [EN_SYS_COMM_CONNECTED]   (on connect)
    #   mcu -> SYSTEM EN_SYS_CHECK_COMM_STATUS [1]                ("comm init")
    #   app -> SYSTEM EN_SYS_COMM_INIT [2]                         ("Send Comm Init")
    #   mcu -> SYSTEM EN_SYS_CHECK_COMM_STATUS [2]                ("comm live")
    #   app -> SYSTEM EN_SYS_CHECK_COMM_STATUS [2]                 ("Send Comm Live")
    # The app answers only once its home screen exists, so the microcontroller repeats each step.
    state, last_sent, t_connect, tries = "wait-connect", 0.0, None, 0
    deadline = time.monotonic() + args.timeout
    while state != "live":
        if time.monotonic() > deadline:
            log(f"handshake: no answer after {args.timeout:.0f} s (state {state})")
            return 1
        for ptype, pdir, cmd, data in link.poll(0.05):
            if ptype != SYSTEM or not data:
                continue
            if cmd == COMM_INIT and data[0] == ids.syscomm["EN_SYS_COMM_CONNECTED"] and state == "wait-connect":
                t_connect = time.monotonic()
                log("handshake: the app connected (EN_SYS_COMM_INIT [CONNECTED])")
                state, last_sent = "comm-init", 0.0
            elif cmd == COMM_INIT and data[0] == COMM_INIT and state == "comm-init":
                log(f"handshake: comm init answered after {tries} tr{'y' if tries == 1 else 'ies'}")
                state, last_sent, tries = "comm-live", 0.0, 0
            elif cmd == CHECK and data[0] == 2 and state == "comm-live":
                log(f"handshake: LIVE, {time.monotonic() - t_connect:.3f} s after the app connected ({tries} tr{'y' if tries == 1 else 'ies'})")
                state = "live"
        now = time.monotonic()
        if state in ("comm-init", "comm-live") and now - last_sent >= args.retry:
            tries += 1
            link.send(SYSTEM, CHECK, [1 if state == "comm-init" else 2], f"(comm {'init' if state == 'comm-init' else 'live'}, try {tries})")
            last_sent = now

    BUTTON, PAD, KNOBS, SLIDER = (ids.pkt[k] for k in ("EN_PKT_TYPE_BUTTON", "EN_PKT_TYPE_PAD", "EN_PKT_TYPE_KNOBS", "EN_PKT_TYPE_SLIDER"))
    PRESSED, RELEASED = ids.btnstate["BTN_PRESSED"], ids.btnstate["BTN_RELEASED"]

    def wait(seconds):
        end = time.monotonic() + seconds
        while time.monotonic() < end:
            link.poll(min(0.05, max(0.0, end - time.monotonic())))

    def button(name, state):
        link.send(BUTTON, ids.btncmd["EN_BTN_STATUS"], [ids.button(name), state])

    def pad(n, state, velocity):
        link.send(PAD, ids.padcmd["EN_PAD_STATE"], [n - 1, state, velocity, 0])

    for step in filter(None, (s.strip() for s in args.steps.split(","))):
        op, _, arg = step.partition(":")
        log(f"step {step}")
        if op == "wait":
            wait(float(arg))
        elif op == "press":
            button(arg, PRESSED)
        elif op == "release":
            button(arg, RELEASED)
        elif op == "tap":
            button(arg, PRESSED)
            wait(0.1)
            button(arg, RELEASED)
        elif op == "pad":
            pad(int(arg), PRESSED, 100)
            wait(0.1)
            pad(int(arg), RELEASED, 0)
        elif op == "padon":
            pad(int(arg), PRESSED, 100)
        elif op == "padoff":
            pad(int(arg), RELEASED, 0)
        elif op == "knob":
            n, delta = (int(x) for x in arg.split(":"))
            cw = ids.knobdir["EN_KNOB_CLOCKWISE" if delta >= 0 else "EN_KNOB_ANTICLOCKWISE"]
            link.send(KNOBS, ids.knobcmd["EN_KNOB_ROTARY" if n == 0 else "EN_KNOB_QLINK"], [n, cw, abs(delta)])
        elif op == "slider":
            link.send(SLIDER, ids.slidercmd["EN_SLIDER_TOUCH_INPUT"], [int(arg)])
        elif op == "raw":
            link.send_raw(bytes.fromhex(arg))
        else:
            log(f"unknown step {step!r}")
            return 2
        wait(0.05)

    wait(args.linger)

    # Summary of what the app sent.
    counts = {}
    for _, f in link.received:
        key = f"{ids.type_name.get(f[0], f[0])} {ids.cmd_name(f[0], f[2])}"
        counts[key] = counts.get(key, 0) + 1
    log(f"summary: {len(link.received)} frames from the app, {link.decoder.bad} bad")
    for k in sorted(counts):
        log(f"  {counts[k]:4d} x {k}")
    return 0


if __name__ == "__main__":
    try:
        sys.exit(main())
    except KeyboardInterrupt:
        sys.exit(130)
