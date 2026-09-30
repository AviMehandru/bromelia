# Configuration format

All Bromelia versions store their configuration as one JSON document with the same schema:

| Platform | Location |
| --- | --- |
| macOS | `~/Library/Application Support/Bromelia/config.json` |
| Windows | `%LOCALAPPDATA%\Bromelia\config.json` |
| Linux | `$XDG_CONFIG_HOME/bromelia/config.json` (usually `~/.config/bromelia/config.json`) |

Job logs, manifests and history live next to it (`jobs/<job id>/`, `history.json`, `archive-checks.json`,
`unfinished-<pid>.json`; on Linux under `$XDG_DATA_HOME/bromelia`). Unknown keys are ignored and missing keys take their defaults, so files move
between versions and platforms freely. Paths may start with `~`.

## Top level

```jsonc
{
  "version": 2,                    // files without a version, or version 1, are upgraded (see below)
  "makemkvconPath": "",            // empty = find automatically
  "mkvmergePath": "",              // empty = find automatically (needed for hand-picked tracks)
  "outputRoot": "~/Movies/Bromelia",
  "pollIntervalSeconds": 10,       // 0 = only rescan on OS media events
  "pollWhileRipping": false,
  "maxConcurrentJobs": 0,          // 0 = no global limit (always one job per drive)
  "registrationKey": "",           // empty = use the key MakeMKV is registered with
  "globalSettings": { "dvd_MinimumTitleLength": "120" },   // MakeMKV settings for every drive
  "defaultDrive": { /* DriveConfig */ },   // drives without their own config, ISO / folders
  "drives": [ /* DriveConfig */ ],
  "presets": [ { "id": "…", "name": "…", "config": { /* DriveConfig */ } } ],
  "plugins": [ /* PostProcessStep */ ],   // steps for every drive, usually with matchName / matchFormats
  "historyLimit": 500,
  "stallTimeoutMinutes": 30,       // stop a rip or backup that prints nothing for this long (stuck drive); 0 = never
  "preventSleep": true,            // keep the computer awake while jobs or background steps run
  "metadata": {                    // online lookup of the movie / show (see "Online lookup")
    "provider": "none",            // none | tmdb | omdb
    "apiKey": "",                  // TMDb: API key (v3) or read access token; OMDb: API key
    "language": "en-US",           // TMDb language of titles
    "episodeTitles": true          // TV shows: look up the titles of the disc's episodes ({episodeTitle})
  },
  "notifications": [               // where to send a message when a job finishes (see "Notifications")
    { "id": "…", "url": "https://discord.com/api/webhooks/…", "enabled": true, "onlyProblems": false }
  ],
  "autoUpdateBetaKey": false,      // register MakeMKV's current beta key at startup and when it expires (never replaces a purchased key)
  "backgroundJobs": 1,             // background post-processing steps that run at the same time
  "archiveCheck": {                // verify the output folder's archives again (see "Archive check")
    "intervalDays": 0              // every N days; 0 = never
  },
  "webUI": {                       // web page (see "Web page")
    "enabled": false,
    "address": "127.0.0.1",        // 127.0.0.1 = this computer only; 0.0.0.0 = the network (needs a token)
    "port": 51280,
    "token": ""                    // required for every request when set
  }
}
```

**Upgrading from version 1.** Version 1 kept MakeMKV's file names and named folders `{disc}`. When a
version 1 file is loaded, drive configurations (and presets) that still have those defaults — an empty
`fileNameTemplate`, or `folderTemplate` = `{disc}` — get the version 2 defaults below. Templates you
changed are kept.

## DriveConfig

