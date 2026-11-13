// The firmware's handles, kept for the operations that need them.
#pragma once
#include "uefi.h"

namespace okuefi {

struct state {
    efi_handle image;
    efi_system_table* st;
    int argc;
    char** argv;
    unsigned long long tsc_per_ms;   // the processor's counter, measured against Stall
};

state& self();
void calibrate_clock();

}  // namespace okuefi
