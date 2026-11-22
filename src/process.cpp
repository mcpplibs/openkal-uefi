// openkal.process on Boot Services (0.13.0): a program found through openkal.fs, loaded by LoadImage
// and run by StartImage, which returns when the started image has ended.
//
// THE FIRMWARE HAS ONE TASK. StartImage runs the started image on the caller's processor and returns
// its status when it calls Exit, so kal_process_spawn returns after the started program has ended
// and kal_process_wait answers its status at once: KAL_PROCESS_PROP_RUNS_TO_END (clause 7.19). No
// running program remains to end or to speak to, so TERMINATE and CHANNEL are not claimed; a started
// program cannot outlive its starter, so BOUND_LIFETIME holds of every start and is claimed.
//
// WHAT THE STARTED IMAGE RECEIVES. The vector is its load options, one UCS-2 string: each element
// that holds a blank, a double quote or `^', or is empty, is quoted and its `"' and `^' are preceded
// by `^', the division start.cpp makes recovering it (clause 7.6); KAL_SPAWN_ONE_STRING joins the
// elements after the first as they are. The variables and the directory the program runs in are
// the start record (state.h), installed on the started image's handle; a program of this
// implementation reads them, and one of another kind receives its load options alone.
//
// WHAT IT DOES NOT. The three streams are the firmware's consoles, which StartImage gives every
// image: a stream placed at a position other than its own, and the null device, are refused with
// kal_err_not_supported (STREAM_PASSING and NULL_STREAMS are not claimed). A grant of directories
// is not claimed either: the started program receives the volume the image was loaded from, as a
// program the Shell starts does. No unit, no confinement, no identifier to open.
//
// NO BOUND OF ITS OWN (clause 7.6). The load options, the record and the table of started programs
// are sized by what a start gives; the one bound left is the firmware's, the load options' size in
// thirty-two bits, and a vector beyond it is kal_err_no_space.
#include <openkal/memory.h>
#include <openkal/process.h>
#include <openkal/time.h>

#include "state.h"
#include "text.h"
#include "uefi.h"

