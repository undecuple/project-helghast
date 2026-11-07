#!/usr/bin/env python3
"""Game-free selector, opt-in, synthetic ticket and RT tests against the patched runtime.

Run after tools/ps3recomp-patched.sh: python3 tools/net/test_network.py
The fixture mocks host resolution and sockets; it never contacts a server.
"""
import os
from pathlib import Path
import shlex
import shutil
import subprocess
import sys

ROOT = Path(__file__).resolve().parents[2]
RUNTIME = ROOT / "work" / "ps3recomp-patched"
WORK = ROOT / "work"


def command(args, env):
    result = subprocess.run(args, cwd=ROOT, env=env, text=True, errors="replace", capture_output=True)
    if result.returncode:
        raise RuntimeError(f"command failed ({result.returncode}): {args[0]}\n{result.stdout}\n{result.stderr}")
    return result.stdout


def compiler():
    selected = os.environ.get("CC")
    if selected:
        if shutil.which(selected):
            return [selected]
        return shlex.split(selected, posix=os.name != "nt")
    found = shutil.which("cc") or shutil.which("gcc") or shutil.which("clang")
    if not found:
        raise RuntimeError("C compiler not found; the network host test cannot run")
    return [found]


def main():
    if not (RUNTIME / "libs" / "network" / "net_policy.c").is_file():
        raise RuntimeError("patched runtime missing: run tools/ps3recomp-patched.sh first")
    WORK.mkdir(exist_ok=True)
    prefix = f"net-evidence-{os.getpid()}"
    exe = WORK / (prefix + (".exe" if os.name == "nt" else ""))
    np_exe = WORK / (prefix + "-np" + (".exe" if os.name == "nt" else ""))
    np_obj = WORK / (prefix + "-np.o")
    hle_obj = WORK / (prefix + "-hle.o")
    config = WORK / (prefix + ".ini")
    ticket = WORK / (prefix + ".ticket")
    env = {k: v for k, v in os.environ.items()
           if not k.upper().startswith(("KZ3_", "PS3_NET_", "PS3_NP_", "PSNR_"))}
    # Compiler scratch files, if any, stay in the checkout rather than a system temporary directory.
    env.update(TEMP=str(ROOT), TMP=str(ROOT), TMPDIR=str(ROOT))
    env.update(KZ3_NET_CONFIG=str(config), KZ3_NET_TEST_TICKET=str(ticket))
    try:
        build = compiler() + [
            "-std=gnu11", "-O1", "-Wall", "-Wextra",
            "-I" + str(RUNTIME / "runtime" / "ppu"),
            "-I" + str(RUNTIME / "libs" / "network"),
            "-I" + str(ROOT / "port" / "net"),
            str(ROOT / "tools" / "net" / "test_network.c"),
            str(ROOT / "tools" / "net" / "host_stubs.c"),
            str(ROOT / "port" / "net" / "np_kz3.c"),
            str(ROOT / "port" / "net" / "np_ticket.c"),
            str(RUNTIME / "libs" / "network" / "net_policy.c"),
            "-o", str(exe),
        ]
        build += ["-lws2_32"] if os.name == "nt" else ["-pthread"]
        command(build, env)

        includes = [
            "-I" + str(RUNTIME / "include"),
            "-I" + str(RUNTIME / "runtime" / "ppu"),
            "-I" + str(RUNTIME / "libs" / "network"),
            "-I" + str(ROOT / "port" / "net"),
        ]
        flags = ["-std=gnu11", "-O1", "-Wall", "-Wextra"] + includes
        command(compiler() + flags + [
            "-Dpsnr_connect=fixture_psnr_connect", "-c",
            str(RUNTIME / "libs" / "network" / "np_psnr.c"), "-o", str(np_obj),
        ], env)
        command(compiler() + [
            "-std=gnu++17", "-x", "c++", "-DKZ3_WITH_NET",
            "-I" + str(ROOT / "tools" / "upstream" / "stub"),
            "-c", str(ROOT / "port" / "kz3" / "hle_kz3.cpp"), "-o", str(hle_obj),
        ], env)
        command(compiler() + [
            "-std=gnu++17", "-x", "c++", "-fsyntax-only",
            "-I" + str(ROOT / "tools" / "upstream" / "stub"),
            str(ROOT / "port" / "kz3" / "hle_kz3.cpp"),
        ], env)
        command(compiler() + flags + [
            str(ROOT / "tools" / "net" / "test_np_gate.c"),
            str(ROOT / "tools" / "net" / "host_stubs.c"),
            str(ROOT / "port" / "net" / "np_kz3.c"),
            str(ROOT / "port" / "net" / "np_ticket.c"),
            str(RUNTIME / "libs" / "network" / "net_policy.c"),
            str(RUNTIME / "libs" / "network" / "sceNp.c"),
            str(RUNTIME / "libs" / "network" / "sceNpScore.c"),
            str(RUNTIME / "libs" / "network" / "sceNpMatching2.c"),
            str(RUNTIME / "libs" / "network" / "psnr" / "psnr.c"),
            str(np_obj), str(hle_obj), "-o", str(np_exe),
        ] + (["-lws2_32"] if os.name == "nt" else ["-pthread"]), env)

        def run(mode, extra=None):
            return command([str(exe), mode], dict(env, **(extra or {})))

        def run_np(mode, extra=None):
            np_env = dict(env, KZ3_ONLINE="1", PSNR_SERVER="psnr-fixture.invalid:36100")
            np_env.update(extra or {})
            return command([str(np_exe), mode], np_env)

        run("make-ticket")
        base = f"host = fallback.invalid\nonline_id = TestUser1\nticket_file = {ticket.name}\n"
        sections = ("[server first]\nhost = first.invalid\n"
                    "[server second]\nhost = selected.invalid\n")
        valid = base + "server = second\n" + sections
        config.write_bytes(valid.encode())

        command([str(np_exe), "good"], dict(env, KZ3_ONLINE="1"))
        run_np("positive-score")
        run_np("positive-matching")
        run_np("offline", {"KZ3_ONLINE": "0"})
        config.write_bytes((base + "server = absent\n" + sections).encode())
        run_np("closed")
        config.write_bytes((base + "sever = second\n" + sections).encode())
        run_np("closed")
        config.write_bytes(("host = fallback.invalid\nfuture_option = enabled\n" + sections).encode())
        run_np("closed")
        config.write_bytes(valid.encode())
        run_np("closed", {"KZ3_NET_SERVER": "invalid@host"})
        run_np("closed-unresolved", {"KZ3_NET_TEST_RESOLVE_FAIL": "1"})
        config.write_bytes(valid.replace("online_id = TestUser1", "online_id = OtherUser").encode())
        run_np("closed-ticket")
        config.write_bytes(valid.encode())
        print("PASS patched NP status and outbound Score/Matching psnr gating")

        for value in (None, "", "0"):
            extra = {"KZ3_NET_SERVER": "override.invalid", "KZ3_NET_CAPTURE": "1"}
            if value is not None:
                extra["KZ3_ONLINE"] = value
            run("offline", extra)
        run("cache-off", {"KZ3_ONLINE": "0"})
        run("cache-on", {"KZ3_ONLINE": "false"})
        print("PASS offline default, opt-in values and cached switch")

        run("functional", {"KZ3_ONLINE": "1"})
        print("PASS config, ticket shape, copy and guest callback")

        good = {"KZ3_ONLINE": "1", "KZ3_NET_EXPECT_HOST": "selected.invalid", "KZ3_NET_EXPECT_TICKET": "1"}
        run("good", good)
        config.write_bytes(valid.replace("online_id = TestUser1\n", "").encode())
        run("good", good)
        config.write_bytes((base + sections).encode())
        run("good", dict(good, KZ3_NET_EXPECT_HOST="fallback.invalid"))
        config.write_bytes((f"online_id = TestUser1\nticket_file = {ticket.name}\n" + sections).encode())
        run("good", dict(good, KZ3_NET_EXPECT_HOST="first.invalid"))
        non_sensitive = valid.replace(f"ticket_file = {ticket.name}\n", "").replace("online_id = TestUser1\n", "")
        config.write_bytes((non_sensitive + "unused = ignored\n").encode())
        run("good", {"KZ3_ONLINE": "1", "KZ3_NET_EXPECT_HOST": "selected.invalid", "KZ3_NET_EXPECT_NO_ID": "1"})
        config.write_bytes(valid.encode())
        run("good", dict(good, KZ3_NET_SERVER="override.invalid", KZ3_NET_EXPECT_HOST="override.invalid"))
        print("PASS explicit section selection, normal defaults and synthetic ticket loading")

        bad = {
            "unknown name": "server = absent\n",
            "empty name": "server =\n",
            "invalid name": "server = bad!\n",
            "long name": "server = " + "x" * 33 + "\n",
            "quoted selector": 'server = "second"\n',
            "unterminated selector": 'server = "second\n',
            "missing equals": "server second\n",
            "selector-shaped key": "server_name = second\n",
            "selector-shaped camel key": "selectedServer = second\n",
            "selector-shaped joined key": "selectedserver = second\n",
            "selector-shaped suffix key": "selected_server = second\n",
            "misspelled selector": "sever = second\n",
            "unknown key": "future_option = enabled\n",
            "unknown key with quoted value": 'future_option = "enabled"\n',
            "misspelled key without equals": "sever second\n",
            "punctuated selector": "server? = second\n",
            "quoted key": '"server" = second\n',
            "repeated selector": "server = first\nserver = second\n",
            "invalid then valid selector": "server = bad!\nserver = second\n",
            "valid then empty selector": "server = second\nserver =\n",
            "misplaced selector": "[server misplaced]\nserver = second\n",
            "selector in unknown section": "[other]\nserver = second\n",
            "malformed section": "[serverfirst]\n",
            "unknown server-shaped section": "[myserver]\n",
            "unterminated section": "[server first\n",
            "invalid section name": "[server bad!]\n",
        }
        deny_env = {
            "KZ3_ONLINE": "1", "KZ3_NET_SERVER": "override.invalid",
            "KZ3_NET_CAPTURE": "1", "KZ3_NET_TEST_LOOPBACK": "1",
        }
        for label, fragment in bad.items():
            config.write_bytes((base + fragment + sections).encode())
            try:
                run("bad", deny_env)
            except RuntimeError as error:
                raise RuntimeError(f"selector case {label}: {error}") from error
        # A ticket path is real and synthetic in every case; order and section must not hide an unknown key.
        ticket_only = base.replace("online_id = TestUser1\n", "")
        for fragment in ("sever = second\n", "future_option = enabled\n", 'future_option = "enabled"\n'):
            config.write_bytes((ticket_only + fragment + sections).encode())
            run("bad", deny_env)
        config.write_bytes(("future_option = enabled\n" + valid.replace("online_id = TestUser1\n", "")).encode())
        run("bad", deny_env)
        config.write_bytes(("future_option = enabled\n" + valid).encode())
        run("bad", deny_env)
        config.write_bytes((valid + "future_option = enabled\n").encode())
        run("bad", deny_env)
        config.write_bytes((base + "[other]\nfuture_option = enabled\n[server]\nserver = second\n" + sections).encode())
        run("bad", deny_env)
        config.write_bytes((base + "[sever second]\nhost = fallback.invalid\n" + sections).encode())
        run("bad", deny_env)
        config.write_bytes(valid.replace(f"ticket_file = {ticket.name}",
                                         f'ticket_file = "{ticket.name}"').encode())
        run("bad", deny_env)
        config.write_bytes((valid.replace(f"ticket_file = {ticket.name}\n", "") + "future_option = enabled\n").encode())
        run("bad", deny_env)
        config.write_bytes((non_sensitive + "future_option = enabled\n").encode())
        run("bad", dict(deny_env, KZ3_NET_ONLINE_ID="TestUser1"))
        config.write_bytes((base + "server = first\n" + sections + "[server FIRST]\nhost = other.invalid\n").encode())
        run("bad", deny_env)
        config.write_bytes((base + "".join(f"[server s{i}]\nhost = s{i}.invalid\n" for i in range(9))).encode())
        run("bad", deny_env)
        config.write_bytes(base.encode() + b"server\x00 = second\n" + sections.encode())
        run("bad", deny_env)
        config.write_bytes((base + "unknown = " + "x" * 600 + "\n" + sections).encode())
        run("bad", deny_env)
        config.write_bytes((base + "x" * (64 * 1024 + 1)).encode())
        run("bad", deny_env)
        config.write_bytes(valid.encode())
        run("bad", dict(deny_env, KZ3_NET_TEST_OPEN_ERROR="1"))
        run("bad", dict(deny_env, KZ3_NET_TEST_SHORT_READ="1"))
        run("bad", dict(deny_env, KZ3_NET_SERVER="invalid@host"))
        run("bad", dict(deny_env, KZ3_NET_SERVER=""))
        config.write_bytes(f"online_id = TestUser1\nticket_file = {ticket.name}\n".encode())
        run("bad", {"KZ3_ONLINE": "1"})
        config.write_bytes(valid.encode())
        run("unresolved", {"KZ3_ONLINE": "1", "KZ3_NET_TEST_RESOLVE_FAIL": "1"})
        config.write_bytes(valid.replace("online_id = TestUser1", "online_id = OtherUser").encode())
        run("bad-ticket", {"KZ3_ONLINE": "1"})
        config.write_bytes(valid.replace(ticket.name, "missing-synthetic.ticket").encode())
        run("bad-ticket", {"KZ3_ONLINE": "1"})
        config.write_bytes(valid.replace(f"ticket_file = {ticket.name}\n", "").encode())
        run("policy-failure", {"KZ3_ONLINE": "1", "KZ3_NET_TEST_ALLOW_FAIL": "1"})
        print(f"PASS {len(bad) + 25} fail-closed selector, file, override, resolution and policy cases")

        output = run("rt")
        lower = output.lower()
        for marker in ("payload redacted", "client_hello", "medius class 4 type 0x58",
                       "client_connect_tcp", "encrypted (not decoded)", "0x3b part 1",
                       "cookie 4 bytes", "trailing bytes"):
            if marker not in lower:
                raise AssertionError(f"missing RT diagnostic: {marker}")
        for secret in ("91 a2 b3 c4", "30 82 00 04", "de ad be ef", "f1 e2 d3 c4",
                       "a1 b2 c3 d4", "9a bc de f0", "deadbeef", "fnv", "key word",
                       "matches online_id", "ticket at message offset",
                       str(int.from_bytes(bytes.fromhex("a1 b2 c3 d4"), "big")),
                       str(int.from_bytes(bytes.fromhex("a1 b2 c3 d4"), "little"))):
            if secret in lower:
                raise AssertionError(f"synthetic RT secret leaked: {secret}")
        print("PASS generated RT framing, encrypted container, replies and payload redaction")
    finally:
        for path in (exe, np_exe, np_obj, hle_obj, config, ticket):
            path.unlink(missing_ok=True)
    return 0


if __name__ == "__main__":
    try:
        sys.exit(main())
    except (OSError, RuntimeError, AssertionError) as error:
        print(f"FAIL network host tests: {error}", file=sys.stderr)
        sys.exit(1)
