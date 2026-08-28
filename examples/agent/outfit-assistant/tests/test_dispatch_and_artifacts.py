import importlib.util
import json
import os
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


# pip package modules — entry file (nodes/*.py) is a thin re-export via
# `globals().update(_m.__dict__)`, but functions like extract_structured_artifacts
# / extract_artifact_paths / _ensure_artifacts_in_package / append_artifacts_markdown
# are CLOSURES bound to the pip package module's globals. So setting
# `executor.AGENT_PACKAGE_DIR` does NOT propagate — we must patch the pip package
# module's AGENT_PACKAGE_DIR for those functions to see the override (F5 single source).
from agentflow_runtime import skill_executor as _pkg_skill_executor
# Artifact URL/render helpers are owned by coordinator_common after the
# v1/v2 convergence; keep the historical local alias so existing fixtures
# patch the closure's real module-level package path.
from agentflow_runtime import coordinator_common as _pkg_coordinator_decide


def _patch_package_dirs(monkeypatch, executor_dir: str, coordinator_dir: str):
    """Patch BOTH entry file and pip package module AGENT_PACKAGE_DIR.

    Use this in tests that call extract_structured_artifacts / extract_artifact_paths
    / _ensure_artifacts_in_package / append_artifacts_markdown so the closures see
    the test's tmp_path override.
    """
    monkeypatch.setattr(executor, "AGENT_PACKAGE_DIR", executor_dir)
    monkeypatch.setattr(_pkg_skill_executor, "AGENT_PACKAGE_DIR", executor_dir)
    monkeypatch.setattr(coordinator, "AGENT_PACKAGE_DIR", coordinator_dir)
    monkeypatch.setattr(_pkg_coordinator_decide, "AGENT_PACKAGE_DIR", coordinator_dir)


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
    monkeypatch.setattr(executor, "AGENT_PACKAGE_DIR", str(package)); monkeypatch.setattr(_pkg_skill_executor, "AGENT_PACKAGE_DIR", str(package))

    result = executor._ensure_artifacts_in_package([str(image)])

    assert result == [str(image.resolve())]
    assert not (package / "outputs" / "look.png").exists()


def test_relative_markdown_image_is_rewritten_without_modifying_skill(tmp_path, monkeypatch):
    package = tmp_path / "custom-agent"
    image = package / "assets" / "look.png"
    image.parent.mkdir(parents=True)
    image.write_bytes(b"image")
    monkeypatch.setattr(coordinator, "AGENT_PACKAGE_DIR", str(package)); monkeypatch.setattr(_pkg_coordinator_decide, "AGENT_PACKAGE_DIR", str(package))

    rendered = coordinator.append_artifacts_markdown(
        "搭配完成：![look](assets/look.png)",
        json.dumps([str(image)]),
    )

    expected = "/api/artifacts/custom-agent/files/assets/look.png"
    assert expected in rendered
    assert "(assets/look.png)" not in rendered
    assert rendered.count(expected) == 1


def test_relative_html_image_is_rewritten_without_modifying_skill(tmp_path, monkeypatch):
    package = tmp_path / "custom-agent"
    image = package / "assets" / "look.png"
    image.parent.mkdir(parents=True)
    image.write_bytes(b"image")
    monkeypatch.setattr(coordinator, "AGENT_PACKAGE_DIR", str(package)); monkeypatch.setattr(_pkg_coordinator_decide, "AGENT_PACKAGE_DIR", str(package))

    rendered = coordinator.append_artifacts_markdown(
        '<section><img src="assets/look.png" alt="look"></section>',
        json.dumps([str(image)]),
    )

    expected = "/api/artifacts/custom-agent/files/assets/look.png"
    assert f'src="{expected}"' in rendered
    assert rendered.count(expected) == 1


def test_local_artifact_becomes_bridge_markdown_url(tmp_path):
    image = tmp_path / "look.png"
    image.write_bytes(b"image")

    rendered = coordinator.append_artifacts_markdown(
        "完成",
        f'["{image}"]',
    )

    assert "## 生成产物" in rendered
    # URL 走 artifact_id 格式（{agent}/{session}/{path} 或 legacy {agent}/files/{path}），无 ?path= 查询参数
    # 设计文档 docs/design/features/sandbox/artifact-storage.md §I-artifact-id
    assert "/api/artifacts/" in rendered
    assert "?path=" not in rendered
    assert "look.png" in rendered


def test_existing_absolute_markdown_image_is_rewritten(tmp_path):
    image = tmp_path / "look.png"
    image.write_bytes(b"image")

    rendered = coordinator.append_artifacts_markdown(
        f"![look]({image})",
        "[]",
    )

    assert str(image) not in rendered
    # URL 走 artifact_id 格式，无 ?path= 查询参数（artifact-storage.md §I-artifact-id）
    assert "/api/artifacts/" in rendered
    assert "?path=" not in rendered


def test_package_artifact_url_preserves_directory_hierarchy():
    path = ROOT / "output" / "report" / "index.html"

    url = _pkg_coordinator_decide.local_artifact_url(
        str(path), package_dir=str(ROOT)
    )

    # agent name = basename(AGENT_PACKAGE_DIR), dynamic not hardcoded (Fix 7)
    # URL 格式：/api/artifacts/{agent_name}/files/{relative}，relative 相对 package_dir 计算
    # 不再含 agent_name 前缀（避免 URL 中 agent_name 重复，见 artifact-storage.md §I-artifact-id）
    agent_name = os.path.basename(coordinator.AGENT_PACKAGE_DIR.rstrip(os.sep))
    assert url == (
        f"/api/artifacts/{agent_name}/files/"
        f"output/report/index.html"
    ), f"URL格式应为 {agent_name}/files/output/report/index.html，实际 {url}"


def test_llm_failure_does_not_claim_an_artifact_without_evidence():
    response = coordinator.llm_error_response("")

    assert "任务未能完成" in response
    assert "产物仍可查看" not in response
    assert "图片仍可查看" not in response


def test_llm_failure_preserves_artifacts_that_were_actually_reported(tmp_path):
    image = tmp_path / "look.png"
    image.write_bytes(b"image")

    response = coordinator.llm_error_response(f'["{image}"]')

    assert "产物仍可查看" in response
    # agent name = basename(AGENT_PACKAGE_DIR), dynamic not hardcoded (Fix 7)
    # URL 走 artifact_id 格式，无 ?path= 查询参数（artifact-storage.md §I-artifact-id）
    assert "/api/artifacts/" in response
    assert "?path=" not in response


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
# 媒体产物提取(文本通道,failure_mode_matrix §五 B-video)——与 _artifact_suffixes
# 双通道对齐;曾缺失 → mp4 文本通道识别不了
# ═══════════════════════════════════════════════════════════════════

