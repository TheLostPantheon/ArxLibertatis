#!/usr/bin/env python3
"""
MCP server (stdio) for Arx Libertatis on the Vita: VitaShell FTP access plus
the benchmark loop (scripts/vita/vita_bench.py).

Tools: list a directory, read/grep a log, download, upload, delete, deploy an
eboot, record a route, run a benchmark replay and show past results. Every
overwrite or delete first copies the remote file into
~/Dev/vita/device-backups/auto/<timestamp>/ so nothing on the device is lost.

No dependencies beyond the standard library. Configure with env vars:
  VITA_FTP_HOST (default: the last address found, else 192.168.0.48; a
  failed connect scans the /24 for the FTP port), VITA_FTP_PORT (default 1337),
  VITA_BACKUP_DIR (default ~/Dev/vita/device-backups/auto).
An FTP server must be up on the Vita (VitaShell's SELECT, or the always-on
plugin) and the Vita awake. Launching the game needs vagentd (port 1339).
"""

import datetime
import ftplib
import io
import json
import os
import re
import subprocess
import sys
import time

# The Vita's DHCP address changes; the last one found is remembered here
# (shared with the OpenMW tools).
HOST_CACHE = os.path.expanduser("~/.config/vita-agent/ftp_host")


def _remembered_host():
    try:
        with open(HOST_CACHE) as f:
            return f.read().strip() or "192.168.0.48"
    except OSError:
        return "192.168.0.48"


HOST = os.environ.get("VITA_FTP_HOST") or _remembered_host()
PORT = int(os.environ.get("VITA_FTP_PORT", "1337"))
BACKUP_ROOT = os.path.expanduser(os.environ.get("VITA_BACKUP_DIR", "~/Dev/vita/device-backups/auto"))
HERE = os.path.dirname(os.path.abspath(__file__))
REPO = os.path.abspath(os.path.join(HERE, "..", ".."))
TITLE = "ARXL00001"
APP_DIR = f"ux0:/app/{TITLE}"
DATA_DIR = "ux0:/data/arx"
PROTOCOL_VERSION = "2024-11-05"


class DeviceError(Exception):
    pass


def discover_host():
    """Scan HOST's /24 for the FTP port; remember and return the hit."""
    import concurrent.futures
    import socket

    prefix = HOST.rsplit(".", 1)[0]

    def probe(i):
        h = f"{prefix}.{i}"
        try:
            socket.create_connection((h, PORT), timeout=1.0).close()
            return h
        except OSError:
            return None

    with concurrent.futures.ThreadPoolExecutor(64) as ex:
        for h in ex.map(probe, range(1, 255)):
            if h:
                os.makedirs(os.path.dirname(HOST_CACHE), exist_ok=True)
                with open(HOST_CACHE, "w") as f:
                    f.write(h + "\n")
                return h
    return None


def connect():
    global HOST
    ftp = ftplib.FTP()
    try:
        ftp.connect(HOST, PORT, timeout=15)
    except OSError as e:
        found = None if os.environ.get("VITA_FTP_HOST") else discover_host()
        if not found:
            raise DeviceError(
                f"cannot reach the Vita's FTP at {HOST}:{PORT} ({e}). "
                "Open VitaShell, press SELECT to start FTP, and keep the Vita awake.")
        HOST = found
        ftp = ftplib.FTP()
        ftp.connect(HOST, PORT, timeout=15)
    ftp.login()
    return ftp


def norm(path):
    """Accept 'ux0:data/x', 'ux0:/data/x' or a path relative to ux0:/data/arx."""
    if re.match(r"^[a-z]+0:", path):
        dev, rest = path.split(":", 1)
        return f"{dev}:/{rest.lstrip('/')}"
    return f"{DATA_DIR}/{path.lstrip('/')}"


def listing(ftp, path):
    # VitaShell rejects "LIST <path>", and resolves CWD relative to the
    # current directory even for "ux0:/..." paths: reset to "/" first.
    lines = []
    try:
        ftp.cwd("/")
        ftp.cwd(path + "/" if path.endswith(":") else path)
    except ftplib.error_perm as e:
        raise DeviceError(f"cannot open directory {path}: {e}")
    ftp.retrlines("LIST", lines.append)
    return lines


def remote_size(ftp, path):
    parent, name = path.rsplit("/", 1)
    try:
        entries = listing(ftp, parent)
    except DeviceError:
        return None
    for line in entries:
        parts = line.split(None, 8)
        if len(parts) == 9 and parts[8] == name and not line.startswith("d"):
            return int(parts[4])
    return None


def fetch(ftp, path):
    buf = io.BytesIO()
    try:
        ftp.retrbinary(f"RETR {path}", buf.write)
    except ftplib.error_perm as e:
        raise DeviceError(f"cannot read {path}: {e}")
    return buf.getvalue()


