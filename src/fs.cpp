// openkal.fs on Boot Services (0.11.0): the volume the program was loaded from, through the simple
// file system protocol and the file protocol of its handles.
//
// THE PREOPEN IS THE VOLUME'S ROOT. The firmware has no namespace of paths a program shares: a
// volume is a device handle carrying EFI_SIMPLE_FILE_SYSTEM_PROTOCOL, opened for its root
// directory. The one directory supplied is the root of the volume the image was loaded from
// (EFI_LOADED_IMAGE_PROTOCOL.DeviceHandle), named "/", so that a C library above resolves an
// absolute name against it and its working directory is the root.
//
// A HANDLE IS A SLOT AND A GENERATION. Each directory, file and listing is a slot of a table here
// holding the firmware's EFI_FILE_PROTOCOL; the word is the slot's index with its generation in the
// high half, so that a released word, or a word this implementation never issued, is refused
// (clause 7.2). The table holds what the firmware has no place for --- how a file was opened, a
// directory's name from the root --- and translates nothing.
//
// A NAME is openkal's, `/'-separated UTF-8, and becomes the firmware's, `\'-separated UCS-2, one
// unit at a time; "." is the directory itself, which the firmware's own naming reserves as well.
//
// WHAT THE FIRMWARE HAS NO SERVICE FOR is answered before it is asked (clause 6.2): kal_fs_props
// claims capacity, a modification time and a rename that does not replace, and no other position.
// A FAT volume has no links, no locks, no executable property and no identity of a node; the
// operations those positions govern report kal_err_not_supported, and a node's identity is left
// clear in `present'. Attenuation is not offered. A rename over an existing name removes it first,
// so a rename is not claimed atomic; one environment task runs, so nothing comes between the two.
#include <openkal/fs.h>
#include <openkal/memory.h>
#include <openkal/stream.h>

#include "uefi.h"
#include "state.h"
#include "text.h"