def test_media_paths_are_extracted_from_text(tmp_path):
    """文本通道应提取视频/音频产物路径(mp4/mov/webm/mp3/wav),与
    _artifact_suffixes 双通道对齐(曾缺失 → mp4 识别不了)。"""
    mp4 = tmp_path / "demo.mp4"
    mp4.write_bytes(b"mp4")
    mov = tmp_path / "clip.mov"
    mov.write_bytes(b"mov")
    webm = tmp_path / "anim.webm"
    webm.write_bytes(b"webm")
    mp3 = tmp_path / "audio.mp3"
    mp3.write_bytes(b"mp3")
    wav = tmp_path / "voice.wav"
    wav.write_bytes(b"wav")

    text = (
        f'{{"stdout": "视频: {mp4}\\n短片: {mov}\\n动画: {webm}\\n'
        f'音频: {mp3}\\n语音: {wav}\\n"}}'
    )
    result = executor.extract_artifact_paths(text)

    for p in (mp4, mov, webm, mp3, wav):
        assert str(p) in result, (
            f"文本通道应提取媒体产物 {p.name}, got: {result}"
        )


def test_missing_media_file_not_admitted(tmp_path):
    """不存在的媒体路径不应被收进产物(与图片 same semantics)。"""
    missing = tmp_path / "ghost.mp4"
    text = f'{{"stdout": "视频: {missing}"}}'

    result = executor.extract_artifact_paths(text)

    assert str(missing) not in result


# ═══════════════════════════════════════════════════════════════════
# HTML 报告产物提取(文本通道,failure_mode_matrix §五 B-html)——
# 与 mp4 同类修复:文本通道曾无法识别 html/htm → coordinator 收不到 html_reports
# ═══════════════════════════════════════════════════════════════════

def test_html_report_paths_are_extracted_from_text(tmp_path):
    """文本通道应提取 HTML 报告产物路径(.html/.htm),与 _artifact_suffixes
    双通道对齐(曾缺失 → html 报告走不到 html_reports 分类,只能落 downloads)。"""
    html = tmp_path / "report.html"
    html.write_bytes(b"<html></html>")
    htm = tmp_path / "summary.htm"
    htm.write_bytes(b"<html></html>")

    text = f'{{"stdout": "报告: {html}\\n摘要: {htm}\\n"}}'
    result = executor.extract_artifact_paths(text)

    assert str(html) in result, (
        f"文本通道应提取 .html 报告, got: {result}"
    )
    assert str(htm) in result, (
        f"文本通道应提取 .htm 报告, got: {result}"
    )


def test_html_report_is_appended_as_report_block(tmp_path, monkeypatch):
    """文本通道提取的 .html 产物在 append 阶段应归入 html_reports →
    追加[查看报告]块(而非 downloads 下载块)。"""
    package = tmp_path / "custom-agent"
    report = package / "outputs" / "report.html"
    report.parent.mkdir(parents=True)
    report.write_bytes(b"<html><body>report</body></html>")
    monkeypatch.setattr(coordinator, "AGENT_PACKAGE_DIR", str(package))
    monkeypatch.setattr(_pkg_coordinator_decide, "AGENT_PACKAGE_DIR", str(package))

    rendered = coordinator.append_artifacts_markdown(
        "报告已生成",
        json.dumps([str(report)]),
    )

    assert "## 生成产物" in rendered, (
        f"html 报告应追加产物块,got:\n{rendered}"
    )
    assert "report.html" in rendered, (
        f"报告块应含 report.html,got:\n{rendered}"
    )


# ═══════════════════════════════════════════════════════════════════
# Non-image artifact UI rendering
# ═══════════════════════════════════════════════════════════════════

def test_pptx_artifact_adds_download_link(tmp_path):
    """append_artifacts_markdown should produce a download link for .pptx."""
    pptx = tmp_path / "presentation.pptx"
    pptx.write_bytes(b"pptx")
    rendered = coordinator.append_artifacts_markdown(
        "PPT 生成完成",
        f'["{pptx}"]',
    )

    assert "## 生成产物" in rendered
    assert "下载" in rendered
    assert "presentation.pptx" in rendered
    # agent name = basename(AGENT_PACKAGE_DIR), dynamic not hardcoded (Fix 7)
    # URL 走 artifact_id 格式，无 ?path= 查询参数（artifact-storage.md §I-artifact-id）
    assert "/api/artifacts/" in rendered
    assert "?path=" not in rendered
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
    assert "deck.pptx" in rendered


# ═══════════════════════════════════════════════════════════════════
# I-6 不变量：artifacts 必须是 skill 本次新生成的文件，
# 不得是 skill 消费过的用户上传输入 / reference / 脚本自身路径。
# 防止框架把任务指令里引用的原图路径误识为"生成产物"，
# 导致最终响应末尾被强追加一段幽灵 "## 生成产物" markdown。
# ═══════════════════════════════════════════════════════════════════

def test_user_uploaded_image_is_not_collected_as_artifact(tmp_path, monkeypatch):
    """用户上传到 workspace/upload/（或 workspace/{session}/upload/）的原图落在任务指令文本里时，
    不应被 extract_artifact_paths 收进 artifacts_json（I-6）。
    复现日志 0707130424-4405fc2d5072 的幽灵产物 bug：
    skill prompt 引用了原图路径让 skill 调 outfit_vision.py 去看，
    框架不该把这个"输入"当成"产物"。
    """
    package = tmp_path / "agent"
    upload_dir = package / "workspace" / "upload"
    upload_dir.mkdir(parents=True)
    upload = upload_dir / "23b1e30ca94b4036.jpg"
    upload.write_bytes(b"fake jpg")
    monkeypatch.setattr(executor, "AGENT_PACKAGE_DIR", str(package)); monkeypatch.setattr(_pkg_skill_executor, "AGENT_PACKAGE_DIR", str(package))

    text = (
        '{"messages":[{"content":"点评一下我的穿搭？ '
        f'用户上传了图片，路径: {upload}'
        '"]}]}'
    )

    result = executor.extract_artifact_paths(text)

    assert result == [], (
        f"用户上传的原图不得被收进 artifacts_json，本次却收到：{result}"
    )


def test_user_upload_under_uploads_dir_is_not_collected(tmp_path, monkeypatch):
    """uploads/ 目录（另一个常见上传输入约定）同样不应被当成产物。"""
    package = tmp_path / "agent"
    upload_dir = package / "uploads"
    upload_dir.mkdir(parents=True)
    upload = upload_dir / "selfie.jpg"
    upload.write_bytes(b"fake jpg")
    monkeypatch.setattr(executor, "AGENT_PACKAGE_DIR", str(package)); monkeypatch.setattr(_pkg_skill_executor, "AGENT_PACKAGE_DIR", str(package))

    text = f'{{"file":"uploads/selfie.jpg"}}'

    result = executor.extract_artifact_paths(text)

    assert result == [], (
        f"uploads/ 下的用户输入不应被当成产物，本次却收到：{result}"
    )


