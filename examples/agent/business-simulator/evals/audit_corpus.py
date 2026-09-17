#!/usr/bin/env python3
"""Read-only corpus coverage inventory. Coverage is not execution accuracy."""
from __future__ import annotations

import argparse
from collections import Counter
import hashlib
import json
import re
import sys
from pathlib import Path

PACKAGE = Path(__file__).resolve().parents[1]
REPO = PACKAGE.parents[2]
sys.path.insert(0, str(REPO / 'benchmark'))
sys.path.insert(0, str(PACKAGE / 'harness'))
from converters import read_xlsx_rows, parse_intents
from business_sim.contracts import SERVICES

OPERATIONS = {
    '100051': ['smart-home', 'control'], '100053': ['smart-home', 'habits'],
    '100101': ['movie-query', 'play'], '200002': ['movie-query', 'recommend'],
    '300001': ['device-gui', 'plan'], '100151': ['health-advice', 'reminder_create'],
    '100152': ['health-advice', 'record_save'], '100153': ['health-advice', 'record_query'],
    '100154': ['health-advice', 'appointment_create'], '100155': ['health-advice', 'triage'],
    '100156': ['health-advice', 'appointment_query'], '100157': ['health-advice', 'reminder_query'],
}


def audit() -> dict:
    sources, categories, operations, domains, preconditions = [], Counter(), Counter(), Counter(), []
    for path in sorted((REPO / 'test_cases/product').glob('*.xlsx')):
        rows = read_xlsx_rows(path)
        sources.append({'file': str(path.relative_to(REPO)), 'sha256': hashlib.sha256(path.read_bytes()).hexdigest(),
                        'rows': len(rows) - 1, 'columns': rows[0]})
        for row in rows[1:]:
            cells = dict(zip(rows[0], row))
            categories[cells.get('S1场景') or '未分类'] += 1
            operations.update(re.findall(r'\b\d{6}\b', cells.get('预期操作类型', '')))
            for labels in parse_intents(cells.get('预期意图', '')).values():
                domains.update(labels)
            text = cells.get('预置条件（可选）', '')
            if text not in ('', '无'):
                preconditions.append({'file': path.name, 'id': cells.get('用例编号'), 'precondition': text})
    dataset = json.loads((REPO / 'benchmark/datasets/product.json').read_text())
    turns = [t for c in dataset['cases'] for t in c['turns']]
    skill_names = sorted({s for t in turns for s in (t.get('expected_skills') or [])})
    return {'notice': 'Static capability inventory only; not a pass rate or a claim of original business equivalence.',
            'sources': sources, 'cases': len(dataset['cases']), 'turns': len(turns),
            'categories': dict(categories), 'annotated_domains': dict(domains),
            'operation_occurrences': dict(sorted(operations.items())), 'operation_map': OPERATIONS,
            'unimplemented_operation_codes': sorted(set(operations) - OPERATIONS.keys()),
            'expected_skills': skill_names, 'unregistered_skills': sorted(set(skill_names) - SERVICES.keys()),
            'turns_without_intent_labels': sum(t.get('expected_skills') is None for t in turns),
            'turns_without_expected_behavior': sum(not t.get('expected_behavior') for t in turns),
            'turns_without_either_label': sum(t.get('expected_skills') is None and not t.get('expected_behavior') for t in turns),
            'preconditions': preconditions,
            'limitations': ['Text benchmark does not verify original UDS messages or real devices.',
                           'Public weather/search are connected; live commerce and traffic require dedicated providers.',
                           'Original operation/intent labels may conflict; no labels are rewritten.',
                           'Agent names beyond corpus domains have inferred roles.',
                           'Image files and local scheduled notifications are implemented; phone push, video generation, physical devices and transactions remain disconnected.']}


def main():
    parser = argparse.ArgumentParser(description=__doc__)
    parser.add_argument('--output', type=Path)
    args = parser.parse_args()
    result = audit()
    content = json.dumps(result, ensure_ascii=False, indent=2) + '\n'
    if args.output:
        args.output.write_text(content, encoding='utf-8')
    else:
        print(content)
    return int(bool(result['unregistered_skills'] or result['unimplemented_operation_codes']))


if __name__ == '__main__':
    raise SystemExit(main())