```jsonc
{
  "id": "45C805B7-47BE-4674-A759-A6EAEA53521A",
  "name": "Left drive",
  "enabled": true,
  "match": {
    "driveName": "BD-RE HL-DT-ST BD-RE  WH16NS60 1.02 KLAM6E84325", // compared case/space-insensitively
    "devicePath": "/dev/rdisk4"                                    // used when driveName is empty
  },
  "settings": { "io_ErrorRetryCount": "10" },   // overrides of globalSettings (MakeMKV settings.conf keys)
  "profile": {
    "mode": "makemkvDefault",                  // makemkvDefault | generated | customFile
    "customPath": "",
    "generated": {
      "name": "Bromelia",
      "selectionRule": "",                     // empty = MakeMKV's default rule
      "setFirstAudioTrackAsDefault": true,
      "setFirstSubtitleTrackAsDefault": true,
      "setFirstForcedSubtitleTrackAsDefault": true,
      "ignoreForcedSubtitlesFlag": true,
      "useISO639Type2T": false,
      "insertFirstChapter00IfMissing": true,
      "lpcmStereo": "lpcm",                    // copy | lpcm | wavex | flac-best | flac-fast
      "lpcmMultichannel": "flac-best"
    }
  },
  "rip": {
    "mode": "mkv",                             // mkv | backup | backupDecrypted | backupThenMkv | infoOnly
                                               // (audioCD and dataImage are chosen from the disc, see "Other discs")
    "backupFormat": "folder",                  // folder | iso
    "keepBackupAfterMKV": true,
    "titleSelection": {
      "strategy": "all",                       // all | longest | indices | manual
      "longestCount": 1,
      "indexPattern": "",                      // e.g. "0,2-4,7-", "last", "all"
      "indexBase": "makemkv",                  // makemkv (0-based title number) | source (playlist / VTS id)
      "minDurationSeconds": 0, "maxDurationSeconds": 0,
      "minChapters": 0, "maxChapters": 0,
      "minSizeMB": 0, "maxSizeMB": 0,
      "includePattern": "", "excludePattern": "",   // case-insensitive regular expressions
      "skipDuplicates": true,
      "skipAlternateAngles": false,
      "maxTitles": 0
    },
    "minLengthSeconds": 60,                    // optional: --minlength
    "cacheMB": 1024,                           // optional: --cache
    "directIO": true,                          // optional: --directio
    "extraArguments": "",
    "writeDiscInfoJSON": false,
    // Modes by disc format for automatic rips and "Rip" (not for titles chosen by hand); missing = "mode".
    "formatModes": { "dvd": "backupThenMkv", "bluray": "mkv", "uhd": "backupDecrypted" }
  },
  "output": {
    "rootOverride": "",                        // empty = outputRoot
    "folderTemplate": "{name}{discLabel? - {discLabel}}",
    // MKV files and backups; empty = keep MakeMKV's names
    "fileNameTemplate": "{name}{episode? - {episode}}{episodeTitle? - {episodeTitle}}{discLabel? - {discLabel}} - {rip}{track? - {track}} - {format}",
    "backupSubfolder": "backup",
    "conflictPolicy": "uniqueSuffix",          // uniqueSuffix | overwrite | skip
    "layout": "templates"                      // templates | mediaServer (Plex / Jellyfin / Emby, see below)
  },
  "automation": {
    "autoRipOnInsert": false,
    "autoRipDelaySeconds": 10,
    "ejectWhenDone": true,
    "ejectOnFailure": false,
    "notify": true,
    "playSound": true,
    "waitForMountSeconds": 30,                 // before an automatic rip, wait up to this long for the disc to be mounted; 0 = don't
    "alreadyArchived": "skip"                  // a disc archived before, in automatic rips: skip | ask | ripAgain (see "Archive check")
  },
  "archive": {
    "checksums": true,                         // SHA256SUMS in the output folder
    "archiveRecord": true,                     // bromelia.json and bromelia-log.txt in the output folder
    "verifyRips": true                         // check each MKV against the disc listing (mkvmerge) and each backup's structure
  },
  "episodes": {
    "splitPlayAll": true,                      // split DVD "play all" titles of TV shows into episodes
    "keepPlayAll": true,                       // keep the unsplit title too
    "readMenuNumbers": true                    // read episode numbers from the menus (ffmpeg + tesseract)
  },
  "other": {                                   // discs without a DVD / Blu-ray structure (see "Other discs")
    "ripAudioCDs": true,
    "imageDataDiscs": true,
    "audioCommand": ""                         // empty = cyanrip, else abcde; {device} is the drive
  },
  "postProcess": [ /* PostProcessStep */ ]
}
```

