#!/usr/bin/env python3
"""scripts/win_test.py — window server gate (G3 proof).

Boots mectov64.iso with -vga std (KVM if available, else TCG), runs the
`winsrv` Ring-3 window server, and asserts end to end: layout pixels,
terminal echo over the keyboard, title-bar drag via mouse buttons, clean
exit with console restore. Exit 0 PASS, nonzero FAIL.

Note: winsrv never echoes keys to serial (it draws them), so typing here
is open-loop (0.3s spacing; the guest drains promptly) with WIN-READY /
WIN-LINE as the closed-loop confirmation instead.
"""
import os
import subprocess
import sys
import time

sys.path.insert(0, os.path.join(os.path.dirname(__file__)))
from qmp import QMP
from kbd_test import has_kvm, wait_for
from vga_test import load_rgb, fg_ratio, strip_bars_ok

ISO = "mectov64.iso"
SERIAL = "serial_wintest.log"
SOCK = "/tmp/qmp_wintest"
SHOT1 = "/tmp/shot_win1.ppm"
SHOT2 = "/tmp/shot_win2.ppm"
SHOT3 = "/tmp/shot_win3.ppm"

TEAL = (26, 43, 60)
BLUE = (0, 0, 128)
GRAY = (187, 187, 187)


def boot():
    cmd = ["qemu-system-x86_64", "-machine", "q35"]
    if has_kvm():
        cmd += ["-cpu", "host", "-enable-kvm"]
    else:
        cmd += ["-cpu", "qemu64,+nx"]
    cmd += ["-m", "256", "-smp", "4", "-vga", "std", "-cdrom", ISO,
            "-serial", f"file:{SERIAL}", "-no-reboot", "-display", "none",
            "-qmp", f"unix:{SOCK},server=on,wait=off"]
    for p in (SOCK, SERIAL, SHOT1, SHOT2, SHOT3):
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


def shot(q, path):
    r = q.hmp(f"screendump {path}")
    if isinstance(r, dict) and "error" in r:
        raise RuntimeError(f"screendump failed: {r['error']}")
    for _ in range(100):
        if os.path.exists(path) and os.path.getsize(path) > 1000:
            return
        time.sleep(0.2)
    raise RuntimeError("screendump file missing")


def type_open(q, text):
    for k in text:
        q.sendkey("spc" if k == " " else k)
        time.sleep(0.3)


def btn(q, down):
    r = q.cmd("input-send-event", {"events": [{"type": "btn", "data": {
        "down": down, "button": "left"}}]})
    if isinstance(r, dict) and "error" in r:
        raise RuntimeError(f"btn failed: {r['error']}")
    time.sleep(1)


def gray_count(px, x0, y0, x1, y1):
    n = 0
    for y in range(y0, y1, 2):
        for x in range(x0, x1, 2):
            if px[x, y] == GRAY:
                n += 1
    return n


def main():
    if not os.path.exists(ISO):
        print("win_test: build the ISO first (make iso64)")
        return 1
    qemu = boot()
    try:
        if not wait_for(SERIAL, "mct> ", 180):
            print("win_test FAIL: no shell prompt")
            return 1
        q = connect_qmp()
        try:
            ready = False
            for _ in range(2):
                type_open(q, list("run winsrv") + ["ret"])
                if wait_for(SERIAL, "WIN-READY 1024x768", 40):
                    ready = True
                    break
            print("server:", "ready" if ready else "MISS")
            if not ready:
                return 1
            time.sleep(2)
            shot(q, SHOT1)
            px = load_rgb(SHOT1)
            layout = (px[50, 700] == TEAL and px[300, 155] == BLUE and
                      px[300, 300] == (0, 0, 0) and
                      gray_count(px, 194, 170, 830, 190) > 50)
            print(f"layout: {layout}")
            type_open(q, list("hi") + ["ret"])
            echoed = wait_for(SERIAL, "WIN-LINE hi", 30)
            # Drag: title starts at (192,150); mouse at (511,383).
            q.hmp("mouse_move 1 -223")
            time.sleep(2)
            btn(q, True)
            q.hmp("mouse_move 100 50")
            time.sleep(2)
            btn(q, False)
            dragged = wait_for(SERIAL, "WIN-DRAG 292,200", 30)
            shot(q, SHOT2)
            px2 = load_rgb(SHOT2)
            moved = (px2[400, 205] == BLUE and px2[300, 155] == TEAL)
            print(f"dragged: {dragged}, pixels: {moved}")
            type_open(q, list("exit") + ["ret"])
            exited = wait_for(SERIAL, "WIN-EXIT", 30)
            time.sleep(2)
            shot(q, SHOT3)
            px3 = load_rgb(SHOT3)
            alive = fg_ratio(px3)
            bars = strip_bars_ok(px3)
            print(f"restored: fg={alive * 100:.2f}% strip={bars}")
            serial = open(SERIAL, errors="replace").read()
        finally:
            q.close()
        checks = [
            (ready, "server-ready"),
            (layout, "layout"),
            (echoed, "terminal-echo"),
            (dragged and moved, "drag"),
            (exited, "clean-exit"),
            (alive > 0.005 and bars, "console-restored"),
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
