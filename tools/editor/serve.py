#!/usr/bin/env python3
"""Local dev server for the level editor.

Serves tools/editor/ and exposes the three things a browser cannot do for
itself: list the level files, read one, write one back, and run the game's own
validator over the result.

    tools/editor/serve.py            # prints the URL it actually bound to

Binds the loopback interface only. It writes into assets/levels/ and runs the
game binary, so it is a development tool and not something to expose: the level
name in a request is matched against a strict pattern and joined to
assets/levels/ here rather than taken as a path, and the binary's argv is built
from that resolved path, never from request text.
"""

import argparse
import http.server
import json
import mimetypes
import os
import re
import subprocess
import sys
import urllib.parse
from pathlib import Path

ROOT = Path(__file__).resolve().parent.parent.parent
EDITOR = ROOT / "tools" / "editor"
LEVELS = ROOT / "assets" / "levels"
MODELS = ROOT / "assets" / "models"
CONFIG = ROOT / "assets" / "config"

# A level, model or config name is a bare stem: no separators, no dots, no
# traversal. The extension is supplied here, never by the request.
LEVEL_NAME = re.compile(r"^[A-Za-z0-9][A-Za-z0-9_-]{0,63}$")

# Where the Linux dev build puts the binary, in the order worth trying.
BINARIES = ["build/Debug/descent", "build/Release/descent"]

# The flags that need no window and no GL, which is all of them that matter
# here: the server may be running over ssh with no display at all.
#
# The third field is the level a check was WRITTEN FOR, or None for one that
# judges any level. --loop-test is the awkward case: despite the name it is
# mine01's regression test, and src/game/loop_test.cpp asserts the level has
# exactly three keyed doors and a gunner. Run against anything else it neither
# skips (it skips only when there are no doors or no exit) nor passes, so its
# verdict says nothing about whether the level is good. Reporting that as a
# failure would train the author to ignore the one check that catches a mine
# you cannot finish.
CHECKS = [
    ("structure", ["--world-test"], None),
    ("collision", ["--collision-test"], None),
    ("loop", ["--loop-test"], "mine01"),
]


def level_path(name):
    """assets/levels/<name>.json, or None if the name is not a bare stem."""
    if not LEVEL_NAME.match(name or ""):
        return None
    return LEVELS / f"{name}.json"


def asset_path(directory, name, suffix):
    """<directory>/<name><suffix>, or None if the name is not a bare stem."""
    if not LEVEL_NAME.match(name or ""):
        return None
    return directory / f"{name}{suffix}"


def find_binary():
    for candidate in BINARIES:
        path = ROOT / candidate
        if path.is_file() and os.access(path, os.X_OK):
            return path
    return None