def backup(ftp, path):
    """Copy a remote file into the backup dir before it is replaced/deleted."""
    if remote_size(ftp, path) is None:
        return None
    stamp = datetime.datetime.now().strftime("%Y%m%d-%H%M%S")
    local = os.path.join(BACKUP_ROOT, stamp, path.replace(":/", "/").replace(":", ""))
    os.makedirs(os.path.dirname(local), exist_ok=True)
    with open(local, "wb") as f:
        f.write(fetch(ftp, path))
    return local


def tool_ls(args):
    path = norm(args.get("path", DATA_DIR))
    ftp = connect()
    try:
        return "\n".join(listing(ftp, path)) or "(empty)"
    finally:
        ftp.quit()


def tool_log(args):
    path = norm(args.get("file", "arx.log"))
    tail = int(args.get("tail", 200))
    pattern = args.get("grep")
    ftp = connect()
    try:
        text = fetch(ftp, path).decode("utf-8", "replace")
    finally:
        ftp.quit()
    lines = text.splitlines()
    total = len(lines)
    if pattern:
        rx = re.compile(pattern)
        lines = [l for l in lines if rx.search(l)]
    shown = lines[-tail:] if tail > 0 else lines
    head = f"{path}: {total} lines" + (f", {len(lines)} match /{pattern}/" if pattern else "")
    return head + f", showing {len(shown)}\n" + "\n".join(shown)


def tool_get(args):
    path = norm(args["remote"])
    local = os.path.expanduser(args["local"])
    ftp = connect()
    try:
        data = fetch(ftp, path)
    finally:
        ftp.quit()
    os.makedirs(os.path.dirname(os.path.abspath(local)), exist_ok=True)
    with open(local, "wb") as f:
        f.write(data)
    return f"downloaded {path} -> {local} ({len(data)} bytes)"


def put_bytes(data, remote, keep_backup=True):
    ftp = connect()
    try:
        saved = backup(ftp, remote) if keep_backup else None
    finally:
        ftp.quit()
    # VitaShell answers STOR with "550 File not found" while the file is open, e.g.
    # the eboot of a running app; retry a few times in case it is about to close.
    for attempt in range(4):
        ftp = connect()
        try:
            ftp.storbinary(f"STOR {remote}", io.BytesIO(data))
            got = remote_size(ftp, remote)
            break
        except ftplib.error_perm:
            if attempt == 3:
                raise
        finally:
            try:
                ftp.quit()
            except (OSError, ftplib.Error):
                ftp.close()
        time.sleep(2)
    if got != len(data):
        raise DeviceError(f"size mismatch after upload: local {len(data)}, device {got}")
    return saved


def put(local, remote):
    local = os.path.expanduser(local)
    if not os.path.isfile(local):
        raise DeviceError(f"local file not found: {local}")
    with open(local, "rb") as f:
        data = f.read()
    saved = put_bytes(data, remote)
    msg = f"uploaded {local} -> {remote} ({len(data)} bytes, verified)"
    return msg + (f"\nprevious version backed up to {saved}" if saved else "")


def delete(remote, keep_backup=True):
    """Delete a remote file; returns the backup path, or None when there was nothing to delete."""
    ftp = connect()
    try:
        if remote_size(ftp, remote) is None:
            return None
        saved = backup(ftp, remote) if keep_backup else ""
        # VitaShell answers DELE with 226, which ftplib.delete rejects.
        try:
            resp = ftp.sendcmd(f"DELE {remote}")
        except ftplib.error_reply as e:
            resp = str(e)
        if not resp.startswith("2"):
            raise DeviceError(f"delete failed: {resp}")
    finally:
        ftp.quit()
    return saved


def tool_put(args):
    return put(args["local"], norm(args["remote"]))


def tool_delete(args):
    path = norm(args["remote"])
    saved = delete(path)
    if saved is None:
        return f"{path} does not exist; nothing deleted"
    return f"deleted {path} (backed up to {saved})"


def tool_deploy(args):
    local = args.get("eboot") or os.path.join(REPO, "build-vita", "eboot.bin")
    return put(local, f"{APP_DIR}/eboot.bin")


def run_bench(argv, timeout):
    proc = subprocess.run([sys.executable, os.path.join(HERE, "vita_bench.py"), *argv],
                          capture_output=True, text=True, timeout=timeout)
    out = (proc.stdout + proc.stderr).strip().splitlines()
    return "\n".join(out[-40:]) + f"\n(exit {proc.returncode})"


def tool_bench_record(args):
    argv = ["--record"]
    if args.get("route"):
        argv += ["--route", args["route"]]
    return run_bench(argv, 120)


def tool_bench_run(args):
    argv = []
    if args.get("deploy"):
        argv.append("--deploy")
    for key in ("label", "route", "settle", "fps"):
        if args.get(key) not in (None, ""):
            argv += [f"--{key}", str(args[key])]
    timeout = float(args.get("timeout", 900))
    argv += ["--timeout", str(timeout)]
    return run_bench(argv, timeout + 180)


