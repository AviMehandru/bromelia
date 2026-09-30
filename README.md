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
- Open a disc, an **ISO image**, a **BDMV / VIDEO_TS folder**, or a file inside one (`.IFO`, `.VOB`, `.mpls`,
  `.m2ts`, …: the disc it belongs to is opened) from the menu, the file dialog or drag and drop.
- **LibreDrive** status of the drive for the opened disc (enabled with its version, not in use, or required but
  not available).
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
- **Expired key or outdated MakeMKV** is detected in every `makemkvcon` run and shown as a banner (and as the
  reason of failed jobs), with a button that reads the current free beta key from MakeMKV's forum and
  registers it.
- **One pass for several titles**: when the chosen titles are the longest ones on the disc, they are ripped in a
  single `makemkvcon` run (like the GUI) instead of one run per title.

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
  (MakeMKV writes an ISO when the destination ends in `.iso`; if it writes a folder instead, the checked
  folder is kept). The *Archive everything* preset sets up backup then MKV with every track and all checks.
- **Title rules** for unattended rips: all titles, the longest *N* (main feature), or an index pattern
  such as `0,2-4,7-` or `last`, matching MakeMKV title numbers or source playlist/VTS numbers. Filters
  cover duration, chapter count and size ranges, include/exclude regular expressions (title name, source
  file such as `00800.mpls`, segment map, …), skipping duplicate titles and alternate angles, and a
  maximum count. A live preview shows why each title is picked or skipped.
- **Automatic ripping on disc insert** with a cancellable countdown, plus automatic eject on success
  and/or failure and desktop notifications.
- **Knows what it is ripping.** The movie or show name is read from the disc (Blu-ray metadata title or
  the volume label: `ONE_PIECE_S2_P7_D2` → *One Piece*, Season 2 Part 7 Disc 2), movies and TV shows are
  told apart (season / volume markers, episode-length titles, episode menus), and the format is detected:
  DVD, Blu-ray or 4K Ultra HD (2160p / HEVC video, or `index.bdmv` version 3 in a backup). The name and
  movie / TV choice can be changed on the disc page before ripping.
- **Archive-style names.** Files and backups are named
  `{Name} - {Episode} - {Disc} - {Rip/Backup} - {Title} - {Format}`, leaving out parts that don't apply,
  e.g. `One Piece - Episode 138 - Season 2 Part 7 Disc 2 - Rip - Title 11 Ch 1-7 - DVD.mkv` or
  `Inception - Backup - 4Ke`. Format codes are `DVD`, `BR` and `4K`, with an `e` suffix (`DVDe`, `BRe`,
  `4Ke`) for backups that were not decrypted.
- **TV episodes from “play all” titles.** Many TV DVDs store several episodes in one title. Bromelia reads
  the disc's own menu navigation (IFO title tables, menu and button jump commands) to find where each
  episode starts and ends, and splits the MKV into one file per episode with `mkvmerge` (no re-encoding).
  Episode numbers are read from the episode menus with `ffmpeg` + `tesseract` when installed, continued from
  the previous disc of the set (`ONE_PIECE_S2_P7_D2` starts after the last episode archived from `…_D1`), or
  entered on the disc page. Works from a disc, an ISO or a backup.
- **Checksums and archive records.** Every output folder gets `SHA256SUMS` (checkable with
  `sha256sum -c SHA256SUMS` / `shasum -a 256 -c SHA256SUMS`), `bromelia.json` (disc identity, titles,
  episodes, files with sizes and hashes, MakeMKV version, errors, disc fingerprint) and the job log.
- **Archive check.** *Verify Archive…* (Archive Check in the sidebar) reads every file of the output folder's
  archives, or of any folder, again and compares it with `SHA256SUMS`, reporting changed, unreadable, missing and
  unlisted files per folder; a finished job's folder can be checked from the history, which shows when each
  folder was last verified. `archiveCheck.intervalDays` repeats the check of the whole output folder on a schedule
  and sends the result to the notifications (also in `bromelia-daemon`, and `POST /api/verify` from the web page).
- **Discs archived before are recognised** by a fingerprint of their listing (volume name and every title's
  source, length, segment map and size), looked up in the history and in the output folder's `bromelia.json`
  files. An automatic rip skips such a disc (or stops and asks, or rips it again: `automation.alreadyArchived`),
  and the disc page says so and asks before ripping it again by hand.
