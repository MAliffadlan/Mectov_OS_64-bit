#!/usr/bin/env python3
"""scripts/vga_test.py — framebuffer console gate over QMP screendump.

Boots mectov64.iso with -vga std (KVM if available, else TCG), waits for
the shell prompt, dumps the VGA framebuffer twice (before/after typing
`help`), and asserts on pixels. Exit 0 PASS, nonzero FAIL.

Checks: screen alive (>0.5% non-black pixels), no column-0 cursor strands
(the cons_putc '\\r' regression: at most 1 full-block cell in column 0,
the live cursor itself), typing `help` changes the frame, serial shows
the help text (screen/serial dual sink in sync), no FATAL.
"""
import os
import subprocess
import sys
import time

sys.path.insert(0, os.path.join(os.path.dirname(__file__)))
from qmp import QMP
from kbd_test import has_kvm, wait_for, type_line

ISO = "mectov64.iso"
SERIAL = "serial_vgatest.log"
SOCK = "/tmp/qmp_vgatest"
SHOT1 = "/tmp/shot_vga1.ppm"
SHOT2 = "/tmp/shot_vga2.ppm"

NCOLS, NROWS = 128, 48  # 1024x768 @ 8x16 cells


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


def load_gray(path):
    from PIL import Image
    im = Image.open(path).convert("L")
    if im.size != (1024, 768):
        raise ValueError(f"unexpected shot size {im.size}")
    return im.load()


def fg_ratio(px):
    n = sum(1 for y in range(0, 768, 2) for x in range(0, 1024, 2)
            if px[x, y] > 30)
    return n / (512 * 384)


def col0_blocks(px):
    """Rows whose column-0 cell is a full 8x16 block. Healthy screen: at
    most 1 (the live cursor resting at a line start). The '\\r' strand
    regression painted all 48."""
    bad = []
    for cy in range(NROWS):
        n = sum(1 for dy in range(16) for dx in range(8)
                if px[dx, cy * 16 + dy] > 30)
        if n >= 120:
            bad.append(cy)
    return bad


def frame_diff(a, b):
    n = sum(1 for y in range(0, 768, 3) for x in range(0, 1024, 3)
            if abs(a[x, y] - b[x, y]) > 30)
    return n / (342 * 256)


def main():
    if not os.path.exists(ISO):
        print("vga_test: build the ISO first (make iso64)")
        return 1
    qemu = boot()
    try:
        if not wait_for(SERIAL, "mct> ", 180):
            print("vga_test FAIL: no shell prompt")
            return 1
        q = connect_qmp()
        try:
            time.sleep(3)  # let pending output flush
            shot(q, SHOT1)
            px1 = load_gray(SHOT1)
            alive = fg_ratio(px1)
            print(f"screen alive: {alive * 100:.2f}% non-black")
            strands = col0_blocks(px1)
            print(f"col0 full-block rows: "
                  f"{strands if strands else 'none (live cursor elsewhere)'}")
            ok_type = type_line(q, SERIAL, list("help") + ["ret"])
            print("type-help:", "ok" if ok_type else "MISS")
            helped = wait_for(SERIAL, "help ps run exec", 60)
            time.sleep(3)
            shot(q, SHOT2)
            px2 = load_gray(SHOT2)
            diff = frame_diff(px1, px2)
            print(f"frame change after help: {diff * 100:.2f}%")
            serial = open(SERIAL, errors="replace").read()
        finally:
            q.close()
        checks = [
            (alive > 0.005, "screen-alive"),
            (len(strands) <= 1, "no-col0-strands"),
            (ok_type, "typing"),
            (helped, "help-output-serial"),
            (diff > 0.0005, "frame-changed"),
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
