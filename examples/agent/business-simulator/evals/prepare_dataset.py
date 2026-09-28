#!/usr/bin/env python3
"""业务原表 → 自进化包原生冻结数据集；不调用模型，不生成标准答案。"""
from __future__ import annotations

import argparse
from collections import Counter, defaultdict
from copy import deepcopy
import hashlib
import json
from pathlib import Path
import random
import subprocess
import sys
import tempfile

PACKAGE = Path(__file__).resolve().parents[1]
REPO = PACKAGE.parents[2]
sys.path[:0] = [str(REPO / 'benchmark'), str(PACKAGE / 'harness')]
from converters import convert_product, load_domain_map, merge_evalsets
from evalset_schema import normalize_evalset, validate_evalset


def fingerprint(value):
    return hashlib.sha256(json.dumps(value, ensure_ascii=False, sort_keys=True).encode()).hexdigest()


def prepare_product_splits(dataset, sizes, seed):
    """整段会话不拆分；同输入的不同设备/前置条件也不能跨集合。"""
    errors = validate_evalset(dataset)
    if errors:
        raise ValueError('; '.join(errors[:5]))
    groups, excluded = defaultdict(list), []
    for case in dataset['cases']:
        missing = [{'turn': index, 'fields': [key for key in ('user', 'expected_behavior')
                    if not turn.get(key, '').strip()]}
                   for index, turn in enumerate(case['turns'], 1)]
        missing = [item for item in missing if item['fields']]
        if missing:
            excluded.append({'case_id': case['id'], 'missing': missing})
            continue
        inputs = [' '.join(turn['user'].split()) for turn in case['turns']]
        groups['product-dialogue:' + fingerprint(inputs)].append(case)
    pool = sorted(groups.items())
    random.Random(seed).shuffle(pool)
    splits = {}
    for split, size in sizes.items():
        selected, remaining = [], []
        for group, cases in pool:
            if len(selected) + len(cases) <= size:
                for original in cases:
                    case = deepcopy(original)
                    case.update(split=split)
                    case['provenance']['group_id'] = group
                    selected.append(case)
            else:
                remaining.append((group, cases))
        if len(selected) != size:
            raise ValueError(f'{split}: 完整且不跨组的用例不足，要求 {size}，实际 {len(selected)}')
        splits[split], pool = selected, remaining
    return splits, {'source_cases': len(dataset['cases']), 'excluded_cases': excluded,
                    'eligible_cases': sum(len(v) for v in groups.values()),
                    'unselected_case_ids': [c['id'] for _, cases in pool for c in cases],
                    'same_input_groups': sum(len(v) > 1 for v in groups.values())}


def evaluation_task(case, profile_hash, reviewed=False):
    """验收标签放 attributes，不进入 initial_state 或实际用户消息。"""
    import re
    provenance = case['provenance']
    turns = []
    for index, turn in enumerate(case['turns'], 1):
        turns.append({'turn': index, 'expected_behavior': turn['expected_behavior'],
                      'expected_targets': turn.get('expected_skills'),
                      'expected_intents': turn.get('expected_intents'),
                      'expected_effects': turn.get('expected_operation_types'),
                      'conditional': bool(re.search(r'如果|否则|若|满足.*才|符合.*才|不.*就|没有.*才',
                          turn['user'] + '\n' + turn['expected_behavior']))})
    return {'task_id': case['id'], 'group_id': provenance['group_id'],
            'task_input': case['turns'][0]['user'],
            'followup_inputs': [turn['user'] for turn in case['turns'][1:]],
            'expected_behavior': '逐轮满足 turn_expectations 的适用要求；后轮修改只覆盖相应旧要求，不能抹去此前已发生的错误。',
            'category': case['category'], 'difficulty': case['difficulty'],
            'initial_state': {'schema': 'business-simulator.initial-state.v1',
                'profile_sha256': profile_hash, 'precondition': provenance.get('precondition', ''),
                'device_type': provenance.get('device_type', ''),
                'voiceprint_id': provenance.get('voiceprint_id', '')},
            'constraints': case.get('constraints', []),
            'budget': {'max_steps': 128 * len(turns), 'max_tool_calls': 96 * len(turns),
                       'timeout_ms': 1800000},
            'split': case['split'], 'review_status': 'reviewed' if reviewed else 'pending',
            'provenance': {'kind': 'golden', 'learning_exposed': False},
            'attributes': {'turn_expectations': turns,
                'product_source': {'workbook': Path(provenance['source']).name,
                    'original_case_id': provenance['original_case_id'], 'title': provenance['title'],
                    'scenarios': provenance['scenarios'], 'case_level': case.get('case_level', '')}}}


