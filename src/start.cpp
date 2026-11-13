// What the firmware calls, and what a program receives (openkal clause 7.13).
//
// A UEFI application is entered at `efi_main(image, system table)` in the Microsoft x64
// convention, which the `x86_64-uefi` row's compiler uses by default. The program is a C or C++
// program like any other: its static initialisers run, then `main(argc, argv, envp)`, and its
// status is the status of `kal_exit`.
#include <openkal/abort.h>

#include "uefi.h"
#include "state.h"

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

// The options the image was started with, as the arguments: UCS-2 text split at spaces, each
// word UTF-8. Options that are not text (a boot option's binary data) are none.
bool printable(const efi_char16* w, efi_uintn n) {
    for (efi_uintn i = 0; i < n; ++i)
        if (w[i] != 0 && (w[i] < 0x20 || w[i] == 0x7f)) return false;
    return true;
}

void record_arguments(const efi_loaded_image_protocol* li) {
    okuefi::state& s = okuefi::self();
    if (!li) return;
    const auto* w = static_cast<const efi_char16*>(li->load_options);
    efi_uintn n = li->load_options_size / sizeof(efi_char16);
    if (!w || n == 0 || (li->load_options_size & 1u) || !printable(w, n)) return;
    while (n && w[n - 1] == 0) --n;
    // At most three bytes per UCS-2 unit, one terminator per word.
    char* text = nullptr;
    if (s.st->boot_services->allocate_pool(EfiLoaderData, n * 3 + n + 1,
                                           reinterpret_cast<void**>(&text)) != EFI_SUCCESS)
        return;
    char** argv = nullptr;
    if (s.st->boot_services->allocate_pool(EfiLoaderData, (n / 2 + 2) * sizeof(char*),
                                           reinterpret_cast<void**>(&argv)) != EFI_SUCCESS)
        return;
    int argc = 0;
    efi_uintn at = 0;
    char* o = text;
    while (at < n) {
        while (at < n && w[at] == u' ') ++at;
        if (at >= n) break;
        argv[argc++] = o;
        for (; at < n && w[at] != u' '; ++at) {
            const unsigned c = w[at];
            if (c < 0x80) *o++ = static_cast<char>(c);
            else if (c < 0x800) { *o++ = static_cast<char>(0xC0 | (c >> 6)); *o++ = static_cast<char>(0x80 | (c & 0x3F)); }
            else { *o++ = static_cast<char>(0xE0 | (c >> 12)); *o++ = static_cast<char>(0x80 | ((c >> 6) & 0x3F));
                   *o++ = static_cast<char>(0x80 | (c & 0x3F)); }
        }
        *o++ = '\0';
    }
    argv[argc] = nullptr;
    s.argc = argc;
    s.argv = argv;
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
    record_arguments(li);
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
