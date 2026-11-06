# Archive.exe: what it does and where it goes wrong

`Archive.exe` is the packer that ships in Skyrim Special Edition's
`Tools\Archive` folder, and the program the Creation Kit runs for "Create
Archive". This is a catalog of what it does, for mod authors and tool makers.
Every claim names the function it comes from, or a run that shows it.

**The short version.** It writes valid archives, and it made every vanilla
archive. But it reports almost nothing. It leaves files out without saying so,
it stops on message boxes nobody can click in a script, and it exits with code 0
after packing only part of what you asked for. If you can, use
[BSArch](#what-to-use-instead). If you must use Archive.exe, read
[Bugs and traps](#bugs-and-traps) first.

The build studied has file version 1.0.0.1, PE date 2016-07-23, 273,920 bytes,
SHA-256 `c72868b9…cede0f7`. The copy in Steam's `Tools\Archive` and the backup
copy we compared it with are the same file. It was decompiled with Ghidra and
read for behaviour only; none of that code is in this repository. Addresses
such as `14000a290` are entry points in this build. Runs were on Windows 11 with
scratch folders. Counts come from runs on 2026-11-05 over a `Data` folder of 893
archives and 664,272 files.

## Running it

```
Archive.exe script.txt [-OutputDir="folder"] [-InputDir="folder"]...
```

It is a dialog program, so a window always appears. The script starts during the
dialog's first paint (`140004e60`), and the dialog closes when the script ends.
For unattended runs, start it on a private desktop and put a timeout on it,
because any message box waits forever (see the traps below). Nothing goes to the
console.

A script that works:

```
Log: C:\work\run.log
New Archive
Check: Meshes
Check: Compress Archive
Check: Retain File Names
Add Directory: C:\work\Data\meshes
Save Archive: C:\work\MyMod.bsa
```

The script keywords (`140005b50`). Case does not matter.

| Line | Effect |
|---|---|
| `Log: path` | Log file. `$Date` in the path becomes the local date and time. |
| `New Archive` | Start over: clears options, restrictions and the file list (`1400031b0`). |
| `Check: option` | Turn an option on. There is no way to turn one off except `New Archive`. |
| `Add Directory: path` | Add a folder and its subfolders (`140001ef0`). |
| `Set File Group Root: path\` | Prefix for `Add File Group` lines only. |
| `Add File Group: list.txt` | Add the files named in a list, wildcards allowed (`140002400`). |
| `Restrict Files: nif,dds` | Only these extensions, with no dot. `~` in front means all except these (`140008460`). |
| `Restrict Directories: a,b` | Only these folders, full name in the archive. `~` works too (`140008320`). |
| `File Order: list.txt` | Write the data of these files first (`14000b8d0`). |
| `Compression Level: n` | LZ4 level, default 16. Levels 0 and 1 gave the same output, and so did 16 and 99. |
| `Compression Flag List: list.txt` | Files that reverse the archive's compression setting (see below). |
| `Scratch Path: folder` | Where the temporary files and the `.bsl` go (`14000a550`, `14000aae0`). |
| `Save Archive: path` | Write the archive (`14000a290`). The extension is forced to `.bsa`. Without a new `New Archive`, later `Add Directory` lines add to the same list, and the next save contains both. |

`Check:` options and the header bits they set (`140003750`, `1400039c0`,
`140003af0` to `140003e00`):

| Archive flags | | File type field | |
|---|---|---|---|
| always on | `0x1` folder names, `0x2` file names (`140007630`) | `Meshes` | `0x1` |
| `Compress Archive` | `0x4` | `Textures` | `0x2` |
| `Retain Directory Names` | `0x8` | `Menus` | `0x4` |
| `Retain File Names` | `0x10` | `Sounds` | `0x8` |
| `Retain File Name Offsets` | `0x20` | `Voices` | `0x10` |
| `XBox` | `0x40` | `Shaders` | `0x20` |
| `Retain Strings During Startup` | `0x80` | `Trees` | `0x40` |
| `Embed File Names` | `0x100` | `Fonts` | `0x80` |
| `XMem Codec` (needs XBox and Compress) | `0x200` | `Misc` | `0x100` |

Archive.exe itself only writes the retain and type bits into the header. **The
file type field records what you checked, not what is in the archive**: 166 of
the 304 archives we can attribute to Archive.exe have it at 0.

`-InputDir` replaces all of the script's `Add Directory` lines. `-OutputDir`
keeps the script's file name and changes its folder.

The Creation Kit writes a script with `Log: Logs\Archives\ArchiveLog.txt`,
`New Archive`, optionally `Check: Compress Archive`, `Scratch Path: C:\temp`,
`Set File Group Root`, `Add File Group` and `Save Archive` (Kit `1411ffe50`). It
checks no type and no retain option, so a Kit-made archive has flags `0x3` or
`0x7` and type 0, and its `.bsl` ends up in `C:\temp`.

## What it writes

Version 105 only. The version is fixed when saving (`14000a290`), and opening
any other version fails with "File version is newer", older ones included
(`140009d10`).

**Layout.** Header, folder records (24 bytes each), then for each folder its
name and file records, then the file name table, then the data with no gap. All
893 archives in the corpus have no gap, whichever tool made them.

**Order.** Folder records are sorted by folder hash, and files inside a folder by
file hash (`140008ba0`, `140008680`). The name table and the data follow the same
order (`14000bd80`), so without `File Order` each file's data starts where the
previous record's data ended. `File Order` moves data only; the records stay
sorted.

**Compression, per archive and per file** (`14000bd80`). `Compress Archive` sets
flag `0x4`, and then **every** file is compressed. There is no built-in list of
extensions to skip, so sounds and string files are compressed too. A file named
in the `Compression Flag List` gets bit 30 of its size field and does the
opposite of the archive. The list matches `*.ext` or the path inside the archive,
ignoring case (`140008880`, `14000a710`). None of the 304 attributed archives uses
it: all 131 compressed ones compress everything.

**There is no fallback for files that do not shrink.** 100,000 random bytes are
stored as 100,019, and a 1-byte file as 20. In the corpus, 742 files in 32
Archive.exe archives take more room than they would stored plain.

**LZ4 settings.** A whole-file LZ4 frame, with the original size in the 4 bytes
before it. It asks for 4 MB linked blocks at level 16 (`140007630`, `14000bd80`,
`14000ce30`). The library then picks the block size and mode from the file size,
which gives a recognisable pattern:

| File size | FLG | BD |
|---|---|---|
| up to 64 KiB | `0x60` | `0x40` |
| up to 256 KiB | `0x60` | `0x50` |
| up to 1 MiB | `0x60` | `0x60` |
| up to 4 MiB | `0x60` | `0x70` |
| above 4 MiB | `0x40` | `0x70` |

There is no content checksum and no content size. `XMem Codec` is only a flag
here: the data was still LZ4 in our run.

**Embedded names** (`14000bd80`). With flag `0x100`, each file's data starts with
a length byte and the lowercase `folder\file` path with no terminator. The stored
size includes them.

**Names.** Folder and file names are stored in lowercase, in the Windows ANSI code
page (`14000acc0`, `14000ad80`). Only `A`-`Z` change, so `É` stays `É`.

**Extra files beside the archive.** Temporary `.tdt` and `.thd` files during the
save, and a `.bsl` listing every time (`14000aae0`, `14000b6e0`). Saving over an
existing archive keeps the old one as `.bak` (`140008d40`).

## The hash

`140024010`, `140023ef0`. The file hash has two parts:

- Low 32 bits, as bytes: last stem character, second-to-last (only if the stem has
  more than 2), stem length, first character.
- High 32 bits: `sdbm(stem without its first and last two characters)` plus
  `sdbm(extension with its dot)`, with `h = h * 0x1003F + c` and bytes unsigned.

The extension adds a marker:

| Extension | Added to the low 32 bits |
|---|---|
| `.nif` | `0x00008000` |
| `.kf` | `0x00000080` |
| `.dds` | `0x00008080` |
| `.wav` | `0x80000000` |
| `.adp` | `0x80008000` |

**XOR or OR?** Neither is what the code does. It adds to each byte separately,
with no carry into the next byte. Adding `0x80` to a single byte is exactly XOR
`0x80`, so bsa-reader's XOR is right and the OR in public descriptions is wrong for
bytes above `0x7F`. Archive.exe confirms it as an oracle. `Éa.wav` has first-character
byte `0x49` (`0xC9 + 0x80`, wrapped), and `péé.dds` has last-character byte `0x69`.
Under the XOR rule, all nine non-ASCII names in that run match except the `.adp` one,
which fails only because of the marker covered below.

**The rest of it, where bsa-reader differs today.** The Kit has the same marker
table and the same hash code (Kit `142629a20`, table at `14393cf28`). The game has the
same table (1.6.1170, `140cff990`, table at `142029ba8`). These are real differences in bsa-reader's
`hash.cpp`, though no archive in the corpus exposes them:

- **`.adp` has a marker.** bsa-reader has none.
- **Only the first four bytes of the extension are compared**, so `.nifx`, `.ddsx`,
  `.wavx` and `.adpx` get the marker too. `.kfx` does not, because `.kf` needs its
  terminator. bsa-reader compares the whole extension.
- **A name with an empty stem hashes to 0**, whatever the extension (`.nif`,
  `.gitignore`).

The corpus has no `.adp` files and no such names, which is why `bsatool check`
reports 0 mismatches over all 664,272 files.

Separately, an extension of 15 or more letters also gives hash 0 in Archive.exe. That
comes from a 16-byte buffer in its path splitting (`140023ef0`), so it may be
Archive.exe's own quirk and not the game's rule: we did not trace the game's path
handling.

## Bugs and traps

Each entry says what you see and what to do. Those marked *run* were reproduced on a
scratch folder. "BSArch" means the fix is to use it (see
[What to use instead](#what-to-use-instead)).

### The "save crash" is an empty archive

**What you see.** The process dies with exit code `0xC0000409`. The log is 0 bytes,
and `.bsl`, `.tdt` and `.thd` files of 0 bytes are left behind. *Run.*

**Why.** The save checks whether any folders were added and throws a bare text
string, "No Directories to save." (`14000a290`, return address `Archive+0xa363`).
Every other error in the program is a different exception type, which the dialog
catches and shows. Nothing catches this one, so the process ends. The log is empty
because it is only flushed when the script finishes. This was first recorded as an
unexplained crash on save. It has nothing to do with the path length or the file
names.

**The usual cause is a relative `Add Directory`.** It is relative to the current
folder, *not* to `Set File Group Root`, which applies only to `Add File Group`.
Any of these ends the same way, because each leaves nothing to save:

- `Add Directory` on a folder that does not exist, if it is the only one. If another
  folder has files, the missing one is skipped with no message. *Run.*
- A folder holding only empty folders or 0-byte files. *Run.*
- A `Restrict Files` filter that matches nothing, such as `.nif` or `*.nif`. It takes
  bare extensions: `nif`. The wrong form with `~`, such as `~.txt`, excludes nothing
  and gives no warning. *Run.*

**Do this.** Use absolute paths, and check the exit code as well as the log.

### Files it leaves out with no message

| Trigger | What happens | Function |
|---|---|---|
| A 0-byte file | Dropped. An empty folder is dropped too. *Run.* | `140007fa0` |
| Folder name starting with `.` | The whole folder is skipped, however it continues (`.hidden`). Only the first character is tested. *Run.* | `140001ef0` |
| Two names with the same hash | The second is dropped. The only trace is a line in `File Collisions.txt` in the current folder, which is never cleared. *Run.* | `1400087f0`, `140008280` |
| Empty stem, or an extension of 15 or more letters | Hash 0, so they all collide and only one survives (`.nif`, `.dds`, `.txt` in one folder keep just `.dds`). *Run.* | `140024010`, `140023ef0` |
| Exactly 1 GiB (or any multiple) | The size is cut to 30 bits and becomes 0, so the file is dropped as if empty. *Run.* | `140008a90` |
| Subfolder name not in the Windows code page | Windows reports `?` in place of the characters, the folder cannot be opened, and it is skipped. *Run.* | `140001ef0` |
| Folder path past the Windows limit of 260 | Skipped. *Run.* | `140001ef0` |
| `Add File Group` line that matches nothing | Skipped. *Run.* | `140002400` |
| `Add File Group` with no root set | Lines pile onto each other, so only the first line works. *Run.* | `140002400` |

Nothing is filtered out by type either, so `Thumbs.db`, `desktop.ini`, `.bak` and
`.gitignore` files all go in. *Run.*

**Do this.** Compare the file count in the log or in `bsatool info` against your
folder. BSArch keeps 0-byte files and refuses or reports most of the rest.

### Files that stop the run, then vanish

A file that can't be opened shows a message box titled `InitInfo() File Error!`
(`140008a90`). *Run*, with:

- a file another program holds open with no sharing;
- a name with characters outside the Windows code page (it reports `???.nif`);
- a full path just over the Windows limit (seen at 260 and 262 characters).

In a script nobody can click it, so the run **waits forever**. If you do click it,
**the rest of that `Add Directory` is abandoned, the save goes ahead and the exit code
is 0**. In our run with three files, `a.nif`, `locked.nif` and `z.nif`, the archive
held only `a.nif` and the log mentioned no error. *Run.*

**Do this.** Keep source names ASCII, close anything using the files, and keep paths
well under 260. Treat a message box or a timeout as a failed run.

### Files over 1 GiB

The size field is masked to 30 bits (`140008a90`). The result depends on compression:

| File | Outcome |
|---|---|
| Exactly a multiple of 1 GiB | Dropped silently. *Run.* |
| Above 1 GiB, `Compress Archive` | **Silently cut.** A file of 1 GiB + 4,096 is stored as 4,096 bytes. Exit 0. *Run.* |
| Above 1 GiB, uncompressed | **Hangs forever.** The whole file is copied to `.tdt`, then the copy loop never finishes, because it reads full buffers rather than the remaining size (`14000bd80`). *Run.* |

The same loop means a file that changes size during packing can hang it (from the code,
not run).

### Archives over 2 GiB

Each file's offset is masked to 31 bits (`14000b6e0`), with no warning. With four files
of 900 MiB the fourth got offset 683,671,708 where 2,831,155,356 was correct, and the
log's last line reads `Offset -520093696: END OF FILE`. Exit 0. *Run.*

**Do this.** Split the archive yourself below 2 GiB, or use BSArch's `-split`.

### Long paths crash it

A file with a full path of 264 characters or more ends the process with `0xC0000409`
(stack cookie failure, subcode 2). A 272-byte buffer in `140007fa0` overflows while
formatting the debug line "Adding: …". We measured a crash at 264 and none at 262. Up
to 259 packs normally. *Run.*

### The "Data" cut mangles folder names

Archive.exe works out a file's path inside the archive by looking for the text `Data`
in its full path (`140009280`). It takes the **first** occurrence **anywhere**,
ignoring case, even in the middle of a word, and keeps what follows. If there is no
match it keeps the whole path minus the drive. It also skips a match at the very end of
the path. You see the result only if you list the archive. *All run:*

| `Add Directory` | Folder in the archive |
|---|---|
| `C:\x\Data\meshes` | `meshes` (correct) |
| `C:\x\Metadata\Data\meshes` | `data\meshes` |
| `C:\x\DATABASE\meshes` | `base\meshes` |
| `C:\x\p_dataroot\Data` | `root\data` |
| `C:\x\mod\meshes` | `x\mod\meshes` |
| `C:\x\Data` | `x\data` for the root files, `meshes` for the subfolder |
| `C:\x\Data\` | `.` for the root files, and `\meshes` with a leading backslash |
| `.\Data` | `.\data` for the root files |

So a folder such as `C:\Users\you\AppData\Local\Temp\Data\meshes` gets cut at
`AppData`, and the textures end up where the game will never look.

**Do this.** Pack from a path whose only `data` is the final `Data` folder, such as
`C:\work\Data\meshes`, and list the archive afterwards. BSArch takes paths relative to
the folder you give it, so it has no such problem.

Root-level files only come out cleanly from `Add File Group` with a root, which gives a
folder named `.`. None of the 893 archives has one, and BSArch refuses such files.

### Modal dialogs in scripts

These hang an unattended run. *All run.*

| Trigger | Dialog | Function |
|---|---|---|
| **Any** `Check:` line after files were added | `Erorr` (spelled that way): "Clicking the XBox Archive box…", even if the option has nothing to do with XBox | `140003cd0` |
| A hyphen in the script path, such as `my-mod.txt` | `Command Line Format Error!` | `140005590` |
| `-help`, or an unknown `-Setting` | A message box, not console text | `140005590` |
| An `Add File Group` line matching `.`, such as `meshes\*` | `InitInfo() File Error!` | `140002400` |

Put all `Check:` lines before the first `Add`. Keep hyphens out of the script path.

### Other script traps

- A script file that does not exist: exit 0, nothing done. No script at all: the dialog
  opens and waits. *Run.*
- Parsing assumes one space after the colon (`140005b50`). `Add Directory:C:\x` loses its
  first character, so the folder is not found and is skipped silently. `Check:Meshes`
  becomes `eshes`, logged but not reported as a failure. *Run.*
- Unknown lines are logged as "Unknown command", and the script continues. *Run.*
- A very long line crashes it (`0xC0000409`): the line buffer holds 5,200 characters, and
  a line of 6,000 did it. *Run.*
- `-OutputDir` with `Save Archive: name.bsa` and no backslash crashes it: the code looks
  for a backslash and uses the result unchecked. *Run* (exit `0xC000041D`).
- Errors never reach the log. The log records script steps; errors are message boxes.
  The exit code is 0 unless the process crashes (`14000a290`, `140005b50`, `140005b10`).

### It adds its own output on the next run

If the archive is saved inside a folder being packed, the next run packs the previous
`.bsa` into the new one. The old `.bsl` is emptied before it is read and ends up with a
name but no record, so the name table no longer lines up. In our run the header said 3
files and 20 bytes of names, and the table read `out.bsa`, `out.bsl`, `a.ni`, cut off at
20 bytes. Lookups by hash still work, but anything listing names gets them wrong or gets
none (`bsatool list` shows only the folders). *Run.* Save outside the source tree.

### Adding a file twice inflates the name table

The second `Add Directory` of the same folder adds no records, but the header's name
length is counted twice (`140007f00`). The table then ends in uninitialised bytes: 12 in
our run. *Run.* It looks harmless, since readers follow the records. One corpus archive,
`MarketplaceTextures.bsa`, has 9 extra bytes at the end of its table, of the same kind
except that they are zero.

### What it leaves in your files

- **The `.bsl` holds your full source paths.** It lists each file's size, time stamp and
  absolute path (`14000b6e0`), which can include your Windows user name. It is written
  beside the archive unless `Scratch Path` is set. Seven of the 893 archives here had a
  `.bsl` shipped alongside them, six with absolute paths in it, from 4 to 16,048 paths
  each. **Don't ship it.**
- **Stray memory in padding.** Four bytes of each folder record and two of the header are
  never set (`1400077a0`, `14000c3e0`, `14000b5c0`), so they hold whatever was on the
  heap. It is a small amount, and harmless to the game. It is also how to recognise this
  tool (below).

### Other

- The archive can be made with no type bits, and the GUI's warning ("no archive type
  selected") doesn't appear in script mode.
- Names outside ASCII are stored in the ANSI code page of the machine that made them.
- It reads each file fully into memory to compress it (`14000bd80`).

## What to use instead

```
BSArch pack "C:\work\Data" "C:\work\MyMod.bsa" -sse -z
```

BSArch (open source, in the xEdit repository) sets flags from content, does not cut paths
on "Data", keeps 0-byte files, handles large inputs with `-split`, and has no dialogs.
Differences to expect: it does not compress `.strings` files or audio other than `.fuz`,
it writes data in a different order from the records (harmless), and `-share` makes
identical files share data. With both `-share` and embedded names, the shared copies carry
the first file's name: 1,709 files in 13 corpus archives.

We have not tested in the game any archive made for this catalog.

## Telling packers apart

Read from an archive's bytes. Run over all 893 archives.

| Trait | Archive.exe | BSArch 1.0 | BSArch 0.9e | CAO (rsm-bsa) |
|---|---|---|---|---|
| FLG/BD, 4 MiB or less | `0x60`, smallest BD | `0x60`, smallest BD | `0x64`, `0x70` | `0x60`/`0x40` up to 64 KiB, then `0x40`/`0x40` |
| FLG/BD, above 4 MiB | `0x40`, `0x70` | `0x60`, `0x70` | `0x64`, `0x70` | `0x40`, `0x40` |
| Data in record order | yes | no | often not | yes |
| Junk in padding | often | never | never | never |
| Files share data | never | with `-share` | with `-share` | not seen |
| Compression | all or none, plus an optional list | not strings or audio except `.fuz` | not strings | all or none |
| Flags | what the script asked | from content, never `0x8` or `0x20` | from content | `0x7` or `0x103` seen |

The BSArch columns were measured by running both versions on the same scratch trees. The CAO
column comes from the settings in rsm-bsa's source and from the 57 archives with its frames; we
did not run CAO.

**The best single proof is byte identity.** Packing one extracted file with Archive.exe and
comparing frames gives identical bytes for vanilla: a 1 MB mesh, a 5 MB mesh with linked blocks,
a 22 MB texture and an 800 KB text file. Other packers' frames differ. So this build, at its
default level, is what made them.

### Results

| Packer | Archives | Files | How we know |
|---|---|---|---|
| **Archive.exe** | **304** | 341,990 | Padding junk 280, linked frames above 4 MiB 10, byte-identical frame 14 |
| **BSArch 0.9e or libbsarch** | **87** | 32,917 | FLG `0x64` frames |
| **BSArch 1.0** | **1** | 2,848 | Independent frame above 4 MiB |
| **BSArch, version unknown** | **27** | 16,321 | Data out of record order, or shared data, and no junk |
| **CAO / rsm-bsa** | **57** | 41,826 | Linked `0x40` frames above 64 KiB |
| Unknown compressor | 1 | 106 | Header like Archive.exe, but bytes match neither it at any level from 3 to 16 nor BSArch 1.0 |
| Unresolved, uncompressed, flags `0x3`, type 0 | 395 | 217,569 | Archive.exe or rsm-bsa |
| Unresolved, uncompressed, type bits set | 21 | 10,695 | Archive.exe or BSArch |

Total 893. All 17 `Skyrim - *.bsa` archives are Archive.exe. Of the 74 Creation Club archives, 4
are proven Archive.exe and 70 are in the 395.

**Why padding junk is proof, but its absence is not.** Across the 172 archives proven to come from
other packers (3,941 folders) there is none. It is absent from many small Archive.exe archives,
though. Among 94 archives proven some other way, the share carrying junk rises with folder count:

| Folders | Proven Archive.exe | With junk |
|---|---|---|
| 1 to 4 | 35 | 18 |
| 5 to 9 | 18 | 13 |
| 10 to 19 | 14 | 13 |
| 20 to 49 | 13 | 12 |
| 50 to 99 | 5 | 5 |
| 100 or more | 9 | 9 |

So most of the 395 unresolved ones are small and could be either. But 17 of them have 50 or more
folders and no junk, which is unlikely for Archive.exe, and at least 2 are siblings of archives
with rsm-bsa frames from the same mod.

### Did the archives contradict the code?

Every prediction held on the 304 attributed archives:

| Prediction | Result |
|---|---|
| Folders and files sorted by hash | 304 of 304 |
| Data in record order (no `File Order`) | 301 of 301 with more than one file |
| Frame rule above | No breaks in 110,517 frames, 4,364 of them above 4 MiB |
| Flags within `0x3FF`, `0x3` always set | 304 of 304 |
| Embedded name is the lowercase folder and file | 17 archives, 0 mismatches |
| No 0-byte files | 0 in any of the 893 |
| Lowercase names | 0 uppercase names in any of the 893 |
| Hash | 0 mismatches over all 664,272 files |
| No compression fallback | Confirmed: 742 files stored larger than plain |

Three things looked like contradictions until checked:

- **Junk versus zero padding.** Most folder records are zero even in Archive.exe archives.
  This is the heap being clean, not another code path: 54 of 978 folders in `Skyrim - Meshes0.bsa`
  have junk.
- **Trailing bytes in `MarketplaceTextures.bsa`'s name table.** See "Adding a file twice" above.
- **A `.bsl` next to an archive with BSArch frames** (1 of the 7). The sidecar is left over from an
  earlier build, so a `.bsl` says nothing about the archive beside it now.

And one earlier note did not hold up: scripts are not always stored uncompressed. Of the 382
archives holding `.pex` files here, 78 compress them, for 5,609 of 54,686 scripts. The frames are
really there, and they come from 40 Archive.exe archives and 38 from BSArch.

## Still unknown

- **What the game does with the options.** Whether it needs the file type field, the retain flags
  or `Retain Strings During Startup`, and whether compressed `.wav`, `.xwm` or `.strings` load.
  The corpus has 103 compressed `.wav` and 2 `.xwm`, but 0 of 3,743 string files compressed. xEdit's
  source says scripts need `Retain File Names` to show in the Kit, and that textures with embedded
  names must be compressed; neither was tested here.
- **The 395 unresolved archives,** including 70 from the Creation Club. None of their traits
  separates Archive.exe from uncompressed rsm-bsa output. Eight of the Club ones have 20 or more
  folders and no junk, so Bethesda may have used another build or tool for them.
- **DarkwaterKeep.bsa:** the compressor is not identified.
- **libbsarch.** FLG `0x64` is proven for BSArch 0.9e. We did not run libbsarch, but 12 archives in
  that class with scripts flagged `0x7` don't match BSArch's flag rule, so another front end on the
  same code is likely.
- **The exact path out of the uncaught exception.** Exit `0xC0000409` is measured, and the throw site
  is seen under a debugger, where the same throw shows as `0xC000041D`. We don't have the fail-fast
  subcode for the run without a debugger.
- **`Scratch Path` on another drive.** The final step moves or replaces the file across drives
  (`140008d40`). The Kit's fixed `C:\temp` depends on this; not tested.
- **The GUI.** Everything here comes from script mode. The dialog's own handlers were not studied
  beyond the prompts shared with the script code.
