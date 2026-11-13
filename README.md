# openkal-uefi

An implementation of [openkal][kal] on UEFI Boot Services, for applications the
firmware loads before an operating system exists.

A program names the target and nothing else. The row `x86_64-uefi`, its compile
and link conventions and its runner are the openkal platform's
([openkal-platform][plat] 0.28.0): the same C library and C++ run-time as on
every other system, openkal-musl and openkal-llvm-runtime, with this
implementation beneath them, so a UEFI application is written in C++23 with
`import std`:

```toml
[target.x86_64-uefi]
platform = "openkal-platform@0.28.0"
```

```sh
mcpp build --target x86_64-uefi      # hello.efi: PE32+, subsystem EFI application
mcpp run   --target x86_64-uefi      # booted by dirun under QEMU with xim's OVMF
```

## Two arrangements

| | entry | initialisers | `main` |
|---|---|---|---|
| beneath a C library (`standalone`, 0.10.0) | `efi_main` records the firmware's tables and the arguments | the C library's (`.ctors`, through `__libc_start_main`) | called by the C library |
| alone (no C library) | `efi_main` | read from the image's own `.ctors` and `.CRT` sections | called here |

openkal-musl selects `standalone` on `cfg(os = "uefi")`. A program that wants no
C library states `c-abi = false` for the row and calls openkal directly.

## What a program receives

The firmware calls `efi_main`; this implementation records its tables, turns off
the boot manager's watchdog, runs the program's static initialisers (read from
the image's own `.ctors` and `.CRT` sections, there being no C runtime) and calls
`main(argc, argv, envp)`. The arguments are the image's load options split at
spaces; firmware that gave none gives one empty name.

| openkal | UEFI |
|---|---|
| `stream` | `SIMPLE_TEXT_OUTPUT_PROTOCOL` (UCS-2, `\n` as CR LF); input reports its end |
| `memory` | `AllocatePool` / `FreePool`, over-allocated for a stricter alignment |
| `env` | the load options; no variables, image, loader or system name (absent) |
| `time` | `GetTime` for the wall clock; the processor's counter, measured against `Stall`, for the monotonic one; `Stall` for a sleep |
| `abort` | `Exit` with the status as a warning code, so a starter reads the number |

`process` waits for `fs` over the firmware's file system protocol, since a
program is started by name through a directory; the specification's property for
an environment with one task (`KAL_PROCESS_PROP_RUNS_TO_END`, openkal 0.25.1
clause 7.19) is what it will claim. The rest have no firmware service beneath them.

## The board's exit

A status leaves an application through `Exit`, to whoever started it. A board on
which it must leave the machine --- an emulator running it as a test --- defines
`okuefi_board_exit(int)` in its platform package, and `kal_exit` calls it first;
the definition here does nothing. openkal-platform's (board/qemu-exit.S) writes the status to
QEMU's debug-exit port, only when the processor names QEMU as its hypervisor.

## The table offsets are checked, not trusted

These structures are ABI. A field one slot out reads a neighbouring function
pointer, which calls something that exists with the wrong arguments. Every member
this implementation reaches is pinned by a `static_assert` on its offset.

## Interfaces (0.10.0)

`[kernel-abi] provides-interfaces` states `openkal.abort`, `openkal.env`,
`openkal.exec`, `openkal.memory`, `openkal.stream` and `openkal.time` (0.9.0
stated the list as a key of `[package]`, where the engine does not read it).
`openkal.exec` is pages of type `EfiLoaderCode`, protected through the memory
attribute protocol where the firmware offers it: what a loader of images (the
ok-image runner carried as a UEFI application) places code in. A file cannot be
mapped (no `openkal.fs`), so `kal_exec_map_part` answers not supported.

## Verified

`examples/hello`, built with mcpp 2026.11.12.1 and booted by dirun as
`EFI/BOOT/BOOTX64.EFI` under QEMU's q35 with the edk2 firmware (0.45 s, KVM):
a static initialiser ran, the arguments are counted, an over-aligned allocation,
a 20 ms sleep measured as such, the wall clock reads the present; status 42
returned through the board's exit.

0.10.0 beneath openkal-musl 0.29.6 and openkal-llvm-runtime 0.20.3 (mcpp
2026.11.13.1, `--release`): a program of `import std` that prints with
`std::println` and sorts with `std::ranges::sort` is a 1.2 MB EFI application;
under OVMF it prints, counts one argument (the empty name) and returns 42 in
0.8 s.

[kal]: https://github.com/mcpplibs/openkal
[plat]: https://github.com/mcpplibs/openkal-platform
