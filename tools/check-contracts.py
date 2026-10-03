#!/usr/bin/env python3
"""Checks the language-neutral contracts in shared/ (rearchitecture plan §18).

Runs in CI and needs only Python 3.10+ with jsonschema and PyYAML. It checks that
the contracts are consistent with each other, not that any platform implements
them (tools/check-architecture.py does that):

  schemas     every shared/schema/**/*.json is a valid JSON Schema (2020-12)
  config      config samples validate; the expanded default document matches;
              the v2 → v3 key map covers every v2 key and points at real v3 keys
  fixtures    file names are unique across shared/fixtures (the macOS test
              bundle flattens folders); *.cases.json files are well formed
  ...         further sections are listed in SECTIONS at the bottom

  --write     regenerate the generated files (config3-defaults.json, …) instead
              of failing when they differ
  --only S    run only the named sections (comma separated)

Exit status 0 when everything holds, 1 otherwise.
"""
import argparse
import copy
import json
import os
import re
import sys
from pathlib import Path

try:
    import jsonschema
    import yaml
    from referencing import Registry, Resource
    from referencing.jsonschema import DRAFT202012
except ImportError as e:  # pragma: no cover
    sys.exit(f"check-contracts: {e}. Install with: pip install jsonschema pyyaml")

ROOT = Path(__file__).resolve().parent.parent
SHARED = ROOT / "shared"
SCHEMA = SHARED / "schema"
FIXTURES = SHARED / "fixtures"
BASE = "https://schema.bromelia.invalid/"


class Report:
    def __init__(self):
        self.errors = []
        self.section = ""
        self.write = False

    def error(self, msg):
        self.errors.append(f"[{self.section}] {msg}")

    def check(self, cond, msg):
        if not cond:
            self.error(msg)
        return cond


R = Report()


def load_json(path):
    with open(path, encoding="utf-8") as f:
        return json.load(f)


def load_yaml(path):
    with open(path, encoding="utf-8") as f:
        return yaml.safe_load(f)


def dump_json(obj):
    return json.dumps(obj, indent=2, ensure_ascii=False) + "\n"


def rel(path):
    return os.path.relpath(path, ROOT)


def generated(path, text):
    """Compares a generated file with what is on disk (or writes it with --write)."""
    path = Path(path)
    old = path.read_text(encoding="utf-8") if path.exists() else None
    if old == text:
        return
    if R.write:
        path.parent.mkdir(parents=True, exist_ok=True)
        path.write_text(text, encoding="utf-8")
        print(f"wrote {rel(path)}")
    else:
        R.error(f"{rel(path)} is out of date; run tools/check-contracts.py --write")


# ---- schemas ----------------------------------------------------------------

_schemas = None
_registry = None


def schemas():
    global _schemas, _registry
    if _schemas is None:
        _schemas = {}
        for p in sorted(SCHEMA.rglob("*.json")):
            doc = load_json(p)
            if "$id" in doc:
                _schemas[doc["$id"]] = (p, doc)
        _registry = Registry().with_resources(
            (sid, Resource.from_contents(doc, default_specification=DRAFT202012)) for sid, (_, doc) in _schemas.items())
    return _schemas


def registry():
    schemas()
    return _registry


def schema_id(name):
    return BASE + name


def validator(name, definition=None):
    sid = schema_id(name)
    ref = sid + (f"#/$defs/{definition}" if definition else "")
    return jsonschema.Draft202012Validator({"$ref": ref}, registry=registry(),
                                           format_checker=jsonschema.FormatChecker())


def validate(instance, name, definition=None, what=""):
    errs = sorted(validator(name, definition).iter_errors(instance), key=lambda e: list(e.absolute_path))
    for e in errs[:10]:
        where = "/".join(str(p) for p in e.absolute_path) or "(root)"
        R.error(f"{what}: {where}: {e.message[:300]}")
    return not errs


