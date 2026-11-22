# openkal-uefi

An implementation of [openkal][kal] on UEFI Boot Services, for applications the
firmware loads before an operating system exists.

A program names the target and nothing else. The row `x86_64-uefi`, its compile
and link conventions and its runner are the openkal platform's
([openkal-platform][plat], from 0.28.0; 0.32.0 holds this release): the same C
library and C++ run-time as on every other system, openkal-musl and
openkal-llvm-runtime, with this implementation beneath them, so a UEFI
application is written in C++23 with `import std`:

```toml
[target.x86_64-uefi]
platform = "openkal-platform@0.32.0"
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
`main(argc, argv, envp)`.

The arguments (0.11.0) are the UEFI Shell's where the Shell started the program:
it installs `EFI_SHELL_PARAMETERS_PROTOCOL` on the image handle with the command
line already divided, its quotes and its escape character `^` removed, and each
argument is made UTF-8. Started otherwise, the arguments are the image's load
options divided by the same rules: spaces and tabs separate, a double quote opens
or closes a part in which they do not, `^` takes the next character as it is.
0.10.0 divided the load options at spaces alone, so `"a b"` arrived as two
arguments with their quotes. Firmware that gave none gives one empty name.

From 0.11.2 the implementation is compiled with `-fno-builtin`. On the release
profile the compiler replaced the loop that measures a UTF-16 name by a call of
`wcslen`, which the C library above defines for a `wchar_t` of thirty-two bits:
a listing gave `f2.` for `f2.txt`, and the firmware refused a rename
(`EFI_ACCESS_DENIED`). The C library example runs on both profiles in CI.

From 0.11.1 the arguments after the name are divided from the load options where
the Shell started the program too. The Shell of edk2 9.2.4 was measured to hand
over a shortened argument where it has ten or more characters and a run of three
or more blanks (`"two   words"` arrives without its end), and the load options of
the same image hold the line as written. The Shell's division is what remains
where the load options are not text or divide into another number of arguments;
the name is the Shell's in both cases.

0.12.0 is written against openkal 0.26, and nothing but the specification's
version changes. What 0.26 requires is of `openkal.process`, which this
implementation did not provide until 0.13.0: a start bounded by the environment
alone (clause 7.6) and a unit claimed only where a reference can be made
(clause 7.18). The side of clause 7.6 that is here already holds: the arguments a
program receives are kept in pool memory of their measured size, whether the
Shell divided them or the load options are divided here, so no table bounds
their number or their length.

| openkal | UEFI |
|---|---|
| `stream` | `SIMPLE_TEXT_OUTPUT_PROTOCOL` (UCS-2, `\n` as CR LF); input reports its end |
| `memory` | `AllocatePool` / `FreePool`, over-allocated for an alignment below a page; pages for a page or more, placed at the alignment from the memory map (0.10.0) |
| `env` | the load options divided with quotes, the name from the Shell's `EFI_SHELL_PARAMETERS_PROTOCOL` (0.11.1); the variables a starter of this implementation gave (0.13.0), none otherwise; no image, loader or system name (absent) |
| `process` | `LoadImage` and `StartImage` of a program found through `fs`, run to its end before the start returns (0.13.0) |
| `fs` | `SIMPLE_FILE_SYSTEM_PROTOCOL` of the volume the image was loaded from (`LoadedImage->DeviceHandle`), its root the one directory supplied, named `/`; `FILE_PROTOCOL` for opening, transfers, `GetInfo`/`SetInfo` (size, time, rename), `Delete`, and a directory's reads for a listing (0.11.0) |
| `random` | `RNG_PROTOCOL` (`LocateProtocol`, the source's default algorithm); firmware without one answers `kal_err_not_supported` (0.11.0) |
| `time` | `GetTime` for the wall clock; the processor's counter, measured against `Stall`, for the monotonic one; `Stall` for a sleep |
| `abort` | `Exit` with the status as a warning code, so a starter reads the number |

## Starting a program (0.13.0)

`openkal.process` is provided. A program found by name beneath a directory
(`openkal.fs`) is loaded by `LoadImage`, from the device path of the volume with
a media file path node naming it, and run by `StartImage`, which returns when
the started image calls `Exit`. The firmware has one task, so
`kal_process_spawn` returns after the started program has ended and
`kal_process_wait` answers its status at once (`KAL_PROCESS_PROP_RUNS_TO_END`,
openkal clause 7.19); `TERMINATE` and `CHANNEL` are not claimed, and
`BOUND_LIFETIME` is, a started program being unable to outlive its starter.
`EXIT_STATUS` and `ONE_STRING` are claimed.

| what a start gives | how it reaches the started image |
|---|---|
| the vector | its load options: each element that holds a blank, a double quote or `^`, or is empty, is quoted, its `"` and `^` preceded by `^`, so the division above recovers it; `KAL_SPAWN_ONE_STRING` joins the elements after the first as they are |
| the variables | the start record, installed on the started image's handle under a GUID of this implementation before `StartImage` and removed after it; a program of this implementation reads its variables there, one of another kind receives its load options alone |
| the directory it runs in | the start record: the started program's first preopen, named by its path from the root (`/okp`), with the root after it |
| the resource preference | `KAL_RESOURCES` among the variables, which `kal_env_resources` reads |
| its status | `Exit`'s status: a program of this implementation ends with its status as a warning code; an abort is `EFI_ABORTED`, reported as an end the program did not choose, with 134 |

