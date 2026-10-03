#!/usr/bin/env python3
"""Checks the layer rules of rearchitecture plan §4 in the rebuilt code trees.

The layers, their folders on each platform and what each may depend on come from the "layers" section of
shared/architecture-map.json. For every source file in a layer folder:

  C (linux/src/<layer>/)       every #include "…" must resolve to the same layer or one it may depend on;
                               domain/ includes only GLib's core (<glib.h>, not gio); domain/ and engine/ use
                               none of the banned calls (g_file_*, g_spawn*, g_get_*time, fopen, getenv, …).
  Swift (BromeliaKit/Sources/) every `import Bro…` must be an allowed layer; BroDomain and BroEngine use none
                               of the banned APIs (FileManager, Process, Date(), URLSession, DispatchQueue, …).
  C# (windows/src/*/)          every <ProjectReference> must be an allowed layer; Bromelia.Domain has no
                               package references; Domain and Engine use none of the banned APIs (System.IO.File,
                               Directory, Process, HttpClient, DateTime.Now, Environment, Registry). The build
                               also bans them with Microsoft.CodeAnalysis.BannedApiAnalyzers; this is a second net.
  Server and Client            may use the Engine's API module only: no services, Scheduler, PipelineRunner or
                               EventBus.

Layers without code yet are reported and pass. Exit status 1 when a rule is broken.
A layer with "checkFromPhase": N in the map (the UI, whose folders hold today's apps) is skipped until --phase N.
Usage: tools/check-layers.py [--platform swift|cs|c] [--phase N] [--quiet]
"""
import argparse
import json
import re
import sys
from pathlib import Path

ROOT = Path(__file__).resolve().parent.parent

C_BANNED = [r"\bg_file_\w+\s*\(", r"\bg_spawn\w*\s*\(", r"\bg_get_\w*time\s*\(", r"\bfopen\s*\(", r"\bgetenv\s*\(", r"\bg_getenv\s*\(",
            r"\bsystem\s*\(", r"\bpopen\s*\(", r"\bg_subprocess\w*\s*\(", r"\btime\s*\(\s*NULL\s*\)", r"\bg_random_\w+\s*\("]
C_DOMAIN_ONLY = [r"\bg_thread_\w+\s*\(", r"\bg_main_context_\w+\s*\(", r"\bg_idle_add\w*\s*\(", r"\bg_timeout_add\w*\s*\("]
SWIFT_BANNED = [r"\bFileManager\b", r"\bProcess\s*\(", r"\bProcessInfo\b", r"\bDate\s*\(\s*\)", r"\bURLSession\b", r"\bDispatchQueue\b",
                r"\bThread\b", r"\bFileHandle\b", r"\bgetenv\s*\(", r"\bUserDefaults\b", r"\bUUID\s*\(\s*\)", r"\.random\s*\("]
SWIFT_DOMAIN_ONLY = [r"\bTask\s*\{", r"\bTask\.sleep\b"]
CS_BANNED = [r"\bSystem\.IO\.File\b", r"\bFile\.\w+\s*\(", r"\bDirectory\.\w+\s*\(", r"\bProcess\.Start\b", r"\bnew\s+Process\s*\(", r"\bHttpClient\b",
             r"\bDateTime\.(Utc)?Now\b", r"\bDateTimeOffset\.(Utc)?Now\b", r"\bEnvironment\.\w+", r"\bRegistry\b", r"\bGuid\.NewGuid\s*\(", r"\bnew\s+Random\s*\("]
CS_DOMAIN_ONLY = [r"\bTask\.Run\b", r"\bnew\s+Thread\s*\(", r"\bThreadPool\b"]
ENGINE_INTERNALS = r"\b(\w+Service|Scheduler|PipelineRunner|EventBus|EngineExecutor|StepRegistry|JobPlanner)\b"


def strip_comments(text, c_like=True):
    text = re.sub(r"/\*.*?\*/", "", text, flags=re.S)
    text = re.sub(r"//[^\n]*", "", text)
    return re.sub(r'"(?:\\.|[^"\\])*"', '""', text)


