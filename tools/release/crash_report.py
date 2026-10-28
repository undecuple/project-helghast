#!/usr/bin/env python3
"""Opt-in crash reports: the launcher sends one Sentry envelope per crash to the project's error tracker
Standard library only; shipped next to launch.py (assemble_payload.py FILES).

What it reads: kz3-install.ini [crash] report=1|0 and dsn=<the build's address> (the installer writes both; a build
without an address writes dsn= and nothing can be turned on), the game's crash files <root>/build/logs/*crash-*.txt
(port/kz3/kz3_log.c kz3_log_write_crash: time, thread, reason, details, the last 200 log lines), the first lines of
the game log a crash file names (only for the GPU vendor), build/paths.json (the dump and update folders, to replace
them by <dump>/<update>), the data folder's online.ini (the online id and server hosts, to remove them) and
<root>/manifest.json (the commit for the release name). It never opens a .dmp (the minidump stays on the PC).

What it sends (only when report=1 and dsn is set): one event, level fatal, message = the crash reason, the exception
(code, details, the [CRASH] lines' rva frames as plain text frames), release kz3@<version>+<commit>, environment
release, platform native, tags os/renderer/gpu_vendor, and the crash file's log lines after scrub() (at most 200
lines and 64 KB) as extra.log and as breadcrumbs. No user, no server_name, no memory, no usage data.

Rules: HTTPS only; one try, 10 s timeout; at most one crash event per launcher session; a 429 or an
x-sentry-rate-limits header that covers errors stops sending until its time is up (kept in the state file); every
crash file is recorded in <root>/logs/crash-sent.json before the send (a file is never sent twice, even when the
state cannot be written afterwards: then nothing is sent); files older than MAX_AGE_DAYS and files that existed
when the player turned reports on are never sent. Failures are one line in the launcher log, never a dialog.
Warnings: the game log has no "serious warning" marker today, so only crashes are sent.
"""
import configparser
import datetime
import glob
import ipaddress
import json
import os
import platform
import re
import socket
import time
import urllib.error
import urllib.parse
import urllib.request
import uuid

HERE = os.path.dirname(os.path.abspath(__file__))
try:
    import names
except ImportError:   # imported from another folder (tests add HERE to sys.path first)
    import sys
    sys.path.insert(0, HERE)
    import names

INI = "kz3-install.ini"
STATE = "crash-sent.json"          # in <root>/logs (the launcher's own folder; the uninstaller removes it)
MAX_LINES = 200
MAX_BYTES = 64 * 1024
MAX_BREADCRUMBS = 100
MAX_AGE_DAYS = 7
TIMEOUT = 10
UNAVAILABLE = "not available in this build"


# ------------------------------------------------------------------------------------------------------ settings
def read_settings(root):
    """-> (report_on, dsn) from kz3-install.ini [crash]."""
    cp = configparser.RawConfigParser()
    try:
        cp.read(os.path.join(root, INI), encoding="utf-8")
    except (configparser.Error, OSError, UnicodeDecodeError):
        return False, ""
    rep = cp.get("crash", "report", fallback="0").strip() == "1"
    return rep, cp.get("crash", "dsn", fallback="").strip()


def set_report(root, on):
    """Write [crash] report=1|0 (keeps dsn and every other key)."""
    p = os.path.join(root, INI)
    cp = configparser.RawConfigParser()
    cp.read(p, encoding="utf-8")
    if not cp.has_section("crash"):
        cp.add_section("crash")
    cp.set("crash", "report", "1" if on else "0")
    with open(p, "w", encoding="utf-8") as f:
        cp.write(f)


