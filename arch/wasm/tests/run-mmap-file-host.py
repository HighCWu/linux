#!/usr/bin/env python3
# SPDX-License-Identifier: MIT
"""Exercise the actual candidate C with host shims, not a Linux/Wasm boot."""
import os
from pathlib import Path
import re
import subprocess
import tempfile

root = Path(__file__).resolve().parent
source = (root.parent / "kernel/mmap_file.c").read_text()
assert "long wasm_mmap_file(" in source and "long wasm_mmap_copy_file(" in source
# Only includes are replaced. Validation, VFS loop, cleanup and atomic reservation
# bodies remain the actual kernel source, compiled for both syscall unit modes.
source = re.sub(r"^#include[^\n]*\n", "", source, flags=re.M)
shim = (root / "mmap-file-host.h").read_text()
checks = (root / "mmap-file-host.c").read_text()
compiler = os.environ.get("CC", "cc")
with tempfile.TemporaryDirectory(prefix="wasm-file-mmap-host-") as directory:
    for bits in (32, 64):
        output = str(Path(directory) / f"checks-{bits}")
        subprocess.run(
            [compiler, "-xc", "-std=gnu11", "-O2", "-Wall", "-Wextra", "-Werror",
             "-Wno-sign-compare", "-DCONFIG_WASM_FILE_MMAP=1",
             *(["-DCONFIG_32BIT=1"] if bits == 32 else []), "-o", output, "-"],
            input=shim + "\n" + source + "\n" + checks, text=True, check=True,
        )
        subprocess.run([output], check=True, timeout=15)
        print(f"host shims: mmap{'2 units' if bits == 32 else ' bytes'} passed", flush=True)
