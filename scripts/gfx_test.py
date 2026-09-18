#!/usr/bin/env python3
"""scripts/gfx_test.py — userspace graphics ABI gate (G2 proof).

Boots mectov64.iso with -vga std (KVM if available, else TCG), runs the
`gfxdemo` Ring-3 program (FB_INFO + FB_MAP + umalloc + direct pixels),
and asserts on pixels: gradient formula, border, RGB squares, sprite,
then console restore after FB_UNMAP. Exit 0 PASS, nonzero FAIL.
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
SERIAL = "serial_gfxtest.log"
SOCK = "/tmp/qmp_gfxtest"
SHOT1 = "/tmp/shot_gfx1.ppm"
SHOT2 = "/tmp/shot_gfx2.ppm"


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


def grad(x, y):
    return ((x * 255 // 1024) << 16) | ((y * 255 // 768) << 8) | 0x80


def rgb(px, x, y):
    r, g, b = px[x, y]
    return (r << 16) | (g << 8) | b


def main():
    if not os.path.exists(ISO):
        print("gfx_test: build the ISO first (make iso64)")
        return 1
    qemu = boot()
    try:
        if not wait_for(SERIAL, "mct> ", 180):
            print("gfx_test FAIL: no shell prompt")
            return 1
        q = connect_qmp()
        try:
            ok_type = type_line(q, SERIAL, list("run gfxdemo") + ["ret"])
            print("type-run:", "ok" if ok_type else "MISS")
            done = wait_for(SERIAL, "GFX-DONE 1024x768", 60)
            time.sleep(2)
            shot(q, SHOT1)
            px = load_rgb(SHOT1)
            border = all(rgb(px, x, y) == 0xFFFFFF
                         for x, y in [(2, 2), (1021, 2), (2, 765),
                                       (1021, 765)])
            squares = (rgb(px, 150, 150) == 0xFF0000 and
                       rgb(px, 300, 150) == 0x00FF00 and
                       rgb(px, 450, 150) == 0x0000FF)
            gradient = all(rgb(px, x, y) == grad(x, y)
                           for x, y in [(600, 400), (50, 700), (900, 50)])
            sprite = (rgb(px, 700, 500) == 0x000000 and
                      rgb(px, 712, 500) == 0xFFFFFF)
            print(f"art: border={border} squares={squares} "
                  f"gradient={gradient} sprite={sprite}")
            unmapped = wait_for(SERIAL, "GFX-UNMAP-OK", 60)
            time.sleep(2)
            shot(q, SHOT2)
            px2 = load_rgb(SHOT2)
            alive = fg_ratio(px2)
            bars = strip_bars_ok(px2)
            print(f"restored: fg={alive * 100:.2f}% strip={bars}")
            serial = open(SERIAL, errors="replace").read()
        finally:
            q.close()
        bad = [m for m in ("GFX-NOFB", "GFX-MAP-FAIL", "GFX-ALLOC-FAIL",
                           "GFX-UNMAP-FAIL") if m in serial]
        checks = [
            (ok_type, "typing"),
            (done, "gfx-done"),
            (border, "border"),
            (squares, "squares"),
            (gradient, "gradient"),
            (sprite, "sprite-umalloc"),
            (unmapped, "unmap-ok"),
            (alive > 0.005 and bars, "console-restored"),
            (not bad, f"no-gfx-fail{bad}"),
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