- **Output templates** for folders and file names using `{name}`, `{episode}`, `{episodeTitle}`, `{discLabel}`, `{rip}`,
  `{track}`, `{format}`, `{season}`, `{disc}`, `{type}`, `{drive}`, `{date}`, `{title}`, `{index}`,
  `{n:2}`, `{source}`, `{duration}`, `{chapters}`, `{original}`, and conditional text such as
  `{comment? - {comment}}`.
- **Post-processing**: any number of steps per drive (programs or scripts; `.ps1`, `.bat` and `.py` on
  Windows), run on success, failure or always, once per job or once per file, with a timeout and optional
  "fail the job". Steps get `BROMELIA_*` environment variables (including the name, format code and
  checksum file) and a JSON manifest of the job. They can be test-run from the editor, and ready-made
  examples are included (move to library, HandBrake, logging, notifications).
- **Plugins**: post-processing steps for every drive that run only for matching discs — by movie / show
  name or disc label (regular expression) and by format code (`DVD`, `BRe`, `BR*`, …). Use them for
  anything particular to one show, one disc or one kind of disc.
- **Hand-picked tracks**: when you untick tracks, the title is ripped with every track and the unwanted
  ones are removed with `mkvmerge` (MKVToolNix). Bromelia checks the track layout and keeps the file
  untouched if it doesn't match.
- **Presets**: save a drive configuration, apply it to other drives, and import/export everything as JSON.
- **Command transparency**: every job logs and can copy the exact `makemkvcon` command lines.
- **Nothing unfinished looks finished.** Rips go to a hidden staging folder and reach the output folder only
  when the job succeeded. If MakeMKV reported read errors (scratches, hash check failures) the job ends as
  *completed with read errors* and the files are kept in `<folder> [READ ERRORS]`; failed or cancelled jobs
  leave `<folder> [INCOMPLETE]`, with a note explaining why and unfinished titles under MakeMKV's own names.
- **Checks against the disc listing.** Every MKV is compared with its title (length, tracks) using
  `mkvmerge -J`, and backups must contain a disc structure. Every job reads the disc listing again, finds
  the chosen titles even if MakeMKV renumbered them, and stops if the disc was changed.
- **Nothing lost on a crash.** Queued and running jobs are written down as they change. If Bromelia stops
  unexpectedly (a crash, a power cut, a forced quit), the next start lists them in the history as interrupted
  or not started, and turns an interrupted job's hidden staging folder into a visible `INCOMPLETE - <id>` folder
  with a note.
- **Unattended safeguards.** The computer is kept awake while jobs or background steps run (also by
  `bromelia-daemon`, through systemd-logind); a rip that stops producing output
  (a stuck drive) is stopped after a configurable time; a job that wouldn't fit on the destination stops
  before writing (from the titles' sizes, or MakeMKV's own warning); error messages name the actual cause
  (e.g. “No space left on device”).
- Safety check: if MakeMKV's drive numbering changes during a backup (backups must use `disc:N`), the job
  stops instead of reading the wrong disc.

### Like the other makemkvcon wrappers (ARM, docker-makemkv, MakeMKV-Auto-Rip, riplex, …)
- **Online lookup** of the movie or show on TMDb or OMDb for canonical names such as `Inception (2010)`, with
  `{releaseYear}`, `{tmdb}` and `{imdb}` tokens, and **episode titles** of TV shows (`{episodeTitle}`, FileBot /
  riplex style). The disc page lists the other results, takes a release year, and lets you pick another result or
  type a TMDb / IMDb id (or paste its web address) when the best match is wrong; the job log names the runners-up.
- **Plex / Jellyfin / Emby layout**: `Movies/Name (Year)/Name (Year).mkv`,
  `TV Shows/Name (Year)/Season 02/Name (Year) - S02E05 - Episode Title.mkv`, extras in `Other/`, backups in an
  ignored `Backup/`; later discs are added to the same show and season folders, and their episode numbers continue
  after the ones already there. **Kodi / Jellyfin / Emby metadata**: `tvshow.nfo` or the movie's `.nfo`, an `.nfo`
  per episode (title, plot, air date) and `poster.jpg`, from the online lookup.
