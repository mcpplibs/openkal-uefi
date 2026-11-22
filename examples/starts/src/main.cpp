// openkal.process on the firmware (0.13.0): a program starts copies of itself through LoadImage and
// StartImage, and each start has ended when kal_process_spawn returns (clause 7.19).
//
// Started by the UEFI Shell as `starts.efi' from the root of a writable FAT volume (CI's
// startup.nsh), it checks that a copy receives its vector unaltered --- elements with blanks, quotes,
// the Shell's escape character and none at all, and more of them than any table held --- and its
// variables, more than any table held; that its status, an abort and the directory it runs in come
// back; that a start which cannot happen is refused by its reason; and that twenty starts of a copy
// that takes 128 MiB and does not free it all succeed, each copy's memory being returned at its end.
//
// The declarations through the headers, whose constants are macros a module does not carry.
#include <openkal/abort.h>
#include <openkal/env.h>
#include <openkal/fs.h>
#include <openkal/memory.h>
#include <openkal/process.h>
#include <openkal/stream.h>

namespace {
kal_uintptr length(const char* s) {
    kal_uintptr n = 0;
    while (s[n]) ++n;
    return n;
}
void say(const char* s) { kal_stream_write(kal_stdout(), s, length(s)); }
int failures = 0;
void check(bool ok, const char* what) {
    say(ok ? "  ok   " : "  FAIL ");
    say(what);
    say("\n");
    if (!ok) ++failures;
}
bool same(const char* a, kal_uintptr n, const char* b) {
    if (n != length(b)) return false;
    for (kal_uintptr i = 0; i < n; ++i)
        if (a[i] != b[i]) return false;
    return true;
}
bool argument_is(kal_uintptr i, const char* want) {
    char buf[64];
    const kal_intptr n = kal_env_arg(i, buf, sizeof buf);
    return n >= 0 && static_cast<kal_uintptr>(n) <= sizeof buf && same(buf, static_cast<kal_uintptr>(n), want);
}

// This program's file, beneath the root: the last component of the name it was started by, which
// the Shell gives as the path it resolved (`FS0:\\starts.efi').
char kSelf[64] = "starts.efi";
void find_self() {
    char a0[256];
    const kal_intptr n = kal_env_arg(0, a0, sizeof a0);
    if (n <= 0 || static_cast<kal_uintptr>(n) >= sizeof a0) return;
    kal_intptr from = n;
    while (from > 0 && a0[from - 1] != '\\' && a0[from - 1] != '/' && a0[from - 1] != ':') --from;
    if (n - from <= 0 || n - from >= static_cast<kal_intptr>(sizeof kSelf)) return;
    for (kal_intptr i = from; i < n; ++i) kSelf[i - from] = a0[i];
    kSelf[n - from] = 0;
}
constexpr kal_uintptr kMany = 1200;   // above 256 arguments and 512 variables, the old tables

// The vector a copy is given: its name, the errand, then the elements it checks.
const char* const kOdd[] = {"a b", "\"quoted\"", "^caret^", "", "tab\there", "plain"};
constexpr kal_uintptr kOddCount = sizeof kOdd / sizeof kOdd[0];

kal_dir root() {
    kal_dir d{};
    for (kal_uintptr i = 0; i < kal_fs_preopen_count(); ++i) {
        char name[256];
        kal_uintptr n = 0;
        if (kal_fs_preopen(i, &d, name, sizeof name, &n) == kal_ok && same(name, n, "/")) return d;
    }
    return d;
}

int start(const char* const* argv, kal_uintptr argc, const char* const* envp, kal_uintptr envc,
          kal_dir work, kal_uintptr flags, int* status, int* terminated) {

    static kal_uintptr alens[2 * kMany], elens[2 * kMany];
    for (kal_uintptr i = 0; i < argc; ++i) alens[i] = length(argv[i]);
    for (kal_uintptr i = 0; i < envc; ++i) elens[i] = length(envp[i]);
    const kal_dir r = root();
    const kal_spawn how{r, work, nullptr, nullptr, 0, flags};
    const kal_spawn_streams streams{{0}, {0}, {0}};
    kal_process p{};
    const int e = kal_process_spawn(&how, kSelf, length(kSelf), const_cast<const char**>(argv), alens, argc,
                                    const_cast<const char**>(envp), elens, envc, &streams, &p);
    if (e != kal_ok) return e;
    const int w = kal_process_wait(p, status, terminated);
    kal_process_close(p);
    return w;
}

// ── the copy's errands ──────────────────────────────────────────────────────

int errand(const char* what) {
    if (same(what, length(what), "vector")) {
        // Its vector: the name, the errand, the odd elements, then kMany of "x<i>".
        bool ok = kal_env_arg_count() == 2 + kOddCount + kMany;
        for (kal_uintptr i = 0; ok && i < kOddCount; ++i) ok = argument_is(2 + i, kOdd[i]);
        char want[16];
        for (kal_uintptr i = 0; ok && i < kMany; ++i) {
            kal_uintptr n = 0, v = i;
            char digits[8];
            do { digits[n++] = static_cast<char>('0' + v % 10); v /= 10; } while (v);
            kal_uintptr k = 0;
            want[k++] = 'x';
            while (n) want[k++] = digits[--n];
            want[k] = 0;
            ok = argument_is(2 + kOddCount + i, want);
        }
        // Its variables: kMany of them, and the last found by its name.
        char value[32];
        const kal_intptr n = kal_env_var("OKV_1199", 8, value, sizeof value);
        ok = ok && kal_env_var_count() == kMany && n == 4 && same(value, 4, "last");
        char name[32];
        const kal_intptr m = kal_env_var_at(0, name, sizeof name);
        ok = ok && m == 5 && same(name, 5, "OKV_0");
        return ok ? 7 : 1;
    }
    if (same(what, length(what), "abort")) {
        static constexpr char why[] = "starts: the copy aborts, as asked";
        kal_abort(why, sizeof why - 1);
    }
    if (same(what, length(what), "where")) {
        // Started in okp: its first preopen is that directory, named from the root, and a file
        // written beneath it lands there.
        kal_dir d{};
        char name[64];
        kal_uintptr n = 0;
        if (kal_fs_preopen_count() != 2 || kal_fs_preopen(0, &d, name, sizeof name, &n) != kal_ok) return 1;
        if (!same(name, n, "/okp")) return 2;
        kal_file f{};
        if (kal_fs_open(d, "here.txt", 8, KAL_OPEN_WRITE | KAL_OPEN_CREATE | KAL_OPEN_TRUNCATE, &f) != kal_ok) return 3;
        kal_stream_write(kal_fs_stream(f), "here", 4);
        kal_fs_close_file(f);
        return 5;
    }
    if (same(what, length(what), "economy")) return kal_env_resources() == KAL_RESOURCES_ECONOMY ? 6 : 1;
    if (same(what, length(what), "hold")) {
        // 128 MiB at a 128 MiB alignment, as the C library's allocator reserves an arena, never freed.
        void* p = kal_alloc(128u << 20, 128u << 20);
        if (!p) return 1;
        static_cast<char*>(p)[0] = 1;
        return 9;
    }
    return 1;
}

}  // namespace

