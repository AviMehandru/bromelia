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
| Disc identity, naming, plugin matching | `Core/MediaIdentity.swift` | `Logic/MediaIdentity.cs` | `bro-identity.c` |
| DVD navigation (episodes in “play all” titles) | `Core/DVDNavigation.swift` | `Logic/DvdNavigation.cs` | `bro-dvd.c` |
| Checksums, archive record, episode splitting tools | `Engine/Archive.swift`, `Engine/EpisodeSplitter.swift` | `Engine/Archive.cs` | `bro-identity.c`, `bro-runner.c` |
| Disc fingerprints, earlier archives, verifying archives | `Engine/Archive.swift` | `Engine/Archive.cs` | `bro-archive.c` |
| Notifications, online lookup, media server names, other discs | `Engine/Integrations.swift` | `Engine/Integrations.cs` | `bro-integrations.c` |
| App state: drives, sessions, queue, history | `App/AppModel.swift` | `Engine/AppState.cs` | `bro-state.c` |
| Background queue, web page | `App/BackgroundQueue.swift`, `App/WebServer.swift` | `Engine/Services.cs` | `bro-state.c`, `bro-web.c` |
| UI | SwiftUI views; none (`--headless`, `App/Headless.swift`) | WinUI 3 pages; none (`Bromelia.Daemon`, `bromelia-daemon.exe`) | GTK 4 / libadwaita widgets (`bromelia`); none (`bromelia-daemon`) |

## makemkvcon

Everything goes through `makemkvcon -r` (robot mode) with `--progress=-same`:

- **Drive scan:** `makemkvcon -r --cache=1 info disc:9999` lists drives (`DRV:` lines) without opening
  a disc. It runs at startup, every *N* seconds when idle, and a few seconds after the OS reports a
  media change (DiskArbitration on macOS, drive polling on Windows, GIO's volume monitor on Linux).
- **Open disc:** `info dev:<device>` (or `iso:` / `file:`) produces `CINFO` / `TINFO` / `SINFO`
  attribute lines, which become a disc → titles → tracks model.
- **Rip:** `mkv <source> <title|all> <folder>` per selected title (a single `all` when every title is
  selected and no tracks were hand-picked). Output files are found by comparing the folder contents
  before and after. The folder is a hidden staging folder inside the output folder (see below).
- **One pass:** `mkv` takes one title or `all`, so a set of titles normally needs one run each (each run reads
  the disc structure again). When three or more titles are chosen and they are exactly the titles longer than
  some length, the job reads the listing with `--minlength` set between them and the rest, checks that it holds
  exactly the chosen titles (by source title, length and segment map), and rips `all` with that minimum length:
  one run. Files are mapped to titles through that listing's output file names.
- **Listing:** every job runs `info` again, even when the disc was opened before. Choices made on the
  opened listing are moved to the new title numbers (matched by source title, length and segment map),
  and the job stops if the disc changed or a chosen title is gone.
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

## Identity, episodes and archiving

After the disc listing is read (for backups too, where a failed listing is only a warning), the job
resolves a *media identity*: name, movie / TV, format (DVD / Blu-ray / 4K UHD) and the place in a set
parsed from the label. It names the output folder and, through the file name template, every file and
backup.

For DVDs of TV shows, `ripTitles` first opens the disc's `VIDEO_TS` — the ISO or folder being ripped, the
backup in *backup then MKV* mode, or the mounted disc — and analyses its navigation:

1. `VIDEO_TS.IFO` maps disc titles to title sets; each `VTS_nn_0.IFO` gives, per title, the chapter
   (PTT) list, each chapter's cells and playback time (×1.001 for NTSC) and the VOB of its first cell.
2. Jump commands are collected from the VMG menu program chains, the menu buttons in the NAV packs of
   `VIDEO_TS.VOB` and `VTS_nn_0.VOB` (`JumpTT`, `JumpVTS_PTT`), and the pre-commands of each title's
   first program chain (`LinkPTTN`, typically “if GPRM7 == 21: LinkPTTN 8” for episode 2).
3. A title entered at several chapters gives an *episode plan*: starts, the end of the last episode (VOB
   boundary), trailing chapters and durations. Plausibility checks keep scene-selection menus of movies
   from being treated as episodes.
4. Menu stills (one per VOB cell) are decoded with `ffmpeg` and read with `tesseract` to find episode
   numbers; the first episode is the start that covers the most numbers read.

After ripping, the MKV of the planned title is checked (`mkvmerge -J` duration, `mkvextract` chapter
times) and split with `mkvmerge --split chapters:…` into hidden temporary files, which are then renamed
with the episode number and chapter range. Finally every produced file is hashed (SHA-256, streamed,
with progress) and `SHA256SUMS` and `bromelia.json` are written before post-processing, so scripts and
plugins can use them.

## Staging, read errors and checks

Rips and backups are written to `<output folder>/.bromelia-incomplete-<job>`. Every ripped MKV is checked
against its title in the listing with `mkvmerge -J` (length, tracks) before it is renamed, and backups are
checked for a disc structure. Error messages that makemkvcon prints during `mkv` and `backup` (read errors,
hash check failures) are collected; a job that otherwise succeeded but has any of them ends as *completed
with read errors*. When the job ends, files are hashed in the staging folder and then moved: into the
output folder on success, or into a folder marked `[READ ERRORS]` / `[INCOMPLETE]` with a note file
otherwise. Paths recorded during the job (files, episodes, checksums) are rewritten to the final location
before the manifest, archive record and post-processing see them.

## Archive checks

- **Disc fingerprints** (`DiscFingerprint` / `bro_disc_fingerprint`) hash the listing's volume name, title count and,
  sorted, every title's source id, length, segment map and size (the exact text is in configuration.md), so the value
  is the same on every platform; `shared/fixtures/fingerprints.json` pins it for the tests. The job computes it after
  reading the listing, stores it in `bromelia.json` and hands it back for the history.
- **Earlier archives** (`ArchiveLookup` / `bro_find_archived`): the app state passes the history's successful jobs
  (fingerprint, folder, date) to the job when it starts; the job looks there first, then walks the output root (four
  levels, hidden folders skipped) for `bromelia*.json` records with status `success` — on the job's thread (a task off
  the main actor on macOS, `Task.Run` on Windows). The disc page uses the history only, so it stays instant.
