#!/usr/bin/env python3
"""Checks shared/architecture-map.json against the three code trees (rearchitecture plan §5.4).

  lint      the map itself: spellings follow the naming rules, no name or file is used twice, every
            function's owner type is in the map, the job enums match shared/schema/common.json, and every
            function a shared fixture names is in the map. Lint problems always fail.
  presence  for each platform: every type's file exists and defines it, every function is declared in its
            owner type's file. Missing entries are reported; they fail only with --strict or for entries of
            a phase ≤ --require-phase.
  unmapped  public symbols in the layer folders that the map doesn't list (reported; fail with --strict).

  idioms    the map's "idioms" section lists, per platform, public symbols that a language idiom of plan §6 needs
            and the other platforms don't have (C#'s BroFailure, C's accessors and boxed-type helpers). They
            aren't reported as unmapped. A C# type nested in a mapped enum and named after one of its cases (an
            enum with values, written as records) counts as mapped too.

Usage: tools/check-architecture.py [--platform swift|cs|c] [--module KEY] [--require-phase N] [--strict] [--quiet]
  --module KEY  only the modules whose key is KEY or starts with KEY/ (e.g. domain/robot, ports); the unmapped
                symbols are those in the files of those modules' types
Needs only the Python 3 standard library.
"""
import argparse
import fnmatch
import json
import re
import sys
from pathlib import Path

ROOT = Path(__file__).resolve().parent.parent
MAP = ROOT / "shared" / "architecture-map.json"
PLATFORMS = {"swift": "macOS (Swift)", "cs": "Windows (C#)", "c": "Linux (C)"}


def snake(name):
    return re.sub(r"(?<=[a-z0-9])(?=[A-Z])|(?<=[A-Z])(?=[A-Z][a-z])", "_", name).lower()


def kebab(name):
    return snake(name).replace("_", "-")


def pascal(name):
    return name[:1].upper() + name[1:]


def expected_spellings(module_key, module, layers):
    """What the naming rules say each entry is called, and where it lives."""
    layer = layers[module["layer"]]
    folder = module["folder"]
    cfolder = module.get("cFolder", module_key.split("/")[1] if folder else "")
    csroot = layer["cs"] if isinstance(layer["cs"], str) else layer["cs"][0]
    types = {}
    for t in module["types"]:
        name = t["name"]
        port = t["kind"] == "protocol"
        cs = ("I" + name) if port else name
        sub = f"/{folder}" if folder else ""
        csub = f"/{cfolder}" if cfolder else ""
        e = {"swift": {"name": name, "file": f"{layer['swift']}{sub}/{name}.swift"},
             "cs": {"name": cs, "file": f"{csroot}{sub}/{cs}.cs"},
             "c": {"name": "Bro" + name, "file": f"{layer['c']}{csub}/bro-{kebab(name)}.h"}}
        if t.get("cases"):
            e["swift"]["cases"] = ["." + c for c in t["cases"]]
            e["cs"]["cases"] = [pascal(c) for c in t["cases"]]
            e["c"]["cases"] = ["BRO_" + snake(name).upper() + "_" + snake(c).upper() for c in t["cases"]]
        types[name] = e
    funcs = {}
    for f in module["functions"]:
        owner, fn = f["name"].split(".")
        labels = []
        for i, p in enumerate(f["params"]):
            lab = p.split(":")[0] if ":" in p else (None if i == 0 else p)
            labels.append((lab or "_") + ":")
        funcs[f["name"]] = {"swift": f"{owner}.{fn}({''.join(labels)})", "cs": f"{owner}.{pascal(fn)}", "c": f"bro_{snake(owner)}_{snake(fn)}"}
    return types, funcs


