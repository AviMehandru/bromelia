# Architecture

The three front-ends are independent native applications with the same structure, so behaviour
matches across platforms:

| Layer | macOS (Swift) | Windows (C#) | Linux (C) |
| --- | --- | --- | --- |
| Robot protocol parser, disc model | `Core/RobotProtocol.swift`, `Core/DiscInfo.swift` | `Bromelia.Core/Robot` | `bro-robot.c` |
| Configuration (shared JSON) | `Core/Configuration.swift` | `Bromelia.Core/Config` | `bro-config.c` |
| Title rules, templates, argument splitting | `Core/TitleSelector.swift`, `Core/Templates.swift` | `Bromelia.Core/Logic` | `bro-logic.c` |
| settings.conf, profiles, per-drive environment | `Core/MakeMKVFiles.swift`, `Engine/MakeMKV.swift` | `Logic/MakeMKVFiles.cs`, `Engine/MakeMKV.cs` | `bro-makemkv.c` |
| Job pipeline, post-processing, mkvmerge | `Engine/JobRunner.swift`, `Engine/PostProcessor.swift` | `Engine/JobRunner.cs`, `Engine/PostProcessor.cs` | `bro-runner.c` |
| App state: drives, sessions, queue, history | `App/AppModel.swift` | `Engine/AppState.cs` | `bro-state.c` |
| UI | SwiftUI views | WinUI 3 pages | GTK 4 / libadwaita widgets |

## makemkvcon

Everything goes through `makemkvcon -r` (robot mode) with `--progress=-same`:

- **Drive scan:** `makemkvcon -r --cache=1 info disc:9999` lists drives (`DRV:` lines) without opening
  a disc. It runs at startup, every *N* seconds when idle, and a few seconds after the OS reports a
  media change (DiskArbitration on macOS, drive polling on Windows, GIO's volume monitor on Linux).
- **Open disc:** `info dev:<device>` (or `iso:` / `file:`) produces `CINFO` / `TINFO` / `SINFO`
  attribute lines, which become a disc → titles → tracks model.
- **Rip:** `mkv <source> <title|all> <folder>` per selected title (a single `all` when every title is
  selected and no tracks were hand-picked). Output files are found by comparing the folder contents
  before and after.
- **Backup:** `backup [--decrypt] disc:<N> <folder or .iso>`. MakeMKV only accepts `disc:N` for backups,
  so Bromelia checks the `DRV:` lines the job prints to make sure drive *N* is still the expected device.

Drives are addressed by device (`dev:`) wherever possible because MakeMKV's drive numbers can change.

## Per-drive MakeMKV settings

`makemkvcon` reads its preferences from `~/Library/MakeMKV/settings.conf` (macOS),
`~/.MakeMKV/settings.conf` (Linux) or `HKCU\Software\MakeMKV` (Windows).

- **macOS / Linux:** every job gets its own folder containing a `settings.conf` built from the global
  settings plus the drive's overrides, and `makemkvcon` runs with `HOME` pointing there. `app_DataDir`
  points back to the user's real MakeMKV folder so downloaded data and keys are shared, and the
  registration key is copied from MakeMKV's own settings unless one is configured. Debug logs
  (`MakeMKV_log.txt`) therefore land in the job folder.
- **Windows:** launches are serialised by a process-wide lock. For each launch the drive's values are
  written to the registry (other catalog keys are cleared), `makemkvcon` is started, and the previous
  values are restored as soon as it prints its first line (it has read its settings by then), exits, or
  after 15 seconds.

The generated profile (`profile.mmcp.xml`) is written to the same job folder and passed with `--profile`.

## Threads

- **macOS:** process output is read on a background queue, parsed, and delivered to the main actor in
  order. Progress updates are coalesced to 10 per second.
- **Windows:** the engine captures the UI `SynchronizationContext` and posts parsed events to it. The job
  pipeline is an `async` method running on the UI thread.
- **Linux:** the pipeline (`bro_run_job`) is synchronous and runs in a `GTask` worker thread. Updates are
  posted with `g_main_context_invoke_full` at idle priority (below GTK's redraw priority, so a busy rip
  can't starve painting), and the task completes at the same priority so its result is applied after
  every update.

## Hand-picked tracks

A title whose tracks were customised is ripped with a second environment whose selection rule is
`+sel:all`, so the MKV contains every stream in `SINFO` order. `mkvmerge -J` confirms the file has the
same number and types of tracks, then `mkvmerge --video-tracks/--audio-tracks/--subtitle-tracks` keeps
the chosen ones. If the layout doesn't match, the file is kept unchanged and a warning is logged.
