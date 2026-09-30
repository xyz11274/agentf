"""Reformat only history already present in an AppWorld model request.

The caller keeps its existing client, model, destination and policy. This pure
transform reads no files and performs no network or model calls. It removes the
existing JSON transcript from the system message and gives the exact same
contents their recorded assistant/user roles, without adding any source data.
"""
from __future__ import annotations

from copy import deepcopy
import json
import re

_HISTORY_SECTION = re.compile(
    r'\n*## Complete recent public history\s*```json\s*\n(.*?)\n```\s*',
    re.DOTALL,
)


def compose_skill_messages(messages, *, skill_name):
    if skill_name != 'appworld-office':
        return messages
    if (not isinstance(messages, list) or len(messages) != 2
            or [item.get('role') for item in messages] != ['system', 'user']
            or not all(isinstance(item.get('content'), str) for item in messages)):
        raise ValueError('appworld_skill_request_shape_invalid')
    system = messages[0]['content']
    sections = list(_HISTORY_SECTION.finditer(system))
    if len(sections) != 1:
        raise ValueError('appworld_existing_history_section_missing_or_duplicate')
    history = json.loads(sections[0].group(1))
    if not isinstance(history, list) or len(history) % 2:
        raise ValueError('appworld_existing_history_invalid')
    for index, item in enumerate(history):
        if (not isinstance(item, dict) or set(item) != {'role', 'content'}
                or item['role'] != ('assistant' if index % 2 == 0 else 'user')
                or not isinstance(item['content'], str)):
            raise ValueError('appworld_existing_history_roles_invalid')
    prepared = deepcopy(messages)
    section = sections[0]
    prepared[0]['content'] = system[:section.start()] + '\n\n' + system[section.end():]
    return [prepared[0], *history, prepared[1]]