def parse_dsn(dsn, allow_http_loopback=False):
    """https://<key>@<host>[:port][/prefix]/<project> -> (envelope url, key); raises ValueError. Plain http is refused,
    except to 127.0.0.1 when allow_http_loopback (host tests only)."""
    u = urllib.parse.urlsplit(dsn or "")
    if u.scheme != "https" and not (allow_http_loopback and u.scheme == "http" and u.hostname == "127.0.0.1"):
        raise ValueError("crash report address must be https")
    if not u.username or not u.hostname:
        raise ValueError("crash report address has no key or host")
    parts = [p for p in u.path.split("/") if p]
    if not parts or not parts[-1].isdigit():
        raise ValueError("crash report address has no project number")
    prefix = "/".join(parts[:-1])
    host = u.hostname + (f":{u.port}" if u.port else "")
    path = "/" + (prefix + "/" if prefix else "") + f"api/{parts[-1]}/envelope/"
    return f"{u.scheme}://{host}{path}", u.username


# ------------------------------------------------------------------------------------------------------ scrubbing
_PATH_USER_RX = re.compile(
    r"(?i)(\b[A-Z]:[\\/]+(?:Users|Documents and Settings)[\\/]+|\b[A-Z]:[\\/]+home[\\/]+|(?<![\w.])/home/|"
    r"(?<![\w.])/Users/|(?<![\w.])/var/home/|[\\/]drive_c[\\/]+users[\\/]+)([^\\/\r\n\"'<>|:*?]+)")
_UNC_RX = re.compile(r"(\\\\)([A-Za-z0-9][A-Za-z0-9.\-_$]*)(?=\\)")
_EMAIL_RX = re.compile(r"(?i)\b[A-Z0-9._%+\-]+@[A-Z0-9.\-]+\.[A-Z]{2,}\b")
_IPV4_RX = re.compile(r"(?<![\w.])(?:\d{1,3}\.){3}\d{1,3}(?![\w]|\.\d)")
_IPV6_RX = re.compile(r"(?<![\w:.])[0-9A-Fa-f]{0,4}(?::[0-9A-Fa-f]{0,4}){2,7}(?:%\w+)?(?![\w:])")
_MAC_RX = re.compile(r"(?i)(?<![\w:\-])[0-9A-F]{2}(?:([:\-])[0-9A-F]{2})(?:\1[0-9A-F]{2}){4}(?![\w:\-])")
_URL_RX = re.compile(r"(?i)\b([a-z][a-z0-9+.\-]*://)(?:[^@/\s]*@)?([^/\s:\"']+)")
# kz3_net.c / np_kz3.c lines: online id "X", online_id is "X", issued to "X", online_id = X; host = X (online.ini echo)
_ONLINE_RX = re.compile(r"(?i)\b((?:online[ _]?id|npid|issued to|signed in as)\b\s*(?:is|=|:)?\s*\"?)([A-Za-z][\w\-]{2,15})")
_HOSTKEY_RX = re.compile(r"(?i)\b((?:host|hostname|server)\s*[=:]\s*\"?)([A-Za-z0-9][\w.\-]*\.[A-Za-z][\w.\-]*)")
_NOT_IP = {"0.0.0.0", "127.0.0.1"}


def _ipv4(m):
    s = m.group(0)
    try:
        ipaddress.IPv4Address(s)
    except ValueError:
        return s
    return s if s in _NOT_IP else "<ip>"


def _ipv6(m):
    s = m.group(0)
    if s.count(":") < 2 or not re.search(r"[0-9A-Fa-f]", s):
        return s
    try:
        a = ipaddress.IPv6Address(s.split("%")[0])
    except ValueError:
        return s
    return s if a.is_loopback or a.is_unspecified else "<ip>"


def _word_rx(words, min_len):
    ws = sorted({w for w in words if w and len(w) >= min_len}, key=len, reverse=True)
    if not ws:
        return None
    return re.compile(r"(?i)(?<![\w\-])(?:" + "|".join(re.escape(w) for w in ws) + r")(?![\w\-])")


def _path_forms(p):
    p = (p or "").rstrip("\\/")
    if len(p) < 4:
        return set()
    out = {p, p.replace("\\", "/"), p.replace("/", "\\")}
    if p.startswith("/"):   # a Linux path the game sees through Wine as Z:\...
        out.add("Z:" + p.replace("/", "\\"))
        out.add("Z:" + p)
    return out


