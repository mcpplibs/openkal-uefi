// openkal's core interfaces, implemented on UEFI Boot Services.
//
// WHY UEFI IS A GOOD FIT FOR THIS SPECIFICATION
//
// openkal divides its interfaces by the KIND of resource rather than by which
// part of a standard library becomes available, and UEFI's Boot Services divide
// the same way. The correspondence is close enough that this backend is mostly
// forwarding:
//
//   openkal.stream   SIMPLE_TEXT_OUTPUT_PROTOCOL / SIMPLE_TEXT_INPUT_PROTOCOL
//   openkal.memory   AllocatePool / FreePool
//   openkal.abort    Exit
//   openkal.fs       SIMPLE_FILE_SYSTEM_PROTOCOL / FILE_PROTOCOL (fs.cpp, 0.11.0)
//   openkal.random   RNG_PROTOCOL (random.cpp, 0.11.0)
//   openkal.process  LoadImage / StartImage (process.cpp, 0.13.0)
//
// `task` is absent because UEFI has no second context to run --- an
// application is the only thing running, and an interface provided in part
// would be worse than one provided not at all. `process` is LoadImage and
// StartImage (process.cpp, 0.13.0): a started program runs to its end before
// its starter resumes, which the specification states as a property (clause
// 7.19) rather than as an absence.
//
// THE ONE PLACE THIS IS NOT FORWARDING: TEXT.
//
// openkal streams carry bytes. UEFI's console takes UCS-2 and treats a line
// feed on its own as a bare cursor movement. Every write therefore widens and
// translates, and does so through a fixed-size buffer rather than an allocation
// — a diagnostic path that allocates is a diagnostic path that can fail while
// reporting a failure.

#include <openkal/abort.h>
#include <openkal/memory.h>
#include <openkal/stream.h>

#include "uefi.h"
#include "state.h"

namespace {

constexpr kal_uintptr kStdin  = 0;
constexpr kal_uintptr kStdout = 1;
constexpr kal_uintptr kStderr = 2;

// The firmware's tables, recorded by efi_main (start.cpp).
#define g_st    (okuefi::self().st)
#define g_image (okuefi::self().image)

// A line at a time is enough to keep the trap count low without making this a
// large object; UEFI's own console is not fast, and the buffer is a translation
// window rather than a cache.
constexpr unsigned kChunk = 128;

kal_intptr write_to(efi_simple_text_output_protocol* out,
                       const unsigned char* p, kal_uintptr n) {
    if (!out) return -kal_err_io;
    efi_char16 buf[kChunk * 2 + 1];        // worst case: every byte becomes CR LF
    kal_uintptr done = 0;
    while (done < n) {
        unsigned w = 0;
        while (done < n && w < kChunk * 2) {
            const unsigned char c = p[done];
            // A bare LF moves the cursor down without returning it, so a
            // second line begins under the end of the first. Firmware differs
            // in how it renders that; none of them do what the writer meant.
            if (c == '\n') buf[w++] = u'\r';
            buf[w++] = static_cast<efi_char16>(c);
            ++done;
        }
        buf[w] = 0;
        if (out->output_string(out, buf) != EFI_SUCCESS)
            return done ? static_cast<kal_intptr>(done) : -kal_err_io;
    }
    return static_cast<kal_intptr>(done);
}

}  // namespace

extern "C" {

// The entry point firmware calls. It records the two values every other
// function here needs and then enters the program.
//
// `kal_main` and not `main`: there is no C runtime to call one, and naming
// it `main` would invite a toolchain to attach startup code that does not exist
// in this arrangement.
// THE BOARD'S EXIT. A status leaves an application through Exit, to whoever started it --- a
// shell, the boot manager --- and a board on which a program's status must leave the machine (an
// emulator that runs it as a test) defines this function in its platform package to carry it
// there before Exit is called. The definition here is the default, and does nothing.
[[gnu::weak]] void okuefi_board_exit(int) {}

// THE BOARD'S ERROR CONSOLE (0.10.0). Standard error is the firmware's StdErr, the console its
// ErrOut variable names. A board on which that console reaches nothing the program's reader sees
// defines this function to return nonzero, and standard error is then ConOut: measured under OVMF
// in QEMU, StdErr wrote nothing to the serial console a runner reads, so a program's diagnostics,
// and the message of an abort, were lost. The definition here is the default, and keeps StdErr.
[[gnu::weak]] int okuefi_board_errors_to_console(void) { return 0; }

}  // extern "C"

