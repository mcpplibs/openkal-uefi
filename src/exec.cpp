// openkal.exec on UEFI Boot Services: memory a program may execute, which a loader of images needs
// (the ok-image runner, carried as a UEFI application, places an image's code here).
//
// A region is pages of type EfiLoaderCode, the type the firmware gives an image it loads, zeroed.
// Firmware that enforces "never writable and executable at once" offers the memory attribute
// protocol (UEFI 2.10): a region is then made not executable when it is reserved, and publishing
// a part makes it read-only and executable; sealing a part makes it read-only and not executable.
// Firmware that offers no such protocol maps every page writable and executable, and has nothing to
// change: publishing is then a statement the caller makes, as the interface allows.
//
// A file cannot be mapped (there is no openkal.fs here), so kal_exec_map_part answers
// kal_err_not_supported, a property of the environment.
#include <openkal/exec.h>
#include <openkal/types.h>

#include "uefi.h"
#include "state.h"

namespace {

constexpr kal_uintptr kPage = 4096;

efi_memory_attribute_protocol* attributes() {
    static bool asked = false;
    static efi_memory_attribute_protocol* protocol = nullptr;
    if (!asked) {
        asked = true;
        void* p = nullptr;
        auto* bs = okuefi::self().st->boot_services;
        if (bs->locate_protocol(&kMemoryAttributeProtocol, nullptr, &p) == EFI_SUCCESS)
            protocol = static_cast<efi_memory_attribute_protocol*>(p);
    }
    return protocol;
}

kal_uintptr pages_of(kal_uintptr size) { return (size + kPage - 1) / kPage; }

void* reserve(int how, unsigned long long at, kal_uintptr size) {
    if (size == 0) return nullptr;
    unsigned long long address = at;
    const kal_uintptr pages = pages_of(size);
    if (okuefi::self().st->boot_services->allocate_pages(how, EfiLoaderCode, pages, &address) != EFI_SUCCESS)
        return nullptr;
    auto* p = reinterpret_cast<unsigned char*>(address);
    for (kal_uintptr i = 0; i < pages * kPage; ++i) p[i] = 0;
    if (auto* a = attributes())
        a->set_memory_attributes(a, address, pages * kPage, EFI_MEMORY_XP);
    return p;
}

int protect(void* region, kal_uintptr offset, kal_uintptr size, bool execute) {
    if (!region || size == 0 || offset % kPage || size % kPage) return kal_err_invalid;
    auto* a = attributes();
    if (!a) return kal_ok;
    const auto base = reinterpret_cast<unsigned long long>(region) + offset;
    if (a->set_memory_attributes(a, base, size, EFI_MEMORY_RO) != EFI_SUCCESS) return kal_err_io;
    const efi_status s = execute ? a->clear_memory_attributes(a, base, size, EFI_MEMORY_XP)
                                 : a->set_memory_attributes(a, base, size, EFI_MEMORY_XP);
    return s == EFI_SUCCESS ? kal_ok : kal_err_io;
}

}  // namespace

extern "C" {

void* kal_exec_alloc(kal_uintptr size) { return reserve(AllocateAnyPages, 0, size); }

void* kal_exec_alloc_at(void* at, kal_uintptr size) {
    const auto address = reinterpret_cast<unsigned long long>(at);
    if (!at || size == 0 || address % kPage) return nullptr;
    return reserve(AllocateAddress, address, size);
}

int kal_exec_publish(void* p, kal_uintptr size) {
    return protect(p, 0, pages_of(size) * kPage, true);
}

int kal_exec_publish_part(void* region, kal_uintptr offset, kal_uintptr size) {
    return protect(region, offset, size, true);
}

int kal_exec_seal_part(void* region, kal_uintptr offset, kal_uintptr size, kal_uintptr flags) {
    if (flags & ~KAL_EXEC_SEAL_FOREVER) return kal_err_invalid;
    return protect(region, offset, size, false);
}

kal_uintptr kal_exec_granularity(void) { return kPage; }

int kal_exec_map_part(void*, kal_uintptr, kal_uintptr, struct kal_file, kal_u64, kal_uintptr) {
    return kal_err_not_supported;
}

void kal_exec_free(void* p, kal_uintptr size) {
    if (!p || size == 0) return;
    if (auto* a = attributes())
        a->clear_memory_attributes(a, reinterpret_cast<unsigned long long>(p), pages_of(size) * kPage,
                                   EFI_MEMORY_RO | EFI_MEMORY_XP);
    okuefi::self().st->boot_services->free_pages(reinterpret_cast<unsigned long long>(p), pages_of(size));
}

// Placement at a chosen address is offered (AllocateAddress); publishing again is not.
kal_uintptr kal_exec_props(void) { return KAL_EXEC_PROP_AVAILABLE | KAL_EXEC_PROP_PLACE; }

}  // extern "C"