def host_revision(environment=None):
    """生成与冻结部署相同的宿主指纹，只绑定源码和公开模型身份，不包含凭据。"""
    import os
    environment = os.environ if environment is None else environment
    files = sorted(p for folder in (PACKAGE, REPO/'packages/agentflow-runtime/src') for p in folder.rglob('*')
                   if p.is_file() and p.suffix in ('.py', '.sh', '.json', '.md')
                   and not set(p.relative_to(folder).parts) & {'outputs', '__pycache__', '.venv', 'tests', 'evals', 'docs'})
    return fingerprint({'files': {str(p.relative_to(REPO)): hashlib.sha256(p.read_bytes()).hexdigest() for p in files},
                        'model': environment.get('AF_LLM_MODEL', ''), 'endpoint': environment.get('AF_LLM_BASE_URL', '')})


def prepare_configuration(destination, dataset):
    """复用组件配置，仅填写此业务 Agent 的普通启动命令和环境参数。"""
    import os
    import shutil
    component = REPO/'tools/trajectory_evolution'
    destination = destination.resolve()
    if destination.exists():
        raise ValueError('配置目录已存在，不覆盖已有实验配置')
    values = {name: json.loads((component/'config'/source).read_text()) for name, source in {
        'runtime.json': 'standard-runtime.json', 'service.json': 'boundary-service.json',
        'evaluation.json': 'evaluation.json', 'agent-evaluation.json': 'agent-evaluation.json', 'gepa.json': 'gepa.json'}.items()}
    runtime = values['runtime.json']
    runtime['evolution']['enable_meta_reflection'] = True
    runtime['evaluation']['strict_model'] = True
    runtime['model'].update(api_key_env='OPENAI_API_KEY', thinking_mode='disabled', timeout_ms=120000,
                            max_output_tokens=16384, sessionlog_max_output_tokens=16384)
    base_url = os.environ.get('OPENAI_BASE_URL') or os.environ.get('AF_LLM_BASE_URL', '')
    runtime['model']['endpoint'] = base_url.rstrip('/') + '/chat/completions' if base_url else ''
    runtime['model']['model_id'] = os.environ.get('OPENAI_MODEL') or os.environ.get('AF_LLM_MODEL', '')
    # 绑定宿主源码、v1 runtime 和模型身份；不读取、复制或记录任何凭据值。
    revision = host_revision()
    service = values['service.json']
    service.update(application_id='business-simulator', data_dir='trace_output/business-evaluation-service', port=0,
        principals=[{'user_id': 'business-training', 'token_env': 'EVOLUTION_BUSINESS_TOKEN'}],
        instrumentation={
            'level': 'L3', 'capabilities': ['model', 'actions', 'loop', 'schedule'],
            # 冻结环境缺少外部回执时，重复调用不会恢复能力；保留原始工具输出，
            # 仅将这次动作标记为不可重试失败，供通用循环控制读取。
            'action_outcomes': [{
                'pointer': '/error', 'equals': 'snapshot_miss', 'status': 'failed',
                'error': {'code': 'snapshot_miss', 'retryability': 'non_retryable'},
            }],
            # v1 的模型客户端可能正常返回失败对象；只标记该次模型活动失败，
            # 不改变主工作流终态，也不将未知 token 用量伪装成零。
            'model_outcomes': [{
                'pointer': '/_llm_error', 'equals': True, 'status': 'failed',
                'error': {'code': 'model_call_failed', 'retryability': 'unknown'},
            }],
        },
        runtime_config='runtime.json', policy_evaluation_file='agent-evaluation.json', evaluation_levels=['L3'])
    service['experience_template_file'] = 'prompts/runtime-experience.txt'
    for name, filename in {'distill': 'shared-experience-distill.txt',
                           'review': 'shared-experience-review.txt',
                           'relation': 'cross-memory-relation.txt'}.items():
        service['sharing'][name+'_prompt_file'] = 'prompts/' + filename
    service['sharing']['relation_batch_prompt_file'] = 'prompts/cross-memory-batch.txt'
    service['control_learning'] = {'enabled': True, 'prompt_file': 'prompts/control-candidates.txt'}
    service['learning'].update(min_new_runs=4, max_batch_runs=16)
    evaluation = values['evaluation.json']
    evaluation.update(splits=['train', 'val', 'holdout'], parallelism=2, max_trials=2000,
                      require_trace=True, max_trace_bytes=67108864, max_report_bytes=134217728,
                      max_output_bytes=4194304, resources={'nice': 10, 'address_space_bytes': 8589934592, 'cpu_seconds': 1800})
    evaluation['variants'] = [{'id': 'business-v1', 'version': revision,
        'command': ['python3', '{config_dir}/' + os.path.relpath(PACKAGE/'evals/run_product_eval.py', destination), '--evolution-entry'],
        'instrumentation_config': 'service.json', 'initial_state_transport': 'file',
        'entry': {'input': {name: {'$eval': name} for name in ('messages', 'user_id', 'conversation_id', 'run_key', 'budget')},
                  'output_pointer': '/response'},
        'pass_env': ['AF_LLM_API_KEY', 'AF_LLM_BASE_URL', 'AF_LLM_MODEL', 'AF_LLM_THINKING_MODE', 'AF_LLM_TIMEOUT',
                     'AF_NODE_EXECUTION_TIMEOUT', 'AF_MAX_SKILL_TASK_ITERATIONS', 'OPENAI_IMAGE_API_KEY',
                     'OPENAI_IMAGE_BASE_URL', 'OPENAI_IMAGE_MODEL', 'AGENTFLOW_DAEMON', 'BUSINESS_EVALUATION_POINT_TIMEOUT_MS']}]
    evaluation['judge'].update(runtime_config='runtime.json', prompt='prompts/task-judge-system.txt',
        loop_review_prompt='prompts/task-loop-review.txt', max_model_calls=4096,
        share_repeated_inputs=True)
    assessment = values['agent-evaluation.json']
    assessment.update(dataset=os.path.relpath(dataset.resolve(), destination), evaluation_config='evaluation.json', agent={'revision': revision}, gepa_config='gepa.json')
    assessment['training'].update(batch_size=16, max_batches=25)
    values['gepa.json'].update(runtime_config='runtime.json', prompt_file='prompts/gepa-reflection.txt',
                              max_trials=6000, timeout_ms=18000000,
                              minibatch_size=32, merge_minibatch_size=16)
    destination.mkdir(parents=True)
    # 运行时模板要求位于配置目录内；固定副本也避免一次实验中提示词被热替换。
    shutil.copytree(component/'config/prompts', destination/'prompts')
    for name, content in values.items():
        (destination/name).write_text(json.dumps(content, ensure_ascii=False, indent=2)+'\n', encoding='utf-8')
    return destination