def test_generated_image_under_outputs_is_still_collected(tmp_path, monkeypatch):
    """守卫只排除用户上传输入目录，不能误伤 skill 真正生成的产物。
    skill 本次新生成到 outputs/ 的效果图必须仍然被收进 artifacts_json。
    """
    package = tmp_path / "agent"
    outputs_dir = package / "outputs"
    outputs_dir.mkdir(parents=True)
    generated = outputs_dir / "outfit-1.png"
    generated.write_bytes(b"generated image")
    monkeypatch.setattr(executor, "AGENT_PACKAGE_DIR", str(package)); monkeypatch.setattr(_pkg_skill_executor, "AGENT_PACKAGE_DIR", str(package))

    text = f'{{"stdout":"效果图已生成: outputs/outfit-1.png"}}'

    result = executor.extract_artifact_paths(text)

    assert str(generated.resolve()) in result, (
        f"outputs/ 下本次新生成的产物应被收集，本次漏了：{result}"
    )


def test_user_upload_and_generated_image_coexist_only_collects_generated(tmp_path, monkeypatch):
    """混合场景：任务指令同时引用了用户上传原图 + 本次生成的效果图，
    框架应只收生成物、不收输入。锁住 I-6 边界与正常产物收集并存。"""
    package = tmp_path / "agent"
    upload_dir = package / "workspace" / "upload"
    outputs_dir = package / "outputs"
    upload_dir.mkdir(parents=True)
    outputs_dir.mkdir(parents=True)
    upload = upload_dir / "user-input.jpg"
    upload.write_bytes(b"input")
    generated = outputs_dir / "outfit-2.png"
    generated.write_bytes(b"generated")
    monkeypatch.setattr(executor, "AGENT_PACKAGE_DIR", str(package)); monkeypatch.setattr(_pkg_skill_executor, "AGENT_PACKAGE_DIR", str(package))

    text = (
        f'{{"prompt":"请看这张图：{upload} 并按它生成效果图 {generated}"}}'
    )

    result = executor.extract_artifact_paths(text)

    # I-6 边界：只收本次生成物，不收用户上传输入 — 长度必须恰好为 1
    assert result == [str(generated.resolve())], (
        f"应只收集本次生成的产物，实际收到：{result}"
    )


# ═══════════════════════════════════════════════════════════════════
# Structured artifact extraction (dual-filter: text regex + JSON artifacts field)
# ═══════════════════════════════════════════════════════════════════

def test_structured_artifacts_field_is_extracted(tmp_path, monkeypatch):
    package = tmp_path / "agent"
    image = package / "outputs" / "result.png"
    image.parent.mkdir(parents=True)
    image.write_bytes(b"img")
    monkeypatch.setattr(executor, "AGENT_PACKAGE_DIR", str(package)); monkeypatch.setattr(_pkg_skill_executor, "AGENT_PACKAGE_DIR", str(package))

    result = json.dumps({"status": "ok", "artifacts": ["outputs/result.png"]})

    extracted = executor.extract_structured_artifacts(result)

    assert str(image.resolve()) in extracted


def test_structured_artifacts_files_field_is_extracted(tmp_path, monkeypatch):
    package = tmp_path / "agent"
    html = package / "reports" / "chart.html"
    html.parent.mkdir(parents=True)
    html.write_text("<html></html>")
    monkeypatch.setattr(executor, "AGENT_PACKAGE_DIR", str(package)); monkeypatch.setattr(_pkg_skill_executor, "AGENT_PACKAGE_DIR", str(package))

    result = json.dumps({"files": ["reports/chart.html"]})

    extracted = executor.extract_structured_artifacts(result)

    assert str(html.resolve()) in extracted


def test_structured_artifacts_nonexistent_file_is_skipped(tmp_path, monkeypatch):
    monkeypatch.setattr(executor, "AGENT_PACKAGE_DIR", str(tmp_path)); monkeypatch.setattr(_pkg_skill_executor, "AGENT_PACKAGE_DIR", str(tmp_path))

    result = json.dumps({"artifacts": ["nonexistent.png"]})

    assert executor.extract_structured_artifacts(result) == []


def test_structured_artifacts_user_upload_is_rejected(tmp_path, monkeypatch):
    package = tmp_path / "agent"
    upload = package / "workspace" / "upload" / "input.jpg"
    upload.parent.mkdir(parents=True)
    upload.write_bytes(b"upload")
    monkeypatch.setattr(executor, "AGENT_PACKAGE_DIR", str(package)); monkeypatch.setattr(_pkg_skill_executor, "AGENT_PACKAGE_DIR", str(package))

    result = json.dumps({"artifacts": ["workspace/upload/input.jpg"]})

    assert executor.extract_structured_artifacts(result) == []


def test_extract_all_merges_text_and_structured_channels(tmp_path, monkeypatch):
    package = tmp_path / "agent"
    img1 = package / "outputs" / "from_text.png"
    img2 = package / "outputs" / "from_structured.png"
    img1.parent.mkdir(parents=True)
    img1.write_bytes(b"1")
    img2.write_bytes(b"2")
    monkeypatch.setattr(executor, "AGENT_PACKAGE_DIR", str(package)); monkeypatch.setattr(_pkg_skill_executor, "AGENT_PACKAGE_DIR", str(package))

    result = json.dumps({
        "stdout": f"生成完成 {img1}",
        "artifacts": ["outputs/from_structured.png"],
    })

    all_artifacts = executor.extract_all_artifacts(result)

    assert str(img1.resolve()) in all_artifacts
    assert str(img2.resolve()) in all_artifacts


def test_extract_all_excludes_text_only_input_reference(tmp_path, monkeypatch):
    """工具仅在文本里回显输入路径时，不得把它当成本轮生成产物。"""
    package = tmp_path / "agent"
    source = package / "references" / "source.png"
    source.parent.mkdir(parents=True)
    source.write_bytes(b"input")
    monkeypatch.setattr(executor, "AGENT_PACKAGE_DIR", str(package))
    monkeypatch.setattr(_pkg_skill_executor, "AGENT_PACKAGE_DIR", str(package))
    result = json.dumps({"stdout": f"analyzed input {source}"})

    assert executor.extract_all_artifacts(
        result,
        excluded_text_paths={str(source.resolve())},
    ) == []


