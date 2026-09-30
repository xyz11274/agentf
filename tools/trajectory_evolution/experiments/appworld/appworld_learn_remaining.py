"""Analyze previously unprocessed, genuinely scored train-01 runs once."""
from __future__ import annotations

from copy import deepcopy
import json
import os
import time

import appworld_experiment as controller
from appworld_learning_prompts import install


def synthesis_summary(report):
    store = controller.TRAINING / 'evolution'
    run_ids = set()
    for trial in report.get('trials', []):
        evidence = trial.get('evidence') or {}
        for trace in evidence.get('traces') or [evidence.get('trace')]:
            run_ids.add(trace['identity']['run_id'])
    matching = []
    for path in (store / 'learning/jobs').glob('*.json'):
        job = controller.read(path)
        if set(job.get('analysis_model_calls', {})) == run_ids:
            matching.append(job)
    if not matching:
        return {'synthesis_status': 'not_observed'}
    batch_ids = {job.get('batch_id') for job in matching}
    for path in (store / 'by-user' / controller.OWNER / 'batches').glob('*/batch.evaluation.json'):
        batch = controller.read(path)
        if batch.get('batch_id') not in batch_ids:
            continue
        meta = batch.get('meta_reflection', {})
        memory = batch.get('memory_update', {})
        failed = int(memory.get('model_failed_count', 0))
        degraded = meta.get('status') == 'MODEL_CALL_STATUS_TRANSPORT_ERROR' or failed > 0
        return {'synthesis_status': 'partial_synthesis_failure' if degraded else 'completed',
                'batch_result': str(path), 'meta_status': meta.get('status'),
                'meta_error_code': meta.get('error_code'), 'meta_attempts': meta.get('attempts'),
                'curation_model_requested': int(memory.get('model_requested_count', 0)),
                'curation_model_succeeded': int(memory.get('model_succeeded_count', 0)),
                'curation_model_failed': failed}
    return {'synthesis_status': 'not_observed'}


def main():
    directory = controller.TRAINING / 'remaining-learning'
    directory.mkdir(exist_ok=True)
    status_path = directory / 'status.json'

    def state(**fields):
        value = controller.read(status_path) if status_path.exists() else {}
        value.update(fields, updated_at=time.time(), pid=os.getpid())
        controller.save(status_path, value)

    controller.state = state
    source = controller.TRAINING / 'captures/train-01/report.json'
    report = deepcopy(controller.read(source))
    selected, skipped = [], []
    for row in report.get('trials', []):
        output = controller.output(row)
        if (row.get('execution_status') != 'completed'
                or row.get('verdict') not in ('passed', 'failed')
                or output.get('appworld_reward') not in (0, 1)
                or output.get('error_code')):
            continue
        evidence = row.get('evidence') or {}
        traces = evidence.get('traces') or [evidence.get('trace')]
        identities = [trace['identity'] for trace in traces]
        paths = [(controller.TRAINING / 'evolution/by-user' / controller.OWNER / 'evaluation'
                  / identity['conversation_id'] / (identity['run_id'] + '.evaluation.json'))
                 for identity in identities]
        # Existing failed analysis is kept as failed, and successful analysis
        # is never repeated. This import contains only genuinely fresh runs.
        if any(path.exists() for path in paths):
            skipped.append({'task_id': row['task_id'], 'existing_analysis': [str(p) for p in paths]})
            continue
        selected.append(row)
    report['trials'] = selected
    if not selected:
        state(phase='no_unprocessed_scored_training_runs', skipped=skipped)
        return
    prompts = install(controller)
    state(phase='starting', source_report=str(source), tasks=[r['task_id'] for r in selected],
          skipped=skipped, analysis_prompt_update=prompts, agent_reruns=0)
    result = controller.learn(report, source, controller.environment())
    synthesis = synthesis_summary(report)
    phase = ('learning_complete_degraded' if synthesis['synthesis_status'] == 'partial_synthesis_failure'
             else 'learning_complete' if result.get('usable') else 'learning_unusable')
    state(phase=phase, learning=result, synthesis=synthesis)
    print(json.dumps({'new_training_tasks': len(selected), 'skipped_existing': len(skipped),
                      'usable': result.get('usable'), 'agent_reruns': 0,
                      'status': str(status_path)}, ensure_ascii=False))


if __name__ == '__main__':
    main()
