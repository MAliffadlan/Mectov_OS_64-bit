#!/usr/bin/env python3
"""scripts/fs_test.py — ext2 read gate (F2a proof).

Boots mectov64.iso with -vga std + the ext2 disk (KVM if available, else
TCG) and asserts: kernel mount + data-path selftest (hello content,
indirect file, missing entry) and the userspace READDIR path via the
boot-spawned fsdemo. Exit 0 PASS, nonzero FAIL.
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
SERIAL = "serial_fstest.log"
SOCK = "/tmp/qmp_fstest"


def ensure_disk():
    if os.path.exists(DISK):
        return
    r = subprocess.run([sys.executable, "scripts/mkfsdisk.py", DISK])
    if r.returncode != 0:
        print("fs_test FAIL: cannot build disk image (need mkfs.ext2)")
        sys.exit(1)


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
        print("fs_test: build the ISO first (make iso64)")
        return 1
    ensure_disk()
    qemu = boot()
    try:
        if not wait_for(SERIAL, "mct> ", 180):
            print("fs_test FAIL: no shell prompt")
            return 1
        q = connect_qmp()
        q.close()
        serial = open(SERIAL, errors="replace").read()
        checks = [
            ("ext2 mounted drive=" in serial, "mount"),
            ("selftest hello OK" in serial, "hello-data"),
            ("selftest bigbin OK" in serial, "indirect-data"),
            ("selftest enoent OK" in serial, "enoent"),
            ("FSDEMO-LIST-OK" in serial, "userspace-list"),
            ("MISMATCH" not in serial, "no-mismatch"),
            ("NODISK" not in serial, "disk-present"),
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
