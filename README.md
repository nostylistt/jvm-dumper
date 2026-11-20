# JVM Heap Dumper

A minimal injectable Windows DLL that dumps the full HotSpot JVM heap on demand.

Useful for reverse-engineering object layouts, chasing pointers through live memory,
or correlating two dumps of the same process taken at different moments.

## What it does

Once the DLL is loaded into a running `java.exe` / `javaw.exe` process, a console
window opens and listens for two hotkeys:

- **F8** — take a dump. Each press produces a new pair of files in `%TEMP%`:
  - `heap_dumpN.bin`      — the dump itself
  - `heap_dumpN_regions.txt` — human-readable index of regions inside the dump
- **END** — unload the DLL cleanly.

### Dump layout

```
Region #00 = the entire configured heap range, as a sparse file.
             Committed pages are written at offset (VA - HEAP_LO);
             uncommitted gaps stay as zeros.
             File length = HEAP_HI - HEAP_LO exactly (typically ~4 GB on disk,
             but only committed bytes occupy real space thanks to NTFS sparse files).

Region #01..N = other committed private/mapped regions outside the heap
                (class space, metaspace, JVM internals), each ≤ 256 MB,
                appended sequentially. See the companion _regions.txt for
                VA ranges and file offsets.
```

The layout is stable across dumps of the same process, so an analyzer can seek
to `(VA - HEAP_LO)` directly for any heap address.

## Configuration

Defaults assume HotSpot with ~4 GB max heap and the common base
`0x6C0000000..0x7C0000000`. If your JVM uses a different base (check with
VMMap or by inspecting a test dump), override via environment variables
on the target process **before** injection:

```
set HEAP_LO=700000000
set HEAP_HI=800000000
```

Values are hex without `0x`.

## Build

Requires an MSVC toolchain (Build Tools for Visual Studio or Visual Studio).
From a `x64 Native Tools Command Prompt`:

```
build.bat
```

Output: `out\heap_dumper.dll`.

## Use

1. Start the target JVM.
2. Inject `heap_dumper.dll` into the JVM process using any standard
   DLL injector (manual `CreateRemoteThread(LoadLibraryA)`, process
   hollowing, your own tool, etc.). This repo does **not** ship an
   injector — bring your own.
3. A console titled `JVM Heap Dumper` appears.
4. Press **F8** whenever you want a snapshot. Dumps land in `%TEMP%`.
5. Press **END** to unload.

Tips:

- Take at least two dumps of meaningfully different JVM states (different
  worlds/maps/scenes/collections) so you can diff them to narrow down the
  object you're after.
- A dump equals the heap range size on disk. On NTFS it stays sparse
  (actual disk use ≈ committed heap). On FAT32/exFAT it won't — watch free
  space.
- The index file (`_regions.txt`) lists the extra regions appended after
  Region #00, with their VA and file offset.

## Platform

- Windows x64 only.
- Only reads the current process's own memory — must run **inside** the
  target JVM (injected DLL), not from an external process.
- Tested against 64-bit HotSpot (OpenJDK and various redistributions).

## License

MIT. See `LICENSE`.