### Output folder, read errors and checks

A job writes into a hidden staging folder inside its output folder (`.bromelia-incomplete-<job>`), and
files are moved into the output folder only when the job succeeded. Otherwise nothing that looks finished
is left behind:

| Outcome | Where the files go |
| --- | --- |
| Succeeded | The output folder, with `SHA256SUMS`, `bromelia.json` and `bromelia-log.txt` |
| Completed with read errors: every title was saved, but MakeMKV reported errors while reading the disc (e.g. `MEDIUM ERROR`, hash check failures) | `<folder> [READ ERRORS]`, with checksums, the archive record (`"status": "errors"`, `readErrors`) and a `READ ERRORS.txt` note |
| Failed or cancelled | `<folder> [INCOMPLETE]`, with an `INCOMPLETE.txt` note. Titles that didn't finish keep MakeMKV's own file name |

The folder is renamed only when it was created for this job; in a shared folder (an empty folder template,
or `conflictPolicy: overwrite` on a folder that has content) a subfolder such as `INCOMPLETE - 1a2b3c4d` is
used. A job that saved nothing leaves no folder. Errors while reading the disc listing don't count as read
errors, only errors during the rip or backup itself.

With `verifyRips`, every MKV is checked with `mkvmerge -J` before it is renamed: its length must match the
title in the disc listing (within 5 s or 0.5 %, whichever is larger) and it must contain tracks (and video,
when the title has video). A file that fails the check fails the job. Chapter and track count differences
are logged as warnings. Backups must contain `BDMV/index.bdmv`, `VIDEO_TS/VIDEO_TS.IFO` or `HVDVD_TS`, and
ISO backups must be image files with an ISO 9660 / UDF volume descriptor. Without mkvmerge, MKV files are
not checked (the log says so).

Every job reads the disc listing again. When titles or tracks were chosen on a disc opened earlier, the
new listing must be the same disc (same volume name, and at least one title in common), and every chosen
title is found again by its source title (playlist / VTS title), length and segment map, even when its
number changed (for instance because the minimum title length changed). Titles with hand-picked tracks
must still have the same track list. Otherwise the job fails before ripping. The same matching is used
to find the chosen titles in the backup in *backup then MKV* mode.

Before ripping MKV files, the job checks that the destination has room for the chosen titles (their sizes
in the listing, plus a copy of titles with hand-picked tracks and of a "play all" title that will be split,
plus 2 % or 256 MB). When MakeMKV itself warns that the output may not fit (message 5038, printed before
it starts writing), the job stops before anything is written. A rip that prints nothing for `stallTimeoutMinutes` is stopped and
fails; a process that can't be stopped even with SIGKILL (a hung drive) is left behind after 30 s so the
job can end.

MakeMKV writes an ISO image when a backup's destination ends in `.iso`. If it writes a folder instead
(which some versions or discs may do), the folder is checked like a folder backup and kept, without the
`.iso` extension, and the log says so.

