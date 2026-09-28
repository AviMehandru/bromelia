# Configuration format

All Bromelia versions store their configuration as one JSON document with the same schema:

| Platform | Location |
| --- | --- |
| macOS | `~/Library/Application Support/Bromelia/config.json` |
| Windows | `%LOCALAPPDATA%\Bromelia\config.json` |
| Linux | `$XDG_CONFIG_HOME/bromelia/config.json` (usually `~/.config/bromelia/config.json`) |

Job logs, manifests and history live next to it (`jobs/<job id>/`, `history.json`; on Linux under
`$XDG_DATA_HOME/bromelia`). Unknown keys are ignored and missing keys take their defaults, so files move
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
  "historyLimit": 500
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
    "writeDiscInfoJSON": false
  },
  "output": {
    "rootOverride": "",                        // empty = outputRoot
    "folderTemplate": "{name}{discLabel? - {discLabel}}",
    // MKV files and backups; empty = keep MakeMKV's names
    "fileNameTemplate": "{name}{episode? - {episode}}{discLabel? - {discLabel}} - {rip}{track? - {track}} - {format}",
    "backupSubfolder": "backup",
    "conflictPolicy": "uniqueSuffix"           // uniqueSuffix | overwrite | skip
  },
  "automation": {
    "autoRipOnInsert": false,
    "autoRipDelaySeconds": 10,
    "ejectWhenDone": true,
    "ejectOnFailure": false,
    "notify": true,
    "playSound": true
  },
  "archive": {
    "checksums": true,                         // SHA256SUMS in the output folder
    "archiveRecord": true                      // bromelia.json and bromelia-log.txt in the output folder
  },
  "episodes": {
    "splitPlayAll": true,                      // split DVD "play all" titles of TV shows into episodes
    "keepPlayAll": true,                       // keep the unsplit title too
    "readMenuNumbers": true                    // read episode numbers from the menus (ffmpeg + tesseract)
  },
  "postProcess": [ /* PostProcessStep */ ]
}
```

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
  "runOn": "success",                          // success | failure | always
  "perFile": true,
  "timeoutSeconds": 0,
  "environment": { "PRESET": "{disc}" },
  "failJobOnError": false,
  "matchName": "",                             // regular expression on the name or disc label; "" = every disc
  "matchFormats": []                           // e.g. ["DVD", "DVDe"] or ["BR*"]; [] = every format
}
```

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
| `BROMELIA_STATUS` | `success`, `failed` or `cancelled` |
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
| `{track}` | `Title 11` (DVD title), `Playlist 00800` (Blu-ray), `Title 11 Ch 8-14` for split episodes (files only) |
| `{disc}`, `{volume}` | Disc name / volume label |
| `{type}` | `dvd`, `bd`, `hddvd`, `disc` |
| `{drive}` | Drive configuration name |
| `{date}`, `{time}`, `{year}`, `{month}`, `{day}`, `{job}` | Date/time of the job, short job id |
| `{title}`, `{index}`, `{n}`, `{source}` | Title name, MakeMKV title number, position in the job, source title id |
| `{duration}`, `{chapters}`, `{original}`, `{comment}` | `h-mm-ss`, chapter count, MakeMKV's file name, title comment |
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
  episode menus (`EPISODE 138` read by `tesseract`), else start at 1. TV discs whose episodes are separate
  titles are numbered in title order.

## Archive files

Every output folder gets, unless switched off in `archive`:

| File | Content |
| --- | --- |
| `SHA256SUMS` | `sha256  relative/path` for every produced file, including all files of backup folders. Entries of earlier jobs writing into the same folder are kept. |
| `bromelia.json` | `{"format": "bromelia-archive", "version": 1, …}`: name, kind, disc (label, volume name, type, format, format code, encrypted, season / part / volume / disc), rip, mode, source, drive, MakeMKV version, job id, times, titles (source id, source file, duration, chapters, size, segment map), episodes, files (path, size, sha256), warning and error counts and error messages |
| `bromelia-log.txt` | The job log |

## Export bundle

*Export drives and presets* writes:

```json
{ "format": "bromelia-drives", "version": 1, "drives": [ … ], "presets": [ … ] }
```