def check_schemas():
    for sid, (path, doc) in schemas().items():
        R.check(sid == schema_id(os.path.relpath(path, SCHEMA).replace(os.sep, "/")),
                f"{rel(path)}: $id should be {schema_id(os.path.relpath(path, SCHEMA))}")
        try:
            jsonschema.Draft202012Validator.check_schema(doc)
        except jsonschema.SchemaError as e:
            R.error(f"{rel(path)}: not a valid schema: {e.message}")
    for p in SCHEMA.rglob("*.json"):
        doc = load_json(p)
        if "$schema" in doc and "$id" not in doc:
            R.error(f"{rel(p)}: a schema needs an $id")
    # Every $ref must resolve.
    for sid, (path, doc) in schemas().items():
        for ref in _refs(doc):
            target = ref if "://" in ref else (sid.rsplit("/", 1)[0] + "/" + ref if not ref.startswith("#") else sid + ref)
            try:
                registry().resolver(base_uri=sid).lookup(ref)
            except Exception as e:  # noqa: BLE001
                R.error(f"{rel(path)}: $ref {ref} doesn't resolve ({type(e).__name__})")


def _refs(node):
    if isinstance(node, dict):
        for k, v in node.items():
            if k == "$ref" and isinstance(v, str):
                yield v
            else:
                yield from _refs(v)
    elif isinstance(node, list):
        for v in node:
            yield from _refs(v)


# ---- schema walking (defaults, key paths) -----------------------------------

def resolve(node, base):
    """Follows $ref (and a single-object allOf) to the schema node; returns (node, base id)."""
    seen = 0
    while isinstance(node, dict) and "$ref" in node and seen < 20:
        seen += 1
        res = registry().resolver(base_uri=base).lookup(node["$ref"])
        node, base = res.contents, res.resolver._base_uri
    return node, base


def object_variant(node, base):
    """The object-typed branch of a schema (through $ref, allOf and oneOf), or None."""
    node, base = resolve(node, base)
    if not isinstance(node, dict):
        return None, base
    if "properties" in node:
        return node, base
    for key in ("allOf", "oneOf", "anyOf"):
        for sub in node.get(key, []):
            found, b = object_variant(sub, base)
            if found is not None:
                return found, b
    return None, base


def array_items(node, base):
    node, base = resolve(node, base)
    if isinstance(node, dict) and "items" in node:
        return node["items"], base
    for key in ("oneOf", "anyOf", "allOf"):
        for sub in node.get(key, []) if isinstance(node, dict) else []:
            items, b = array_items(sub, base)
            if items is not None:
                return items, b
    return None, base


def expand_defaults(value, node, base, sparse=True):
    """Fills missing keys with the schema's defaults, as every decoder must. Sparse (x-inherit) objects are left
    alone unless sparse is False (the bottom layer of ProfileResolver)."""
    obj, obase = object_variant(node, base)
    if isinstance(value, dict) and obj is not None:
        if sparse and (obj.get("x-inherit") or _inherits(node, base)):
            return value
        out = dict(value)
        for key, prop in obj.get("properties", {}).items():
            pnode, pbase = resolve(prop, obase)
            if key not in out:
                default = prop.get("default", pnode.get("default") if isinstance(pnode, dict) else None)
                if "default" in prop or (isinstance(pnode, dict) and "default" in pnode):
                    out[key] = copy.deepcopy(default)
                elif isinstance(pnode, dict) and _is_object(prop, obase) and not _nullable(prop, obase):
                    out[key] = {}
                else:
                    continue
            out[key] = expand_defaults(out[key], prop, obase, sparse)
        if obj.get("x-default-background") and "background" not in value:
            out["background"] = out.get("kind") == "handbrake"
        return out
    if isinstance(value, list):
        items, ibase = array_items(node, base)
        if items is not None:
            return [expand_defaults(v, items, ibase, sparse) for v in value]
    return value


def _inherits(node, base):
    n, b = resolve(node, base)
    if isinstance(n, dict):
        if n.get("x-inherit"):
            return True
        for sub in n.get("allOf", []):
            if _inherits(sub, b):
                return True
    return False


def _is_object(node, base):
    n, _ = resolve(node, base)
    return isinstance(n, dict) and (n.get("type") == "object" or "properties" in n)


def _nullable(node, base):
    n, b = resolve(node, base)
    if not isinstance(n, dict):
        return False
    t = n.get("type")
    if t == "null" or (isinstance(t, list) and "null" in t):
        return True
    return any(_nullable(s, b) for s in n.get("oneOf", []) + n.get("anyOf", []))