**MakeMKV messages shown outside the log.** “Using LibreDrive mode (…)” while opening a disc is shown on the
disc page and recorded as `libreDrive` in `bromelia.json`; “LibreDrive compatible drive is required” is shown
there too. An expired evaluation / beta key (messages 5052, 5055), an evaluation that hasn't been started, and
“This application version is too old” show a banner with *Get the Current Beta Key* (which downloads the key
from MakeMKV's forum and runs `makemkvcon reg`; on Linux it needs `curl`), and a job that fails because of
one of them says so in its error.

**Archive everything.** The drive editor's Presets menu has *Archive everything*: *backup then MKV* with a
decrypted folder backup kept (every file and title of the disc, including menus and titles shorter than
the minimum title length), MKV files of every title with every track (selection rule `+sel:all`),
`disc-info.json`, and all checks on. With MakeMKV's default selection rule, tracks it doesn't select
(other languages when a preferred language is set, the separate core of lossless audio, 3D video) are not
in the MKV files; the log says how many of a title's tracks each file has.

Title rules are applied in this order: filters (duration, chapters, size, patterns, angles), then
duplicate removal (same segment map, length and angle), then the strategy, then `maxTitles`.
Include/exclude patterns are matched against a string made of the title name, comment, output file
name, source file name (e.g. `00800.mpls`), segment map, `#<source id>` and duration.

## PostProcessStep

```jsonc
{
  "id": "…",
  "name": "Encode",
  "enabled": true,
  "executable": "/usr/bin/HandBrakeCLI",
  "interpreter": "",                           // e.g. /usr/bin/python3; empty = run directly
  "arguments": "-i {file} -o \"{outputDir}/{stem}.mp4\"",
  "workingDirectory": "",                      // empty = job output folder
  "runOn": "success",                          // success | failure (also: cancelled, read errors) | always
  "perFile": true,
  "timeoutSeconds": 0,
  "environment": { "PRESET": "{disc}" },
  "failJobOnError": false,
  "matchName": "",                             // regular expression on the name or disc label; "" = every disc
  "matchFormats": [],                          // e.g. ["DVD", "DVDe"] or ["BR*"]; [] = every format
  "background": false                          // run after the job, in the background queue (see below)
}
```

**Background steps** (`"background": true`) run after the job has finished and the disc is out, in a queue
of their own (`backgroundJobs` at a time), so a slow encode or upload doesn't hold up the next disc. They
get the same tokens and variables, can't change the job's result, and write their output to
`background.txt` in the job's log folder. The queue is shown under *Queue* (and on the web page).

`matchName` is matched case-insensitively against the movie / show name used for file names *and*
against the disc label, so `^One Piece$` targets a show and `ONE_PIECE_S2_P7_D2` (or `S2_P7`) one
particular disc. `matchFormats` lists format codes; a trailing `*` matches every code with that prefix
(`BR*` = `BR` and `BRe`). Both conditions must hold. Steps in a drive's `postProcess` run first, then the
top-level `plugins`.

Arguments are split like a POSIX shell command line (quotes and backslashes, no expansion) **before**
tokens are filled in, so a value containing spaces stays one argument. A lone `{files}` argument
expands to one argument per produced file.

Environment variables available to every step:

| Variable | Meaning |
| --- | --- |
| `BROMELIA_JOB_ID` | Job identifier |
| `BROMELIA_STATUS` | `success`, `errors` (completed with read errors), `failed` or `cancelled` |
| `BROMELIA_MODE` | Rip mode (`mkv`, `backup`, …) |
| `BROMELIA_DRIVE_NAME`, `BROMELIA_DRIVE_ID` | Drive configuration |
| `BROMELIA_DEVICE` | OS device of the drive |
| `BROMELIA_DISC_NAME`, `BROMELIA_DISC_TYPE` | Disc label, `dvd` / `bd` / `hddvd` / `disc` |
| `BROMELIA_OUTPUT_DIR` | Output folder |
| `BROMELIA_FILES`, `BROMELIA_FILE_COUNT` | Produced files (newline separated) and count |
| `BROMELIA_FILE` | Current file (per-file steps) |
| `BROMELIA_MANIFEST` | Path of the job manifest JSON (status, titles, files, times, error) |
| `BROMELIA_LOG` | Job log |
| `BROMELIA_SOURCE` | makemkvcon source (`dev:…`, `iso:…`, `file:…`) |
| `BROMELIA_ERROR` | Error message, when the job failed |
| `BROMELIA_NAME` | Movie or show name |
| `BROMELIA_KIND` | `movie` or `tv` |
| `BROMELIA_FORMAT` | Format code: `DVD`, `BR`, `4K`, `DVDe`, `BRe`, `4Ke` |
| `BROMELIA_ENCRYPTED` | `1` for backups that were not decrypted, else `0` |
| `BROMELIA_SEASON`, `BROMELIA_DISC_NUMBER`, `BROMELIA_DISC_SET` | From the disc label (`2`, `2`, `Season 2 Part 7 Disc 2`) |
| `BROMELIA_CHECKSUMS` | Path of `SHA256SUMS`, when written |

The manifest (`BROMELIA_MANIFEST`) also contains `name`, `kind`, `format`, `formatCode`, `encrypted`,
`season`, `discNumber`, `episodes` (file, episode number, source title, chapter range) and `checksums`
(path, size, sha256 of every file).

## Templates

| Token | Value |
| --- | --- |
| `{name}` | Movie or show name: typed on the disc page, else the Blu-ray metadata title, else the cleaned-up volume label |
| `{discLabel}` | Place in the set from the label, e.g. `Season 2 Part 7 Disc 2`; empty when the label has none |
| `{season}`, `{part}`, `{volumeNumber}`, `{discNumber}` | The numbers alone |
| `{rip}` | `Rip` for MKV files, `Backup` for backups |
| `{format}` | `DVD`, `BR`, `4K` (`HDDVD`, `DISC`); with an `e` suffix for backups that are not decrypted |
| `{kind}` | `movie` or `tv` |
| `{episode}`, `{episodeNumber}` | `Episode 138` / `138` for TV episodes, empty for other titles (files only) |
| `{episodeTitle}` | The episode's title from the online lookup, e.g. `The One with the Breast Milk`; empty without one (files only) |
| `{track}` | `Title 11` (DVD title), `Playlist 00800` (Blu-ray), `Title 11 Ch 8-14` for split episodes (files only) |
| `{disc}`, `{volume}` | Disc name / volume label |
| `{type}` | `dvd`, `bd`, `hddvd`, `disc` |
| `{drive}` | Drive configuration name |
| `{date}`, `{time}`, `{year}`, `{month}`, `{day}`, `{job}` | Date/time of the job, short job id |
| `{title}`, `{index}`, `{n}`, `{source}` | Title name, MakeMKV title number, position in the job, source title id |
| `{duration}`, `{chapters}`, `{original}`, `{comment}` | `h-mm-ss`, chapter count, MakeMKV's file name, title comment |
| `{releaseYear}`, `{tmdb}`, `{imdb}` | Year and ids found by the online lookup (empty without one) |
| `{libraryFolder}`, `{seasonOr1}` | `Movies` / `TV Shows`; the season, or 1 (media server names) |
| `{outputDir}`, `{file}`, `{filename}`, `{stem}`, `{files}`, `{status}`, `{manifest}`, `{device}`, `{checksums}` | Scripts only |

`{n:3}` zero-pads to three digits; `{token?text}` inserts `text` (which may contain tokens) only
when `token` is not empty. Unknown tokens are left as they are. In folder and file templates, `/`
separates folders and values are made safe for file names on every platform.

The file name template also names backups: with `{rip}` = `Backup` and no episode or track, an ISO
becomes `One Piece - Season 2 Part 7 Disc 2 - Backup - DVDe.iso` and a folder backup a folder of that
name containing `VIDEO_TS` / `BDMV`.

## Naming, formats and episodes

- **Name.** Labels are split at `_`, `.` and spaces; everything from the first set marker on (`S2`,
  `SEASON 2`, `P7`, `PART 7`, `V3`, `VOL 3`, `D2`, `DISC 2`, `S1D2`, `EP`) is left out, medium words (`WS`,
  `NTSC`, `BLURAY`, `UHD`, …) are dropped and all-caps labels are title-cased (`THE_LORD_OF_THE_RINGS`
  → *The Lord of the Rings*, Roman numerals stay upper case).
- **Movie or TV.** TV when the label has season / episode / volume markers, when a DVD menu plays three or
  more episode-length parts of one title, or when three or more titles are 10–75 minutes long and within
  35 % of each other. Otherwise a movie. The disc page can override it.
- **Format.** From the disc listing: Blu-ray whose video is 2160p or HEVC = `4K`; otherwise the disc
  type. Without a listing: the drive's file system flags, and for folder backups `BDMV/index.bdmv`
  (`INDX0300` = UHD).
- **Episodes.** For TV DVDs the disc's navigation is read (from the ISO or backup being ripped, or from
  the mounted disc): the title tables in the IFO files and the jump commands in menus, highlight buttons
  and title pre-commands. A title that the menus enter at several chapters is a “play all” title; each
  entry chapter starts an episode, and the last episode ends where the disc moves on to another VOB (or
  after as many chapters as the previous one). The split is only done when the parts look like episodes
  (at least 5 minutes, at most twice as long as each other) and the ripped file matches the disc title
  (chapter count, duration within 2 s, chapter times via `mkvextract` when available). A short clip after
  the last episode becomes its own file. Episode numbers come from the disc page, else from the
  episode menus (`EPISODE 138` read by `tesseract`), else from the previous disc of the set (below), else
  start at 1. TV discs whose episodes are separate titles are numbered in title order.