namespace {

// ── the table ───────────────────────────────────────────────────────────────

enum : unsigned char { kFree = 0, kDir, kFile, kList };

struct node {
    efi_file_protocol* f;
    efi_char16* path;        // a directory's name from the root, `\'-separated; "" for the root
    kal_uintptr flags;       // a file's KAL_OPEN_* flags
    kal_uintptr owner;       // a listing's directory
    unsigned generation;
    unsigned char kind;
};

constexpr unsigned kSlots = 1024;
node g_nodes[kSlots];
kal_uintptr g_root = 0;      // the preopen's word while it is held
bool g_read_only = false;    // the volume states it is read-only

kal_uintptr word_of(unsigned i) {
    return (static_cast<kal_uintptr>(g_nodes[i].generation) << 32) | (i + 1);
}

node* take(kal_uintptr h, unsigned char kind) {
    const kal_uintptr i = (h & 0xFFFFFFFFu) - 1;
    if (i >= kSlots) return nullptr;
    node& n = g_nodes[i];
    if (n.kind != kind || n.generation != static_cast<unsigned>(h >> 32)) return nullptr;
    return &n;
}

kal_uintptr issue(unsigned char kind, efi_file_protocol* f) {
    for (unsigned i = 0; i < kSlots; ++i) {
        node& n = g_nodes[i];
        if (n.kind != kFree) continue;
        if (n.generation == 0) n.generation = 1;
        n.kind = kind;
        n.f = f;
        n.path = nullptr;
        n.flags = 0;
        n.owner = 0;
        return word_of(i);
    }
    return 0;
}

void retire(node& n) {
    if (n.path) kal_free(n.path, 0, 0);
    n.path = nullptr;
    n.f = nullptr;
    n.kind = kFree;
    if (++n.generation == 0) n.generation = 1;
}

int error_of(efi_status s) {
    switch (s) {
        case EFI_NOT_FOUND:         return kal_err_not_found;
        case EFI_ACCESS_DENIED:
        case EFI_WRITE_PROTECTED:   return kal_err_permission;
        case EFI_VOLUME_FULL:       return kal_err_no_space;
        case EFI_OUT_OF_RESOURCES:  return kal_err_no_memory;
        case EFI_INVALID_PARAMETER:
        case EFI_BAD_BUFFER_SIZE:   return kal_err_invalid;
        case EFI_UNSUPPORTED:       return kal_err_not_supported;
        default:                    return kal_err_io;
    }
}

// ── names ───────────────────────────────────────────────────────────────────

constexpr kal_uintptr kMaxName = 1024;

// A name this interface accepts (fs.h): not empty, not beginning with a separator, no empty
// component and none that ascends; and no `\', which the firmware reads as a separator.
struct name16 {
    efi_char16 w[kMaxName + 1];
    long long n = -1;
    name16(const char* s, kal_uintptr len) {
        if (!s || len == 0 || len > kMaxName || s[0] == '/') return;
        kal_uintptr start = 0;
        for (kal_uintptr i = 0; i <= len; ++i) {
            if (i < len && s[i] == '\\') return;
            if (i == len || s[i] == '/') {
                const kal_uintptr k = i - start;
                if (k == 0) return;
                if (k == 2 && s[start] == '.' && s[start + 1] == '.') return;
                start = i + 1;
            }
        }
        n = okuefi::to_ucs2(s, len, w, kMaxName);
        for (long long i = 0; i < n; ++i)
            if (w[i] == u'/') w[i] = u'\\';
    }
    bool ok() const { return n > 0; }
    bool self() const { return n == 1 && w[0] == u'.'; }
};

kal_uintptr length16(const efi_char16* w) { kal_uintptr n = 0; while (w[n]) ++n; return n; }

// `base\name', the name of a node from the root; null where there is no memory for it.
efi_char16* joined(const efi_char16* base, const efi_char16* name) {
    const kal_uintptr a = base ? length16(base) : 0, b = length16(name);
    auto* p = static_cast<efi_char16*>(kal_alloc((a + b + 2) * sizeof(efi_char16), alignof(efi_char16)));
    if (!p) return nullptr;
    kal_uintptr o = 0;
    for (kal_uintptr i = 0; i < a; ++i) p[o++] = base[i];
    if (a) p[o++] = u'\\';
    for (kal_uintptr i = 0; i < b; ++i) p[o++] = name[i];
    p[o] = 0;
    return p;
}

// ── what the firmware states of a node ──────────────────────────────────────

// EFI_FILE_INFO of a handle, in `small' where it fits and in an allocation where it does not.
struct file_info {
    alignas(8) unsigned char small[kFileInfoFixed + 2 * 260];
    efi_file_info* info = nullptr;
    efi_uintn size = 0;
    void* big = nullptr;
    efi_status status = EFI_DEVICE_ERROR;
    explicit file_info(efi_file_protocol* f) {
        size = sizeof small;
        status = f->get_info(f, &kFileInfo, &size, small);
        if (status == EFI_SUCCESS) { info = reinterpret_cast<efi_file_info*>(small); return; }
        if (status != EFI_BUFFER_TOO_SMALL) return;
        big = kal_alloc(size, 8);
        if (!big) { status = EFI_OUT_OF_RESOURCES; return; }
        status = f->get_info(f, &kFileInfo, &size, big);
        if (status == EFI_SUCCESS) info = static_cast<efi_file_info*>(big);
    }
    ~file_info() { if (big) kal_free(big, 0, 0); }
    file_info(const file_info&) = delete;
    file_info& operator=(const file_info&) = delete;
};

bool is_directory(const efi_file_info* i) { return (i->attribute & EFI_FILE_DIRECTORY) != 0; }

// The fields of kal_node_info that EFI_FILE_INFO gives: kind, size, modification time and whether
// the node may be written. A node has no identity the firmware states, and no executable property.
// No more of `out' than its self_size is written, and a position is stated only for a field that is
// written.
int describe(const efi_file_info* i, kal_u32 wanted, kal_node_info* out) {
    if (!out || out->self_size < 2 * sizeof(kal_u32)) return kal_err_invalid;
    kal_node_info v{};
    v.self_size = out->self_size;
    v.kind = is_directory(i) ? kal_node_directory : kal_node_file;
    v.size = i->file_size;
    v.modified_ns = okuefi::ns_of(i->modification_time);
    v.writable = !g_read_only && !(i->attribute & EFI_FILE_READ_ONLY);
    kal_u32 present = KAL_INFO_KIND | KAL_INFO_SIZE | KAL_INFO_WRITABLE;
    if (v.modified_ns) present |= KAL_INFO_MODIFIED;
    (void)wanted;   // everything the firmware gives is in one answer
    const kal_uintptr room = out->self_size < sizeof v ? out->self_size : sizeof v;
    const auto fits = [&](kal_uintptr offset, kal_uintptr size) { return offset + size <= room; };
    if (!fits(__builtin_offsetof(kal_node_info, kind), sizeof v.kind)) present &= ~KAL_INFO_KIND;
    if (!fits(__builtin_offsetof(kal_node_info, size), sizeof v.size)) present &= ~KAL_INFO_SIZE;
    if (!fits(__builtin_offsetof(kal_node_info, modified_ns), sizeof v.modified_ns)) present &= ~KAL_INFO_MODIFIED;
    if (!fits(__builtin_offsetof(kal_node_info, writable), sizeof v.writable)) present &= ~KAL_INFO_WRITABLE;
    v.present = present;
    const auto* from = reinterpret_cast<const unsigned char*>(&v);
    auto* to = reinterpret_cast<unsigned char*>(out);
    for (kal_uintptr k = 0; k < room; ++k) to[k] = from[k];
    return kal_ok;
}

// Sets what a handle's EFI_FILE_INFO states, through a copy: the times are cleared, which the
// firmware reads as "keep them", unless `modified' is given; the name is kept unless `rename' is.
efi_status amend(efi_file_protocol* f, const unsigned long long* size, const efi_time* modified,
                 const efi_char16* rename) {
    file_info fi(f);
    if (!fi.info) return fi.status;
    const kal_uintptr name = rename ? length16(rename) : length16(fi.info->file_name);
    const efi_uintn bytes = kFileInfoFixed + (name + 1) * sizeof(efi_char16);
    auto* p = static_cast<efi_file_info*>(kal_alloc(bytes, 8));
    if (!p) return EFI_OUT_OF_RESOURCES;
    const auto* src = reinterpret_cast<const unsigned char*>(fi.info);
    auto* dst = reinterpret_cast<unsigned char*>(p);
    for (efi_uintn k = 0; k < kFileInfoFixed; ++k) dst[k] = src[k];
    const efi_char16* n = rename ? rename : fi.info->file_name;
    for (kal_uintptr k = 0; k <= name; ++k) p->file_name[k] = n[k];
    p->size = bytes;
    p->create_time = efi_time{};
    p->last_access_time = efi_time{};
    p->modification_time = modified ? *modified : efi_time{};
    if (size) p->file_size = *size;
    const efi_status s = f->set_info(f, &kFileInfo, bytes, p);
    kal_free(p, 0, 0);
    return s;
}

// ── the volume ──────────────────────────────────────────────────────────────

kal_uintptr root() {
    if (g_root && take(g_root, kDir)) return g_root;
    g_root = 0;
    const okuefi::state& s = okuefi::self();
    if (!s.st || !s.device) return 0;
    void* p = nullptr;
    if (s.st->boot_services->handle_protocol(s.device, &kSimpleFileSystemProtocol, &p) != EFI_SUCCESS || !p)
        return 0;
    auto* sfs = static_cast<efi_simple_file_system_protocol*>(p);
    efi_file_protocol* f = nullptr;
    if (sfs->open_volume(sfs, &f) != EFI_SUCCESS || !f) return 0;
    alignas(8) unsigned char buf[256];
    efi_uintn size = sizeof buf;
    if (f->get_info(f, &kFileSystemInfo, &size, buf) == EFI_SUCCESS)
        g_read_only = reinterpret_cast<efi_file_system_info*>(buf)->read_only != 0;
    const kal_uintptr h = issue(kDir, f);
    if (!h) { f->close(f); return 0; }
    node* n = take(h, kDir);
    n->path = joined(nullptr, u"");
    if (!n->path) { f->close(f); retire(*n); return 0; }
    return g_root = h;
}

// Opens `name' beneath a directory with the firmware's mode; "." is the directory itself.
efi_status open_beneath(node* d, const name16& name, unsigned long long mode, unsigned long long attributes,
                        efi_file_protocol** out) {
    *out = nullptr;
    return d->f->open(d->f, out, name.w, mode, attributes);
}

// Whether a directory holds anything but its own two entries.
int emptiness(efi_file_protocol* dir, bool* empty) {
    efi_file_protocol* h = nullptr;
    efi_status s = dir->open(dir, &h, u".", EFI_FILE_MODE_READ, 0);
    if (s != EFI_SUCCESS) return error_of(s);
    *empty = true;
    alignas(8) unsigned char buf[kFileInfoFixed + 2 * 260];
    for (;;) {
        efi_uintn size = sizeof buf;
        s = h->read(h, &size, buf);
        if (s == EFI_BUFFER_TOO_SMALL) { *empty = false; break; }   // a long name is an entry
        if (s != EFI_SUCCESS) { h->close(h); return error_of(s); }
        if (size == 0) break;
        const efi_char16* n = reinterpret_cast<efi_file_info*>(buf)->file_name;
        if ((n[0] == u'.' && n[1] == 0) || (n[0] == u'.' && n[1] == u'.' && n[2] == 0)) continue;
        *empty = false;
        break;
    }
    h->close(h);
    return kal_ok;
}

// ── a file's transfers ──────────────────────────────────────────────────────

kal_intptr read_file(node* n, void* buf, kal_uintptr len) {
    if (!(n->flags & KAL_OPEN_READ)) return -kal_err_invalid;
    if (len == 0) return 0;
    if (!buf) return -kal_err_invalid;
    efi_uintn size = len;
    const efi_status s = n->f->read(n->f, &size, buf);
    if (s == EFI_SUCCESS) return static_cast<kal_intptr>(size);
    // A position beyond the end is the end, which the firmware reports as a device's failure.
    if (s == EFI_DEVICE_ERROR) {
        unsigned long long at = 0;
        file_info fi(n->f);
        if (fi.info && n->f->get_position(n->f, &at) == EFI_SUCCESS && at >= fi.info->file_size) return 0;
    }
    return -error_of(s);
}

kal_intptr write_file(node* n, const void* buf, kal_uintptr len) {
    if (!(n->flags & KAL_OPEN_WRITE)) return -kal_err_invalid;
    if (len == 0) return 0;
    if (!buf) return -kal_err_invalid;
    // Appending: one task runs, so the end found here is the end written at.
    if (n->flags & KAL_OPEN_APPEND) n->f->set_position(n->f, EFI_FILE_POSITION_END);
    const auto* p = static_cast<const unsigned char*>(buf);
    kal_uintptr done = 0;
    while (done < len) {
        efi_uintn size = len - done;
        const efi_status s = n->f->write(n->f, &size, p + done);
        if (s != EFI_SUCCESS) return done ? static_cast<kal_intptr>(done) : -error_of(s);
        if (size == 0) return done ? static_cast<kal_intptr>(done) : -kal_err_io;
        done += size;
    }
    return static_cast<kal_intptr>(done);
}

}  // namespace

