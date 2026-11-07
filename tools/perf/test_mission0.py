#!/usr/bin/env python3
"""Synthetic, game-free checks for the native Windows benchmark runner."""

import contextlib
import hashlib
import io
import json
import os
from pathlib import Path
import re
import shutil
import stat
import struct
import subprocess
import sys
from types import SimpleNamespace
import unittest
from unittest import mock
import uuid
import warnings

HERE = Path(__file__).resolve().parent
REPO = HERE.parents[1]
sys.path.insert(0, str(HERE))
import mission0_helper as helper  # noqa: E402
import test_firstrun_check as release_fixture  # noqa: E402


class BenchmarkTests(unittest.TestCase):
    @classmethod
    def setUpClass(cls):
        base = REPO / "work" / "helsari-p0"
        base.mkdir(parents=True, exist_ok=True)
        cls.scratch = base / ("test-" + uuid.uuid4().hex)
        cls.scratch.mkdir()

    @classmethod
    def tearDownClass(cls):
        def remove_readonly(function, path, _error):
            os.chmod(path, stat.S_IWRITE)
            function(path)

        shutil.rmtree(cls.scratch, onerror=remove_readonly)

    def new_dir(self):
        path = self.scratch / uuid.uuid4().hex
        path.mkdir()
        return path

    def assert_reason(self, code, fn, *args, **kwargs):
        with self.assertRaises(helper.BenchmarkError) as caught:
            fn(*args, **kwargs)
        self.assertEqual(str(caught.exception), code)

    def make_profile(self, base):
        source = base / "source"
        profile = source / "dev_hdd0" / "home" / "00000001" / "savedata" / "fixture" / "PROFILE.DAT"
        profile.parent.mkdir(parents=True)
        data = bytearray(helper.profile_edit.SIZE)
        struct.pack_into(">I", data, 0, helper.profile_edit.VERSION_WORD)
        struct.pack_into(">i", data, helper.profile_edit.CONT_LEVEL, 12)
        struct.pack_into(">b", data, helper.profile_edit.CONT_DIFF, 1)
        struct.pack_into(">I", data, helper.profile_edit.CONT_SIZE, helper.profile_edit.CONT_REC_LEN)
        profile.write_bytes(data)
        return source, profile, bytes(data)

    @staticmethod
    def format_action(body):
        action = helper._script_events("0:" + body)[0]["action"]
        if action[0] == "press":
            hold = f"{action[3]} polls" if action[2] == "polls" else f"{action[3]:.2f}s"
            return f"press 0x{action[1]:04X} for {hold}"
        axes = action[1]
        return (f"sticks l=({axes[0]},{axes[1]}) r=({axes[2]},{axes[3]}) "
                f"l2={axes[4]} r2={axes[5]} for {action[3]:.2f}s")

    @staticmethod
    def flip_ms(line):
        match = helper.FLIP.match(line)
        return int(match.group(2)) if match else None

    def make_log(self, base, *, step_ms=250, offset=0.5, actuals=None, emissions=None,
                 host_times=None, end=None):
        route = helper.route()
        log = base / "combined.log"
        if end is None:
            end = helper.WINDOW_END + 6
        host_base = 1_000_000
        records = [(-1, 0, f"[cellPad] PAD_SCRIPT: {route['event_count']} event(s)"),
                   (30001, 0, "[fs] open 'SENSITIVE_TEST_MARKER/levels.single_player.mission_0.fixture' -> fd 1")]
        actuals, emissions, host_times = actuals or {}, emissions or {}, host_times or {}
        for line in route["script"].splitlines():
            time_text, body = line.split(":", 1)
            t = float(time_text)
            if t < helper.WINDOW_END:
                actual = actuals.get(t, t)
                emission = emissions.get(t, actual + offset)
                host_time = host_times.get(t, actual + offset)
                action = self.format_action(body)
                records.append((round(emission * 1000), 1,
                                f"[cellPad] script t={t:.1f}s {action} (at {actual:.1f}s) "
                                f"host={host_base + round(host_time * 1000)} ms"))
        for sample in range(int(end * 1000 / step_ms) + 1):
            ms = round(sample * step_ms)
            n = 1 + round(sample * step_ms * 30 / 1000)
            records.append((ms, 0, f"[flip] {n} at {ms} ms host={host_base + ms} ms"))
        log.write_text("\n".join(record[2] for record in sorted(records)) + "\n", encoding="utf-8")
        return log

    def make_first_boot_log(self, base):
        spec = helper.first_boot_route()
        lines = [f"[cellPad] PAD_SCRIPT: {spec['event_count']} event(s)"]
        for line in spec["script"].splitlines():
            time, body = line.split(":", 1)
            scheduled = float(time)
            action = self.format_action(body)
            lines.append(f"[cellPad] script t={scheduled:.1f}s {action} (at {scheduled:.1f}s) "
                         f"host={1_000_000 + round(scheduled * 1000)} ms")
        log = base / "first-boot.log"
        log.write_text("\n".join(lines) + "\n", encoding="utf-8")
        return log, lines

    def make_developer_build(self):
        base = self.new_dir() / "SENSITIVE_TEST_MARKER"
        repo, build = base / "repo", base / "repo" / "build" / "developer"
        source, patches, toolkit = repo / "port" / "kz3", repo / "port" / "patches", repo / "work" / "ps3recomp-patched"
        for directory in (source, patches, toolkit, build / "CMakeFiles"):
            directory.mkdir(parents=True)
        (source / "CMakeLists.txt").write_text("# synthetic project\n", encoding="utf-8")
        (source / "kz3_ini.h").write_text('"KZ3_CACHE_ROOT"', encoding="utf-8")
        translation = repo / "port" / "text" / "port_ui.tsv"
        translation.parent.mkdir(parents=True)
        translation.write_bytes(b"synthetic-repository-language")
        git = shutil.which("git")
        self.assertIsNotNone(git, "Git is required for synthetic repository fixtures")
        for args in (["init", "--quiet", str(repo)],
                     ["-C", str(repo), "add", "--force", "--", str(translation.relative_to(repo))]):
            result = subprocess.run([git, *args], capture_output=True, timeout=30, check=False)
            self.assertEqual(result.returncode, 0, "synthetic Git fixture setup failed")
        (patches / "001.patch").write_bytes(b"synthetic patch\n")
        stamp = helper.patch_digest(patches) + "-1234567"
        toolkit_stamp = toolkit / ".kz3-patchset"
        toolkit_stamp.write_text(stamp, encoding="ascii")
        cache = build / "CMakeCache.txt"
        cache.write_text(f"CMAKE_HOME_DIRECTORY:INTERNAL={source.as_posix()}\n"
                         f"CMAKE_CACHEFILE_DIR:INTERNAL={build.as_posix()}\n"
                         f"ps3recomp_runtime_SOURCE_DIR:STATIC={toolkit.as_posix()}\n"
                         "CMAKE_GENERATOR:INTERNAL=Ninja\n"
                         "KZ3_EMBED_EBOOT:FILEPATH=\n", encoding="utf-8")
        manifest, rules, exe = build / "build.ninja", build / "CMakeFiles" / "rules.ninja", build / "kz3.exe"
        manifest.write_text("# synthetic ninja file\n", encoding="utf-8")
        rules.write_text("# synthetic rules\n", encoding="utf-8")
        exe.write_bytes(b"MZ synthetic developer executable")
        old, new = 1_600_000_000_000_000_000, 1_600_000_010_000_000_000
        for path in (toolkit_stamp, cache, manifest, rules):
            os.utime(path, ns=(old, old))
        os.utime(exe, ns=(new, new))
        return {"base": base, "repo": repo, "build": build, "source": source, "toolkit": toolkit,
                "stamp": stamp, "toolkit_stamp": toolkit_stamp, "cache": cache, "manifest": manifest,
                "rules": rules, "exe": exe, "old": old, "new": new, "translation": translation}

    def verify_developer_fixture(self, fixture, ninja_stdout="ninja: no work to do.\n", ninja_returncode=0):
        gitlink, head = "1234567" + "0" * 33, "b" * 40
        replies = [subprocess.CompletedProcess([], 0, f"160000 {gitlink} 0\tfixture\n"),
                   subprocess.CompletedProcess([], 0, head + "\n"),
                   subprocess.CompletedProcess([], 0), subprocess.CompletedProcess([], 0),
                   subprocess.CompletedProcess([], ninja_returncode, ninja_stdout, "")]
        with mock.patch.object(helper.shutil, "which",
                               side_effect=lambda name: name if name in ("git", "ninja") else None), \
                mock.patch.object(helper.subprocess, "run", side_effect=replies):
            return helper.verify_developer_build(fixture["repo"], fixture["build"])

    def configure_embedded_fixture(self, fixture, eboot):
        cache = fixture["cache"]
        content = cache.read_text(encoding="utf-8")
        content = re.sub(r"^KZ3_EMBED_EBOOT:FILEPATH=.*$", f"KZ3_EMBED_EBOOT:FILEPATH={eboot.as_posix()}",
                         content, flags=re.M)
        cache.write_text(content, encoding="utf-8")
        header = fixture["build"] / "kz3_embed_gen" / "kz3_eboot_embed.h"
        header.parent.mkdir(exist_ok=True)
        digest = hashlib.sha256(eboot.read_bytes()).hexdigest()
        header.write_text(f'/* synthetic configure output */\n#define KZ3_EMBED_EBOOT_FILE "{eboot.as_posix()}"\n'
                          f'#define KZ3_EMBED_EBOOT_SHA256 "{digest}"\n', encoding="utf-8")
        for path in (cache, header, eboot):
            os.utime(path, ns=(fixture["old"], fixture["old"]))
        return header, digest

    def make_install_resources(self, install):
        for directory in ("runtime", "fonts"):
            (install / directory).mkdir(parents=True, exist_ok=True)
        (install / "runtime" / "SDL3.dll").write_bytes(b"synthetic-sdl")
        for name in helper.shaders.fr.NOTICE_FONTS:
            (install / "fonts" / name).write_bytes(("synthetic-" + name).encode("ascii"))

    def make_stale_install(self, base):
        root = base / "install"
        build = root / "build"
        dump, update = root / "fixture-dump", root / "fixture-update"
        eboot = update / "USRDIR" / "synthetic eboot with spaces.elf"
        for directory in (build, dump, eboot.parent, root / "fixture-fonts"):
            directory.mkdir(parents=True)
        with warnings.catch_warnings():
            warnings.simplefilter("ignore", ResourceWarning)
            release_fixture.write_sfo(str(update / "PARAM.SFO"),
                                      {"APP_VER": "01.14", "CATEGORY": "GD", "TITLE": "Synthetic",
                                       "TITLE_ID": "BCUS98234"})
            release_fixture.add_update_edat(str(update))
        (update / "USRDIR" / "EBOOT.BIN").write_bytes(b"synthetic-update")
        eboot.write_bytes(b"synthetic-elf")
        (build / "kz3.exe").write_bytes(b"MZ stale installed executable")
        (root / "portable.txt").write_bytes(b"")
        paths = {"dump": "fixture-dump", "update": "fixture-update",
                 "eboot": str(eboot.relative_to(root)), "font_dir": "fixture-fonts"}
        (build / "paths.json").write_text(json.dumps(paths), encoding="utf-8")
        stamp = {"root": str(root), "cache_root": False, "eboot_embedded": "old-embedded",
                 "eboot_stat": helper.shaders.fr.eboot_stat(str(eboot)),
                 "parts": {"base": "stale", "lift": "stale", "runtime": "stale"}}
        (build / "build-stamp.json").write_text(json.dumps(stamp), encoding="utf-8")
        self.make_install_resources(root)
        return {"root": root, "build": build, "dump": dump, "update": update, "eboot": eboot,
                "paths": paths, "stamp": stamp}

    def test_existing_route_and_fixed_window(self):
        spec = helper.route()
        script, duration = helper.shaders.route_script("profile")
        self.assertEqual(spec["script"], script)
        self.assertEqual((spec["duration_seconds"], spec["window_start"], spec["window_end"]),
                         (duration, 265, 325))
        self.assertEqual(spec["event_count"], len(script.splitlines()))
        self.assertEqual(spec["clock"], "flip_ms_since_first_flip")

    def test_windows_ci_invokes_synthetic_process_fixture(self):
        workflow = (REPO / ".github" / "workflows" / "ci.yml").read_text(encoding="utf-8")
        windows = workflow.split("  release-tools-windows:", 1)[1].split("\n  secrets:", 1)[0]
        self.assertIn("runs-on: windows-latest", windows)
        self.assertRegex(windows, r"(?m)^\s+run:\s*python3 -B tools/perf/test_mission0\.py\s*$")
        host_tests = (REPO / "tools" / "host-tests.txt").read_text(encoding="utf-8").splitlines()
        self.assertIn("tools/perf/test_mission0.py", host_tests)

    def test_real_launcher_print_is_read_only_for_synthetic_roots(self):
        root = self.new_dir() / "install"
        root.mkdir()
        result = helper.readonly_launcher(root)
        self.assertEqual(result["action"], "first-build")
        self.assertEqual(list(root.iterdir()), [])
        build = root / "build"
        build.mkdir()
        stamp = build / "build-stamp.json"
        original = json.dumps({"root": str(root.parent / "other-install")}).encode("utf-8")
        stamp.write_bytes(original)
        self.assert_reason("launcher_moved_root", helper.readonly_launcher, root)
        self.assertEqual(stamp.read_bytes(), original)
        self.assertEqual(list(build.iterdir()), [stamp])

    def test_staged_ini_paths_keep_launcher_base(self):
        root = self.new_dir() / "install"
        root.mkdir()
        result = {"ini": {"eboot": r"game\fixture.elf", "font_dir": r"fonts\fixture", "lang": "en"}}
        helper.absolute_launcher_ini_paths(result, root)
        self.assertEqual(result["ini"]["eboot"], os.path.normpath(str(root / "build" / r"game\fixture.elf")))
        self.assertEqual(result["ini"]["font_dir"], os.path.normpath(str(root / "build" / r"fonts\fixture")))
        self.assertEqual(result["ini"]["lang"], "en")
        header = root / "port" / "kz3" / "kz3_ini.h"
        header.parent.mkdir(parents=True)
        header.write_text('"root"', encoding="utf-8")
        result = {"ini": {"eboot": r"game\fixture.elf"}}
        helper.absolute_launcher_ini_paths(result, root)
        self.assertEqual(result["ini"]["eboot"], os.path.normpath(str(root / r"game\fixture.elf")))

    def test_private_profile_seed_and_no_fallback(self):
        base = self.new_dir()
        source, profile, original = self.make_profile(base)
        dest = base / "session" / "seed"
        self.assertTrue(helper.prepare_seed(source, dest)["ok"])
        copied = dest / "gamedata" / profile.relative_to(source)
        self.assertEqual(profile.read_bytes(), original)
        info = helper.profile_edit.continue_info(copied.read_bytes())
        self.assertEqual((info["level_id"], info["diff"], info["rec_size"]), (-1, -1, 0))
        self.assert_reason("seed_destination_invalid", helper.prepare_seed, source, dest)
        self.assert_reason("profile_missing", helper.prepare_seed, base / "absent", base / "session2" / "seed")
        second = source / "dev_hdd0" / "home" / "00000001" / "savedata" / "second" / "PROFILE.DAT"
        second.parent.mkdir(parents=True)
        second.write_bytes(original)
        self.assert_reason("profile_count", helper.prepare_seed, source, base / "session3" / "seed")
        second.unlink()
        profile.write_bytes(b"synthetic invalid")
        self.assert_reason("profile_invalid", helper.prepare_seed, source, base / "session4" / "seed")

    def test_profile_inventory_is_explicit_and_never_mutates_source(self):
        base = self.new_dir()
        session = base / "session"
        missing = base / "missing" / "gamedata"
        self.assertEqual(helper.inspect_profile(missing, session)["profile_count"], 0)
        self.assertFalse(missing.exists())
        self.assert_reason("profile_missing", helper.prepare_seed, missing, session / "seed")
        empty = base / "empty"
        empty.mkdir()
        self.assertEqual(helper.inspect_profile(empty, session)["profile_count"], 0)
        self.assert_reason("profile_count", helper.prepare_seed, empty, session / "seed")
        source, profile, original = self.make_profile(base)
        self.assertEqual(helper.inspect_profile(source, session)["profile_count"], 1)
        self.assertEqual(profile.read_bytes(), original)
        second = source / "dev_hdd0" / "home" / "00000001" / "savedata" / "second" / "PROFILE.DAT"
        second.parent.mkdir(parents=True)
        second.write_bytes(original)
        self.assert_reason("profile_count", helper.inspect_profile, source, session)
        self.assertFalse(session.exists())

    def test_profile_inventory_rejects_missing_overlaps_links_and_vfs(self):
        base = self.new_dir()
        missing = base / "missing"
        self.assert_reason("profile_source_overlap", helper.inspect_profile, missing, missing / "session")
        self.assert_reason("profile_source_overlap", helper.inspect_profile, missing / "profile", missing)
        original_lstat = Path.lstat

        def reparse_ancestor(path):
            info = original_lstat(path)
            if path == base:
                return SimpleNamespace(st_mode=info.st_mode, st_file_attributes=0x400)
            return info

        with mock.patch.object(Path, "lstat", reparse_ancestor):
            self.assert_reason("profile_contains_link", helper.inspect_profile, missing, base / "session")
        source = base / "source"
        forbidden = source / "vfs"
        forbidden.mkdir(parents=True)
        original_scandir = os.scandir

        def guarded_scandir(path):
            if Path(path) == forbidden:
                raise AssertionError("must not traverse vfs")
            return original_scandir(path)

        with mock.patch.object(helper.os, "scandir", guarded_scandir):
            self.assert_reason("profile_source_invalid", helper.inspect_profile, source, base / "session")

    def test_profile_inventory_fails_closed_on_scan_errors_and_nonfiles(self):
        base = self.new_dir()
        source = base / "source"
        source.mkdir()

        def inaccessible(_root, *, followlinks, onerror):
            self.assertFalse(followlinks)
            onerror(PermissionError("SENSITIVE_TEST_MARKER"))
            return iter(())

        with mock.patch.object(helper.os, "walk", side_effect=inaccessible):
            self.assert_reason("profile_scan_failed", helper.inspect_profile, source, base / "session")
        (source / "PROFILE.DAT").mkdir()
        self.assert_reason("profile_invalid", helper.inspect_profile, source, base / "session")

    def test_first_boot_route_and_strict_pad_evidence(self):
        base = self.new_dir()
        spec = helper.first_boot_route()
        script, duration = helper.shaders.route_script("first-boot")
        self.assertEqual((spec["script"], spec["duration_seconds"]), (script, duration))
        self.assertEqual(spec["sha256"], hashlib.sha256(script.encode("utf-8")).hexdigest())
        log, original = self.make_first_boot_log(base)
        self.assertEqual(helper.analyze_first_boot(log), {"valid": True, "reason": "ok",
                                                          "route_sha256": spec["sha256"]})
        cases = [
            (original[:-1], "pad_incomplete"),
            ([original[0].replace(str(spec["event_count"]), "1"), *original[1:]], "pad_registration"),
            ([*original, original[-1]], "pad_event_unexpected"),
            ([original[0], original[1].replace("press 0x4000", "sticks l=(128,0)"), *original[2:]],
             "pad_action_invalid"),
            ([original[0], original[1].replace("0x4000", "0x0008"), *original[2:]], "pad_action_mismatch"),
            ([original[0], original[2], original[1], *original[3:]], "pad_order_invalid"),
            ([original[0], original[1].replace("(at 2.0s)", "(at 5.0s)"), *original[2:]], "pad_event_late"),
            ([original[0], original[1].replace("(at 2.0s)", "(at 1.0s)"), *original[2:]], "pad_event_time_invalid"),
            ([original[0], original[1].replace("(at 2.0s)", ""), *original[2:]], "pad_timestamp_missing"),
            ([*original, "[cellPad] PAD_SCRIPT: WARNING fixture"], "pad_warning"),
            ([*original, "[CRASH] fixture"], "crash_log"),
            ([*original, "pipeline creation failed"], "shader_failure"),
        ]
        for lines, reason in cases:
            with self.subTest(reason=reason):
                log.write_text("\n".join(lines) + "\n", encoding="utf-8")
                self.assert_reason(reason, helper.analyze_first_boot, log)

    def test_profile_overlap_rejected_before_creation_or_copy(self):
        base = self.new_dir()
        source, _, _ = self.make_profile(base)
        session = base / "session"
        self.assertTrue(helper.check_profile_source(source, session)["ok"])
        cases = [base, session, session / "seed", session / "seed" / "gamedata", session / "other"]
        with mock.patch.object(helper, "_no_links", side_effect=AssertionError("must not scan")):
            for overlapping in cases:
                with self.subTest(source=overlapping.name):
                    self.assert_reason("profile_source_overlap", helper.check_profile_source, overlapping, session)
                    self.assert_reason("profile_source_overlap", helper.prepare_seed, overlapping, session / "seed")
                    self.assertFalse(session.exists())
        session.mkdir()
        nested, _, _ = self.make_profile(session)
        with mock.patch.object(helper, "_no_links", side_effect=AssertionError("must not scan")):
            self.assert_reason("profile_source_overlap", helper.prepare_seed, nested, session / "seed")
        self.assertFalse((session / "seed").exists())

    def test_profile_symlink_is_rejected(self):
        base = self.new_dir()
        source, _, _ = self.make_profile(base)
        target = base / "target"
        target.mkdir()
        link = source / "link"
        try:
            link.symlink_to(target, target_is_directory=True)
        except OSError:
            if os.name != "nt":
                self.skipTest("creating a synthetic symlink is unavailable")
            import _winapi
            _winapi.CreateJunction(str(target), str(link))
        self.assert_reason("profile_contains_link", helper.prepare_seed, source, base / "session" / "seed")

    def test_profile_vfs_path_is_rejected_without_inspection(self):
        base = self.new_dir()
        with mock.patch.object(helper, "_no_link_ancestors", side_effect=AssertionError("must not inspect")):
            self.assert_reason("profile_source_invalid", helper.check_profile_source,
                               base / "VFS" / "gamedata", base / "session")

    def test_profile_symlink_ancestor_is_rejected(self):
        base = self.new_dir()
        source, _, _ = self.make_profile(base)
        link = base / "linked-source"
        try:
            link.symlink_to(source, target_is_directory=True)
        except OSError:
            if os.name != "nt":
                self.skipTest("creating a synthetic symlink is unavailable")
            import _winapi
            _winapi.CreateJunction(str(source), str(link))
        self.assert_reason("profile_contains_link", helper.prepare_seed, link, base / "session" / "seed")

    def test_staged_launcher_resources(self):
        base = self.new_dir()
        install, app = base / "install", base / "app"
        (install / "fonts").mkdir(parents=True)
        (install / "lang").mkdir()
        (install / "runtime").mkdir()
        app.mkdir()
        font = helper.shaders.fr.NOTICE_FONTS[0]
        (install / "fonts" / font).write_bytes(b"synthetic-font")
        (install / "lang" / "port_ui.tsv").write_bytes(b"synthetic-language")
        (install / "runtime" / "SDL3.dll").write_bytes(b"synthetic-library")
        with mock.patch.object(helper.shutil, "which", side_effect=AssertionError("normal mode must not check Git")):
            self.assertEqual(helper.stage_resources(install, app), {"ok": True})
        self.assertEqual((app / "fonts" / font).read_bytes(), b"synthetic-font")
        self.assertEqual((app / "lang" / "port_ui.tsv").read_bytes(), b"synthetic-language")
        self.assertEqual((app / "SDL3.dll").read_bytes(), b"synthetic-library")
        self.assert_reason("app_destination_invalid", helper.stage_resources, install, base / "missing")

    def test_sampled_flip_window_and_redacted_output(self):
        log = self.make_log(self.new_dir())
        result = helper.analyze(log)
        self.assertTrue(result["valid"])
        self.assertEqual(result["fps_mean_flip_approx"], 30.0)
        self.assertEqual(result["gpu_time_status"], "unavailable")
        self.assertEqual(result["pad_clock_alignment"]["method"], "shared_monotonic_timestamp")
        self.assertGreater(result["pad_clock_alignment"]["offset_lower_seconds"], 0)
        self.assertNotIn("SENSITIVE_TEST_MARKER", json.dumps(result))
        self.assertNotIn(str(self.scratch), json.dumps(result))

    def test_native_ten_flip_sampling(self):
        log = self.make_log(self.new_dir(), step_ms=1000 / 3)
        self.assertEqual(helper.analyze(log)["fps_mean_flip_approx"], 30.0)

    def test_rejects_bad_evidence_without_echoing_it(self):
        log = self.make_log(self.new_dir())
        clean = log.read_text()
        cases = [
            ("pad_warning", clean + "[cellPad] PAD_SCRIPT: WARNING bad input\n"),
            ("crash_log", clean + "[CRASH] SENSITIVE_TEST_MARKER\n"),
            ("crash_log", clean + "[HardExit] SENSITIVE_TEST_MARKER\n"),
            ("crash_log", clean + "[null-call] SENSITIVE_TEST_MARKER\n"),
            ("crash_log", clean + "[exit] SPU pool did not drain; forced guest exit code 3\n"),
            ("shader_failure", clean + "PSO failed SENSITIVE_TEST_MARKER\n"),
            ("pad_registration", clean.replace(f"{helper.route()['event_count']} event", "1 event")),
            ("pad_incomplete",
             clean.replace(f"[cellPad] script t={helper.route()['mission_start']:.1f}s",
                           f"[cellPad] missing t={helper.route()['mission_start']:.1f}s")),
            ("pad_action_mismatch", clean.replace("press 0x4000", "press 0x0008", 1)),
            ("pad_action_mismatch", clean.replace("for 40 polls", "for 41 polls", 1)),
            ("pad_action_mismatch", clean.replace("sticks l=(128,0)", "sticks l=(129,0)", 1)),
            ("mission_not_observed", clean.replace("mission_0", "not_mission_0")),
            ("flip_gap", "\n".join(line for line in clean.splitlines()
                                    if not (self.flip_ms(line) is not None
                                            and (helper.WINDOW_START + 5) * 1000
                                            <= self.flip_ms(line)
                                            <= (helper.WINDOW_START + 7) * 1000))),
            ("incomplete_window", "\n".join(line for line in clean.splitlines()
                                            if not (self.flip_ms(line) is not None
                                                    and self.flip_ms(line)
                                                    >= helper.WINDOW_END * 1000))),
        ]
        for reason, text in cases:
            with self.subTest(reason=reason):
                log.write_text(text, encoding="utf-8")
                self.assert_reason(reason, helper.analyze, log)

    def test_pad_actual_timing_and_flip_clock_alignment(self):
        window_start = float(helper.WINDOW_START)
        log = self.make_log(self.new_dir(), actuals={window_start: window_start + 1})
        self.assertTrue(helper.analyze(log)["valid"])
        late = self.make_log(self.new_dir(), actuals={window_start: window_start + 95},
                             end=helper.WINDOW_END + 45)
        self.assert_reason("pad_event_late", helper.analyze, late)
        mission_start = helper.route()["mission_start"]
        late_before = self.make_log(self.new_dir(), actuals={mission_start: mission_start + 2.1})
        self.assert_reason("pad_event_late", helper.analyze, late_before)
        outside = self.make_log(self.new_dir(), offset=-0.5)
        self.assert_reason("pad_outside_window", helper.analyze, outside)
        before_window = window_start - 3
        near_window_end = float(helper.WINDOW_END - 3)
        outside_before = self.make_log(
            self.new_dir(), offset=1.5, actuals={before_window: before_window + 1.9})
        self.assert_reason("pad_outside_window", helper.analyze, outside_before)
        outside_end = self.make_log(
            self.new_dir(), offset=1.5, actuals={near_window_end: near_window_end + 1.9})
        self.assert_reason("pad_outside_window", helper.analyze, outside_end)
        wrong_origin = self.make_log(self.new_dir(), offset=4.0)
        self.assert_reason("pad_clock_unaligned", helper.analyze, wrong_origin)
        delayed_write = self.make_log(
            self.new_dir(), emissions={window_start: window_start + 5.5})
        self.assertTrue(helper.analyze(delayed_write)["valid"])
        inconsistent = self.make_log(
            self.new_dir(), host_times={window_start: window_start + 5.5})
        self.assert_reason("pad_clock_unaligned", helper.analyze, inconsistent)
        missing = self.make_log(self.new_dir())
        missing.write_text(missing.read_text().replace(f"(at {window_start:.1f}s)", ""))
        self.assert_reason("pad_timestamp_missing", helper.analyze, missing)
        imprecise = self.make_log(self.new_dir())
        imprecise.write_text(
            imprecise.read_text().replace(
                f"(at {window_start:.1f}s)", f"(at {helper.WINDOW_START}s)"))
        self.assert_reason("pad_timestamp_missing", helper.analyze, imprecise)
        early = self.make_log(self.new_dir())
        early.write_text(early.read_text().replace(f"(at {mission_start:.1f}s)", "(at 140.0s)", 1))
        self.assert_reason("pad_event_time_invalid", helper.analyze, early)
        unbracketed = self.make_log(self.new_dir())
        unbracketed.write_text("\n".join(
            line for line in unbracketed.read_text().splitlines()
            if not (self.flip_ms(line) is not None
                    and (mission_start - 1) * 1000 <= self.flip_ms(line) <= (mission_start + 2) * 1000)))
        self.assertTrue(helper.analyze(unbracketed)["valid"])
        missing_pad_clock = self.make_log(self.new_dir())
        missing_pad_clock.write_text(re.sub(
            rf"(\[cellPad\] script t={window_start:.1f}s[^\n]*?) host=\d+ ms", r"\1",
            missing_pad_clock.read_text(), count=1))
        self.assert_reason("shared_clock_timestamp_missing", helper.analyze, missing_pad_clock)
        missing_flip_clock = self.make_log(self.new_dir())
        missing_flip_clock.write_text(re.sub(
            r"(\[flip\] \d+ at \d+ ms) host=\d+ ms", r"\1",
            missing_flip_clock.read_text(), count=1))
        self.assert_reason("shared_clock_timestamp_missing", helper.analyze, missing_flip_clock)

    def test_patchset_identity(self):
        base = self.new_dir()
        patches = base / "patches"
        patches.mkdir()
        (patches / "002.patch").write_bytes(b"second\n")
        (patches / "001.patch").write_bytes(b"first\n")
        digest = hashlib.sha256(b"first\nsecond\n").hexdigest()[:16]
        self.assertEqual(helper.patch_digest(patches), digest)
        gitlink = "1234567" + "0" * 33
        stamp = digest + "-1234567"
        self.assertEqual(helper.validate_patch_stamp(stamp, stamp, digest, gitlink), stamp)
        self.assert_reason("patchset_mismatch", helper.validate_patch_stamp, stamp, "stale", digest, gitlink)
        self.assert_reason("patchset_mismatch", helper.validate_patch_stamp, stamp, stamp, "0" * 16, gitlink)
        self.assert_reason("patchset_mismatch", helper.validate_patch_stamp, stamp, stamp, digest, "f" * 40)

    def test_build_verification_and_failure_paths(self):
        base = self.new_dir()
        repo, install = base / "repo", base / "install"
        patches = repo / "port" / "patches"
        toolkit = install / "work" / "ps3recomp-patched"
        build = install / "build"
        for directory in (patches, toolkit, build):
            directory.mkdir(parents=True)
        (patches / "001.patch").write_bytes(b"synthetic patch\n")
        digest = helper.patch_digest(patches)
        gitlink = "1234567" + "0" * 33
        head = "b" * 40
        stamp = digest + "-1234567"
        (toolkit / ".kz3-patchset").write_text(stamp, encoding="ascii")
        (build / "build-stamp.json").write_text(json.dumps({"patchset": stamp}), encoding="utf-8")

        def replies(index=None, revision=None, work=0, staged=0):
            return [
                subprocess.CompletedProcess([], 0, index if index is not None else f"160000 {gitlink} 0\tfixture\n"),
                subprocess.CompletedProcess([], 0, revision if revision is not None else head + "\n"),
                subprocess.CompletedProcess([], work),
                subprocess.CompletedProcess([], staged),
            ]

        def verify(responses):
            with mock.patch.object(helper.shutil, "which", return_value="git"), \
                    mock.patch.object(helper.subprocess, "run", side_effect=responses):
                return helper.verify_build(repo, install)

        result = verify(replies())
        self.assertEqual(result, {"ok": True, "patchset": stamp, "git_head": head})
        self.assertNotIn(str(base), json.dumps(result))
        self.assert_reason("patchset_gitlink_missing", verify, replies(index="bad index"))
        self.assert_reason("git_head_missing", verify, replies(revision="bad revision"))
        self.assert_reason("patchset_submodule_changed", verify, replies(work=1))
        self.assert_reason("patchset_submodule_changed", verify, replies(staged=1))
        (build / "build-stamp.json").write_text('{"patchset":"stale"}', encoding="utf-8")
        self.assert_reason("patchset_mismatch", verify, replies())
        with mock.patch.object(helper.shutil, "which", return_value=None):
            self.assert_reason("git_missing", helper.verify_build, repo, install)

    def test_developer_build_cache_and_provenance(self):
        fixture = self.make_developer_build()
        result = self.verify_developer_fixture(fixture)
        self.assertEqual(result["patchset"], fixture["stamp"])
        self.assertEqual(result["exe_sha256"], hashlib.sha256(fixture["exe"].read_bytes()).hexdigest())
        self.assertEqual(result["cmake_cache_sha256"], hashlib.sha256(fixture["cache"].read_bytes()).hexdigest())
        self.assertEqual((result["cmake_generator"], result["configure_toolkit_stamp"]), ("Ninja", "not_recorded"))
        self.assertEqual((result["eboot_mode"], result["eboot_sha256"]), ("argument", None))
        self.assertEqual(result["freshness"], "ninja_dry_run_clean")
        self.assertNotIn(str(fixture["base"]), json.dumps(result))
        original = fixture["cache"].read_text(encoding="utf-8")
        variants = [
            (original.replace(fixture["source"].as_posix(), fixture["base"].as_posix()), "developer_source_mismatch"),
            (original.replace(fixture["toolkit"].as_posix(), fixture["base"].as_posix()), "developer_toolkit_mismatch"),
            (original.replace(fixture["build"].as_posix(), fixture["base"].as_posix()), "developer_build_dir_mismatch"),
            (original.replace("CMAKE_GENERATOR:INTERNAL=Ninja", "CMAKE_GENERATOR:INTERNAL=Unknown"),
             "developer_generator_unsupported"),
            ("\n".join(line for line in original.splitlines() if not line.startswith("ps3recomp_runtime_SOURCE_DIR")),
             "developer_cache_invalid"),
            ("\n".join(line for line in original.splitlines() if not line.startswith("KZ3_EMBED_EBOOT")),
             "developer_cache_invalid"),
            (original + f"CMAKE_HOME_DIRECTORY:INTERNAL={fixture['source'].as_posix()}\n", "developer_cache_invalid"),
            (original + "KZ3_EMBED_EBOOT:FILEPATH=\n", "developer_cache_invalid"),
            (original + f"PS3RECOMP_DIR:PATH={fixture['base'].as_posix()}\n", "developer_toolkit_mismatch"),
        ]
        for content, reason in variants:
            with self.subTest(reason=reason):
                fixture["cache"].write_text(content, encoding="utf-8")
                self.assert_reason(reason, self.verify_developer_fixture, fixture)
        fixture["cache"].write_text(original, encoding="utf-8")
        os.utime(fixture["cache"], ns=(fixture["old"], fixture["old"]))
        fixture["manifest"].unlink()
        self.assert_reason("developer_configure_output_missing", self.verify_developer_fixture, fixture)
        fixture["manifest"].write_text("# synthetic ninja file\n", encoding="utf-8")
        os.utime(fixture["manifest"], ns=(fixture["old"], fixture["old"]))
        (fixture["repo"] / "port" / "patches" / "001.patch").write_bytes(b"changed patch\n")
        self.assert_reason("patchset_mismatch", self.verify_developer_fixture, fixture)

    def test_developer_build_stamp_format_and_freshness(self):
        fixture = self.make_developer_build()
        config_stamp = fixture["build"] / ".kz3-toolkit-stamp"
        config_stamp.write_text(fixture["stamp"], encoding="ascii")
        os.utime(config_stamp, ns=(fixture["old"], fixture["old"]))
        result = self.verify_developer_fixture(fixture)
        self.assertEqual(result["configure_toolkit_stamp"], "matched")
        config_stamp.write_text("stale", encoding="ascii")
        self.assert_reason("developer_configure_stamp_mismatch", self.verify_developer_fixture, fixture)
        config_stamp.write_text(fixture["stamp"], encoding="ascii")
        os.utime(config_stamp, ns=(fixture["old"], fixture["old"]))
        for path in (fixture["toolkit_stamp"], fixture["cache"], fixture["manifest"], fixture["rules"], config_stamp):
            with self.subTest(output=path.name):
                os.utime(path, ns=(fixture["new"], fixture["new"]))
                self.assert_reason("developer_build_stale", self.verify_developer_fixture, fixture)
                os.utime(path, ns=(fixture["old"], fixture["old"]))
        fixture["exe"].write_bytes(b"not an executable")
        self.assert_reason("developer_executable_invalid", self.verify_developer_fixture, fixture)
        fixture["exe"].write_bytes(b"MZ synthetic developer executable")
        os.utime(fixture["exe"], ns=(fixture["new"], fixture["new"]))
        fixture["toolkit_stamp"].write_text("not-a-patchset", encoding="ascii")
        self.assert_reason("patchset_mismatch", self.verify_developer_fixture, fixture)

    def test_developer_build_dependency_graph_must_be_clean(self):
        fixture = self.make_developer_build()
        metadata_only = (
            f"ninja: Entering directory `{fixture['build']}'\n"
            "[0/2] Re-checking globbed directories...\n"
            "[1/2] Re-running CMake...\n"
            f"ninja explain: {fixture['build']}/CMakeFiles/VerifyGlobs.cmake_force is dirty\n"
            f"ninja explain: {fixture['build']}/CMakeFiles/cmake.verify_globs is dirty\n"
        )
        self.assertTrue(self.verify_developer_fixture(fixture, metadata_only)["ok"])
        self.assert_reason("developer_build_graph_stale", self.verify_developer_fixture, fixture,
                           "[1/1] Building CXX object synthetic.obj\n")
        self.assert_reason("developer_freshness_check_failed", self.verify_developer_fixture, fixture, "", 1)
        with mock.patch.object(helper.shutil, "which", return_value=None):
            self.assert_reason("ninja_missing", helper._verify_build_graph_fresh,
                               fixture["build"], fixture["exe"], "Ninja")
        self.assert_reason("developer_freshness_unsupported", helper._verify_build_graph_fresh,
                           fixture["build"], fixture["exe"], "NMake Makefiles")
        glob_dir = fixture["build"] / "generated"
        glob_dir.mkdir()
        glob_source = glob_dir / "one.cpp"
        glob_source.write_text("// fixture\n", encoding="utf-8")
        verify_globs = fixture["build"] / "CMakeFiles" / "VerifyGlobs.cmake"
        verify_globs.write_text(
            f'file(GLOB NEW_GLOB LIST_DIRECTORIES true "{glob_dir.as_posix()}/*.cpp")\n'
            "set(OLD_GLOB\n)\n", encoding="utf-8")
        with mock.patch.object(helper.shutil, "which", return_value="ninja"):
            self.assert_reason("developer_build_graph_stale", helper._verify_build_graph_fresh,
                               fixture["build"], fixture["exe"], "Ninja")

    def test_stale_install_resolves_developer_launcher_without_changes(self):
        fixture = self.make_developer_build()
        install = self.make_stale_install(fixture["base"])
        original_stamp = (install["build"] / "build-stamp.json").read_bytes()
        original_paths = (install["build"] / "paths.json").read_bytes()
        decision = helper.readonly_launcher(install["root"])
        self.assertEqual(decision["action"], "rebuild")
        self.assertFalse((install["root"] / "lang" / "port_ui.tsv").exists())
        result = helper.developer_launcher(install["root"], fixture["build"], fixture["repo"])
        self.assertEqual(result["action"], "rebuild")
        self.assertEqual(result["argv"], [str(fixture["exe"]), str(install["eboot"])])
        self.assertEqual(result["eboot_mode"], "argument")
        self.assertEqual(result["eboot_sha256"], hashlib.sha256(install["eboot"].read_bytes()).hexdigest())
        self.assertEqual(result["cwd"], str(install["root"]))
        self.assertEqual(result["env"]["PS3_VFS_ROOT"], str(install["dump"]))
        self.assertEqual(result["env"]["KZ3_UPDATE_DIR"], str(install["update"] / "USRDIR"))
        self.assertEqual(result["ini"]["eboot"], str(install["eboot"]))
        self.assertEqual(result["ini"]["font_dir"], str(install["root"] / "fixture-fonts"))
        self.assertEqual((install["build"] / "build-stamp.json").read_bytes(), original_stamp)
        self.assertEqual((install["build"] / "paths.json").read_bytes(), original_paths)
        fixture["translation"].unlink()
        self.assert_reason("developer_resource_missing", helper.developer_launcher,
                           install["root"], fixture["build"], fixture["repo"])
        self.assertEqual((install["build"] / "build-stamp.json").read_bytes(), original_stamp)

    def test_developer_embedded_eboot_provenance_and_launcher(self):
        fixture = self.make_developer_build()
        install = self.make_stale_install(fixture["base"])
        header, digest = self.configure_embedded_fixture(fixture, install["eboot"])
        build = self.verify_developer_fixture(fixture)
        self.assertEqual((build["eboot_mode"], build["eboot_sha256"]), ("embedded", digest))
        self.assertNotIn(str(fixture["base"]), json.dumps(build))
        launch = helper.developer_launcher(install["root"], fixture["build"], fixture["repo"])
        self.assertEqual(launch["argv"], [str(fixture["exe"])])
        self.assertEqual((launch["eboot_mode"], launch["eboot_sha256"]), ("embedded", digest))
        self.assertEqual(launch["cmake_cache_sha256"], build["cmake_cache_sha256"])
        self.assertEqual(launch["ini"]["eboot"], str(install["eboot"]))
        self.assertTrue(header.is_file())
        # A separately located input is acceptable only if it matches both its generated header and the install.
        separate = fixture["base"] / "separate input" / "synthetic.elf"
        separate.parent.mkdir()
        separate.write_bytes(install["eboot"].read_bytes())
        self.configure_embedded_fixture(fixture, separate)
        self.assertEqual(self.verify_developer_fixture(fixture)["eboot_sha256"], digest)
        self.assertEqual(helper.developer_launcher(install["root"], fixture["build"], fixture["repo"])["argv"],
                         [str(fixture["exe"])])
        relative = fixture["source"] / "relative synthetic.elf"
        relative.write_bytes(install["eboot"].read_bytes())
        self.configure_embedded_fixture(fixture, relative)
        cache = fixture["cache"]
        cache.write_text(cache.read_text(encoding="utf-8").replace(relative.as_posix(), relative.name), encoding="utf-8")
        os.utime(cache, ns=(fixture["old"], fixture["old"]))
        self.assertEqual(self.verify_developer_fixture(fixture)["eboot_sha256"], digest)
        self.assertEqual(helper.developer_launcher(install["root"], fixture["build"], fixture["repo"])["argv"],
                         [str(fixture["exe"])])
        separate.write_bytes(b"different-synthetic-data")
        self.configure_embedded_fixture(fixture, separate)
        self.assert_reason("developer_eboot_mismatch", helper.developer_launcher,
                           install["root"], fixture["build"], fixture["repo"])

    def test_developer_eboot_cache_and_header_fail_closed(self):
        fixture = self.make_developer_build()
        install = self.make_stale_install(fixture["base"])
        original_cache = fixture["cache"].read_text(encoding="utf-8")
        header, _digest = self.configure_embedded_fixture(fixture, install["eboot"])
        original_header = header.read_text(encoding="utf-8")
        for content, reason in [
            (original_header.replace(install["eboot"].as_posix(), fixture["exe"].as_posix()),
             "developer_eboot_mismatch"),
            (re.sub(r'KZ3_EMBED_EBOOT_SHA256 "[0-9a-f]{64}"', f'KZ3_EMBED_EBOOT_SHA256 "{"0" * 64}"',
                    original_header), "developer_eboot_mismatch"),
            (original_header.replace("KZ3_EMBED_EBOOT_SHA256", "WRONG_SHA"), "developer_eboot_header_invalid"),
            (original_header + original_header.splitlines()[-1] + "\n", "developer_eboot_header_invalid"),
        ]:
            with self.subTest(reason=reason):
                header.write_text(content, encoding="utf-8")
                self.assert_reason(reason, self.verify_developer_fixture, fixture)
                self.assert_reason(reason, helper.developer_launcher, install["root"], fixture["build"], fixture["repo"])
        header.write_text(original_header, encoding="utf-8")
        for path in (header, install["eboot"]):
            with self.subTest(stale=path.name):
                os.utime(path, ns=(fixture["new"], fixture["new"]))
                self.assert_reason("developer_build_stale", self.verify_developer_fixture, fixture)
                os.utime(path, ns=(fixture["old"], fixture["old"]))
        header.unlink()
        self.assert_reason("developer_eboot_header_missing", self.verify_developer_fixture, fixture)
        self.assert_reason("developer_eboot_header_missing", helper.developer_launcher,
                           install["root"], fixture["build"], fixture["repo"])
        header.write_text(original_header, encoding="utf-8")
        os.utime(header, ns=(fixture["old"], fixture["old"]))
        for value, reason in [
            ((fixture["base"] / "missing file.elf").as_posix(), "developer_eboot_missing"),
            ((fixture["base"] / "vfs" / "not-inspected").as_posix(), "link_free_launcher_required"),
            ('invalid"file', "developer_eboot_path_invalid"),
        ]:
            with self.subTest(reason=reason):
                fixture["cache"].write_text(original_cache.replace("KZ3_EMBED_EBOOT:FILEPATH=",
                                                                   "KZ3_EMBED_EBOOT:FILEPATH=" + value),
                                            encoding="utf-8")
                self.assert_reason(reason, self.verify_developer_fixture, fixture)
                self.assert_reason(reason, helper.developer_launcher, install["root"], fixture["build"], fixture["repo"])
                command = [sys.executable, "-B", str(HERE / "mission0_helper.py"), "launcher",
                           "--install", str(install["root"]), "--developer-build", str(fixture["build"]),
                           "--repo", str(fixture["repo"])]
                result = subprocess.run(command, capture_output=True, text=True, timeout=30, check=False)
                self.assertNotEqual(result.returncode, 0)
                self.assertEqual(json.loads(result.stdout)["reason"], reason)
                self.assertNotIn("SENSITIVE_TEST_MARKER", result.stdout + result.stderr)
                self.assertNotIn(str(fixture["base"]), result.stdout + result.stderr)
        fixture["cache"].write_text(original_cache, encoding="utf-8")
        self.assert_reason("developer_eboot_header_mismatch", self.verify_developer_fixture, fixture)
        self.assert_reason("developer_eboot_header_mismatch", helper.developer_launcher,
                           install["root"], fixture["build"], fixture["repo"])

    def test_developer_eboot_rejects_reparse_and_missing_stored_input_privately(self):
        fixture = self.make_developer_build()
        install = self.make_stale_install(fixture["base"])
        self.configure_embedded_fixture(fixture, install["eboot"])
        original_lstat = Path.lstat

        def reparse_eboot(path):
            info = original_lstat(path)
            if path == install["eboot"]:
                return SimpleNamespace(st_mode=info.st_mode, st_file_attributes=0x400)
            return info

        with mock.patch.object(Path, "lstat", reparse_eboot):
            self.assert_reason("developer_path_link", self.verify_developer_fixture, fixture)
            self.assert_reason("developer_path_link", helper.developer_launcher,
                               install["root"], fixture["build"], fixture["repo"])
        fixture["cache"].write_text(
            fixture["cache"].read_text(encoding="utf-8").replace(install["eboot"].as_posix(), ""), encoding="utf-8")
        (fixture["build"] / "kz3_embed_gen" / "kz3_eboot_embed.h").unlink()
        with mock.patch.object(Path, "lstat", reparse_eboot):
            self.assert_reason("developer_path_link", helper.developer_launcher,
                               install["root"], fixture["build"], fixture["repo"])
        install["eboot"].unlink()
        self.assert_reason("developer_recorded_paths_invalid", helper.developer_launcher,
                           install["root"], fixture["build"], fixture["repo"])
        command = [sys.executable, "-B", str(HERE / "mission0_helper.py"), "launcher",
                   "--install", str(install["root"]), "--developer-build", str(fixture["build"]),
                   "--repo", str(fixture["repo"])]
        result = subprocess.run(command, capture_output=True, text=True, timeout=30, check=False)
        self.assertNotEqual(result.returncode, 0)
        self.assertEqual(json.loads(result.stdout)["reason"], "developer_recorded_paths_invalid")
        self.assertNotIn("SENSITIVE_TEST_MARKER", result.stdout + result.stderr)
        self.assertNotIn(str(fixture["base"]), result.stdout + result.stderr)

    def test_developer_launcher_rejects_missing_and_linked_paths(self):
        fixture = self.make_developer_build()
        install = self.make_stale_install(fixture["base"])
        paths_file = install["build"] / "paths.json"
        paths = dict(install["paths"])
        paths["update"] = "missing-update"
        paths_file.write_text(json.dumps(paths), encoding="utf-8")
        self.assert_reason("developer_recorded_paths_invalid", helper.developer_launcher,
                           install["root"], fixture["build"], fixture["repo"])
        paths["update"] = str(Path("vfs") / "not-inspected")
        paths_file.write_text(json.dumps(paths), encoding="utf-8")
        with mock.patch.object(helper, "readonly_launcher", side_effect=AssertionError("must not inspect")):
            self.assert_reason("link_free_launcher_required", helper.developer_launcher,
                               install["root"], fixture["build"], fixture["repo"])
        paths_file.write_text(json.dumps(install["paths"]), encoding="utf-8")
        original_lstat = Path.lstat
        parameter_file = install["update"] / "PARAM.SFO"

        def reparse_parameter(path):
            info = original_lstat(path)
            if path == parameter_file:
                return SimpleNamespace(st_mode=info.st_mode, st_file_attributes=0x400)
            return info

        with mock.patch.object(Path, "lstat", reparse_parameter), \
                mock.patch.object(helper, "readonly_launcher", side_effect=AssertionError("must not inspect")):
            self.assert_reason("developer_path_link", helper.developer_launcher,
                               install["root"], fixture["build"], fixture["repo"])
        paths = dict(install["paths"])
        link = fixture["base"] / "linked-dump"
        try:
            link.symlink_to(install["dump"], target_is_directory=True)
        except (OSError, NotImplementedError):
            if os.name != "nt":
                self.skipTest("directory symlinks are unavailable")
            import _winapi
            _winapi.CreateJunction(str(install["dump"]), str(link))
        paths["dump"] = str(link)
        paths_file.write_text(json.dumps(paths), encoding="utf-8")
        self.assert_reason("developer_path_link", helper.developer_launcher,
                           install["root"], fixture["build"], fixture["repo"])

    def test_developer_launcher_honors_and_validates_hand_paths(self):
        fixture = self.make_developer_build()
        install = self.make_stale_install(fixture["base"])
        alternate_game = fixture["base"] / "other-game"
        alternate_update = fixture["base"] / "other-update"
        alternate_data = fixture["base"] / "other-data"
        for directory in (alternate_game, alternate_update, alternate_data):
            directory.mkdir()
        ini = install["build"] / "kz3.ini"
        (install["build"] / helper.shaders.la.INI_WRITTEN).write_text("{}", encoding="utf-8")
        text = (f"[game]\nvfs_root = {alternate_game}\nupdate_dir = {alternate_update}\n"
                f"data_dir = {alternate_data}\n")
        ini.write_text(text, encoding="utf-8")
        result = helper.developer_launcher(install["root"], fixture["build"], fixture["repo"])
        self.assertEqual(result["env"]["PS3_VFS_ROOT"], str(alternate_game))
        self.assertEqual(result["env"]["KZ3_UPDATE_DIR"], str(alternate_update))
        self.assertEqual(result["cwd"], str(alternate_data))
        for key in ("vfs_root", "update_dir", "data_dir"):
            lines = [f"{key} = {fixture['base'] / 'missing'}" if line.startswith(key + " =") else line
                     for line in text.splitlines()]
            ini.write_text("\n".join(lines), encoding="utf-8")
            with self.subTest(key=key):
                self.assert_reason("developer_recorded_paths_invalid", helper.developer_launcher,
                                   install["root"], fixture["build"], fixture["repo"])

    def test_developer_resources_are_required_and_exact(self):
        fixture = self.make_developer_build()
        install, app = fixture["base"] / "install", fixture["base"] / "app"
        self.make_install_resources(install)
        (install / "lang").mkdir()
        old_translation = install / "lang" / "port_ui.tsv"
        old_translation.write_bytes(b"synthetic-outdated-install-language")
        (install / "build" / "fonts").mkdir(parents=True)
        (install / "build" / "SDL3.dll").write_bytes(b"not-a-fallback")
        (app / "fonts").mkdir(parents=True)
        (app / "lang").mkdir()
        fixture["translation"].write_bytes(b"synthetic-current-tracked-language")
        (app / "lang" / "port_ui.tsv").write_bytes(b"x" * fixture["translation"].stat().st_size)
        font = helper.shaders.fr.NOTICE_FONTS[0]
        source = install / "fonts" / font
        (app / "fonts" / font).write_bytes(b"x" * source.stat().st_size)
        install_before = {p.relative_to(install): p.read_bytes() for p in install.rglob("*") if p.is_file()}
        with mock.patch.object(helper.shaders.fr, "place_lang", side_effect=AssertionError("must use tracked table")):
            result = helper.stage_resources(install, app, required=True, repo=fixture["repo"])
        self.assertEqual(result, {"ok": True, "ui_translations_source": "repository_tracked",
                                  "ui_translations_sha256": hashlib.sha256(fixture["translation"].read_bytes()).hexdigest()})
        self.assertEqual((app / "lang" / "port_ui.tsv").read_bytes(), fixture["translation"].read_bytes())
        self.assertEqual(old_translation.read_bytes(), b"synthetic-outdated-install-language")
        self.assertEqual({p.relative_to(install): p.read_bytes() for p in install.rglob("*") if p.is_file()},
                         install_before)
        for relative in ("SDL3.dll", *(f"fonts/{name}" for name in helper.shaders.fr.NOTICE_FONTS)):
            payload = "runtime/SDL3.dll" if relative == "SDL3.dll" else relative
            self.assertEqual((app / relative).read_bytes(), (install / payload).read_bytes())
        for name in helper.shaders.fr.NOTICE_FONTS:
            (install / "build" / "fonts" / name).write_bytes(b"not-a-fallback")
        for relative in ("runtime/SDL3.dll", *(f"fonts/{name}" for name in helper.shaders.fr.NOTICE_FONTS)):
            path = install / relative
            original = path.read_bytes()
            path.unlink()
            self.assert_reason("developer_resource_missing", helper.stage_resources, install, app,
                               required=True, repo=fixture["repo"])
            path.write_bytes(original)

    def test_developer_translation_must_be_tracked_regular_and_link_free(self):
        fixture = self.make_developer_build()
        install, app = fixture["base"] / "install", fixture["base"] / "app"
        self.make_install_resources(install)
        (install / "lang").mkdir()
        (install / "lang" / "port_ui.tsv").write_bytes(b"not-a-fallback")
        app.mkdir()
        table = fixture["translation"]
        original = table.read_bytes()
        table.unlink()
        self.assert_reason("developer_resource_missing", helper.stage_resources, install, app,
                           required=True, repo=fixture["repo"])
        table.mkdir()
        self.assert_reason("developer_resource_missing", helper.stage_resources, install, app,
                           required=True, repo=fixture["repo"])
        table.rmdir()
        table.write_bytes(original)
        git = shutil.which("git")
        untrack = subprocess.run([git, "-C", str(fixture["repo"]), "rm", "--cached", "--quiet", "--",
                                  str(table.relative_to(fixture["repo"]))],
                                 capture_output=True, timeout=30, check=False)
        self.assertEqual(untrack.returncode, 0, "synthetic Git index update failed")
        self.assert_reason("developer_translation_untracked", helper.stage_resources, install, app,
                           required=True, repo=fixture["repo"])
        result = subprocess.run([sys.executable, "-B", str(HERE / "mission0_helper.py"), "stage-resources",
                                 "--install", str(install), "--app", str(app), "--required",
                                 "--repo", str(fixture["repo"])],
                                capture_output=True, text=True, timeout=30, check=False)
        self.assertNotEqual(result.returncode, 0)
        self.assertEqual(json.loads(result.stdout)["reason"], "developer_translation_untracked")
        self.assertNotIn("SENSITIVE_TEST_MARKER", result.stdout + result.stderr)
        self.assertNotIn(str(fixture["base"]), result.stdout + result.stderr)
        self.assertFalse((app / "lang" / "port_ui.tsv").exists())
        text_dir = table.parent
        relocated = fixture["base"] / "relocated-text"
        text_dir.rename(relocated)
        try:
            text_dir.symlink_to(relocated, target_is_directory=True)
        except (OSError, NotImplementedError):
            if os.name != "nt":
                self.skipTest("directory symlinks are unavailable")
            import _winapi
            _winapi.CreateJunction(str(relocated), str(text_dir))
        self.assert_reason("developer_path_link", helper.stage_resources, install, app,
                           required=True, repo=fixture["repo"])

    def test_developer_linked_resources_and_build_are_rejected(self):
        fixture = self.make_developer_build()
        install = fixture["base"] / "install"
        self.make_install_resources(install)
        fonts = install / "fonts"
        relocated = install / "relocated-fonts"
        fonts.rename(relocated)
        build_link = fixture["base"] / "linked-build"
        for target, link in ((relocated, fonts), (fixture["build"], build_link)):
            try:
                link.symlink_to(target, target_is_directory=True)
            except (OSError, NotImplementedError):
                if os.name != "nt":
                    self.skipTest("directory symlinks are unavailable")
                import _winapi
                _winapi.CreateJunction(str(target), str(link))
        app = fixture["base"] / "app"
        app.mkdir()
        self.assert_reason("developer_path_link", helper.stage_resources, install, app,
                           required=True, repo=fixture["repo"])
        self.assert_reason("developer_path_link", self.verify_developer_fixture, dict(fixture, build=build_link))

    def test_developer_helper_invalid_arguments_are_private(self):
        private = self.new_dir() / "SENSITIVE_TEST_MARKER"
        helper_cmd = [sys.executable, "-B", str(HERE / "mission0_helper.py")]
        cases = [
            (["launcher", "--install", str(private), "--repo", str(private)], "arguments_invalid"),
            (["launcher", "--install", str(private), "--developer-build", str(private)], "arguments_invalid"),
            (["launcher", "--install", str(private), "--developer-build", "", "--repo", str(private)],
             "arguments_invalid"),
            (["stage-resources", "--install", str(private), "--app", str(private), "--required"],
             "arguments_invalid"),
            (["stage-resources", "--install", str(private), "--app", str(private), "--repo", str(private)],
             "arguments_invalid"),
            (["verify-developer-build", "--repo", str(private), "--build", str(private)], "developer_source_invalid"),
        ]
        for args, reason in cases:
            with self.subTest(reason=reason):
                result = subprocess.run(helper_cmd + args, capture_output=True, text=True, timeout=30, check=False)
                self.assertNotEqual(result.returncode, 0)
                self.assertEqual(json.loads(result.stdout)["reason"], reason)
                self.assertNotIn(str(private), result.stdout + result.stderr)
                self.assertNotIn("SENSITIVE_TEST_MARKER", result.stdout + result.stderr)

    def test_helper_errors_never_echo_private_paths(self):
        private = self.new_dir() / "SENSITIVE_TEST_MARKER"
        result = subprocess.run([sys.executable, "-B", str(HERE / "mission0_helper.py"), "analyze",
                                 "--log", str(private)],
                                capture_output=True, text=True, timeout=30, check=False)
        self.assertNotEqual(result.returncode, 0)
        self.assertEqual(json.loads(result.stdout)["reason"], "input_or_tool_error")
        self.assertNotIn(str(private), result.stdout + result.stderr)
        invalid = subprocess.run([sys.executable, "-B", str(HERE / "mission0_helper.py"), "analyze",
                                  "--log", str(private), "--unexpected", str(private)],
                                 capture_output=True, text=True, timeout=30, check=False)
        self.assertNotEqual(invalid.returncode, 0)
        self.assertEqual(json.loads(invalid.stdout)["reason"], "arguments_invalid")
        self.assertNotIn(str(private), invalid.stdout + invalid.stderr)
        output = io.StringIO()
        with mock.patch.object(helper, "analyze", side_effect=RuntimeError("SENSITIVE_TEST_MARKER")), \
                contextlib.redirect_stdout(output):
            self.assertEqual(helper.main(["analyze", "--log", "one"]), 1)
        self.assertEqual(json.loads(output.getvalue())["reason"], "input_or_tool_error")
        self.assertNotIn("SENSITIVE_TEST_MARKER", output.getvalue())
        helper_cmd = [sys.executable, "-B", str(HERE / "mission0_helper.py")]
        for args, reason in [
            (["analyze-first-boot", "--log", str(private)], "input_or_tool_error"),
            (["inspect-profile", "--source", str(private), "--session", str(private / "session")],
             "profile_source_overlap"),
        ]:
            with self.subTest(reason=reason):
                result = subprocess.run(helper_cmd + args, capture_output=True, text=True, timeout=30, check=False)
                self.assertNotEqual(result.returncode, 0)
                self.assertEqual(json.loads(result.stdout)["reason"], reason)
                self.assertNotIn(str(private), result.stdout + result.stderr)
                self.assertNotIn("SENSITIVE_TEST_MARKER", result.stdout + result.stderr)

    @unittest.skipUnless(os.name == "nt", "Win32 process control requires Windows")
    def test_window_close_targets_only_the_tracked_pid(self):
        import ctypes
        from ctypes import wintypes

        class FakeFunction:
            def __init__(self, func):
                self.func = func

            def __call__(self, *args):
                return self.func(*args)

        class FakeUser32:
            def __init__(self):
                self.posted = []
                self.EnumWindows = FakeFunction(self.enumerate)
                self.GetWindowThreadProcessId = FakeFunction(self.window_pid)
                self.IsWindowVisible = FakeFunction(lambda hwnd: hwnd != 103)
                self.PostMessageW = FakeFunction(self.post)

            @staticmethod
            def enumerate(callback, _):
                for hwnd in (101, 102, 103):
                    callback(hwnd, 0)
                return 1

            @staticmethod
            def window_pid(hwnd, result):
                ctypes.cast(result, ctypes.POINTER(wintypes.DWORD))[0] = 456 if hwnd == 102 else 123
                return 1

            def post(self, hwnd, message, *_):
                self.posted.append((hwnd, message))
                return 1

        user32 = FakeUser32()
        with mock.patch.object(ctypes, "WinDLL", return_value=user32):
            helper._close_process_windows(123)
            self.assertEqual(user32.posted, [(101, 0x0010)])
            self.assert_reason("close_window_unavailable", helper._close_process_windows, 999)

    @unittest.skipUnless(os.name == "nt", "Win32 process control requires Windows")
    def test_supervisor_parent_loss_kills_only_its_child(self):
        import ctypes
        from ctypes import wintypes

        base = self.new_dir()
        kernel = ctypes.WinDLL("kernel32", use_last_error=True)
        kernel.OpenProcess.argtypes = [wintypes.DWORD, wintypes.BOOL, wintypes.DWORD]
        kernel.OpenProcess.restype = wintypes.HANDLE
        kernel.WaitForSingleObject.argtypes = [wintypes.HANDLE, wintypes.DWORD]
        kernel.WaitForSingleObject.restype = wintypes.DWORD
        kernel.TerminateProcess.argtypes = [wintypes.HANDLE, wintypes.UINT]
        kernel.TerminateProcess.restype = wintypes.BOOL
        kernel.CloseHandle.argtypes = [wintypes.HANDLE]
        kernel.CloseHandle.restype = wintypes.BOOL
        command = [sys.executable, "-B", "-c", "import time; time.sleep(20)"]
        allowed = ("SystemRoot", "WINDIR", "PATH", "PATHEXT", "TEMP", "TMP", "USERPROFILE", "APPDATA",
                   "LOCALAPPDATA")
        spec = {"executable": sys.executable, "command_line": subprocess.list2cmdline(command),
                "cwd": str(base), "environment": {k: os.environ[k] for k in allowed if k in os.environ},
                "log": str(base / "merged.log"), "duration_seconds": 15, "grace_seconds": 1,
                "close_mode": "stdin"}
        supervisor = subprocess.Popen([sys.executable, "-I", "-B", str(HERE / "mission0_helper.py"), "supervise"],
                                      stdin=subprocess.PIPE, stdout=subprocess.PIPE, stderr=subprocess.PIPE,
                                      text=True, encoding="utf-8")
        innocent = subprocess.Popen(command, stdin=subprocess.DEVNULL, stdout=subprocess.DEVNULL,
                                    stderr=subprocess.DEVNULL)
        child_handle = None
        try:
            supervisor.stdin.write(json.dumps(spec) + "\n")
            supervisor.stdin.flush()
            started = json.loads(supervisor.stdout.readline())
            self.assertEqual(started["event"], "started")
            self.assertGreater(int(started["created_filetime"]), 0)
            child_handle = kernel.OpenProcess(0x00100001, False, started["pid"])
            self.assertTrue(child_handle)
            supervisor.stdin.close()
            self.assertEqual(supervisor.wait(timeout=10), 1)
            result = json.loads(supervisor.stdout.read())
            self.assertEqual(result["reason"], "supervisor_parent_lost")
            self.assertEqual(kernel.WaitForSingleObject(child_handle, 0), 0)
            self.assertIsNone(innocent.poll())
        finally:
            if not supervisor.stdin.closed:
                supervisor.stdin.close()
            if supervisor.poll() is None:
                supervisor.kill()
                supervisor.wait(timeout=5)
            if child_handle:
                if kernel.WaitForSingleObject(child_handle, 0) != 0:
                    kernel.TerminateProcess(child_handle, 1)
                    kernel.WaitForSingleObject(child_handle, 5000)
                kernel.CloseHandle(child_handle)
            supervisor.stdout.close()
            supervisor.stderr.close()
            if innocent.poll() is None:
                innocent.terminate()
                innocent.wait(timeout=5)

    @unittest.skipUnless(os.name == "nt", "PowerShell plan tests require Windows")
    def test_powershell_baseline_plans_and_mode_validation(self):
        powershell = shutil.which("powershell.exe")
        self.assertIsNotNone(powershell, "Windows PowerShell is required")
        command = [powershell, "-NoLogo", "-NoProfile", "-NonInteractive", "-ExecutionPolicy", "Bypass",
                   "-File", str(HERE / "mission0.ps1")]
        candidates = ("GCM_FAST_METHODS", "LD_CB_REUSE", "KZ3_SPU_TASK_POOL", "KZ3_PARK_IDLE_WAKE")
        for requested in (5, 7, 100):
            with self.subTest(requested=requested):
                result = subprocess.run(command + ["-Baseline", "-Plan", "-SamplesPerSide", str(requested)],
                                        capture_output=True, text=True, timeout=30, check=False)
                self.assertEqual(result.returncode, 0, "baseline plan failed")
                plan = json.loads(result.stdout)
                self.assertEqual((plan["status"], plan["mode"], plan["candidate"]), ("plan_only", "baseline", None))
                self.assertEqual(plan["schedule"], ["off"] * requested)
                self.assertEqual(plan["conditions"]["mode"], "baseline")
                self.assertIsNone(plan["conditions"]["candidate"])
                self.assertEqual(plan["conditions"]["resolution"], "1920x1080")
                self.assertEqual(plan["conditions"]["requested_valid_baseline_windows"], requested)
                self.assertEqual(plan["conditions"]["candidate_switches"],
                                 {"off": {name: "0" for name in candidates}})
                self.assertNotIn(str(REPO), result.stdout + result.stderr)
                self.assertNotIn(str(Path.home()), result.stdout + result.stderr)
        for arguments, reason in [
            (["-Baseline", "-Candidate", "GCM_FAST_METHODS", "-Plan"], "baseline_candidate_conflict"),
            (["-Baseline", "-Candidate", "LD_CB_REUSE"], "baseline_candidate_conflict"),
            (["-Baseline", "-Candidate", "SENSITIVE_TEST_MARKER", "-Plan"], "baseline_candidate_conflict"),
            (["-Baseline"], "exclusive_gpu_confirmation_required"),
            (["-Baseline", "-Plan", "-SamplesPerSide", "4"], "sample_count_invalid"),
            (["-Baseline", "-Plan", "-SamplesPerSide", "101"], "sample_count_invalid"),
            (["-Baseline", "-Plan", "-SamplesPerSide", str(Path.home() / "SENSITIVE_TEST_MARKER")],
             "sample_count_invalid"),
            (["-Baseline", "-Plan", "-SamplesPerSide", "99999999999999999999"], "sample_count_invalid"),
            (["-Candidate", str(Path.home() / "SENSITIVE_TEST_MARKER"), "-Plan"], "candidate_invalid"),
        ]:
            with self.subTest(reason=reason, arguments=arguments):
                rejected = subprocess.run(command + arguments, capture_output=True, text=True, timeout=30, check=False)
                self.assertNotEqual(rejected.returncode, 0)
                self.assertIn("benchmark failed: " + reason, rejected.stderr)
                self.assertNotIn(str(REPO), rejected.stdout + rejected.stderr)
                self.assertNotIn(str(Path.home()), rejected.stdout + rejected.stderr)
                self.assertNotIn("SENSITIVE_TEST_MARKER", rejected.stdout + rejected.stderr)

    @unittest.skipUnless(os.name == "nt", "PowerShell plan tests require Windows")
    def test_powershell_candidate_plan_switch_isolation(self):
        powershell = shutil.which("powershell.exe")
        self.assertIsNotNone(powershell, "Windows PowerShell is required")
        command = [powershell, "-NoLogo", "-NoProfile", "-NonInteractive", "-ExecutionPolicy", "Bypass",
                   "-File", str(HERE / "mission0.ps1")]
        candidates = ("GCM_FAST_METHODS", "LD_CB_REUSE", "KZ3_SPU_TASK_POOL", "KZ3_PARK_IDLE_WAKE")
        for selected in candidates:
            with self.subTest(candidate=selected):
                result = subprocess.run(command + ["-Candidate", selected.lower(), "-Plan"],
                                        capture_output=True, text=True, timeout=30, check=False)
                self.assertEqual(result.returncode, 0, "candidate plan failed")
                plan = json.loads(result.stdout)
                self.assertEqual(plan["mode"], "candidate")
                self.assertEqual(plan["candidate"], selected)
                self.assertEqual(plan["conditions"]["resolution"], "1920x1080")
                self.assertEqual(plan["conditions"]["requested_valid_windows_per_side"], 5)
                self.assertEqual(plan["schedule"], ["off", "on", "on", "off"] * 3)
                switches = plan["conditions"]["candidate_switches"]
                self.assertEqual(switches["off"], {name: "0" for name in candidates})
                self.assertEqual(switches["on"], {name: "1" if name == selected else "0" for name in candidates})
                self.assertNotIn(str(REPO), result.stdout + result.stderr)

    @unittest.skipUnless(os.name == "nt", "PowerShell plan tests require Windows")
    def test_powershell_developer_plan_and_argument_validation(self):
        powershell = shutil.which("powershell.exe")
        self.assertIsNotNone(powershell, "Windows PowerShell is required")
        command = [powershell, "-NoLogo", "-NoProfile", "-NonInteractive", "-ExecutionPolicy", "Bypass",
                   "-File", str(HERE / "mission0.ps1")]
        root = self.new_dir()
        private = root / "SENSITIVE_TEST_MARKER"
        result = subprocess.run(command + ["-Baseline", "-Plan", "-InstallRoot", str(root),
                                           "-DeveloperBuildDir", str(private)],
                                capture_output=True, text=True, timeout=30, check=False)
        self.assertEqual(result.returncode, 0)
        plan = json.loads(result.stdout)
        self.assertEqual((plan["mode"], plan["conditions"]["build_source"]), ("baseline", "developer_cmake"))
        self.assertEqual(plan["conditions"]["resolution"], "1920x1080")
        self.assertNotIn(str(private), result.stdout + result.stderr)
        self.assertNotIn(str(root), result.stdout + result.stderr)
        result = subprocess.run(command + ["-Candidate", "LD_CB_REUSE", "-Plan", "-InstallRoot", str(root),
                                           "-DeveloperBuildDir", str(private)],
                                capture_output=True, text=True, timeout=30, check=False)
        self.assertEqual(result.returncode, 0)
        plan = json.loads(result.stdout)
        self.assertEqual((plan["mode"], plan["candidate"], plan["conditions"]["build_source"]),
                         ("candidate", "LD_CB_REUSE", "developer_cmake"))
        self.assertEqual(plan["schedule"], ["off", "on", "on", "off"] * 3)
        self.assertNotIn(str(private), result.stdout + result.stderr)
        self.assertNotIn(str(root), result.stdout + result.stderr)
        result = subprocess.run(command + ["-Baseline", "-Plan", "-InstallRoot", str(root),
                                           "-DeveloperBuildDir", str(private), "-InitializeProfile",
                                           "-ProfileSource", str(private / "missing")],
                                capture_output=True, text=True, timeout=30, check=False)
        self.assertEqual(result.returncode, 0)
        plan = json.loads(result.stdout)
        self.assertEqual(plan["bootstrap"], {
            "status": "if_source_has_no_profile", "route": "first-boot",
            "route_sha256": helper.first_boot_route()["sha256"], "provenance": "developer_cmake",
        })
        self.assertEqual(plan["bootstrap_duration_seconds"], helper.first_boot_route()["duration_seconds"] + 15)
        self.assertEqual(plan["schedule"], ["off"] * 5)
        self.assertFalse(private.exists())
        self.assertNotIn(str(private), result.stdout + result.stderr)
        self.assertNotIn(str(root), result.stdout + result.stderr)
        candidate = subprocess.run(command + ["-Candidate", "LD_CB_REUSE", "-Plan",
                                               "-DeveloperBuildDir", str(private), "-InitializeProfile"],
                                   capture_output=True, text=True, timeout=30, check=False)
        self.assertEqual(candidate.returncode, 0)
        candidate_plan = json.loads(candidate.stdout)
        self.assertEqual(candidate_plan["bootstrap"], plan["bootstrap"])
        self.assertEqual(candidate_plan["schedule"], ["off", "on", "on", "off"] * 3)
        self.assertEqual(candidate_plan["conditions"]["candidate_switches"]["on"]["LD_CB_REUSE"], "1")
        self.assertNotIn(str(private), candidate.stdout + candidate.stderr)
        cases = [
            (["-Baseline", "-Candidate", "GCM_FAST_METHODS", "-Plan", "-DeveloperBuildDir", str(private)],
             "baseline_candidate_conflict"),
            (["-Baseline", "-Plan", "-DeveloperBuildDir", " "], "developer_build_dir_invalid"),
            (["-Baseline", "-Plan", "-InitializeProfile"], "initialize_profile_requires_developer_build"),
            (["-Candidate", "GCM_FAST_METHODS", "-Plan", "-InitializeProfile"],
             "initialize_profile_requires_developer_build"),
            (["-Baseline", "-Plan", "-InitializeProfile", "-DeveloperBuildDir", " "], "developer_build_dir_invalid"),
            (["-Baseline", "-Plan", "-InitializeProfile", "-InitializeProfile", "-DeveloperBuildDir", str(private)],
             "arguments_invalid"),
            (["-Baseline", "-ConfirmExclusiveGpu", "-InstallRoot", str(root), "-DeveloperBuildDir", str(private)],
             "developer_build_dir_invalid"),
        ]
        for args, reason in cases:
            with self.subTest(reason=reason):
                result = subprocess.run(command + args, capture_output=True, text=True, timeout=30, check=False)
                self.assertNotEqual(result.returncode, 0)
                self.assertIn("benchmark failed: " + reason, result.stderr)
                self.assertNotIn("SENSITIVE_TEST_MARKER", result.stdout + result.stderr)
                self.assertNotIn(str(root), result.stdout + result.stderr)
                self.assertNotIn(str(REPO), result.stdout + result.stderr)

    @unittest.skipUnless(os.name == "nt", "PowerShell plan tests require Windows")
    def test_powershell_argument_errors_are_private(self):
        powershell = shutil.which("powershell.exe")
        command = [powershell, "-NoLogo", "-NoProfile", "-NonInteractive", "-ExecutionPolicy", "Bypass",
                   "-File", str(HERE / "mission0.ps1")]
        private = str(self.new_dir() / "SENSITIVE_TEST_MARKER")
        invalid = [
            ["-Baseline", "-Plan", "-DeveloperBuildDir"],
            ["-Baseline", "-InstallRoot", private, "-Plan", "-Python"],
            ["-Baseline", "-Plan", "-NotAnOption", private],
            ["-Baseline", "-Baseline", "-Plan"],
            ["-Baseline", "-Plan", "-DeveloperBuildDir", private, private],
        ]
        for arguments in invalid:
            with self.subTest(case=invalid.index(arguments)):
                result = subprocess.run(command + arguments, capture_output=True, text=True, timeout=30, check=False)
                self.assertNotEqual(result.returncode, 0)
                self.assertEqual(result.stderr.strip(), "benchmark failed: arguments_invalid")
                self.assertNotIn("SENSITIVE_TEST_MARKER", result.stdout + result.stderr)
                self.assertNotIn(str(REPO), result.stdout + result.stderr)
                self.assertNotIn(str(Path.home()), result.stdout + result.stderr)
        accepted = subprocess.run(command + ["-Base", "-Pl", "-DeveloperBuildDir:" + private],
                                  capture_output=True, text=True, timeout=30, check=False)
        self.assertEqual(accepted.returncode, 0)
        self.assertEqual(json.loads(accepted.stdout)["conditions"]["build_source"], "developer_cmake")
        self.assertNotIn("SENSITIVE_TEST_MARKER", accepted.stdout + accepted.stderr)
        accepted = subprocess.run(command + ["-Candidate:LD_CB_REUSE", "-Plan:$true"],
                                  capture_output=True, text=True, timeout=30, check=False)
        self.assertEqual(accepted.returncode, 0)
        self.assertEqual(json.loads(accepted.stdout)["candidate"], "LD_CB_REUSE")

    @unittest.skipUnless(os.name == "nt", "PowerShell process tests require Windows")
    def test_powershell_plan_and_synthetic_processes(self):
        powershell = shutil.which("powershell.exe")
        self.assertIsNotNone(powershell, "Windows PowerShell is required")
        plan = subprocess.run([powershell, "-NoLogo", "-NoProfile", "-NonInteractive", "-ExecutionPolicy", "Bypass",
                               "-File", str(HERE / "mission0.ps1"), "-Candidate", "GCM_FAST_METHODS", "-Plan"],
                              capture_output=True, text=True, timeout=30, check=False)
        self.assertEqual(plan.returncode, 0, "plan failed")
        plan_json = json.loads(plan.stdout)
        self.assertEqual(plan_json["status"], "plan_only")
        self.assertEqual((plan_json["mode"], plan_json["candidate"]), ("candidate", "GCM_FAST_METHODS"))
        self.assertEqual(plan_json["schedule"], ["off", "on", "on", "off"] * 3)
        switches = plan_json["conditions"]["candidate_switches"]
        self.assertEqual(switches["off"], {"GCM_FAST_METHODS": "0", "LD_CB_REUSE": "0",
                                           "KZ3_SPU_TASK_POOL": "0", "KZ3_PARK_IDLE_WAKE": "0"})
        self.assertEqual(switches["on"], dict(switches["off"], GCM_FAST_METHODS="1"))
        self.assertNotIn(str(REPO), plan.stdout)
        self.assertNotIn(str(Path.home()), plan.stdout)
        plan7 = subprocess.run([powershell, "-NoLogo", "-NoProfile", "-NonInteractive", "-ExecutionPolicy", "Bypass",
                                "-File", str(HERE / "mission0.ps1"), "-Candidate", "LD_CB_REUSE", "-Plan",
                                "-SamplesPerSide", "7"], capture_output=True, text=True, timeout=30, check=False)
        self.assertEqual(plan7.returncode, 0, "larger plan failed")
        self.assertEqual(json.loads(plan7.stdout)["schedule"], ["off", "on", "on", "off"] * 4)
        command = [powershell, "-NoLogo", "-NoProfile", "-NonInteractive", "-ExecutionPolicy", "Bypass",
                   "-File", str(HERE / "mission0.ps1")]
        for arguments, reason in [
            (["-Plan"], "candidate_required"),
            (["-Candidate", "GCM_FAST_METHODS"], "exclusive_gpu_confirmation_required"),
            (["-Candidate", "GCM_FAST_METHODS", "-Plan", "-Python",
              str(self.new_dir() / "SENSITIVE_TEST_MARKER.exe")], "python_missing"),
        ]:
            with self.subTest(reason=reason):
                rejected = subprocess.run(command + arguments, capture_output=True, text=True, timeout=30, check=False)
                self.assertNotEqual(rejected.returncode, 0)
                self.assertIn("benchmark failed: " + reason, rejected.stderr)
                self.assertNotIn("SENSITIVE_TEST_MARKER", rejected.stdout + rejected.stderr)
                self.assertNotIn(str(REPO), rejected.stdout + rejected.stderr)
        install = self.new_dir() / "synthetic-install"
        install.mkdir()
        rejected = subprocess.run(command + ["-Candidate", "GCM_FAST_METHODS", "-ConfirmExclusiveGpu",
                                              "-InstallRoot", str(install)],
                                  capture_output=True, text=True, timeout=30, check=False)
        self.assertNotEqual(rejected.returncode, 0)
        self.assertIn("benchmark failed: launcher_not_runnable", rejected.stderr)
        self.assertEqual(list(install.iterdir()), [])
        self.assertNotIn(str(REPO), rejected.stdout + rejected.stderr)
        result = subprocess.run([powershell, "-NoLogo", "-NoProfile", "-NonInteractive", "-ExecutionPolicy", "Bypass",
                                 "-File", str(HERE / "test_mission0.ps1"), "-Scratch", str(self.new_dir()),
                                 "-PythonExe", sys.executable], capture_output=True, text=True, timeout=240, check=False)
        failure = re.search(r"test_mission0_ps: FAIL \([a-z_]+\)", result.stderr)
        diagnostic = re.search(r"diagnostic: reason=[a-z_]+ type=[A-Za-z]+ line=\d+", result.stderr)
        mismatch = re.search(r"fixture reason mismatch: expected=[a-z_]+ actual=[a-z_]*", result.stderr)
        assertion = re.search(r"fixture assertion line: \d+", result.stderr)
        details = (failure.group(0) if failure else "PowerShell fixture failed") + (
            " " + diagnostic.group(0) if diagnostic else "") + (" " + mismatch.group(0) if mismatch else "") + (
                " " + assertion.group(0) if assertion else "")
        self.assertEqual(result.returncode, 0, details)
        self.assertIn("test_mission0_ps: PASS", result.stdout)
        self.assertNotIn(str(REPO), result.stdout + result.stderr)


if __name__ == "__main__":
    unittest.main(verbosity=2)