def scrubber(paths=None, users=(), hosts=(), online_ids=()):
    """-> scrub(text). paths: {placeholder: folder} replaced first (longest first, any slash style, any case);
    users: user names (in paths always, as a bare word when 4+ letters); hosts: machine names (bare word, 2+);
    online_ids: the player's online ids (bare word, 3+). Then the generic rules: user folders in Windows, Wine/Proton
    and Linux paths, UNC hosts, URLs' hosts, e-mail, IPv4/IPv6 (not loopback), MAC addresses, online-id phrases."""
    pairs = []
    for ph, p in (paths or {}).items():
        for f in _path_forms(p):
            pairs.append((f, ph))
    pairs.sort(key=lambda x: len(x[0]), reverse=True)
    path_rx = re.compile("|".join(re.escape(f) for f, _ in pairs), re.IGNORECASE) if pairs else None
    path_map = {f.lower(): ph for f, ph in pairs}
    user_rx = _word_rx(users, 4)
    host_rx = _word_rx(list(hosts) + [h.split(".")[0] for h in hosts if h], 2)
    id_rx = _word_rx(online_ids, 3)

    def scrub(text):
        s = text
        if path_rx is not None:
            s = path_rx.sub(lambda m: path_map.get(m.group(0).lower(), "<path>"), s)
        s = _EMAIL_RX.sub("<email>", s)
        s = _URL_RX.sub(lambda m: m.group(1) + "<host>", s)
        s = _PATH_USER_RX.sub(lambda m: m.group(1) + "<user>", s)
        s = _UNC_RX.sub(lambda m: m.group(1) + "<host>", s)
        s = _MAC_RX.sub("<mac>", s)
        s = _IPV6_RX.sub(_ipv6, s)
        s = _IPV4_RX.sub(_ipv4, s)
        s = _ONLINE_RX.sub(lambda m: m.group(1) + "<online-id>", s)
        s = _HOSTKEY_RX.sub(lambda m: m.group(1) + "<host>", s)
        if id_rx is not None:
            s = id_rx.sub("<online-id>", s)
        if host_rx is not None:
            s = host_rx.sub("<host>", s)
        if user_rx is not None:
            s = user_rx.sub("<user>", s)
        return s
    return scrub


def online_ini_values(path):
    """The online id(s) and server hosts in online.ini (port/net/kz3_net.c keys), to remove them from the log."""
    ids, hosts = set(), set()
    try:
        with open(path, encoding="utf-8", errors="replace") as f:
            for line in f:
                k, sep, v = line.partition("=")
                if not sep:
                    continue
                k, v = k.strip().lower(), v.split(";")[0].split("#")[0].strip().strip('"')
                if k == "online_id" and v:
                    ids.add(v)
                elif k == "host" and v:
                    hosts.add(v)
    except OSError:
        pass
    return ids, hosts


def machine_scrubber(root, environ, data_dir=None):
    """The scrubber for this PC: the install, dump and update folders, the user and machine names, online.ini."""
    paths = {}
    pj = _load_json(os.path.join(root, "build", "paths.json")) or {}
    for key, ph in (("dump", "<dump>"), ("update", "<update>")):
        if pj.get(key):
            paths[ph] = pj[key]
    paths["<install>"] = root
    for k in ("USERPROFILE", "HOME"):
        if environ.get(k) and len(environ[k]) > 3 and "<home>" not in paths.values():
            paths.setdefault("<home>", environ[k])
    users = {environ.get(k, "") for k in ("USERNAME", "USER", "LOGNAME")}
    hosts = {environ.get(k, "") for k in ("COMPUTERNAME", "HOSTNAME", "USERDOMAIN")}
    try:
        hosts.add(socket.gethostname())
    except OSError:
        pass
    ids, ohosts = online_ini_values(os.path.join(data_dir, "online.ini")) if data_dir else (set(), set())
    return scrubber(paths, users, hosts | ohosts, ids)


