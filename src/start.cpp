// What the firmware calls, and what a program receives (openkal clause 7.13).
//
// A UEFI application is entered at `efi_main(image, system table)` in the Microsoft x64
// convention, which the `x86_64-uefi` row's compiler uses by default. The program is a C or C++
// program like any other: its static initialisers run, then `main(argc, argv, envp)`, and its
// status is the status of `kal_exit`.
#include <openkal/abort.h>

#include "uefi.h"
#include "state.h"
#include "text.h"

extern "C" int main(int, char**, char**);

#ifdef OKUEFI_STANDALONE
// THE STANDALONE ARRANGEMENT (0.10.0): a C library above this implementation (openkal-musl)
// receives the program at the name every C library's hand-over has. It obtains the arguments
// through openkal.env and runs the initialisers itself, so this entry records the firmware's
// tables and the arguments and hands over; the image's sections are not walked here.
extern "C" int __libc_start_main(int (*)(int, char**, char**), int, char**,
                                 void (*)(), void (*)(), void (*)());
#endif

namespace {

// THE STATIC INITIALISERS are found in this image's own section table. A compiler for this target
// places each translation unit's in `.ctors' (the GNU list, measured with clang 23 for
// x86_64-unknown-uefi), another COFF compiler in `.CRT$XCU' (merged into `.CRT'); the linker
// states the bounds of neither here, and nothing else runs them, there being no C runtime. So the
// sections are read from the PE header the firmware loaded, and each entry that is a function
// (neither 0 nor the list's -1 sentinel) is called, in the order the linker placed them.
using initialiser = void (*)();

void run_section(const unsigned char* base, const char* name) {  // name: eight bytes, padded with NUL
    const auto u32 = [](const unsigned char* p) {
        return static_cast<unsigned>(p[0]) | static_cast<unsigned>(p[1]) << 8 |
               static_cast<unsigned>(p[2]) << 16 | static_cast<unsigned>(p[3]) << 24;
    };
    const auto u16 = [](const unsigned char* p) { return static_cast<unsigned>(p[0]) | static_cast<unsigned>(p[1]) << 8; };
    if (base[0] != 'M' || base[1] != 'Z') return;
    const unsigned char* nt = base + u32(base + 0x3c);
    if (nt[0] != 'P' || nt[1] != 'E') return;
    const unsigned sections = u16(nt + 6);
    const unsigned optional = u16(nt + 20);
    const unsigned char* sh = nt + 24 + optional;
    for (unsigned i = 0; i < sections; ++i, sh += 40) {
        bool same = true;
        for (int k = 0; k < 8; ++k)
            if (sh[k] != static_cast<unsigned char>(name[k])) { same = false; break; }
        if (!same) continue;
        const unsigned size = u32(sh + 8);    // VirtualSize
        const unsigned rva = u32(sh + 12);
        auto* p = reinterpret_cast<initialiser const*>(base + rva);
        for (unsigned k = 0; k < size / sizeof(initialiser); ++k) {
            const auto v = reinterpret_cast<unsigned long long>(p[k]);
            if (v != 0 && v != ~0ull) p[k]();
        }
    }
}

void run_initialisers(const void* image_base) {
    if (!image_base) return;
    const auto* base = static_cast<const unsigned char*>(image_base);
    run_section(base, ".ctors\0");
    run_section(base, ".CRT\0\0\0\0");
}

// THE ARGUMENTS (0.11.0). A program the UEFI Shell starts finds EFI_SHELL_PARAMETERS_PROTOCOL on its
// own image handle, holding the command line already divided by the Shell's rules --- quotes and
// the Shell's escape character removed --- and those are the arguments, each UCS-2 made UTF-8.
// Started otherwise (a boot option, another application), the arguments are the image's load
// options, divided here by the same rules: spaces and tabs separate, a double quote begins or ends
// a part in which they do not, and `^' takes the character after it as it is. 0.10.0 divided the
// options at spaces alone, so `"a b"' arrived as two arguments with their quotes. Options that are
// not text (a boot option's binary data) are none.
bool printable(const efi_char16* w, efi_uintn n) {
    for (efi_uintn i = 0; i < n; ++i)
        if (w[i] != 0 && w[i] != u'\t' && (w[i] < 0x20 || w[i] == 0x7f)) return false;
    return true;
}

template <class T>
T* pool(efi_uintn count) {
    void* p = nullptr;
    if (okuefi::self().st->boot_services->allocate_pool(EfiLoaderData, count * sizeof(T), &p) != EFI_SUCCESS)
        return nullptr;
    return static_cast<T*>(p);
}

void from_shell(const efi_shell_parameters_protocol* sp) {
    okuefi::state& s = okuefi::self();
    if (!sp || !sp->argv || sp->argc == 0) return;
    efi_uintn bytes = 0;
    for (efi_uintn i = 0; i < sp->argc; ++i) {
        const efi_char16* a = sp->argv[i] ? sp->argv[i] : u"";
        kal_uintptr n = 0;
        while (a[n]) ++n;
        bytes += okuefi::utf8_length(a, n) + 1;
    }
    char* text = pool<char>(bytes);
    char** argv = pool<char*>(sp->argc + 1);
    if (!text || !argv) return;
    char* o = text;
    for (efi_uintn i = 0; i < sp->argc; ++i) {
        const efi_char16* a = sp->argv[i] ? sp->argv[i] : u"";
        kal_uintptr n = 0;
        while (a[n]) ++n;
        argv[i] = o;
        o += okuefi::to_utf8(a, n, o, okuefi::utf8_length(a, n));
        *o++ = '\0';
    }
    argv[sp->argc] = nullptr;
    s.argc = static_cast<int>(sp->argc);
    s.argv = argv;
}

void from_load_options(const efi_loaded_image_protocol* li) {
    okuefi::state& s = okuefi::self();
    if (!li) return;
    const auto* w = static_cast<const efi_char16*>(li->load_options);
    efi_uintn n = li->load_options_size / sizeof(efi_char16);
    if (!w || n == 0 || (li->load_options_size & 1u) || !printable(w, n)) return;
    for (efi_uintn i = 0; i < n; ++i)
        if (w[i] == 0) { n = i; break; }
    // An argument is at most its units, with one terminator; there is at most one per two units.
    efi_char16* part = pool<efi_char16>(n + 1);
    char* text = pool<char>(okuefi::utf8_length(w, n) + n + 1);
    char** argv = pool<char*>(n / 2 + 2);
    if (!part || !text || !argv) return;
    int argc = 0;
    efi_uintn at = 0;
    char* o = text;
    const auto blank = [](efi_char16 c) { return c == u' ' || c == u'\t'; };
    while (at < n) {
        while (at < n && blank(w[at])) ++at;
        if (at >= n) break;
        kal_uintptr k = 0;
        bool quoted = false;
        for (; at < n && (quoted || !blank(w[at])); ++at) {
            if (w[at] == u'^' && at + 1 < n) { part[k++] = w[++at]; continue; }
            if (w[at] == u'"') { quoted = !quoted; continue; }
            part[k++] = w[at];
        }
        argv[argc++] = o;
        o += okuefi::to_utf8(part, k, o, okuefi::utf8_length(part, k));
        *o++ = '\0';
    }
    s.st->boot_services->free_pool(part);
    argv[argc] = nullptr;
    s.argc = argc;
    s.argv = argv;
}

void record_arguments(efi_handle image, const efi_loaded_image_protocol* li) {
    void* p = nullptr;
    if (okuefi::self().st->boot_services->handle_protocol(image, &kShellParametersProtocol, &p) == EFI_SUCCESS && p)
        from_shell(static_cast<const efi_shell_parameters_protocol*>(p));
    if (okuefi::self().argc == 0) from_load_options(li);
}

}  // namespace

