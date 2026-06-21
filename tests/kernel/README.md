<!--
SPDX-FileCopyrightText: 2026 Alexandre Gomes Gaigalas <alganet@gmail.com>
SPDX-License-Identifier: GPL-3.0-or-later
-->

# Kernel tests — hs on a builder-hex0-arch kernel

These tests validate hs in the environment it is actually designed for: a
minimal kernel that offers little more than raw syscalls. They boot a
[builder-hex0-arch](https://github.com/alganet/builder-hex0-arch) kernel under
QEMU, bootstrap a native stage0 toolchain *inside* it, compile `hs.c` there with
M2-Planet, and run a handful of self-checking hs scripts. The host CI only ever
exercises hs on a full Linux box; this lane catches cross-arch and
minimal-environment regressions that nothing else can.

## How it works

```
host                                    guest (QEMU, builder-hex0-arch kernel)
----                                    --------------------------------------
pack.sh  -> src-protocol image          stage1 -> stage2 shell reads the image:
run.sh   -> dd image, boot QEMU           src ...            load every file into RAM FS
                                          kaem-optional-seed /driver.kaem
                                            driver.kaem: stage0 mini bootstrap
                                              -> native M2-Planet/blood-elf/M1/hex2/kaem
                                            build.<arch>.kaem: compile hs.c (strict)
                                            probe.kaem -> ./hs ./probe.hs
                                              -> run every cases/*.hs, print HS-PROBE END
run.sh   -> read outcomes from serial log NUL -> flush -> reboot (QEMU --no-reboot)
```

This lane is a **discovery tool**, not a green/red gate: it maps which hs
features work on a minimal kernel and which expose kernel incompatibilities.
`probe.hs` runs each `cases/*.hs` as an isolated child and absorbs hard failures
with `||`, so one boot surfaces every outcome and ends with `HS-PROBE END`.

Verdict (`run.sh`): no `HS-PROBE END` -> the boot / stage0 bootstrap / hs build
did not complete (exit 2, a real breakage). Otherwise it lists the per-probe
outcomes and exits non-zero if any `HS-KERNEL-FAIL` (survivable mismatch) or
`HS-PROBE-HARDFAIL` (a case that aborted hs) markers are present. Those findings
are catalogued in [../KERNEL.md](../KERNEL.md); the lane is expected to report
findings until the kernel-side items are fixed.

## Running locally

Requires the matching `qemu-system-*` (`qemu-system-x86_64` /
`qemu-system-riscv64` / `qemu-system-aarch64`) and a `builder-hex0-arch`
checkout (default: a sibling `../builder-hex0-arch`). `stage0-posix-1.9.1/` is
reused if present, else fetched.

```sh
# from the hs repo root
ARCH=x86      sh tests/kernel/run.sh    # board defaults to bios
ARCH=riscv64  sh tests/kernel/run.sh    # board defaults to virt
ARCH=aarch64  sh tests/kernel/run.sh
```

**x86 is the fast lane**: it boots as an MBR under `qemu-system-x86_64` with KVM
when `/dev/kvm` is available, finishing a full bootstrap + build + sweep in about
a minute. The cross arches (riscv64/aarch64) run under QEMU TCG — tens of minutes,
dominated by the in-kernel stage0 bootstrap and the hs.c compile. The same
findings reproduce on all three (32-bit x86 and 64-bit riscv64 confirmed).
Override the QEMU kill timeout with `QEMU_TIMEOUT=<seconds>` and the kernel
checkout with `BUILDER=<path>`.

## Files

| File | Role |
|------|------|
| `run.sh` | host harness: pack, assemble disk image, boot QEMU under `timeout`, read outcomes |
| `pack.sh` | host: emit the builder-hex0 `src`-protocol provisioning stream |
| `driver.<arch>.kaem` | in-kernel: stage0 mini bootstrap, then `build.kaem` (strict) then `probe.kaem` |
| `build.<arch>.kaem` | in-kernel: compile hs.c with the native toolchain |
| `probe.kaem` / `probe.hs` | in-kernel: run every case, absorbing hard failures, print `HS-PROBE END` |
| `cases/*.hs` | self-checking probes (expansion, control flow, arithmetic, param ops, here-docs, big alloc, fork/exec status, cd/pwd, test -e/-f/-d, file I/O, command substitution, pipe, redirection, exec) |

Wired up: x86/bios (32-bit, MBR), riscv64/virt and aarch64/virt (64-bit,
`-kernel`). The cases rely solely on hs builtins (and re-exec hs itself for the
fork/exec cases), since no coreutils exist in-kernel —
which is exactly the portability surface these probes map. New findings: add a
`cases/<name>.hs` (validate it on the host gcc build first), list it in
`probe.hs`, and document any kernel incompatibility it surfaces in
[../KERNEL.md](../KERNEL.md).
