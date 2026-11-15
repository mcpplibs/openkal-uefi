// The firmware's handles, kept for the operations that need them.
#pragma once
#include <openkal/types.h>

#include "uefi.h"

namespace okuefi {

struct state {
    efi_handle image;
    efi_system_table* st;
    int argc;
    char** argv;
    unsigned long long tsc_per_ms;   // the processor's counter, measured against Stall
    efi_handle device;               // the volume the image was loaded from (openkal.fs, 0.11.0)
};

state& self();
void calibrate_clock();

// A civil time of the firmware as nanoseconds of the wall clock, and the reverse (time.cpp).
kal_u64 ns_of(const efi_time&);
efi_time time_of(kal_u64 ns);

// The stream of an open file (openkal.fs, fs.cpp): whether a stream's word is one, and its
// transfers. openkal.stream (kal.cpp) carries the console's three and passes the rest here.
bool file_stream(kal_uintptr h);
kal_intptr file_write(kal_uintptr h, const void* buf, kal_uintptr n);
kal_intptr file_read(kal_uintptr h, void* buf, kal_uintptr n);
int file_flush(kal_uintptr h);

}  // namespace okuefi
