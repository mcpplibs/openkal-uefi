// A UEFI application written against openkal, with no UEFI type in sight: a static initialiser,
// the arguments, the two clocks, two over-aligned allocations, the volume it was loaded from, a
// source of random bytes, and a status the board carries out.
//
// Started as the firmware's default boot file, it reads its own volume (read-only under the
// board's runner). Started by the UEFI Shell as `hello.efi --shell "a b" c' (CI's startup.nsh, on
// a writable FAT volume), it also checks the Shell's arguments, writes, renames and removes files
// and directories, leaves okfs/kept.txt for the host to read, and requires a random source.
import openkal.stream;
import openkal.abort;
import openkal.memory;
import openkal.env;
import openkal.time;
import openkal.fs;
import openkal.random;

namespace {
kal_uintptr length(const char* s) {
    kal_uintptr n = 0;
    while (s[n]) ++n;
    return n;
}
void say(const char* s) { kal_stream_write(kal_stdout(), s, length(s)); }
void say(const char* s, kal_uintptr n) { kal_stream_write(kal_stdout(), s, n); }
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
// FAT compares names without regard to case, and keeps the case a name was written in.
bool same_name(const char* a, kal_uintptr n, const char* b) {
    if (n != length(b)) return false;
    const auto fold = [](char c) { return c >= 'a' && c <= 'z' ? static_cast<char>(c - 32) : c; };
    for (kal_uintptr i = 0; i < n; ++i)
        if (fold(a[i]) != fold(b[i])) return false;
    return true;
}
struct initialised {
    int value = 7;
    initialised() { value = 42; }
} g_initialised;

// Whether a listing of `d' holds `name', of kind `kind'.
bool lists(kal_dir d, const char* name, int kind) {
    kal_uintptr it = 0;
    if (kal_fs_list_begin(d, &it) != kal_ok) return false;
    bool found = false;
    char buf[256];
    while (it) {
        kal_uintptr n = 0;
        int k = 0;
        if (kal_fs_list_next(d, &it, buf, sizeof buf, &n, &k) != kal_ok) return false;
        if (!it) break;
        say("         ");
        say(buf, n);
        say(k == kal_node_directory ? "/\n" : "\n");
        if (same_name(buf, n, name) && k == kind) found = true;
    }
    return found;
}

int kind_of(kal_dir d, const char* name) {
    kal_node_info i{};
    i.self_size = sizeof i;
    if (kal_fs_info(d, name, length(name), 0, kal::fs::field::kind, &i) != kal_ok) return -1;
    return (i.present & kal::fs::field::kind) ? i.kind : -1;
}

bool put(kal_dir d, const char* name, const char* text) {
    kal_file f{};
    const kal_uintptr how = (kal::fs::open::write | kal::fs::open::create | kal::fs::open::truncate).bits;
    if (kal_fs_open(d, name, length(name), how, &f) != kal_ok) return false;
    const bool ok = kal_stream_write(kal_fs_stream(f), text, length(text)) == static_cast<kal_intptr>(length(text));
    kal_fs_close_file(f);
    return ok;
}

bool holds(kal_dir d, const char* name, const char* text) {
    kal_file f{};
    if (kal_fs_open(d, name, length(name), kal::fs::open::read.bits, &f) != kal_ok) return false;
    char buf[128];
    kal_uintptr got = 0;
    for (;;) {
        const kal_intptr r = kal_stream_read(kal_fs_stream(f), buf + got, sizeof buf - got);
        if (r <= 0) break;
        got += static_cast<kal_uintptr>(r);
    }
    kal_fs_close_file(f);
    return same(buf, got, text);
}

// What every start can check of the volume: the supplied directory, a listing of it, and the first
// bytes of the program's own image, read through a file's stream.
void read_the_volume(kal_dir root, const char* own_image, const char* listed, int listed_kind) {
    say("the volume, listed:\n");
    check(lists(root, listed, listed_kind), "the program's volume is listed");
    kal_file f{};
    char mz[2] = {};
    const bool opened = kal_fs_open(root, own_image, length(own_image), kal::fs::open::read.bits, &f) == kal_ok;
    if (opened) {
        kal_stream_read(kal_fs_stream(f), mz, 2);
        kal_fs_close_file(f);
    }
    check(opened && mz[0] == 'M' && mz[1] == 'Z', "the program's own image is read from its volume");
    kal_node_info i{};
    i.self_size = sizeof i;
    check(kal_fs_info(root, "no-such-name", 12, 0, kal::fs::field::kind, &i) == kal_ok && i.kind == kal_node_absent,
          "an absent name is an answer, not an error");
    check(kal_fs_open(root, "no-such-name", 12, kal::fs::open::read.bits, &f) == kal_err_not_found,
          "opening an absent name is kal_err_not_found");
}

void write_the_volume(kal_dir root) {
    const char* dir = "okfs";
    kal_fs_mkdir(root, dir, length(dir));        // a previous run may have left it
    check(kind_of(root, dir) == kal_node_directory, "a directory is made");
    check(kal_fs_mkdir(root, dir, length(dir)) == kal_err_exists, "making it again is kal_err_exists");
    kal_dir d{};
    check(kal_fs_open_dir(root, dir, length(dir), &d) == kal_ok, "the directory is opened");

    check(put(d, "note.txt", "written on the volume\n"), "a file is written");
    check(holds(d, "note.txt", "written on the volume\n"), "and read back as written");
    check(put(root, "okfs/kept.txt", "kept by openkal-uefi\n"), "a file is written through a name of two components");

    kal_file f{};
    const kal_uintptr rw = (kal::fs::open::read | kal::fs::open::write).bits;
    check(kal_fs_open(d, "note.txt", 8, rw, &f) == kal_ok, "a file is opened to read and write");
    char at[6] = {};
    check(kal_fs_read_at(f, at, 6, 8) == 6 && same(at, 6, "on the"), "a read at an offset");
    check(kal_fs_write_at(f, "ON", 2, 8) == 2, "a write at an offset");
    kal_u64 pos = 1;
    check(kal_fs_seek(f, 0, kal::fs::seek_current, &pos) == kal_ok && pos == 0, "neither moved the position");
    kal_node_info i{};
    i.self_size = sizeof i;
    check(kal_fs_file_info(f, kal::fs::field::size, &i) == kal_ok && (i.present & kal::fs::field::size) && i.size == 22,
          "an open file's size");
    check(kal_fs_truncate(f, 7) == kal_ok, "a file is shortened");
    kal_fs_close_file(f);
    check(holds(d, "note.txt", "written"), "and holds what was left");

    const kal_uintptr excl = (kal::fs::open::write | kal::fs::open::create | kal::fs::open::exclusive).bits;
    check(kal_fs_open(d, "note.txt", 8, excl, &f) == kal_err_exists, "an exclusive create of a name that exists");

    kal_fs_remove(d, "renamed.txt", 11);
    check(kal_fs_rename(d, "note.txt", 8, d, "renamed.txt", 11) == kal_ok, "a file is renamed");
    check(kind_of(d, "note.txt") == kal_node_absent && kind_of(d, "renamed.txt") == kal_node_file,
          "the old name is absent and the new one present");
    check(put(d, "other.txt", "other\n") &&
              kal_fs_rename_flags(d, "other.txt", 9, d, "renamed.txt", 11, kal::fs::no_replace.bits) == kal_err_exists,
          "a rename that does not replace is refused onto a name that exists");
    check(kal_fs_rename(d, "other.txt", 9, root, "moved.txt", 9) == kal_ok && kind_of(root, "moved.txt") == kal_node_file,
          "a file is renamed into another directory");
    check(kal_fs_remove(root, "moved.txt", 9) == kal_ok && kind_of(root, "moved.txt") == kal_node_absent,
          "a file is removed");
    say("okfs, listed:\n");
    check(lists(d, "renamed.txt", kal_node_file), "the directory is listed");
    check(kal_fs_remove(root, dir, length(dir)) == kal_err_not_empty, "a directory that is not empty is not removed");
    kal_fs_mkdir(d, "empty", 5);
    check(kal_fs_remove(d, "empty", 5) == kal_ok, "an empty directory is removed");
    check(kal_fs_remove(d, "renamed.txt", 11) == kal_ok, "a file is removed by its directory");

    const kal_u64 when = 1767225600ull * 1000000000ull;   // 2026-01-01T00:00:00Z
    check(kal_fs_set_modified_at(d, "kept.txt", 8, when) == kal_ok, "a modification time is set");
    check(kal_fs_info(d, "kept.txt", 8, 0, kal::fs::field::modified, &i) == kal_ok && (i.present & kal::fs::field::modified) &&
              i.modified_ns / 1000000000ull == when / 1000000000ull,
          "and reads back");
    kal_u64 total = 0, available = 0;
    check(kal_fs_capacity(root, &total, &available) == kal_ok && total > 0 && available <= total,
          "the volume's capacity");
    check(kal_fs_link_create(d, "l", 1, "kept.txt", 8, 0) == kal_err_not_supported &&
              !kal::fs::has(root, kal::fs::links),
          "links are not claimed, and not made");
    kal_fs_close_dir(d);
    check(kal_fs_list_begin(d, &pos) == kal_err_invalid, "a closed directory is refused");
}
}  // namespace