def cap_log(text, max_lines=MAX_LINES, max_bytes=MAX_BYTES):
    """The last max_lines lines, then whole lines from the end up to max_bytes (UTF-8)."""
    lines = text.splitlines()[-max_lines:]
    out, size = [], 0
    for ln in reversed(lines):
        n = len(ln.encode("utf-8")) + 1
        if size + n > max_bytes:
            break
        out.append(ln)
        size += n
    return "\n".join(reversed(out))


# ------------------------------------------------------------------------------------------------------ crash files
def _load_json(p):
    try:
        with open(p, encoding="utf-8") as f:
            return json.load(f)
    except (OSError, ValueError):
        return None


def crash_dir(root):
    return os.path.join(root, "build", "logs")


def crash_files(root):
    """The game's crash text files, oldest first (never the .dmp beside them)."""
    fs = [p for p in glob.glob(os.path.join(crash_dir(root), "*crash-*.txt")) if os.path.isfile(p)]
    return sorted(fs, key=lambda p: (os.path.getmtime(p), p))


_LINE_RX = re.compile(r"^\s*\d+\s+\d+(?:\s+(\S+))?\s+([EWIDT])\s(.*)$")
_FRAME_RX = re.compile(r"#\s*(\d+)\s+rva=0x([0-9A-Fa-f]+)")
_LEVELS = {"E": "error", "W": "warning", "I": "info", "D": "debug", "T": "debug"}


def parse_crash(text):
    """kz3_log_write_crash's file -> {reason, details, thread, log_file, lines, crash_lines, frames}."""
    r = {"reason": "", "details": "", "thread": "", "log_file": "", "lines": [], "crash_lines": [], "frames": []}
    head, sep, tail = text.partition("last log lines (oldest first):")
    for ln in head.splitlines():
        if ln.startswith("reason:") and not r["reason"]:
            r["reason"] = ln[7:].strip()
        elif ln.startswith("log file:") and not r["log_file"]:
            r["log_file"] = ln[9:].strip()
        elif ln.startswith("time:"):
            m = re.search(r"thread (\S+)", ln)
            r["thread"] = m.group(1) if m else ""
        elif ln.strip() and not ln.startswith("Killzone 3 port crash report") and not r["details"] and r["reason"]:
            r["details"] = ln.strip()
    r["lines"] = [ln for ln in (tail if sep else "").splitlines() if ln.strip()]
    r["crash_lines"] = [ln[ln.index("[CRASH]") + 7:].strip() for ln in r["lines"] if "[CRASH]" in ln]
    for c in r["crash_lines"]:
        m = _FRAME_RX.search(c)
        if m:
            r["frames"].append((int(m.group(1)), int(m.group(2), 16)))
    if not r["reason"]:
        r["reason"] = "crash"
    return r


def gpu_vendor(log_path, max_lines=400):
    """The vendor of the D3D12 adapter line ([D3D12] adapter: <name>) in the first lines of the game log, or ""."""
    try:
        with open(log_path, encoding="utf-8", errors="replace") as f:
            for i, ln in enumerate(f):
                if i >= max_lines:
                    break
                m = re.search(r"\[D3D12\] adapter:\s*(.+)", ln)
                if m:
                    n = m.group(1).upper()
                    for key, v in (("NVIDIA", "NVIDIA"), ("GEFORCE", "NVIDIA"), ("AMD", "AMD"), ("RADEON", "AMD"),
                                   ("INTEL", "Intel"), ("MICROSOFT", "Microsoft")):
                        if key in n:
                            return v
                    return "other"
    except OSError:
        pass
    return ""


def os_tag():
    try:
        if os.name == "nt":
            v = platform.version()
            return f"Windows {v}" if v else "Windows"
        return f"{platform.system()} {platform.release()}".strip()
    except Exception:  # noqa: BLE001  (a tag is never worth a failure)
        return ""


def release_name(root):
    commit = str((_load_json(os.path.join(root, "manifest.json")) or {}).get("repo_commit") or "")
    commit = re.sub(r"[^0-9a-f]", "", commit.lower())[:12]
    return f"kz3@{names.VERSION}" + (f"+{commit}" if commit else "")


