# shared/

Everything the three implementations share. Today's apps use `catalog/`, `web/` and the original files in
`fixtures/`. The rest is the language-neutral contract of the rebuild (`claudeDocs/rearchitecture-plan.md`, §18):
phase 0, written before any engine code, so the three engines are built against one specification.

| Path | What | Checked by |
| --- | --- | --- |
| `architecture-map.json` | Every module, type, port and public function of Foundation, Domain and Ports with its Swift, C# and C name and file; the layer folders and what each may depend on | `tools/check-architecture.py`, `tools/check-layers.py` |
| `schema/common.json` | Ids, times, messages, secrets and the shared enumerations | every other schema |
| `schema/config-3.json` | The configuration, version 3 (plan §24); `fixtures/config/config3-defaults.json` is its expanded default | `tools/check-contracts.py` |
| `schema/config-migration-2-3.json` | Where each version 2 key goes, with golden cases in `fixtures/config/config-migration-cases.json` | `tools/check-contracts.py` |
| `schema/archive-record-3.json`, `archive-record-2.json` | `bromelia-<unit8>.json`, and today's `bromelia.json` that the rebuild still reads | samples in `fixtures/archive/` |
| `schema/manifest-2.json` | The post-processing manifest; adds to version 1 without renaming anything | samples in `fixtures/archive/` |
| `schema/events.json` | The event stream (SSE) | `fixtures/api/events-sample.jsonl` |
| `schema/api/routes.json`, `api/dtos.json` | One HTTP route per EngineAPI method, and the data they exchange | `fixtures/api/api-samples.json` |
| `schema/db/0001_init.sql` | The SQLite schema every engine runs verbatim; `0001_init.expected.txt` is the result | `tools/check-contracts.py` (applies it) |
| `schema/scenario.json` | The format of `scenarios/` | `tools/check-contracts.py` |
| `messages/codes.json`, `messages/en.json` | Every code the engine emits, and its English text (ICU MessageFormat) | `tools/check-contracts.py` |
| `fixtures/` | Recorded makemkvcon output, lookup answers, NFO, notifications, IFOs (today's), and the rebuild's golden cases: `domain/*.cases.json` for every pure function, `api/`, `host/`, `adapters/*.contract.json`, `config/`, `archive/` | each platform's tests |
| `scenarios/` | End-to-end behaviour written once and run by every platform's scenario runner; `COVERAGE.md` maps every test of today's suites and every documented behaviour to a fixture or scenario | `tools/check-contracts.py` |
| `catalog/settings-catalog.json` | MakeMKV's settings, for the settings forms | today's apps |
| `web/` | The web page | Playwright in CI |

Rules:

- **File names under `fixtures/` are unique**, because the macOS test bundle flattens the folder
  (`tools/check-contracts.py` checks it).
- **Golden files are exact.** Every implementation must produce exactly the expected output of a case, not
  something similar.
- **Generated files** (`config3-defaults.json`, `profile3-defaults.json`, `0001_init.expected.txt`,
  `scenarios/COVERAGE.md`) are rewritten with `tools/check-contracts.py --write`; CI fails when they are stale.

```bash
pip install "jsonschema>=4.18" pyyaml
python3 tools/check-contracts.py
python3 tools/check-architecture.py
python3 tools/check-layers.py
```
