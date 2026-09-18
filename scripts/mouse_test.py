#!/usr/bin/env python3
"""scripts/mouse_test.py — PS/2 mouse gate over QMP (G1 proof).

Boots mectov64.iso with -vga std (KVM if available, else TCG), runs the
`mousedemo` Ring-3 program via the shell, injects relative motion with
HMP mouse_move, and asserts end to end: controller init, IRQ12 packets,
cursor composite on screen, GETMOUSE syscall values. Exit 0 PASS.

Checks: `mouse: PS/2 live` in serial, typing, MOUSE-START position sane
(center 511,383), total reported delta-x >= 250 over 4x150 moves, y
unchanged, white cursor pixels near the last reported position,
MOUSE-DONE, no FATAL.
"""
import os
import re
import subprocess
import sys
import time

sys.path.insert(0, os.path.join(os.path.dirname(__file__)))
from qmp import QMP
from kbd_test import has_kvm, wait_for, type_line

ISO = "mectov64.iso"
SERIAL = "serial_mousetest.log"
SOCK = "/tmp/qmp_mousetest"
SHOT = "/tmp/shot_mouse.ppm"

MOVES = [(150, 0)] * 3


def boot():
    cmd = ["qemu-system-x86_64", "-machine", "q35"]
    if has_kvm():
        cmd += ["-cpu", "host", "-enable-kvm"]
    else:
        cmd += ["-cpu", "qemu64,+nx"]
    cmd += ["-m", "256", "-smp", "4", "-vga", "std", "-cdrom", ISO,
            "-serial", f"file:{SERIAL}", "-no-reboot", "-display", "none",
            "-qmp", f"unix:{SOCK},server=on,wait=off"]
    for p in (SOCK, SERIAL, SHOT):
        try:
            os.unlink(p)
        except OSError:
            pass
    return subprocess.Popen(cmd, stdout=subprocess.DEVNULL,
                            stderr=subprocess.DEVNULL)


def connect_qmp(timeout=30):
    end = time.time() + timeout
    while time.time() < end:
        try:
            return QMP(SOCK)
        except OSError:
            time.sleep(0.5)
    raise RuntimeError("qmp connect timeout")


def shot(q):
    r = q.hmp(f"screendump {SHOT}")
    if isinstance(r, dict) and "error" in r:
        raise RuntimeError(f"screendump failed: {r['error']}")
    for _ in range(100):
        if os.path.exists(SHOT) and os.path.getsize(SHOT) > 1000:
            return
        time.sleep(0.2)
    raise RuntimeError("screendump file missing")


def positions():
    """All (x, y) from MOUSE-START, MOUSE reports and MOUSE-END, in order
    (MOUSE-END is the exact final position, free of report lag)."""
    out = []
    with open(SERIAL, errors="replace") as f:
        for line in f:
            m = re.search(r"MOUSE(?:-START|-END)? x=(\d+) y=(\d+)", line)
            if m:
                out.append((int(m.group(1)), int(m.group(2))))
    return out


def cursor_near(x, y, rad=120):
    from PIL import Image
    im = Image.open(SHOT).convert("RGB")
    p = im.load()
    n = 0
    for yy in range(max(0, y - rad), min(752, y + rad), 2):
        for xx in range(max(0, x - rad), min(1024, x + rad), 2):
            if p[xx, yy] == (255, 255, 255):
                n += 1
    return n


def main():
    if not os.path.exists(ISO):
        print("mouse_test: build the ISO first (make iso64)")
        return 1
    qemu = boot()
    try:
        if not wait_for(SERIAL, "mct> ", 180):
            print("mouse_test FAIL: no shell prompt")
            return 1
        serial_early = open(SERIAL, errors="replace").read()
        q = connect_qmp()
        try:
            ok_type = type_line(q, SERIAL, list("run mousedemo") + ["ret"])
            print("type-run:", "ok" if ok_type else "MISS")
            started = wait_for(SERIAL, "MOUSE-START", 60)
            for dx, dy in MOVES:
                q.hmp(f"mouse_move {dx} {dy}")
                time.sleep(3)
            done = wait_for(SERIAL, "MOUSE-DONE", 120)
            shot(q)
            pos = positions()
            serial = open(SERIAL, errors="replace").read()
        finally:
            q.close()
        base = pos[0] if pos else (0, 0)
        last = pos[-1] if pos else (0, 0)
        moved = last[0] - base[0] if pos else 0
        same_y = all(p[1] == base[1] for p in pos) if pos else False
        print(f"positions: base={base} last={last} n={len(pos)}")
        white = cursor_near(last[0], last[1]) if pos else 0
        print(f"white pixels near last pos: {white}")
        checks = [
            ("mouse: PS/2 live" in serial_early, "mouse-live"),
            (ok_type, "typing"),
            (started and base == (511, 383), "start-center"),
            (done, "mouse-done"),
            (moved >= 250, "delta-x>=250"),
            (same_y, "y-steady"),
            (white >= 10, "cursor-visible"),
            ("FATAL" not in serial, "no-FATAL"),
        ]
        rc = 0
        for good, name in checks:
            print(f"  [{'PASS' if good else 'FAIL'}] {name}")
            rc = rc or (not good)
        return rc
    finally:
        qemu.terminate()
        try:
            qemu.wait(timeout=10)
        except subprocess.TimeoutExpired:
            qemu.kill()


if __name__ == "__main__":
    sys.exit(main())