def tool_bench_results(args):
    return run_bench(["--results", str(int(args.get("count", 10)))], 60)


TOOLS = {
    "arx_ls": (tool_ls, "List a directory on the Vita. Paths: 'ux0:/app/ARXL00001', or relative to "
               "ux0:/data/arx.", {"path": {"type": "string", "description": "Default ux0:/data/arx"}}, []),
    "arx_log": (tool_log, "Read a log from ux0:/data/arx (arx.log, debug.log, critical_oom.log, "
                "bench/results.txt...), optionally filtered by a regex, returning the last N lines.",
                {"file": {"type": "string", "description": "Default arx.log"},
                 "grep": {"type": "string", "description": "Python regex filter, e.g. '\\[Frame\\]'"},
                 "tail": {"type": "integer", "description": "Lines to return (0 = all). Default 200"}}, []),
    "arx_get": (tool_get, "Download a file from the Vita to a local path.",
                {"remote": {"type": "string"}, "local": {"type": "string"}}, ["remote", "local"]),
    "arx_put": (tool_put, "Upload a local file to the Vita. Backs up any file it replaces, verifies size.",
                {"local": {"type": "string"}, "remote": {"type": "string"}}, ["local", "remote"]),
    "arx_delete": (tool_delete, "Delete a file on the Vita after backing it up locally.",
                   {"remote": {"type": "string"}}, ["remote"]),
    "arx_deploy": (tool_deploy, "Deploy an eboot.bin to ux0:/app/ARXL00001 (default: build-vita/eboot.bin). "
                   "Backs up the installed one first, verifies size.",
                   {"eboot": {"type": "string", "description": "Local eboot.bin path"}}, []),
    "arx_bench_record": (tool_bench_record, "Arm route recording: the next time Arx is launched, once a save has "
                         "loaded, the player's path is recorded (10 Hz) into ux0:/data/arx/bench/<route> until "
                         "the game quits, together with a copy of that save (loading another save restarts it).",
                         {"route": {"type": "string", "description": "Route file name, default route.txt"}}, []),
    "arx_bench_run": (tool_bench_run, "Replay a recorded route on the Vita hands-free: optionally deploy "
                      "build-vita/eboot.bin, launch Arx through vagentd, follow the live log until the result, "
                      "and append it to the bench ledger. Takes the route length plus ~1 minute.",
                      {"label": {"type": "string", "description": "Name for this run in the ledger"},
                       "route": {"type": "string", "description": "Route file in bench/, default route.txt"},
                       "deploy": {"type": "boolean", "description": "Upload build-vita/eboot.bin first"},
                       "settle": {"type": "number", "description": "Seconds at the route start before timing"},
                       "fps": {"type": "integer", "description": "Frame limit for this run (0 = uncapped)"},
                       "timeout": {"type": "number", "description": "Give up after this many seconds (900)"}}, []),
    "arx_bench_results": (tool_bench_results, "Show the last benchmark runs from the ledger, newest last.",
                          {"count": {"type": "integer", "description": "Runs to show (10)"}}, []),
}


def handle(msg):
    method = msg.get("method")
    if method == "initialize":
        return {"protocolVersion": PROTOCOL_VERSION, "capabilities": {"tools": {}},
                "serverInfo": {"name": "arx-vita", "version": "1.0"}}
    if method == "ping":
        return {}
    if method == "tools/list":
        return {"tools": [{"name": n, "description": d,
                           "inputSchema": {"type": "object", "properties": p, "required": r}}
                          for n, (_, d, p, r) in TOOLS.items()]}
    if method == "tools/call":
        name = msg["params"]["name"]
        args = msg["params"].get("arguments") or {}
        if name not in TOOLS:
            return {"content": [{"type": "text", "text": f"unknown tool {name}"}], "isError": True}
        try:
            text = TOOLS[name][0](args)
            return {"content": [{"type": "text", "text": text}]}
        except (DeviceError, ftplib.Error, OSError, KeyError, subprocess.TimeoutExpired) as e:
            return {"content": [{"type": "text", "text": f"{type(e).__name__}: {e}"}], "isError": True}
    raise LookupError(method)


def main():
    for line in sys.stdin:
        line = line.strip()
        if not line:
            continue
        msg = json.loads(line)
        if "id" not in msg:
            continue  # notification (e.g. notifications/initialized)
        try:
            reply = {"jsonrpc": "2.0", "id": msg["id"], "result": handle(msg)}
        except LookupError:
            reply = {"jsonrpc": "2.0", "id": msg["id"],
                     "error": {"code": -32601, "message": f"method not found: {msg.get('method')}"}}
        sys.stdout.write(json.dumps(reply) + "\n")
        sys.stdout.flush()


if __name__ == "__main__":
    main()
