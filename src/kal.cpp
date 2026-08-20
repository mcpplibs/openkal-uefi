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
//
// ⚠️ `process` and `task` are absent because UEFI has no process model — an
// application is the only thing running, and an interface provided in part
// would be worse than one provided not at all. `import openkal.process;` does
// not resolve, which is the honest answer rather than a set of calls that
// always fail.
//
// ⚠️ THE ONE PLACE THIS IS NOT FORWARDING: TEXT.
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

namespace {

constexpr kal_uintptr kStdin  = 0;
constexpr kal_uintptr kStdout = 1;
constexpr kal_uintptr kStderr = 2;

efi_handle          g_image = nullptr;
efi_system_table*   g_st    = nullptr;

// A line at a time is enough to keep the trap count low without making this a
// large object; UEFI's own console is not fast, and the buffer is a translation
// window rather than a cache.
constexpr unsigned kChunk = 128;

kal_io_result write_to(efi_simple_text_output_protocol* out,
                       const unsigned char* p, kal_uintptr n) {
    if (!out) return kal_io_result{0, kal_err_io};
    efi_char16 buf[kChunk * 2 + 1];        // worst case: every byte becomes CR LF
    kal_uintptr done = 0;
    while (done < n) {
        unsigned w = 0;
        while (done < n && w < kChunk * 2) {
            const unsigned char c = p[done];
            // ⚠️ A bare LF moves the cursor down without returning it, so a
            // second line begins under the end of the first. Firmware differs
            // in how it renders that; none of them do what the writer meant.
            if (c == '\n') buf[w++] = u'\r';
            buf[w++] = static_cast<efi_char16>(c);
            ++done;
        }
        buf[w] = 0;
        if (out->output_string(out, buf) != EFI_SUCCESS)
            return kal_io_result{done, kal_err_io};
    }
    return kal_io_result{done, kal_ok};
}

}  // namespace

extern "C" {

// The entry point firmware calls. It records the two values every other
// function here needs and then enters the program.
//
// ⚠️ `kal_main` and not `main`: there is no C runtime to call one, and naming
// it `main` would invite a toolchain to attach startup code that does not exist
// in this arrangement.
[[noreturn]] void kal_main();

efi_status efi_main(efi_handle image, efi_system_table* st) {
    g_image = image;
    g_st    = st;
    kal_main();
}

// ── openkal.abort ───────────────────────────────────────────────────────────
KAL_NORETURN void kal_abort(const char* msg, kal_uintptr len) {
    if (g_st && msg && len)
        write_to(g_st->std_err ? g_st->std_err : g_st->con_out,
                 reinterpret_cast<const unsigned char*>(msg), len);
    if (g_st) {
        const unsigned char nl = '\n';
        write_to(g_st->std_err ? g_st->std_err : g_st->con_out, &nl, 1);
        g_st->boot_services->exit(g_image, EFI_ABORTED, 0, nullptr);
    }
    for (;;) {}
}

KAL_NORETURN void kal_exit(int code) {
    if (g_st)
        g_st->boot_services->exit(g_image,
                                  code == 0 ? EFI_SUCCESS : EFI_ABORTED,
                                  0, nullptr);
    for (;;) {}
}

// ── openkal.stream ──────────────────────────────────────────────────────────
kal_stream kal_stdin (void) { return kal_stream{kStdin};  }
kal_stream kal_stdout(void) { return kal_stream{kStdout}; }
kal_stream kal_stderr(void) { return kal_stream{kStderr}; }

kal_io_result kal_stream_write(kal_stream s, const void* buf, kal_uintptr n) {
    if (!g_st) return kal_io_result{0, kal_err_io};
    efi_simple_text_output_protocol* out =
        s.h == kStdout ? g_st->con_out
      : s.h == kStderr ? (g_st->std_err ? g_st->std_err : g_st->con_out)
      : nullptr;
    if (!out) return kal_io_result{0, kal_err_invalid};
    return write_to(out, static_cast<const unsigned char*>(buf), n);
}

// ⚠️ Not provided in a usable form, and reported rather than faked. UEFI's
// console input is a key-stroke protocol with a wait event, not a byte stream;
// presenting it as one would give a reader something that appears to work and
// loses every key that is not a plain character. Returning "no bytes, no error"
// is end of input, which is the truthful reading of a console this
// implementation does not attempt to interpret.
kal_io_result kal_stream_read(kal_stream s, void*, kal_uintptr) {
    if (s.h != kStdin) return kal_io_result{0, kal_err_invalid};
    return kal_io_result{0, kal_ok};
}

// The console is unbuffered by this implementation; the bytes were handed to
// firmware when the write returned.
int kal_stream_flush(kal_stream s) {
    if (s.h != kStdin && s.h != kStdout && s.h != kStderr) return kal_err_invalid;
    return kal_ok;
}

kal_uintptr kal_stream_props(kal_stream s) {
    if (s.h != kStdin && s.h != kStdout && s.h != kStderr) return 0;
    return KAL_STREAM_PROP_INTERACTIVE;
}

// ── openkal.memory ──────────────────────────────────────────────────────────
//
// ⚠️ AllocatePool has no alignment parameter. It guarantees 8-byte alignment,
// which covers every fundamental type on this architecture but not an
// over-aligned one. Rather than return memory that does not meet the request,
// a stricter alignment is satisfied by over-allocating and storing the original
// pointer immediately before the aligned address — the same technique a C
// library uses for `aligned_alloc` where the platform lacks one.
void* kal_alloc(kal_uintptr size, kal_uintptr align) {
    if (!g_st) return nullptr;
    if (size == 0) size = 1;
    if (align <= 8) {
        void* p = nullptr;
        if (g_st->boot_services->allocate_pool(EfiLoaderData, size, &p) != EFI_SUCCESS)
            return nullptr;
        return p;
    }
    void* raw = nullptr;
    const kal_uintptr slack = align + sizeof(void*);
    if (g_st->boot_services->allocate_pool(EfiLoaderData, size + slack, &raw) != EFI_SUCCESS)
        return nullptr;
    auto base = reinterpret_cast<kal_uintptr>(raw) + sizeof(void*);
    auto aligned = (base + align - 1) & ~(align - 1);
    reinterpret_cast<void**>(aligned)[-1] = raw;
    return reinterpret_cast<void*>(aligned);
}

void kal_free(void* p, kal_uintptr, kal_uintptr align) {
    if (!p || !g_st) return;
    void* raw = align <= 8 ? p : reinterpret_cast<void**>(p)[-1];
    g_st->boot_services->free_pool(raw);
}

}  // extern "C"
