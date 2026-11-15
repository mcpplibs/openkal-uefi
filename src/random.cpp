// openkal.random on Boot Services (0.11.0): EFI_RNG_PROTOCOL, the firmware's source, found with
// LocateProtocol and asked for its default algorithm.
//
// WHETHER THE FIRMWARE HAS A SOURCE IS KNOWN ONLY WHEN THE PROGRAM RUNS. Clause 6.1 states the
// absence of a source as the absence of this interface at the link; an application built once for
// x86_64-uefi meets firmware with a source (a driver over the processor's instruction, or over a
// virtual device) and firmware without one, and the link cannot know which. The interface is
// therefore provided, and firmware that installs no RNG protocol is answered with
// kal_err_not_supported and no byte filled: not kal_err_again, which states a source that will
// fill later, and not bytes from a clock, which are predictable to an adversary (random.h).
//
// No position of kal_random_props is claimed. The firmware's source answers at once or reports
// that it is not ready (kal_err_again), so it does not block; whether it is a device of the
// processor or of a board is not stated by the protocol, so HARDWARE is not claimed.
#include <openkal/random.h>

#include "uefi.h"
#include "state.h"

namespace {

efi_rng_protocol* source() {
    static efi_rng_protocol* rng = nullptr;
    if (!rng) {
        const okuefi::state& s = okuefi::self();
        void* p = nullptr;
        if (s.st && s.st->boot_services->locate_protocol(&kRngProtocol, nullptr, &p) == EFI_SUCCESS)
            rng = static_cast<efi_rng_protocol*>(p);
    }
    return rng;
}

}  // namespace

extern "C" {

int kal_random_fill(void* out, kal_uintptr len) {
    if (len == 0) return kal_ok;
    if (!out) return kal_err_invalid;
    efi_rng_protocol* rng = source();
    if (!rng) return kal_err_not_supported;
    // In parts, since a source may bound one request; every part or none is a success.
    auto* p = static_cast<efi_u8*>(out);
    constexpr kal_uintptr kPart = 4096;
    for (kal_uintptr done = 0; done < len;) {
        const kal_uintptr n = len - done < kPart ? len - done : kPart;
        const efi_status s = rng->get_rng(rng, nullptr, n, p + done);
        if (s == EFI_NOT_READY) return kal_err_again;
        if (s == EFI_UNSUPPORTED) return kal_err_not_supported;
        if (s != EFI_SUCCESS) return kal_err_io;
        done += n;
    }
    return kal_ok;
}

kal_uintptr kal_random_props(void) { return 0; }

}  // extern "C"