- **Numbering across discs.** Disc 2 and later of a set continue after the last episode of the previous disc: the
  `bromelia*.json` records in the history's folders and in the output root (up to four folders deep) are searched
  for a TV show with the same name (after the online lookup) or the same title in its label, the same season, part
  and volume, and the disc number one lower; records of rips that failed don't count, rips with read errors do. In
  the media server layout, when no such record exists and no later disc of the set was archived, the highest
  `SxxEyy` of the season in the show's `Season NN` folder is used. When the menus or the disc page give another
  first number, the log warns about the gap or overlap. `shared/fixtures/episode-continuation.json` holds the cases
  every platform's tests check.

## Archive files

Every output folder gets, unless switched off in `archive`:

| File | Content |
| --- | --- |
| `SHA256SUMS` | `sha256  relative/path` for every produced file, including all files of backup folders. Entries of earlier jobs writing into the same folder are kept. |
| `bromelia.json` | `{"format": "bromelia-archive", "version": 2, …}`: status (`success` / `errors`), name, kind, disc (label, volume name, type, format, format code, encrypted, season / part / volume / disc, fingerprint), rip, mode, source, drive, MakeMKV version, job id, times, titles (source id, source file, duration, chapters, size, segment map), episodes (file, episode number, source title, chapter range, title), files (path, size, sha256), warning and error counts and error messages. A folder shared by several jobs gets `bromelia (2).json` and so on |
| `bromelia-log.txt` | The job log |

