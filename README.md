# openkal-uefi

An implementation of [openkal][kal] on UEFI Boot Services, for applications the
firmware loads before an operating system exists.

```toml
[build]
target  = "x86_64-windows-gnu"
ldflags = ["-nostdlib", "-Wl,--subsystem,10", "-Wl,-e,efi_main"]

[dependencies]
openkal      = "0.5.1"
openkal-uefi = "0.3.0"
```

## ⚠️ The target is `x86_64-windows-gnu`, and that is not a workaround

A UEFI application is PE/COFF with subsystem 10, entered through the Microsoft
x64 calling convention. Both are properties this toolchain already has, so
firmware function pointers are called directly rather than through per-call
annotations.

The first analysis of this backend concluded that mcpp would need a new "PE
freestanding" target before a UEFI application could exist at all. **Measured:
it does not.** The three link flags above produce
`IMAGE_SUBSYSTEM_EFI_APPLICATION (0xA)` with no DLL imports, which is what
firmware loads.

## Why UEFI fits this specification

openkal divides its interfaces by the **kind of resource** rather than by which
part of a standard library becomes available, and Boot Services divide the same
way. This backend is mostly forwarding:

| openkal | UEFI |
|---|---|
| `stream` | `SIMPLE_TEXT_OUTPUT_PROTOCOL` |
| `memory` | `AllocatePool` / `FreePool` |
| `abort` | `Exit` |

⚠️ `process` and `task` are absent because UEFI has no process model. An
application is the only thing running, and an interface provided in part would
be worse than one provided not at all — `import openkal.process;` does not
resolve, which is the honest answer rather than a set of calls that always fail.

The five interfaces version 0.8 adds are absent by the same rule. UEFI has no
process model to copy an address space within, so `space` is not provided; no
second context to bound a wait against, so `timeout` is not; and while Boot
Services do expose a network stack, this backend forwards only the protocols
listed above, so `net` and `datagram` are not. `terminal` is absent because
`SIMPLE_TEXT_OUTPUT_PROTOCOL` has no mode to read or set.

Clause 6.1 makes each absence a link-time absence, so a program requiring one of
them is refused when it is built rather than when it runs.

## The three places this is not forwarding

**Text.** openkal streams carry bytes; UEFI's console takes UCS-2 and treats a
bare line feed as a cursor movement that does not return the carriage. Every
write widens and translates, through a fixed-size buffer rather than an
allocation — a diagnostic path that allocates is one that can fail while
reporting a failure.

**Alignment.** `AllocatePool` has no alignment parameter and guarantees eight
bytes. A stricter request is satisfied by over-allocating and storing the
original pointer immediately before the aligned address, which is what a C
library does where the platform lacks `aligned_alloc`.

**Input.** ⚠️ `kal_stream_read` reports end of input rather than pretending.
UEFI's console input is a key-stroke protocol with a wait event, not a byte
stream; presenting it as one would give a reader something that appears to work
and silently loses every key that is not a plain character.

## The table offsets are checked, not trusted

These structures are ABI. A field written one slot out reads a neighbouring
function pointer — which calls something that exists, with the wrong arguments,
and the failure appears far from the cause. Every member this backend reaches is
pinned by a `static_assert` on its offset, so a mistake is a compile error.

## Verified

`examples/hello` boots under OVMF in QEMU as `EFI/BOOT/BOOTX64.EFI`:

```
BdsDxe: starting Boot0001 "UEFI QEMU HARDDISK QM00001 " …
hello from openkal over UEFI
pool ok
```

`kal_exit` returns control to the firmware, which proceeds to its own boot
manager — the status reaches the loader rather than the program spinning.

[kal]: https://github.com/mcpplibs/openkal
