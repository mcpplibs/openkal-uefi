// The firmware's text and openkal's: UCS-2 (UTF-16 where a pair occurs) and UTF-8.
//
// A conversion of representation and nothing more. The firmware names files and passes arguments in
// sixteen-bit units; openkal names them in bytes. Each direction is one pass with no table.
#pragma once
#include <openkal/types.h>

#include "uefi.h"

namespace okuefi {

// The UTF-8 length of `n' units of firmware text. A pair of surrogates is one character of four
// bytes; a surrogate alone is encoded as itself, three bytes, rather than refused --- a name the
// firmware gave is reported, not judged.
inline kal_uintptr utf8_length(const efi_char16* w, kal_uintptr n) {
    kal_uintptr bytes = 0;
    for (kal_uintptr i = 0; i < n; ++i) {
        const unsigned c = w[i];
        if (c < 0x80) bytes += 1;
        else if (c < 0x800) bytes += 2;
        else if (c >= 0xD800 && c < 0xDC00 && i + 1 < n && w[i + 1] >= 0xDC00 && w[i + 1] < 0xE000) { bytes += 4; ++i; }
        else bytes += 3;
    }
    return bytes;
}

// Writes the UTF-8 form of `n' units at `out', at most `cap' bytes of it, and returns the whole
// length (utf8_length), so a caller with a short buffer learns how long the text is.
inline kal_uintptr to_utf8(const efi_char16* w, kal_uintptr n, char* out, kal_uintptr cap) {
    kal_uintptr o = 0;
    const auto put = [&](unsigned b) { if (out && o < cap) out[o] = static_cast<char>(b); ++o; };
    for (kal_uintptr i = 0; i < n; ++i) {
        unsigned c = w[i];
        if (c >= 0xD800 && c < 0xDC00 && i + 1 < n && w[i + 1] >= 0xDC00 && w[i + 1] < 0xE000) {
            c = 0x10000 + ((c - 0xD800) << 10) + (w[i + 1] - 0xDC00);
            ++i;
        }
        if (c < 0x80) put(c);
        else if (c < 0x800) { put(0xC0 | (c >> 6)); put(0x80 | (c & 0x3F)); }
        else if (c < 0x10000) { put(0xE0 | (c >> 12)); put(0x80 | ((c >> 6) & 0x3F)); put(0x80 | (c & 0x3F)); }
        else { put(0xF0 | (c >> 18)); put(0x80 | ((c >> 12) & 0x3F)); put(0x80 | ((c >> 6) & 0x3F)); put(0x80 | (c & 0x3F)); }
    }
    return o;
}

// The firmware's form of `n' bytes of UTF-8 at `s', into `out' (room for `cap' units and a NUL).
// Returns the number of units, or -1 for text that is not UTF-8 or does not fit. A character
// beyond the sixteen-bit plane is a pair of surrogates, which a volume that stores UCS-2 keeps
// as two units.
inline long long to_ucs2(const char* s, kal_uintptr n, efi_char16* out, kal_uintptr cap) {
    kal_uintptr o = 0;
    for (kal_uintptr i = 0; i < n;) {
        const unsigned b = static_cast<unsigned char>(s[i]);
        unsigned c = 0, more = 0;
        if (b < 0x80) { c = b; }
        else if ((b & 0xE0) == 0xC0) { c = b & 0x1F; more = 1; }
        else if ((b & 0xF0) == 0xE0) { c = b & 0x0F; more = 2; }
        else if ((b & 0xF8) == 0xF0) { c = b & 0x07; more = 3; }
        else return -1;
        if (more && i + more >= n) return -1;
        for (unsigned k = 1; k <= more; ++k) {
            const unsigned t = static_cast<unsigned char>(s[i + k]);
            if ((t & 0xC0) != 0x80) return -1;
            c = (c << 6) | (t & 0x3F);
        }
        i += 1 + more;
        if (c == 0) return -1;
        if (c < 0x10000) {
            if (o + 1 > cap) return -1;
            out[o++] = static_cast<efi_char16>(c);
        } else {
            if (c > 0x10FFFF || o + 2 > cap) return -1;
            c -= 0x10000;
            out[o++] = static_cast<efi_char16>(0xD800 + (c >> 10));
            out[o++] = static_cast<efi_char16>(0xDC00 + (c & 0x3FF));
        }
    }
    out[o] = 0;
    return static_cast<long long>(o);
}

}  // namespace okuefi