def main(argv=None):
    parser = argparse.ArgumentParser(description=__doc__)
    parser.add_argument('--output', type=Path, default=REPO/'tools/trajectory_evolution/experiments/business-coverage/datasets/source')
    parser.add_argument('--seed', type=int, default=20260921)
    parser.add_argument('--train', type=int, default=400)
    parser.add_argument('--val', type=int, default=200)
    parser.add_argument('--holdout', type=int, default=200)
    parser.add_argument('--evolution-bin', type=Path, default=REPO/'build/trajectory-evolution/trajectory_evolution')
    parser.add_argument('--reviewed', action='store_true', help='仅在业务方已确认所选验收标准后使用；不会自动审核')
    parser.add_argument('--config-output', type=Path, help='另生成原生评测/训练/GEPA 配置；不启动实验')
    parser.add_argument('--config-only', action='store_true', help='为 --output 指向的现有冻结集生成配置，不重新划分')
    args = parser.parse_args(argv)
    output = args.output.resolve()
    if args.config_only:
        if not args.config_output or not (output/'manifest.json').is_file():
            parser.error('--config-only 需要现有冻结集和新的 --config-output')
        subprocess.run([str(args.evolution_bin.resolve()), 'dataset', 'check', str(output)], check=True)
        print(prepare_configuration(args.config_output, output))
        return
    if output.exists() or output.is_symlink():
        parser.error('目标目录已存在，不覆盖已冻结的数据集')
    if args.config_output and args.config_output.exists():
        parser.error('配置目录已存在，不覆盖已有实验配置')
    sizes = {k: getattr(args, k) for k in ('train', 'val', 'holdout')}
    if any(n <= 0 for n in sizes.values()):
        parser.error('三个集合的数量都必须为正数')
    mapping = load_domain_map(REPO/'benchmark/domain_skill_map.json')
    sources = sorted((REPO/'test_cases/product').glob('*.xlsx'))
    warnings = []
    dataset = normalize_evalset(merge_evalsets([convert_product(p, mapping, warnings) for p in sources]))
    if warnings:
        raise ValueError('转换存在未解决告警：' + '; '.join(warnings[:5]))
    splits, summary = prepare_product_splits(dataset, sizes, args.seed)
    from business_sim.evaluation_world import load_profile
    profile = load_profile(PACKAGE/'data/evaluation-profile.json')
    tasks = [evaluation_task(case, profile['sha256'], args.reviewed)
             for cases in splits.values() for case in cases]
    summary.update(seed=args.seed, selection_unit='whole_dialogue_group',
        profile_sha256=profile['sha256'],
        sources={p.name: hashlib.sha256(p.read_bytes()).hexdigest() for p in sources},
        splits={name: {'cases': len(cases), 'turns': sum(len(c['turns']) for c in cases),
            'multi_turn_cases': sum(len(c['turns']) > 1 for c in cases),
            'intent_labeled_turns': sum(t['expected_skills'] is not None for c in cases for t in c['turns']),
            'categories': dict(Counter(c['category'] for c in cases))} for name, cases in splits.items()})
    output.parent.mkdir(parents=True, exist_ok=True)
    # 临时文件仅供原生 builder 导入；最终只保存一套权威任务，不生成第二套评测库。
    with tempfile.TemporaryDirectory(prefix='.prepare-dataset-', dir=output.parent) as temporary:
        staging = Path(temporary)
        (staging/'tasks.jsonl').write_text(''.join(json.dumps(t, ensure_ascii=False) + '\n' for t in tasks))
        config = {'schema_version': 1, 'name': 'business-product-v1', 'split_seed': str(args.seed),
                  'split_percent': [50, 25, 25], 'max_tasks': len(tasks), 'max_model_calls': 0,
                  'budget': {'max_steps': 128, 'max_tool_calls': 96, 'timeout_ms': 1800000},
                  'sources': [{'kind': 'golden', 'path': 'tasks.jsonl'}]}
        (staging/'config.json').write_text(json.dumps(config))
        result = subprocess.run([str(args.evolution_bin.resolve()), 'dataset', 'build', '--config',
            str(staging/'config.json'), '--output-root', str(staging/'frozen')],
            capture_output=True, text=True)
        if result.returncode:
            manifest = staging/'frozen/manifest.json'
            details = json.loads(manifest.read_text()) if manifest.is_file() else {}
            raise ValueError('原生构建拒绝了选择集：' + result.stdout + result.stderr
                             + json.dumps(details.get('issues', []), ensure_ascii=False))
        built = json.loads(result.stdout)
        if built['task_count'] != len(tasks) or built['splits'] != sizes or built['rejected_count']:
            raise ValueError('原生构建数量与选择结果不一致：' + result.stdout)
        subprocess.run([str(args.evolution_bin.resolve()), 'dataset', 'check', str(staging/'frozen')], check=True)
        (staging/'frozen/selection.json').write_text(json.dumps(summary, ensure_ascii=False, indent=2) + '\n')
        (staging/'frozen').rename(output)
    if args.config_output:
        prepare_configuration(args.config_output, output)
    print(json.dumps({'output': str(output), 'excluded_cases': len(summary['excluded_cases']),
        'unselected_cases': len(summary['unselected_case_ids']), 'splits': summary['splits'],
        'review_status': 'reviewed' if args.reviewed else 'pending'}, ensure_ascii=False, indent=2))


if __name__ == '__main__':
    main()
