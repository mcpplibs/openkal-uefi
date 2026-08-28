#include <openkal/version.h>

// What this implementation says about itself before it is used. Both answers
// are constants; see openkal/version.h for why they belong to no interface.
extern "C" {

kal_u64 kal_version(void) { return KAL_VERSION; }

// Firmware supplies a console and a page allocator and nothing else this
// specification has an interface for. `openkal.env' is absent because a program
// started by firmware receives no arguments this implementation can report, and
// reporting none would be a claim rather than an absence.
kal_u64 kal_interfaces(void) {
    return KAL_IFACE_ABORT | KAL_IFACE_STREAM | KAL_IFACE_MEMORY;
}

}
