// A UEFI application written against openkal, with no UEFI type in sight: a static initialiser,
// the arguments, the two clocks, two over-aligned allocations, and a status the board carries out.
import openkal.stream;
import openkal.abort;
import openkal.memory;
import openkal.env;
import openkal.time;

namespace {
void say(const char* s) {
    kal_uintptr n = 0;
    while (s[n]) ++n;
    kal_stream_write(kal_stdout(), s, n);
}
int failures = 0;
void check(bool ok, const char* what) {
    say(ok ? "  ok   " : "  FAIL ");
    say(what);
    say("\n");
    if (!ok) ++failures;
}
struct initialised {
    int value = 7;
    initialised() { value = 42; }
} g_initialised;
}  // namespace

int main(int argc, char**, char**) {
    say("hello from openkal over UEFI\n");
    check(g_initialised.value == 42, "a static initialiser ran before main");
    check(argc >= 1 && kal_env_arg_count() == static_cast<kal_uintptr>(argc), "the arguments are counted alike");
    void* p = kal_alloc(4096, 64);
    check(p != nullptr && (reinterpret_cast<kal_uintptr>(p) & 63) == 0, "an over-aligned allocation");
    kal_free(p, 4096, 64);
    // An alignment of a page or more is pages placed at it (0.10.0): the 256 MiB a C library's
    // allocator states for an arena, which over-allocating would have cost in backed memory.
    void* arena = kal_alloc(32u << 20, 256u << 20);
    check(arena != nullptr && (reinterpret_cast<kal_uintptr>(arena) & ((256u << 20) - 1)) == 0,
          "an allocation aligned to 256 MiB");
    kal_free(arena, 32u << 20, 256u << 20);
    const kal_duration t0 = kal_time_monotonic();
    kal_time_sleep(20u * 1000u * 1000u);
    const kal_duration t1 = kal_time_monotonic();
    check(t1 - t0 >= 19u * 1000u * 1000u && t1 - t0 < 2000u * 1000u * 1000u, "a sleep of 20 ms is measured as such");
    check(kal_time_wall() > 1700000000ull * 1000000000ull, "the wall clock reads the present");
    say(failures ? "FAILED\n" : "all held\n");
    return failures ? 1 : 42;
}
