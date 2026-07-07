import importlib.util
import json
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


def test_failed_skill_context_contains_original_error_for_model():
    message = coordinator.build_skill_completion_message(
        "outfit-stylist",
        "false",
        '{"status":"failed","error":"http request timeout"}',
        [],
    )

    assert "success=false" in message
    assert "http request timeout" in message
    assert "直接回复用户" in message
    assert "重新委派" in message


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


def test_relative_artifact_path_is_resolved_against_package_root(tmp_path):
    """Relative paths like .agentfoundry/.../file.png are resolved against AGENT_PACKAGE_DIR."""
    # Create a file under AGENT_PACKAGE_DIR (simulating LLM saving to .agentfoundry/)
    sub = ROOT / ".agentfoundry" / "assets"
    sub.mkdir(parents=True, exist_ok=True)
    image = sub / "hiking_outfit.png"
    image.write_bytes(b"image")
    text = (
        '{"stdout": "效果图已生成\\n",'
        '"file_path": ".agentfoundry/assets/hiking_outfit.png"}'
    )

    result = executor.extract_artifact_paths(text)

    assert len(result) == 1
    assert result[0] == str(image.resolve())



def test_relative_artifact_path_with_dot_slash(tmp_path):
    """Relative paths like ./assets/outfit.png are resolved against AGENT_PACKAGE_DIR."""
    sub = ROOT / "assets"
    sub.mkdir(parents=True, exist_ok=True)
    image = sub / "outfit.png"
    image.write_bytes(b"image")
    text = f'{{"stdout": "./assets/outfit.png\\n"}}'

    result = executor.extract_artifact_paths(text)

    assert len(result) == 1
    assert result[0] == str(image.resolve())


def test_package_artifact_keeps_its_original_directory_tree(tmp_path, monkeypatch):
    package = tmp_path / "custom-agent"
    image = package / "reports" / "lookbook" / "assets" / "look.png"
    image.parent.mkdir(parents=True)
    image.write_bytes(b"image")
    monkeypatch.setattr(executor, "AGENT_PACKAGE_DIR", str(package))

    result = executor._ensure_artifacts_in_package([str(image)])

    assert result == [str(image.resolve())]
    assert not (package / "outputs" / "look.png").exists()


def test_relative_markdown_image_is_rewritten_without_modifying_skill(tmp_path, monkeypatch):
    package = tmp_path / "custom-agent"
    image = package / "assets" / "look.png"
    image.parent.mkdir(parents=True)
    image.write_bytes(b"image")
    monkeypatch.setattr(coordinator, "AGENT_PACKAGE_DIR", str(package))

    rendered = coordinator.append_artifacts_markdown(
        "搭配完成：![look](assets/look.png)",
        json.dumps([str(image)]),
    )

    expected = "/api/artifacts/outfit-assistant/files/custom-agent/assets/look.png"
    assert expected in rendered
    assert "(assets/look.png)" not in rendered
    assert rendered.count(expected) == 1


def test_relative_html_image_is_rewritten_without_modifying_skill(tmp_path, monkeypatch):
    package = tmp_path / "custom-agent"
    image = package / "assets" / "look.png"
    image.parent.mkdir(parents=True)
    image.write_bytes(b"image")
    monkeypatch.setattr(coordinator, "AGENT_PACKAGE_DIR", str(package))

    rendered = coordinator.append_artifacts_markdown(
        '<section><img src="assets/look.png" alt="look"></section>',
        json.dumps([str(image)]),
    )

    expected = "/api/artifacts/outfit-assistant/files/custom-agent/assets/look.png"
    assert f'src="{expected}"' in rendered
    assert rendered.count(expected) == 1


def test_local_artifact_becomes_bridge_markdown_url():
    rendered = coordinator.append_artifacts_markdown(
        "完成",
        '["/tmp/output/look.png"]',
    )

    assert "## 生成产物" in rendered
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
        "outfit-assistant/output/report/index.html"
    )


def test_llm_failure_does_not_claim_an_artifact_without_evidence():
    response = coordinator.llm_error_response("")

    assert "任务未能完成" in response
    assert "产物仍可查看" not in response
    assert "图片仍可查看" not in response


def test_llm_failure_preserves_artifacts_that_were_actually_reported():
    response = coordinator.llm_error_response('["/tmp/output/look.png"]')

    assert "产物仍可查看" in response
    assert "/api/artifacts/outfit-assistant?path=" in response


# ═══════════════════════════════════════════════════════════════════
# Non-image artifact extraction (PPT, DOCX, PDF, XLSX)
# ═══════════════════════════════════════════════════════════════════

def test_pptx_path_is_extracted(tmp_path):
    """Extract artifact paths should discover .pptx files from tool output."""
    pptx = tmp_path / "presentation.pptx"
    pptx.write_bytes(b"pptx content")
    text = f'{{"result": "{pptx}"}}'

    result = executor.extract_artifact_paths(text)

    assert str(pptx) in result


def test_docx_path_is_extracted(tmp_path):
    """.docx files should be extracted from tool output."""
    docx = tmp_path / "report.docx"
    docx.write_bytes(b"docx content")
    text = f'{{"result": "{docx}"}}'

    result = executor.extract_artifact_paths(text)

    assert str(docx) in result


def test_pdf_path_is_extracted(tmp_path):
    """.pdf files should be extracted from tool output."""
    pdf = tmp_path / "document.pdf"
    pdf.write_bytes(b"pdf content")
    text = f'{{"result": "{pdf}"}}'

    result = executor.extract_artifact_paths(text)

    assert str(pdf) in result


def test_xlsx_path_is_extracted(tmp_path):
    """.xlsx files should be extracted from tool output."""
    xlsx = tmp_path / "data.xlsx"
    xlsx.write_bytes(b"xlsx content")
    text = f'{{"result": "{xlsx}"}}'

    result = executor.extract_artifact_paths(text)

    assert str(xlsx) in result


# ═══════════════════════════════════════════════════════════════════
# Non-image artifact UI rendering
# ═══════════════════════════════════════════════════════════════════

def test_pptx_artifact_adds_download_link():
    """append_artifacts_markdown should produce a download link for .pptx."""
    rendered = coordinator.append_artifacts_markdown(
        "PPT 生成完成",
        '["/tmp/output/presentation.pptx"]',
    )

    assert "## 生成产物" in rendered
    assert "📥" in rendered or "下载" in rendered
    assert "presentation.pptx" in rendered
    assert "/api/artifacts/outfit-assistant?path=%2Ftmp%2Foutput%2Fpresentation.pptx" in rendered
    # .pptx 不应被误渲染为图片（不应有 ![]  markdown 图片语法）
    assert "![" not in rendered


def test_mixed_image_and_download_artifacts(tmp_path):
    """Mixed image + non-image artifacts both appear in output."""
    image = tmp_path / "look.png"
    image.write_bytes(b"image")
    pptx = tmp_path / "deck.pptx"
    pptx.write_bytes(b"pptx")

    rendered = coordinator.append_artifacts_markdown(
        "完成",
        json.dumps([str(image), str(pptx)]),
    )

    assert "## 生成产物" in rendered
    assert "![生成图片" in rendered
    assert "📥" in rendered or "deck.pptx" in rendered