namespace {

// ── the programs started ────────────────────────────────────────────────────

struct started {
    bool used;
    int status;
    int terminated;
    kal_u64 id;
    kal_u64 started_ns;
};

started* g_started = nullptr;
kal_uintptr g_count = 0;   // rows in use or once used
kal_uintptr g_cap = 0;

// This program's own handle (kal_process_self): no row's.
constexpr kal_uintptr kSelf = ~kal_uintptr{0};

// A free row, the table doubled where there is none; zero where there is no memory.
kal_uintptr take_row() {
    for (kal_uintptr i = 0; i < g_count; ++i)
        if (!g_started[i].used) return i + 1;
    if (g_count == g_cap) {
        const kal_uintptr cap = g_cap ? 2 * g_cap : 16;
        auto* grown = static_cast<started*>(kal_alloc(cap * sizeof(started), alignof(started)));
        if (!grown) return 0;
        for (kal_uintptr i = 0; i < g_count; ++i) grown[i] = g_started[i];
        if (g_started) kal_free(g_started, g_cap * sizeof(started), alignof(started));
        g_started = grown;
        g_cap = cap;
    }
    g_started[g_count] = started{};
    return ++g_count;
}

started* row(kal_process p) {
    if (p.h == 0 || p.h > g_count) return nullptr;
    started* s = &g_started[p.h - 1];
    return s->used ? s : nullptr;
}

// ── the vector as the load options ──────────────────────────────────────────

bool special(efi_char16 c) { return c == u' ' || c == u'\t' || c == u'"' || c == u'^'; }

// Memory released when it goes out of scope.
struct held {
    void* p = nullptr;
    kal_uintptr size = 0;
    kal_uintptr align = 1;
    held() = default;
    held(kal_uintptr n, kal_uintptr a) : size(n ? n : 1), align(a) { p = kal_alloc(size, align); }
    ~held() { if (p) kal_free(p, size, align); }
    held(const held&) = delete;
    held& operator=(const held&) = delete;
};

// The command line: each element in UCS-2, quoted as start.cpp divides it. An element that is not
// UTF-8, or holds a NUL, has no UCS-2 form and is kal_err_invalid.
int command_line(const char** argv, const kal_uintptr* lens, kal_uintptr argc, bool one_string,
                 held& line, kal_uintptr& units) {
    kal_uintptr longest = 0, bound = 1;
    for (kal_uintptr i = 0; i < argc; ++i) {
        if (lens[i] > longest) longest = lens[i];
        bound += 2 * lens[i] + 3;   // every unit preceded by `^', two quotes, a separator
    }
    held part((longest + 1) * sizeof(efi_char16), alignof(efi_char16));
    held whole(bound * sizeof(efi_char16), alignof(efi_char16));
    if (!part.p || !whole.p) return kal_err_no_memory;
    auto* w = static_cast<efi_char16*>(part.p);
    auto* o = static_cast<efi_char16*>(whole.p);
    kal_uintptr at = 0;
    for (kal_uintptr i = 0; i < argc; ++i) {
        long long n = 0;
        if (lens[i] > 0) {
            n = okuefi::to_ucs2(argv[i], lens[i], w, longest);
            if (n < 0) return kal_err_invalid;
        }
        if (i) o[at++] = u' ';
        bool quote = n == 0;
        for (long long k = 0; k < n && !quote; ++k) quote = special(w[k]);
        if (one_string && i > 0) quote = false;
        if (quote) o[at++] = u'"';
        for (long long k = 0; k < n; ++k) {
            if (quote && (w[k] == u'"' || w[k] == u'^')) o[at++] = u'^';
            o[at++] = w[k];
        }
        if (quote) o[at++] = u'"';
    }
    o[at] = 0;
    units = at;
    // LoadOptionsSize is thirty-two bits: the firmware's bound, and the only one.
    if ((at + 1) * sizeof(efi_char16) > 0xFFFFFFFFull) return kal_err_no_space;
    line.p = whole.p; line.size = whole.size; line.align = whole.align;
    whole.p = nullptr;
    return kal_ok;
}

// ── the image's device path ─────────────────────────────────────────────────

kal_uintptr node_length(const efi_device_path_node* n) {
    return static_cast<kal_uintptr>(n->length[0]) | static_cast<kal_uintptr>(n->length[1]) << 8;
}

// The device path of the volume with a media file path node naming `file' before its end.
int image_path(const efi_char16* file, held& out) {
    okuefi::state& s = okuefi::self();
    void* p = nullptr;
    if (!s.device || s.st->boot_services->handle_protocol(s.device, &kDevicePathProtocol, &p) != EFI_SUCCESS || !p)
        return kal_err_not_supported;
    const auto* volume = static_cast<const unsigned char*>(p);
    kal_uintptr prefix = 0;
    for (;;) {
        const auto* n = reinterpret_cast<const efi_device_path_node*>(volume + prefix);
        const kal_uintptr len = node_length(n);
        if (n->type == kDevicePathEnd && n->subtype == kDevicePathEndAll) break;
        if (len < sizeof(efi_device_path_node)) return kal_err_io;   // a path the firmware garbled
        prefix += len;
    }
    kal_uintptr units = 0;
    while (file[units]) ++units;
    const kal_uintptr file_node = sizeof(efi_device_path_node) + (units + 1) * sizeof(efi_char16);
    if (file_node > 0xFFFF) return kal_err_no_space;
    held path(prefix + file_node + sizeof(efi_device_path_node), 1);
    if (!path.p) return kal_err_no_memory;
    auto* b = static_cast<unsigned char*>(path.p);
    for (kal_uintptr i = 0; i < prefix; ++i) b[i] = volume[i];
    auto* f = reinterpret_cast<efi_device_path_node*>(b + prefix);
    f->type = kDevicePathMedia;
    f->subtype = kDevicePathFile;
    f->length[0] = static_cast<efi_u8>(file_node & 0xFF);
    f->length[1] = static_cast<efi_u8>(file_node >> 8);
    // The node's text is not aligned: written a byte at a time.
    unsigned char* t = b + prefix + sizeof(efi_device_path_node);
    for (kal_uintptr i = 0; i <= units; ++i) {
        t[2 * i] = static_cast<unsigned char>(file[i] & 0xFF);
        t[2 * i + 1] = static_cast<unsigned char>(file[i] >> 8);
    }
    auto* end = reinterpret_cast<efi_device_path_node*>(b + prefix + file_node);
    *end = efi_device_path_node{kDevicePathEnd, kDevicePathEndAll, {4, 0}};
    out.p = path.p; out.size = path.size; out.align = path.align;
    path.p = nullptr;
    return kal_ok;
}

int load_error(efi_status s) {
    switch (s) {
        case EFI_NOT_FOUND:          return kal_err_not_found;
        case EFI_UNSUPPORTED:
        case EFI_LOAD_ERROR:         return kal_err_not_program;
        case EFI_ACCESS_DENIED:
        case EFI_SECURITY_VIOLATION: return kal_err_permission;
        case EFI_OUT_OF_RESOURCES:   return kal_err_no_memory;
        case EFI_INVALID_PARAMETER:  return kal_err_invalid;
        default:                     return kal_err_io;
    }
}

// ── the environment the record carries ──────────────────────────────────────

bool well_formed(const char* e, kal_uintptr n) {
    kal_uintptr k = 0;
    while (k < n && e[k] != '=') ++k;
    return k > 0 && k < n;
}

constexpr char kResources[] = "KAL_RESOURCES=";
constexpr kal_uintptr kResourcesLen = sizeof kResources - 1;

bool names_resources(const char* e, kal_uintptr n) {
    if (n < kResourcesLen) return false;
    for (kal_uintptr i = 0; i < kResourcesLen; ++i) if (e[i] != kResources[i]) return false;
    return true;
}

constexpr kal_uintptr kKnownFlags = KAL_SPAWN_BOUND_LIFETIME | KAL_SPAWN_ONE_STRING
    | KAL_SPAWN_RESOURCES_BALANCED | KAL_SPAWN_RESOURCES_PERFORMANCE | KAL_SPAWN_RESOURCES_ECONOMY;

// A position inherits where its word is zero or the position's own stream.
bool inherited(const kal_spawn_streams* s) {
    if (!s) return true;
    return (s->in.h == 0) && (s->out.h == 0 || s->out.h == 1) && (s->err.h == 0 || s->err.h == 2);
}

}  // namespace

