// The firmware's handles, kept for the operations that need them.
#pragma once
#include <openkal/fs.h>
#include <openkal/types.h>

#include "uefi.h"

namespace okuefi {

// THE START RECORD (0.13.0): what a program of this implementation gives a program it starts
// (openkal.process, process.cpp), installed on the started image's handle under kStartRecord before
// StartImage and removed after it returns. The firmware carries the command line (the load options)
// and nothing else; the environment's variables and the directory the program runs in travel here.
// The record and what it points to are the starter's, which is suspended until the started program
// has ended (clause 7.19), so they hold for the whole of the started program's life. A program the
// firmware or the Shell started finds none.
struct start_record {
    efi_u32 magic;                 // kStartMagic
    efi_u32 version;               // 1
    efi_uintn size;                // of this structure: a later version appends
    const char* const* entries;    // `NAME=value', each `lens[i]' bytes
    const kal_uintptr* lens;
    efi_uintn count;
    const efi_char16* work;        // the directory it runs in, from the volume's root, `\'-separated
};
inline constexpr efi_u32 kStartMagic = 0x52534b4f;   // "OKSR"
inline constexpr efi_guid kStartRecord = {
    0xEEE8134B, 0x1540, 0x4E0A, {0x9E, 0x09, 0x44, 0xD8, 0x74, 0x14, 0x69, 0x07}};

struct state {
    efi_handle image;
    efi_system_table* st;
    int argc;
    char** argv;
    unsigned long long tsc_per_ms;   // the processor's counter, measured against Stall
    efi_handle device;               // the volume the image was loaded from (openkal.fs, 0.11.0)
    const start_record* start;       // the starter's record, where a program started this one (0.13.0)
    const efi_char16* work;          // the directory the program runs in; null or "" for the root
};

state& self();
void calibrate_clock();

// A sentence on the error console (kal.cpp), for what the program is told of without an operation.
void say(const char* text);

// A civil time of the firmware as nanoseconds of the wall clock, and the reverse (time.cpp).
kal_u64 ns_of(const efi_time&);
efi_time time_of(kal_u64 ns);

// The stream of an open file (openkal.fs, fs.cpp): whether a stream's word is one, and its
// transfers. openkal.stream (kal.cpp) carries the console's three and passes the rest here.
bool file_stream(kal_uintptr h);
kal_intptr file_write(kal_uintptr h, const void* buf, kal_uintptr n);
kal_intptr file_read(kal_uintptr h, void* buf, kal_uintptr n);
int file_flush(kal_uintptr h);

// What openkal.process (process.cpp, 0.13.0) needs of the volume (fs.cpp): the firmware's name of a
// program beneath a directory, a directory's name from the root, and every handle closed at the end.
int program_name(kal_dir base, const char* name, kal_uintptr len, efi_char16** out);
const efi_char16* directory_name(kal_dir d);
void close_all();

// AT THE PROGRAM'S END (kal.cpp, 0.13.0): the memory it obtained is returned to the firmware. The
// pages of a region of code (exec.cpp) are recorded with the allocator's, and made writable and not
// executable again before they are returned.
void release_memory();
bool pages_taken(unsigned long long at, kal_uintptr pages, bool code);
kal_uintptr pages_returned(unsigned long long at);
void exec_unprotect(unsigned long long at, kal_uintptr pages);

}  // namespace okuefi
