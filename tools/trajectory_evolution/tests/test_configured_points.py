"""Configuration-owned points: disabled baseline and pre-transport privacy."""
import json
import os
from pathlib import Path
import sys
import tempfile
import unittest
from unittest.mock import patch

sys.path.insert(0, str(Path(__file__).resolve().parents[1] / "clients/python"))
from evolution_configured import ConfiguredPoints, _LocalPoints, _private_payload


class ConfiguredPointsTests(unittest.TestCase):
    def test_disabled_keeps_original_call_and_identity(self):
        with patch.dict(os.environ, {"EVOLUTION_POINT_CONFIG": ""}):
            client = ConfiguredPoints()
            payload, calls = {"data": [1, 2]}, []
            self.assertIs(client.call({"input": payload}, lambda value: calls.append(value) or value), payload)
            self.assertEqual(calls, [payload])
            self.assertTrue(client.allow_loop({"state": "unchanged"}))

    def test_sensitive_content_never_reaches_transport(self):
        wire = []
        def transport(operation, fields, *args):
            wire.append(json.dumps(fields))
            return {"input": fields.get("input"), "decision": "allow"}
        dummy_secret = "unfamiliar-provider-fixture-938174"
        with patch.dict(os.environ, {"VENDOR_SECRET": dummy_secret}), patch.object(_LocalPoints, "_invoke", side_effect=transport):
            client = _LocalPoints("http://127.0.0.1:8766", "fixture-auth-token-not-a-real-key")
            for i, payload in enumerate((
                {"api_key": "fixture-value"}, {"text": dummy_secret},
                {"text": "ark-" + "z" * 32}, {"text": "Bearer " + "b" * 32},
                {"text": "https://example.invalid/?token=fixture-private-value"},
            )):
                fields = {"ref": {"run": "safe-job", "key": f"private-{i}"}, "kind": "action", "input": payload}
                calls = []
                result = client.call(fields, lambda actual: calls.append(actual) or payload)
                self.assertIs(result, payload)
                self.assertEqual(calls, [payload])
            serialized = "\n".join(wire)
            self.assertNotIn(dummy_secret, serialized)
            self.assertNotIn("api_key", serialized)
            self.assertNotIn("fixture-private-value", serialized)
            self.assertNotIn("ark-", serialized)
            self.assertTrue(all(json.loads(item)["phase"] in ("gap", "abort") for item in wire))

    def test_configured_identity_and_string_tool_contract(self):
        with tempfile.TemporaryDirectory(dir=sys.argv[1] if len(sys.argv) > 1 else None) as directory:
            config = Path(directory) / "client.json"
            config.write_text(json.dumps({"url": "http://127.0.0.1:8766", "principals": [
                {"user_id": "user-a", "token_env": "FIXTURE_CAPTURE_TOKEN"}]}))
            with patch.dict(os.environ, {"EVOLUTION_POINT_CONFIG": str(config), "FIXTURE_CAPTURE_TOKEN": "fixture-auth-token-not-a-real-key"}):
                client = ConfiguredPoints()
                calls, points = [], []
                def point(fields, **options):
                    points.append(fields)
                    return {"input": fields.get("input"), "decision": "reject", "reason": "configured_rule"}
                with patch.object(_LocalPoints, "point", side_effect=point):
                    result = client.call({"ref": {"run": "run-1"}, "kind": "action", "input": {}},
                                         lambda value: calls.append(value), user_id="user-a", result_format="json_text")
                self.assertEqual(json.loads(result)["error"], "configured_rule")
                self.assertEqual(calls, [])
                self.assertEqual(points[0]["ref"], points[1]["ref"])
                self.assertFalse(points[1]["executed"])
                self.assertIsNone(client._client("other-user"))

    def test_explicit_public_identifier_does_not_disable_credential_checks(self):
        with patch.dict(os.environ, {"TASK_KEY": "dispatch:job:1", "VENDOR_API_KEY": "private-vendor-fixture-value"}):
            self.assertTrue(_private_payload({"ref": {"key": "dispatch:job:1"}}))
            self.assertFalse(_private_payload({"ref": {"key": "dispatch:job:1"}}, ["TASK_KEY"]))
            self.assertTrue(_private_payload({"text": "private-vendor-fixture-value"}, ["TASK_KEY"]))
            self.assertTrue(_private_payload({"api_key": "test"}, ["TASK_KEY"]))
            with patch.dict(os.environ, {"EVOLUTION_SECRET_ENV_NAMES": "TASK_KEY"}):
                self.assertTrue(_private_payload({"text": "dispatch:job:1"}, ["TASK_KEY"]))

    def test_http_trial_loads_its_own_identity_without_production_credentials(self):
        with tempfile.TemporaryDirectory(dir=sys.argv[1] if len(sys.argv) > 1 else None) as directory:
            path = Path(directory) / "trial.json"
            path.write_text(json.dumps({"principals": [{"user_id": "trial-user", "token_env": "TRAJECTORY_EVOLUTION_TRIAL_TOKEN"}]}))
            with patch.dict(os.environ, {"EVOLUTION_POINT_CONFIG": "", "TRAJECTORY_EVOLUTION_TRIAL_CONFIG": str(path),
                                         "TRAJECTORY_EVOLUTION_TRIAL_URL": "http://127.0.0.1:8766",
                                         "TRAJECTORY_EVOLUTION_TRIAL_TOKEN": "fixture-trial-auth-token-00000001"}):
                client = ConfiguredPoints()._client("trial-user")
                self.assertIsInstance(client, _LocalPoints)
                self.assertEqual(client._endpoint.port, 8766)
                with patch.dict(os.environ, {"TRAJECTORY_EVOLUTION_TRIAL_URL": ""}):
                    self.assertIsNone(ConfiguredPoints()._client("trial-user"))


if __name__ == "__main__":
    unittest.main(argv=[sys.argv[0]])