The three streams are the firmware's consoles, which `StartImage` gives every
image: a stream placed at another position and the null device are refused with
`kal_err_not_supported`, as are grants, a unit and confinement, none of which
is claimed. A start that cannot happen is refused by its reason before the
firmware is asked: a name that is not there, a directory, a file that is not an
image (`EFI_LOAD_ERROR`, `kal_err_not_program`), an entry without `=`.

Nothing bounds a start but the firmware's load options, whose size is thirty-two
bits (clause 7.6): the command line, the record and the table of started
programs are sized by what a start gives.

AT ITS END A PROGRAM RETURNS WHAT IT HOLDS. The firmware frees an application's
image when it exits and nothing the application allocated, so every program's
pool and pages stayed taken until the machine restarted: measured with 1 GiB, a
program that started copies of itself which each reserved 128 MiB, as the C
library's allocator reserves an arena, could start six. Every block of the pool
is now preceded by its link in a list and every range of pages is a row of a
table; `kal_exit` and `kal_abort` close the files and directories still open and
return both, and twenty such starts succeed. Pages of code are made writable
and not executable again before they are returned. The board's exit
(`okuefi_board_exit`) is the session's: a program another program started ends
into its starter, and only one the firmware or the Shell started carries its
status off the machine.

From 0.13.0 the arguments are the program's own memory too, and a program for
whose arguments there is none ends with that reason; before, it ran with none
and was not told.

## The volume (0.11.0)