int main(int, char**, char**) {
    char what[16];
    if (kal_env_arg_count() >= 2) {
        // A copy: its errand is its second element.
        const kal_intptr n = kal_env_arg(1, what, sizeof what - 1);
        what[n > 0 ? n : 0] = 0;
        return errand(what);
    }

    find_self();
    say("openkal-uefi: starts, as ");
    say(kSelf);
    say("\n");
    const kal_uintptr props = kal_process_props();
    check((props & KAL_PROCESS_PROP_RUNS_TO_END) && !(props & KAL_PROCESS_PROP_TERMINATE),
          "a started program runs to its end first, and none is left to end");
    const kal_dir r = root();
    int status = -1, terminated = -1;

    static const char* argv[2 + kOddCount + kMany];
    static char numbers[kMany][8];
    static const char* envp[kMany];
    static char entries[kMany][24];
    argv[0] = kSelf;
    argv[1] = "vector";
    for (kal_uintptr i = 0; i < kOddCount; ++i) argv[2 + i] = kOdd[i];
    for (kal_uintptr i = 0; i < kMany; ++i) {
        char* o = numbers[i];
        kal_uintptr n = 0, v = i;
        char digits[8];
        do { digits[n++] = static_cast<char>('0' + v % 10); v /= 10; } while (v);
        *o++ = 'x';
        while (n) *o++ = digits[--n];
        *o = 0;
        argv[2 + kOddCount + i] = numbers[i];
        char* e = entries[i];
        const char* key = "OKV_";
        while (*key) *e++ = *key++;
        n = 0; v = i;
        do { digits[n++] = static_cast<char>('0' + v % 10); v /= 10; } while (v);
        while (n) *e++ = digits[--n];
        const char* value = i == kMany - 1 ? "=last" : "=v";
        while (*value) *e++ = *value++;
        *e = 0;
        envp[i] = entries[i];
    }
    check(start(argv, 2 + kOddCount + kMany, envp, kMany, r, 0, &status, &terminated) == kal_ok
              && status == 7 && terminated == 0,
          "a copy receives 1208 arguments, blanks, quotes, `^' and an empty one among them, and 1200 "
          "variables, unaltered, and its status comes back");

    const char* abort_argv[] = {kSelf, "abort"};
    check(start(abort_argv, 2, nullptr, 0, r, 0, &status, &terminated) == kal_ok && status == 134 && terminated == 1,
          "a copy that aborts is reported ended, not ending itself, with the status of an abort");

    kal_fs_mkdir(r, "okp", 3);
    kal_dir okp{};
    const char* where_argv[] = {kSelf, "where"};
    check(kal_fs_open_dir(r, "okp", 3, &okp) == kal_ok
              && start(where_argv, 2, nullptr, 0, okp, 0, &status, &terminated) == kal_ok && status == 5,
          "a copy runs in the directory it was given: its first preopen, named /okp");
    kal_node_info info{};
    info.self_size = sizeof info;
    check(kal_fs_info(okp, "here.txt", 8, 0, KAL_INFO_KIND, &info) == kal_ok && info.kind == kal_node_file,
          "the file the copy wrote beneath its directory is there");
    kal_fs_close_dir(okp);

    const char* economy_argv[] = {kSelf, "economy"};
    check(start(economy_argv, 2, nullptr, 0, r, KAL_SPAWN_RESOURCES_ECONOMY, &status, &terminated) == kal_ok
              && status == 6,
          "the resource preference reaches the copy");

    const char* hold_argv[] = {kSelf, "hold"};
    bool held = true;
    for (int i = 0; i < 20 && held; ++i)
        held = start(hold_argv, 2, nullptr, 0, r, 0, &status, &terminated) == kal_ok && status == 9;
    check(held, "twenty copies each take 128 MiB and end without freeing it: each end returns it");

    const kal_spawn how{r, r, nullptr, nullptr, 0, 0};
    const kal_spawn_streams inherit{{0}, {0}, {0}};
    kal_process p{};
    const char* none_argv[] = {"absent.efi"};
    const kal_uintptr none_len[] = {10};
    check(kal_process_spawn(&how, "absent.efi", 10, none_argv, none_len, 1, nullptr, nullptr, 0, &inherit, &p)
              == kal_err_not_found,
          "a name that is not there is kal_err_not_found");
    check(kal_process_spawn(&how, "okp", 3, none_argv, none_len, 1, nullptr, nullptr, 0, &inherit, &p)
              == kal_err_is_directory,
          "a directory is kal_err_is_directory");
    kal_file f{};
    if (kal_fs_open(r, "text.efi", 8, KAL_OPEN_WRITE | KAL_OPEN_CREATE | KAL_OPEN_TRUNCATE, &f) == kal_ok) {
        kal_stream_write(kal_fs_stream(f), "not a program\n", 14);
        kal_fs_close_file(f);
    }
    check(kal_process_spawn(&how, "text.efi", 8, none_argv, none_len, 1, nullptr, nullptr, 0, &inherit, &p)
              == kal_err_not_program,
          "a file that is no image is kal_err_not_program");
    const kal_spawn_streams placed{{0}, {kal_stdin().h + 7}, {0}};
    check(kal_process_spawn(&how, kSelf, length(kSelf), none_argv, none_len, 1, nullptr, nullptr, 0, &placed, &p)
              == kal_err_not_supported,
          "a stream placed at another position is kal_err_not_supported: the firmware gives every image its consoles");
    const char* bad_env[] = {"NOEQUALS"};
    const kal_uintptr bad_len[] = {8};
    check(kal_process_spawn(&how, kSelf, length(kSelf), none_argv, none_len, 1, bad_env, bad_len, 1, &inherit, &p)
              == kal_err_invalid,
          "an entry without `=' is kal_err_invalid");

    say(failures ? "FAILED\n" : "all held\n");
    return failures ? 1 : 42;
}
