#!/usr/bin/env python3
"""Exercise proton-inner's optional controller with fake game/display commands, no GPU."""
import json
import os
from pathlib import Path
import signal
import subprocess
import sys
import tempfile
import unittest

ROOT = Path(__file__).resolve().parents[1]


class RunnerTests(unittest.TestCase):
    def one(self, steps=None, early_exit=False):
        parent = ROOT / 'work/proton'; parent.mkdir(parents=True, exist_ok=True)
        with tempfile.TemporaryDirectory(prefix='test-play-runner-', dir=parent) as tmp:
            run = Path(tmp); (run / 'app').mkdir()
            bin_dir = run / 'bin'; bin_dir.mkdir()
            scripts = {
                'steam-run': """import os,sys,time
from pathlib import Path
if len(sys.argv)>2 and sys.argv[2]=='-k': sys.exit(0)
Path('app_stdout.txt').write_text('READY\\n[flip] 10 at 1000 ms\\n'+(f"[exit code {os.environ['FAKE_EARLY_EXIT']}]\\n" if os.environ.get('FAKE_EARLY_EXIT') else ''))
if os.environ.get('FAKE_EARLY_EXIT'): sys.exit(0)
time.sleep(30)
""",
                'import': """import sys
from pathlib import Path
Path(sys.argv[-1]).write_bytes(b'\\x89PNG\\r\\n\\x1a\\nsynthetic')
""",
                'xdotool': "print('123')\n",
            }
            for name, text in scripts.items():
                p = bin_dir / name
                p.write_text(f'#!{sys.executable}\n' + text)
                p.chmod(0o755)
            env = dict(os.environ, PATH=str(bin_dir) + os.pathsep + os.environ['PATH'], KZ3_REAL_DISPLAY=':fake')
            env.pop('KZ3_VIDEO', None)
            env.pop('KZ3_PLAY_SCENARIO', None)
            if early_exit:
                env['FAKE_EARLY_EXIT'] = str(early_exit)
            if steps is not None:
                recipe = run / 'scenario.json'
                recipe.write_text(json.dumps(dict(schema='kz3.play/1', name='runner-test', steps=steps)))
                env['KZ3_PLAY_SCENARIO'] = str(recipe)
            proc = subprocess.Popen(['bash', str(ROOT / 'tools/proton-inner.sh'), '/fake-proton', str(run),
                                     '3', str(run / 'shot.png'), 'fake.exe'], env=env,
                                    stdout=subprocess.PIPE, stderr=subprocess.PIPE, text=True, start_new_session=True)
            try:
                stdout, stderr = proc.communicate(timeout=12)
                result = json.loads((run / 'play.json').read_text()) if (run / 'play.json').exists() else None
                return proc.returncode, result, stdout + stderr
            finally:
                try:
                    os.killpg(proc.pid, signal.SIGKILL)
                except ProcessLookupError:
                    pass
                proc.wait()
                proc.stdout.close()
                proc.stderr.close()

    def test_successful_controller_preserves_cap_and_returns_zero(self):
        rc, report, output = self.one([dict(id='gate', op='wait', text='READY'), dict(id='image', op='capture')])
        self.assertEqual(rc, 0, output)
        self.assertEqual(report['status'], 'complete', output)
        self.assertIn('scripted play ended (status 0)', output)

    def test_failed_gate_propagates_exit_five(self):
        rc, report, output = self.one([dict(id='gate', op='wait', text='MISSING', timeout=0.1)])
        self.assertEqual(rc, 5, output)
        self.assertEqual(report['status'], 'failed')

    def test_early_game_exit_cannot_override_failed_play(self):
        for code in ('0', '1'):
            with self.subTest(game_status=code):
                rc, report, output = self.one([dict(id='gate', op='wait', text='MISSING', timeout=1)], early_exit=code)
                self.assertEqual(rc, 5, output)
                self.assertTrue(report is None or report['status'] != 'complete')

    def test_opt_out_keeps_ordinary_runner(self):
        rc, report, output = self.one()
        self.assertEqual(rc, 0, output)
        self.assertIsNone(report)


if __name__ == '__main__':
    unittest.main()