## Archive check

**Verifying archives.** *Verify Archive…* (the Archive Check page), *Verify Folder* in the history, `POST /api/verify`
and `archiveCheck.intervalDays` look for every folder with a `SHA256SUMS` under the chosen folder (hidden folders, such
as staging folders, are skipped), read every listed file again and compare its SHA-256. Each folder is reported as
OK or damaged, with the files that are **changed**, **unreadable** or **missing**, and files the folder has that
`SHA256SUMS` doesn't list (**not listed**; that alone doesn't make a folder damaged: post-processing may add files).
Bromelia's own files (`SHA256SUMS`, `bromelia*.json`, `bromelia-log*.txt`, the `INCOMPLETE.txt` / `READ ERRORS.txt`
notes) are not reported. When each folder was last verified is kept in `archive-checks.json` next to `history.json`
(`{"<folder>": {"checkedAt", "ok", "summary"}}`); the history shows it. With `archiveCheck.intervalDays`, the apps and
`bromelia-daemon` check the whole output folder when its last check is that old (looking a minute after starting,
then every hour, and only while no job runs) and send the result to `notifications` (status `success`, or `failed`
when a folder is damaged, which `onlyProblems` targets get); a damaged folder is also shown in the app. The computer
is kept awake while a check runs (`preventSleep`).

