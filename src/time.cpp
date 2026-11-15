// openkal.time on Boot Services.
//
// The wall clock is the firmware's (GetTime, a runtime service), whose resolution is the
// firmware's too --- a second on many. The monotonic clock is the processor's counter, measured
// against Stall when the program starts: Boot Services offer a monotonic COUNT
// (GetNextMonotonicCount) but no rate, and an event timer whose callbacks a single-task
// environment runs only while it waits. A sleep is Stall, which waits the microseconds it is told.
#include <openkal/time.h>

#include "state.h"

namespace {

unsigned long long counter() {
#if defined(__x86_64__)
    unsigned lo, hi;
    __asm__ __volatile__("rdtsc" : "=a"(lo), "=d"(hi));
    return (static_cast<unsigned long long>(hi) << 32) | lo;
#else
#error "openkal-uefi measures time with the processor's counter: x86_64"
#endif
}

// Days from 1970-01-01 to a civil date (proleptic Gregorian).
long long days_from_civil(long long y, unsigned m, unsigned d) {
    y -= m <= 2;
    const long long era = (y >= 0 ? y : y - 399) / 400;
    const unsigned yoe = static_cast<unsigned>(y - era * 400);
    const unsigned doy = (153 * (m + (m > 2 ? -3 : 9)) + 2) / 5 + d - 1;
    const unsigned doe = yoe * 365 + yoe / 4 - yoe / 100 + doy;
    return era * 146097 + static_cast<long long>(doe) - 719468;
}

// The civil date of a count of days from 1970-01-01, the inverse of the above.
void civil_from_days(long long z, long long& y, unsigned& m, unsigned& d) {
    z += 719468;
    const long long era = (z >= 0 ? z : z - 146096) / 146097;
    const unsigned doe = static_cast<unsigned>(z - era * 146097);
    const unsigned yoe = (doe - doe / 1460 + doe / 36524 - doe / 146096) / 365;
    const unsigned doy = doe - (365 * yoe + yoe / 4 - yoe / 100);
    const unsigned mp = (5 * doy + 2) / 153;
    d = doy - (153 * mp + 2) / 5 + 1;
    m = mp < 10 ? mp + 3 : mp - 9;
    y = static_cast<long long>(yoe) + era * 400 + (m <= 2);
}

}  // namespace

namespace okuefi {

// THE FIRMWARE'S TIME AND openkal's. EFI_TIME is a civil date in a zone the time states, or in the
// firmware's local zone when it states none (2047), which is read as UTC: the wall clock and a
// volume's times (FAT records local time and states no zone) are then read alike, and a time
// written to a file reads back as written.
kal_u64 ns_of(const efi_time& t) {
    if (t.year == 0 || t.month == 0 || t.day == 0) return 0;
    long long secs = days_from_civil(t.year, t.month, t.day) * 86400 + t.hour * 3600 + t.minute * 60 + t.second;
    if (t.time_zone != 2047) secs += static_cast<long long>(t.time_zone) * 60;   // to UTC
    return secs < 0 ? 0 : static_cast<kal_u64>(secs) * 1000000000ull + t.nanosecond;
}

efi_time time_of(kal_u64 ns) {
    const long long secs = static_cast<long long>(ns / 1000000000ull);
    long long y = 0;
    unsigned m = 0, d = 0;
    civil_from_days(secs / 86400, y, m, d);
    const long long rest = secs % 86400;
    efi_time t{};
    t.year = static_cast<efi_u16>(y);
    t.month = static_cast<efi_u8>(m);
    t.day = static_cast<efi_u8>(d);
    t.hour = static_cast<efi_u8>(rest / 3600);
    t.minute = static_cast<efi_u8>(rest / 60 % 60);
    t.second = static_cast<efi_u8>(rest % 60);
    t.nanosecond = static_cast<efi_u32>(ns % 1000000000ull);
    t.time_zone = 2047;
    return t;
}

void calibrate_clock() {
    state& s = self();
    const unsigned long long a = counter();
    s.st->boot_services->stall(10000);   // ten milliseconds
    const unsigned long long b = counter();
    s.tsc_per_ms = (b - a) / 10;
    if (s.tsc_per_ms == 0) s.tsc_per_ms = 1;
}
}  // namespace okuefi

extern "C" {

kal_duration kal_time_monotonic(void) {
    const okuefi::state& s = okuefi::self();
    const unsigned long long t = counter();
    return static_cast<kal_duration>(t / s.tsc_per_ms * 1000000ull + (t % s.tsc_per_ms) * 1000000ull / s.tsc_per_ms);
}

kal_duration kal_time_monotonic_granularity(void) { return 1000; }

kal_duration kal_time_wall(void) {
    const okuefi::state& s = okuefi::self();
    efi_time t{};
    if (!s.st || !s.st->runtime_services || s.st->runtime_services->get_time(&t, nullptr) != EFI_SUCCESS)
        return 0;
    return okuefi::ns_of(t);
}

void kal_time_sleep(kal_duration ns) {
    const okuefi::state& s = okuefi::self();
    if (!s.st) return;
    kal_duration us = (ns + 999) / 1000;
    while (us) {
        const kal_duration step = us > 1000000 ? 1000000 : us;
        s.st->boot_services->stall(static_cast<efi_uintn>(step));
        us -= step;
    }
}

kal_uintptr kal_time_props(void) { return KAL_TIME_PROP_WALL_AVAILABLE | KAL_TIME_PROP_SLEEP_PRECISE; }

}  // extern "C"
