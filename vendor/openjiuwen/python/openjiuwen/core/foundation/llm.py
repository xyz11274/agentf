from dataclasses import dataclass
from typing import Optional


@dataclass
class ModelClientConfig:
    client_provider: Optional[str] = None
    api_key: Optional[str] = None
    api_base: Optional[str] = None
    verify_ssl: bool = False


@dataclass
class ModelRequestConfig:
    model: str = ''
