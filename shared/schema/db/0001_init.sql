-- Bromelia database, migration 1 (rearchitecture plan §10.4, §23).
--
-- Run verbatim by SqliteStore on all three platforms, inside one transaction,
-- when PRAGMA user_version is below 1. Connections set journal_mode=WAL,
-- synchronous=FULL and foreign_keys=ON when they open; this file sets only
-- user_version. tools/check-contracts.py applies it to an empty database and
-- compares the result with 0001_init.expected.txt.
--
-- Conventions
--   * STRICT tables, so a value has the same type on every platform.
--   * Times are TEXT in UTC, ISO 8601 with milliseconds (2026-10-02T21:53:04.123Z).
--   * Booleans are INTEGER 0 / 1. JSON is TEXT that json_valid() accepts.
--   * Ids are lower-case UUID text, except libraries, drives and configuration
--     entries, which use the ids of the configuration (EntryId, DriveId).
--   * Nothing is deleted when a history limit is reached: jobs and units stay;
--     only job folders on disk are pruned (maintenance.retention.jobLogsDays).
--   * The database never claims more than the filesystem holds (invariant I6):
--     archive_units is rebuilt from the records by a rescan, and the records win.

-- ---- drives -------------------------------------------------------------------

CREATE TABLE drives (
  id              TEXT PRIMARY KEY,                 -- DriveId (DriveJoin.driveId)
  identification  TEXT NOT NULL,                    -- MakeMKV's drive name
  model           TEXT NOT NULL DEFAULT '',
  last_device     TEXT NOT NULL DEFAULT '',
  config_id       TEXT,                             -- drives[] entry last matched
  first_seen_at   TEXT NOT NULL,
  last_seen_at    TEXT NOT NULL
) STRICT;

-- Drive health (backlog 7): one row per drive and day.
CREATE TABLE drive_stats_daily (
  drive_id         TEXT NOT NULL REFERENCES drives(id) ON DELETE CASCADE,
  day              TEXT NOT NULL,                   -- YYYY-MM-DD
  jobs             INTEGER NOT NULL DEFAULT 0,
  failed_jobs      INTEGER NOT NULL DEFAULT 0,
  read_error_jobs  INTEGER NOT NULL DEFAULT 0,
  read_errors      INTEGER NOT NULL DEFAULT 0,
  bytes_read       INTEGER NOT NULL DEFAULT 0,
  seconds_reading  REAL NOT NULL DEFAULT 0,
  PRIMARY KEY (drive_id, day)
) STRICT;

-- ---- catalogue ------------------------------------------------------------------

CREATE TABLE libraries (
  id             TEXT PRIMARY KEY,                  -- libraries[] entry id
  name           TEXT NOT NULL,
  path           TEXT NOT NULL,
  marker_id      TEXT NOT NULL,                     -- the id in .bromelia-library
  attached_at    TEXT NOT NULL,
  online         INTEGER NOT NULL DEFAULT 0 CHECK (online IN (0, 1)),
  last_seen_at   TEXT
) STRICT;