def build_event(root, crash_path, scrub, event_id=None, mtime=None):
    """-> the Sentry event dict for one crash file."""
    with open(crash_path, encoding="utf-8", errors="replace") as f:
        c = parse_crash(f.read())
    log = cap_log(scrub("\n".join(c["lines"])))
    reason, details = scrub(c["reason"]), scrub(c["details"])
    code = re.search(r"0x[0-9A-Fa-f]{8}", c["reason"])
    frames = [{"function": f"kz3.exe+0x{rva:X}", "package": "kz3.exe", "in_app": True}
              for _, rva in sorted(c["frames"], reverse=True)]          # Sentry: outermost first, crashing frame last
    gm = re.search(r"guest-fn=(0x[0-9A-Fa-f]+)", c["details"])
    if gm:
        frames.append({"function": f"guest {gm.group(1)}", "package": "EBOOT (recompiled)", "in_app": True})
    exc = {"type": code.group(0).upper().replace("0X", "0x") if code else "fatal error", "value": reason,
           "mechanism": {"type": "kz3-crash-file", "handled": False}}
    if frames:
        exc["stacktrace"] = {"frames": frames}
    tags = {"renderer": "d3d12"}
    o = os_tag()
    if o:
        tags["os"] = o
    if c["log_file"]:
        lf = os.path.join(os.path.dirname(crash_path), re.split(r"[\\/]", c["log_file"])[-1])
        if lf.lower().endswith(".log"):
            v = gpu_vendor(lf)
            if v:
                tags["gpu_vendor"] = v
    if c["thread"]:
        tags["thread"] = scrub(c["thread"])
    crumbs = []
    for ln in log.splitlines()[-MAX_BREADCRUMBS:]:
        m = _LINE_RX.match(ln)
        crumbs.append({"category": "log", "level": _LEVELS.get(m.group(2), "info") if m else "info",
                       "message": m.group(3) if m else ln})
    ts = mtime if mtime is not None else os.path.getmtime(crash_path)
    return {
        "event_id": event_id or uuid.uuid4().hex,
        "timestamp": datetime.datetime.fromtimestamp(ts, datetime.timezone.utc).isoformat().replace("+00:00", "Z"),
        "level": "fatal", "platform": "native", "logger": "kz3.launcher",
        "release": release_name(root), "environment": "release",
        "message": {"formatted": reason},
        "exception": {"values": [exc]},
        "tags": tags,
        "extra": {"details": details, "crash_lines": [scrub(x) for x in c["crash_lines"]][-40:], "log": log},
        "breadcrumbs": {"values": crumbs},
        "sdk": {"name": f"{names.REPO_SLUG}.launcher", "version": names.VERSION},
    }


def envelope(event, dsn, sent_at=None):
    """-> bytes: the envelope header line (event_id, sent_at, dsn), the item header, the JSON event."""
    body = json.dumps(event, separators=(",", ":"), ensure_ascii=False).encode("utf-8")
    sent = sent_at or datetime.datetime.now(datetime.timezone.utc).isoformat(timespec="seconds").replace("+00:00", "Z")
    head = json.dumps({"event_id": event["event_id"], "sent_at": sent, "dsn": dsn}, separators=(",", ":"))
    item = json.dumps({"type": "event", "length": len(body), "content_type": "application/json"}, separators=(",", ":"))
    return head.encode("utf-8") + b"\n" + item.encode("utf-8") + b"\n" + body + b"\n"


# ------------------------------------------------------------------------------------------------------ sending
def rate_limit_seconds(status, headers):
    """Seconds to wait before the next send from a reply (0 = none): x-sentry-rate-limits entries whose categories
    are empty (all) or include error, else Retry-After on a 429, else 60 s on a 429."""
    secs = 0
    rl = (headers.get("x-sentry-rate-limits") or headers.get("X-Sentry-Rate-Limits") or "") if headers else ""
    for part in rl.split(","):
        f = part.strip().split(":")
        if len(f) >= 2 and f[0].strip().isdigit():
            cats = [x.strip() for x in f[1].split(";") if x.strip()]
            if not cats or "error" in cats or "default" in cats:
                secs = max(secs, int(f[0]))
    if status == 429 and not secs:
        ra = (headers.get("Retry-After") or headers.get("retry-after") or "") if headers else ""
        secs = int(ra) if str(ra).strip().isdigit() else 60
    return secs