def test_structured_artifact_overrides_input_reference_exclusion(
        tmp_path, monkeypatch):
    """Tool 明确声明同一路径为输出时，结构化通道仍是权威来源。"""
    package = tmp_path / "agent"
    edited = package / "outputs" / "edited.png"
    edited.parent.mkdir(parents=True)
    edited.write_bytes(b"edited")
    monkeypatch.setattr(executor, "AGENT_PACKAGE_DIR", str(package))
    monkeypatch.setattr(_pkg_skill_executor, "AGENT_PACKAGE_DIR", str(package))
    result = json.dumps({
        "stdout": f"saved {edited}",
        "files": ["outputs/edited.png"],
    })

    assert executor.extract_all_artifacts(
        result,
        excluded_text_paths={str(edited.resolve())},
    ) == [str(edited.resolve())]


# ═══════════════════════════════════════════════════════════════════
# Structured extraction: defensive parsing edge cases
# ═══════════════════════════════════════════════════════════════════

def test_structured_outputs_field_is_extracted(tmp_path, monkeypatch):
    package = tmp_path / "agent"
    svg = package / "diagrams" / "flow.svg"
    svg.parent.mkdir(parents=True)
    svg.write_text("<svg/>")
    monkeypatch.setattr(executor, "AGENT_PACKAGE_DIR", str(package)); monkeypatch.setattr(_pkg_skill_executor, "AGENT_PACKAGE_DIR", str(package))

    result = json.dumps({"outputs": ["diagrams/flow.svg"]})

    assert len(executor.extract_structured_artifacts(result)) == 1


def test_structured_http_url_passes_through(tmp_path, monkeypatch):
    monkeypatch.setattr(executor, "AGENT_PACKAGE_DIR", str(tmp_path)); monkeypatch.setattr(_pkg_skill_executor, "AGENT_PACKAGE_DIR", str(tmp_path))

    result = json.dumps({"artifacts": ["https://cdn.example.com/img.png"]})

    extracted = executor.extract_structured_artifacts(result)
    assert "https://cdn.example.com/img.png" in extracted


def test_structured_absolute_path_is_extracted(tmp_path, monkeypatch):
    img = tmp_path / "abs_result.png"
    img.write_bytes(b"img")
    monkeypatch.setattr(executor, "AGENT_PACKAGE_DIR", str(tmp_path / "pkg")); monkeypatch.setattr(_pkg_skill_executor, "AGENT_PACKAGE_DIR", str(tmp_path / "pkg"))

    result = json.dumps({"artifacts": [str(img)]})

    extracted = executor.extract_structured_artifacts(result)
    assert len(extracted) == 1


def test_structured_non_artifact_suffix_rejected(tmp_path, monkeypatch):
    txt = tmp_path / "readme.txt"
    txt.write_text("not an artifact")
    monkeypatch.setattr(executor, "AGENT_PACKAGE_DIR", str(tmp_path)); monkeypatch.setattr(_pkg_skill_executor, "AGENT_PACKAGE_DIR", str(tmp_path))

    result = json.dumps({"artifacts": ["readme.txt"]})

    assert executor.extract_structured_artifacts(result) == []


def test_structured_non_list_field_returns_empty(tmp_path, monkeypatch):
    monkeypatch.setattr(executor, "AGENT_PACKAGE_DIR", str(tmp_path)); monkeypatch.setattr(_pkg_skill_executor, "AGENT_PACKAGE_DIR", str(tmp_path))

    assert executor.extract_structured_artifacts('{"artifacts": "not-a-list"}') == []
    assert executor.extract_structured_artifacts('{"artifacts": 42}') == []
    assert executor.extract_structured_artifacts('{"artifacts": null}') == []


def test_structured_non_string_items_skipped(tmp_path, monkeypatch):
    img = tmp_path / "valid.png"
    img.write_bytes(b"img")
    monkeypatch.setattr(executor, "AGENT_PACKAGE_DIR", str(tmp_path)); monkeypatch.setattr(_pkg_skill_executor, "AGENT_PACKAGE_DIR", str(tmp_path))

    result = json.dumps({"artifacts": [123, None, {"nested": True}, str(img), ""]})

    extracted = executor.extract_structured_artifacts(result)
    assert len(extracted) == 1


def test_structured_empty_list_returns_empty(tmp_path, monkeypatch):
    monkeypatch.setattr(executor, "AGENT_PACKAGE_DIR", str(tmp_path)); monkeypatch.setattr(_pkg_skill_executor, "AGENT_PACKAGE_DIR", str(tmp_path))

    assert executor.extract_structured_artifacts('{"artifacts": []}') == []


def test_structured_malformed_json_returns_empty(tmp_path, monkeypatch):
    monkeypatch.setattr(executor, "AGENT_PACKAGE_DIR", str(tmp_path)); monkeypatch.setattr(_pkg_skill_executor, "AGENT_PACKAGE_DIR", str(tmp_path))

    assert executor.extract_structured_artifacts('{bad json') == []
    assert executor.extract_structured_artifacts('') == []
    assert executor.extract_structured_artifacts('   ') == []


def test_structured_non_dict_json_returns_empty(tmp_path, monkeypatch):
    monkeypatch.setattr(executor, "AGENT_PACKAGE_DIR", str(tmp_path)); monkeypatch.setattr(_pkg_skill_executor, "AGENT_PACKAGE_DIR", str(tmp_path))

    assert executor.extract_structured_artifacts('[1, 2, 3]') == []
    assert executor.extract_structured_artifacts('"just a string"') == []
    assert executor.extract_structured_artifacts('42') == []


def test_structured_multiple_fields_artifacts_takes_priority(tmp_path, monkeypatch):
    """When both 'artifacts' and 'files' exist, 'artifacts' wins (short-circuit or)."""
    package = tmp_path / "agent"
    real = package / "outputs" / "real.png"
    decoy = package / "outputs" / "decoy.png"
    real.parent.mkdir(parents=True)
    real.write_bytes(b"1")
    decoy.write_bytes(b"2")
    monkeypatch.setattr(executor, "AGENT_PACKAGE_DIR", str(package)); monkeypatch.setattr(_pkg_skill_executor, "AGENT_PACKAGE_DIR", str(package))

    result = json.dumps({
        "artifacts": ["outputs/real.png"],
        "files": ["outputs/decoy.png"],
    })

    extracted = executor.extract_structured_artifacts(result)
    # Only artifacts field is processed (or-chain short-circuits on first truthy)
    assert len(extracted) == 1


# ═══════════════════════════════════════════════════════════════════
# Dual-filter: dedup and channel independence
# ═══════════════════════════════════════════════════════════════════