-- A movie or show (from the online lookup, else from the disc's name).
CREATE TABLE works (
  id          TEXT PRIMARY KEY,
  kind        TEXT NOT NULL CHECK (kind IN ('movie', 'tv')),
  title       TEXT NOT NULL,
  year        INTEGER,
  tmdb_id     INTEGER,
  imdb_id     TEXT,
  created_at  TEXT NOT NULL,
  updated_at  TEXT NOT NULL
) STRICT;
CREATE INDEX works_title ON works (title COLLATE NOCASE);
CREATE UNIQUE INDEX works_tmdb ON works (kind, tmdb_id) WHERE tmdb_id IS NOT NULL;

-- The discs of a release that belong together (Season 2 Part 7, Volume 3, …).
CREATE TABLE disc_sets (
  id           TEXT PRIMARY KEY,
  work_id      TEXT REFERENCES works(id) ON DELETE SET NULL,
  label_title  TEXT NOT NULL DEFAULT '',            -- LabelParser title, for sets without a work
  season       INTEGER,
  part         INTEGER,
  volume       INTEGER,
  description  TEXT NOT NULL DEFAULT '',            -- "Season 2 Part 7"
  known_count  INTEGER                              -- discs in the set, when known
) STRICT;
CREATE INDEX disc_sets_work ON disc_sets (work_id);

-- A physical disc (backlog 9: barcode and shelf location).
CREATE TABLE physical_discs (
  id           TEXT PRIMARY KEY,
  set_id       TEXT REFERENCES disc_sets(id) ON DELETE SET NULL,
  work_id      TEXT REFERENCES works(id) ON DELETE SET NULL,
  disc_number  INTEGER,
  fingerprint  TEXT,                                -- v1:… (Fingerprint.of)
  label        TEXT NOT NULL DEFAULT '',
  barcode      TEXT,
  location     TEXT,
  notes        TEXT NOT NULL DEFAULT '',
  created_at   TEXT NOT NULL
) STRICT;
CREATE INDEX physical_discs_fingerprint ON physical_discs (fingerprint);
CREATE INDEX physical_discs_set ON physical_discs (set_id, disc_number);

-- One archived acquisition of a disc: a folder with SHA256SUMS and bromelia-<unit8>.json.
CREATE TABLE archive_units (
  id                TEXT PRIMARY KEY,
  library_id        TEXT NOT NULL REFERENCES libraries(id),
  physical_disc_id  TEXT REFERENCES physical_discs(id) ON DELETE SET NULL,
  job_id            TEXT,                           -- the job that made it (NULL for imported v2 records)
  path              TEXT NOT NULL,                  -- the unit's folder, relative to the library
  record_file       TEXT NOT NULL,                  -- bromelia-<unit8>.json, or bromelia.json for v2
  record_version    INTEGER NOT NULL CHECK (record_version IN (2, 3)),
  state             TEXT NOT NULL CHECK (state IN ('committing', 'committed', 'quarantined', 'missing')),
  status            TEXT NOT NULL CHECK (status IN ('success', 'errors', 'incomplete')),
  name              TEXT NOT NULL,
  kind              TEXT NOT NULL CHECK (kind IN ('movie', 'tv')),
  format            TEXT NOT NULL,
  format_code       TEXT NOT NULL,
  encrypted         INTEGER NOT NULL DEFAULT 0 CHECK (encrypted IN (0, 1)),
  fingerprint       TEXT,
  label             TEXT NOT NULL DEFAULT '',
  season            INTEGER,
  part              INTEGER,
  volume            INTEGER,
  disc              INTEGER,
  makemkv_version   TEXT NOT NULL DEFAULT '',
  bytes             INTEGER NOT NULL DEFAULT 0,
  file_count        INTEGER NOT NULL DEFAULT 0,
  attempts          INTEGER NOT NULL DEFAULT 1,
  created_at        TEXT NOT NULL,
  committed_at      TEXT
) STRICT;
CREATE INDEX archive_units_fingerprint ON archive_units (fingerprint, status);
CREATE INDEX archive_units_library ON archive_units (library_id, path);
CREATE INDEX archive_units_previous_disc ON archive_units (kind, label, season, part, volume, disc);
CREATE INDEX archive_units_name ON archive_units (name COLLATE NOCASE);

CREATE TABLE unit_files (
  unit_id  TEXT NOT NULL REFERENCES archive_units(id) ON DELETE CASCADE,
  path     TEXT NOT NULL,                           -- relative to the unit's folder
  size     INTEGER NOT NULL,
  sha256   TEXT NOT NULL CHECK (length(sha256) = 64),
  role     TEXT NOT NULL DEFAULT 'title',
  title    INTEGER,
  episode  INTEGER,
  PRIMARY KEY (unit_id, path)
) STRICT;

-- The commit protocol's intent (plan §22.3, steps 4–6): rolled forward at startup.
CREATE TABLE commit_intents (
  unit_id      TEXT PRIMARY KEY REFERENCES archive_units(id) ON DELETE CASCADE,
  job_id       TEXT NOT NULL,
  staging      TEXT NOT NULL,                       -- absolute path of the staging folder
  destination  TEXT NOT NULL,                       -- absolute path of the unit's folder
  merge        INTEGER NOT NULL DEFAULT 0 CHECK (merge IN (0, 1)),
  quarantine   INTEGER NOT NULL DEFAULT 0 CHECK (quarantine IN (0, 1)),
  created_at   TEXT NOT NULL
) STRICT;

CREATE TABLE commit_items (
  unit_id    TEXT NOT NULL REFERENCES commit_intents(unit_id) ON DELETE CASCADE,
  seq        INTEGER NOT NULL,
  from_path  TEXT NOT NULL,                         -- relative to staging
  to_path    TEXT NOT NULL,                         -- relative to the destination (after ConflictNamer)
  sha256     TEXT,                                  -- files only: recognises an item already moved
  is_dir     INTEGER NOT NULL DEFAULT 0 CHECK (is_dir IN (0, 1)),
  moved      INTEGER NOT NULL DEFAULT 0 CHECK (moved IN (0, 1)),
  PRIMARY KEY (unit_id, seq)
) STRICT;

-- Verification results (plan §22.4): one row per check of a unit (or of a folder that isn't one).
CREATE TABLE checks (
  id           TEXT PRIMARY KEY,
  unit_id      TEXT REFERENCES archive_units(id) ON DELETE CASCADE,
  replica_id   TEXT REFERENCES replicas(id) ON DELETE CASCADE,
  folder       TEXT NOT NULL,                       -- absolute path that was checked
  job_id       TEXT,
  started_at   TEXT NOT NULL,
  finished_at  TEXT,
  result       TEXT NOT NULL CHECK (result IN ('ok', 'damaged', 'error', 'stopped')),
  files        INTEGER NOT NULL DEFAULT 0,
  bytes        INTEGER NOT NULL DEFAULT 0,
  changed      TEXT NOT NULL DEFAULT '[]' CHECK (json_valid(changed)),
  unreadable   TEXT NOT NULL DEFAULT '[]' CHECK (json_valid(unreadable)),
  missing      TEXT NOT NULL DEFAULT '[]' CHECK (json_valid(missing)),
  unlisted     TEXT NOT NULL DEFAULT '[]' CHECK (json_valid(unlisted)),
  error        TEXT CHECK (error IS NULL OR json_valid(error))   -- BroMessage
) STRICT;
CREATE INDEX checks_unit ON checks (unit_id, finished_at);
CREATE INDEX checks_folder ON checks (folder, finished_at);

-- Verified second copies (backlog 2).
CREATE TABLE replicas (
  id           TEXT PRIMARY KEY,
  unit_id      TEXT NOT NULL REFERENCES archive_units(id) ON DELETE CASCADE,
  target_id    TEXT NOT NULL,                       -- targets[] entry id
  path         TEXT NOT NULL,
  state        TEXT NOT NULL CHECK (state IN ('pending', 'copying', 'verified', 'stale', 'failed')),
  verified_at  TEXT,
  error        TEXT CHECK (error IS NULL OR json_valid(error)),
  UNIQUE (unit_id, target_id)
) STRICT;

-- PAR2 recovery data (backlog 3).
CREATE TABLE parity (
  unit_id      TEXT PRIMARY KEY REFERENCES archive_units(id) ON DELETE CASCADE,
  percent      INTEGER NOT NULL CHECK (percent BETWEEN 1 AND 100),
  files        TEXT NOT NULL DEFAULT '[]' CHECK (json_valid(files)),
  state        TEXT NOT NULL CHECK (state IN ('pending', 'ready', 'stale', 'failed')),
  created_at   TEXT NOT NULL,
  verified_at  TEXT
) STRICT;

-- ---- jobs -------------------------------------------------------------------------

CREATE TABLE jobs (
  id                TEXT PRIMARY KEY,
  kind              TEXT NOT NULL,                  -- JobKind
  parent_id         TEXT REFERENCES jobs(id),
  state             TEXT NOT NULL CHECK (state IN ('queued', 'waitingForResources', 'running', 'blocked', 'awaitingDecision', 'finished')),
  outcome           TEXT CHECK (outcome IS NULL OR outcome IN ('succeeded', 'succeededWithReadErrors', 'succeededStepsFailed', 'failed', 'cancelled', 'skipped', 'interrupted')),
  error             TEXT CHECK (error IS NULL OR json_valid(error)),          -- BroError
  blocked_by        TEXT CHECK (blocked_by IS NULL OR json_valid(blocked_by)),
  decision          TEXT CHECK (decision IS NULL OR json_valid(decision)),    -- the open question
  queue             TEXT NOT NULL CHECK (queue IN ('acquisition', 'processing', 'maintenance')),
  position          INTEGER NOT NULL DEFAULT 0,
  drive_id          TEXT,
  media_generation  INTEGER,                         -- the drive's medium generation when queued
  automatic         INTEGER NOT NULL DEFAULT 0 CHECK (automatic IN (0, 1)),
  mode              TEXT,
  title             TEXT NOT NULL DEFAULT '',
  fingerprint       TEXT,
  request           TEXT NOT NULL CHECK (json_valid(request)),               -- JobRequest, with the session snapshot
  plan              TEXT CHECK (plan IS NULL OR json_valid(plan)),           -- JobPlan once PlanStep ran
  unit_id           TEXT,
  created_at        TEXT NOT NULL,
  started_at        TEXT,
  finished_at       TEXT
) STRICT;
CREATE INDEX jobs_active ON jobs (state, queue, position) WHERE state <> 'finished';
CREATE INDEX jobs_finished ON jobs (finished_at) WHERE state = 'finished';
CREATE INDEX jobs_parent ON jobs (parent_id);
CREATE INDEX jobs_fingerprint ON jobs (fingerprint);

-- One row per step of a job: the durable checkpoint between steps (plan §12.3).
CREATE TABLE job_steps (
  job_id       TEXT NOT NULL REFERENCES jobs(id) ON DELETE CASCADE,
  seq          INTEGER NOT NULL,
  kind         TEXT NOT NULL,                        -- StepKind
  state        TEXT NOT NULL CHECK (state IN ('pending', 'running', 'succeeded', 'failed', 'skipped', 'cancelled', 'interrupted')),
  attempt      INTEGER NOT NULL DEFAULT 1,
  started_at   TEXT,
  finished_at  TEXT,
  output       TEXT CHECK (output IS NULL OR json_valid(output)),         -- StepOutput
  error        TEXT CHECK (error IS NULL OR json_valid(error)),
  checkpoint   TEXT CHECK (checkpoint IS NULL OR json_valid(checkpoint)), -- resumable steps (e.g. a data-rescue sector map)
  PRIMARY KEY (job_id, seq)
) STRICT;

-- Space reserved on a volume by jobs that will write there (plan §21).
CREATE TABLE reservations (
  id          TEXT PRIMARY KEY,
  job_id      TEXT NOT NULL REFERENCES jobs(id) ON DELETE CASCADE,
  volume_id   TEXT NOT NULL,
  bytes       INTEGER NOT NULL CHECK (bytes >= 0),
  created_at  TEXT NOT NULL
) STRICT;
CREATE INDEX reservations_volume ON reservations (volume_id);

-- ---- services ---------------------------------------------------------------------

CREATE TABLE lookup_cache (
  provider     TEXT NOT NULL,                        -- tmdb | omdb | makemkvForum
  request_key  TEXT NOT NULL,                        -- the request without credentials
  status       INTEGER NOT NULL,
  response     BLOB NOT NULL,
  fetched_at   TEXT NOT NULL,
  expires_at   TEXT NOT NULL,
  PRIMARY KEY (provider, request_key)
) STRICT;

-- Notifications waiting to be sent, and what happened to them.
CREATE TABLE outbox (
  id               TEXT PRIMARY KEY,
  target_id        TEXT NOT NULL,                    -- notifications.targets[] entry id
  job_id           TEXT,
  title            TEXT NOT NULL CHECK (json_valid(title)),   -- BroMessage
  body             TEXT NOT NULL CHECK (json_valid(body)),    -- BroMessage
  status           TEXT NOT NULL,                    -- StatusWord
  created_at       TEXT NOT NULL,
  attempts         INTEGER NOT NULL DEFAULT 0,
  next_attempt_at  TEXT,
  sent_at          TEXT,
  last_error       TEXT CHECK (last_error IS NULL OR json_valid(last_error))
) STRICT;
CREATE INDEX outbox_pending ON outbox (next_attempt_at) WHERE sent_at IS NULL;

CREATE TABLE kv (
  key         TEXT PRIMARY KEY,
  value       TEXT NOT NULL CHECK (json_valid(value)),
  updated_at  TEXT NOT NULL
) STRICT;

PRAGMA user_version = 1;
