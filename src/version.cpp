#include <openkal/version.h>

// What this implementation says about itself before it is used. Both answers
// are constants; see openkal/version.h for why they belong to no interface.
extern "C" {

kal_u64 kal_version(void) { return KAL_VERSION; }

// Firmware supplies a console, a pool, a clock, the options an image was started with, and pages
// of code (openkal.exec, 0.10.0: what a loader of images places their code in).
// `openkal.process' waits for `openkal.fs' (a program is named through a directory handle);
// the rest have no firmware service beneath them here.
kal_u64 kal_interfaces(void) {
    return KAL_IFACE_ABORT | KAL_IFACE_STREAM | KAL_IFACE_MEMORY | KAL_IFACE_ENV | KAL_IFACE_TIME |
           KAL_IFACE_EXEC;
}

}
