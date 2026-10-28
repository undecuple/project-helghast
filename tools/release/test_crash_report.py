#!/usr/bin/env python3
"""Host test for tools/release/crash_report.py and its launcher hooks. No network: a local HTTP server on 127.0.0.1
(the test-only allow_http_loopback flag) stands in for the error tracker; the DSN is fake. Exit 0 = PASS, 1 = FAIL.

    python3 tools/release/test_crash_report.py

Cases: 1 scrub rules, each with positive and negative cases (Proton Z:\\home\\alice\\, Windows C:\\Users\\Bob\\, Linux
/home/<n>/, install/dump paths any slash style, machine name, IPv4/IPv6 (loopback kept), e-mail, MAC, UNC host,
URL host, online ids from phrases and from online.ini, user names); 2 cap at 200 lines / 64 KB; 3 DSN parsing and
https-only; 4 crash file parsing and the event (level fatal, release, frames, tags, no user/server_name, scrubbed log);
5 envelope format (header line event_id/sent_at/dsn, item header with the body length, JSON body); 6 opt-out and an
empty DSN send nothing; 7 exactly one envelope reaches the fake server, the sent marker prevents a resend, at most
one per session; 8 the .dmp is never opened; 9 a 429 with x-sentry-rate-limits stops the session and keeps the file
for later; 10 turning reports on does not send old crashes, files older than 7 days are not sent; 11 launch.py
--crash-reports (no dsn: "not available in this build"; on/off switch); 12 the support package hides the dsn.
"""
import builtins
import http.server
import json
import os
import shutil
import sys
import tempfile
import threading
import time
import zipfile

sys.path.insert(0, os.path.dirname(os.path.abspath(__file__)))
import crash_report as cr  # noqa: E402
import launch as la  # noqa: E402
import support  # noqa: E402

FAILS = []
FAKE_DSN = "https://0123456789abcdef0123456789abcdef@example.invalid/2"


def check(cond, what, detail=""):
    print(("PASS " if cond else "FAIL ") + what + (f"  ({detail})" if detail and not cond else ""))
    if not cond:
        FAILS.append(what)


CRASH = """Killzone 3 port crash report
time: 123456 ms after start, thread 4242
log file: Z:\\home\\alice\\Games\\Project Helghast\\build\\logs\\Killzone3-PC-20261027-101010.log
reason: unhandled exception 0xC0000005

rip rva=0x1A2B3C guest-fn=0x0012ABCD cia=0x0012ABF0 lr=0x0012AB00 tid=4242

last log lines (oldest first):
     1000  4242 main I [boot] game folder D:\\Dumps\\BCUS98234\\PS3_GAME
     1001  4242 main I [net] online id "Helghan99", host = medius.example.net
     1002  4242 main W [fs] C:\\Users\\Bob\\AppData\\Roaming\\Project Helghast\\keybinds.ini from 192.168.1.20
   123450  4242 ppu0 E [CRASH] code=0xC0000005 rip=00007FF6001A2B3C
   123451  4242 ppu0 E [CRASH] module=00007FF600000000 rva=0x1A2B3C
   123452  4242 ppu0 E [CRASH]   #0  rva=0x1A2B3C
   123453  4242 ppu0 E [CRASH]   #1  rva=0x10F00
"""


def make_root(tmp, report="1", dsn=FAKE_DSN, crash=True, age=0):
    root = os.path.join(tmp, "Project Helghast")
    logs = os.path.join(root, "build", "logs")
    os.makedirs(logs, exist_ok=True)
    with open(os.path.join(root, "kz3-install.ini"), "w", encoding="utf-8") as f:
        f.write(f"[install]\nportable = 0\n\n[crash]\nreport = {report}\ndsn = {dsn}\n")
    with open(os.path.join(root, "build", "paths.json"), "w", encoding="utf-8") as f:
        json.dump({"dump": "D:\\Dumps\\BCUS98234", "update": "D:\\Dumps\\update"}, f)
    with open(os.path.join(root, "manifest.json"), "w", encoding="utf-8") as f:
        json.dump({"repo_commit": "44c59f6a0123456789abcdef"}, f)
    with open(os.path.join(logs, "Killzone3-PC-20261027-101010.log"), "w", encoding="utf-8") as f:
        f.write("        5     1 main I [D3D12] adapter: NVIDIA GeForce RTX 4070 (VRAM 12000 MB)\n")
    if crash:
        add_crash(root, "Killzone3-PC-crash-20261027-101012.txt", age)
    return root


