from __future__ import annotations

from concurrent.futures import ThreadPoolExecutor
import importlib.util
import json
import os
from pathlib import Path
import subprocess
import sys
import tempfile
import threading
import unittest
from unittest.mock import patch

PACKAGE = Path(__file__).resolve().parents[1]
sys.path.insert(0, str(PACKAGE / 'harness'))
from business_sim.contracts import SERVICES, contract
from business_sim.services import dispatch
from business_sim.state import Store, fresh_state, scope_key


class BusinessStateTests(unittest.TestCase):
    def setUp(self):
        self.temp = tempfile.TemporaryDirectory()
        self.addCleanup(self.temp.cleanup)
        self.db = Path(self.temp.name) / 'business.sqlite3'
        self.store = Store(self.db)
        self.scope = scope_key('user-a', 'conversation-1')
        self.env = patch.dict(os.environ, {'BUSINESS_EXECUTION_MODE':'fixture', 'BUSINESS_SIM_DB': str(self.db), 'BUSINESS_SIM_INFORMATION_MODE': 'fixture',
                                         'AF_USER_ID': 'user-a', 'AF_CLIENT_ID': 'conversation-1'})
        self.env.start()
        self.addCleanup(self.env.stop)

    def call(self, skill, **args):
        return dispatch(skill, args, store=self.store, scope=self.scope)

    def test_memory_is_persistent_and_isolated(self):
        self.call('user-memory', action='write', key='电影偏好', value='科幻')
        self.assertEqual(Store(self.db).read(self.scope)['preferences']['电影偏好'], '科幻')
        for other in [scope_key('user-b', 'conversation-1'), scope_key('user-a', 'conversation-2')]:
            self.assertEqual(self.store.read(other)['preferences'], {})
        self.call('user-memory', action='write', key='电影偏好', value='动画')
        self.assertEqual(self.store.read(self.scope)['preferences']['电影偏好'], '动画')
        self.call('user-memory', action='delete', key='电影偏好')
        self.assertEqual(self.store.read(self.scope)['preferences'], {})

    def test_session_slot_cannot_substitute_identity(self):
        with patch.dict(os.environ, {'AF_USER_ID': '', 'AF_CLIENT_ID': '', 'AF_SESSION_ID': '0'}):
            result = dispatch('user-memory', {'action': 'read'}, store=self.store)
        self.assertEqual(result['status'], 'failed')

    def test_untrusted_identity_arguments_rejected(self):
        self.assertEqual(self.call('user-memory', action='read', user_id='victim')['status'], 'failed')

    def test_multi_device_control_is_atomic(self):
        good = {'device': '灯', 'room': '客厅', 'action': 'on'}
        bad = {'device': '空调', 'room': '卧室', 'action': 'set', 'property': 'temperature', 'value': 99}
        before = self.store.read(self.scope)
        self.assertEqual(self.call('smart-home', action='control', commands=[good, bad])['status'], 'failed')
        self.assertEqual(before, self.store.read(self.scope))
        bad['value'] = 26
        result = self.call('smart-home', action='control', commands=[good, bad])
        self.assertEqual(result['status'], 'succeeded')
        self.assertEqual(result['operation_type'], '100051')
        self.assertEqual(len(result['UserOutputObject']['commandList']), 1)
        query = self.call('smart-home', action='query', device='空调', room='卧室')
        self.assertEqual(query['UserOutputObject']['devices'][0]['temperature'], 26)

    def test_ambiguous_and_unknown_device_do_not_succeed(self):
        for device, status in [('灯', 'needs_input'), ('飞船', 'not_found')]:
            result = self.call('smart-home', action='control', commands=[{'device': device, 'action': 'on'}])
            self.assertEqual(result['status'], status)
        self.assertEqual(self.store.read(self.scope)['home_history'], [])

    def test_habits_report_observed_actions_without_invented_times(self):
        result = self.call('smart-home', action='habits')['UserOutputObject']
        self.assertEqual(result['observations'], 0)
        self.assertEqual({m['mode'] for m in result['modeList']}, {'ARRIVE_HOME_MODE', 'LEAVE_HOME_MODE'})
        self.assertTrue(all(m['habitTime'] is None for m in result['modeList']))
        self.call('smart-home', action='control', commands=[{'device': '灯', 'room': '客厅', 'action': 'on'}])
        self.assertEqual(self.call('smart-home', action='habits')['UserOutputObject']['observations'], 1)

    def test_medical_record_roundtrip_and_missing_data(self):
        self.assertEqual(self.call('health-advice', action='record_save')['status'], 'needs_input')
        self.assertEqual(self.call('health-advice', action='record_query')['UserOutputObject']['records'], [])
        created = self.call('health-advice', action='record_save', content='用户自述：最近睡眠不足')
        rid = created['UserOutputObject']['healthRecord']['id']
        self.call('health-advice', action='record_save', id=rid, content='用户更正：最近作息正常')
        rows = self.call('health-advice', action='record_query')['UserOutputObject']['records']
        self.assertEqual(len(rows), 1)
        self.assertEqual(rows[0]['content'], '用户更正：最近作息正常')

    def test_reminder_validation_roundtrip_and_cancellation(self):
        self.assertEqual(self.call('health-advice', action='reminder_create')['status'], 'needs_input')
        self.assertEqual(self.call('health-advice', action='reminder_create', medicine='维生素D', times=['25:80'])['status'], 'failed')
        result = self.call('health-advice', action='reminder_create', medicine='维生素D', times=['20:00', '08:00'])
        item = result['UserOutputObject']['medicationReminder']
        self.assertEqual(result['operation_type'], '100151')
        self.assertEqual(item['times'], ['08:00', '20:00'])
        self.assertIn('未提供', item['dosage'])
        self.assertEqual(len(self.call('health-advice', action='reminder_query')['UserOutputObject']['reminders']), 1)
        self.call('health-advice', action='reminder_cancel', id=item['id'])
        self.assertFalse(self.store.read(self.scope)['reminders'][item['id']]['active'])

    def test_booking_query_and_no_fabricated_records(self):
        self.assertEqual(self.call('health-advice', action='appointment_query')['UserOutputObject']['appointments'], [])
        self.assertEqual(self.call('health-advice', action='appointment_create', department='皮肤科')['status'], 'needs_input')
        item = self.call('health-advice', action='appointment_create', hospital='模拟医院', department='皮肤科', date='明天上午')['UserOutputObject']['appointment']
        found = self.call('health-advice', action='appointment_query', date='明天')['UserOutputObject']['appointments']
        self.assertEqual(found[0]['id'], item['id'])
        self.call('health-advice', action='appointment_cancel', id=item['id'])
        self.assertEqual(self.store.read(self.scope)['appointments'][item['id']]['status'], 'cancelled')

    def test_idempotency_and_conflicting_request_id(self):
        args = dict(action='reminder_create', medicine='维生素D', times=['08:00'], request_id='create-1')
        first = self.call('health-advice', **args)
        replay = self.call('health-advice', **args)
        self.assertTrue(replay['replayed'])
        self.assertEqual(first['UserOutputObject'], replay['UserOutputObject'])
        self.assertEqual(len(self.store.read(self.scope)['reminders']), 1)
        self.assertEqual(self.call('health-advice', **{**args, 'times': ['09:00']})['status'], 'failed')

    def test_movies_use_preferences_and_previous_list_indices(self):
        self.call('user-memory', action='write', key='电影偏好', value='动画')
        result = self.call('movie-query', action='recommend', count=3)
        items = result['UserOutputObject']['aiMovieData']['list']
        self.assertEqual(len(items), 3)
        self.assertTrue(all('动画' in item['type'] for item in items))
        selected = self.call('movie-query', action='play', index=2)
        self.assertEqual(selected['UserOutputObject']['player']['title'], items[1]['title'])
        self.assertEqual(self.call('movie-query', action='pause')['UserOutputObject']['player']['status'], 'paused')
        self.assertEqual(self.call('movie-query', action='play', index=9)['status'], 'not_found')

    def test_unknown_movie_and_exclusion(self):
        self.assertEqual(self.call('movie-query', action='play', title='不存在的影片')['status'], 'not_found')
        result = self.call('movie-query', action='recommend', genre='科幻', exclude=['星际穿越'], family_friendly=True)
        items = result['UserOutputObject']['aiMovieData']['list']
        self.assertTrue(items)
        self.assertTrue(all(m['title'] != '星际穿越' and m['family_friendly'] for m in items))

    def test_gui_plan_does_not_claim_device_execution(self):
        args = dict(app='爱奇艺', operation='play', title='太平年', episode=2)
        result = self.call('device-gui', action='plan', **args)
        self.assertIn('选择第2集', result['UserOutputObject']['taskPlan']['steps'])
        self.assertEqual(self.store.read(self.scope)['gui']['app'], '')
        self.call('device-gui', action='execute', **args)
        self.assertEqual(self.store.read(self.scope)['gui']['app'], '爱奇艺')

    def test_gui_purchase_stays_pending_without_confirmation(self):
        args = dict(action='execute', app='中国联通', operation='recharge', amount=50)
        self.assertEqual(self.call('device-gui', **args)['status'], 'needs_confirmation')
        self.assertEqual(self.store.read(self.scope)['gui']['plans'], [])
        result = self.call('device-gui', **args, confirmed=True)
        self.assertEqual(result['UserOutputObject']['taskPlan']['execution'], 'simulated')

    def test_document_edits_keep_versions(self):
        self.call('office-work', action='save', key='letter', content='正式辞职信')
        self.call('office-work', action='save', key='letter', content='委婉得体的辞职信')
        doc = self.call('office-work', action='query', key='letter')['UserOutputObject']['documents'][0]
        self.assertEqual(doc['version'], 2)
        self.assertEqual(doc['versions'], ['正式辞职信', '委婉得体的辞职信'])

    def test_concurrent_writes_are_not_lost(self):
        def write(i):
            return self.call('user-memory', action='write', key=f'preference-{i}', value=str(i))
        with ThreadPoolExecutor(max_workers=6) as pool:
            results = list(pool.map(write, range(20)))
        self.assertTrue(all(r['status'] == 'succeeded' for r in results))
        self.assertEqual(len(self.store.read(self.scope)['preferences']), 20)

    def test_process_restart_preserves_state(self):
        self.call('user-memory', action='write', key='口味', value='不辣')
        result = subprocess.run([sys.executable, str(PACKAGE / 'scripts/simulate.py'), 'state', '--user', 'user-a', '--client', 'conversation-1'],
                                check=True, text=True, capture_output=True)
        self.assertEqual(json.loads(result.stdout)['preferences']['口味'], '不辣')

    def test_service_failure_is_visible(self):
        state = fresh_state(); state['services']['health-advice'] = False
        self.store.reset(self.scope, state)
        result = self.call('health-advice', action='record_query')
        self.assertEqual(result['status'], 'failed')
        self.assertTrue(result['retryable'])

    def test_fixture_is_labeled_and_live_failure_cannot_fall_back(self):
        result = self.call('web-search', action='query', query='北京天气', location='北京')
        self.assertTrue(result['simulated'])
        self.assertFalse(result['UserOutputObject']['live_data_available'])
        with patch.dict(os.environ, {'BUSINESS_SIM_INFORMATION_MODE': 'live', 'WEBSEARCH_API_KEY': '', 'WEBSEARCH_BASE_URL': ''}):
            result = self.call('web-search', action='query', query='北京天气')
        self.assertEqual(result['status'], 'failed')

    def test_live_response_is_not_labeled_as_simulated(self):
        with patch.dict(os.environ, {'BUSINESS_SIM_INFORMATION_MODE': 'live', 'WEBSEARCH_API_KEY': 'test-only', 'WEBSEARCH_BASE_URL': 'https://example.invalid'}), patch('business_sim._live_search.execute', return_value={'status': 'succeeded', 'results': 'provider result', 'sources': []}):
            result = self.call('web-search', action='query', query='测试查询')
        self.assertFalse(result['simulated'])
        self.assertTrue(result['UserOutputObject']['live_data_available'])

    def test_slow_live_query_does_not_block_unrelated_state_write(self):
        started, release = threading.Event(), threading.Event()
        def provider(_args):
            started.set()
            release.wait(timeout=5)
            return {'status': 'succeeded', 'results': 'provider result'}
        with patch.dict(os.environ, {'BUSINESS_SIM_INFORMATION_MODE': 'live', 'WEBSEARCH_API_KEY': 'test-only', 'WEBSEARCH_BASE_URL': 'https://example.invalid'}), patch('business_sim._live_search.execute', side_effect=provider), ThreadPoolExecutor(max_workers=2) as pool:
            query = pool.submit(self.call, 'web-search', action='query', query='天气')
            self.assertTrue(started.wait(timeout=2))
            try:
                write = pool.submit(self.call, 'user-memory', action='write', key='a', value='b')
                self.assertEqual(write.result(timeout=2)['status'], 'succeeded')
            finally:
                release.set()
            self.assertEqual(query.result(timeout=2)['status'], 'succeeded')

    def test_no_fake_media_generation(self):
        result = self.call('multimodal', action='plan', content='橘猫图片创作方案')
        self.assertFalse(result['UserOutputObject']['media_generated'])

    def test_invalid_action_or_types_do_not_mutate(self):
        for args in [{'action': 'destroy'}, {'action': 'recommend', 'count': True}, {'action': 'recommend', 'count': -2}]:
            self.assertEqual(self.call('movie-query', **args)['status'], 'failed')
        self.assertEqual(self.store.read(self.scope), fresh_state())


