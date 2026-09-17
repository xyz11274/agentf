from pathlib import Path
import json
import sys
from types import SimpleNamespace
from unittest.mock import patch

PACKAGE=Path(__file__).resolve().parents[1]
sys.path.insert(0,str(PACKAGE.parents[2]/'benchmark'))
sys.path.insert(0,str(PACKAGE/'evals'))
import conversation_client as replay


def test_replay_uses_actual_responses_and_isolates_dialogues():
    client=SimpleNamespace()
    with patch.object(replay,'_send',return_value={}) as sent, patch.object(replay,'_wait',return_value={'final_text':'川西实际湿度86%','error':'','timed_out':False}):
        replay.send_turn(client,'agent','user','chat','dialogue','查川西湿度')
        replay.wait_turn_response(client,999)
        replay.send_turn(client,'agent','user','chat','dialogue','这种湿度适合徒步吗？')
        payload=json.loads(sent.call_args.args[-1])
        assert payload['messages']==[{'role':'user','content':'查川西湿度'},
                                     {'role':'assistant','content':'川西实际湿度86%'},
                                     {'role':'user','content':'这种湿度适合徒步吗？'}]
        assert 'expected_behavior' not in json.dumps(payload)
        replay.send_turn(client,'agent','other','chat','dialogue','你好')
        assert json.loads(sent.call_args.args[-1])['messages']==[{'role':'user','content':'你好'}]


def test_failed_response_is_not_replayed_as_success():
    client=SimpleNamespace()
    with patch.object(replay,'_send',return_value={}), patch.object(replay,'_wait',return_value={'final_text':'partial','error':'failed','timed_out':False}):
        replay.send_turn(client,'agent','user','chat','dialogue','请求')
        replay.wait_turn_response(client,999)
    assert len(client._business_conversations[('agent','user','chat','dialogue')])==1
