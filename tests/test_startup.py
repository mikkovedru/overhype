"""Exercise startup routing with isolated profiles and no desktop session."""
import hashlib
import json
import os
from pathlib import Path
import subprocess
import shutil
import tempfile
import time
import unittest

APP = Path(__file__).resolve().parents[1] / 'build/hype'


class StartupTests(unittest.TestCase):
    def setUp(self):
        self.temp = tempfile.TemporaryDirectory()
        self.addCleanup(self.temp.cleanup)
        self.root = Path(self.temp.name)
        self.env = {k: v for k, v in os.environ.items() if k not in ('DISPLAY', 'WAYLAND_DISPLAY')}
        self.env.update(XDG_CONFIG_HOME=str(self.root / 'config'), XDG_STATE_HOME=str(self.root / 'state'),
                        XDG_CACHE_HOME=str(self.root / 'cache'), XDG_RUNTIME_DIR=str(self.root / 'runtime'),
                        HOME=str(self.root / 'home'), QT_QPA_PLATFORM='wayland;xcb', QT_QPA_PLATFORMTHEME='generic')
        (self.root / 'runtime').mkdir(mode=0o700)

    def run_app(self, *args, gui=False, code=0):
        env = dict(self.env)
        if gui:
            env['QT_QPA_PLATFORM'] = 'offscreen'
            env['QT_STYLE_OVERRIDE'] = 'Fusion'
        result = subprocess.run([str(APP), *map(str, args)], cwd=self.root, env=env,
                                capture_output=True, text=True, timeout=15)
        self.assertEqual(result.returncode, code, result.stderr)
        return result

    def test_help_version_and_errors_are_headless_and_do_not_write_session_state(self):
        for flag in ('--help', '-h', '--help-all', '--version', '-v'):
            self.assertTrue(self.run_app(flag).stdout)
        for args in (('--home', '--new'), ('--slide', '0'), ('--home', '--save'), ('--unknown',), ('a.md', 'b.md')):
            self.assertTrue(self.run_app(*args, code=1).stderr)
        self.assertFalse((self.root / 'config').exists())
        self.assertFalse((self.root / 'state').exists())
        self.assertIn('graphical display', self.run_app(code=1).stderr)
        self.assertIn('graphical display', self.run_app('--', '--render-looking.md', code=1).stderr)
        self.assertIn('graphical display', self.run_app('--theme', '--pdf', code=1).stderr)

    def test_home_new_missing_file_and_delimited_filename_captures(self):
        self.run_app('--home', '--screenshot', self.root / 'home.png', gui=True)
        self.run_app('--new', '--screenshot', self.root / 'new.png', gui=True)
        self.run_app('missing.md', '--screenshot', self.root / 'missing.png', gui=True)
        deck = self.root / '--render-looking.md'
        deck.write_text('# Delimited filename\n')
        self.run_app('--screenshot', self.root / 'file.png', '--', deck.name, gui=True)
        for name in ('home', 'new', 'missing', 'file'):
            self.assertGreater((self.root / (name + '.png')).stat().st_size, 1000)
        self.assertFalse((self.root / 'state').exists())
        self.assertFalse((self.root / 'config').exists())

    def test_repeated_gui_launch_reuses_instance_and_cli_remains_independent(self):
        a, b = self.root / 'a.md', self.root / 'b.md'
        a.write_text('# First\n'); b.write_text('# Second\n')
        env = dict(self.env, QT_QPA_PLATFORM='offscreen', QT_STYLE_OVERRIDE='Fusion')
        first = subprocess.Popen([str(APP), str(a)], cwd=self.root, env=env, stdout=subprocess.PIPE, stderr=subprocess.PIPE)
        self.addCleanup(lambda: self.stop(first))
        recents = self.root / 'state/hype/recents.json'
        deadline = time.monotonic() + 8
        while not recents.exists() and time.monotonic() < deadline:
            if first.poll() is not None:
                self.fail(first.communicate()[1].decode())
            time.sleep(.05)
        self.assertTrue(recents.exists())
        self.run_app(b, gui=True)
        self.assertIsNone(first.poll())
        entries = json.loads(recents.read_text())
        self.assertEqual([entry['path'] for entry in entries[:2]], [str(b), str(a)])
        before = recents.read_bytes()
        self.run_app('slides', a, '--json')
        self.assertEqual(recents.read_bytes(), before)
        self.run_app('--home', gui=True)
        self.assertIsNone(first.poll())

    def test_recovery_remembered_slide_and_explicit_override_in_real_startup(self):
        deck = self.root / 'slides.md'
        source = '# One\n---\n# Two\n'
        deck.write_text(source)
        first, second = self.root / 'first.png', self.root / 'second.png'
        self.run_app(deck, '--slide', '1', '--screenshot', first, gui=True)
        self.run_app(deck, '--slide', '2', '--screenshot', second, gui=True)
        folder = self.root / 'state/hype/recovery' / hashlib.sha256(str(deck).encode()).hexdigest()
        folder.mkdir(parents=True)
        snapshot = dict(version=1, path=str(deck), source=source, saved=source, header='',
                        sha256=hashlib.sha256(source.encode()).hexdigest(), selected=1, anchor=1,
                        slides=[dict(start=0, end=6), dict(start=10, end=16)])
        (folder / 'latest.json').write_text(json.dumps(snapshot))
        recovered, explicit = self.root / 'recovered.png', self.root / 'explicit.png'
        self.run_app(deck, '--screenshot', recovered, gui=True)
        self.run_app(deck, '--slide', '1', '--screenshot', explicit, gui=True)
        self.assertEqual(second.read_bytes(), recovered.read_bytes())
        self.assertEqual(first.read_bytes(), explicit.read_bytes())
        self.assertNotEqual(first.read_bytes(), second.read_bytes())

    def test_new_process_accepts_immediate_typing_and_ipc_markdown_opens_editor(self):
        display = os.environ.get('DISPLAY')
        if not display or not shutil.which('xdotool'):
            self.skipTest('An X11 display and xdotool are needed for process keyboard input')
        env = dict(self.env, DISPLAY=display, QT_QPA_PLATFORM='xcb', QT_STYLE_OVERRIDE='Fusion')
        process = subprocess.Popen([str(APP), '--new'], cwd=self.root, env=env, stdout=subprocess.PIPE, stderr=subprocess.PIPE)
        self.addCleanup(lambda: self.stop(process))
        window = self.owned_window(process, env)
        subprocess.run(['xdotool', 'type', '--window', window, 'StartupCheck'], env=env,
                       check=True, capture_output=True, timeout=5)
        self.wait_for_recovered_text('StartupCheck', process)
        self.run_app('--home', gui=True)
        self.run_app('--markdown', gui=True)
        config = self.root / 'config/hype/hype.ini'
        deadline = time.monotonic() + 5
        while time.monotonic() < deadline:
            if config.exists() and 'markdown' in config.read_text():
                break
            time.sleep(.05)
        self.assertIn('markdown', config.read_text())

    def owned_window(self, process, env):
        deadline = time.monotonic() + 8
        while time.monotonic() < deadline:
            if process.poll() is not None:
                self.fail(process.communicate()[1].decode())
            result = subprocess.run(['xdotool', 'search', '--onlyvisible', '--pid', str(process.pid)], env=env,
                                    capture_output=True, text=True, timeout=2)
            if result.returncode == 0 and result.stdout.strip():
                time.sleep(.15)
                return result.stdout.splitlines()[0]
            time.sleep(.05)
        self.fail('The new editor did not expose its own window')

    def wait_for_recovered_text(self, text, process):
        deadline = time.monotonic() + 7
        while time.monotonic() < deadline:
            for path in (self.root / 'state/hype/recovery').glob('*/latest.json'):
                try:
                    source = json.loads(path.read_text()).get('source', '')
                except (ValueError, OSError):
                    continue
                if '# ' + text in source:
                    return
            if process.poll() is not None:
                self.fail(process.communicate()[1].decode())
            time.sleep(.05)
        self.fail('Typing did not replace the new presentation headline')

    @staticmethod
    def stop(process):
        if process.poll() is None:
            process.terminate()
            try:
                process.communicate(timeout=5)
            except subprocess.TimeoutExpired:
                process.kill(); process.communicate()


if __name__ == '__main__':
    unittest.main()
