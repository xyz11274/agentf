"""Send actual prior messages, matching the Bridge contract, without eval labels."""
from __future__ import annotations
import json
from run_eval import send_turn as _send, wait_turn_response as _wait


def send_turn(client, agent_id, user_id, client_id, dialogue_id, content):
    identity = (agent_id,user_id,client_id,dialogue_id)
    conversations = getattr(client,'_business_conversations',None)
    if conversations is None:
        conversations = client._business_conversations = {}
    previous = conversations.get(identity, [])
    if not previous and getattr(client, '_business_environment', ''):
        # 仅传可信环境事实，绝不把预期意图、标准答案或评分规则交给 Agent。
        previous = [{'role': 'system', 'content': client._business_environment}]
    messages = [*previous,{'role':'user','content':content}]
    response = _send(client,agent_id,user_id,client_id,dialogue_id,
                     json.dumps({'stream':False,'messages':messages},ensure_ascii=False))
    conversations[identity] = messages
    client._business_pending_conversation = identity
    return response


def wait_turn_response(client, deadline):
    response = _wait(client,deadline)
    identity = getattr(client,'_business_pending_conversation',None)
    if identity is not None and response.get('final_text') and not response.get('error') and not response.get('timed_out'):
        client._business_conversations[identity].append({'role':'assistant','content':response['final_text']})
    client._business_pending_conversation = None
    return response
