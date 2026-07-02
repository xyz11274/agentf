import importlib.util
from pathlib import Path


ROOT = Path(__file__).parents[1]


def load(name: str, path: Path):
    spec = importlib.util.spec_from_file_location(name, path)
    module = importlib.util.module_from_spec(spec)
    assert spec.loader is not None
    spec.loader.exec_module(module)
    return module


coordinator = load("outfit_coordinator", ROOT / "nodes/coordinator_decide.py")
executor = load("outfit_executor_artifacts", ROOT / "nodes/skill_executor.py")


class FakeDataBus:
    def __init__(self):
        self.global_writes = []
        self.writes = []

    def put_global(self, key, value):
        self.global_writes.append((key, value))

    def put(self, key, value):
        self.writes.append((key, value))


def test_dispatch_keys_are_unique_and_dialogue_scoped():
    bus = FakeDataBus()
    args = {"skills": [{"name": "outfit-stylist", "prompt": "p"}]}

    coordinator.handle_run_skills_call(args, bus, "s1", "dialogue-a")
    coordinator.handle_run_skills_call(args, bus, "s1", "dialogue-a")

    keys = [key for key, _ in bus.global_writes]
    assert len(set(keys)) == 2
    assert all(key.startswith("dispatch:dialogue-a:") for key in keys)


def test_image_paths_are_collected_from_tool_results(tmp_path):
    image = tmp_path / "look.png"
    image.write_bytes(b"image")
    text = (
        f'{{"stdout":"{image}\\n",'
        '"other":"https://cdn.example/look.jpg",'
        '"missing":"/missing/look.png"}'
    )

    assert executor.extract_artifact_paths(text) == [
        str(image),
        "https://cdn.example/look.jpg",
    ]


def test_local_artifact_becomes_bridge_markdown_url():
    rendered = coordinator.append_artifacts_markdown(
        "完成",
        '["/tmp/output/look.png"]',
    )

    assert "## 生成图片" in rendered
    assert "/api/artifacts/outfit-assistant?path=%2Ftmp%2Foutput%2Flook.png" in rendered


def test_existing_absolute_markdown_image_is_rewritten(tmp_path):
    image = tmp_path / "look.png"
    image.write_bytes(b"image")

    rendered = coordinator.append_artifacts_markdown(
        f"![look]({image})",
        "[]",
    )

    assert str(image) not in rendered
    assert "/api/artifacts/outfit-assistant?path=" in rendered


def test_package_artifact_url_preserves_directory_hierarchy():
    path = ROOT / "output" / "report" / "index.html"

    url = coordinator.local_artifact_url(str(path))

    assert url == (
        "/api/artifacts/outfit-assistant/files/"
        "outfit_assistant/output/report/index.html"
    )
