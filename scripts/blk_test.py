#!/usr/bin/env python3
"""scripts/blk_test.py — AHCI block gate (F1 proof).

Boots mectov64.iso with -vga std + a raw disk on q35 AHCI (KVM if
available, else TCG) and asserts the kernel finds the controller,
brings the port up, and reads sectors (boot selftest). Read-only as far
as the guest is concerned. Exit 0 PASS, nonzero FAIL.
"""
import os
import subprocess
import sys
import time

sys.path.insert(0, os.path.join(os.path.dirname(__file__)))
from qmp import QMP
from kbd_test import has_kvm, wait_for

ISO = "mectov64.iso"
DISK = "disk.img"
SERIAL = "serial_blktest.log"
SOCK = "/tmp/qmp_blktest"


def ensure_disk():
    if not os.path.exists(DISK):
        with open(DISK, "wb") as f:
            f.write(bytes(16 * 1024 * 1024))


def boot():
    cmd = ["qemu-system-x86_64", "-machine", "q35"]
    if has_kvm():
        cmd += ["-cpu", "host", "-enable-kvm"]
    else:
        cmd += ["-cpu", "qemu64,+nx"]
    cmd += ["-m", "256", "-smp", "4", "-vga", "std", "-cdrom", ISO,
            "-drive", f"file={DISK},format=raw",
            "-serial", f"file:{SERIAL}", "-no-reboot", "-display", "none",
            "-qmp", f"unix:{SOCK},server=on,wait=off"]
    for p in (SOCK, SERIAL):
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


def main():
    if not os.path.exists(ISO):
        print("blk_test: build the ISO first (make iso64)")
        return 1
    ensure_disk()
    qemu = boot()
    try:
        if not wait_for(SERIAL, "mct> ", 180):
            print("blk_test FAIL: no shell prompt")
            return 1
        q = connect_qmp()
        q.close()
        serial = open(SERIAL, errors="replace").read()
        checks = [
            ("[AHCI] controller @ " in serial, "controller-found"),
            ("SATA -> drive 4" in serial, "port-up"),
            ("[AHCI] ready" in serial, "ready"),
            ("selftest read LBA0-1 rc=0" in serial, "sector-read"),
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