class Checker:
    def __init__(self, args):
        self.args = args
        self.doc = json.loads(MAP.read_text(encoding="utf-8"))
        self.lint_errors, self.missing, self.unmapped = [], [], []
        self.types = {}   # name → (module key, entry)
        for key, m in self.doc["modules"].items():
            for t in m["types"]:
                self.types.setdefault(t["name"], []).append((key, t))

    # ---- lint -----------------------------------------------------------------------------------
    def lint(self):
        layers = self.doc["layers"]
        seen_files, seen_funcs = {}, {}
        for key, m in self.doc["modules"].items():
            if m["layer"] not in layers:
                self.lint_errors.append(f"{key}: unknown layer {m['layer']}")
                continue
            types, funcs = expected_spellings(key, m, layers)
            for t in m["types"]:
                exp = types[t["name"]]
                for p in PLATFORMS:
                    if t.get(p) != exp[p]:
                        self.lint_errors.append(f"{key}: type {t['name']}: {p} spelling {t.get(p)} should be {exp[p]}")
                    f = exp[p]["file"]
                    if f in seen_files:
                        self.lint_errors.append(f"{key}: {t['name']} and {seen_files[f]} share the file {f}")
                    seen_files[f] = t["name"]
            for f in m["functions"]:
                exp = funcs[f["name"]]
                for p in PLATFORMS:
                    if f.get(p) != exp[p]:
                        self.lint_errors.append(f"{key}: {f['name']}: {p} spelling {f.get(p)} should be {exp[p]}")
                if f["name"] in seen_funcs:
                    self.lint_errors.append(f"{key}: {f['name']} is also in {seen_funcs[f['name']]}")
                seen_funcs[f["name"]] = key
                owner = f["name"].split(".")[0]
                if owner not in self.types:
                    self.lint_errors.append(f"{key}: {f['name']}: its owner type {owner} isn't in the map")
        for platform, entries in self.doc.get("idioms", {}).items():
            if platform not in PLATFORMS:
                self.lint_errors.append(f"idioms: unknown platform {platform}")
                continue
            for i in entries:
                if not i.get("symbol") or not i.get("why"):
                    self.lint_errors.append(f"idioms ({platform}): every entry needs a symbol and a why: {i}")
        for name, entries in self.types.items():
            if len(entries) > 1:
                self.lint_errors.append(f"type {name} is in {', '.join(k for k, _ in entries)}")
        common = json.loads((ROOT / "shared" / "schema" / "common.json").read_text(encoding="utf-8"))["$defs"]
        for name in ("JobKind", "JobState", "Outcome", "StepKind", "StepState", "Queue"):
            entries = self.types.get(name)
            if entries and entries[0][1].get("cases") != common[name]["enum"]:
                self.lint_errors.append(f"enum {name} doesn't match shared/schema/common.json")
        for p in sorted((ROOT / "shared" / "fixtures").rglob("*.json")):
            try:
                d = json.loads(p.read_text(encoding="utf-8"))
            except json.JSONDecodeError:
                continue
            for fn in d.get("functions", []) if isinstance(d, dict) else []:
                if fn not in seen_funcs:
                    self.lint_errors.append(f"{p.relative_to(ROOT)} names {fn}, which isn't in the map")

    # ---- presence --------------------------------------------------------------------------------
    def defines_type(self, platform, text, name, kind):
        if platform == "swift":
            return re.search(rf"\b(struct|enum|class|protocol|actor|typealias)\s+{re.escape(name)}\b", text)
        if platform == "cs":
            return re.search(rf"\b(class|record|struct|interface|enum)\s+{re.escape(name)}\b", text)
        return re.search(rf"\b{re.escape(name)}\b", text)

    def declares_function(self, platform, text, spelled):
        if platform == "swift":
            fn = spelled.split(".", 1)[1].split("(")[0]
            return re.search(rf"\bfunc\s+{re.escape(fn)}\s*[(<]", text) or re.search(rf"\bvar\s+{re.escape(fn)}\b", text)
        if platform == "cs":
            fn = spelled.split(".", 1)[1]
            return re.search(rf"\b{re.escape(fn)}\s*[(<]", text)
        return re.search(rf"\b{re.escape(spelled)}\s*\(", text)

    def selected(self):
        sel = self.args.module
        return {k: m for k, m in self.doc["modules"].items()
                if not sel or k == sel or k.startswith(sel.rstrip("/") + "/")}

    def presence(self, platform):
        req = self.args.require_phase
        for key, m in self.selected().items():
            for t in m["types"]:
                f = ROOT / t[platform]["file"]
                text = f.read_text(encoding="utf-8", errors="replace") if f.exists() else ""
                if not text or not self.defines_type(platform, text, t[platform]["name"], t["kind"]):
                    self.missing.append((platform, m["phase"], f"{key}: type {t[platform]['name']} ({t[platform]['file']})"))
            for fn in m["functions"]:
                owner = fn["name"].split(".")[0]
                entry = self.types[owner][0][1] if owner in self.types else None
                f = ROOT / entry[platform]["file"] if entry else None
                text = f.read_text(encoding="utf-8", errors="replace") if f and f.exists() else ""
                if platform == "c" and f is not None and not text:
                    text = ""
                if not text or not self.declares_function(platform, text, fn[platform]):
                    self.missing.append((platform, m["phase"], f"{key}: {fn[platform]}"))
        return req

    # ---- unmapped public symbols ------------------------------------------------------------------
    def unmapped_symbols(self, platform):
        layers = self.doc["layers"]
        known_types = {t[platform]["name"] for es in self.types.values() for _, t in es}
        idioms = [i["symbol"] for i in self.doc.get("idioms", {}).get(platform, [])]
        known_types |= set(idioms)
        if platform == "cs":
            known_types |= {c for es in self.types.values() for _, t in es for c in t["cs"].get("cases", [])}
        only = None
        if self.args.module:
            only = {(ROOT / t[platform]["file"]).resolve() for m in self.selected().values() for t in m["types"]}
        known_funcs = set()
        for m in self.doc["modules"].values():
            for fn in m["functions"]:
                s = fn[platform]
                known_funcs.add(s if platform == "c" else s.split(".", 1)[1].split("(")[0])
        known_funcs |= set(idioms)

        def known(name, pool):
            return name in pool or any(fnmatch.fnmatchcase(name, i) for i in idioms)
        for lname in ("foundation", "domain", "ports"):
            roots = layers[lname][platform]
            for root in roots if isinstance(roots, list) else [roots]:
                base = ROOT / root
                if not base.exists():
                    continue
                for f in sorted(base.rglob({"swift": "*.swift", "cs": "*.cs", "c": "*.h"}[platform])):
                    if "/obj/" in f.as_posix() or "/bin/" in f.as_posix() or (only is not None and f.resolve() not in only):
                        continue
                    text = f.read_text(encoding="utf-8", errors="replace")
                    rel = f.relative_to(ROOT)
                    if platform == "swift":
                        for m in re.finditer(r"\bpublic\s+(?:final\s+)?(?:struct|enum|class|protocol|actor)\s+(\w+)", text):
                            if not known(m.group(1), known_types):
                                self.unmapped.append((platform, f"{rel}: type {m.group(1)}"))
                        for m in re.finditer(r"\bpublic\s+(?:static\s+)?func\s+(\w+)", text):
                            if not known(m.group(1), known_funcs):
                                self.unmapped.append((platform, f"{rel}: func {m.group(1)}"))
                    elif platform == "cs":
                        for m in re.finditer(r"\bpublic\s+(?:(?:sealed|static|abstract|readonly|partial)\s+)*(?:class|record(?:\s+struct|\s+class)?|struct|interface|enum)\s+(\w+)", text):
                            if not known(m.group(1), known_types):
                                self.unmapped.append((platform, f"{rel}: type {m.group(1)}"))
                    else:
                        for m in re.finditer(r"^[A-Za-z_][\w\s\*]*?\b(bro_\w+)\s*\(", text, re.M):
                            if not known(m.group(1), known_funcs) and not m.group(1).endswith(("_get_type", "_free", "_copy", "_new", "_ref", "_unref")):
                                self.unmapped.append((platform, f"{rel}: {m.group(1)}"))

    def run(self):
        self.lint()
        platforms = [self.args.platform] if self.args.platform else list(PLATFORMS)
        for p in platforms:
            self.presence(p)
            self.unmapped_symbols(p)
        status = 0
        for e in self.lint_errors:
            print(f"lint: {e}")
        if self.lint_errors:
            status = 1
        modules = self.selected()
        nt = sum(len(m["types"]) for m in modules.values())
        nf = sum(len(m["functions"]) for m in modules.values())
        print(f"architecture map{f' ({self.args.module})' if self.args.module else ''}: {len(modules)} modules, {nt} types, {nf} functions; {len(self.lint_errors)} lint problem(s)")
        for p in platforms:
            miss = [x for x in self.missing if x[0] == p]
            unm = [x for x in self.unmapped if x[0] == p]
            due = [x for x in miss if self.args.require_phase is not None and x[1] <= self.args.require_phase]
            print(f"{PLATFORMS[p]}: {nt + nf - len(miss)} of {nt + nf} entries present, {len(miss)} missing"
                  f"{f' ({len(due)} due by phase {self.args.require_phase})' if self.args.require_phase is not None else ''}, {len(unm)} unmapped public symbol(s)")
            if not self.args.quiet:
                for _, phase, what in (due if due else miss)[:20]:
                    print(f"  missing (phase {phase}): {what}")
                if len(due if due else miss) > 20:
                    print(f"  … and {len(due if due else miss) - 20} more")
                for _, what in unm[:20]:
                    print(f"  unmapped: {what}")
            if due or (self.args.strict and (miss or unm)):
                status = 1
        return status


def main():
    ap = argparse.ArgumentParser(description=__doc__, formatter_class=argparse.RawDescriptionHelpFormatter)
    ap.add_argument("--platform", choices=list(PLATFORMS))
    ap.add_argument("--module", help="only this module, or the modules under this prefix")
    ap.add_argument("--require-phase", type=int, help="fail on missing entries of this phase or earlier")
    ap.add_argument("--strict", action="store_true", help="fail on any missing entry or unmapped symbol")
    ap.add_argument("--quiet", action="store_true", help="only the summary")
    return Checker(ap.parse_args()).run()


if __name__ == "__main__":
    sys.exit(main())