namespace {
efi_simple_text_output_protocol* error_console() {
    if (g_st->std_err && !okuefi_board_errors_to_console()) return g_st->std_err;
    return g_st->con_out;
}
}  // namespace

namespace okuefi {
void say(const char* text) {
    if (!g_st || !text) return;
    kal_uintptr n = 0;
    while (text[n]) ++n;
    write_to(error_console(), reinterpret_cast<const unsigned char*>(text), n);
}
}  // namespace okuefi

extern "C" {

// THE PROGRAM'S END (0.13.0). What the program holds is returned first --- its files closed, its
// memory freed (release_memory) --- so that a starter that follows it finds the volume and the
// memory as before the start. The board's exit is the session's: a program another program started
// ends into its starter, which reads the status from StartImage, and only a program the firmware or
// the Shell started carries its status off the machine.
static void end_program(efi_status status, int code) {
    okuefi::close_all();
    okuefi::release_memory();
    if (!okuefi::self().start) okuefi_board_exit(code);
    g_st->boot_services->exit(g_image, status, 0, nullptr);
}

KAL_NORETURN void kal_abort(const char* msg, kal_uintptr len) {
    if (g_st && msg && len)
        write_to(error_console(), reinterpret_cast<const unsigned char*>(msg), len);
    if (g_st) {
        const unsigned char nl = '\n';
        write_to(error_console(), &nl, 1);
        okuefi::close_all();
        okuefi::release_memory();
        g_st->boot_services->exit(g_image, EFI_ABORTED, 0, nullptr);
    }
    for (;;) {}
}

KAL_NORETURN void kal_exit(int code) {
    // The status itself, as a warning code (the high bit clear), so that the program that started
    // this one reads the number and not only success or failure.
    if (g_st) end_program(static_cast<efi_status>(code & 0xff), code);
    okuefi_board_exit(code);
    for (;;) {}
}

// ── openkal.stream ──────────────────────────────────────────────────────────
kal_stream kal_stdin (void) { return kal_stream{kStdin};  }
kal_stream kal_stdout(void) { return kal_stream{kStdout}; }
kal_stream kal_stderr(void) { return kal_stream{kStderr}; }

kal_intptr kal_stream_write(kal_stream s, const void* buf, kal_uintptr n) {
    if (!g_st) return -kal_err_io;
    if (okuefi::file_stream(s.h)) return okuefi::file_write(s.h, buf, n);
    efi_simple_text_output_protocol* out =
        s.h == kStdout ? g_st->con_out
      : s.h == kStderr ? error_console()
      : nullptr;
    if (!out) return -kal_err_invalid;
    return write_to(out, static_cast<const unsigned char*>(buf), n);
}

// Not provided in a usable form, and reported rather than faked. UEFI's
// console input is a key-stroke protocol with a wait event, not a byte stream;
// presenting it as one would give a reader something that appears to work and
// loses every key that is not a plain character. Returning "no bytes, no error"
// is end of input, which is the truthful reading of a console this
// implementation does not attempt to interpret.
//
// A file's stream (openkal.fs, 0.11.0) is read from the file.
kal_intptr kal_stream_read(kal_stream s, void* buf, kal_uintptr n) {
    if (okuefi::file_stream(s.h)) return okuefi::file_read(s.h, buf, n);
    if (s.h != kStdin) return -kal_err_invalid;
    return 0;
}

// The console is unbuffered by this implementation; the bytes were handed to
// firmware when the write returned. A file's stream is flushed by the firmware.
int kal_stream_flush(kal_stream s) {
    if (okuefi::file_stream(s.h)) return okuefi::file_flush(s.h);
    if (s.h != kStdin && s.h != kStdout && s.h != kStderr) return kal_err_invalid;
    return kal_ok;
}

kal_uintptr kal_stream_props(kal_stream s) {
    if (okuefi::file_stream(s.h)) return 0;
    if (s.h != kStdin && s.h != kStdout && s.h != kStderr) return 0;
    return KAL_STREAM_PROP_INTERACTIVE;
}

// ── openkal.memory ──────────────────────────────────────────────────────────
//
// AllocatePool has no alignment parameter. It guarantees 8-byte alignment,
// which covers every fundamental type on this architecture but not an
// over-aligned one. Rather than return memory that does not meet the request,
// a stricter alignment is satisfied by over-allocating and storing the original
// pointer immediately before the aligned address — the same technique a C
// library uses for `aligned_alloc` where the platform lacks one.
//
// AN ALIGNMENT OF A PAGE OR MORE IS PAGES (0.10.0). Over-allocating costs the
// alignment in memory that is backed, and the allocator of a C library above
// this one asks for its arenas at 256 MiB: measured under OVMF with 512 MiB,
// a 32 MiB arena became a 288 MiB request and failed. Pages are placed at an
// aligned address instead: the memory map is read for free memory that holds
// an aligned range of the size, and the pages there are allocated
// (AllocateAddress), the highest such range first, where the firmware's own
// allocations would otherwise go. Nothing beyond the size is taken.
// The quantum this environment allocates and protects memory in.
//
// Firmware hands out memory in pages of four kilobytes --- that is the unit of
// `AllocatePages', which is what this allocator draws on --- and it applies no
// protection of its own. The coarser of the two is therefore the page, and a
// caller that rounds to it is never wrong.
kal_uintptr kal_memory_granularity(void) { return 4096; }

// The pool's size is the firmware's and not stated by Boot Services; nothing is reported.
int kal_memory_info(kal_memory_status* out) {
    if (out == nullptr || out->self_size < 2 * sizeof(kal_u32)) return kal_err_invalid;
    out->present = 0;
    return kal_ok;
}

}  // extern "C"

