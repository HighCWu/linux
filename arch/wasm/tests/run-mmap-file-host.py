#!/usr/bin/env python3
# SPDX-License-Identifier: MIT
"""Exercise the actual candidate C with host shims, not a Linux/Wasm boot."""
import os
from pathlib import Path
import re
import resource
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
def no_core():
    resource.setrlimit(resource.RLIMIT_CORE, (0, 0))

with tempfile.TemporaryDirectory(prefix="wasm-file-mmap-host-") as directory:
    for bits in (32, 64):
        output = str(Path(directory) / f"checks-{bits}")
        command = [compiler, "-xc", "-std=gnu11", "-O2", "-Wall", "-Wextra", "-Werror",
                   "-Wno-sign-compare", "-DCONFIG_WASM_FILE_MMAP=1",
                   "-DCONFIG_WASM_MMAP_COPY_TEST=1",
                   *(["-DCONFIG_32BIT=1"] if bits == 32 else []), "-o", output, "-"]
        subprocess.run(
            command,
            input=shim + "\n" + source + "\n" + checks, text=True, check=True,
        )
        subprocess.run([output], check=True, timeout=15)
        # Restoring the former exact-flags gate must reject the hint cases.
        gate = "(flags & ~(MAP_PRIVATE | advisory_flags))"
        assert source.count(gate) == 1
        negative = source.replace(gate, "((void)advisory_flags, flags != MAP_PRIVATE)")
        subprocess.run(command, input=shim + "\n" + negative + "\n" + checks,
                       text=True, check=True)
        result = subprocess.run([output], capture_output=True, text=True,
                                preexec_fn=no_core, timeout=15)
        assert result.returncode != 0 and "== 65536" in result.stderr, result.stderr
        # Bypass only fatal admission; retain references so this compiles
        # under the same warnings as the positive kernel-function model.
        guard = "fatal_signal_pending(current)"
        assert source.count(guard) == 2
        for index in (0, 1):
            before, after = source.split(guard, 1) if index == 0 else source.rsplit(guard, 1)
            negative = before + "(false && fatal_signal_pending(current))" + after
            subprocess.run(command, input=shim + "\n" + negative + "\n" + checks,
                           text=True, check=True)
            result = subprocess.run([output], capture_output=True, text=True,
                                    preexec_fn=no_core, timeout=15)
            expected = "reads == reads_before" if index == 0 else "-EINTR"
            assert result.returncode != 0 and expected in result.stderr, result.stderr
        print(f"host shims: mmap{'2 units' if bits == 32 else ' bytes'} passed", flush=True)