class Layers:
    def __init__(self, args):
        self.args = args
        self.all_layers = json.loads((ROOT / "shared" / "architecture-map.json").read_text(encoding="utf-8"))["layers"]
        self.errors, self.notes = [], []
        self.layers = {}
        for name, l in self.all_layers.items():
            if l.get("checkFromPhase", 0) > args.phase:
                self.notes.append(f"{name}: not checked before phase {l['checkFromPhase']} ({l.get('note', '')})")
            else:
                self.layers[name] = l

    def roots(self, layer, platform):
        r = self.layers[layer][platform]
        return [ROOT / p for p in (r if isinstance(r, list) else [r])]

    def allowed(self, layer):
        deps = self.all_layers[layer]["deps"]
        if "*" in deps:
            return set(self.all_layers)
        out = {layer} | {d for d in deps if d in self.layers}
        if "engine-api" in deps:
            out.add("engine")
        return out

    def layer_of(self, path, platform):
        for name in self.layers:
            for r in self.roots(name, platform):
                try:
                    path.relative_to(r)
                    return name
                except ValueError:
                    continue
        return None

    def banned(self, text, patterns, rel, what):
        for pat in patterns:
            for m in re.finditer(pat, text):
                line = text.count("\n", 0, m.start()) + 1
                self.errors.append(f"{rel}:{line}: {what} uses {m.group(0).strip()}")

    def check_files(self, platform, layer, files):
        if not files:
            self.notes.append(f"{platform} {layer}: no code yet")

    # ---- C ----------------------------------------------------------------------------------------
    def c(self):
        headers = {}
        for name in self.layers:
            for r in self.roots(name, "c"):
                if r.exists():
                    for h in r.rglob("*.h"):
                        headers.setdefault(h.name, []).append((name, h))
        for name in self.layers:
            files = [f for r in self.roots(name, "c") if r.exists() for f in r.rglob("*") if f.suffix in (".c", ".h")]
            self.check_files("c", name, files)
            for f in files:
                rel = f.relative_to(ROOT)
                raw = f.read_text(encoding="utf-8", errors="replace")
                for m in re.finditer(r'^\s*#\s*include\s+"([^"]+)"', raw, re.M):
                    target = Path(m.group(1)).name
                    owners = {n for n, _ in headers.get(target, [])}
                    if owners and not owners & self.allowed(name):
                        self.errors.append(f"{rel}: #include \"{m.group(1)}\" reaches up into {', '.join(sorted(owners))}")
                if name == "domain":
                    for m in re.finditer(r"^\s*#\s*include\s+<([^>]+)>", raw, re.M):
                        inc = m.group(1)
                        if not (inc in ("glib.h", "string.h", "stdlib.h", "stdint.h", "stdbool.h", "stddef.h", "limits.h", "math.h", "ctype.h", "stdio.h")
                                or inc.startswith("glib/")):
                            self.errors.append(f"{rel}: domain includes <{inc}> (GLib's core only)")
                if name in ("domain", "engine"):
                    text = strip_comments(raw)
                    self.banned(text, C_BANNED + (C_DOMAIN_ONLY if name == "domain" else []), rel, name)
                if name in ("server", "client"):
                    self.banned(strip_comments(raw), [ENGINE_INTERNALS.replace(r"\w+Service", r"bro_\w+_service\w*")], rel, f"{name} (Engine API only)")

    # ---- Swift --------------------------------------------------------------------------------------
    def swift(self):
        targets = {}
        for name in self.layers:
            for r in self.roots(name, "swift"):
                targets[r.name] = name
        for name in self.layers:
            files = [f for r in self.roots(name, "swift") if r.exists() for f in r.rglob("*.swift")]
            self.check_files("swift", name, files)
            for f in files:
                rel = f.relative_to(ROOT)
                raw = f.read_text(encoding="utf-8", errors="replace")
                for m in re.finditer(r"^\s*(?:@testable\s+)?import\s+(\w+)", raw, re.M):
                    mod = m.group(1)
                    if mod in targets and targets[mod] not in self.allowed(name):
                        self.errors.append(f"{rel}: imports {mod} ({targets[mod]}), which {name} may not use")
                text = strip_comments(raw)
                if name in ("domain", "engine"):
                    self.banned(text, SWIFT_BANNED + (SWIFT_DOMAIN_ONLY if name == "domain" else []), rel, name)
                if name in ("server", "client", "ui"):
                    self.banned(text, [ENGINE_INTERNALS], rel, f"{name} (Engine API only)")

    # ---- C# -----------------------------------------------------------------------------------------
    def cs(self):
        projects = {}
        for name in self.layers:
            for r in self.roots(name, "cs"):
                projects[r.name] = name
        for name in self.layers:
            for r in self.roots(name, "cs"):
                if not r.exists():
                    self.notes.append(f"cs {name}: no code yet ({r.relative_to(ROOT)})")
                    continue
                for proj in r.glob("*.csproj"):
                    text = proj.read_text(encoding="utf-8", errors="replace")
                    for m in re.finditer(r'<ProjectReference\s+Include="([^"]+)"', text):
                        ref = Path(m.group(1).replace("\\", "/")).stem
                        if ref in projects and projects[ref] not in self.allowed(name):
                            self.errors.append(f"{proj.relative_to(ROOT)}: references {ref} ({projects[ref]}), which {name} may not use")
                    if name == "domain" and "<PackageReference" in text:
                        self.errors.append(f"{proj.relative_to(ROOT)}: Bromelia.Domain may not have package references")
                files = [f for f in r.rglob("*.cs") if "/obj/" not in f.as_posix() and "/bin/" not in f.as_posix()]
                for f in files:
                    rel = f.relative_to(ROOT)
                    text = strip_comments(f.read_text(encoding="utf-8", errors="replace"))
                    if name in ("domain", "engine"):
                        self.banned(text, CS_BANNED + (CS_DOMAIN_ONLY if name == "domain" else []), rel, name)
                    if name in ("server", "client", "ui"):
                        self.banned(text, [ENGINE_INTERNALS], rel, f"{name} (Engine API only)")

    def run(self):
        for p in [self.args.platform] if self.args.platform else ["swift", "cs", "c"]:
            getattr(self, p)()
        if not self.args.quiet:
            for n in self.notes:
                print(f"note: {n}")
        for e in self.errors:
            print(f"layer rule broken: {e}")
        print(f"check-layers: {len(self.errors)} problem(s); {len(self.notes)} layer folder(s) without code yet")
        return 1 if self.errors else 0


def main():
    ap = argparse.ArgumentParser(description=__doc__, formatter_class=argparse.RawDescriptionHelpFormatter)
    ap.add_argument("--platform", choices=["swift", "cs", "c"])
    ap.add_argument("--phase", type=int, default=0, help="the build phase reached (layers checked from a later phase are skipped)")
    ap.add_argument("--quiet", action="store_true")
    return Layers(ap.parse_args()).run()


if __name__ == "__main__":
    sys.exit(main())