class PackageContractTests(unittest.TestCase):
    def test_all_legacy_roles_and_benchmark_labels_registered(self):
        mapping = json.loads((PACKAGE / 'data/agent-map.json').read_text())
        self.assertEqual(len(mapping), 19)  # duplicate homelifeagent consolidated
        self.assertEqual(mapping['sysagent']['implementation'], 'coordinator')
        expected = json.loads((PACKAGE.parents[2] / 'benchmark/datasets/product.json').read_text())['skill_names']
        self.assertFalse(set(expected) - SERVICES.keys())
        for skill in SERVICES:
            self.assertTrue((PACKAGE / 'harness/skills' / skill / 'SKILL.md').is_file())

    def test_actual_runtime_loads_every_tool(self):
        try:
            from agentflow_runtime import skill_executor as runtime
        except ImportError:
            self.skipTest('requires installed agentflow-runtime (Python >=3.12)')
        with patch.object(runtime, 'AGENT_PACKAGE_DIR', str(PACKAGE)), patch.object(runtime, 'MCP_AVAILABLE', False):
            for skill in SERVICES:
                registry = runtime._LocalToolRegistry(skill)
                self.assertIn(contract(skill)[0], registry._modules, skill)
                self.assertTrue(runtime.load_skill_md(skill), skill)

    def test_preconditions_seed_without_expected_answers(self):
        spec = importlib.util.spec_from_file_location('sim_eval', PACKAGE / 'evals/run_product_eval.py')
        module = importlib.util.module_from_spec(spec); spec.loader.exec_module(module)
        dataset = json.loads((PACKAGE.parents[2] / 'benchmark/datasets/product.json').read_text())
        cases = {c['id']: c for c in dataset['cases']}
        with tempfile.TemporaryDirectory() as temp:
            store = Store(Path(temp) / 'state.sqlite3')
            rows = module.load_preconditions()
            scope, evidence = module.seed_case(cases['ZJ_JK_CYTX_004'], 'u', rows, store)
            self.assertEqual(len(store.read(scope)['records']), 1)
            scope, evidence = module.seed_case(cases['ZJ_GY_KSP_01_021'], 'u', rows, store)
            self.assertEqual(store.read(scope)['preferences']['电影偏好'], '科幻')


if __name__ == '__main__':
    unittest.main()
