# AgentFlow 导师汇报（2026-10-09）

[可编辑PPT](AgentFlow_自进化系统_导师汇报_20261009.pptx) · [PDF预览](AgentFlow_自进化系统_导师汇报_20261009.pdf) · [逐页讲稿](逐页讲稿.md) · [仓库详查与证据](仓库梳理与证据索引.md) · [源码定位](源码与产物定位.md)

30页主讲，建议30分钟；8页答疑附录。每页包含演讲备注、源码或产物来源。架构图、表格及3幅实验柱状图均为可编辑元素，图表数据嵌入PPT。

汇报结构：总体架构与请求运行（4—6页）、自进化机制（7—18页）、实验进展和问题（19—25页）、研究方向与导师讨论（26—30页）、源码及答疑（31—38页）。

当前代码快照为`f44bbaf4a9531731bfbd3eb6c115fe4210df95b1`。历史实验的运行Git commit不能证明等于当前HEAD；本次只核查当前源码与现存摘要，没有重跑模型、构建、软件回归或故障注入。

AppWorld同500开发对照正式strict 322→338、Q .644→.676、平均无效loop .380→.204；质量相对提升4.968944%，未达到5%门槛；loop下降46.315789%。交付标记为`best_available_not_qualified`。500已用于开发反馈，不能称独立留出。原生来源和本机运行环境的缺失在文稿中已标注。官方字段与正式严格验收分别报告。

验证材料：

- [20项实验摘要核查](实验数据核查回执.json)：同题、覆盖、指标、状态、来源路径及业务选择偏差；不证明原轨迹或真实模型复现。
- [文稿校验](文稿校验回执.json)：38页、30分钟主讲、演讲备注、原生图表、页面边界及PDF正文。
- [独立内容审查](内容审查记录.md)：架构、自进化机制和实验数字。第24页表格的视觉问题已修整为单行。
- [全部页面预览](preview/contact-sheet.png)。

重新生成需要Python3、`python-pptx`、Pillow、PyMuPDF和LibreOffice。`build_content.py`保存内容，`build_report.py`保存版式；均只操作汇报目录。

```bash
python3 docs/advisor-report-20261009/build_content.py
python3 docs/advisor-report-20261009/build_report.py
libreoffice --headless --convert-to pdf --outdir docs/advisor-report-20261009 \
  docs/advisor-report-20261009/AgentFlow_自进化系统_导师汇报_20261009.pptx
python3 docs/advisor-report-20261009/validate_presentation.py
python3 docs/advisor-report-20261009/render_preview.py
```

现存实验摘要可用`validate_experiment_summaries.py`重新核查；它固定检查本次源码快照并写本目录回执，不调用模型或评分器。