- **Audio CDs** ripped with cyanrip or abcde (MusicBrainz tags, FLAC) and **data discs** saved as exact ISO images,
  chosen automatically from what is in the drive.
- **Separate modes for DVDs, Blu-rays and 4K discs** (e.g. backups of DVDs, MKVs of Blu-rays).
- **Built-in transcoding** (as in ARM): a *Transcode with HandBrake* step encodes every ripped MKV with a HandBrake
  preset (or one exported from HandBrake) in the background queue, into its own files next to the untouched archive.
- **Notifications** to Discord, Slack, ntfy, any webhook, or any Apprise service (Telegram, Pushover, e-mail, …).
- **Background post-processing**: encoding or upload steps run after the disc is out, in their own queue, so the
  drive is free for the next disc.
- **Web page** for watching and controlling Bromelia from a browser: rip, eject, close tray, cancel; open a disc and
  rip the titles you tick; job logs and history details; automatic rips and modes per drive; with a JSON API,
  local-only by default, token-protected on a network, and HTTPS with a PEM certificate and key.
- **Headless**: `bromelia-daemon` and a Docker image for servers and NAS boxes ([docs/docker.md](docs/docker.md)).
- **Beta key kept current** automatically (at startup and when it expires; purchased keys are never replaced).
- **Trays**: close one drive's tray or all trays; automatic rips wait for the system to mount the disc first.
- **Sequential or parallel** ripping: one job per drive in parallel, or set the maximum number of jobs to 1.

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
Requires Windows 10 1809+, the .NET 8 SDK (or newer) and Visual Studio 2022 or newer (or its Build Tools)
with the *Windows application development* workload; open `windows/Bromelia.sln`, or build from a
*Developer PowerShell*:

```powershell
cd windows
msbuild src/Bromelia.App/Bromelia.App.csproj -restore -p:Configuration=Release -p:Platform=x64
msbuild src/Bromelia.App/Bromelia.App.csproj -restore -t:Publish -p:Configuration=Release -p:Platform=x64 -p:RuntimeIdentifier=win-x64 -p:SelfContained=true -p:PublishDir=publish\
```

Use Visual Studio's `msbuild`, not `dotnet build`: the Windows App SDK loads its resource (PRI) build
tasks from Visual Studio's MSBuild folder, which the .NET SDK doesn't include (`dotnet build` fails with
MSB4062 *ExpandPriContent*). The core library and its tests build with plain `dotnet`.

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

`-Dui=false` builds only the core library, `bromelia-daemon` and the tests (no GTK needed).

### Headless / Docker
Bromelia runs without a window on every platform (automatic rips, post-processing, the archive check, notifications,
the web page), with the same options (`--config`, `--listen`, `--port`, `--token` or `BROMELIA_WEB_TOKEN`), one line
per job change, and a clean stop on SIGTERM / Ctrl+C (running jobs are cancelled; a second signal quits at once):

| Platform | Command | At login |
| --- | --- | --- |
| Linux | `bromelia-daemon` | a systemd user service, or Docker (below) |
| macOS | `/Applications/Bromelia.app/Contents/MacOS/Bromelia --headless` | *Settings → General → Run Bromelia in the background at login*, or `Bromelia --install-agent` (a launchd agent; log in `~/Library/Logs/Bromelia/headless.log`) |
| Windows | `bromelia-daemon.exe` (next to `Bromelia.exe`) | *Settings → Run bromelia-daemon at every logon*, or `bromelia-daemon --install-task` (Task Scheduler; log in `daemon.log` in the data folder) |

Only one Bromelia at a time rips inserted discs and runs the scheduled archive check: the one holding
`automation.lock` in the data folder. The app says so when a background Bromelia has it, and takes over a minute after
that one quits (and the other way round). Web settings given on the command line are not written to the configuration.

`docker/Dockerfile` builds `bromelia-daemon` together with MakeMKV:

```bash
docker build -f docker/Dockerfile --build-arg ACCEPT_MAKEMKV_EULA=yes -t bromelia .
```

See [docs/docker.md](docs/docker.md) for drives, folders and the web page token.

## Testing

