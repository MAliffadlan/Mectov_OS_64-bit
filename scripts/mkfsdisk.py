#!/usr/bin/env python3
"""scripts/mkfsdisk.py — build the F2 ext2 disk image (deterministic).

Uses mke2fs -d to populate directly from a host tree (no debugfs write
path: its block-bitmap updates proved unreliable — a file's last block
came out unmarked, double-allocating under our allocator). Verifies the
superblock magic and a clean e2fsck pass. No loop mounts, no root.
"""
import os
import shutil
import struct
import subprocess
import sys
import tempfile

SIZE_MB = 16
SIZE_BLOCKS = SIZE_MB * 1024  # 1KiB blocks
LABEL = "MECTOV"
MAGIC = 0xEF53


def run(cmd, **kw):
    r = subprocess.run(cmd, capture_output=True, text=True, **kw)
    if r.returncode != 0:
        print(f"[-] {' '.join(cmd)} failed:\n{r.stderr.strip()}")
        sys.exit(1)
    return r


def main():
    path = sys.argv[1] if len(sys.argv) > 1 else "disk.img"
    for tool in ("mke2fs", "e2fsck"):
        if not shutil.which(tool):
            print(f"[-] host tool missing: {tool} (need e2fsprogs)")
            sys.exit(1)
    path = os.path.abspath(path)
    seed = tempfile.mkdtemp(prefix="mkfsdisk-")
    try:
        with open(os.path.join(seed, "hello.txt"), "wb") as f:
            f.write(b"hello from ext2\n")
        with open(os.path.join(seed, "big.bin"), "wb") as f:
            f.write(bytes((i * 2654435761) & 0xFF for i in range(40960)))
        os.makedirs(os.path.join(seed, "sub", "deep"))
        with open(os.path.join(seed, "sub", "nested.txt"), "wb") as f:
            f.write(b"nested ok\n")
        with open(os.path.join(seed, "sub", "deep", "leaf.bin"), "wb") as f:
            f.write(bytes((i * 2654435761) & 0xFF for i in range(3072)))
        if os.path.exists(path):
            os.unlink(path)
        run(["mke2fs", "-q", "-F", "-t", "ext2", "-b", "1024", "-I", "128",
             "-L", LABEL, "-d", seed, path, str(SIZE_BLOCKS)])
    finally:
        shutil.rmtree(seed, ignore_errors=True)
    with open(path, "rb") as f:
        f.seek(1024)
        sb = f.read(1024)
    magic = struct.unpack_from("<H", sb, 56)[0]
    if magic != MAGIC:
        print(f"[-] superblock magic 0x{magic:04X} != 0x{MAGIC:04X}")
        sys.exit(1)
    # Neuter reserved inodes (1,3-10): mke2fs -d leaves a stale file's
    # metadata in them (ino 7 keeps a triple-indirect chain into live
    # data). By spec they own nothing; stale pointers abort e2fsck and
    # phantom-claim blocks our allocator reuses. Zero the whole 128B
    # slot, and free the one bitmap bit the junk had marked (337).
    with open(path, "r+b") as f:
        f.seek(2048 + 8)
        itable = struct.unpack("<I", f.read(4))[0]
        if not itable:
            print("[-] group 0 inode table is 0")
            sys.exit(1)
        for ino in (1, 3, 4, 5, 6, 7, 8, 9, 10):
            idx = ino - 1
            blk = itable + (idx * 128) // 1024
            off = (idx * 128) % 1024
            f.seek(blk * 1024 + off)
            if f.read(128) != b"\x00" * 128:
                f.seek(blk * 1024 + off)
                f.write(b"\x00" * 128)
                print(f"[...] neutered reserved inode {ino}")
        # Free block 337 (junk-owned, marked): clear its bit + fix counts.
        # NOTE: bitmap bit N owns block N+1 -> block 337 is bit 336.
        bit = (337 - 1) % 8192
        f.seek(66 * 1024 + bit // 8)
        v = struct.unpack("<B", f.read(1))[0]
        if v & (1 << (bit % 8)):
            f.seek(66 * 1024 + bit // 8)
            f.write(struct.pack("<B", v & ~(1 << (bit % 8))))
            for off, size in ((1024 + 12, 4), (2048 + 12, 2)):
                f.seek(off)
                n = int.from_bytes(f.read(size), "little") + 1
                f.seek(off)
                f.write(n.to_bytes(size, "little"))
            print("[...] freed stale junk block 337 (+2 free counts)")
    r = subprocess.run(["e2fsck", "-n", "-f", path], capture_output=True,
                       text=True)
    out = r.stdout + r.stderr
    if r.returncode != 0:
        print(f"[-] seed image not e2fsck-clean (exit {r.returncode}):")
        print("\n".join(out.splitlines()[:15]))
        sys.exit(1)
    print(f"[+] {path}: {SIZE_MB}MB ext2 label={LABEL} clean")
    print("    /hello.txt /big.bin /sub/nested.txt /sub/deep/leaf.bin")


if __name__ == "__main__":
    main()
