#!/usr/bin/env python3
"""Remove the trailing dot from OCCT's Android SONAMEs, in place.

OCCT sets BUILD_SOVERSION_NUMBERS=0 on Android and still passes
SOVERSION "" to set_target_properties(). CMake then emits dynamic strings
like "libTKBO.so." while the installed file is "libTKBO.so". Android's linker
looks for the SONAME verbatim, cannot find "libTKBO.so.", and dlopen fails -
the app shows an SDL error about the missing toolkit.

This rewrites the final dot of each libTK*.so dynamic-string entry to NUL. The
file size and section offsets are unchanged, so ELF segment alignment (16 KB
for Android 15+) is preserved. Only ELF64 little-endian files are handled,
which covers the arm64-v8a build.

Usage:
    python3 android/scripts/fix_occt_soname.py "$PREFIX"/lib/libTK*.so
"""
import re
import struct
import sys

# Names look like "libTKBO.so." / "libTKernel.so."; the dot before the NUL is
# the one CMake appends for an empty SOVERSION.
DOTTED = re.compile(rb"(libTK[A-Za-z0-9_]+\.so)\.\x00")


def patch(path):
    data = bytearray(open(path, "rb").read())
    if data[:4] != b"\x7fELF" or data[4] != 2:
        return 0
    endian = "<" if data[5] == 1 else ">"
    e_shoff = struct.unpack_from(endian + "Q", data, 0x28)[0]
    e_shentsize = struct.unpack_from(endian + "H", data, 0x3A)[0]
    e_shnum = struct.unpack_from(endian + "H", data, 0x3C)[0]
    e_shstrndx = struct.unpack_from(endian + "H", data, 0x3E)[0]

    def section(i):
        off = e_shoff + i * e_shentsize
        return (struct.unpack_from(endian + "I", data, off)[0],
                struct.unpack_from(endian + "Q", data, off + 0x18)[0],
                struct.unpack_from(endian + "Q", data, off + 0x20)[0])

    _, str_off, str_size = section(e_shstrndx)
    shstr = data[str_off:str_off + str_size]
    for i in range(e_shnum):
        name_off, off, size = section(i)
        end = shstr.find(b"\0", name_off)
        if (shstr[name_off:end] if end >= 0 else b"") != b".dynstr":
            continue
        sec = bytes(data[off:off + size])
        new, count = DOTTED.subn(lambda m: m.group(1) + b"\x00\x00", sec)
        if count:
            data[off:off + size] = new
            open(path, "wb").write(data)
        return count
    return 0


def main():
    if len(sys.argv) < 2:
        sys.exit("usage: fix_occt_soname.py <libTK*.so>...")
    patched = 0
    for path in sys.argv[1:]:
        n = patch(path)
        if n:
            print("patched %d SONAME/NEEDED entr%s in %s"
                  % (n, "y" if n == 1 else "ies", path))
            patched += 1
    print("OCCT SONAME fix: %d file(s) changed" % patched)
    return 0


if __name__ == "__main__":
    sys.exit(main())
