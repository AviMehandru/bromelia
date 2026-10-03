# Shared scenarios

End-to-end behaviour, written once and run by every platform (rearchitecture plan §26). Each platform has a
scenario runner that builds the engine with `TestCompositionRoot` (fake clock, fake drives, scripted processes,
a temporary folder, an in-memory database), plays the scenario's steps against `LocalEngine`, and checks the
expectations. In CI the same scenarios also run end to end: `RemoteEngine` → `Server` → `LocalEngine`.

Pure functions are not tested here. They have golden fixtures (`shared/fixtures/domain/*.cases.json`), which
run faster and pin exact outputs. A scenario is for behaviour that needs the pipeline, the scheduler, the
filesystem, processes, the database or the API.

`COVERAGE.md` (generated from `coverage.json`) maps every test of today's three suites, and every behaviour that
`docs/*.md` describes, to the fixture or scenario that now covers it. `tools/check-contracts.py` checks that
nothing is missing.

| Folder | What |
| --- | --- |
| `*.yaml` | Scenarios. The file name (without `.yaml`) is the scenario's id. |
| `discs/` | Disc definitions: what the fake drive or ISO contains. |
| `tools/` | Scripts for scripted tools (`makemkvcon`, `mkvmerge`, `HandBrakeCLI`, the audio ripper, …). |
| `tools/lines.yaml` | Named robot-mode lines that scripts print (`saved`, `readError`, …). |
| `../schema/scenario.json` | The format, as JSON Schema; the checker validates every file against it. |
| `coverage.json` | Today's tests and documented behaviours → fixture or scenario. |

Scenarios are YAML 1.2 and use only what JSON can express (no anchors, tags or merge keys), so a runner may
convert them to JSON with any YAML library or with `tools/check-contracts.py --export-scenarios`.

## A scenario

```yaml
title: Read errors keep the files apart
from: [docs/configuration.md#output-folder-read-errors-and-checks]
given:
  disc: sample-movie            # discs/sample-movie.yaml, in the ISO the job reads
  tools:
    makemkvcon: tools/makemkvcon-read-error.yaml
steps:
  - rip: {iso: true, titles: [0], as: job}
  - expect:
      job: {name: job, outcome: succeededWithReadErrors, error: {code: rip.readErrors, params: {count: 1}}}
      files:
        include: ["library/_Quarantine/Sample Movie [READ ERRORS]/READ ERRORS.txt"]
```

### The world a scenario starts in

Unless `given` says otherwise:

- **Folders.** The runner makes an empty temporary folder, the scenario *root*. Paths in a scenario are relative
  to it and use `/`. `library/` holds a library with id `lib` and its marker `.bromelia-library`. The engine's
  data folder is `data/` (so job folders are `data/jobs/<job id>/`).
- **Configuration.** `shared/fixtures/config/config3-scenario-base.json`: the defaults, the library `lib` at
  `{root}/library` (`{root}` in configuration strings is the scenario root), no drive polling, desktop
  notifications off, no eject, no mount wait. Tool paths are empty: the test root's ToolLocator finds exactly the
  tools that have scripts.
  `given.config` names another file in `shared/fixtures/config/`; `given.configPatch` is an RFC 7386 merge
  patch over it; `given.profile` is a merge patch over the default profile (the common case).
- **Disc.** `given.disc` is what `iso: true` sources and the first fake drive contain: a name in `discs/`, or a
  disc definition written inline.
- **Clock.** `2026-10-02T20:00:00.000Z`. It moves on `advance` steps, and in an `expect` step without `when` while
  every unsettled job is waiting only on the clock (a countdown, a mount wait, a stall timeout): the runner then
  moves it to the next timer. An `expect` with `when` never moves it.
- **Drives.** None. `given.drives` adds fake drives: `{id, name, identification, device, disc}`. `id` is the
  scenario's name for the drive (`left`); the engine's DriveId is computed from `identification` as usual.
- **Tools.** Every external program is scripted: launching one runs its script from `given.tools` (or the
  default below) instead of a process. A tool with no script is *not installed* (ToolLocator finds nothing).
  Defaults: `makemkvcon` → `tools/makemkvcon-saves.yaml`; no `mkvmerge`, `ffmpeg`, `tesseract`, `HandBrakeCLI`,
  `cyanrip`, `abcde`, `apprise`.
