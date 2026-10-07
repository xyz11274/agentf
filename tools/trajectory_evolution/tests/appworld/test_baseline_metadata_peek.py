"""Baseline metadata hints never replace retained source verification; no API."""
from copy import deepcopy
import io
import json
from pathlib import Path
import sys
import tempfile
import unittest
from unittest.mock import patch

ROOT = Path(__file__).resolve().parents[4]
sys.path.insert(0, str(ROOT / 'tools/trajectory_evolution/experiments/appworld'))
import compare
import layout


class BaselineFingerprintPeekTest(unittest.TestCase):
    def setUp(self):
        self.temp = tempfile.TemporaryDirectory()
        self.addCleanup(self.temp.cleanup)
        self.path = Path(self.temp.name) / 'report.json'

    def test_configuration_header_avoids_reading_execution_evidence(self):
        document = json.dumps({'configuration': {'judge_fingerprint': 'same'},
                               'trials': ['x' * 600000]})
        class BoundedStream(io.BytesIO):
            consumed = 0
            def read(stream, count=-1):
                self.assertGreater(count, 0)
                result = super().read(count)
                stream.consumed += len(result)
                return result
        stream = BoundedStream(document.encode())
        with patch.object(Path, 'open', return_value=stream), \
                patch.object(layout, 'read', side_effect=AssertionError('no full historical read')):
            self.assertEqual(compare._baseline_report_judge_fingerprint(self.path), 'same')
        self.assertEqual(stream.consumed, 4096)

    def test_unicode_escapes_and_configuration_cross_chunk_boundaries(self):
        document = {'note': '中\\"文' * 1100,
                    'configuration': {'judge_fingerprint': 'escaped-"-中', 'note': '字' * 5000},
                    'trials': ['x' * 600000]}
        self.path.write_text(json.dumps(document, ensure_ascii=False), encoding='utf-8')
        with patch.object(layout, 'read', side_effect=AssertionError('no full read')):
            self.assertEqual(compare._baseline_report_judge_fingerprint(self.path), 'escaped-"-中')

    def test_configuration_after_trials_uses_the_original_reader(self):
        self.path.write_text(json.dumps({'trials': [],
                                         'configuration': {'judge_fingerprint': 'late'}}))
        with patch.object(layout, 'read', wraps=layout.read) as reader:
            self.assertEqual(compare._baseline_report_judge_fingerprint(self.path), 'late')
        reader.assert_called_once_with(self.path)

    def test_oversized_configuration_uses_the_original_reader(self):
        self.path.write_text(json.dumps({'configuration': {
            'note': 'x' * 300000, 'judge_fingerprint': 'large'}}))
        with patch.object(layout, 'read', wraps=layout.read) as reader:
            self.assertEqual(compare._baseline_report_judge_fingerprint(self.path), 'large')
        reader.assert_called_once_with(self.path)

    def test_deep_configuration_uses_the_original_reader(self):
        # Python 3.12's C JSON scanner has a separate recursion limit.
        depth = 50000
        self.path.write_text('{"configuration":{"nested":' + '[' * depth + '0' +
                             ']' * depth + ',"judge_fingerprint":"deep"}}')
        with patch.object(layout, 'read', return_value={
                'configuration': {'judge_fingerprint': 'deep'}}) as reader:
            self.assertEqual(compare._baseline_report_judge_fingerprint(self.path), 'deep')
        reader.assert_called_once_with(self.path)

    def test_invalid_unicode_keeps_original_failure(self):
        self.path.write_bytes(b'{"configuration":{"judge_fingerprint":"\xff"}}')
        with patch.object(layout, 'read', wraps=layout.read) as reader:
            with self.assertRaises(UnicodeError):
                compare._baseline_report_judge_fingerprint(self.path)
        reader.assert_called_once_with(self.path)

    def test_replaced_source_is_reopened_without_a_stale_hint(self):
        for fingerprint in ['before', 'after']:
            self.path.write_text(json.dumps({'configuration': {'judge_fingerprint': fingerprint}}))
            self.assertEqual(compare._baseline_report_judge_fingerprint(self.path), fingerprint)


class BaselineMetadataSourceValidationTest(unittest.TestCase):
    def setUp(self):
        from test_appworld_baseline_subset import LargerBaselineSubsetTest
        LargerBaselineSubsetTest.setUp(self)
        # Match the native writer: metadata precedes the execution rows.
        self.native = {'configuration': self.native['configuration'],
                       'complete': True, 'trials': self.native['trials']}
        self.write(self.source, self.native)

    @staticmethod
    def write(path, value):
        path.parent.mkdir(parents=True, exist_ok=True)
        path.write_text(json.dumps(value))

    def larger(self):
        return compare._larger_baseline_source(self.small, self.ids, self.config, self.tasks)

    def original_larger(self):
        with patch.object(compare, '_baseline_report_judge_fingerprint', side_effect=lambda path:
                (layout.read(path).get('configuration') or {}).get('judge_fingerprint')):
            return self.larger()

    def test_same_profile_result_is_identical_without_parsing_historical_trials(self):
        history = self.large / 'regrade/retry-01/report.json'
        self.write(history, {'configuration': {'judge_fingerprint': 'older-native-judge'},
                             'trials': ['x' * 600000]})
        before = self.original_larger()
        with patch.object(layout, 'read', wraps=layout.read) as reader:
            self.assertEqual(self.larger(), before)
        self.assertNotIn(history, [call.args[0] for call in reader.call_args_list])
        # Actual selected execution is still parsed and compared in full.
        self.assertIn(self.source, [call.args[0] for call in reader.call_args_list])

    def test_changed_profile_remains_rejected(self):
        self.result['judge_projection']['require_state_dependency'] = False
        self.write(self.large / 'result.json', self.result)
        self.assertIsNone(self.original_larger())
        self.assertIsNone(self.larger())

    def test_unknown_profile_remains_rejected(self):
        self.result.pop('judge_projection')
        self.write(self.large / 'result.json', self.result)
        self.assertIsNone(self.original_larger())
        self.assertIsNone(self.larger())

    def test_selected_truncated_tail_cannot_be_accepted_from_its_metadata_hint(self):
        self.source.write_text(json.dumps(self.native)[:-20])
        self.assertEqual(compare._baseline_report_judge_fingerprint(self.source),
                         'fixture-native-judge')
        with self.assertRaises(json.JSONDecodeError):
            self.original_larger()
        with self.assertRaises(json.JSONDecodeError):
            self.larger()

    def test_selected_execution_and_cost_changes_still_fail_full_source_checks(self):
        changed = deepcopy(self.native)
        changed['trials'][0]['metrics']['tokens'] += 1
        self.write(self.source, changed)
        before = self.original_larger()
        after = self.larger()
        self.assertEqual(after, before)
        # The changed cost-bearing report is rejected; the original healthy
        # capture remains an independently valid fallback, as before.
        self.assertNotEqual(after[1], self.source)
        self.assertEqual(after[0]['trials'][0]['metrics'],
                         self.small_capture['trials'][0]['metrics'])


if __name__ == '__main__':
    unittest.main()