Unit tests on all three platforms run against the shared fixtures, including the navigation files
(IFOs only, no video) of a TV DVD with a six-episode “play all” title, and recorded `makemkvcon` failures
(full disk, killed process, missing source or title) whose expected outcomes are listed in
[`shared/fixtures/rip-outcomes.json`](shared/fixtures/rip-outcomes.json) and replayed by every platform. Each suite also contains
end-to-end tests that run a real disc image through the complete pipeline when pointed at one:

| Variable | Test |
| --- | --- |
| `BROMELIA_TEST_ISO` | Any disc image: title choice, `mkvmerge` track removal, renaming, post-processing script, manifest |
| `BROMELIA_TEST_DVD_ISO` | `One_Piece_S2_P7_D2.iso`: episode analysis straight from the ISO, menu OCR |
| `BROMELIA_TEST_PLAYALL_ISO` | Same ISO: rip → split into episodes 138–143 → names → `SHA256SUMS` → plugins |
| `BROMELIA_TEST_ONEPASS_ISO` | Same ISO: several titles ripped in one `makemkvcon` run |

The data-disc tests image a random file standing in for a drive; with `BROMELIA_TEST_DVD_ISO` set they copy that
image instead and compare hashes.

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
including the full WinUI 3 build on a Windows runner. It also checks what the unit tests can't:

- **Web page in a browser:** [`shared/web/test/check-web-page.mjs`](shared/web/test/check-web-page.mjs) opens the page
  in Chromium against `bromelia-daemon` and a stand-in `makemkvcon` (drives, rip and cancel, eject, close tray, the
  token, 401 and 403, the archive check, dark mode, phone width) and keeps screenshots.
- **Windows screens:** the app started with `BROMELIA_SNAPSHOT=<folder>` and its own `BROMELIA_DATA_DIR` shows
  every page and tab in the light and dark theme, saves each as a PNG and quits.
- **Docker image:** it is built and checked by [`docker/smoke-test.sh`](docker/smoke-test.sh) (see
  [docs/docker.md](docs/docker.md)).

The screenshots are kept as the run's artifacts.

```bash
# Web page (needs Node; any OS where the daemon builds)
npm install --no-save playwright && npx playwright install chromium
node shared/web/test/check-web-page.mjs linux/build-headless/src/bromelia-daemon screenshots
```

## Repository layout

```
shared/     settings catalog, robot-output fixtures, web page, icon source
macos/      Xcode project: Bromelia (SwiftUI app) and BromeliaTests
windows/    Bromelia.sln: Bromelia.Core (platform-neutral engine), Bromelia.App (WinUI 3), tests
linux/      meson project: core static library, GTK application, bromelia-daemon, tests, desktop data
docker/     Dockerfile for bromelia-daemon with MakeMKV
docs/       configuration format, architecture notes, running headless
```

See [docs/architecture.md](docs/architecture.md) for how the pieces fit together.

## Limitations
- MakeMKV 2.0 removed the streaming server, so Bromelia has no streaming feature.
- A laptop with its lid closed still sleeps (unless it is connected to power and a display), whatever
  Bromelia asks for.
- Firmware flashing isn't offered. Only read-only commands of MakeMKV's firmware tool are exposed.
- On Windows, per-drive MakeMKV settings rely on applying registry values around each launch. If the
  MakeMKV GUI is open at the same time and saves its preferences, it may write values back.
- Individual track selection and episode splitting need `mkvmerge` from MKVToolNix; reading episode
  numbers from menus needs `ffmpeg` and `tesseract`.
- Episode splitting works for DVDs (their menu navigation says where episodes start). Blu-ray TV discs
  normally store each episode as its own playlist, which is ripped and named per episode anyway.
- Without an online lookup the name comes from the disc itself; change it on the disc page when the label is
  cryptic. Episode titles need the show's season numbering to match the disc's: a disc numbered across seasons
  (absolute numbers such as 138) gets titles only when the online season lists those numbers.
- Audio CDs need cyanrip or abcde (not on Windows by default: set an audio CD command); `apprise` URLs need the
  `apprise` command.
- The `e` format codes mean “not decrypted by Bromelia”: a DVD without CSS backed up without decryption
  is still labelled `DVDe`.
