from dataclasses import dataclass


@dataclass
class WorkflowAgentConfig:
    id: str
    version: str
    description: str = ''