namespace okuefi {
state& self() {
    static state s{};
    return s;
}
}  // namespace okuefi

extern "C" efi_status efi_main(efi_handle image, efi_system_table* st) {
    okuefi::state& s = okuefi::self();
    s.image = image;
    s.st = st;
    // THE FIRMWARE RESETS THE MACHINE five minutes after it started an application unless told
    // otherwise (the boot manager arms a watchdog); a program is not a boot loader.
    st->boot_services->set_watchdog_timer(0, 0, 0, nullptr);
    okuefi::calibrate_clock();
    efi_loaded_image_protocol* li = nullptr;
    if (st->boot_services->handle_protocol(image, &kLoadedImageProtocol, reinterpret_cast<void**>(&li)) != EFI_SUCCESS)
        li = nullptr;
    s.device = li ? li->device_handle : nullptr;
    record_arguments(image, li);
    // Position zero is the name the program was started by; firmware that gave none gives an empty
    // name rather than none (openkal.env), in both arrangements.
    static char name[] = "";
    static char* unnamed[] = {name, nullptr};
    if (s.argc == 0) { s.argc = 1; s.argv = unnamed; }
#ifdef OKUEFI_STANDALONE
    __libc_start_main(main, 0, nullptr, nullptr, nullptr, nullptr);
    kal_exit(127);
#else
    run_initialisers(li ? li->image_base : nullptr);
    static char* no_environment[] = {nullptr};
    kal_exit(main(s.argc, s.argv, no_environment));
#endif
}