def schema_path_exists(name, definition, dotted):
    """Whether Def:a.b[].c is a path of the schema (a trailing .* means a map's keys)."""
    sid = schema_id(name)
    node, base = ({"$ref": "#"}, sid) if definition in (None, "Config") else ({"$ref": f"#/$defs/{definition}"}, sid)
    parts = [p for p in re.split(r"\.", dotted) if p]
    for part in parts:
        if part == "*":
            n, _ = resolve(node, base)
            return isinstance(n, dict) and ("additionalProperties" in n or "properties" in n)
        is_array = part.endswith("[]")
        key = part[:-2] if is_array else part
        obj, obase = object_variant(node, base)
        if obj is None or key not in obj.get("properties", {}):
            return False
        node, base = obj["properties"][key], obase
        if is_array:
            node, base = array_items(node, base)
            if node is None:
                return False
    return True


# ---- config -----------------------------------------------------------------

V2_TYPES = {
    "Config": {"defaultDrive": "DriveConfig", "drives[]": "DriveConfig", "presets[].config": "DriveConfig",
               "plugins[]": "PostProcessStep"},
    "DriveConfig": {"postProcess[]": "PostProcessStep"},
}
V2_MAPS = {"Config": {"globalSettings"}, "DriveConfig": {"settings", "rip.formatModes"}, "PostProcessStep": {"environment"}}


def v2_paths(value, typ="Config", prefix=""):
    """Leaf paths of a v2 document, typed like the migration map: DriveConfig:rip.mode."""
    out = set()
    if isinstance(value, dict):
        for k, v in value.items():
            p = f"{prefix}.{k}" if prefix else k
            sub = V2_TYPES.get(typ, {})
            if p in V2_MAPS.get(typ, set()):
                out.add(f"{typ}:{p}.*")
                continue
            if p in sub:
                out.add(f"{typ}:{p}")
                out |= v2_paths(v, sub[p], "")
                continue
            if isinstance(v, list):
                ap = p + "[]"
                if ap in sub:
                    out.add(f"{typ}:{ap}")
                    for item in v:
                        out |= v2_paths(item, sub[ap], "")
                    continue
                if v and isinstance(v[0], dict):
                    for item in v:
                        out |= v2_paths(item, typ, ap)
                    continue
                out.add(f"{typ}:{p}")
                continue
            if isinstance(v, dict):
                out |= v2_paths(v, typ, p)
            else:
                out.add(f"{typ}:{p}")
    return out


def check_config():
    cfg_dir = FIXTURES / "config"
    for p in sorted(cfg_dir.glob("config3-*.json")):
        validate(load_json(p), "config-3.json", what=rel(p))
    # The fully expanded default document.
    defaults = expand_defaults({"version": 3}, {"$ref": "#"}, schema_id("config-3.json"))
    generated(cfg_dir / "config3-defaults.json", dump_json(defaults))
    validate(defaults, "config-3.json", what="expanded defaults")
    # The bottom layer of ProfileResolver.resolve: every profile field at its default.
    bottom = expand_defaults({"id": "default", "name": "Default"}, {"$ref": "#/$defs/Profile"}, schema_id("config-3.json"), sparse=False)
    generated(cfg_dir / "profile3-defaults.json", dump_json(bottom))
    validate(bottom, "config-3.json", "Profile", what="profile defaults")
    # Migration cases.
    cases = load_json(cfg_dir / "config-migration-cases.json")
    for c in cases["cases"]:
        what = f"config-migration-cases.json: {c['what'][:50]}"
        expect = load_json(cfg_dir / c["expect"]) if "expect" in c else c.get("expectJson")
        if R.check(expect is not None, f"{what}: no expected document"):
            validate(expect, "config-3.json", what=what)
            refs = set(_secret_names(expect))
            R.check(refs == set(c.get("secrets", {})), f"{what}: secrets {sorted(c.get('secrets', {}))} don't match the SecretRefs {sorted(refs)}")
            _check_references(expect, what)
        R.check(("input" in c) != ("inputJson" in c), f"{what}: give input or inputJson")
        if "input" in c:
            R.check((cfg_dir / c["input"]).exists(), f"{what}: {c['input']} is missing")
        for issue in c.get("issues", []):
            R.check(issue.get("code") in message_codes(), f"{what}: unknown issue code {issue.get('code')}")
    # The key map.
    mig = load_json(SCHEMA / "config-migration-2-3.json")
    froms = {k["from"] for k in mig["keys"]}
    for k in mig["keys"]:
        to = k.get("to")
        if to is None:
            R.check("note" in k, f"migration: {k['from']} is dropped without a note")
            continue
        definition, _, path = to.partition(":")
        R.check(schema_path_exists("config-3.json", definition, path), f"migration: {k['from']} → {to}: no such v3 key")
    v2 = load_json(cfg_dir / "config2-everything.json")
    for p in sorted(v2_paths(v2)):
        if p.startswith("Config:presets[].") and p not in froms:
            continue
        R.check(p in froms, f"migration: v2 key {p} (in config2-everything.json) has no entry")
    for f in sorted(froms):
        R.check(f in v2_paths(v2) or f.endswith("[]") or f in ("Config:defaultDrive",),
                f"migration: {f} isn't a key of config2-everything.json (add it there so the map is checked)")


