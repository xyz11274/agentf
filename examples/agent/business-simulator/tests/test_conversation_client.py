from pathlib import Path
import json
import sys
import time
from types import SimpleNamespace
from unittest.mock import patch

import pytest

PACKAGE=Path(__file__).resolve().parents[1]
sys.path.insert(0,str(PACKAGE.parents[2]/'benchmark'))
sys.path.insert(0,str(PACKAGE/'evals'))
import conversation_client as replay
import run_eval as benchmark


class _Socket:
    def settimeout(self, _timeout):
        pass


def _pushed_client(message):
    payload = json.dumps({"method": "agent.response", "params": message})
    return SimpleNamespace(sock=_Socket(), pushed=[], _recv_line=lambda: payload)


def test_error_response_is_terminal_without_final_flag():
    result = benchmark.wait_turn_response(
        _pushed_client({"content": "批次结果读取失败", "error": True}),
        time.time() + 60,
    )
    assert result == {
        "texts": ["批次结果读取失败"],
        "final_text": "批次结果读取失败",
        "error": "批次结果读取失败",
        "timed_out": False,
    }


def test_empty_final_response_is_terminal_and_remains_empty():
    result = benchmark.wait_turn_response(
        _pushed_client({"content": "", "final": True}),
        time.time() + 60,
    )
    assert result == {"texts": [], "final_text": "", "error": "", "timed_out": False}


def test_rpc_eof_is_a_connection_error_instead_of_an_empty_json_loop():
    client = benchmark.RpcClient('127.0.0.1', 1)
    client.sock = SimpleNamespace(recv=lambda _size: b'')
    with pytest.raises(ConnectionResetError, match='closed'):
        client._recv_line()


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


def test_snapshot_time_is_sent_once_without_evaluation_labels():
    client=SimpleNamespace(_business_environment='业务时间：2026-09-21T09:00:00+08:00')
    with patch.object(replay,'_send',return_value={}) as sent:
        replay.send_turn(client,'agent','user','chat','dialogue','查天气')
        replay.send_turn(client,'agent','user','chat','dialogue','继续')
    messages=json.loads(sent.call_args.args[-1])['messages']
    assert sum(m['role']=='system' for m in messages)==1
    assert messages[0]['content']==client._business_environment
    assert 'expected_' not in json.dumps(messages)