def post(url, key, data, timeout=TIMEOUT, allow_http_loopback=False):
    """One POST of an envelope -> (status, headers dict, body text). Raises OSError on no connection."""
    u = urllib.parse.urlsplit(url)
    if u.scheme != "https" and not (allow_http_loopback and u.scheme == "http" and u.hostname == "127.0.0.1"):
        raise ValueError("crash report address must be https")
    req = urllib.request.Request(url, data=data, method="POST", headers={
        "Content-Type": "application/x-sentry-envelope",
        "User-Agent": f"{names.REPO_SLUG}-crash-report/{names.VERSION}",
        "X-Sentry-Auth": f"Sentry sentry_version=7, sentry_key={key}, "
                         f"sentry_client={names.REPO_SLUG}-launcher/{names.VERSION}"})
    opener = urllib.request.build_opener(urllib.request.ProxyHandler({}) if u.hostname == "127.0.0.1"
                                         else urllib.request.ProxyHandler())
    try:
        with opener.open(req, timeout=timeout) as r:
            return r.status, dict(r.headers.items()), r.read(4096).decode("utf-8", "replace")
    except urllib.error.HTTPError as e:
        return e.code, dict(e.headers.items()) if e.headers else {}, ""


class Session:
    """One launcher run: at most one crash event; a rate limit stops the rest of the session."""

    def __init__(self, root, environ=None, data_dir=None, log=None, now=None, allow_http_loopback=False, poster=None):
        self.root, self.environ = root, dict(os.environ if environ is None else environ)
        self.data_dir, self.log = data_dir, (log or (lambda s: None))
        self.now = now or time.time
        self.allow_http_loopback, self.poster = allow_http_loopback, (poster or post)
        self.sent, self.stopped = 0, False

    def state_path(self):
        return os.path.join(self.root, "logs", STATE)

    def load_state(self):
        s = _load_json(self.state_path())
        if not isinstance(s, dict) or not isinstance(s.get("files"), dict):
            s = {"files": {}}
        return s

    def save_state(self, s):
        try:
            os.makedirs(os.path.dirname(self.state_path()), exist_ok=True)
            tmp = self.state_path() + ".tmp"
            with open(tmp, "w", encoding="utf-8") as f:
                json.dump(s, f, indent=1, sort_keys=True)
            os.replace(tmp, self.state_path())
            return True
        except OSError as e:
            self.log(f"crash report: state file not written ({e.strerror or e}); nothing sent")
            return False

    def mark_existing(self, why="before opt-in"):
        """Record every crash file present now as never to be sent (turning reports on does not send old crashes)."""
        s = self.load_state()
        for p in crash_files(self.root):
            s["files"].setdefault(os.path.basename(p), {"mtime": os.path.getmtime(p), "status": why})
        return self.save_state(s)

    def pending(self, s):
        out, now = [], self.now()
        # Crashes from before the player turned reports on are never sent: the installer (or the Start menu switch)
        # writes kz3-install.ini when it records the choice, so a crash file older than that file is "before opt-in".
        try:
            since = os.path.getmtime(os.path.join(self.root, INI))
        except OSError:
            since = now
        for p in crash_files(self.root):
            n, mt = os.path.basename(p), os.path.getmtime(p)
            rec = s["files"].get(n)
            if rec and abs(float(rec.get("mtime", 0)) - mt) < 1e-3:
                continue
            if mt < since:
                s["files"][n] = {"mtime": mt, "status": "before opt-in"}
                continue
            if now - mt > MAX_AGE_DAYS * 86400:
                s["files"][n] = {"mtime": mt, "status": "too old"}
                continue
            out.append((p, n, mt))
        return out

    def check(self):
        """Send the newest crash not handled yet. -> None (nothing to do / not opted in), or a dict
        {"status": "sent"|"failed"|"rate-limited"|"limit", "event_id", "file"}."""
        on, dsn = read_settings(self.root)
        if not on or not dsn:
            return None
        s = self.load_state()
        todo = self.pending(s)
        if not todo:
            return None
        p, n, mt = todo[-1]
        if self.stopped or self.sent >= 1:
            return {"status": "limit", "event_id": "", "file": n}
        if float(s.get("blocked_until", 0)) > self.now():
            self.stopped = True
            self.log("crash report: the tracker asked to wait; not sent this session")
            return {"status": "rate-limited", "event_id": "", "file": n}
        try:
            url, key = parse_dsn(dsn, self.allow_http_loopback)
        except ValueError as e:
            self.log(f"crash report: {e}; not sent")
            return {"status": "failed", "event_id": "", "file": n}
        for q, qn, qmt in todo[:-1]:   # older unsent crashes of the same gap: only the newest is reported
            s["files"][qn] = {"mtime": qmt, "status": "skipped (newer crash sent)"}
        ev = build_event(self.root, p, machine_scrubber(self.root, self.environ, self.data_dir), mtime=mt)
        s["files"][n] = {"mtime": mt, "status": "sending", "event_id": ev["event_id"]}
        if not self.save_state(s):
            return {"status": "failed", "event_id": "", "file": n}
        self.sent += 1
        try:
            status, headers, _ = self.poster(url, key, envelope(ev, dsn), TIMEOUT, self.allow_http_loopback)
        except (OSError, ValueError) as e:
            status, headers = 0, {}
            self.log(f"crash report: not sent ({type(e).__name__}: {getattr(e, 'reason', e)})")
        wait = rate_limit_seconds(status, headers) if status else 0
        if wait:
            s["blocked_until"] = self.now() + wait
            self.stopped = True
        if status == 429:
            s["files"].pop(n, None)     # not accepted: try again in a later session after the wait
            res = "rate-limited"
            self.log(f"crash report: rate limited for {wait} s; kept for a later session")
        elif 200 <= status < 300:
            res = "sent"
            self.log(f"crash report: sent {n} as event {ev['event_id']}")
        else:
            res = "failed"
            if status:
                self.log(f"crash report: the tracker answered HTTP {status}; not retried")
        if n in s["files"]:
            s["files"][n]["status"] = res
        live = {os.path.basename(x) for x in crash_files(self.root)}
        s["files"] = {k: v for k, v in s["files"].items() if k in live}
        self.save_state(s)
        return {"status": res, "event_id": ev["event_id"] if res == "sent" else "", "file": n}


