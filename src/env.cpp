// openkal.env on Boot Services: the arguments are the image's options (start.cpp). The firmware keeps
// no environment of variables; a program of this implementation that starts another gives it its
// variables in the start record (0.13.0, state.h), and those are the started program's. A program
// the firmware or the Shell started has none. The firmware names no image, loader or system a
// program could be told, so each of those is absent (clause 7.7).
#include <openkal/env.h>

#include "state.h"

namespace {
kal_uintptr length(const char* s) { kal_uintptr n = 0; while (s[n]) ++n; return n; }

kal_intptr give(const char* s, kal_uintptr n, char* out, kal_uintptr cap) {
    if (out && cap) {
        const kal_uintptr k = n < cap ? n : cap;
        for (kal_uintptr i = 0; i < k; ++i) out[i] = s[i];
    }
    return static_cast<kal_intptr>(n);
}

// The length of an entry's name: what precedes its first `='.
kal_uintptr name_length(const char* e, kal_uintptr n) {
    kal_uintptr k = 0;
    while (k < n && e[k] != '=') ++k;
    return k;
}

const okuefi::start_record* record() { return okuefi::self().start; }
}  // namespace

extern "C" {

kal_uintptr kal_env_arg_count(void) {
    return static_cast<kal_uintptr>(okuefi::self().argc);
}

kal_intptr kal_env_arg(kal_uintptr index, char* out, kal_uintptr cap) {
    const okuefi::state& s = okuefi::self();
    if (index >= static_cast<kal_uintptr>(s.argc)) return -kal_err_not_found;
    const char* a = s.argv[index];
    return give(a, length(a), out, cap);
}

kal_uintptr kal_env_var_count(void) {
    const okuefi::start_record* r = record();
    return r ? r->count : 0;
}

kal_intptr kal_env_var_at(kal_uintptr index, char* out, kal_uintptr cap) {
    const okuefi::start_record* r = record();
    if (!r || index >= r->count) return -kal_err_not_found;
    return give(r->entries[index], name_length(r->entries[index], r->lens[index]), out, cap);
}

kal_intptr kal_env_var(const char* name, kal_uintptr name_len, char* out, kal_uintptr cap) {
    if (!name || name_len == 0) return -kal_err_invalid;
    const okuefi::start_record* r = record();
    for (kal_uintptr i = 0; r && i < r->count; ++i) {
        const char* e = r->entries[i];
        const kal_uintptr n = name_length(e, r->lens[i]);
        if (n != name_len || n == r->lens[i]) continue;
        bool same = true;
        for (kal_uintptr k = 0; k < n && same; ++k) same = e[k] == name[k];
        if (same) return give(e + n + 1, r->lens[i] - n - 1, out, cap);
    }
    return -kal_err_not_found;
}

kal_intptr kal_env_image(char*, kal_uintptr) { return -kal_err_not_found; }
kal_intptr kal_env_loader(char*, kal_uintptr) { return -kal_err_not_found; }
kal_intptr kal_env_system(char*, kal_uintptr) { return -kal_err_not_found; }

// THE RESOURCE PREFERENCE (openkal 0.23) is KAL_RESOURCES, as the other implementations read it: a
// starter that asks for one writes it among the started program's variables (process.cpp).
int kal_env_resources(void) {
    char v[16];
    const kal_intptr n = kal_env_var("KAL_RESOURCES", 13, v, sizeof v);
    const auto is = [&](const char* w) {
        const kal_uintptr k = length(w);
        if (n != static_cast<kal_intptr>(k)) return false;
        for (kal_uintptr i = 0; i < k; ++i) if (v[i] != w[i]) return false;
        return true;
    };
    return is("performance") ? KAL_RESOURCES_PERFORMANCE
         : is("economy")     ? KAL_RESOURCES_ECONOMY
                             : KAL_RESOURCES_BALANCED;
}

}  // extern "C"