**Discs archived before.** Every job that reads a disc listing computes the disc's fingerprint: `v1:` and the first
32 hex digits of the SHA-256 of

```
bromelia-disc-fingerprint 1
volume:<volume name (CINFO 32), else the disc name>
titles:<title count>
<source title id (TINFO 24, -1 if none)>|<length in seconds>|<segment map (TINFO 26)>|<size in bytes (TINFO 11)>
…one line per title, sorted by byte value
```

so a disc is recognised whatever numbers MakeMKV gives its titles, and another disc of a set with the same label is
not taken for it. The fingerprint is stored in `bromelia.json` (`disc.fingerprint`) and in the history. Before an
MKV rip or a backup, the job looks for it among the history's successful jobs whose folder still exists, then in the
`bromelia*.json` files with status `success` in the output root and up to four folders below it. When found:

| `automation.alreadyArchived` | Automatic rip |
| --- | --- |
| `skip` (default) | stops as *cancelled* (“Already archived in …”), without post-processing; the disc is ejected when `ejectWhenDone` is on |
| `ask` | stops the same way but leaves the disc in the drive; rip it again from the drive page |
| `ripAgain` | rips it as usual (the log says it was archived before) |

A rip started by hand is never stopped: the disc page says the disc was archived before and asks before ripping it
again, and the job only logs a warning. `shared/fixtures/fingerprints.json` holds fingerprints that every platform's
tests check.

## Export bundle

*Export drives and presets* writes:

```json
{ "format": "bromelia-drives", "version": 1, "drives": [ … ], "presets": [ … ] }
```

## Media server layout

With `"layout": "mediaServer"` the templates are replaced by the names Plex, Jellyfin and Emby expect:

```
Movies/Inception (2010)/Inception (2010).mkv                    the main feature (longest title)
Movies/Inception (2010)/Other/Inception (2010) - Playlist 00800.mkv
Movies/Inception (2010)/Backup/Inception (2010) - Backup - BR   backups (.plexignore / .ignore keep servers out)
TV Shows/Friends (1994)/Season 02/Friends (1994) - S02E02 - The One with the Breast Milk.mkv
```

A show or movie keeps one folder: later discs are added to it (`Season 02` gets the next disc's episodes)
instead of creating numbered folders. The year comes from the online lookup; without one the names have no
year. Jobs that fail or have read errors are kept outside the library in `<output root>/<name> [INCOMPLETE …]`.

## Other discs

Automatic rips and *Rip* look at what is in the drive. DVDs and Blu-rays use the drive's mode (or
`formatModes`). Other discs:

- **Audio CDs** (`ripAudioCDs`) are ripped with `audioCommand`, run in the output folder with `{device}` set
  to the drive. Empty uses `cyanrip -d <device> -o flac`, else `abcde -d <device> -o flac -N`; both look the
  album up in MusicBrainz.
- **Data discs** (`imageDataDiscs`) are copied to `<name> - Backup - DISC.iso`: byte for byte on Linux and
  Windows, with `hdiutil` (2048-byte sectors) on macOS. A read error fails the job; with `verifyRips` the
  image must contain an ISO 9660 or UDF file system.

When an option is off, the disc is left alone (and *Rip* says why). How discs are recognised: macOS
mounts audio CDs as `cddafs`; Linux asks udev (`ID_CDROM_MEDIA_TRACK_COUNT_AUDIO`); Windows reads the disc's table of
contents (any audio track makes an audio CD).
A mounted disc with `BDMV`, `VIDEO_TS` or `HVDVD_TS` always counts as a video disc.

## Online lookup

With `metadata.provider` set to `tmdb` (The Movie Database; free API key at themoviedb.org → Settings → API,
v3 key or read access token) or `omdb` (omdbapi.com), every job looks up the name read from the disc and uses
the canonical title for `{name}`, plus `{releaseYear}`, `{tmdb}` and `{imdb}`. A failed lookup is logged and the
job continues with the disc's own name. Requests use `curl` on Linux; the key never appears on a command line.

