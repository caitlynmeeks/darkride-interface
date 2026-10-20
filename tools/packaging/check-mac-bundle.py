#!/usr/bin/env python3
#
#  check-mac-bundle.py
#  tools/packaging
#
#  Checks that a packaged Darkride.app stands on its own (RELEASING.md, macOS): every library a Mach-O file in it
#  loads resolves inside the app or is part of macOS, and no search path (LC_RPATH) points outside the app.
#  Exits 1 on any miss, so an automated build can stop there.
#    python3 tools/packaging/check-mac-bundle.py stage/Darkride.app
#
#  Distributed under the Apache License, Version 2.0.
#  See the accompanying file LICENSE or http://www.apache.org/licenses/LICENSE-2.0.html
#

import os
import subprocess
import sys

MACH_O_MAGICS = (b"\xcf\xfa\xed\xfe", b"\xca\xfe\xba\xbe")  # 64-bit Mach-O, universal
SYSTEM_PREFIXES = ("/System/", "/usr/lib/")
LOAD_COMMANDS = ("LC_LOAD_DYLIB", "LC_LOAD_WEAK_DYLIB", "LC_REEXPORT_DYLIB")


def mach_o_files(app):
    for root, _, files in os.walk(app):
        for name in files:
            path = os.path.join(root, name)
            if os.path.islink(path):
                continue
            with open(path, "rb") as f:
                if f.read(4) in MACH_O_MAGICS:
                    yield path


def load_commands(path):
    """The libraries a file loads and its search paths, from otool -l."""
    dependencies, search_paths, command = [], [], None
    for line in subprocess.run(["otool", "-l", path], capture_output=True, text=True).stdout.splitlines():
        line = line.strip()
        if line.startswith("cmd "):
            command = line.split()[1]
        elif line.startswith("name ") and command in LOAD_COMMANDS:
            dependencies.append(line.split()[1])
        elif line.startswith("path ") and command == "LC_RPATH":
            search_paths.append(line.split()[1])
    return dependencies, search_paths


def main():
    if len(sys.argv) != 2 or not sys.argv[1].endswith(".app"):
        sys.exit("usage: check-mac-bundle.py path/to/Darkride.app")
    app = os.path.abspath(sys.argv[1])
    executable_dir = os.path.join(app, "Contents", "MacOS")
    problems, count = [], 0
    for path in mach_o_files(app):
        count += 1
        where = os.path.relpath(path, app)
        dependencies, search_paths = load_commands(path)

        def expand(p):
            return p.replace("@executable_path", executable_dir).replace("@loader_path", os.path.dirname(path))

        for search_path in search_paths:
            if not search_path.startswith("@"):
                problems.append(f"search path outside the app: {search_path}  ({where})")
        bundle_search_paths = [expand(p) for p in search_paths if p.startswith("@")]
        for dependency in dependencies:
            if dependency.startswith(SYSTEM_PREFIXES):
                continue
            if dependency.startswith("@rpath/"):
                # Only the file's own search paths count. dyld also tries those of whatever loaded it (the main
                # executable, say), so this is stricter than macOS: right for an app packaged as in RELEASING.md,
                # where every Mach-O carries its own, but a bundle packaged another way can fail here and still run.
                found = any(os.path.exists(os.path.join(p, dependency[len("@rpath/"):])) for p in bundle_search_paths)
            elif dependency.startswith(("@executable_path", "@loader_path")):
                found = os.path.exists(expand(dependency))
            else:
                found = False  # an absolute path outside macOS: on someone else's Mac it isn't there
            if not found:
                problems.append(f"unresolved: {dependency}  ({where})")
    print(f"{count} Mach-O files, {len(problems)} problems")
    for problem in problems:
        print("  " + problem)
    sys.exit(1 if problems else 0)


if __name__ == "__main__":
    main()
