#!/usr/bin/env python3
"""scripts/gui_test.py — shell integration gate (G4 proof).

Boots mectov64.iso with -vga std (KVM if available, else TCG), enters
graphics mode with the shell `gui` builtin, verifies the GUI screen,
exits back to the text prompt, and verifies the console restore.
Exit 0 PASS, nonzero FAIL.
"""
import os
import subprocess
import sys
import time

sys.path.insert(0, os.path.join(os.path.dirname(__file__)))
from qmp import QMP
from kbd_test import has_kvm, wait_for, type_line
from vga_test import load_rgb, fg_ratio, strip_bars_ok

ISO = "mectov64.iso"
SERIAL = "serial_guitest.log"
SOCK = "/tmp/qmp_guitest"
SHOT1 = "/tmp/shot_gui1.ppm"
SHOT2 = "/tmp/shot_gui2.ppm"


def boot():
    cmd = ["qemu-system-x86_64", "-machine", "q35"]
    if has_kvm():
        cmd += ["-cpu", "host", "-enable-kvm"]
    else:
        cmd += ["-cpu", "qemu64,+nx"]
    cmd += ["-m", "256", "-smp", "4", "-vga", "std", "-cdrom", ISO,
            "-serial", f"file:{SERIAL}", "-no-reboot", "-display", "none",
            "-qmp", f"unix:{SOCK},server=on,wait=off"]
    for p in (SOCK, SERIAL, SHOT1, SHOT2):
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
        q.sendkey(k)
        time.sleep(0.3)


def main():
    if not os.path.exists(ISO):
        print("gui_test: build the ISO first (make iso64)")
        return 1
    qemu = boot()
    try:
        if not wait_for(SERIAL, "mct> ", 180):
            print("gui_test FAIL: no shell prompt")
            return 1
        q = connect_qmp()
        try:
            ok_type = type_line(q, SERIAL, list("gui") + ["ret"])
            print("type-gui:", "ok" if ok_type else "MISS")
            entered = wait_for(SERIAL, "entering GUI", 30)
            ready = wait_for(SERIAL, "WIN-READY 1024x768", 60)
            time.sleep(2)
            shot(q, SHOT1)
            px = load_rgb(SHOT1)
            gui = (px[50, 700] == (26, 43, 60) and
                   px[300, 155] == (0, 0, 128))
            print(f"gui screen: {gui}")
            # winsrv draws keys instead of echoing: open-loop exit.
            type_open(q, list("exit") + ["ret"])
            exited = wait_for(SERIAL, "WIN-EXIT", 30)
            if not exited:
                type_open(q, ["backspace"] * 5 + list("exit") + ["ret"])
                exited = wait_for(SERIAL, "WIN-EXIT", 30)
            back = wait_for(SERIAL, "reaped", 30)
            time.sleep(2)
            shot(q, SHOT2)
            px2 = load_rgb(SHOT2)
            alive = fg_ratio(px2)
            bars = strip_bars_ok(px2)
            print(f"restored: fg={alive * 100:.2f}% strip={bars}")
            serial = open(SERIAL, errors="replace").read()
        finally:
            q.close()
        checks = [
            (ok_type, "typing"),
            (entered and ready, "gui-enter"),
            (gui, "gui-screen"),
            (exited and back, "gui-exit"),
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
