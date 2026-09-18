#!/usr/bin/env python3
"""scripts/mkfsdisk.py — build the F2 ext2 disk image (deterministic).

Creates disk.img (16MB, zeroed), formats ext2 (1KiB blocks, label MECTOV),
seeds a fixed tree via debugfs, verifies the superblock magic. No loop
mounts, no root needed. Exit nonzero with a clear message when the host
lacks mkfs.ext2/debugfs.
"""
import os
import shutil
import struct
import subprocess
import sys
import tempfile

SIZE_MB = 16
BLOCK = 1024
LABEL = "MECTOV"
MAGIC = 0xEF53

TEXT_FILES = {
    "/hello.txt": b"hello from ext2\n",
    "/sub/nested.txt": b"nested ok\n",
    "/sub/deep/leaf.bin": b"0123456789abcdef" * 192,  # 3KiB
}
BIG_BIN = ("/big.bin", 40960)


def run(cmd, **kw):
    r = subprocess.run(cmd, capture_output=True, text=True, **kw)
    if r.returncode != 0:
        print(f"[-] {' '.join(cmd)} failed:\n{r.stderr.strip()}")
        sys.exit(1)
    return r


def main():
    path = sys.argv[1] if len(sys.argv) > 1 else "disk.img"
    for tool in ("mkfs.ext2", "debugfs"):
        if not shutil.which(tool):
            print(f"[-] host tool missing: {tool} (need e2fsprogs)")
            sys.exit(1)
    path = os.path.abspath(path)
    with open(path, "wb") as f:
        f.write(bytes(SIZE_MB * 1024 * 1024))
    run(["mkfs.ext2", "-q", "-F", "-t", "ext2", "-b", str(BLOCK), "-I",
         "128", "-L", LABEL, path, str(SIZE_MB * 1024)])
    tmp = tempfile.mkdtemp(prefix="mkfsdisk-")
    try:
        cmds = ["mkdir /sub", "mkdir /sub/deep"]
        for name, data in TEXT_FILES.items():
            src = os.path.join(tmp, name.replace("/", "_"))
            with open(src, "wb") as f:
                f.write(data)
            cmds.append(f"write {src} {name}")
        bigname, bigsize = BIG_BIN
        src = os.path.join(tmp, "big.bin")
        with open(src, "wb") as f:
            f.write(bytes((i * 2654435761) & 0xFF for i in range(bigsize)))
        cmds.append(f"write {src} {bigname}")
        for c in cmds:
            run(["debugfs", "-w", "-R", c, path], cwd=tmp)
    finally:
        shutil.rmtree(tmp, ignore_errors=True)
    with open(path, "rb") as f:
        f.seek(1024)
        sb = f.read(1024)
    magic = struct.unpack_from("<H", sb, 56)[0]
    if magic != MAGIC:
        print(f"[-] superblock magic 0x{magic:04X} != 0x{MAGIC:04X}")
        sys.exit(1)
    print(f"[+] {path}: {SIZE_MB}MB ext2 label={LABEL} "
          f"blocks={BLOCK}B seeded")
    for name in list(TEXT_FILES) + [BIG_BIN[0]]:
        print(f"    {name}")


if __name__ == "__main__":
    main()
