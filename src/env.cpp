// openkal.env on Boot Services: the arguments are the image's options (start.cpp); the firmware
// keeps no environment of variables a program inherits, and names no image, loader or system a
// program could be told, so each of those is absent (clause 7.7).
#include <openkal/env.h>

#include "state.h"

namespace {
kal_uintptr length(const char* s) { kal_uintptr n = 0; while (s[n]) ++n; return n; }
}  // namespace

extern "C" {

kal_uintptr kal_env_arg_count(void) {
    return static_cast<kal_uintptr>(okuefi::self().argc);
}

kal_intptr kal_env_arg(kal_uintptr index, char* out, kal_uintptr cap) {
    const okuefi::state& s = okuefi::self();
    if (index >= static_cast<kal_uintptr>(s.argc)) return -kal_err_not_found;
    const char* a = s.argv[index];
    const kal_uintptr n = length(a);
    if (out && cap) {
        const kal_uintptr k = n < cap ? n : cap;
        for (kal_uintptr i = 0; i < k; ++i) out[i] = a[i];
    }
    return static_cast<kal_intptr>(n);
}

kal_uintptr kal_env_var_count(void) { return 0; }
kal_intptr kal_env_var(const char*, kal_uintptr, char*, kal_uintptr) { return -kal_err_not_found; }
kal_intptr kal_env_var_at(kal_uintptr, char*, kal_uintptr) { return -kal_err_not_found; }
kal_intptr kal_env_image(char*, kal_uintptr) { return -kal_err_not_found; }
kal_intptr kal_env_loader(char*, kal_uintptr) { return -kal_err_not_found; }
kal_intptr kal_env_system(char*, kal_uintptr) { return -kal_err_not_found; }
int kal_env_resources(void) { return KAL_RESOURCES_BALANCED; }

}  // extern "C"
