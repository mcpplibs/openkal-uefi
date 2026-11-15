#include <openkal/version.h>

// What this implementation says about itself before it is used. Both answers
// are constants; see openkal/version.h for why they belong to no interface.
extern "C" {

kal_u64 kal_version(void) { return KAL_VERSION; }

// Firmware supplies a console, a pool, a clock, the options an image was started with, pages of code
// (openkal.exec, 0.10.0: what a loader of images places their code in), the volume the image was
// loaded from and a source of random bytes (openkal.fs and openkal.random, 0.11.0).
// `openkal.process' is not provided yet: StartImage of an image found through openkal.fs is what
// it will be, a started program running to its end first (clause 7.19); the rest have no firmware
// service beneath them here.
kal_u64 kal_interfaces(void) {
    return KAL_IFACE_ABORT | KAL_IFACE_STREAM | KAL_IFACE_MEMORY | KAL_IFACE_ENV | KAL_IFACE_TIME |
           KAL_IFACE_EXEC | KAL_IFACE_FS | KAL_IFACE_RANDOM;
}

}
