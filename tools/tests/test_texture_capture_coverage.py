"""Reject capture runs that exit cleanly without establishing requested coverage."""
import importlib.util
import json
from pathlib import Path
import tempfile
import unittest

PATH = Path(__file__).resolve().parents[1] / 'textures/capture_coverage.py'
spec = importlib.util.spec_from_file_location('capture_coverage', PATH)
capture = importlib.util.module_from_spec(spec)
spec.loader.exec_module(capture)


class CoverageEvidenceTests(unittest.TestCase):
    def fixture(self, directory, log, state='40', buttons=0):
        target = Path(directory)
        (target / 'run').mkdir()
        (target / 'run/result.json').write_text(json.dumps({'passed': True}))
        (target / 'run/runtime.log').write_text(log)
        (target / 'run/state.csv').write_text(f'tick,state\n570,{state}\n')
        (target / 'run/input.csv').write_text(f'tick,controller,buttons,x,y\n570,0,{buttons},0,0\n')
        return target

    def test_clean_exit_wrong_course_is_failure(self):
        with tempfile.TemporaryDirectory() as directory:
            target = self.fixture(directory, '[texture-capture] race course=0 mode=1 players=2 difficulty=0\n')
            result = capture.observe_run(target, dict(course=1, players=2, mode='versus'), {})
            self.assertFalse(result['passed'])
            self.assertTrue(result['capture_fixture_initialized'])
            self.assertFalse(result['capture_fixture_matched'])

    def test_scene_requires_observed_page_and_idle_input(self):
        for state, buttons, expected in [('72', 0, True), ('10', 0, False), ('72', 32768, False)]:
            with self.subTest(state=state, buttons=buttons), tempfile.TemporaryDirectory() as directory:
                target = self.fixture(directory,
                    '[texture-capture] scene=audio expected_state=72 actual_state=72\n', state, buttons)
                result = capture.observe_run(target, dict(scene='audio', expected_state=72), {})
                self.assertEqual(result['passed'], expected)

    def test_postrace_requires_native_finish_flags_and_reached_state(self):
        for flags, state, expected in [('1,1,1,1', '52', True), ('1,0,0,0', '52', False),
                                       ('1,1,1,1', '40', False)]:
            with self.subTest(flags=flags, state=state), tempfile.TemporaryDirectory() as directory:
                target = self.fixture(directory,
                    f'[texture-capture] postrace=results tick=569 actual_state=52 finish_flags={flags} retire_flags=0,0,0,0\n', state)
                result = capture.observe_run(target, dict(postrace='results', expected_state=52), {})
                self.assertEqual(result['passed'], expected)

    def test_overview_and_postrace_plan_is_bounded(self):
        fixtures = capture.plan_fixtures(['overviews', 'postrace'], [1], 2100)
        overviews = [f for f in fixtures if f.get('scene') == 'overview']
        self.assertEqual([f['round'] for f in overviews], list(range(8)))
        self.assertTrue(all(f['through'] <= 1050 and f['difficulty'] == 2 for f in overviews))
        self.assertEqual({f['postrace'] for f in fixtures if 'postrace' in f}, {'results', 'finish-hud'})


if __name__ == '__main__':
    unittest.main()
