#!/bin/sh
# SPDX-License-Identifier: GPL-2.0
#
# Multi-pass wasm-ld link for vmlinux.o with CONFIG_KALLSYMS=y.
# wasm-ld links vmlinux.o directly (without -r), so kallsyms tables must be
# converged and linked into vmlinux.o before arch/wasm/Makefile converts it to
# vmlinux.wasm.

set -e

if [ $# -lt 2 ]; then
	echo "Usage: $0 <output> <ld-args...>" >&2
	exit 1
fi

output="$1"
shift

srctree="${srctree:-.}"
PERL="${PERL:-perl}"
KALLSYMS="./scripts/kallsyms"

ksym_all=""
if grep -q "^CONFIG_KALLSYMS_ALL=y" include/config/auto.conf 2>/dev/null; then
	ksym_all="--all-symbols"
fi

cleanup() {
	rm -f .tmp_vmlinux1.o .tmp_vmlinux1.o.map .tmp_vmlinux1.syms \
		.tmp_vmlinux2.o .tmp_vmlinux2.o.map .tmp_vmlinux2.syms \
		.tmp_kallsyms0.S .tmp_kallsyms0.o \
		.tmp_kallsyms1.S .tmp_kallsyms1.o \
		.tmp_kallsyms2.S .tmp_kallsyms2.o \
		"${output}.map"
}
trap cleanup EXIT

wasm_mksysmap() {
	wasm2wat "$1" --enable-all \
		| ${PERL} "${srctree}/arch/wasm/scripts/wasm-mksysmap.pl" "$2" \
		| sed -f "${srctree}/scripts/mksysmap" > "$3"
}

gen_kallsyms() {
	${KALLSYMS} ${ksym_all} --wasm "$1" > "$2"
	${CC} ${KBUILD_CPPFLAGS} ${KBUILD_AFLAGS} -c -o "$3" "$2"
}

# Pass 0: empty kallsyms tables to satisfy initial symbol references.
${KALLSYMS} --wasm /dev/null > .tmp_kallsyms0.S
${CC} ${KBUILD_CPPFLAGS} ${KBUILD_AFLAGS} -c -o .tmp_kallsyms0.o .tmp_kallsyms0.S

# Pass 1: link with empty kallsyms, extract symbols, build kallsyms1.
${LD} ${KBUILD_LDFLAGS} --no-gc-sections -Map=.tmp_vmlinux1.o.map \
	-o .tmp_vmlinux1.o "$@" .tmp_kallsyms0.o
wasm_mksysmap .tmp_vmlinux1.o .tmp_vmlinux1.o.map .tmp_vmlinux1.syms
gen_kallsyms .tmp_vmlinux1.syms .tmp_kallsyms1.S .tmp_kallsyms1.o

# Pass 2: link with kallsyms1 at the end to stabilize offsets, build kallsyms2.
${LD} ${KBUILD_LDFLAGS} --no-gc-sections -Map=.tmp_vmlinux2.o.map \
	-o .tmp_vmlinux2.o "$@" .tmp_kallsyms1.o
wasm_mksysmap .tmp_vmlinux2.o .tmp_vmlinux2.o.map .tmp_vmlinux2.syms
gen_kallsyms .tmp_vmlinux2.syms .tmp_kallsyms2.S .tmp_kallsyms2.o

# Final link with converged kallsyms2 and generate System.map.
${LD} ${KBUILD_LDFLAGS} --no-gc-sections -Map="${output}.map" \
	-o "${output}" "$@" .tmp_kallsyms2.o
wasm_mksysmap "${output}" "${output}.map" System.map