def test_dual_filter_same_file_in_both_channels_is_deduped(tmp_path, monkeypatch):
    package = tmp_path / "agent"
    img = package / "outputs" / "shared.png"
    img.parent.mkdir(parents=True)
    img.write_bytes(b"img")
    monkeypatch.setattr(executor, "AGENT_PACKAGE_DIR", str(package)); monkeypatch.setattr(_pkg_skill_executor, "AGENT_PACKAGE_DIR", str(package))

    result = json.dumps({
        "stdout": f"see {img}",
        "artifacts": ["outputs/shared.png"],
    })

    all_artifacts = executor.extract_all_artifacts(result)
    # Same file found by both channels → appears exactly once
    matching = [a for a in all_artifacts if os.path.isfile(a) and "shared.png" in a]
    assert len(matching) == 1


def test_dual_filter_text_only_channel_works(tmp_path, monkeypatch):
    package = tmp_path / "agent"
    img = package / "outputs" / "text_only.png"
    img.parent.mkdir(parents=True)
    img.write_bytes(b"img")
    monkeypatch.setattr(executor, "AGENT_PACKAGE_DIR", str(package)); monkeypatch.setattr(_pkg_skill_executor, "AGENT_PACKAGE_DIR", str(package))

    result = json.dumps({"stdout": f"generated at {img}"})

    all_artifacts = executor.extract_all_artifacts(result)
    matching = [a for a in all_artifacts if os.path.isfile(a) and "text_only.png" in a]
    assert len(matching) == 1


def test_dual_filter_structured_only_channel_works(tmp_path, monkeypatch):
    package = tmp_path / "agent"
    img = package / "outputs" / "structured_only.png"
    img.parent.mkdir(parents=True)
    img.write_bytes(b"img")
    monkeypatch.setattr(executor, "AGENT_PACKAGE_DIR", str(package)); monkeypatch.setattr(_pkg_skill_executor, "AGENT_PACKAGE_DIR", str(package))

    result = json.dumps({"artifacts": ["outputs/structured_only.png"]})

    all_artifacts = executor.extract_all_artifacts(result)
    matching = [a for a in all_artifacts if os.path.isfile(a) and "structured_only.png" in a]
    assert len(matching) == 1


# ═══════════════════════════════════════════════════════════════════
# URL generation: edge cases for dynamic agent name
# ═══════════════════════════════════════════════════════════════════

def test_url_no_hardcoded_agent_name_in_any_path(tmp_path, monkeypatch):
    package = tmp_path / "my-cool-agent"
    img = package / "assets" / "look.png"
    img.parent.mkdir(parents=True)
    img.write_bytes(b"img")
    monkeypatch.setattr(coordinator, "AGENT_PACKAGE_DIR", str(package)); monkeypatch.setattr(_pkg_coordinator_decide, "AGENT_PACKAGE_DIR", str(package))

    url = _pkg_coordinator_decide.local_artifact_url(str(img))

    assert "outfit-assistant" not in url
    assert "my-cool-agent" in url


def test_url_package_name_with_dots(tmp_path, monkeypatch):
    package = tmp_path / "org.example.agent"
    img = package / "out.png"
    img.parent.mkdir(parents=True)
    img.write_bytes(b"img")
    monkeypatch.setattr(coordinator, "AGENT_PACKAGE_DIR", str(package)); monkeypatch.setattr(_pkg_coordinator_decide, "AGENT_PACKAGE_DIR", str(package))

    url = _pkg_coordinator_decide.local_artifact_url(str(img))

    assert "org.example.agent" in url


def test_url_deep_nested_directory_hierarchy(tmp_path, monkeypatch):
    package = tmp_path / "agent"
    deep = package / "a" / "b" / "c" / "d" / "e" / "deep.png"
    deep.parent.mkdir(parents=True)
    deep.write_bytes(b"img")
    monkeypatch.setattr(coordinator, "AGENT_PACKAGE_DIR", str(package)); monkeypatch.setattr(_pkg_coordinator_decide, "AGENT_PACKAGE_DIR", str(package))

    url = _pkg_coordinator_decide.local_artifact_url(str(deep))

    assert "/files/" in url
    assert "a/b/c/d/e/deep.png" in url


def test_append_artifacts_http_url_not_rewritten(tmp_path, monkeypatch):
    """HTTP URLs in artifacts should pass through unchanged (no local_artifact_url rewrite)."""
    monkeypatch.setattr(coordinator, "AGENT_PACKAGE_DIR", str(tmp_path)); monkeypatch.setattr(_pkg_coordinator_decide, "AGENT_PACKAGE_DIR", str(tmp_path))

    rendered = coordinator.append_artifacts_markdown(
        "完成",
        json.dumps(["https://cdn.example.com/image.png"]),
    )

    assert "https://cdn.example.com/image.png" in rendered
    assert "/api/artifacts/" not in rendered.split("https://cdn.example.com")[0]


# ═══════════════════════════════════════════════════════════════════
# 失败模式矩阵(failure_mode_matrix.md §二 append)——A3 HTML 死链接、
# A4 外部 URL 同 basename 幂等
# ═══════════════════════════════════════════════════════════════════

def test_append_html_img_dead_link_still_appends_block(tmp_path, monkeypatch):
    """A3: content 中 HTML <img src="./assets/look.png"> 引用在 rewrite 因
    basename 歧义(两个 look.png)无法解析时是死链接 → 不得误判为"已引用"而
    跳过生成产物块追加。若 basename 唯一可解析,rewrite 成功即内容已可访问,
    不追加是幂等正确行为(由 R6/R1 覆盖)。"""
    package = tmp_path / "custom-agent"
    image_a = package / "harness" / "assets" / "look.png"
    image_b = package / "outputs" / "look.png"
    image_a.parent.mkdir(parents=True)
    image_b.parent.mkdir(parents=True)
    image_a.write_bytes(b"png-a")
    image_b.write_bytes(b"png-b")
    monkeypatch.setattr(coordinator, "AGENT_PACKAGE_DIR", str(package))
    monkeypatch.setattr(_pkg_coordinator_decide, "AGENT_PACKAGE_DIR", str(package))

    rendered = coordinator.append_artifacts_markdown(
        '<section><img src="./assets/look.png" alt="look"></section>',
        json.dumps([str(image_a), str(image_b)]),
    )

    assert "## 生成产物" in rendered, (
        f"HTML 死链接(歧义未解析)不得跳过追加,got:\n{rendered}"
    )
    assert "/api/artifacts/custom-agent/files/" in rendered