def _secret_names(node):
    if isinstance(node, dict):
        if set(node) == {"secret"}:
            yield node["secret"]
        for v in node.values():
            yield from _secret_names(v)
    elif isinstance(node, list):
        for v in node:
            yield from _secret_names(v)


def _check_references(doc, what):
    """Ids that a v3 document refers to must exist."""
    ids = {k: {e["id"] for e in doc.get(k, [])} for k in ("libraries", "profiles", "drives", "steps", "targets")}
    def need(kind, value, where):
        if value is not None:
            R.check(value in ids[kind], f"{what}: {where} refers to the unknown {kind[:-1]} '{value}'")
    need("libraries", doc.get("defaultLibrary"), "defaultLibrary")
    need("profiles", doc.get("defaultProfile", "default"), "defaultProfile")
    for p in doc.get("profiles", []):
        need("libraries", p.get("library"), f"profile {p['id']}")
        for s in p.get("steps", []):
            need("steps", s, f"profile {p['id']}")
        for t in p.get("archive", {}).get("replicateTo", []):
            need("targets", t, f"profile {p['id']}")
    for d in doc.get("drives", []):
        need("profiles", d.get("profile"), f"drive {d['id']}")
    for r in doc.get("rules", []):
        for s in r["then"].get("steps", []):
            need("steps", s, f"rule {r['id']}")
        need("profiles", r["then"].get("profile"), f"rule {r['id']}")
        for p in r.get("when", {}).get("profiles", []):
            need("profiles", p, f"rule {r['id']}")
        for d in r.get("when", {}).get("drives", []):
            need("drives", d, f"rule {r['id']}")
    for kind in ids:
        entries = [e["id"] for e in doc.get(kind, [])]
        R.check(len(entries) == len(set(entries)), f"{what}: duplicate {kind} ids")



# ---- ICU MessageFormat (the subset en.json uses) ------------------------------

class IcuError(ValueError):
    pass


def icu_parse(text):
    """Parses an ICU message into a list of arguments: (name, type, style, branches). Raises IcuError.
    Supports {x}, {x, number|bytes|duration|durationPrecise}, {x, plural, =0 {…} one {…} other {…}} and
    {x, select, key {…} other {…}}. A single apostrophe is literal unless it quotes { or } (ICU's default)."""
    args = []
    pos = _icu_message(text, 0, args, in_plural=False, depth=0)
    if pos != len(text):
        raise IcuError(f"unexpected '{text[pos]}' at {pos}")
    return args


def _icu_message(t, i, args, in_plural, depth):
    while i < len(t):
        c = t[i]
        if c == "'" and i + 1 < len(t) and t[i + 1] in "{}'":
            end = t.find("'", i + 2) if t[i + 1] != "'" else i + 1
            if end < 0:
                raise IcuError("unterminated quote")
            i = end + 1
        elif c == "{":
            i = _icu_arg(t, i + 1, args, depth)
        elif c == "}":
            return i
        else:
            i += 1
    return i