int main(int argc, char** argv, char**) {
    say("hello from openkal over UEFI\n");
    check(g_initialised.value == 42, "a static initialiser ran before main");
    check(argc >= 1 && kal_env_arg_count() == static_cast<kal_uintptr>(argc), "the arguments are counted alike");
    const bool shell = argc > 1 && same(argv[1], length(argv[1]), "--shell");
    if (shell) {
        check(argc == 4, "the Shell gave four arguments");
        check(argc == 4 && same(argv[2], length(argv[2]), "a b"), "a quoted argument with a space is one argument");
        check(argc == 4 && same(argv[3], length(argv[3]), "c"), "and the one after it is the next");
    }
    void* p = kal_alloc(4096, 64);
    check(p != nullptr && (reinterpret_cast<kal_uintptr>(p) & 63) == 0, "an over-aligned allocation");
    kal_free(p, 4096, 64);
    // An alignment of a page or more is pages placed at it (0.10.0), which over-allocating would
    // have cost in backed memory (a C library's allocator states 256 MiB for an arena; 16 MiB here
    // fits the 256 MiB machine this example boots in).
    void* arena = kal_alloc(4u << 20, 16u << 20);
    check(arena != nullptr && (reinterpret_cast<kal_uintptr>(arena) & ((16u << 20) - 1)) == 0,
          "an allocation aligned to 16 MiB");
    kal_free(arena, 4u << 20, 16u << 20);
    const kal_duration t0 = kal_time_monotonic();
    kal_time_sleep(20u * 1000u * 1000u);
    const kal_duration t1 = kal_time_monotonic();
    check(t1 - t0 >= 19u * 1000u * 1000u && t1 - t0 < 2000u * 1000u * 1000u, "a sleep of 20 ms is measured as such");
    check(kal_time_wall() > 1700000000ull * 1000000000ull, "the wall clock reads the present");

    // openkal.fs (0.11.0): the volume the program was loaded from, supplied as "/".
    kal_dir root{};
    char name[8];
    kal_uintptr name_len = 0;
    check(kal_fs_preopen_count() == 1 && kal_fs_preopen(0, &root, name, sizeof name, &name_len) == kal_ok &&
              same(name, name_len, "/"),
          "one directory is supplied, the volume's root, named /");
    if (shell) {
        read_the_volume(root, "hello.efi", "startup.nsh", kal_node_file);
        write_the_volume(root);
    } else {
        read_the_volume(root, "EFI/BOOT/BOOTX64.EFI", "EFI", kal_node_directory);
    }

    // openkal.random (0.11.0): the firmware's source, where it has one.
    unsigned char a[32] = {}, b[32] = {};
    const int ra = kal_random_fill(a, sizeof a);
    const int rb = kal_random_fill(b, sizeof b);
    if (ra == kal_err_not_supported && !shell) {
        say("  --   the firmware has no random source (EFI_RNG_PROTOCOL), reported as kal_err_not_supported\n");
    } else {
        bool differ = false, nonzero = false;
        for (unsigned i = 0; i < sizeof a; ++i) {
            if (a[i] != b[i]) differ = true;
            if (a[i]) nonzero = true;
        }
        check(ra == kal_ok && rb == kal_ok && differ && nonzero, "the firmware's random source fills bytes");
    }

    say(failures ? "FAILED\n" : "all held\n");
    return failures ? 1 : 42;
}