def test_append_external_url_same_basename_is_idempotent(tmp_path, monkeypatch):
    """A4: content 已含外部 URL(https://cdn.example.com/look.png,可访问)且
    basename 与本地产物相同 → 视为已在内容中被引用,不重复追加本地产物(I-4 幂等)。"""
    package = tmp_path / "custom-agent"
    image = package / "harness" / "assets" / "look.png"
    image.parent.mkdir(parents=True)
    image.write_bytes(b"png")
    monkeypatch.setattr(coordinator, "AGENT_PACKAGE_DIR", str(package))
    monkeypatch.setattr(_pkg_coordinator_decide, "AGENT_PACKAGE_DIR", str(package))

    rendered = coordinator.append_artifacts_markdown(
        "参考图:![look](https://cdn.example.com/look.png)",
        json.dumps([str(image)]),
    )

    assert rendered.count("## 生成产物") == 0, (
        f"外部 URL 同 basename 应视为已引用,不追加块,got:\n{rendered}"
    )
    assert "https://cdn.example.com/look.png" in rendered


# ═══════════════════════════════════════════════════════════════════
# 补充场景维度(failure_mode_matrix.md §五)——B-video 视频产物、
# C-mixed 混合去重、C-versioned 版本号不误伤
# ═══════════════════════════════════════════════════════════════════

def test_append_video_artifact_becomes_download_block(tmp_path, monkeypatch):
    """B-video: 视频产物(.mp4)不在 _image_suffixes → 归入 downloads →
    追加下载块(而非图片块)。bridge 白名单不含视频后缀为已知限制,记录不修。"""
    package = tmp_path / "custom-agent"
    video = package / "outputs" / "demo.mp4"
    video.parent.mkdir(parents=True)
    video.write_bytes(b"mp4-bytes")
    monkeypatch.setattr(coordinator, "AGENT_PACKAGE_DIR", str(package))
    monkeypatch.setattr(_pkg_coordinator_decide, "AGENT_PACKAGE_DIR", str(package))

    rendered = coordinator.append_artifacts_markdown(
        "视频已生成",
        json.dumps([str(video)]),
    )

    assert "## 生成产物" in rendered, (
        f"视频产物应追加产物块,got:\n{rendered}"
    )
    assert "![生成图片" not in rendered, (
        f"视频不应走图片块,got:\n{rendered}"
    )
    assert "demo.mp4" in rendered, (
        f"视频应以下载块呈现,got:\n{rendered}"
    )


def test_append_mixed_duplicate_and_unique_artifacts(tmp_path, monkeypatch):
    """C-mixed: 2 个同 basename(look.png) + 1 个不重复(other.png) →
    追加 2 块(重复合并 1 次 + 不重复 1 次),不丢不重复产物。"""
    package = tmp_path / "custom-agent"
    a = package / "harness" / "assets" / "look.png"
    b = package / "outputs" / "look.png"
    c = package / "assets" / "other.png"
    a.parent.mkdir(parents=True)
    b.parent.mkdir(parents=True)
    c.parent.mkdir(parents=True)
    a.write_bytes(b"a")
    b.write_bytes(b"b")
    c.write_bytes(b"c")
    monkeypatch.setattr(coordinator, "AGENT_PACKAGE_DIR", str(package))
    monkeypatch.setattr(_pkg_coordinator_decide, "AGENT_PACKAGE_DIR", str(package))

    rendered = coordinator.append_artifacts_markdown(
        "完成",
        json.dumps([str(a), str(b), str(c)]),
    )

    assert rendered.count("## 生成产物") == 1, (
        f"混合产物只追加 1 个产物块,got:\n{rendered}"
    )
    assert "look.png" in rendered and "other.png" in rendered, (
        f"重复合并后不重复产物不得丢失,got:\n{rendered}"
    )
    assert rendered.count("![生成图片") == 2, (
        f"应渲染 2 张图片(look 1 次 + other 1 次),got:\n{rendered}"
    )


def test_append_versioned_artifacts_keep_highest(tmp_path, monkeypatch):
    """C-versioned: outfit-1.png + outfit-2.png 同家族(版本号) →
    _dedup_versioned_artifacts 只保留最高版本,追加 1 块(不误伤,幂等)。"""
    package = tmp_path / "custom-agent"
    v1 = package / "assets" / "outfit-1.png"
    v2 = package / "assets" / "outfit-2.png"
    v1.parent.mkdir(parents=True)
    v1.write_bytes(b"v1")
    v2.write_bytes(b"v2")
    monkeypatch.setattr(coordinator, "AGENT_PACKAGE_DIR", str(package))
    monkeypatch.setattr(_pkg_coordinator_decide, "AGENT_PACKAGE_DIR", str(package))

    rendered = coordinator.append_artifacts_markdown(
        "完成",
        json.dumps([str(v1), str(v2)]),
    )

    assert rendered.count("## 生成产物") == 1, (
        f"版本号产物应合并为 1 块,got:\n{rendered}"
    )
    assert "outfit-2.png" in rendered, (
        f"应保留最高版本 outfit-2.png,got:\n{rendered}"
    )
    assert "outfit-1.png" not in rendered, (
        f"低版本 outfit-1.png 不应出现,got:\n{rendered}"
    )


# ═══════════════════════════════════════════════════════════════════
# _ensure_artifacts_in_package: external file handling
# ═══════════════════════════════════════════════════════════════════

def test_ensure_external_file_copied_to_outputs(tmp_path, monkeypatch):
    package = tmp_path / "agent"
    outputs = package / "outputs"
    monkeypatch.setattr(executor, "AGENT_PACKAGE_DIR", str(package)); monkeypatch.setattr(_pkg_skill_executor, "AGENT_PACKAGE_DIR", str(package))

    external = tmp_path / "external.png"
    external.write_bytes(b"external-img")

    result = executor._ensure_artifacts_in_package([str(external)])

    assert len(result) == 1
    assert (outputs / "external.png").exists()
    assert (outputs / "external.png").read_bytes() == b"external-img"


def test_ensure_external_collision_gets_suffix(tmp_path, monkeypatch):
    package = tmp_path / "agent"
    outputs = package / "outputs"
    monkeypatch.setattr(executor, "AGENT_PACKAGE_DIR", str(package)); monkeypatch.setattr(_pkg_skill_executor, "AGENT_PACKAGE_DIR", str(package))

    dir_a = tmp_path / "dir_a"
    dir_b = tmp_path / "dir_b"
    dir_a.mkdir()
    dir_b.mkdir()
    file_a = dir_a / "report.png"
    file_b = dir_b / "report.png"
    file_a.write_bytes(b"AAA")
    file_b.write_bytes(b"BBB")

    result = executor._ensure_artifacts_in_package([str(file_a), str(file_b)])

    assert len(result) == 2
    assert (outputs / "report.png").exists()
    assert (outputs / "report_1.png").exists()
    assert (outputs / "report.png").read_bytes() == b"AAA"
    assert (outputs / "report_1.png").read_bytes() == b"BBB"