def notice(result):
    """The launcher's one sentence after a crash it handled (None when it sent nothing and was not asked to)."""
    if not result:
        return None
    if result["status"] == "sent":
        return f"A crash report was sent to the project (id {result['event_id'][:8]}). Thank you."
    if result["status"] == "rate-limited":
        return "The crash report was not sent: the project's tracker asked to wait. It is sent on a later start."
    if result["status"] == "limit":
        return "The crash report is sent on the next start (one report per session)."
    return "The crash report could not be sent (no connection?). It is not retried."


def toggle_text(root):
    """-> (available, on, question or info text) for the launcher's "Crash reports" entry."""
    on, dsn = read_settings(root)
    if not dsn:
        return False, False, f"Crash reports are {UNAVAILABLE}."
    if on:
        return True, True, ("Crash reports are on: after a crash the launcher sends the stack trace and the last log "
                            "lines, with user names, machine names, addresses and your folders removed, to the "
                            "project. Never a memory dump, never usage data.\n\nTurn crash reports off?")
    return True, False, ("Crash reports are off.\n\nTurn them on? After a crash the launcher then sends the stack trace "
                         "and the last log lines, with user names, machine names, addresses and your folders "
                         "removed, to the project's own error tracker. Never a memory dump, never usage data. "
                         "Crashes from before you turn this on are not sent.")
