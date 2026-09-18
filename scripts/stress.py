#!/usr/bin/env python3
"""scripts/stress.py — flake hunt: run the ISO under KVM N times, collect
FATAL signatures + marker counts per run. Exit 0 iff every run is clean
(no FATAL/FAIL) even when markers vary by timing.

Usage: python3 scripts/stress.py [N=6] [timeout_s=100]
"""
import os
import re
import subprocess
import sys
import time

N = int(sys.argv[1]) if len(sys.argv) > 1 else 6
TIMEOUT = int(sys.argv[2]) if len(sys.argv) > 2 else 100
Serial = re.compile(r"serial_stress(\d*)\.log")


def run_one(i):
    log = f"serial_stress{i}.log"
    for f in (log,):
        try:
            os.unlink(f)
        except OSError:
            pass
    cmd = ["qemu-system-x86_64", "-machine", "q35", "-cpu", "host",
           "-enable-kvm", "-m", "256", "-smp", "4", "-cdrom", "mectov64.iso",
           "-serial", f"file:{log}", "-no-reboot", "-display", "none"]
    t0 = time.time()
    try:
        subprocess.run(cmd, timeout=TIMEOUT, stdout=subprocess.DEVNULL,
                       stderr=subprocess.DEVNULL)
        why = f"exit-early({time.time()-t0:.0f}s)"
    except subprocess.TimeoutExpired:
        why = "timeout-alive"
    try:
        data = open(log, errors="replace").read()
    except OSError:
        data = ""
    fatals = re.findall(r"FATAL (?:PF|EXC) (?:vec=\d+ )?err=(0x[0-9a-f]+) addr=(0x[0-9a-f]+) rip=(0x[0-9a-f]+)", data)
    marks = len(re.findall(r"BRK-DONE|NX-DONE|SMP-DONE|FORK-DONE|HELLO-DONE|CLONE-DONE|FPU-DONE|SHELLTEST-DONE|ARG-DONE", data))
    ticks = len(re.findall(r"\[K64\] tick ", data))
    return why, fatals, marks, ticks


def main():
    if not os.path.exists("mectov64.iso"):
        print("build the ISO first (make iso64)")
        return 1
    bad = 0
    for i in range(N):
        why, fatals, marks, ticks = run_one(i)
        flag = ""
        if fatals:
            flag = " FAULT!"
            bad += 1
        print(f"run{i}: {why} markers={marks} ticks={ticks}{flag}", flush=True)
        for f in fatals[:3]:
            print(f"   FATAL err={f[0]} addr={f[1]} rip={f[2]}", flush=True)
    print(f"done: {N-bad}/{N} clean")
    return 1 if bad else 0


if __name__ == "__main__":
    sys.exit(main())
