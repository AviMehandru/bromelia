# Bromelia

Bromelia is a native desktop front-end for `makemkvcon` (MakeMKV's command-line tool) built around
**multiple optical drives**. Every drive gets its own MakeMKV settings, conversion profile, title
rules, output naming and post-processing scripts, and drives rip in parallel.

| Platform | Stack | Folder |
| --- | --- | --- |
| macOS 14+ | SwiftUI, Xcode project | [`macos/`](macos) |
| Windows 10/11 | WinUI 3 (Windows App SDK), C# / .NET 8 | [`windows/`](windows) |
| Linux | C, GTK 4, libadwaita, meson | [`linux/`](linux) |

All three use the same configuration file format ([docs/configuration.md](docs/configuration.md)),
the same settings catalog ([`shared/catalog/settings-catalog.json`](shared/catalog/settings-catalog.json))
and the same test fixtures ([`shared/fixtures/`](shared/fixtures)), which are real `makemkvcon -r`
output. A configuration exported on one platform can be imported on the others.

Bromelia is not affiliated with MakeMKV. You need MakeMKV installed (and registered, or using the beta key).

## Features

### Everything the MakeMKV GUI does
- Drive list with drive model, device, tray state, disc label and disc type, updated automatically.
- Open a disc, an **ISO image** or a **BDMV / VIDEO_TS folder** (menu, file dialog or drag and drop).
- Title and track tree with checkboxes, and an information panel showing every attribute MakeMKV reports.
- **Make MKV** from the selected titles, choosing individual tracks.
- **Backup**, encrypted (1:1) or with video files decrypted.
- Rename output files per title (MakeMKV's expert-mode rename) and pick a one-off output folder.
- Eject.
- Progress with current and total operation, elapsed and remaining time, cancel, full colour-coded log.
- Every MakeMKV preference: data directory, expert mode, debug log, AV-sync messages, update check,
  proxy, message and preferred language, read retries, read buffer, minimum title length, structure
  protection removal, Java, CCExtractor, default profile, default selection rule, output file name
  template, backup decryption default, plus any other `settings.conf` key.
- MakeMKV profiles, either the default, a generated one or your own `.mmcp.xml`: selection rules, MKV
  default-track flags, ISO 639-2/T codes, chapter 0, and LPCM → FLAC/WAV conversion.
- Registration key entry (`makemkvcon reg`) and read-only drive/firmware information (`makemkvcon f`).

### And more
- **Per-drive configuration.** Each drive is matched by the identification string MakeMKV reports
  (model, firmware, usually serial), so the configuration follows the physical drive. Drives without
  their own configuration use a default one.
- **Truly separate MakeMKV settings per drive.** On macOS and Linux every job runs `makemkvcon` with a
  private `HOME` containing its own `settings.conf`. On Windows, where MakeMKV reads the registry, launches
  are serialised and the drive's values are applied only until `makemkvcon` has read them. The data
  folder (SDF, keys) and registration key are shared.
- **Parallel rips**, one job per drive at a time, with an optional global limit, a queue (reorder,
  cancel, retry, start now) and a persistent history with logs.
- **Rip modes per drive**: MKV; encrypted backup; decrypted backup; backup then MKV from the backup (one
  fast sequential read, safest for damaged discs); or scan only. Backups can be folders or ISO images
  (ISO output relies on MakeMKV's own support for `.iso` destinations).
- **Title rules** for unattended rips: all titles, the longest *N* (main feature), or an index pattern
  such as `0,2-4,7-` or `last`, matching MakeMKV title numbers or source playlist/VTS numbers. Filters
  cover duration, chapter count and size ranges, include/exclude regular expressions (title name, source
  file such as `00800.mpls`, segment map, …), skipping duplicate titles and alternate angles, and a
  maximum count. A live preview shows why each title is picked or skipped.
- **Automatic ripping on disc insert** with a cancellable countdown, plus automatic eject on success
  and/or failure and desktop notifications.
- **Output templates** for folders and file names using `{disc}`, `{type}`, `{drive}`, `{date}`,
  `{title}`, `{index}`, `{n:2}`, `{source}`, `{duration}`, `{chapters}`, `{original}`, and conditional
  text such as `{comment? - {comment}}`.
- **Post-processing**: any number of steps per drive (programs or scripts; `.ps1`, `.bat` and `.py` on
  Windows), run on success, failure or always, once per job or once per file, with a timeout and optional
  "fail the job". Steps get `BROMELIA_*` environment variables and a JSON manifest of the job. They can
  be test-run from the editor, and ready-made examples are included (move to library, HandBrake,
  logging, notifications).
- **Hand-picked tracks**: when you untick tracks, the title is ripped with every track and the unwanted
  ones are removed with `mkvmerge` (MKVToolNix). Bromelia checks the track layout and keeps the file
  untouched if it doesn't match.
- **Presets**: save a drive configuration, apply it to other drives, and import/export everything as JSON.
- **Command transparency**: every job logs and can copy the exact `makemkvcon` command lines.
- Safety check: if MakeMKV's drive numbering changes during a backup (backups must use `disc:N`), the job
  stops instead of reading the wrong disc.

## Building

### macOS
Requires Xcode 16 or newer (tested with Xcode 26) and MakeMKV in `/Applications`.

```bash
cd macos
xcodebuild -project Bromelia.xcodeproj -scheme Bromelia -configuration Release -derivedDataPath build/DerivedData build
open build/DerivedData/Build/Products/Release/Bromelia.app
```

Or open `macos/Bromelia.xcodeproj` in Xcode. The app is not sandboxed (it runs `makemkvcon`, your scripts
and `mkvmerge`) and is ad-hoc signed; set your own team for distribution.

### Windows
Requires Windows 10 1809+ and the .NET 8 SDK (Visual Studio 2022 with the *Windows application
development* workload also works; open `windows/Bromelia.sln`).

```powershell
cd windows
dotnet build src/Bromelia.App/Bromelia.App.csproj -c Release -p:Platform=x64
dotnet publish src/Bromelia.App/Bromelia.App.csproj -c Release -p:Platform=x64 -r win-x64 --self-contained -o publish
```

The app is unpackaged and self-contained (Windows App SDK included). MakeMKV is found in
`Program Files (x86)\MakeMKV` automatically.

### Linux
Requires GTK ≥ 4.12, libadwaita ≥ 1.5, json-glib and meson (Debian 13, Ubuntu 24.04, Fedora 40 or newer).

```bash
sudo apt install meson ninja-build libgtk-4-dev libadwaita-1-dev libjson-glib-dev   # Debian / Ubuntu
cd linux
meson setup builddir
ninja -C builddir
./builddir/src/bromelia
sudo ninja -C builddir install   # optional: desktop file, icon, AppStream metadata
```

`-Dui=false` builds only the core library and tests (no GTK needed).

## Testing

Unit tests on all three platforms run against the shared fixtures. Each suite also contains an
end-to-end test that rips a real disc image through the complete pipeline (title choice, `mkvmerge`
track removal, renaming, post-processing script, manifest) when `BROMELIA_TEST_ISO` points to an ISO.

```bash
# macOS
cd macos && xcodebuild -project Bromelia.xcodeproj -scheme Bromelia -derivedDataPath build/DerivedData test
TEST_RUNNER_BROMELIA_TEST_ISO=/path/disc.iso xcodebuild … test          # with the end-to-end test

# Windows core (runs on any OS with the .NET 8 SDK)
cd windows && dotnet test tests/Bromelia.Core.Tests/Bromelia.Core.Tests.csproj

# Linux
cd linux && meson test -C builddir
BROMELIA_TEST_ISO=/path/disc.iso meson test -C builddir
```

[`.github/workflows/ci.yml`](.github/workflows/ci.yml) builds and tests all three platforms,
including the full WinUI 3 build on a Windows runner.

## Repository layout

```
shared/     settings catalog, robot-output fixtures, icon source
macos/      Xcode project: Bromelia (SwiftUI app) and BromeliaTests
windows/    Bromelia.sln: Bromelia.Core (platform-neutral engine), Bromelia.App (WinUI 3), tests
linux/      meson project: core static library, GTK application, tests, desktop data
docs/       configuration format and architecture notes
```

See [docs/architecture.md](docs/architecture.md) for how the pieces fit together.

## Limitations
- MakeMKV 2.0 removed the streaming server, so Bromelia has no streaming feature.
- Firmware flashing isn't offered. Only read-only commands of MakeMKV's firmware tool are exposed.
- On Windows, per-drive MakeMKV settings rely on applying registry values around each launch. If the
  MakeMKV GUI is open at the same time and saves its preferences, it may write values back.
- Individual track selection needs `mkvmerge` from MKVToolNix.