// ── openkal.stream, for a file's stream ─────────────────────────────────────

namespace okuefi {
bool file_stream(kal_uintptr h) { return take(h, kFile) != nullptr; }
kal_intptr file_write(kal_uintptr h, const void* buf, kal_uintptr n) {
    node* f = take(h, kFile);
    return f ? write_file(f, buf, n) : -kal_err_invalid;
}
kal_intptr file_read(kal_uintptr h, void* buf, kal_uintptr n) {
    node* f = take(h, kFile);
    return f ? read_file(f, buf, n) : -kal_err_invalid;
}
int file_flush(kal_uintptr h) {
    node* f = take(h, kFile);
    if (!f) return kal_err_invalid;
    if (!(f->flags & KAL_OPEN_WRITE)) return kal_ok;
    const efi_status s = f->f->flush(f->f);
    return s == EFI_SUCCESS ? kal_ok : error_of(s);
}
}  // namespace okuefi

extern "C" {

// ── directories ─────────────────────────────────────────────────────────────

kal_uintptr kal_fs_preopen_count(void) { return root() ? 1 : 0; }

int kal_fs_preopen(kal_uintptr index, kal_dir* out, char* name_out, kal_uintptr name_cap,
                   kal_uintptr* name_len) {
    if (index != 0 || !out) return kal_err_invalid;
    const kal_uintptr h = root();
    if (!h) return kal_err_invalid;
    *out = kal_dir{h};
    if (name_out && name_cap) name_out[0] = '/';
    if (name_len) *name_len = 1;
    return kal_ok;
}

// FAT compares names without regard to case and has no links; it records a modification time, its
// capacity is stated (EFI_FILE_SYSTEM_INFO), and a rename into a name that exists is refused by
// the firmware, which is the rename that does not replace.
kal_uintptr kal_fs_props(kal_dir d) {
    if (!take(d.h, kDir)) return 0;
    return KAL_FS_PROP_MODIFIED_TIME | KAL_FS_PROP_CAPACITY | KAL_FS_PROP_RENAME_NO_REPLACE;
}

int kal_fs_open_dir(kal_dir base, const char* name, kal_uintptr len, kal_dir* out) {
    node* d = take(base.h, kDir);
    const name16 n(name, len);
    if (!d || !out || !n.ok()) return kal_err_invalid;
    efi_file_protocol* f = nullptr;
    const efi_status s = open_beneath(d, n, EFI_FILE_MODE_READ, 0, &f);
    if (s != EFI_SUCCESS) return error_of(s);
    {
        file_info fi(f);
        if (!fi.info) { f->close(f); return error_of(fi.status); }
        if (!is_directory(fi.info)) { f->close(f); return kal_err_not_directory; }
    }
    efi_char16* path = n.self() ? joined(nullptr, d->path) : joined(d->path, n.w);
    const kal_uintptr h = path ? issue(kDir, f) : 0;
    if (!h) { if (path) kal_free(path, 0, 0); f->close(f); return kal_err_no_memory; }
    take(h, kDir)->path = path;
    *out = kal_dir{h};
    return kal_ok;
}

int kal_fs_open(kal_dir base, const char* name, kal_uintptr len, kal_uintptr flags, kal_file* out) {
    node* d = take(base.h, kDir);
    const name16 n(name, len);
    constexpr kal_uintptr known = KAL_OPEN_READ | KAL_OPEN_WRITE | KAL_OPEN_CREATE | KAL_OPEN_EXCLUSIVE |
                                  KAL_OPEN_TRUNCATE | KAL_OPEN_APPEND;
    if (!d || !out || !n.ok() || (flags & ~known) || !(flags & (KAL_OPEN_READ | KAL_OPEN_WRITE)))
        return kal_err_invalid;
    // The firmware's modes all read; writing is asked for by any intent that writes.
    if (flags & (KAL_OPEN_CREATE | KAL_OPEN_TRUNCATE | KAL_OPEN_APPEND)) flags |= KAL_OPEN_WRITE;
    unsigned long long mode = EFI_FILE_MODE_READ;
    if (flags & KAL_OPEN_WRITE) mode |= EFI_FILE_MODE_WRITE;
    efi_file_protocol* f = nullptr;
    // Exclusion: the firmware's create opens what exists, so existence is asked first. One task
    // runs, and nothing creates the name between the question and the creation.
    if ((flags & KAL_OPEN_CREATE) && (flags & KAL_OPEN_EXCLUSIVE)) {
        if (open_beneath(d, n, EFI_FILE_MODE_READ, 0, &f) == EFI_SUCCESS) { f->close(f); return kal_err_exists; }
    }
    if (flags & KAL_OPEN_CREATE) mode |= EFI_FILE_MODE_CREATE;
    efi_status s = open_beneath(d, n, mode, 0, &f);
    if (s != EFI_SUCCESS) return error_of(s);
    {
        file_info fi(f);
        if (!fi.info) { f->close(f); return error_of(fi.status); }
        if (is_directory(fi.info)) { f->close(f); return kal_err_is_directory; }
        if ((flags & KAL_OPEN_TRUNCATE) && fi.info->file_size != 0) {
            const unsigned long long zero = 0;
            s = amend(f, &zero, nullptr, nullptr);
            if (s != EFI_SUCCESS) { f->close(f); return error_of(s); }
        }
    }
    const kal_uintptr h = issue(kFile, f);
    if (!h) { f->close(f); return kal_err_no_memory; }
    take(h, kFile)->flags = flags;
    *out = kal_file{h};
    return kal_ok;
}

kal_uintptr kal_fs_max_name(void) { return kMaxName; }

// Closing a directory releases the listings it produced (fs.h).
void kal_fs_close_dir(kal_dir d) {
    node* n = take(d.h, kDir);
    if (!n) return;
    for (unsigned i = 0; i < kSlots; ++i) {
        node& l = g_nodes[i];
        if (l.kind == kList && l.owner == d.h) { l.f->close(l.f); retire(l); }
    }
    n->f->close(n->f);
    retire(*n);
    if (d.h == g_root) g_root = 0;
}

void kal_fs_close_file(kal_file file) {
    node* n = take(file.h, kFile);
    if (!n) return;
    n->f->close(n->f);
    retire(*n);
}

kal_stream kal_fs_stream(kal_file file) {
    return take(file.h, kFile) ? kal_stream{file.h} : kal_stream{~kal_uintptr{0}};
}

int kal_fs_seek(kal_file file, kal_i64 offset, int whence, kal_u64* result) {
    node* n = take(file.h, kFile);
    if (!n) return kal_err_invalid;
    long long from = 0;
    if (whence == KAL_SEEK_CURRENT) {
        unsigned long long at = 0;
        const efi_status s = n->f->get_position(n->f, &at);
        if (s != EFI_SUCCESS) return error_of(s);
        from = static_cast<long long>(at);
    } else if (whence == KAL_SEEK_END) {
        file_info fi(n->f);
        if (!fi.info) return error_of(fi.status);
        from = static_cast<long long>(fi.info->file_size);
    } else if (whence != KAL_SEEK_SET) {
        return kal_err_invalid;
    }
    const long long to = from + offset;
    if (to < 0) return kal_err_invalid;
    const efi_status s = n->f->set_position(n->f, static_cast<unsigned long long>(to));
    if (s != EFI_SUCCESS) return error_of(s);
    if (result) *result = static_cast<kal_u64>(to);
    return kal_ok;
}

// Extending writes zero bytes (the firmware's own extension does), and discarding releases them.
int kal_fs_truncate(kal_file file, kal_u64 size) {
    node* n = take(file.h, kFile);
    if (!n || !(n->flags & KAL_OPEN_WRITE)) return kal_err_invalid;
    const unsigned long long to = size;
    const efi_status s = amend(n->f, &to, nullptr, nullptr);
    return s == EFI_SUCCESS ? kal_ok : error_of(s);
}

// At an offset: the position is kept and restored around the transfer. One task runs, so no other
// transfer observes the position in between.
kal_intptr kal_fs_read_at(kal_file file, void* buf, kal_uintptr len, kal_u64 offset) {
    node* n = take(file.h, kFile);
    if (!n) return -kal_err_invalid;
    if (!(n->flags & KAL_OPEN_READ)) return -kal_err_invalid;
    {
        file_info fi(n->f);
        if (!fi.info) return -error_of(fi.status);
        if (offset >= fi.info->file_size) return 0;
    }
    unsigned long long was = 0;
    efi_status s = n->f->get_position(n->f, &was);
    if (s != EFI_SUCCESS) return -error_of(s);
    s = n->f->set_position(n->f, offset);
    if (s != EFI_SUCCESS) return -error_of(s);
    const kal_intptr r = read_file(n, buf, len);
    n->f->set_position(n->f, was);
    return r;
}

kal_intptr kal_fs_write_at(kal_file file, const void* buf, kal_uintptr len, kal_u64 offset) {
    node* n = take(file.h, kFile);
    if (!n) return -kal_err_invalid;
    if (!(n->flags & KAL_OPEN_WRITE)) return -kal_err_invalid;
    if (len == 0) return 0;
    {
        // A write beyond the end leaves zero bytes between: the file is extended to the offset
        // first, which the firmware fills.
        file_info fi(n->f);
        if (!fi.info) return -error_of(fi.status);
        if (offset > fi.info->file_size) {
            const unsigned long long to = offset;
            const efi_status s = amend(n->f, &to, nullptr, nullptr);
            if (s != EFI_SUCCESS) return -error_of(s);
        }
    }
    unsigned long long was = 0;
    efi_status s = n->f->get_position(n->f, &was);
    if (s != EFI_SUCCESS) return -error_of(s);
    s = n->f->set_position(n->f, offset);
    if (s != EFI_SUCCESS) return -error_of(s);
    const kal_uintptr keep = n->flags;
    n->flags &= ~KAL_OPEN_APPEND;     // at the offset (fs.h permits either)
    const kal_intptr r = write_file(n, buf, len);
    n->flags = keep;
    n->f->set_position(n->f, was);
    return r;
}

// ── enquiry, creation and removal ───────────────────────────────────────────

// There are no links, so the name and what it finally refers to are one node and KAL_FS_NO_RESOLVE
// changes nothing. A name that is absent, or beneath a component that is not a directory, is an
// answer (clause 7.7).
int kal_fs_info(kal_dir base, const char* name, kal_uintptr len, kal_uintptr flags, kal_u32 wanted,
                kal_node_info* out) {
    node* d = take(base.h, kDir);
    const name16 n(name, len);
    if (!d || !n.ok() || (flags & ~KAL_FS_NO_RESOLVE) || !out || out->self_size < 2 * sizeof(kal_u32))
        return kal_err_invalid;
    if (n.self()) {
        file_info fi(d->f);
        return fi.info ? describe(fi.info, wanted, out) : error_of(fi.status);
    }
    efi_file_protocol* f = nullptr;
    const efi_status s = open_beneath(d, n, EFI_FILE_MODE_READ, 0, &f);
    if (s == EFI_NOT_FOUND) {
        out->present = 0;
        if (out->self_size >= __builtin_offsetof(kal_node_info, kind) + sizeof(int)) {
            out->kind = kal_node_absent;
            out->present = KAL_INFO_KIND;
        }
        return kal_ok;
    }
    if (s != EFI_SUCCESS) return error_of(s);
    int r;
    {
        file_info fi(f);
        r = fi.info ? describe(fi.info, wanted, out) : error_of(fi.status);
    }
    f->close(f);
    return r;
}

int kal_fs_mkdir(kal_dir base, const char* name, kal_uintptr len) {
    node* d = take(base.h, kDir);
    const name16 n(name, len);
    if (!d || !n.ok()) return kal_err_invalid;
    if (n.self()) return kal_err_exists;
    efi_file_protocol* f = nullptr;
    if (open_beneath(d, n, EFI_FILE_MODE_READ, 0, &f) == EFI_SUCCESS) { f->close(f); return kal_err_exists; }
    const efi_status s = open_beneath(d, n, EFI_FILE_MODE_READ | EFI_FILE_MODE_WRITE | EFI_FILE_MODE_CREATE,
                                      EFI_FILE_DIRECTORY, &f);
    if (s != EFI_SUCCESS) return error_of(s);
    f->close(f);
    return kal_ok;
}

// A directory must be empty; the firmware's Delete reports a refusal as a warning that does not
// say why, so emptiness is asked first.
int kal_fs_remove(kal_dir base, const char* name, kal_uintptr len) {
    node* d = take(base.h, kDir);
    const name16 n(name, len);
    if (!d || !n.ok() || n.self()) return kal_err_invalid;
    efi_file_protocol* f = nullptr;
    efi_status s = open_beneath(d, n, EFI_FILE_MODE_READ | EFI_FILE_MODE_WRITE, 0, &f);
    if (s != EFI_SUCCESS) return error_of(s);
    {
        file_info fi(f);
        if (!fi.info) { f->close(f); return error_of(fi.status); }
        if (is_directory(fi.info)) {
            bool empty = false;
            const int e = emptiness(f, &empty);
            if (e != kal_ok) { f->close(f); return e; }
            if (!empty) { f->close(f); return kal_err_not_empty; }
        }
    }
    s = f->remove(f);   // closes the handle in every case
    if (s == EFI_SUCCESS) return kal_ok;
    return s == EFI_WARN_DELETE_FAILURE ? kal_err_permission : error_of(s);
}

// Through the firmware's own rename: the name in EFI_FILE_INFO, set to the destination's name from
// the root. A destination that exists is removed first, as a rename replaces it, unless the caller
// asked that it not be (KAL_RENAME_NO_REPLACE), which the firmware's refusal answers.
int kal_fs_rename_flags(kal_dir from, const char* a, kal_uintptr alen, kal_dir to, const char* b,
                        kal_uintptr blen, kal_uintptr flags) {
    node* df = take(from.h, kDir);
    node* dt = take(to.h, kDir);
    const name16 na(a, alen), nb(b, blen);
    if (!df || !dt || !na.ok() || !nb.ok() || na.self() || nb.self() || (flags & ~KAL_RENAME_NO_REPLACE))
        return kal_err_invalid;
    efi_file_protocol* f = nullptr;
    efi_status s = open_beneath(df, na, EFI_FILE_MODE_READ | EFI_FILE_MODE_WRITE, 0, &f);
    if (s != EFI_SUCCESS) return error_of(s);
    bool source_is_directory = false;
    {
        file_info fi(f);
        if (!fi.info) { f->close(f); return error_of(fi.status); }
        source_is_directory = is_directory(fi.info);
    }
    efi_char16* target = joined(dt->path, nb.w);
    if (!target) { f->close(f); return kal_err_no_memory; }
    // An absolute name, from the root: the firmware reads a name beginning with `\' so.
    const kal_uintptr tl = length16(target);
    auto* absolute = static_cast<efi_char16*>(kal_alloc((tl + 2) * sizeof(efi_char16), alignof(efi_char16)));
    if (!absolute) { kal_free(target, 0, 0); f->close(f); return kal_err_no_memory; }
    absolute[0] = u'\\';
    for (kal_uintptr k = 0; k <= tl; ++k) absolute[k + 1] = target[k];
    kal_free(target, 0, 0);

    int result = kal_ok;
    efi_file_protocol* g = nullptr;
    if (open_beneath(dt, nb, EFI_FILE_MODE_READ | EFI_FILE_MODE_WRITE, 0, &g) == EFI_SUCCESS) {
        // The destination exists. Renaming a node onto itself changes nothing.
        file_info ga(g);
        file_info fa(f);
        bool same = false;
        if (ga.info && fa.info) {
            // The firmware gives no identity; the same node is the same name from the root, which
            // the two handles were opened by (a case-insensitive volume compares it so).
            efi_char16* src = joined(df->path, na.w);
            if (src) {
                const auto fold = [](efi_char16 c) { return c >= u'a' && c <= u'z' ? static_cast<efi_char16>(c - 32) : c; };
                kal_uintptr k = 0;
                same = true;
                for (;; ++k) {
                    if (fold(src[k]) != fold(absolute[k + 1])) { same = false; break; }
                    if (!src[k]) break;
                }
                kal_free(src, 0, 0);
            }
        }
        if (same) { g->close(g); f->close(f); kal_free(absolute, 0, 0); return kal_ok; }
        if (flags & KAL_RENAME_NO_REPLACE) result = kal_err_exists;
        else if (!ga.info) result = error_of(ga.status);
        else if (is_directory(ga.info) && !source_is_directory) result = kal_err_is_directory;
        else if (!is_directory(ga.info) && source_is_directory) result = kal_err_not_directory;
        else if (is_directory(ga.info)) {
            bool empty = false;
            result = emptiness(g, &empty);
            if (result == kal_ok && !empty) result = kal_err_not_empty;
        }
        if (result != kal_ok) { g->close(g); f->close(f); kal_free(absolute, 0, 0); return result; }
        s = g->remove(g);
        if (s != EFI_SUCCESS) {
            f->close(f);
            kal_free(absolute, 0, 0);
            return s == EFI_WARN_DELETE_FAILURE ? kal_err_permission : error_of(s);
        }
    }
    s = amend(f, nullptr, nullptr, absolute);
    kal_free(absolute, 0, 0);
    f->close(f);
    if (s == EFI_SUCCESS) return kal_ok;
    return s == EFI_ACCESS_DENIED && (flags & KAL_RENAME_NO_REPLACE) ? kal_err_exists : error_of(s);
}

int kal_fs_rename(kal_dir from, const char* a, kal_uintptr alen, kal_dir to, const char* b, kal_uintptr blen) {
    return kal_fs_rename_flags(from, a, alen, to, b, blen, 0);
}

int kal_fs_file_info(kal_file file, kal_u32 wanted, kal_node_info* out) {
    node* n = take(file.h, kFile);
    if (!n || !out || out->self_size < 2 * sizeof(kal_u32)) return kal_err_invalid;
    file_info fi(n->f);
    return fi.info ? describe(fi.info, wanted, out) : error_of(fi.status);
}

int kal_fs_set_modified(kal_file file, kal_u64 modified_ns) {
    node* n = take(file.h, kFile);
    if (!n || !(n->flags & KAL_OPEN_WRITE)) return kal_err_invalid;
    const efi_time t = okuefi::time_of(modified_ns);
    const efi_status s = amend(n->f, nullptr, &t, nullptr);
    return s == EFI_SUCCESS ? kal_ok : error_of(s);
}

int kal_fs_set_modified_at(kal_dir base, const char* name, kal_uintptr len, kal_u64 modified_ns) {
    node* d = take(base.h, kDir);
    const name16 n(name, len);
    if (!d || !n.ok()) return kal_err_invalid;
    efi_file_protocol* f = nullptr;
    efi_status s = open_beneath(d, n, EFI_FILE_MODE_READ | EFI_FILE_MODE_WRITE, 0, &f);
    if (s != EFI_SUCCESS) return error_of(s);
    const efi_time t = okuefi::time_of(modified_ns);
    s = amend(f, nullptr, &t, nullptr);
    f->close(f);
    return s == EFI_SUCCESS ? kal_ok : error_of(s);
}

// ── what the volume does not have (clause 6.2: not claimed in kal_fs_props) ──

int kal_fs_set_executable_at(kal_dir base, const char*, kal_uintptr, int) {
    return take(base.h, kDir) ? kal_err_not_supported : kal_err_invalid;
}
int kal_fs_lock(kal_file file, kal_u64, kal_u64, kal_uintptr) {
    return take(file.h, kFile) ? kal_err_not_supported : kal_err_invalid;
}
int kal_fs_unlock(kal_file file, kal_u64, kal_u64) {
    return take(file.h, kFile) ? kal_err_not_supported : kal_err_invalid;
}
int kal_fs_link_create(kal_dir base, const char*, kal_uintptr, const char*, kal_uintptr, kal_uintptr) {
    return take(base.h, kDir) ? kal_err_not_supported : kal_err_invalid;
}
kal_intptr kal_fs_link_read(kal_dir base, const char*, kal_uintptr, char*, kal_uintptr) {
    return take(base.h, kDir) ? -kal_err_not_supported : -kal_err_invalid;
}
int kal_fs_hard_link(kal_dir from, const char*, kal_uintptr, kal_dir to, const char*, kal_uintptr) {
    return take(from.h, kDir) && take(to.h, kDir) ? kal_err_not_supported : kal_err_invalid;
}
kal_intptr kal_fs_resolved_name(kal_dir base, const char*, kal_uintptr, char*, kal_uintptr) {
    return take(base.h, kDir) ? -kal_err_not_supported : -kal_err_invalid;
}
int kal_fs_dir_attenuate(kal_dir d, const kal_dir_rights*, kal_dir*) {
    return take(d.h, kDir) ? kal_err_not_supported : kal_err_invalid;
}

// ── capacity ────────────────────────────────────────────────────────────────

int kal_fs_capacity(kal_dir d, kal_u64* total, kal_u64* available) {
    node* n = take(d.h, kDir);
    if (!n) return kal_err_invalid;
    alignas(8) unsigned char small[256];
    void* buf = small;
    efi_uintn size = sizeof small;
    efi_status s = n->f->get_info(n->f, &kFileSystemInfo, &size, buf);
    if (s == EFI_BUFFER_TOO_SMALL) {
        buf = kal_alloc(size, 8);
        if (!buf) return kal_err_no_memory;
        s = n->f->get_info(n->f, &kFileSystemInfo, &size, buf);
    }
    if (s == EFI_SUCCESS) {
        const auto* i = static_cast<const efi_file_system_info*>(buf);
        if (total) *total = i->volume_size;
        if (available) *available = i->free_space;
    }
    if (buf != small) kal_free(buf, 0, 0);
    return s == EFI_SUCCESS ? kal_ok : error_of(s);
}

// ── enumeration ─────────────────────────────────────────────────────────────
//
// A listing is a second handle on the directory ("."), whose reads give an EFI_FILE_INFO for each
// entry; the directory's own two entries are not reported. The listing is a slot, released at its
// end or with its directory.

int kal_fs_list_begin(kal_dir d, kal_uintptr* iter) {
    node* n = take(d.h, kDir);
    if (!n || !iter) return kal_err_invalid;
    efi_file_protocol* f = nullptr;
    const efi_status s = n->f->open(n->f, &f, u".", EFI_FILE_MODE_READ, 0);
    if (s != EFI_SUCCESS) return error_of(s);
    const kal_uintptr h = issue(kList, f);
    if (!h) { f->close(f); return kal_err_no_memory; }
    take(h, kList)->owner = d.h;
    *iter = h;
    return kal_ok;
}

int kal_fs_list_next_info(kal_dir d, kal_uintptr* iter, char* name_out, kal_uintptr name_cap,
                          kal_uintptr* name_len, kal_u32 wanted, kal_node_info* info) {
    if (!take(d.h, kDir) || !iter || *iter == 0) return kal_err_invalid;
    node* l = take(*iter, kList);
    if (!l || l->owner != d.h) return kal_err_invalid;
    if (info && info->self_size < 2 * sizeof(kal_u32)) return kal_err_invalid;
    alignas(8) unsigned char small[kFileInfoFixed + 2 * 260];
    void* buf = small;
    efi_uintn cap = sizeof small;
    for (;;) {
        efi_uintn size = cap;
        efi_status s = l->f->read(l->f, &size, buf);
        if (s == EFI_BUFFER_TOO_SMALL) {
            if (buf != small) kal_free(buf, 0, 0);
            buf = kal_alloc(size, 8);
            if (!buf) return kal_err_no_memory;
            cap = size;
            s = l->f->read(l->f, &size, buf);
        }
        if (s != EFI_SUCCESS) { if (buf != small) kal_free(buf, 0, 0); return error_of(s); }
        if (size == 0) {
            // The end: the iterator is released and becomes zero, with a name of length zero.
            l->f->close(l->f);
            retire(*l);
            *iter = 0;
            if (name_len) *name_len = 0;
            if (info) {
                info->present = 0;
            }
            if (buf != small) kal_free(buf, 0, 0);
            return kal_ok;
        }
        const auto* e = static_cast<const efi_file_info*>(buf);
        const efi_char16* w = e->file_name;
        if ((w[0] == u'.' && w[1] == 0) || (w[0] == u'.' && w[1] == u'.' && w[2] == 0)) continue;
        const kal_uintptr n = okuefi::to_utf8(w, length16(w), name_out, name_cap);
        if (name_len) *name_len = n;
        int r = kal_ok;
        if (info) r = describe(e, wanted, info);
        if (buf != small) kal_free(buf, 0, 0);
        return r;
    }
}

int kal_fs_list_next(kal_dir d, kal_uintptr* iter, char* name_out, kal_uintptr name_cap,
                     kal_uintptr* name_len, int* kind) {
    kal_node_info i{};
    i.self_size = sizeof i;
    const int r = kal_fs_list_next_info(d, iter, name_out, name_cap, name_len, KAL_INFO_KIND, &i);
    if (r == kal_ok && kind) *kind = *iter ? i.kind : kal_node_absent;
    return r;
}

}  // extern "C"