**Choosing the match.** The search results are ranked: a title equal to the disc's name (ignoring case and
punctuation) scores 4, the release year 2 (1 when it is off by one); ties keep the provider's order. The year comes
from the disc page, or from a name typed as `Inception (2010)`; a year that finds nothing is dropped. The job log
names the match and up to four runners-up. When a disc is opened, the disc page looks it up too and lists the
results: pick another one, or type an id — a TMDb id (`603`, `tmdb:603`, `movie/603`, `tv/1668`, or a
themoviedb.org address) or an IMDb id (`tt0133093`, or an imdb.com address). A TMDb id without `movie/` or `tv/`
uses the disc's kind. OMDb only takes IMDb ids. A chosen id replaces the search; it also decides movie or TV
unless the disc page does. When the disc menus turn a disc taken for a movie into a TV show, the job searches again
as a TV show.

**Episode titles.** With `episodeTitles` (on by default), a TV disc's episodes get their titles from the show's
season (the season from the label, else 1): TMDb `/tv/{id}/season/{n}`, OMDb `?i={imdbId}&Season={n}`. They fill
`{episodeTitle}`, the media server names and the archive record. When the season doesn't list every episode number
of the disc (discs numbered across seasons, say), no episode gets a title and the log says why.

The recorded answers in `shared/fixtures/lookup` and what must come out of them (`expected.json`) are checked by
the tests of all three platforms.

## Notifications

Every entry in `notifications` gets a message when a job finishes (with `onlyProblems`, only when it
didn't succeed):

| URL | Sent as |
| --- | --- |
| `https://discord.com/api/webhooks/…` | Discord webhook (`content`) |
| `https://hooks.slack.com/services/…` | Slack webhook (`text`) |
| `ntfy://topic`, `ntfy://host/topic`, `ntfys://host/topic`, `https://ntfy.sh/topic` | ntfy (title, tags) |
| any other `http(s)://` URL | JSON `{"app", "title", "body", "status"}` |
| anything else (`tgram://…`, `pover://…`, `mailto://…`) | the [`apprise`](https://github.com/caronc/apprise) command, when installed |

`status` is `success`, `errors`, `failed` or `cancelled`. Failures are written to the job log.

## Web page

With `webUI.enabled` Bromelia serves a page at `http://<address>:<port>/` showing drives, jobs,
background steps, recent history and the last archive check, with *Rip*, *Eject*, *Close tray*, *Cancel* and
*Check output folder*. The same data is
available as JSON:

| Request | Result |
| --- | --- |
| `GET /api/status` | `{app, makemkv, problem, drives[{lane, name, device, state, hasDisc, disc, busy}], jobs[{id, title, state, stateLabel, phase, progress, error, outputDirectory, lane}], background[{id, title, state}], history[{title, state, stateLabel, error, outputDirectory, finishedAt}], verify{running, path, progress, folder, stopped, finishedAt, folders, damaged[{folder, summary}]}}` |
| `POST /api/drives/<lane>/rip` · `eject` · `close` | 204, or 400 with the reason |
| `POST /api/jobs/<id>/cancel` | 204, or 400 (`No such job`) |
| `POST /api/verify` · `/api/verify/cancel` | Verify the output folder's archives (see "Archive check") · stop; 204, or 400 with the reason |

Access rules: with a `token`, every request needs `Authorization: Bearer <token>` or `?token=<token>`
(the page passes it on). Without one, only pages on this computer are served (the `Host` header must be
`localhost`, `127.0.0.1` or `[::1]`, which also stops DNS rebinding), and an `address` other than
`127.0.0.1` / `localhost` / `::1` is refused. `POST` requests need the header `X-Bromelia: 1`, which other
web sites can't send. There is no TLS: put a reverse proxy in front of it for access over the internet.