- **Verifying** (`ArchiveVerifier` / `bro_verify_folders`) collects the folders with a `SHA256SUMS`, sums up the listed
  files' sizes for the progress, re-hashes each file with the streaming hasher and lists the folder's other files.
  The app state runs one check at a time on a worker thread; progress is copied to the UI four times a second, and
  the results are recorded in `archive-checks.json` per folder and for the checked root. The scheduled check
  (`archiveCheck.intervalDays`) is started by the one-second tick when the root's record is old enough and no job
  runs; its notifications are sent from the worker thread.

## Unfinished jobs

The queue lives in memory. So that a crash, a power cut or a forced quit never loses a job silently, the queued,
waiting and running jobs are also written to `unfinished-<pid>.json` in the data folder (next to `history.json`)
whenever they change: id, title, drive, disc, mode, state, the output folder once the job has chosen it, and the log
path. The file is removed when no job is left.

At start, each `unfinished-*.json` whose process has gone (the pid isn't running, or it is this process's pid but a
different instance token, as for pid 1 in a container) is turned into history records: running jobs as *failed*
(“Interrupted: …”), queued and waiting ones as *cancelled* (“Not started: …”). A running job's staging folder
(`<output folder>/.bromelia-incomplete-<id>`) is renamed to `INCOMPLETE - <id>` with an `INCOMPLETE.txt` note, or
removed (with the output folder, if that is then empty) when nothing visible was saved. The app shows a message and
the daemon logs it. Files of processes that are still running (the Linux app next to `bromelia-daemon`) are left
alone. The jobs themselves are not queued again: the disc in the drive may have changed since.

## Stopping processes

`makemkvcon` ignores SIGINT, so it is stopped with SIGTERM (other tools get SIGINT first), then SIGKILL
after 5 s (Windows kills the process tree at once). A watchdog stops a `makemkvcon` run that prints
nothing for the stall timeout. Once a process has ended, its output is read for at most 5 more seconds (a
child it left behind may keep the pipe open), and a process that is still there 30 s after SIGKILL — stuck
in the kernel on a hung drive — is abandoned so the job can end. On Linux all of this runs on a watchdog
thread per process, so it doesn't depend on a main loop.

