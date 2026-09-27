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
  "version": 1,
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
  "historyLimit": 500
}
```

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
    "folderTemplate": "{disc}",
    "fileNameTemplate": "",                    // empty = keep MakeMKV's names
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
  "failJobOnError": false
}
```

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

## Templates

| Token | Value |
| --- | --- |
| `{disc}`, `{volume}` | Disc name / volume label |
| `{type}` | `dvd`, `bd`, `hddvd`, `disc` |
| `{drive}` | Drive configuration name |
| `{date}`, `{time}`, `{year}`, `{month}`, `{day}`, `{job}` | Date/time of the job, short job id |
| `{title}`, `{index}`, `{n}`, `{source}` | Title name, MakeMKV title number, position in the job, source title id |
| `{duration}`, `{chapters}`, `{original}`, `{comment}` | `h-mm-ss`, chapter count, MakeMKV's file name, title comment |
| `{outputDir}`, `{file}`, `{filename}`, `{stem}`, `{files}`, `{status}`, `{manifest}`, `{device}` | Scripts only |

`{n:3}` zero-pads to three digits; `{token?text}` inserts `text` (which may contain tokens) only
when `token` is not empty. Unknown tokens are left as they are. In folder and file templates, `/`
separates folders and values are made safe for file names on every platform.

## Export bundle

*Export drives and presets* writes:

```json
{ "format": "bromelia-drives", "version": 1, "drives": [ … ], "presets": [ … ] }
```