def add_crash(root, name, age=0):
    p = os.path.join(root, "build", "logs", name)
    with open(p, "w", encoding="utf-8") as f:
        f.write(CRASH)
    with open(p[:-4] + ".dmp", "wb") as f:
        f.write(b"MDMP" + b"\0" * 64)
    t = time.time() - age
    os.utime(p, (t, t))
    return p


class Tracker(http.server.BaseHTTPRequestHandler):
    got = []
    answer = (200, {})

    def do_POST(self):  # noqa: N802
        n = int(self.headers.get("Content-Length") or 0)
        Tracker.got.append((self.path, dict(self.headers.items()), self.rfile.read(n)))
        code, hdrs = Tracker.answer
        self.send_response(code)
        for k, v in hdrs.items():
            self.send_header(k, v)
        body = json.dumps({"id": "x"}).encode()
        self.send_header("Content-Length", str(len(body)))
        self.end_headers()
        self.wfile.write(body)

    def log_message(self, *a):
        pass


def main():
    env = {"USERNAME": "alice", "COMPUTERNAME": "DESKTOP-AB12", "USERPROFILE": "C:\\Users\\alice"}

    # 1 scrubbing
    s = cr.scrubber({"<install>": "/ho" "me/alice/Games/Project Helghast", "<dump>": "D:\\Dumps\\BCUS98234"},
                    users=["alice", "Bob"], hosts=["DESKTOP-AB12"], online_ids=["Helghan99"])
    cases = [
        ("Z:\\home\\alice\\Games\\Project Helghast\\build\\kz3.exe", "<install>\\build\\kz3.exe", "install path (Proton form)"),
        ("Z:\\home\\carol\\other\\x.txt", "Z:\\home\\<user>\\other\\x.txt", "Proton Z:\\home\\<n>\\ path"),
        ("C:\\Users\\Bob\\AppData\\Roaming\\x", "C:\\Users\\<user>\\AppData\\Roaming\\x", "Windows C:\\Users\\<n>\\ path"),
        ("c:/users/Dave/x", "c:/users/<user>/x", "C:/Users forward slashes"),
        ("/ho" "me/carol/.steam/x", "/ho" "me/<user>/.steam/x", "Linux /home/<n>/"),
        ("C:\\users\\steamuser\\drive_c", "C:\\users\\<user>\\drive_c", "Wine steamuser"),
        ("d:/dumps/bcus98234/PS3_GAME/USRDIR", "<dump>/PS3_GAME/USRDIR", "dump path, other case and slashes"),
        ("C:\\Program Files\\x.exe", "C:\\Program Files\\x.exe", "neg: a path with no user folder"),
        ("built on DESKTOP-AB12 (desktop-ab12.lan)", "built on <host> (<host>.lan)", "machine name, any case"),
        ("Killzone3-PC-crash-1.txt DESKTOP-AB12X", "Killzone3-PC-crash-1.txt DESKTOP-AB12X", "neg: machine name inside a longer word"),
        ("from 192.168.1.20:3074", "from <ip>:3074", "IPv4"),
        ("bind 127.0.0.1 and 0.0.0.0, v1.2.3, 999.1.1.1", "bind 127.0.0.1 and 0.0.0.0, v1.2.3, 999.1.1.1", "neg: loopback, versions, not an address"),
        ("peer fe80::1ff:fe23:4567:890a%eth0 and 2001:db8::7", "peer <ip> and <ip>", "IPv6"),
        ("at 12:34:56, ::1, Foo::Bar, a::b::c", "at 12:34:56, ::1, Foo::Bar, a::b::c", "neg: times, loopback, C++ names"),
        ("mail bob.smith@example.org now", "mail <email> now", "e-mail"),
        ("cellFs@0x1234 x@y", "cellFs@0x1234 x@y", "neg: not an e-mail"),
        ("mac 00:1A:2B:3C:4D:5E", "mac <mac>", "MAC address"),
        ("\\\\NAS01\\games\\kz3", "\\\\<host>\\games\\kz3", "UNC host"),
        ("GET https://srv.example.com/api", "GET https://<host>/api", "URL host"),
        ('ticket_file: issued to "Zed_1", but online_id is "Helghan99"', 'ticket_file: issued to "<online-id>", but online_id is "<online-id>"', "online-id phrases"),
        ("player Helghan99 joined", "player <online-id> joined", "known online id as a word"),
        ("host = medius.example.net", "host = <host>", "host key"),
        ("Alice wrote to alice's folder", "<user> wrote to <user>'s folder", "user name as a word"),
        ("Bobby and Bob", "Bobby and Bob", "neg: 3-letter user name stays as a bare word, longer word kept"),
    ]
    for src, want, what in cases:
        got = s(src)
        check(got == want, "1 scrub: " + what, f"{src!r} -> {got!r}, want {want!r}")
    with tempfile.TemporaryDirectory() as tmp:
        p = os.path.join(tmp, "online.ini")
        with open(p, "w", encoding="utf-8") as f:
            f.write("online_id = Tester_7\n[server Home]\nhost = 10.0.0.5 ; lan\n")
        ids, hosts = cr.online_ini_values(p)
        check(ids == {"Tester_7"} and hosts == {"10.0.0.5"}, "1 online.ini values", f"{ids} {hosts}")

    # 2 cap
    big = "\n".join(f"line {i} " + "x" * 500 for i in range(400))
    c = cr.cap_log(big)
    check(len(c.splitlines()) <= 200 and len(c.encode()) <= 64 * 1024 and c.endswith("x" * 500)
          and c.splitlines()[-1].startswith("line 399"), "2 cap: last lines, 200 lines and 64 KB")
    check(len(cr.cap_log("\n".join(str(i) for i in range(300))).splitlines()) == 200, "2 cap: 200 lines")

    # 3 DSN
    url, key = cr.parse_dsn(FAKE_DSN)
    check(url == "https://example.invalid/api/2/envelope/" and key == "0123456789abcdef0123456789abcdef", "3 dsn parsed", url)
    url2, _ = cr.parse_dsn("https://k@example.invalid:8443/sentry/7")
    check(url2 == "https://example.invalid:8443/sentry/api/7/envelope/", "3 dsn with port and prefix", url2)
    for bad, what in (("http://k@example.invalid/2", "http refused"), ("http://k@127.0.0.1:9/2", "http loopback refused without the flag"),
                      ("https://example.invalid/2", "no key"), ("https://k@example.invalid/", "no project"), ("", "empty")):
        try:
            cr.parse_dsn(bad)
            check(False, "3 dsn: " + what)
        except ValueError:
            check(True, "3 dsn: " + what)
    try:
        cr.post("http://example.invalid/api/2/envelope/", "k", b"x")
        check(False, "3 post refuses http")
    except ValueError:
        check(True, "3 post refuses http")

    # 4 event
    with tempfile.TemporaryDirectory() as tmp:
        root = make_root(tmp)
        crash = os.path.join(root, "build", "logs", "Killzone3-PC-crash-20261027-101012.txt")
        sc = cr.machine_scrubber(root, env)
        ev = cr.build_event(root, crash, sc, event_id="a" * 32)
        blob = json.dumps(ev)
        check(ev["level"] == "fatal" and ev["platform"] == "native" and ev["environment"] == "release", "4 event level/platform/environment")
        check(ev["release"] == "kz3@" + cr.names.VERSION + "+44c59f6a0123", "4 release kz3@<version>+<commit>", ev["release"])
        check(ev["message"]["formatted"] == "unhandled exception 0xC0000005", "4 message = reason")
        fr = ev["exception"]["values"][0]["stacktrace"]["frames"]
        check([f["function"] for f in fr] == ["kz3.exe+0x10F00", "kz3.exe+0x1A2B3C", "guest 0x0012ABCD"], "4 frames outermost first", str(fr))
        check(ev["tags"].get("gpu_vendor") == "NVIDIA" and ev["tags"].get("renderer") == "d3d12", "4 tags", str(ev["tags"]))
        check("user" not in ev and "server_name" not in ev, "4 no user, no server_name")
        for leak in ("alice", "Bob", "Helghan99", "192.168.1.20", "medius.example.net", "BCUS98234", "DESKTOP-AB12", "RTX"):
            check(leak.lower() not in blob.lower(), f"4 event has no {leak!r}")
        check("<dump>\\PS3_GAME" in ev["extra"]["log"] and len(ev["breadcrumbs"]["values"]) == 7, "4 scrubbed log and breadcrumbs")

        # 5 envelope
        env_b = cr.envelope(ev, FAKE_DSN, sent_at="2026-10-27T10:00:00Z")
        lines = env_b.split(b"\n")
        h, item = json.loads(lines[0]), json.loads(lines[1])
        body = env_b[len(lines[0]) + len(lines[1]) + 2:-1]
        check(h == {"event_id": "a" * 32, "sent_at": "2026-10-27T10:00:00Z", "dsn": FAKE_DSN}, "5 envelope header", str(h))
        check(item["type"] == "event" and item["length"] == len(body) and item["content_type"] == "application/json", "5 item header")
        check(json.loads(body.decode())["event_id"] == "a" * 32 and env_b.endswith(b"\n"), "5 JSON body")

    # 6-10 sending
    srv = http.server.HTTPServer(("127.0.0.1", 0), Tracker)
    threading.Thread(target=srv.serve_forever, daemon=True).start()
    local = f"http://0123456789abcdef0123456789abcdef@127.0.0.1:{srv.server_address[1]}/2"
    try:
        for report, dsn, what in (("0", local, "opt-out"), ("1", "", "empty dsn"), ("0", "", "both")):
            with tempfile.TemporaryDirectory() as tmp:
                root = make_root(tmp, report, dsn)
                Tracker.got = []
                r = cr.Session(root, env, allow_http_loopback=True).check()
                check(r is None and Tracker.got == [] and not os.path.exists(os.path.join(root, "logs", cr.STATE)),
                      f"6 {what}: nothing sent, nothing written")
        with tempfile.TemporaryDirectory() as tmp:
            root = make_root(tmp, "1", local)
            Tracker.got, Tracker.answer = [], (200, {"x-sentry-rate-limits": "86400:transaction;span"})
            logs = []
            opened = []
            real_open = builtins.open
            builtins.open = lambda f, *a, **k: (opened.append(str(f)), real_open(f, *a, **k))[1]
            try:
                ses = cr.Session(root, env, log=logs.append, allow_http_loopback=True)
                r = ses.check()
            finally:
                builtins.open = real_open
            check(r and r["status"] == "sent" and len(Tracker.got) == 1, "7 one envelope sent", f"{r} {len(Tracker.got)}")
            path, hdrs, data = Tracker.got[0]
            check(path == "/api/2/envelope/" and hdrs.get("Content-Type") == "application/x-sentry-envelope"
                  and "sentry_key=0123456789abcdef0123456789abcdef" in hdrs.get("X-Sentry-Auth", ""), "7 endpoint and auth header")
            check(json.loads(data.split(b"\n")[0])["dsn"] == local, "7 envelope carries the dsn")
            check(not any(o.endswith(".dmp") for o in opened), "8 the .dmp is never opened", str(opened))
            check(not ses.stopped, "7 a transaction-only rate limit does not stop crash reports")
            st = json.load(open(os.path.join(root, "logs", cr.STATE)))
            check(list(st["files"].values())[0]["status"] == "sent", "7 sent marker written")
            add_crash(root, "Killzone3-PC-crash-20261027-111111.txt")
            r2 = ses.check()
            check(r2 and r2["status"] == "limit" and len(Tracker.got) == 1, "7 at most one event per session", str(r2))
            r3 = cr.Session(root, env, allow_http_loopback=True).check()
            check(r3 and r3["status"] == "sent" and len(Tracker.got) == 2, "7 the next session sends the new crash")
            r4 = cr.Session(root, env, allow_http_loopback=True).check()
            check(r4 is None and len(Tracker.got) == 2, "7 sent marker prevents a resend")
            check(cr.notice(r) .startswith("A crash report was sent"), "7 notice text")
        # 9 rate limit
        with tempfile.TemporaryDirectory() as tmp:
            root = make_root(tmp, "1", local)
            Tracker.got, Tracker.answer = [], (429, {"Retry-After": "120", "x-sentry-rate-limits": "120::organization"})
            ses = cr.Session(root, env, allow_http_loopback=True)
            r = ses.check()
            check(r["status"] == "rate-limited" and ses.stopped and len(Tracker.got) == 1, "9 429 stops the session", str(r))
            r2 = cr.Session(root, env, allow_http_loopback=True).check()
            check(r2["status"] == "rate-limited" and len(Tracker.got) == 1, "9 the wait holds in the next session")
            Tracker.answer = (200, {})
            r3 = cr.Session(root, env, allow_http_loopback=True, now=lambda: time.time() + 200).check()
            check(r3["status"] == "sent" and len(Tracker.got) == 2, "9 after the wait the kept crash is sent")
            check(cr.rate_limit_seconds(200, {"x-sentry-rate-limits": "60:error:key, 10:transaction"}) == 60, "9 header parsing")
        # 10 old crashes
        with tempfile.TemporaryDirectory() as tmp:
            root = make_root(tmp, "0", local)
            Tracker.got = []
            cr.Session(root, env).mark_existing()
            cr.set_report(root, True)
            check(cr.read_settings(root) == (True, local), "10 set_report keeps the dsn")
            r = cr.Session(root, env, allow_http_loopback=True).check()
            check(r is None and Tracker.got == [], "10 crashes from before opt-in are not sent")
        with tempfile.TemporaryDirectory() as tmp:
            root = make_root(tmp, "1", local, age=8 * 86400)
            Tracker.got = []
            r = cr.Session(root, env, allow_http_loopback=True).check()
            check(r is None and Tracker.got == [], "10 a crash older than 7 days is not sent")
        # installer path: a crash file older than kz3-install.ini (the opt-in was recorded after it) is never sent
        with tempfile.TemporaryDirectory() as tmp:
            root = make_root(tmp, "1", local)
            ini = os.path.join(root, cr.INI)
            t_now = time.time()
            os.utime(ini, (t_now + 60, t_now + 60))
            Tracker.got = []
            r = cr.Session(root, env, allow_http_loopback=True, now=lambda: t_now + 120).check()
            check(r is None and Tracker.got == [], "11 a crash from before the installer's opt-in is not sent")
        # unreachable tracker: silent failure, a launcher log line, no resend
        with tempfile.TemporaryDirectory() as tmp:
            dead = http.server.HTTPServer(("127.0.0.1", 0), Tracker)
            port = dead.server_address[1]
            dead.server_close()
            root = make_root(tmp, "1", f"http://k@127.0.0.1:{port}/2")
            logs = []
            r = cr.Session(root, env, log=logs.append, allow_http_loopback=True).check()
            r2 = cr.Session(root, env, log=logs.append, allow_http_loopback=True).check()
            check(r["status"] == "failed" and r2 is None and len(logs) == 1 and "not sent" in logs[0], "7 offline: one log line, not retried", str(logs))
    finally:
        srv.shutdown()

    # 11 launcher entry
    class UI:
        def __init__(self, yes):
            self.yes, self.said = yes, []

        def ask(self, t):
            self.said.append(t)
            return self.yes

        def info(self, t):
            self.said.append(t)

    with tempfile.TemporaryDirectory() as tmp:
        root = make_root(tmp, "0", "")
        ui = UI(True)
        la.main(["--root", root, "--crash-reports"], ui=ui, environ={})
        check(len(ui.said) == 1 and cr.UNAVAILABLE in ui.said[0] and cr.read_settings(root) == (False, ""), "11 no dsn: not available in this build")
        shutil.rmtree(root)
        root = make_root(tmp, "0", FAKE_DSN)
        ui = UI(True)
        la.main(["--root", root, "--crash-reports"], ui=ui, environ={})
        st = json.load(open(os.path.join(root, "logs", cr.STATE)))
        check(cr.read_settings(root)[0] and "Turn them on?" in ui.said[0] and ui.said[-1] == "Crash reports are on."
              and list(st["files"].values())[0]["status"] == "before opt-in", "11 switch on (old crashes marked)")
        ui = UI(True)
        la.main(["--root", root, "--crash-reports"], ui=ui, environ={})
        check(not cr.read_settings(root)[0] and "Turn crash reports off?" in ui.said[0], "11 switch off")
        ui = UI(False)
        la.main(["--root", root, "--crash-reports"], ui=ui, environ={})
        check(not cr.read_settings(root)[0] and len(ui.said) == 1, "11 No changes nothing")

    # 12 support package never carries the dsn
    with tempfile.TemporaryDirectory() as tmp:
        root = make_root(tmp, "1", FAKE_DSN)
        dest, _ = support.build(root, os.path.join(tmp, "data"), os.path.join(tmp, "out"), lambda t: t, "20261027-000000")
        with zipfile.ZipFile(dest) as z:
            ini = z.read("kz3-install.ini").decode()
        check("example.invalid" not in ini and "dsn = <set>" in ini, "12 support package: dsn reduced to <set>", ini)

    print("FAIL" if FAILS else "PASS", f"({len(FAILS)} failed)" if FAILS else "")
    return 1 if FAILS else 0


if __name__ == "__main__":
    sys.exit(main())