The robot messages that decide a job's outcome were checked against real `makemkvcon` 2.0 output recorded
in `shared/fixtures`: a full disk (`rip-disk-full`: exit status 0, message 5038, write error 2018, the
partial file deleted by MakeMKV), a process killed while writing (`rip-killed`: exit 143, partial file
left), a missing source (`rip-missing-source`: exit 11, message 5006) and a missing title
(`rip-missing-title`: exit 12). `makemkvcon` exits with 0 when a title fails, so the saved / failed counts
(5036, 5037, 5004) and the output files decide. The job's error message is the first error of the run
other than the saved / failed summary. `rip-outcomes.json` lists the expected result of each recorded run;
the tests of all three platforms replay it through the job pipeline with a stand-in `makemkvcon`.

## Sleep

While jobs or background steps run (and `preventSleep` is on), macOS gets a `ProcessInfo` activity (no idle sleep,
no App Nap), Windows `SetThreadExecutionState(ES_SYSTEM_REQUIRED)`, and Linux an inhibitor chosen by the front-end:
the GTK app uses `gtk_application_inhibit` (suspend and idle, through the desktop), and `bromelia-daemon` asks
systemd-logind over the system bus (`Inhibit("sleep:idle", …, "block")`, `bro-sleep.c`), keeping the returned file
descriptor open until the work is done. When logind can't be reached (a container without the system bus socket,
a system without systemd) the daemon logs it once and carries on. The app state decides when; the inhibitor only
turns on and off.

## Services

- **Notifications and online lookup** are plain HTTPS requests (`URLSession`, `HttpClient`, and `curl` on Linux
  with the request written to a private config file so keys stay off the command line). Other Apprise URLs run
  the `apprise` command. They happen on the job's thread at the end of the job (notifications) or after the
  listing has been read (lookup); failures are logged and never fail a job.
- **Online lookup** (`MetadataLookup` / `bro_metadata_*`): a search ranks the candidates (title, then year hint), a
  chosen id (`OnlineId` / `bro_online_id_parse`) is read with the provider's details or `/find` request, and a
  season's episodes give the episode titles once the numbering is settled. The job looks up after reading the
  listing and again if the menus turn the disc into a TV show; the disc page runs the same search in the background
  when a disc is opened (a task, `Task.Run`, a `GTask`) and keeps the candidates on the disc session, which a
  generation counter protects from stale answers.
- **Numbering across discs** (`EpisodeContinuation` / `bro_episode_continuation`) reads the `bromelia*.json` records
  of the history's folders and the output root on the job's thread, after the menus were read and before the
  episode titles are looked up.
- **Other discs.** Before an automatic or quick rip, a disc whose `DRV` flags show no DVD / Blu-ray structure is
  probed (DiskArbitration, the Windows table of contents, udev) and gets the `audioCD` or `dataImage` mode. Those
  jobs skip `makemkvcon` entirely: the audio command runs in the staging folder, and data discs are copied to an
  ISO there; staging, checksums, records and marked folders work as for any other job.
- **Background queue.** Steps marked `background` are handed from the finished job to a queue in the app state
  that runs them `backgroundJobs` at a time on worker threads, with the job's tokens and variables captured when
  the job ended.
- **Web page.** A small HTTP/1.1 server (Network.framework, `TcpListener`, `GSocketService`) answers one request
  per connection on the UI thread: the page itself (`shared/web/bromelia-web.html`, bundled as a resource), a
  status document built from the app state, the end of a job's log, and actions that call the same functions as the
  UI (opening a disc fills the drive's disc session, as the disc page does). HTTPS wraps the same connections in
  TLS: `NWProtocolTLS` with an identity imported from a PKCS #12 that `openssl` makes of the PEM files, `SslStream`,
  and `GTlsServerConnection`.
- **Media server layout** swaps the folder and file templates for fixed ones and makes the show / movie folder
  shared between jobs: when files are moved out of the staging folder, folders that exist already (`Season 02`,
  `Other`, `Backup`) are merged item by item instead of getting a numbered name.
- **Without a window**: `bromelia-daemon` (Linux), `Bromelia --headless` (macOS: the same binary, which starts
  `NSApplication` without windows instead of the SwiftUI app) and `bromelia-daemon.exe` (Windows: a console project on
  the engine, with its own single-thread `SynchronizationContext` and a one-second timer for the queue, the media
  check and drive polling) are the app state without a window: they scan drives, start automatic rips, run the
  background queue and serve the web page, logging job changes to stdout. Automatic rips and the scheduled archive
  check happen only in the process that holds `automation.lock` (`flock` on macOS and Linux, a file opened without
  sharing on Windows); the others try again every minute. Command-line web settings are kept out of the saved
  configuration. `docker/Dockerfile` builds it
  together with MakeMKV (see [docker.md](docker.md)).