namespace {

constexpr kal_uintptr kPage = 4096;

kal_uintptr pages_of(kal_uintptr size) { return (size + kPage - 1) / kPage; }

// The highest address, aligned to `align', at which `pages' free pages lie; zero where none do.
unsigned long long aligned_free_range(kal_uintptr pages, kal_uintptr align) {
    auto* bs = g_st->boot_services;
    efi_uintn size = 0, key = 0, step = 0;
    unsigned int version = 0;
    if (bs->get_memory_map(&size, nullptr, &key, &step, &version) != EFI_BUFFER_TOO_SMALL || step == 0)
        return 0;
    // The pool allocation below may add entries to the map it is to hold.
    size += 4 * step;
    void* buffer = nullptr;
    if (bs->allocate_pool(EfiLoaderData, size, &buffer) != EFI_SUCCESS) return 0;
    unsigned long long best = 0;
    if (bs->get_memory_map(&size, static_cast<efi_memory_descriptor*>(buffer), &key, &step, &version)
        == EFI_SUCCESS) {
        const unsigned long long want = static_cast<unsigned long long>(pages) * kPage;
        for (efi_uintn at = 0; at + step <= size; at += step) {
            const auto* d = reinterpret_cast<const efi_memory_descriptor*>(
                static_cast<const unsigned char*>(buffer) + at);
            if (d->type != EfiConventionalMemory || d->pages * kPage < want) continue;
            const unsigned long long start = d->physical_start;
            const unsigned long long end = start + d->pages * kPage;
            const unsigned long long top = (end - want) & ~(static_cast<unsigned long long>(align) - 1);
            if (top >= start && top != 0 && top > best) best = top;
        }
    }
    bs->free_pool(buffer);
    return best;
}

void* aligned_pages(kal_uintptr size, kal_uintptr align) {
    auto* bs = g_st->boot_services;
    const kal_uintptr pages = pages_of(size);
    unsigned long long address = 0;
    if (align <= kPage)
        return bs->allocate_pages(AllocateAnyPages, EfiLoaderData, pages, &address) == EFI_SUCCESS
             ? reinterpret_cast<void*>(address) : nullptr;
    address = aligned_free_range(pages, align);
    if (address == 0) return nullptr;
    return bs->allocate_pages(AllocateAddress, EfiLoaderData, pages, &address) == EFI_SUCCESS
         ? reinterpret_cast<void*>(address) : nullptr;
}

// WHAT THE PROGRAM HOLDS, SO THAT ITS END RETURNS IT (0.13.0). The firmware frees an application's
// image when it exits and nothing it allocated: the pool and the pages a program obtained stay taken
// until the machine restarts. A program the firmware or the Shell started ends the session, but one a
// program started (openkal.process) is followed by its starter, which would find the memory of every
// program it started still taken --- the C library's allocator reserves its arenas in pages, so a
// starter ran out after a few starts. Each block of the pool is preceded by its link in a list, each
// range of pages is a row of a table, and release_memory returns both at the program's end.
struct block { block* prev; block* next; };
static_assert(sizeof(block) == 16, "a block's link keeps the pool's alignment of eight");
block* g_blocks = nullptr;

struct range { unsigned long long at; kal_uintptr pages; bool code; };
range* g_ranges = nullptr;
kal_uintptr g_range_count = 0, g_range_cap = 0;

void* linked(void* raw) {
    auto* b = static_cast<block*>(raw);
    b->prev = nullptr;
    b->next = g_blocks;
    if (g_blocks) g_blocks->prev = b;
    g_blocks = b;
    return b + 1;
}

void unlink(block* b) {
    if (b->prev) b->prev->next = b->next; else g_blocks = b->next;
    if (b->next) b->next->prev = b->prev;
}

// The table grows by doubling, in the pool and outside the list it records.
bool record(unsigned long long at, kal_uintptr pages, bool code) {
    if (g_range_count == g_range_cap) {
        const kal_uintptr cap = g_range_cap ? 2 * g_range_cap : 64;
        void* p = nullptr;
        if (g_st->boot_services->allocate_pool(EfiLoaderData, cap * sizeof(range), &p) != EFI_SUCCESS)
            return false;
        auto* grown = static_cast<range*>(p);
        for (kal_uintptr i = 0; i < g_range_count; ++i) grown[i] = g_ranges[i];
        if (g_ranges) g_st->boot_services->free_pool(g_ranges);
        g_ranges = grown;
        g_range_cap = cap;
    }
    g_ranges[g_range_count++] = range{at, pages, code};
    return true;
}

// The pages recorded at `at', removed from the table; zero where none were.
kal_uintptr forget(unsigned long long at) {
    for (kal_uintptr i = 0; i < g_range_count; ++i) {
        if (g_ranges[i].at != at) continue;
        const kal_uintptr pages = g_ranges[i].pages;
        g_ranges[i] = g_ranges[--g_range_count];
        return pages;
    }
    return 0;
}

}  // namespace

