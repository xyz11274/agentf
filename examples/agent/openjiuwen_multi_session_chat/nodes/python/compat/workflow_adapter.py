#!/usr/bin/env python3
import asyncio
import json
import uuid
from dataclasses import dataclass
from typing import Dict, Optional

from .runtime_bridge import (
    DataBusBridge,
    build_request_id,
    current_runtime_binding,
    llm_env,
    load_openjiuwen_modules,
)


@dataclass
class ReasonerResult:
    text: str
    source: str
    error: str = ""


class OpenJiuwenWorkflowAdapter:
    def __init__(self, workflow_id: str, description: str, databus: Optional[DataBusBridge] = None):
        self.workflow_id = workflow_id
        self.description = description
        self.databus = databus or DataBusBridge()
        self.modules = load_openjiuwen_modules()
        self.binding = current_runtime_binding()

    def build_workflow_card(self, input_params: Dict) -> Dict:
        if self.modules.available:
            workflow = self.modules.workflow
            try:
                card = workflow.WorkflowCard(
                    id=f"{self.workflow_id}_bridge",
                    name=self.workflow_id,
                    version="1.0",
                    description=self.description,
                    input_params=input_params,
                )
                return {"provider": "openjiuwen", "card": card}
            except Exception as exc:
                return {"provider": "fallback", "error": f"openjiuwen WorkflowCard failed: {exc}"}
        return {"provider": "fallback", "error": self.modules.reason}

    def query_reasoner(self, prompt: str, system_prompt: str, response_schema: Dict, timeout_ms: int = 2500) -> ReasonerResult:
        card_info = self.build_workflow_card({"type": "object", "properties": {"query": {"type": "string"}}, "required": ["query"]})
        self.databus.bind_runtime_state(
            "reasoner_request",
            {
                "adapter_workflow_id": self.workflow_id,
                "description": self.description,
                "provider_available": self.modules.available,
                "provider_reason": self.modules.reason,
                "card_provider": card_info.get("provider", "fallback"),
                "prompt": prompt,
                "system_prompt": system_prompt,
                "response_schema": response_schema,
            },
            self.binding,
        )
        self.databus.publish_lifecycle(
            "reasoner_started",
            {
                "adapter_workflow_id": self.workflow_id,
                "provider_available": self.modules.available,
            },
            self.binding,
        )
        if self.modules.available:
            openjiuwen_result = self._query_with_openjiuwen(prompt, system_prompt, response_schema)
            if openjiuwen_result.text:
                self._record_result("openjiuwen", openjiuwen_result)
                return openjiuwen_result
        if self.databus.available():
            request_id = build_request_id(self.workflow_id)
            self.databus.bind_runtime_state(
                "reasoner_databus_request",
                {
                    "request_id": request_id,
                    "prompt": prompt,
                    "system_prompt": system_prompt,
                    "response_schema": response_schema,
                },
                self.binding,
            )
            self.databus.request_llm(request_id, prompt, channel=f"agentflow.llm.chunk.{request_id}")
            raw = self.databus.next_event(f"agentflow.llm.done.{request_id}", timeout_ms=timeout_ms)
            text = self._extract_text(raw)
            if text:
                error = "" if self.modules.available else self.modules.reason
                result = ReasonerResult(text=text, source="agentflow_databus", error=error)
                self._record_result(request_id, result)
                return result
        result = ReasonerResult(text="", source="fallback", error=self.modules.reason)
        self._record_result("fallback", result)
        return result

    def _record_result(self, request_id: str, result: ReasonerResult) -> None:
        self.databus.store_reasoner_result(
            request_id,
            {
                "adapter_workflow_id": self.workflow_id,
                "source": result.source,
                "text": result.text,
                "error": result.error,
            },
            self.binding,
        )
        stage = "reasoner_completed" if result.text else "reasoner_fallback"
        self.databus.publish_lifecycle(
            stage,
            {
                "adapter_workflow_id": self.workflow_id,
                "source": result.source,
                "text_length": len(result.text),
                "error": result.error,
            },
            self.binding,
        )

    def _extract_text(self, raw: str) -> str:
        blob = (raw or "").strip()
        if not blob:
            return ""
        try:
            payload = json.loads(blob)
        except json.JSONDecodeError:
            return blob
        if isinstance(payload, dict):
            for key in ("text", "output", "response", "result"):
                value = payload.get(key)
                if isinstance(value, str) and value.strip():
                    return value.strip()
        return blob

    def _query_with_openjiuwen(self, prompt: str, system_prompt: str, response_schema: Dict) -> ReasonerResult:
        env = llm_env()
        if not env["api_key"] or not env["model"]:
            return ReasonerResult(text="", source="openjiuwen", error="missing AF_LLM_API_KEY or AF_LLM_MODEL for openjiuwen execution")
        try:
            return asyncio.run(self._run_openjiuwen(prompt, system_prompt, response_schema, env))
        except Exception as exc:
            return ReasonerResult(text="", source="openjiuwen", error=f"openjiuwen execution failed: {exc}")

    async def _run_openjiuwen(self, prompt: str, system_prompt: str, response_schema: Dict, env: Dict[str, str]) -> ReasonerResult:
        workflow = self.modules.workflow
        llm = self.modules.llm
        runner = self.modules.runner
        single_agent = self.modules.single_agent
        application = self.modules.application

        model_client_config = llm.ModelClientConfig(
            client_provider=env["provider"] or "openai",
            api_key=env["api_key"],
            api_base=env["api_base"] or None,
            verify_ssl=env["ssl_verify"].lower() == "true",
        )
        model_config = llm.ModelRequestConfig(model=env["model"])

        workflow_key = f"{self.workflow_id}_{uuid.uuid4().hex[:8]}"
        self.databus.bind_runtime_state(
            "openjiuwen_workflow_binding",
            {
                "adapter_workflow_id": self.workflow_id,
                "openjiuwen_workflow_key": workflow_key,
                "agentflow_workflow_id": self.binding.workflow_id,
                "agentflow_workflow_instance_id": self.binding.workflow_instance_id,
                "agentflow_worker_id": self.binding.worker_id,
            },
            self.binding,
        )
        workflow_card = workflow.WorkflowCard(
            id=workflow_key,
            name=workflow_key,
            version="1.0",
            description=self.description,
            input_params={"type": "object", "properties": {"query": {"type": "string"}}, "required": ["query"]},
        )
        flow = workflow.Workflow(card=workflow_card)
        start = workflow.Start()
        end = workflow.End({"responseTemplate": "{{output}}"})
        component = workflow.LLMComponent(
            workflow.LLMCompConfig(
                model_client_config=model_client_config,
                model_config=model_config,
                template_content=[
                    {"role": "system", "content": system_prompt},
                    {"role": "user", "content": "{{query}}"},
                ],
                response_format={"type": "json"},
                output_config=response_schema,
            )
        )
        flow.set_start_comp("start", start, inputs_schema={"query": "${query}"})
        flow.add_workflow_comp("llm", component, inputs_schema={"query": "${start.query}"})
        flow.set_end_comp("end", end, inputs_schema={"output": "${llm.output}"})
        flow.add_connection("start", "llm")
        flow.add_connection("llm", "end")
        runner.Runner.resource_mgr.add_workflow(
            workflow.WorkflowCard(id=workflow.generate_workflow_key(flow.card.id, flow.card.version)),
            lambda: flow,
        )
        workflow_agent = application.WorkflowAgent(
            single_agent.WorkflowAgentConfig(id=f"{workflow_key}_agent", version="0.1.0", description=self.description)
        )
        workflow_agent.add_workflows([flow])
        invoke_result = await runner.Runner.run_agent(workflow_agent, {"query": prompt})
        self.databus.bind_runtime_state(
            "openjiuwen_runner_state",
            {
                "adapter_workflow_id": self.workflow_id,
                "openjiuwen_workflow_key": workflow_key,
                "invoke_result_keys": list(invoke_result.keys()) if hasattr(invoke_result, "keys") else [],
            },
            self.binding,
        )
        output = invoke_result.get("output")
        if hasattr(output, "result"):
            result = output.result
            if isinstance(result, dict):
                for key in ("output", "response", "text"):
                    value = result.get(key)
                    if isinstance(value, str) and value.strip():
                        return ReasonerResult(text=value.strip(), source="openjiuwen")
                return ReasonerResult(text=json.dumps(result, ensure_ascii=False), source="openjiuwen")
        return ReasonerResult(text="", source="openjiuwen", error="openjiuwen returned no output field")
