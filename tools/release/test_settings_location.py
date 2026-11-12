#!/usr/bin/env python3
"""Synthetic launcher child-environment and display-settings migration regressions.

No Windows/game process, network or real user data. An explicit source option
allows the same controls to verify the previous launcher without editing it.
"""
import argparse
import importlib.util
import json
import os
from pathlib import Path
import shutil
import sys
import tempfile
import unittest
from unittest.mock import patch

sys.dont_write_bytecode = True
parser = argparse.ArgumentParser(add_help=False)
parser.add_argument('--source', type=Path, default=Path(__file__).with_name('launch.py'))
parser.add_argument('--scratch', type=Path)
options, unittest_args = parser.parse_known_args()
source = options.source.resolve()
sys.path.insert(0, str(source.parent))
spec = importlib.util.spec_from_file_location('launch', source)
la = importlib.util.module_from_spec(spec)
sys.modules['launch'] = la
spec.loader.exec_module(la)


class NoPromptUI:
    def ask(self, message):
        raise AssertionError('unexpected prompt: ' + message)

    def info(self, message):
        raise AssertionError('unexpected message: ' + message)


class SettingsLocation(unittest.TestCase):
    def setUp(self):
        self.temp = tempfile.TemporaryDirectory(prefix='settings-', dir=options.scratch)
        self.addCleanup(self.temp.cleanup)
        self.base = Path(self.temp.name)
        self.root = self.base / 'Install'
        (self.root / 'build').mkdir(parents=True)
        self.ad, self.local = self.base / 'Roaming', self.base / 'Local'
        self.env = {'APPDATA': str(self.ad), 'LOCALAPPDATA': str(self.local), 'PATH': 'fixture-path'}
        self.paths = {'dump': 'GameDump', 'eboot': 'GameDump/EBOOT.elf'}
        (self.root / 'build/paths.json').write_text(json.dumps(self.paths))
        self.write(self.root / 'port/kz3/kz3_ini.h', '"root"\n')
        self.mode(False)
        self.addCleanup(patch.stopall)
        patch.object(la, 'known_folder', return_value='').start()
        patch.object(la, 'decide', return_value=('run', '')).start()
        patch.object(la, 'ensure_vfs', return_value=(str(self.root / 'vfs'), '')).start()
        for helper in ('place_lang', 'place_fonts', 'place_sdl3'):
            patch.object(la.fr, helper, return_value=None).start()

    @staticmethod
    def write(path, value):
        path.parent.mkdir(parents=True, exist_ok=True)
        path.write_text(value)

    def mode(self, portable, marker=False):
        la.fr.write_install_ini(str(self.root), portable, False)
        if marker:
            (self.root / la.PORTABLE_FILE).touch()

    def run_game(self, *arguments, env=None):
        calls = []

        def run(argv, **kwargs):
            calls.append((argv, kwargs))
            return 0

        def refuse_start(*args, **kwargs):
            raise AssertionError('no external background process is allowed')

        rc = la.main(['--root', str(self.root), *arguments], ui=NoPromptUI(), run=run,
                     start=refuse_start, environ=dict(self.env if env is None else env))
        self.assertEqual(rc, 0)
        self.assertEqual(len(calls), 1)
        self.assertEqual(calls[0][0][0], str(self.root / 'build/kz3.exe'))
        return calls[0][1]

    def assert_settings_path(self, call, expected):
        self.assertEqual(call['cwd'], str(expected))
        self.assertEqual(call['env'].get('KZ3_KEYBINDS'), str(expected / 'keybinds.ini'))
        # Runtime main.cpp uses display.ini beside the binding path. This checks
        # the actual child handoff; it does not execute the Windows consumer.
        self.assertEqual(Path(call['env']['KZ3_KEYBINDS']).with_name('display.ini'), expected / 'display.ini')

    def hand_data_dir(self, value):
        paths = la.load_json(str(self.root / 'build/paths.json'))
        la.write_game_ini(str(self.root), la.game_ini(str(self.root), paths, False, self.env))
        la.fr.ini_set(str(self.root / 'build/kz3.ini'), [('game', 'data_dir', value)])

    def test_one_run_portable_sets_actual_child_and_leaves_stored_mode(self):
        custom = '[display]\nmode = borderless\nfilter = nearest\n'
        self.write(self.root / 'display.ini', custom)
        self.assert_settings_path(self.run_game('--portable'), self.root)
        self.assertEqual((self.root / 'display.ini').read_text(), custom)
        self.assertFalse(la.read_ini(str(self.root))['portable'])
        self.assertFalse((self.root / la.PORTABLE_FILE).exists())
        self.assert_settings_path(self.run_game(), self.ad / la.SAVE_FOLDER)

    def test_one_run_appdata_overrides_marker_without_mutating_it(self):
        self.mode(True, marker=True)
        target = self.ad / la.SAVE_FOLDER
        self.assert_settings_path(self.run_game('--appdata'), target)
        self.assertTrue(la.read_ini(str(self.root))['portable'])
        self.assertTrue((self.root / la.PORTABLE_FILE).exists())
        self.assert_settings_path(self.run_game(), self.root)

    def test_hand_data_dir_wins_over_mode_and_survives_relaunch(self):
        target = self.root / 'Custom Settings'
        target.mkdir()
        custom = '[display]\nscaling = fit\nfilter = nearest\n'
        self.write(target / 'display.ini', custom)
        self.hand_data_dir('Custom Settings')
        for args in (('--portable',), ('--appdata',), ()):
            self.assert_settings_path(self.run_game(*args), target)
            self.assertEqual((target / 'display.ini').read_text(), custom)

    def test_relative_hand_data_dir_follows_moved_install(self):
        target = self.root / 'Custom Settings'
        target.mkdir()
        self.hand_data_dir('Custom Settings')
        moved = self.base / 'Moved Install'
        shutil.move(self.root, moved)
        self.root = moved
        self.assert_settings_path(self.run_game(), moved / 'Custom Settings')

    def test_explicit_binding_override_and_other_environment_values_win(self):
        override = self.base / 'Manual Bindings' / 'my-keybinds.ini'
        self.write(override, 'manual bindings')
        self.write(override.with_name('display.ini'), 'manual-display-settings')
        env = dict(self.env, KZ3_KEYBINDS=str(override), PS3_VFS_ROOT='manual-vfs',
                   KZ3_CACHE_ROOT='manual-cache', PS3_EDAT_CACHE='manual-edat', RSX_SHADER_CACHE_DIR='manual-shaders',
                   KZ3_DISPLAY_SCALING='fit', KZ3_FOV='95')
        call = self.run_game('--portable', env=env)
        for key, value in env.items():
            self.assertEqual(call['env'][key], value, key)
        self.assertEqual(override.read_text(), 'manual bindings')
        self.assertEqual(override.with_name('display.ini').read_text(), 'manual-display-settings')

    def test_relative_binding_override_is_preserved_verbatim(self):
        call = self.run_game('--portable', env=dict(self.env, KZ3_KEYBINDS='Manual/my-keys.ini'))
        self.assertEqual(call['env']['KZ3_KEYBINDS'], 'Manual/my-keys.ini')

    def test_empty_binding_override_uses_effective_data_dir(self):
        self.assert_settings_path(self.run_game('--portable', env=dict(self.env, KZ3_KEYBINDS='')), self.root)

    def make_migration_data(self):
        source = self.ad / la.SAVE_FOLDER
        values = {'gamedata/SAVE/P': 'save-content', 'keybinds.ini': 'custom-keys',
                  'display.ini': '[display]\nscaling = fit\nfilter = nearest\n', 'online.ini': 'offline-settings'}
        for name, value in values.items():
            self.write(source / name, value)
        self.write(self.local / la.SAVE_FOLDER / 'cache/shaders/pso.bin', 'shader-cache')
        return source, values

    def assert_source_intact(self, source, values):
        for name, value in values.items():
            self.assertEqual((source / name).read_text(), value, name)
        for name in ('gamedata', 'keybinds.ini', 'display.ini', 'online.ini', 'cache'):
            self.assertFalse((self.root / name).exists(), name)
            self.assertFalse((self.root / (name + '.copying')).exists(), name)
        self.assertFalse(la.read_ini(str(self.root))['portable'])
        self.assertFalse((self.root / la.PORTABLE_FILE).exists())

    def test_display_round_trip_survives_default_generation(self):
        source, values = self.make_migration_data()
        ok, message = la.switch_data(str(self.root), True, self.env)
        self.assertTrue(ok, message)
        self.assertFalse((source / 'display.ini').exists())
        la.ensure_default_inis(str(self.root))
        self.assertEqual((self.root / 'display.ini').read_text(), values['display.ini'])
        ok, message = la.switch_data(str(self.root), False, self.env)
        self.assertTrue(ok, message)
        self.assert_source_intact(source, values)
        self.assertEqual((self.local / la.SAVE_FOLDER / 'cache/shaders/pso.bin').read_text(), 'shader-cache')

    def test_existing_destination_display_refuses_entire_move(self):
        source, values = self.make_migration_data()
        self.write(self.root / 'display.ini', 'different-destination-settings')
        ok, message = la.switch_data(str(self.root), True, self.env)
        self.assertFalse(ok, message)
        self.assertIn('display.ini', message)
        for name, value in values.items():
            self.assertEqual((source / name).read_text(), value, name)
        self.assertEqual((self.root / 'display.ini').read_text(), 'different-destination-settings')
        self.assertFalse((self.root / 'gamedata').exists())
        self.assertFalse(la.read_ini(str(self.root))['portable'])

    def test_partial_display_copy_failure_preserves_all_sources(self):
        source, values = self.make_migration_data()
        original = shutil.copy2

        def fail_display(src, dst, *args, **kwargs):
            if Path(src).name == 'display.ini':
                Path(dst).write_text('partial')
                raise OSError('synthetic display write failure')
            return original(src, dst, *args, **kwargs)

        with patch.object(shutil, 'copy2', side_effect=fail_display):
            ok, message = la.switch_data(str(self.root), True, self.env)
        self.assertFalse(ok, message)
        self.assert_source_intact(source, values)

    def test_display_promotion_failure_rolls_back_other_promotions(self):
        source, values = self.make_migration_data()
        original = os.rename

        def fail_display(src, dst):
            if Path(src).name == 'display.ini.copying':
                raise OSError('synthetic display rename failure')
            return original(src, dst)

        with patch.object(os, 'rename', side_effect=fail_display):
            ok, message = la.switch_data(str(self.root), True, self.env)
        self.assertFalse(ok, message)
        self.assert_source_intact(source, values)

    def test_mode_record_failure_restores_display_after_it_was_moved(self):
        source, values = self.make_migration_data()
        original = os.rename
        promotions = []

        def record(src, dst):
            if Path(src).name == 'display.ini.copying':
                promotions.append(Path(dst))
            return original(src, dst)

        with patch.object(os, 'rename', side_effect=record), \
                patch.object(la, 'write_ini', side_effect=OSError('synthetic mode-write failure')):
            ok, message = la.switch_data(str(self.root), True, self.env)
        self.assertFalse(ok, message)
        self.assertEqual(promotions, [self.root / 'display.ini', source / 'display.ini'])
        self.assert_source_intact(source, values)
        self.assertEqual((self.local / la.SAVE_FOLDER / 'cache/shaders/pso.bin').read_text(), 'shader-cache')


if __name__ == '__main__':
    unittest.main(argv=[sys.argv[0], *unittest_args])