namespace okuefi {

// The pages of a region of code (exec.cpp), recorded as the allocator's are; a range that cannot be
// recorded is returned at once, and the reservation fails.
bool pages_taken(unsigned long long at, kal_uintptr pages, bool code) {
    if (record(at, pages, code)) return true;
    g_st->boot_services->free_pages(at, pages);
    return false;
}
kal_uintptr pages_returned(unsigned long long at) { return forget(at); }

void release_memory() {
    if (!g_st) return;
    auto* bs = g_st->boot_services;
    for (kal_uintptr i = 0; i < g_range_count; ++i) {
        if (g_ranges[i].code) exec_unprotect(g_ranges[i].at, g_ranges[i].pages);
        bs->free_pages(g_ranges[i].at, g_ranges[i].pages);
    }
    g_range_count = 0;
    while (g_blocks) {
        block* b = g_blocks;
        g_blocks = b->next;
        bs->free_pool(b);
    }
    if (g_ranges) bs->free_pool(g_ranges);
    g_ranges = nullptr;
    g_range_cap = 0;
}

}  // namespace okuefi

extern "C" {

void* kal_alloc(kal_uintptr size, kal_uintptr align) {
    if (!g_st) return nullptr;
    if (size == 0) size = 1;
    if (align >= kPage && (align & (align - 1)) == 0) {
        void* p = aligned_pages(size, align);
        if (p && !okuefi::pages_taken(reinterpret_cast<unsigned long long>(p), pages_of(size), false))
            return nullptr;
        return p;
    }
    if (align <= 8) {
        void* raw = nullptr;
        if (g_st->boot_services->allocate_pool(EfiLoaderData, sizeof(block) + size, &raw) != EFI_SUCCESS)
            return nullptr;
        return linked(raw);
    }
    void* raw = nullptr;
    const kal_uintptr slack = sizeof(block) + align + sizeof(void*);
    if (g_st->boot_services->allocate_pool(EfiLoaderData, size + slack, &raw) != EFI_SUCCESS)
        return nullptr;
    auto base = reinterpret_cast<kal_uintptr>(linked(raw)) + sizeof(void*);
    auto aligned = (base + align - 1) & ~(align - 1);
    reinterpret_cast<void**>(aligned)[-1] = raw;
    return reinterpret_cast<void*>(aligned);
}

void kal_free(void* p, kal_uintptr size, kal_uintptr align) {
    if (!p || !g_st) return;
    if (align >= kPage && (align & (align - 1)) == 0) {
        const auto at = reinterpret_cast<unsigned long long>(p);
        const kal_uintptr recorded = forget(at);
        g_st->boot_services->free_pages(at, recorded ? recorded : pages_of(size ? size : 1));
        return;
    }
    void* raw = align <= 8 ? static_cast<void*>(static_cast<block*>(p) - 1) : reinterpret_cast<void**>(p)[-1];
    unlink(static_cast<block*>(raw));
    g_st->boot_services->free_pool(raw);
}

}  // extern "C"
