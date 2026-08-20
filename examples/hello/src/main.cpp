// A UEFI application written against openkal, with no UEFI type in sight.
//
// ⚠️ `kal_main` and not `main`: nothing here supplies a C runtime, and the
// backend's `efi_main` is what firmware calls. That indirection is the point —
// the same source compiles against any openkal implementation.
import openkal.stream;
import openkal.abort;
import openkal.memory;

namespace {
void say(const char* s) {
    kal_uintptr n = 0;
    while (s[n]) ++n;
    kal_stream_write(kal_stdout(), s, n);
}
}  // namespace

extern "C" [[noreturn]] void kal_main() {
    say("hello from openkal over UEFI\n");

    void* p = kal_alloc(4096, 64);          // over-aligned on purpose
    say(p ? "pool ok\n" : "pool failed\n");
    kal_free(p, 4096, 64);

    kal_exit(p ? 0 : 1);
}