class Handler(http.server.BaseHTTPRequestHandler):
    server_version = "descent-editor"

    def log_message(self, fmt, *args):
        sys.stderr.write(f"  {self.address_string()} {fmt % args}\n")

    # --- plumbing ---------------------------------------------------------

    def send_json(self, status, payload):
        body = json.dumps(payload).encode()
        self.send_response(status)
        self.send_header("Content-Type", "application/json")
        self.send_header("Content-Length", str(len(body)))
        self.end_headers()
        self.wfile.write(body)

    def send_bytes(self, status, body, content_type):
        self.send_response(status)
        self.send_header("Content-Type", content_type)
        self.send_header("Content-Length", str(len(body)))
        # The editor is edited while it is open. Nothing here is worth caching.
        self.send_header("Cache-Control", "no-store")
        self.end_headers()
        self.wfile.write(body)

    def read_body(self):
        length = int(self.headers.get("Content-Length") or 0)
        if length <= 0 or length > 32 * 1024 * 1024:
            return None
        return self.rfile.read(length)

    # --- routes -----------------------------------------------------------

    def do_GET(self):
        path = urllib.parse.urlparse(self.path).path

        if path == "/api/levels":
            names = sorted(p.stem for p in LEVELS.glob("*.json"))
            return self.send_json(200, {"levels": names, "binary": str(find_binary() or "")})

        if path == "/api/models":
            names = sorted(p.stem for p in MODELS.glob("*.obj"))
            return self.send_json(200, {"models": names})

        if path.startswith("/api/models/"):
            resolved = asset_path(MODELS, path[len("/api/models/"):], ".obj")
            if resolved is None or not resolved.is_file():
                return self.send_json(404, {"error": "no such model"})
            return self.send_bytes(200, resolved.read_bytes(), "text/plain")

        if path.startswith("/api/config/"):
            resolved = asset_path(CONFIG, path[len("/api/config/"):], ".json")
            if resolved is None or not resolved.is_file():
                return self.send_json(404, {"error": "no such config"})
            return self.send_bytes(200, resolved.read_bytes(), "application/json")

        if path.startswith("/api/levels/"):
            resolved = level_path(path[len("/api/levels/"):])
            if resolved is None:
                return self.send_json(400, {"error": "not a level name"})
            if not resolved.is_file():
                return self.send_json(404, {"error": f"no such level: {resolved.name}"})
            return self.send_bytes(200, resolved.read_bytes(), "application/json")

        return self.serve_static(path)

    def do_PUT(self):
        path = urllib.parse.urlparse(self.path).path
        if not path.startswith("/api/levels/"):
            return self.send_json(404, {"error": "not a route"})

        resolved = level_path(path[len("/api/levels/"):])
        if resolved is None:
            return self.send_json(400, {"error": "not a level name"})

        body = self.read_body()
        if body is None:
            return self.send_json(400, {"error": "empty or oversized body"})

        # Parse before writing. A save that leaves unreadable JSON in
        # assets/levels/ makes every later game start fatal, editor or not.
        try:
            json.loads(body)
        except json.JSONDecodeError as e:
            return self.send_json(400, {"error": f"not valid JSON: {e}"})

        # Write beside the target and rename, so an interrupted save leaves the
        # previous level intact rather than half a file.
        temp = resolved.with_suffix(".json.tmp")
        temp.write_bytes(body)
        temp.replace(resolved)
        return self.send_json(200, {"wrote": str(resolved.relative_to(ROOT)), "bytes": len(body)})

    def do_POST(self):
        path = urllib.parse.urlparse(self.path).path
        if path != "/api/verify":
            return self.send_json(404, {"error": "not a route"})

        body = self.read_body() or b"{}"
        try:
            name = json.loads(body).get("level")
        except json.JSONDecodeError:
            return self.send_json(400, {"error": "not valid JSON"})

        resolved = level_path(name)
        if resolved is None or not resolved.is_file():
            return self.send_json(400, {"error": "not a level name"})

        binary = find_binary()
        if binary is None:
            return self.send_json(503, {
                "error": "no game binary found. Build it with: "
                         "cmake --preset linux && cmake --build --preset linux-debug",
            })

        results = []
        for label, flags, written_for in CHECKS:
            argv = [str(binary), *flags, "--level", str(resolved)]
            try:
                done = subprocess.run(argv, cwd=ROOT, capture_output=True, text=True, timeout=180)
                output, code = done.stdout + done.stderr, done.returncode
            except subprocess.TimeoutExpired:
                output, code = "timed out after 180s", -1
            # Two different things hide behind a non-zero exit, and the
            # difference is the one an author needs. A [FATAL] means the game
            # could not load the level at all - that is broken and nothing else
            # matters. Everything else is an assertion that did not hold, and
            # some of those are about the level being too small to exercise the
            # system under test ("no segment is dark", "portal culling is doing
            # nothing") rather than about the level being wrong. So the abort
            # is separated out and the objections are passed through verbatim
            # for the author to read.
            aborted = "[FATAL]" in output
            notes = [line.split("[ERROR] ", 1)[-1].strip()
                     for line in output.splitlines() if "[ERROR]" in line]
            results.append({
                "check": label,
                "argv": argv[1:],
                "exit": code,
                "output": output,
                "aborted": aborted,
                "notes": notes,
                "advisory": written_for is not None and written_for != name,
                "writtenFor": written_for,
            })

        return self.send_json(200, {"level": name, "results": results})

    # --- static files -----------------------------------------------------

    def serve_static(self, path):
        relative = path.lstrip("/") or "index.html"
        target = (EDITOR / relative).resolve()
        # Resolve first, then confirm the result is still inside tools/editor.
        # Checking the requested path instead would follow ../ and symlinks
        # straight back out.
        try:
            target.relative_to(EDITOR)
        except ValueError:
            return self.send_json(404, {"error": "not found"})
        if not target.is_file():
            return self.send_json(404, {"error": "not found"})

        kind, _ = mimetypes.guess_type(target.name)
        if target.suffix in (".js", ".mjs"):
            kind = "text/javascript"
        return self.send_bytes(200, target.read_bytes(), kind or "application/octet-stream")


# Not 8080. That one is already taken on a lot of machines - llama.cpp's web UI
# and half the dev servers ever written default to it - and the failure is
# nasty: the editor does not start, the browser shows whatever does own 8080,
# and it looks like the editor is broken rather than absent.
DEFAULT_PORT = 8137
PORTS_TO_TRY = 12


def serve(host, port, explicit):
    """Bind before announcing anything, so the URL printed is the real one.

    When the port was not asked for by name, walk forward a few and say which
    one it landed on. When it WAS asked for by name, fail on that port rather
    than quietly serving somewhere the caller is not looking.
    """
    last = None
    for candidate in range(port, port + (1 if explicit else PORTS_TO_TRY)):
        try:
            return http.server.ThreadingHTTPServer((host, candidate), Handler)
        except OSError as e:
            last = e
            continue

    if explicit:
        raise SystemExit(
            f"cannot listen on {host}:{port} - {last.strerror}.\n"
            f"Something else is already using that port; 8080 in particular is usually "
            f"llama.cpp's web UI or another dev server.\n"
            f"Run without --port to take the first free one from {DEFAULT_PORT}, or name a "
            f"different one.")
    raise SystemExit(
        f"cannot listen on any port from {port} to {port + PORTS_TO_TRY - 1} on {host} - "
        f"{last.strerror}.\nName one explicitly with --port.")


def main():
    parser = argparse.ArgumentParser(description=__doc__)
    parser.add_argument("--port", type=int, default=None,
                        help=f"default {DEFAULT_PORT}, or the next free port after it")
    parser.add_argument("--host", default="127.0.0.1",
                        help="loopback by default, and there is no good reason to change it")
    args = parser.parse_args()

    httpd = serve(args.host, args.port or DEFAULT_PORT, explicit=args.port is not None)
    bound = httpd.server_address[1]

    binary = find_binary()
    # Flushed, because the one thing a person needs from this program is the
    # URL, and Python block-buffers stdout the moment it is piped or captured.
    print(f"descent level editor    http://{args.host}:{bound}", flush=True)
    if args.port is None and bound != DEFAULT_PORT:
        print(f"  note     {DEFAULT_PORT} was busy, so this is on {bound}", flush=True)
    print(f"  levels   {LEVELS.relative_to(ROOT)}", flush=True)
    print(f"  verify   {binary.relative_to(ROOT) if binary else 'no build found - Verify disabled'}",
          flush=True)
    try:
        httpd.serve_forever()
    except KeyboardInterrupt:
        print()


if __name__ == "__main__":
    main()