def _icu_arg(t, i, args, depth):
    m = re.match(r"\s*([A-Za-z][A-Za-z0-9]*)\s*", t[i:])
    if not m:
        raise IcuError(f"argument name expected at {i}")
    name = m.group(1)
    i += m.end()
    if i < len(t) and t[i] == "}":
        args.append((name, None, None, None))
        return i + 1
    if i >= len(t) or t[i] != ",":
        raise IcuError(f"',' or '}}' expected after {name}")
    m = re.match(r"\s*([A-Za-z]+)\s*", t[i + 1:])
    if not m:
        raise IcuError(f"format expected after {name}")
    kind = m.group(1)
    i += 1 + m.end()
    if kind in ("plural", "select", "selectordinal"):
        if i >= len(t) or t[i] != ",":
            raise IcuError(f"{name}: branches expected")
        i += 1
        branches = {}
        while True:
            m = re.match(r"\s*(=\d+|[A-Za-z][A-Za-z0-9_]*)\s*", t[i:])
            if not m:
                break
            key = m.group(1)
            i += m.end()
            if i >= len(t) or t[i] != "{":
                raise IcuError(f"{name}: '{{' expected after branch {key}")
            sub = []
            end = _icu_message(t, i + 1, sub, in_plural=kind != "select", depth=depth + 1)
            if end >= len(t) or t[end] != "}":
                raise IcuError(f"{name}: branch {key} isn't closed")
            branches[key] = sub
            for a in sub:
                args.append(a)
            i = end + 1
        while i < len(t) and t[i].isspace():
            i += 1
        if i >= len(t) or t[i] != "}":
            raise IcuError(f"{name}: '}}' expected after the branches")
        if "other" not in branches:
            raise IcuError(f"{name}: a {kind} needs an 'other' branch")
        args.append((name, kind, None, branches))
        return i + 1
    style = None
    if i < len(t) and t[i] == ",":
        m = re.match(r"\s*([^}]*)", t[i + 1:])
        style = m.group(1).strip()
        i += 1 + m.end()
    if i >= len(t) or t[i] != "}":
        raise IcuError(f"{name}: '}}' expected")
    args.append((name, kind, style, None))
    return i + 1


def icu_argument_names(text):
    return {a[0] for a in icu_parse(text)}


# ---- messages (used by other sections) -------------------------------------

_codes = None


def message_codes():
    global _codes
    if _codes is None:
        p = SHARED / "messages" / "codes.json"
        _codes = load_json(p)["codes"] if p.exists() else {}
    return _codes


CODE_RE = re.compile(r"^[a-z][a-zA-Z0-9]*(\.[a-z][a-zA-Z0-9]*)+$")
FORMATS_FOR = {"plural": {"int"}, "select": {"string", "bool"}, "number": {"int"}, "bytes": {"bytes"},
               "duration": {"duration"}, "durationPrecise": {"duration"}, None: None}


def check_messages():
    doc = load_json(SHARED / "messages" / "codes.json")
    codes = doc["codes"]
    kinds, types = set(doc["kinds"]), set(doc["paramTypes"])
    for lang_file in sorted((SHARED / "messages").glob("*.json")):
        if lang_file.name == "codes.json":
            continue
        texts = load_json(lang_file)
        missing, extra = set(codes) - set(texts), set(texts) - set(codes)
        R.check(not missing, f"{lang_file.name}: no text for {sorted(missing)[:10]}")
        R.check(not extra, f"{lang_file.name}: text for unknown codes {sorted(extra)[:10]}")
        for code, text in texts.items():
            if code not in codes:
                continue
            try:
                args = icu_parse(text)
            except IcuError as e:
                R.error(f"{lang_file.name}: {code}: {e}")
                continue
            params = codes[code].get("params", {})
            names = {a[0] for a in args}
            R.check(names == set(params), f"{lang_file.name}: {code} uses {sorted(names)}, codes.json declares {sorted(params)}")
            for name, kind, style, branches in args:
                ptype = params.get(name)
                allowed = FORMATS_FOR.get(kind, "?")
                if allowed == "?":
                    R.error(f"{lang_file.name}: {code}: unknown format {kind}")
                elif allowed is not None and ptype not in allowed:
                    R.error(f"{lang_file.name}: {code}: {{{name}, {kind}}} needs a {'/'.join(sorted(allowed))} parameter, not {ptype}")
                if kind is None and ptype in ("bytes", "duration"):
                    R.error(f"{lang_file.name}: {code}: {name} is {ptype}; render it with {{{name}, {ptype}}}")
                if kind == "select" and ptype == "bool":
                    R.check(set(branches) <= {"true", "false", "other"}, f"{lang_file.name}: {code}: a bool select takes true / false / other")
    for code, e in codes.items():
        R.check(bool(CODE_RE.match(code)), f"codes.json: {code} isn't a valid code")
        R.check(e.get("kind") in kinds, f"codes.json: {code}: unknown kind {e.get('kind')}")
        for name, t in {**e.get("params", {}), **e.get("data", {})}.items():
            R.check(t in types, f"codes.json: {code}: {name} has the unknown type {t}")
        R.check(not set(e.get("params", {})) & set(e.get("data", {})), f"codes.json: {code}: a parameter is both shown and data")
        if "http" in e:
            R.check(e["kind"] in ("error", "problem", "issue"), f"codes.json: {code}: only errors have an HTTP status")
            R.check(e["http"] in (400, 401, 403, 404, 405, 409, 422, 500, 503, 507), f"codes.json: {code}: unusual HTTP status {e['http']}")
        if "severity" in e:
            R.check(e["severity"] in ("debug", "info", "warning", "error"), f"codes.json: {code}: bad severity")
    # Every code that another contract mentions must exist.
    for p in sorted(list(SHARED.rglob("*.json")) + list(SHARED.rglob("*.yaml"))):
        if p.parent == SHARED / "messages" or "catalog" in p.parts or "web" in p.parts:
            continue
        doc = load_yaml(p) if p.suffix == ".yaml" else load_json(p)
        for code in _codes_in(doc):
            R.check(code in codes, f"{rel(p)}: unknown message code {code}")


