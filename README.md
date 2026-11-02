# bsa-reader

Read-only C++17 library for Bethesda BSA archives, header versions 103, 104
and 105: Oblivion, Fallout 3, Fallout: New Vegas, Skyrim LE and Skyrim SE/AE.
No dependencies. GPL-3.0.

It is built for tools that open many archives and ask each one a few
questions, such as a shell extension or a mod-list scanner, rather than for
extracting whole archives:

- The archive is memory-mapped and every table is range-checked once at open.
  Nothing else is parsed until asked for.
- Lookups use the archive's own sorted hash tables, the way the game engine
  does: compute the hash, binary-search, done. The name tables are never
  touched and nothing is allocated.
- Nothing throws. Every call returns a `Status`. Truncated, hostile or
  corrupt input is a status, never a crash. On Windows, a mapping whose
  backing file vanished mid-read reports `IoError` instead of faulting.
- The file is opened with share read/write/delete so mod managers can keep
  working while it is open.
- zlib and LZ4-frame decoders are built in, bounds-checked, and write into a
  caller-owned buffer of a known size.

Not supported, by design: Morrowind archives, BA2 (Fallout 4, Starfield),
Xbox 360 big-endian archives, and writing. Each is refused with a specific
status.

## Use

```cpp
#include <bsa/reader.hpp>

bsa::Reader r;
if (r.open(L"Skyrim - Interface.bsa") != bsa::Status::Ok) ...

bsa::Entry e;
if (r.find("strings\\skyrim_english.strings", e) == bsa::Status::Ok) {
    std::vector<uint8_t> data;
    if (r.read(e, data) == bsa::Status::Ok) ...   // decompressed, capped at 256 MiB by default
}

// Enumerate (names are decoded on demand only here)
for (uint32_t i = 0; i < r.header().fileCount; ++i) {
    bsa::Entry e;
    std::string_view folder, name;
    r.entry(i, e);
    r.folder_name(e.folderIndex, folder);
    r.file_name(e, name);
}
```

Keep readers short-lived. Windows refuses to truncate a file that has a
mapping open (renames and deletes still work), so a long-lived reader can
block a tool that rewrites an archive in place.

CMake: `add_subdirectory` or `FetchContent` this repo and link `bsa::reader`.

## bsatool

`bsatool info|list|extract|check`. `check <dir>` opens every archive in a
folder, recomputes every folder and file hash from the name tables and
compares with the stored ones, resolves every path by hash lookup, and
decodes a sample of files (`--read-all` decodes everything). It is the
corpus test: run it over a game's Data folder after any change.

## Format notes that matter

Verified against the shipped Skyrim SE and Oblivion archives:

- Folder records are sorted by folder hash; file records inside a folder by
  file hash. That is what makes hash lookup possible without names.
- A folder record's offset field includes the total file-name length; subtract
  it to get the position of the folder's name + file records.
- File size field: bit 30 toggles the archive-wide compressed flag for that
  file; the low 30 bits are the stored block size, prefixes included.
- With archive flag 0x100 (v104/v105 only) each stored block starts with a
  length-prefixed path. Oblivion archives carry the bit too and ignore it.
- A compressed block starts with the 4-byte uncompressed size, then a zlib
  stream (v103/v104) or an LZ4 *frame* (v105, magic `04 22 4D 18`).
- Hash: `hash1 = last | second_last << 8 | len << 16 | first << 24`, then
  **XOR** `0x80` for `.kf`, `0x8000` for `.nif`, `0x8080` for `.dds`,
  `0x80000000` for `.wav`; high 32 bits are `sdbm(stem[1..len-3]) + sdbm(ext)`
  with `h = h * 0x1003F + c`. ASCII letters are lowercased, `/` becomes `\`,
  bytes above 0x7F pass through unchanged and unsigned. Public descriptions
  say OR for the marker; that is only equivalent for ASCII names, and
  third-party SSE archives with non-ASCII names show the packers XOR. v103 is
  believed to treat high bytes as signed in the sums (per xEdit); the shipped
  Oblivion archives have no such names, so that is unverified.

## Build

Windows: `build.cmd` (MSVC 14.44 + Ninja). Anywhere: `cmake -B build &&
cmake --build build && ctest --test-dir build`.
