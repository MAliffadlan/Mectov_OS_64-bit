#!/usr/bin/env python3
"""scripts/fs_test.py — ext2 read/write gate (F2a+F2b proof).

Boots mectov64.iso with -vga std + the ext2 disk (KVM if available, else
TCG) and asserts: kernel mount + data-path selftest (hello content,
indirect file, missing entry), the userspace READDIR path via the
boot-spawned fsdemo, the fswrite path, and a post-mortem host e2fsck
(exit 0: guest writes leave a reference-clean filesystem).
Exit 0 PASS, nonzero FAIL.
"""
import os
import shutil
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
        # Let the boot-spawned fswrite finish (killing mid-write tears
        # allocator state and proves nothing about consistency).
        fswrite = wait_for(SERIAL, "FSWRITE-DONE", 180)
        serial = open(SERIAL, errors="replace").read()
        checks = [
            ("ext2 mounted drive=" in serial, "mount"),
            ("selftest hello OK" in serial, "hello-data"),
            ("selftest bigbin OK" in serial, "indirect-data"),
            ("selftest enoent OK" in serial, "enoent"),
            ("FSDEMO-LIST-OK" in serial, "userspace-list"),
            (fswrite, "write-path"),
            ("MISMATCH" not in serial, "no-mismatch"),
            ("NODISK" not in serial, "disk-present"),
            ("FATAL" not in serial, "no-FATAL"),
        ]
        rc = 0
        for good, name in checks:
            print(f"  [{'PASS' if good else 'FAIL'}] {name}")
            rc = rc or (not good)
        serial_rc = rc
    finally:
        qemu.terminate()
        try:
            qemu.wait(timeout=10)
        except subprocess.TimeoutExpired:
            qemu.kill()
    # Post-mortem host verdict: the guest's writes must leave a
    # reference-clean filesystem (exit 0). QEMU is down, disk quiesced.
    if serial_rc == 0:
        if not shutil.which("e2fsck"):
            print("  [FAIL] host-clean (e2fsck missing)")
            return 1
        r = subprocess.run(["e2fsck", "-n", "-f", DISK], capture_output=True,
                           text=True)
        good = r.returncode == 0
        print(f"  [{'PASS' if good else 'FAIL'}] host-clean")
        if not good:
            print((r.stdout + r.stderr).strip().splitlines()[:8])
            return 1
        return 0
    return serial_rc


if __name__ == "__main__":
    sys.exit(main())