def _codes_in(node, key=None):
    """Values of 'code' keys (and of keys ending in 'Code', 'reason' objects) that look like message codes."""
    if isinstance(node, dict):
        for k, v in node.items():
            if k in ("code", "errorCode", "problem", "operation", "blockedBy") and isinstance(v, str) and CODE_RE.match(v):
                yield v
            elif k in ("code", "errorCode", "problem", "operation", "blockedBy", "log", "logs", "noLog") and isinstance(v, list):
                for item in v:
                    if isinstance(item, str) and CODE_RE.match(item):
                        yield item
                    else:
                        yield from _codes_in(item, k)
            else:
                yield from _codes_in(v, k)
    elif isinstance(node, list):
        for v in node:
            yield from _codes_in(v, key)


# ---- fixtures ---------------------------------------------------------------

def check_fixtures():
    seen = {}
    for p in sorted(FIXTURES.rglob("*")):
        if p.is_file() and p.name != ".DS_Store":
            if p.name in seen:
                R.error(f"{rel(p)} and {rel(seen[p.name])} have the same name; the macOS test bundle flattens folders")
            seen[p.name] = p
        if p.is_file() and p.suffix == ".json":
            try:
                load_json(p)
            except json.JSONDecodeError as e:
                R.error(f"{rel(p)}: {e}")


# ---- archive records and manifests ------------------------------------------

def check_documents():
    arch = FIXTURES / "archive"
    for p in sorted(arch.glob("record3-*.json")):
        doc = load_json(p)
        validate(doc, "archive-record-3.json", what=rel(p))
        for prob in doc.get("problems", []):
            R.check(prob["code"] in message_codes(), f"{rel(p)}: unknown code {prob['code']}")
        R.check(doc["unit"]["short"] == doc["unit"]["id"][:8], f"{rel(p)}: unit.short isn't the first 8 digits of unit.id")
        listed = {f["path"] for f in doc["files"]}
        for e in doc["episodes"]:
            R.check(e["file"] in listed, f"{rel(p)}: episode file {e['file']} isn't in files")
    for p in sorted(arch.glob("record2-*.json")):
        validate(load_json(p), "archive-record-2.json", what=rel(p))
    for r in load_json(FIXTURES / "episode-continuation.json")["records"]:
        validate(r["record"], "archive-record-2.json", what=f"episode-continuation.json {r['path']}")
    for p in sorted(arch.glob("manifest2-*.json")):
        validate(load_json(p), "manifest-2.json", what=rel(p))


SECTIONS = {
    "schemas": check_schemas,
    "config": check_config,
    "messages": check_messages,
    "documents": check_documents,
    "fixtures": check_fixtures,
}


def main():
    ap = argparse.ArgumentParser(description=__doc__, formatter_class=argparse.RawDescriptionHelpFormatter)
    ap.add_argument("--write", action="store_true", help="regenerate generated files")
    ap.add_argument("--only", help="comma separated sections: " + ", ".join(SECTIONS))
    args = ap.parse_args()
    R.write = args.write
    names = args.only.split(",") if args.only else list(SECTIONS)
    for name in names:
        if name not in SECTIONS:
            sys.exit(f"unknown section {name}")
        R.section = name
        SECTIONS[name]()
    for e in R.errors:
        print(e)
    print(f"check-contracts: {len(names)} section(s), {len(R.errors)} problem(s)")
    return 1 if R.errors else 0


if __name__ == "__main__":
    sys.exit(main())