A name is openkal's, `/`-separated UTF-8, and becomes the firmware's,
`\`-separated UCS-2, one unit at a time; `.` is the directory itself. A handle is a
slot of a table and a generation, so a released word and a word never issued are
refused (clause 7.2); the table holds the firmware's file handle and what the
firmware has no place for (how a file was opened, a directory's name from the
root, which a rename names its destination by).

What the firmware has no service for is answered before it is asked (clause 6.2).
`kal_fs_props` claims `MODIFIED_TIME`, `CAPACITY` and `RENAME_NO_REPLACE`, and no
other position: a FAT volume compares names without regard to case and has no
links, locks, executable property or identity of a node. Links, hard links,
locks, the executable property, the resolved name and attenuation report
`kal_err_not_supported`, and an identity is left clear in `present`. A rename
over an existing name removes it first, so a rename is not claimed atomic; the
firmware runs one task, and nothing comes between the two, which is also what
makes an exclusive create, an append and a rename that does not replace one act
each. A name removed while another handle holds it is removed at once and the
held file then fails its transfers, so `REMOVE_OPEN` is not claimed. FAT records
a modification time to two seconds.

The firmware's source of random bytes is a protocol a driver installs, over the
processor's instruction or a virtual device, so whether there is one is known
only when the program runs. Clause 6.1 states an absent source as an absent
interface, which an application built once for every machine cannot be; the
interface is provided, and firmware without the protocol answers
`kal_err_not_supported` and fills nothing --- neither `kal_err_again`, which
states a source that will fill later, nor bytes from a clock.

## The board's exit

A status leaves an application through `Exit`, to whoever started it. A board on
which it must leave the machine --- an emulator running it as a test --- defines
`okuefi_board_exit(int)` in its platform package, and `kal_exit` calls it first;
the definition here does nothing. openkal-platform's (board/qemu-exit.S) writes the status to
QEMU's debug-exit port, only when the processor names QEMU as its hypervisor.

Standard error is the firmware's `StdErr`. A board on which that console reaches
nothing a reader sees defines `okuefi_board_errors_to_console()` to return
nonzero, and standard error is then `ConOut` (0.10.0): under OVMF in QEMU,
`StdErr` writes nothing to the serial console a runner reads, and a program's
diagnostics and the message of an abort were lost. openkal-platform's board, and
this repository's (examples/board), define it.

## The table offsets are checked, not trusted

These structures are ABI. A field one slot out reads a neighbouring function
pointer, which calls something that exists with the wrong arguments. Every member
this implementation reaches is pinned by a `static_assert` on its offset, the file
system, file, RNG and Shell parameters protocols of 0.11.0 included, with their
GUIDs from the UEFI and UEFI Shell specifications.

## Interfaces (0.11.0)

`[kernel-abi] provides-interfaces` states `openkal.abort`, `openkal.env`,
`openkal.exec`, `openkal.fs`, `openkal.memory`, `openkal.process` (0.13.0),
`openkal.random`, `openkal.stream` and `openkal.time`, and `kal_interfaces()`
answers the same
(0.9.0 stated the list as a key of `[package]`, where the engine does not read
it; 0.11.0 adds `openkal.fs` and `openkal.random`). `openkal.exec` is pages of
type `EfiLoaderCode`, protected through the memory attribute protocol where the
firmware offers it: what a loader of images (the ok-image runner carried as a
UEFI application) places code in. The firmware maps no file, so
`KAL_EXEC_PROP_FILE` is not claimed and `kal_exec_map_part` answers not
supported; a loader reads the file through `openkal.fs` and copies it.

The sources are compiled without stack probes (0.11.0): the firmware's stack is
committed in whole and nothing beneath this implementation defines `__chkstk`,
which a frame larger than a page otherwise calls on a row that does not state
`-mno-stack-arg-probe` for the program.

## Verified

`examples/hello`, built with mcpp 2026.11.12.1 and booted by dirun as
`EFI/BOOT/BOOTX64.EFI` under QEMU's q35 with the edk2 firmware (0.45 s, KVM):
a static initialiser ran, the arguments are counted, an over-aligned allocation,
a 20 ms sleep measured as such, the wall clock reads the present; status 42
returned through the board's exit. 0.10.0 adds an allocation aligned to 16 MiB,
which is pages placed at that alignment and no more than its size.

0.10.0 beneath openkal-musl 0.29.6 and openkal-llvm-runtime 0.20.3 (mcpp
2026.11.13.1, `--release`): a program of `import std` that prints with
`std::println` and sorts with `std::ranges::sort` is a 1.2 MB EFI application;
under OVMF it prints, counts one argument (the empty name) and returns 42 in
0.8 s. An ok image carried by such an application (openkal-platform's carrier,
the core runner placing it through `openkal.exec`) prints, writes to standard
error, throws and catches, and returns its status in 0.4 s with 1 GiB or more:
the carrier's allocator and the image's each place a 32 MiB arena at a 256 MiB
alignment.

0.11.0, measured with mcpp 2026.11.15.1 under QEMU's q35 with the edk2 firmware
(the same steps run in CI with Ubuntu's OVMF). Booted as `BOOTX64.EFI` from a
read-only volume, `examples/hello` lists the volume's root and reads its own
image through a file's stream; the firmware had no RNG protocol there, and the
source answered `kal_err_not_supported`. Started by the UEFI Shell from
`startup.nsh` as `hello.efi --shell "a b" c` on a writable FAT volume with a
virtual random device, it receives four arguments with `a b` whole; makes,
opens, lists and removes a directory; writes a file and reads it back, at an
offset and through its stream, shortens it, renames it within a directory and
into another, refuses a rename that does not replace and an exclusive create
onto names that exist, sets and reads a modification time, reads the volume's
capacity, and leaves `okfs/kept.txt`, which the host reads; the random source
fills bytes. `examples/std`, `import std` beneath openkal-musl 0.29.6 and
openkal-llvm-runtime 0.20.3 on openkal-platform 0.28.0's row (0.31.2's from 0.13.0, the group of openkal 0.26) with this
implementation in place of the released one, receives the Shell's arguments in
`main`, reports `/` as its working directory, lists it through
`std::filesystem`, writes and reads a file through `std::ofstream` and
`std::ifstream`, and draws from `std::random_device`; mimalloc's warning that it
is "unable to use secure randomness", printed above 0.10.0, is absent, and
appears again, with `std::random_device` failing, on firmware with no source.

0.13.0, measured with mcpp 2026.11.22.1 under QEMU's q35 with the edk2 firmware
9.2.4 (the same steps run in CI with Ubuntu's OVMF). `examples/starts`, with no
C library, started by the Shell from a writable FAT volume, starts copies of
itself: one receives 1208 arguments, among them `a b`, `"quoted"`, `^caret^`,
an empty one and one with a tab, and 1200 variables, unaltered, and returns 7;
one aborts and is reported ended with 134; one runs in `okp`, finds it as its
first preopen named `/okp` and writes a file there, which its starter finds; one
reads the preference `economy`; twenty each take 128 MiB without freeing it; an
absent name, a directory, a file that is no image, a placed stream and an entry
without `=` are refused by their reasons. `examples/spawn`, C beneath
openkal-musl 0.30.0 on openkal-platform 0.31.2's row with this implementation in
place of the released one, sets a variable with `setenv`, moves its directory
with `chdir`, starts itself by `posix_spawn` with `a b` and an empty argument,
and reads the copy's status 3 with `waitpid`; `posix_spawn` of an absent name is
`ENOENT`.

[kal]: https://github.com/mcpplibs/openkal
[plat]: https://github.com/mcpplibs/openkal-platform