extern "C" {

kal_uintptr kal_process_props(void) {
    return KAL_PROCESS_PROP_RUNS_TO_END | KAL_PROCESS_PROP_EXIT_STATUS | KAL_PROCESS_PROP_ONE_STRING
         | KAL_PROCESS_PROP_BOUND_LIFETIME;
}

int kal_process_spawn(const kal_spawn* how, const char* path, kal_uintptr path_len,
                      const char** argv, const kal_uintptr* argv_lens, kal_uintptr argc,
                      const char** envp, const kal_uintptr* envp_lens, kal_uintptr envc,
                      const kal_spawn_streams* streams, kal_process* out) {
    okuefi::state& s = okuefi::self();
    if (!how || !path || path_len == 0 || !out || !s.st) return kal_err_invalid;
    if ((argc && (!argv || !argv_lens)) || (envc && (!envp || !envp_lens))) return kal_err_invalid;
    for (kal_uintptr i = 0; i < envc; ++i)
        if (!envp[i] || !well_formed(envp[i], envp_lens[i])) return kal_err_invalid;
    const kal_uintptr prefs = how->flags & (KAL_SPAWN_RESOURCES_BALANCED | KAL_SPAWN_RESOURCES_PERFORMANCE
                                            | KAL_SPAWN_RESOURCES_ECONOMY);
    if (prefs & (prefs - 1)) return kal_err_invalid;
    if (how->flags & ~kKnownFlags) return kal_err_not_supported;
    if (how->grants || how->job || !inherited(streams)) return kal_err_not_supported;
    const efi_char16* work = okuefi::directory_name(how->work);
    if (!work) return kal_err_invalid;

    // The program, named beneath `base': a start that cannot happen is reported by its reason
    // before the firmware is asked.
    efi_char16* file = nullptr;
    const int named = okuefi::program_name(how->base, path, path_len, &file);
    if (named != kal_ok) return named;
    held file_held;
    file_held.p = file;

    held line;
    kal_uintptr units = 0;
    if (const int e = command_line(argv, argv_lens, argc, (how->flags & KAL_SPAWN_ONE_STRING) != 0, line, units);
        e != kal_ok)
        return e;

    // The variables: the caller's, with the preference in place of its KAL_RESOURCES where one is
    // asked for. They are pointed to, not copied: the caller's arrays hold for the whole call.
    const char* pref = prefs == KAL_SPAWN_RESOURCES_PERFORMANCE ? "KAL_RESOURCES=performance"
                     : prefs == KAL_SPAWN_RESOURCES_ECONOMY     ? "KAL_RESOURCES=economy"
                     : prefs == KAL_SPAWN_RESOURCES_BALANCED    ? "KAL_RESOURCES=balanced" : nullptr;
    held entries((envc + 1) * sizeof(const char*), alignof(const char*));
    held lens((envc + 1) * sizeof(kal_uintptr), alignof(kal_uintptr));
    if (!entries.p || !lens.p) return kal_err_no_memory;
    auto* e = static_cast<const char**>(entries.p);
    auto* l = static_cast<kal_uintptr*>(lens.p);
    kal_uintptr count = 0;
    for (kal_uintptr i = 0; i < envc; ++i) {
        if (pref && names_resources(envp[i], envp_lens[i])) continue;
        e[count] = envp[i]; l[count] = envp_lens[i]; ++count;
    }
    if (pref) {
        kal_uintptr n = 0;
        while (pref[n]) ++n;
        e[count] = pref; l[count] = n; ++count;
    }
    okuefi::start_record record{okuefi::kStartMagic, 1, sizeof(okuefi::start_record), e, l, count, work};

    held device_path;
    if (const int r = image_path(file, device_path); r != kal_ok) return r;

    const kal_uintptr index = take_row();
    if (!index) return kal_err_no_memory;

    auto* bs = s.st->boot_services;
    efi_handle child = nullptr;
    efi_status st = bs->load_image(0, s.image, device_path.p, nullptr, 0, &child);
    if (st == EFI_SECURITY_VIOLATION && child) bs->unload_image(child);
    if (st != EFI_SUCCESS) return load_error(st);

    efi_loaded_image_protocol* li = nullptr;
    if (bs->handle_protocol(child, &kLoadedImageProtocol, reinterpret_cast<void**>(&li)) != EFI_SUCCESS || !li) {
        bs->unload_image(child);
        return kal_err_io;
    }
    li->load_options = line.p;
    li->load_options_size = static_cast<efi_u32>((units + 1) * sizeof(efi_char16));
    efi_handle h = child;
    st = bs->install_protocol_interface(&h, &okuefi::kStartRecord, EFI_NATIVE_INTERFACE, &record);
    if (st != EFI_SUCCESS) {
        bs->unload_image(child);
        return load_error(st);
    }

    const kal_u64 started_ns = kal_time_monotonic();
    efi_uintn exit_size = 0;
    efi_char16* exit_data = nullptr;
    st = bs->start_image(child, &exit_size, &exit_data);
    // The record holds the handle once the firmware has unloaded the image; removing it releases
    // both. A start the firmware refused before the image ran leaves the image loaded.
    bs->uninstall_protocol_interface(child, &okuefi::kStartRecord, &record);
    if (st == EFI_SECURITY_VIOLATION || st == EFI_INVALID_PARAMETER) bs->unload_image(child);
    if (exit_data) bs->free_pool(exit_data);

    // THE STATUS: a program of this implementation exits with its status as a warning code
    // (kal.cpp), success being zero; an abort is EFI_ABORTED, reported as an end the program did
    // not choose with the status of an abort elsewhere (134). Another image's error code is its
    // number, which no success shares.
    started& r = g_started[index - 1];
    r = started{true, 0, 0, reinterpret_cast<kal_u64>(child), started_ns};
    if (st == EFI_ABORTED) { r.status = 134; r.terminated = 1; }
    else if (!efi_error(st)) r.status = static_cast<int>(st & 0xFF);
    else r.status = (st & 0xFF) ? static_cast<int>(st & 0xFF) : 1;
    *out = kal_process{index};
    return kal_ok;
}

int kal_process_wait(kal_process p, int* status, int* terminated) {
    started* r = row(p);
    if (!r) return kal_err_invalid;
    if (status) *status = r->status;
    if (terminated) *terminated = r->terminated;
    return kal_ok;
}

// Nothing runs to be ended: a started program has ended before its handle exists.
int kal_process_terminate(kal_process p) { return row(p) ? kal_err_not_supported : kal_err_invalid; }

void kal_process_close(kal_process p) { if (started* r = row(p)) r->used = false; }

int kal_process_channel(kal_stream*, kal_stream*) { return kal_err_not_supported; }
void kal_process_channel_close(kal_stream) {}

// No request to end reaches a program here (STOP_REQUESTED is not claimed): no word, and no request
// whose manner could be reproduced.
const kal_u32* kal_process_stop_requested(void) { return nullptr; }
int kal_process_end_as_requested(void) { return kal_err_invalid; }

int kal_process_self(kal_process* out) {
    if (!out) return kal_err_invalid;
    *out = kal_process{kSelf};
    return kal_ok;
}

int kal_process_open_id(kal_u64, kal_process*) { return kal_err_not_supported; }

// WHAT IS KNOWN OF A PROGRAM: its identifier, the firmware's handle of its image; for a started
// program, when it started (the monotonic clock) and that it has ended. The firmware keeps no
// account of processor time or memory per image.
int kal_process_info(kal_process p, kal_u32 wanted, kal_program_info* out) {
    if (!out || out->self_size < 2 * sizeof(kal_u32)) return kal_err_invalid;
    kal_program_info info{};
    info.self_size = out->self_size;
    if (p.h == kSelf) {
        info.id = reinterpret_cast<kal_u64>(okuefi::self().image);
        info.ended = 0;
        info.present = KAL_PINFO_ID | KAL_PINFO_ENDED;
    } else if (const started* r = row(p)) {
        info.id = r->id;
        info.started_ns = r->started_ns;
        info.ended = 1;
        info.present = KAL_PINFO_ID | KAL_PINFO_STARTED | KAL_PINFO_ENDED;
    } else {
        return kal_err_invalid;
    }
    info.present &= wanted;
    const kal_uintptr n = out->self_size < sizeof info ? out->self_size : sizeof info;
    const auto* from = reinterpret_cast<const unsigned char*>(&info);
    auto* to = reinterpret_cast<unsigned char*>(out);
    for (kal_uintptr i = 0; i < n; ++i) to[i] = from[i];
    return kal_ok;
}

// No unit: KAL_PROCESS_PROP_JOB and its successors are not claimed (clause 6.2).
int kal_process_job_enter(kal_job*) { return kal_err_not_supported; }
int kal_process_job_terminate(kal_job) { return kal_err_not_supported; }
void kal_process_job_close(kal_job) {}
int kal_process_job_create(kal_job*, char*, kal_uintptr, kal_uintptr*) { return kal_err_not_supported; }
int kal_process_job_open(const char*, kal_uintptr, kal_job*) { return kal_err_not_supported; }
int kal_process_job_observe(kal_job, const char*, kal_uintptr, kal_u32*) { return kal_err_not_supported; }
int kal_process_job_retire(kal_job, const char*, kal_uintptr, const kal_program_identity*, kal_u32*) {
    return kal_err_not_supported;
}
int kal_process_job_usage(kal_job, kal_job_usage*) { return kal_err_not_supported; }
int kal_process_job_limit(kal_job, kal_uintptr, kal_u64) { return kal_err_not_supported; }

}  // extern "C"