def test_package_output_is_not_overwritten_by_external_same_name(
        tmp_path, monkeypatch):
    """同批已有 package output 时，外部同名文件必须使用后缀。"""
    package = tmp_path / "agent"
    existing = package / "outputs" / "report.png"
    existing.parent.mkdir(parents=True)
    existing.write_bytes(b"package")
    external = tmp_path / "external" / "report.png"
    external.parent.mkdir()
    external.write_bytes(b"external")
    monkeypatch.setattr(executor, "AGENT_PACKAGE_DIR", str(package))
    monkeypatch.setattr(_pkg_skill_executor, "AGENT_PACKAGE_DIR", str(package))

    result = executor._ensure_artifacts_in_package(
        [str(existing), str(external)]
    )

    assert result == [
        str(existing.resolve()),
        str((package / "outputs" / "report_1.png").resolve()),
    ]
    assert existing.read_bytes() == b"package"
    assert (package / "outputs" / "report_1.png").read_bytes() == b"external"


def test_ensure_nonexistent_file_skipped(tmp_path, monkeypatch):
    package = tmp_path / "agent"
    monkeypatch.setattr(executor, "AGENT_PACKAGE_DIR", str(package)); monkeypatch.setattr(_pkg_skill_executor, "AGENT_PACKAGE_DIR", str(package))

    result = executor._ensure_artifacts_in_package(["/nonexistent/ghost.png"])

    assert result == []


def test_ensure_package_local_artifact_not_copied(tmp_path, monkeypatch):
    """Files already inside the package must NOT be copied — directory tree preserved."""
    package = tmp_path / "agent"
    nested = package / "reports" / "2024" / "q3" / "chart.png"
    nested.parent.mkdir(parents=True)
    nested.write_bytes(b"chart")
    monkeypatch.setattr(executor, "AGENT_PACKAGE_DIR", str(package)); monkeypatch.setattr(_pkg_skill_executor, "AGENT_PACKAGE_DIR", str(package))

    result = executor._ensure_artifacts_in_package([str(nested)])

    assert result == [str(nested.resolve())]
    assert not (package / "outputs" / "chart.png").exists()


# ═══════════════════════════════════════════════════════════════════
# Business risk: HTTP URLs must survive _ensure_artifacts_in_package
# ═══════════════════════════════════════════════════════════════════

def test_ensure_http_url_survives_pipeline(tmp_path, monkeypatch):
    """External CDN URLs must not be dropped by _ensure_artifacts_in_package.
    Pre-fix bug: Path('https://...').resolve() → not a file → silently skipped.
    """
    monkeypatch.setattr(executor, "AGENT_PACKAGE_DIR", str(tmp_path / "agent")); monkeypatch.setattr(_pkg_skill_executor, "AGENT_PACKAGE_DIR", str(tmp_path / "agent"))

    result = executor._ensure_artifacts_in_package(["https://cdn.example.com/img.png"])

    assert "https://cdn.example.com/img.png" in result


def test_ensure_mixed_http_and_local_both_survive(tmp_path, monkeypatch):
    package = tmp_path / "agent"
    local_img = package / "outputs" / "local.png"
    local_img.parent.mkdir(parents=True)
    local_img.write_bytes(b"local")
    monkeypatch.setattr(executor, "AGENT_PACKAGE_DIR", str(package)); monkeypatch.setattr(_pkg_skill_executor, "AGENT_PACKAGE_DIR", str(package))

    result = executor._ensure_artifacts_in_package([
        "https://cdn.example.com/remote.png",
        str(local_img),
    ])

    assert len(result) == 2
    assert "https://cdn.example.com/remote.png" in result


# ═══════════════════════════════════════════════════════════════════
# Business risk: version dedup must keep only latest variant
# ═══════════════════════════════════════════════════════════════════

def test_version_dedup_keeps_highest_numbered(tmp_path):
    paths = [
        str(tmp_path / "review-1.png"),
        str(tmp_path / "review-2.png"),
        str(tmp_path / "review-3.png"),
    ]

    result = _pkg_coordinator_decide._dedup_versioned_artifacts(paths)

    assert result == [str(tmp_path / "review-3.png")]


def test_version_dedup_keeps_unversioned_separately(tmp_path):
    paths = [
        str(tmp_path / "outfit-1.png"),
        str(tmp_path / "outfit-2.png"),
        str(tmp_path / "standalone.png"),
    ]

    result = _pkg_coordinator_decide._dedup_versioned_artifacts(paths)

    assert str(tmp_path / "outfit-2.png") in result
    assert str(tmp_path / "standalone.png") in result
    assert str(tmp_path / "outfit-1.png") not in result


def test_version_dedup_in_append_artifacts_markdown(tmp_path, monkeypatch):
    """End-to-end: user should only see the latest version, not all variants."""
    monkeypatch.setattr(coordinator, "AGENT_PACKAGE_DIR", str(tmp_path)); monkeypatch.setattr(_pkg_coordinator_decide, "AGENT_PACKAGE_DIR", str(tmp_path))
    # 必须真实落盘，否则 rewrite_local_artifact_links 因 isfile=False 跳过、URL 也不生成
    a1 = tmp_path / "outfit-review-1.png"
    a2 = tmp_path / "outfit-review-2.png"
    a1.write_bytes(b"png1")
    a2.write_bytes(b"png2")
    artifacts = json.dumps([str(a1), str(a2)])

    rendered = coordinator.append_artifacts_markdown("done", artifacts)

    assert "outfit-review-2.png" in rendered
    assert "outfit-review-1.png" not in rendered


# ═══════════════════════════════════════════════════════════════════
# Business risk: unresolved references must be left as-is (not broken)
# ═══════════════════════════════════════════════════════════════════

def test_unresolved_markdown_reference_left_unchanged(tmp_path, monkeypatch):
    """LLM references a file that doesn't exist → reference stays as-is,
    not rewritten to a broken /api/artifacts/ URL."""
    monkeypatch.setattr(coordinator, "AGENT_PACKAGE_DIR", str(tmp_path)); monkeypatch.setattr(_pkg_coordinator_decide, "AGENT_PACKAGE_DIR", str(tmp_path))

    rendered = coordinator.append_artifacts_markdown(
        "see ![ghost](nonexistent/ghost.png)",
        "[]",
    )

    assert "nonexistent/ghost.png" in rendered
    assert "/api/artifacts/" not in rendered


def test_unresolved_html_src_left_unchanged(tmp_path, monkeypatch):
    monkeypatch.setattr(coordinator, "AGENT_PACKAGE_DIR", str(tmp_path)); monkeypatch.setattr(_pkg_coordinator_decide, "AGENT_PACKAGE_DIR", str(tmp_path))

    rendered = coordinator.append_artifacts_markdown(
        '<img src="missing/photo.jpg">',
        "[]",
    )

    assert 'src="missing/photo.jpg"' in rendered
    assert "/api/artifacts/" not in rendered