- **Network.** HTTP requests are answered from `given.http` (`{method, url}` → recorded response file in
  `shared/fixtures/lookup/` or an inline body); anything else fails with `http.failed`.
- **Catalogue.** Empty, unless `given.units` adds archive units. Each one is a folder (relative to the root) with
  its `files` (each holding `contents of <name>, long enough to hash`), a `SHA256SUMS` listing them, a record
  (`record: 2` writes today's `bromelia.json`, `3` or nothing a `bromelia-<unit8>.json`) carrying `name`, `kind`,
  `label`, `season`, `part`, `volume`, `disc`, `episodes`, `status` and `archivedAt`, and, unless
  `inCatalogue: false`, the database rows a commit would have made. `fingerprintOf` gives it the fingerprint of a
  disc (a name in `discs/` or an inline disc).
- **Libraries.** `given.libraries` replaces the default library: `{id, path, marker}` (`marker: false` = a folder
  without `.bromelia-library`, so the library is offline).
- **Free space.** `given.freeBytes` is what the fake FileSystem reports for every volume (default: 1 TB).
- **Platform.** `given.platform` makes the platform-specific fakes behave like that OS (`linux`: logind is
  unreachable unless the test root provides a bus); default: the platform running the scenario.

### Steps

Steps run in order. An action step does one thing; an `expect` step checks the state *after every job started
so far has settled* (finished, blocked, or awaiting a decision), unless it names a `when`.

| Action | Meaning |
| --- | --- |
| `rip: {iso: true \| drive: <id> \| folder: <path> \| session: <name>, titles: [..], mode, automatic, as}` | Start a job: `iso: true` = an ISO image of `given.disc` (source `iso:<root>/test.iso`); `drive` = `EngineAPI.rip`; `session` = `ripSession`. `titles` are the manual choice (MakeMKV title numbers). `as` names the job for later steps. |
| `open: {iso: true \| drive \| folder \| file, as}` | `openSession`; `as` names the session. |
| `choose: {session, titles, tracks: {<title>: [stream, …]}, names: {<title>: name}, name, kind, year, onlineId, firstEpisode}` | `patchSession` with the session's current rev. |
| `insert: {drive, disc}` / `remove: {drive}` / `openTray: {drive}` | The fake drive reports a medium arriving / leaving / the tray opening (DeviceMonitor events). |
| `cancel: <job>` / `retry: {job, drive, fromStep}` / `decide: {job, answer}` / `move: {job, by}` / `startNow: <job>` | The job commands. |
| `verify: {library \| path \| unit, as}` | `EngineAPI.verify`. |
| `advance: <seconds>` | Move the fake clock; due timers fire in order. |
| `kill: {at: <crash point>}` then `restart: {}` | Stop the engine graph abruptly at a named crash point (see below) and build a new one on the same root, database and data folder. |
| `swapDisc: <disc>` | The ISO (or folder) now holds another disc: a disc opened earlier is no longer the one a job reads. |
| `mounted: {drive, after}` | The OS mounts the drive's disc (after that many seconds of the fake clock). |
| `cancelCountdown: {drive}` | Stop an automatic rip's countdown. |
| `testStep: {step, unit, as}` / `testNotification: {target}` | `EngineAPI.testStep` / `testNotification`. |
| `unmount: <library>` / `mount: <library>` | The library's folder goes away / comes back (`unmount` moves it aside, so the marker is gone). |
| `write: {path, text \| bytes}` / `flip: {path, byte}` / `delete: <path>` | Change files (fault injection). `flip` XORs one byte with 1. |
| `config: <merge patch>` | `updateConfig` with the patch applied to the current document. |
| `tools: {<tool>: <script>}` | Change a tool's script from now on. |
| `api: {method, path, body, headers, transport: socket \| tcp, as}` | A raw request to the Server (end-to-end and API contract scenarios). |
| `wait: {job, state \| outcome \| step}` | Run until the job gets there (an `expect` with `when` does the same). |

**Crash points** are named places where the test root can stop the engine: `beforeStep:<kind>`, `inStep:<kind>` (once the
step's first process has started), `afterStep:<kind>` (after its result is stored),
`commit.afterIntent`, `commit.afterMove:<n>`, `commit.beforeDone`, `seal.afterHash`. Every platform defines all of
them; a `kill` at a point that is never reached fails the scenario.

### Expectations

Every key is optional; all given keys must hold. Lists of files are matched after sorting. `json` subsets match
recursively: objects by the keys given, lists element by element (same length).

**Placeholders** in steps, expected paths and texts: `{root}` the scenario root; `{unit8}` and `{jobId}` the unit
short id and job id of the job named in the same `expect` (or of the only job); `{unit8:<name>}` / `{jobId:<name>}`
of another job, where the name is a job's `as` or an `api` step's `as` whose response was a Job;
`{driveId:<drive>}` the DriveId of a fake drive; `{sessionId:<session>}` an open session's id.

`file.text` may be `<<shared/fixtures/…>>`: the file must equal that fixture byte for byte.

| Key | Checks |
| --- | --- |
| `job: {name, state, outcome, error: {code, params}, errorText, noError, blockedBy, decision, mode, kind, titles, files, unit: {state, status, attempts}, counts, steps: [kinds]}` | One job. `error.params` is a subset match; `errorText` a substring of the error rendered in English. `titles` are the MakeMKV titles it ripped, `files` how many files it produced. `steps` lists the step kinds that ran, in order. |
| `jobs: {count, outcomes: [..]}` | All jobs. |
| `files: {exactly: [...], include: [...], exclude: [...], under: <folder>}` | Files and folders under the root (or `under`), hidden ones only when written explicitly. `*` matches within one path component, `**` across components; `{unit8}` is the job's unit short id. |
| `file: {path, text, contains: [..], json: <subset>, sha256}` | One file's content. `contains` strings are English text rendered from `en.json`. |
| `sums: {folder: <path>, ok: true}` | `sha256sum -c SHA256SUMS` would pass in that folder (and it lists exactly `files` if given). |
| `record: {folder, json: <subset>}` / `manifest: {job, json: <subset>}` | The unit's archive record / the job's manifest, subset match. |
| `transcript: {job, runs, contains: [..]}` | `makemkv-<unit8>-log.txt`: the number of `==== … $ <command>` headers and lines it contains. |
| `calls: {<tool>: {count, include: [..], exclude: [..], counts: {<substring>: n}, none: true}}` | The tool's command lines (arguments joined by spaces). `none`: the tool was never launched. |
| `log: {job, include: [codes], exclude: [codes]}` | Codes in the job's `log.jsonl`. |
| `events: {include: [{type, …subset}], sequence: [types], exclude: [types]}` | The events published so far. `sequence` must appear in this order (other events may come between). |
| `problems: {active: [codes], none: true}` | ProblemService's active problems. |
| `drives: [{id, …Drive subset}]` | `EngineAPI.drives()`. |
| `session: {name, …Session subset}` | An open session. |
| `db: {<table>: {count, rows: [subset]}}` | Database rows. |
| `power: {held: true \| false, changes: [true, false, …]}` | The fake PowerManager: whether a guard is held now, and every change so far. |
| `notifications: {sent: [{target, title: {code}, status}], count}` | The fake HTTP client / Apprise launches of NotificationSender. |
| `response: {as, status, json: <subset>, error: {code}}` | The answer to an `api` step. |

`when` on an `expect` step runs until the condition holds instead of until everything settles:
`when: {job: j, step: acquire}` (the step started), `{event: automationCountdown}`, `{job: j, state: running}`.

### Real-disc scenarios

`realDisc: {env}` marks a scenario that needs a real disc image (today's `BROMELIA_TEST_*ISO` tests). It runs only
when that variable is set, with the real makemkvcon, mkvmerge, ffmpeg and tesseract instead of scripted tools;
`iso: true` is the image, and `drive: realdisc` is the image attached as a device (macOS `hdiutil attach -nomount`,
Linux a loop device, Windows the file itself). Their expectations are what today's integration tests check.

## Discs (`discs/*.yaml`)

```yaml
volume: SAMPLE_MOVIE        # CINFO 2 and 32
type: bluray                # dvd | bluray | uhd | hddvd → CINFO 1 "DVD disc", "Blu-ray disc"; uhd adds 2160p video
titles:                     # generated listing; or `listing: info-dvd.txt` (a file in shared/fixtures)
  - {duration: "0:00:10", source: 1}
  - {duration: "0:00:20", source: 2, size: 1152921504606846976, chapters: 12}
content: video              # video | audio | data | blank (what DriveControl.probeContent says)
videoTs: dvd-play-all       # a folder in shared/fixtures: the disc's VIDEO_TS for ReadNavigation
image: {bytes: 3147776, seed: 7, iso9660: true}   # data discs: what the SectorReader returns
```

A generated listing is exactly this text (`<i>` the title number, `<s>` its source, `<d>` its duration):

```
MSG:1005,0,1,"MakeMKV v1.18.1 darwin(arm64-release) started","%1 started","MakeMKV v1.18.1 darwin(arm64-release)"
TCOUNT:<n>
CINFO:1,6209,"Blu-ray disc"
CINFO:2,0,"<volume>"
CINFO:32,0,"<volume>"
TINFO:<i>,8,0,"<chapters, default 2>"
TINFO:<i>,9,0,"<d>"
TINFO:<i>,16,0,"0000<s>.mpls"
TINFO:<i>,24,0,"<s>"
TINFO:<i>,26,0,"<s>"
TINFO:<i>,27,0,"title_t0<i>.mkv"
TINFO:<i>,11,0,"<size>"          (only when the title has a size)
SINFO:<i>,0,1,6201,"Video"
SINFO:<i>,1,1,6202,"Audio"
MSG:5011,0,0,"Operation successfully completed","Operation successfully completed"
```

for each title in order. CINFO 1 follows `type`: `dvd` → `6206,"DVD disc"`, `bluray` and `uhd` →
`6209,"Blu-ray disc"`, `hddvd` → `6207,"HD-DVD disc"` (`uhd` also gives every title's video stream
`SINFO:<i>,0,19,0,"3840x2160"`). It is the listing today's tests build, so the
scenarios taken from them keep their meaning.

## Tool scripts (`tools/*.yaml`)

A script answers each launch of one tool with the first rule whose `when` matches:

```yaml
tool: makemkvcon
rules:
  - when: {command: info}           # info | mkv | backup | reg | f; also title: "1", argsInclude: ["--minlength=11"]
    print: [listing]                # the disc's listing (titles shorter than --minlength left out and renumbered)
  - when: {command: mkv}
    writeTitles: {content: "mkv data {title}"}   # title_t0<i>.mkv in the destination: for "all", every title listed
    print: [saved]                  # names from tools/lines.yaml, or literal lines
    exit: 0
```

| Key | Meaning |
| --- | --- |
| `when` | `command`: for makemkvcon the subcommand (`info`, `mkv`, `backup`, `reg`, `f`), for other tools the first argument; `title`: makemkvcon's title argument; `argsInclude`: arguments that must be present; `file`: a glob on the input file. |
| `print` | Lines on stdout, in order: a name from `lines.yaml`, `listing`, `fixture:<file>` (a file in `shared/fixtures`), or a literal line. `{title}`, `{dest}`, `{device}`, `{debugLog}`, `{input}`, `{output}` (the `-i` / `-o` arguments), `{arg0}`, `{arg1}`, … (the arguments) and `{env:NAME}` are filled in. |
| `writeTitles: {content, titles}` | Write `title_t0<i>.mkv` for the title argument (for `all`: every listed title, or only `titles`). |
| `write: [{path, content \| bytes \| copyOf}]` | Write files relative to the destination (`{dest}`), or to the working directory for tools without one. `copyOf: input` copies the `-i` file (HandBrakeCLI). |
| `writeIso: {bytes, descriptor: CD001 \| BEA01}` | Write the destination as an image file with the volume descriptor at byte 32769. |
| `writeFolder: {<path>: <content>}` | Write the destination as a folder tree (a folder backup, or an ISO destination MakeMKV turned into a folder). |
| `writeDebugLog: <text>` | Replace MakeMKV's debug log (`{debugLog}`, inside the job's MakeMKV home) with this text. |
| `silence: <seconds>` | Print nothing for this long before going on (stall tests; the fake clock moves). |
| `ignore: [SIGINT]` | Signals the fake process ignores (makemkvcon ignores SIGINT; that is the default for it). |
| `exit` | Exit status (default 0). `never` = only ends when killed. |
| `probe: {<file glob>: {durationSeconds, tracks: [video, audio, …], chapters}}` | mkvmerge `-J`: the JSON to print for matching files. `matchListing: true` answers what the title in the listing says. |
| `chapters: {<file glob>: [seconds, …]}` | mkvextract chapter times. |
