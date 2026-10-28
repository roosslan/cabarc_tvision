# Cabine

**English** | [Русский](readme.ru.md)

A CAB archiver and file manager for the Windows console, built on
[Turbo Vision](https://github.com/magiblot/tvision). CAB archives are created and modified
through the system Cabinet API (`cabinet.dll`); ZIP, RAR, 7Z, GZ, TAR and TAR.GZ can be
browsed and extracted.

![cabine](cabine.gif)

## Features

- A single panel for both the disk and archives: directories inside an archive are browsed
  the same way as on disk; on startup the panel opens where the program was closed.
- Creating CAB archives, adding and deleting files in them; extracting, testing and viewing
  (F3) archives of every supported format. The format is detected by file contents.
- CAB compression level chosen from a list, from "No compression" to "Maximum (LZX:21)".
- Multi-volume CAB and self-extracting archives (EXE), including multi-volume ones;
  the extractor is also built on Turbo Vision.
- ZIP passwords (PKWARE, WinZip AES) and RAR passwords, including RAR with encrypted headers.
- Built-in text viewer with UTF-8 / Windows-1251 / DOS-866 encoding switching.
- Editing a file with F4 in vim (the default), Notepad++ or Notepad; a file from a CAB
  archive changed in vim is updated in the archive.
- Explorer context menu and batch mode: adding to an archive and extracting without the
  panel, with a progress window and a "Cancel" button.
- Associating archive files with Cabine and choosing the F4 editor ("Settings" →
  "Options").
- Russian user interface, hotkeys with Cyrillic letters, Unicode file names.

## Formats

| Format | Read | Write | Library |
|---|---|---|---|
| CAB (including multi-volume and SFX) | yes | yes | Windows Cabinet API |
| ZIP | yes | — | minizip-ng, zlib |
| RAR (RAR4, RAR5) | yes | — | UnRAR |
| 7Z | yes | — | LZMA SDK |
| GZ, TAR, TAR.GZ | yes | — | zlib, own tar parser |

## Building

Requires Windows 10 or later, Visual Studio 2022 and CMake 3.16+. The libraries are
included as git submodules.

```bash
git clone --recursive https://github.com/roosslan/cabarc_tvision.git
```

```bash
cmake -S . -B build -A x64
```

```bash
cmake --build build --config Release
```

The result is `build\Release\cabine.exe` and `cabine-shell.exe` (a helper for the context
menu that lives next to `cabine.exe`). The self-extracting archive stub is embedded in
`cabine.exe`. The VC++ Redistributable is not required.

## Usage

| Command | Action |
|---|---|
| `cabine.exe [<directory \| archive>]` | Panel at the given location, or where the program was closed |
| `cabine.exe -a <path>...` | Add to `<name>.cab` next to the items |
| `cabine.exe -ai <path>...` | The same, with a window: archive name, compression, volume size, SFX |
| `cabine.exe -x <archive>...` | Extract into the archive's directory |
| `cabine.exe -e <archive>...` | Extract into a subdirectory named after the archive |
| `cabine.exe -ei <archive>...` | Extract into a directory chosen in a window |
| `cabine.exe --register` / `--unregister` | Add / remove the "Cabine" submenu in Explorer and the program directory in `Path` |

The submenu is written to `HKCU` of the current user (no administrator rights are needed)
and contains the items "Open in Cabine", "Extract here", "Extract to ...", "Add to CAB
archive" and "Add to archive ...". The program directory is added to the user's `Path`,
so `cabine` can be run by name in new console windows. After the program is moved, the
registration has to be repeated.

## Limitations

- ZIP, RAR, 7Z, GZ and TAR are opened read-only.
- CAB: a file in an archive and a volume are limited to 2 GB, at most 65535 files.
  Modifying an archive rebuilds it; multi-volume archives cannot be modified.
- LZX in `cabinet.dll` is very slow on repetitive data (for example, logs); "Fast (MSZIP)"
  is better suited for such data.
- A 7z solid block is extracted into memory as a whole.
- ZIP archives with BZip2, LZMA, Zstandard, XZ or PPMd methods can be opened, but such
  files are not extracted.
- Files for viewing and running are extracted into a temporary directory, which is deleted
  when Cabine exits.
- Settings are stored in `%APPDATA%\Cabine\cabine.ini` (the directory can be changed with
  the `CABINE_SETTINGS_DIR` variable).

## Turbo Vision workarounds

The `tvision` submodule is pinned to a specific commit. The library itself is not
modified; Cabine works around its limitations in its own code:

- the strings of standard dialogs are translated (`src/tvtext_ru.cpp` instead of
  `tvtext2.cpp`);
- hotkeys with Cyrillic letters and Cyrillic input in fields (`src/hotkeys.cpp`);
- a custom file dialog instead of `TFileDialog`/`TChDirDialog` (buffer overflows on long
  paths), multi-line text instead of `TStaticText` (255-byte limit).

## Third-party libraries

- **Turbo Vision**: Borland source code released by Borland; changes and new code by
  magiblot under the MIT license.
- **zlib**, **minizip-ng**: zlib license.
- **LZMA SDK** (from the 7-Zip repository): public domain, (c) Igor Pavlov.
- **UnRAR**: (c) Alexander Roshal, used unmodified for extraction only. UnRAR license
  condition (full text in `third_party/unrar/license.txt`):

  > UnRAR source code may be used in any software to handle RAR archives without
  > limitations free of charge, but cannot be used to develop RAR (WinRAR) compatible
  > archiver and to re-create RAR compression algorithm, which is proprietary.
  > Distribution of modified UnRAR source code in separate form or as a part of other
  > software is permitted, provided that full text of this paragraph, starting from
  > "UnRAR source code" words, is included in license, or in documentation if license
  > is not available, and in source code comments of resulting package.