# ═══════════════════════════════════════════════════════════════════
# Business risk: I-6 boundary on structured channel for uploads/ dir
# ═══════════════════════════════════════════════════════════════════

def test_structured_channel_rejects_uploads_dir(tmp_path, monkeypatch):
    package = tmp_path / "agent"
    upload = package / "uploads" / "selfie.jpg"
    upload.parent.mkdir(parents=True)
    upload.write_bytes(b"upload")
    monkeypatch.setattr(executor, "AGENT_PACKAGE_DIR", str(package)); monkeypatch.setattr(_pkg_skill_executor, "AGENT_PACKAGE_DIR", str(package))

    result = json.dumps({"artifacts": ["uploads/selfie.jpg"]})

    assert executor.extract_structured_artifacts(result) == []


# ═══════════════════════════════════════════════════════════════════
# Business risk: HTML href and poster attributes must be rewritten
# ═══════════════════════════════════════════════════════════════════

def test_html_href_attribute_is_rewritten(tmp_path, monkeypatch):
    package = tmp_path / "agent"
    report = package / "reports" / "full.html"
    report.parent.mkdir(parents=True)
    report.write_text("<html></html>")
    monkeypatch.setattr(coordinator, "AGENT_PACKAGE_DIR", str(package)); monkeypatch.setattr(_pkg_coordinator_decide, "AGENT_PACKAGE_DIR", str(package))

    rendered = coordinator.append_artifacts_markdown(
        '<a href="reports/full.html">Full Report</a>',
        json.dumps([str(report)]),
    )

    assert "/api/artifacts/" in rendered
    # 断言用原始 HTML 属性串（带 href= 前缀），避免匹配重写后 URL 里的同名子串
    # 反模式：`'reports/full.html"'` 会误匹配 `/api/artifacts/.../files/agent/reports/full.html"`
    assert 'href="reports/full.html"' not in rendered, (
        f"原始 HTML href 属性未被重写，rendered={rendered!r}"
    )


def test_html_poster_attribute_is_rewritten(tmp_path, monkeypatch):
    package = tmp_path / "agent"
    poster = package / "assets" / "thumb.jpg"
    poster.parent.mkdir(parents=True)
    poster.write_bytes(b"jpg")
    monkeypatch.setattr(coordinator, "AGENT_PACKAGE_DIR", str(package)); monkeypatch.setattr(_pkg_coordinator_decide, "AGENT_PACKAGE_DIR", str(package))

    rendered = coordinator.append_artifacts_markdown(
        '<video poster="assets/thumb.jpg"></video>',
        json.dumps([str(poster)]),
    )

    # 断言用原始 HTML 属性串（带 poster= 前缀），避免匹配重写后 URL 里的同名子串
    # 反模式：`'assets/thumb.jpg"'` 会误匹配 `/api/artifacts/.../files/agent/assets/thumb.jpg"`
    assert "/api/artifacts/" in rendered
    assert 'poster="assets/thumb.jpg"' not in rendered, (
        f"原始 HTML 属性未被重写，rendered={rendered!r}"
    )


# ═══════════════════════════════════════════════════════════════════
# append_artifacts_markdown 归一化去重(I-4 幂等)
# 缺陷修复:原实现用 `url not in content` 字符串匹配,LLM 输出以本地路径/占位符
# 引用产物时 URL 匹配不到 → 同一产物重复追加("## 生成产物" 块出现 2 次)。
# 修复后按 basename 归一化判断 content 是否已引用,避免重复。
# ═══════════════════════════════════════════════════════════════════

def test_artifact_already_in_content_not_appended_twice(tmp_path):
    """content 已含产物引用(basename 命中)时,不再追加第二个产物块。"""
    image = tmp_path / "look.png"
    image.write_bytes(b"image")
    # content 以本地路径形式引用同一产物(LLM 常见输出)
    rendered = coordinator.append_artifacts_markdown(
        f"![穿搭效果图]({image})",
        json.dumps([str(image)]),
    )
    # basename look.png 已在 content 中被引用 → 不追加任何 "## 生成产物" 块
    assert rendered.count("## 生成产物") == 0, f"已引用产物不应再追加块,got:\n{rendered}"
    assert "/api/artifacts/" in rendered


def test_same_basename_different_path_deduplicated(tmp_path):
    """同 basename 不同路径的产物只追加一次(归一化 key = basename)。"""
    a = tmp_path / "a" / "look.png"
    b = tmp_path / "b" / "look.png"
    a.parent.mkdir(parents=True)
    b.parent.mkdir(parents=True)
    a.write_bytes(b"a")
    b.write_bytes(b"b")
    # content 不含引用,两个产物 basename 相同 → 只追加 1 个块
    rendered = coordinator.append_artifacts_markdown(
        "完成",
        json.dumps([str(a), str(b)]),
    )
    assert rendered.count("## 生成产物") == 1, f"同 basename 应只追加 1 次,got:\n{rendered}"


def test_artifact_not_in_content_still_appended(tmp_path):
    """content 未引用产物时仍正常追加(未破坏正常追加路径)。"""
    image = tmp_path / "gen.png"
    image.write_bytes(b"png")
    rendered = coordinator.append_artifacts_markdown(
        "完成",
        json.dumps([str(image)]),
    )
    assert rendered.count("## 生成产物") == 1
    assert "/api/artifacts/" in rendered


def test_dead_link_same_basename_still_appends_block(tmp_path, monkeypatch):
    """content 中的死链接(./assets/look.png)与真实产物(harness/assets/look.png)同名，
    但路径错位 + basename 歧义(存在两个 look.png)导致 rewrite 无法解析 → 不得因同名
    误判"已在内容中被引用"而跳过生成产物块追加(fix-outfit-image-display 次因)。"""
    package = tmp_path / "custom-agent"
    a = package / "harness" / "assets" / "look.png"
    b = package / "outputs" / "look.png"
    a.parent.mkdir(parents=True)
    b.parent.mkdir(parents=True)
    a.write_bytes(b"a")
    b.write_bytes(b"b")
    monkeypatch.setattr(coordinator, "AGENT_PACKAGE_DIR", str(package))
    monkeypatch.setattr(_pkg_coordinator_decide, "AGENT_PACKAGE_DIR", str(package))

    rendered = coordinator.append_artifacts_markdown(
        "完成:![look](./assets/look.png)",
        json.dumps([str(a), str(b)]),
    )

    # 死链接不得跳过追加，且同 basename 去重只追加 1 个块
    assert rendered.count("## 生成产物") == 1, (
        f"死链接同名不得跳过追加且应去重为 1 块,got:\n{rendered}"
    )
    assert "/api/artifacts/" in rendered, (
        f"追加的产物块应含可访问 URL,got:\n{rendered}"
    )
