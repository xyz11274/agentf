# AppWorld 实验

使用 `examples/agent/business-simulator` 的 AgentFlow 底座执行 AppWorld 跨应用任务。环境和官方评分适配位于 `examples/agent/business-simulator/appworld/`；本目录负责正常训练、经验学习、策略编译、GEPA与对照发布。

当前机器已经具备构建产物和 AppWorld 环境，无需重装。新机器先准备 Linux、Python 3.12、tmux，以及 C++17／CMake／Protobuf与protoc／libcurl／OpenSSL／RapidJSON工具链；AgentFlow环境见[开发环境](../../../../docs/development-environment.md)，原生组件依赖见[安装文档](../../docs/02-installation.md)。以下一次性准备命令均在仓库根目录执行：

```bash
# 构建真正执行Agent的daemon；RapidJSON路径兼容Conda或系统安装。
cmake -S . -B build -DCMAKE_BUILD_TYPE=Release -DAGENTFLOW_BUILD_TESTS=OFF \
  -DRAPIDJSON_INCLUDE_DIR="${CONDA_PREFIX:-/usr}/include/rapidjson"
cmake --build build --target agentflowd --parallel 2

# 构建本实验实际优先使用的最新原生CLI和库。
cmake -S tools/trajectory_evolution -B tools/trajectory_evolution/build-v4 -DCMAKE_BUILD_TYPE=Release
cmake --build tools/trajectory_evolution/build-v4 --target trajectory_evolution trajectory_evolution_local --parallel 2

# AppWorld使用独立Python 3.12虚拟环境，沿用当前已验证的官方包版本。
python3.12 -m venv examples/agent/business-simulator/appworld/runtime/venv
examples/agent/business-simulator/appworld/runtime/venv/bin/python -m pip install 'appworld==0.1.3.post1'
# 官方CLI先解包应用源码，再下载data 0.1.0到适配器固定world目录。
examples/agent/business-simulator/appworld/runtime/venv/bin/python -m appworld.cli install
examples/agent/business-simulator/appworld/runtime/venv/bin/python -m appworld.cli download data \
  --root examples/agent/business-simulator/appworld/runtime/world

# 仅新环境创建凭据文件；随后填写AF_LLM_API_KEY、AF_LLM_BASE_URL、AF_LLM_MODEL。
[ -f examples/agent/business-simulator/.env ] || cp examples/agent/business-simulator/.env.example examples/agent/business-simulator/.env
```

AppWorld要求 `pydantic<2`，生产runtime的MCP依赖使用另一套环境；不要将 `agentflow-runtime` 的完整依赖安装进AppWorld虚拟环境。本适配器直接读取仓库中的runtime与客户端源码。`business-simulator/setup-host.sh` 只准备业务环境，不能代替上面的AppWorld解包和数据准备。安装和数据下载需要联网；下载只在新环境执行，官方命令会替换目标目录已有的 `data/`。Host使用固定 `runtime/venv/bin/python` 和 `runtime/world`，AgentFlow daemon默认使用 `build/agentflowd`；若daemon已有于其它位置，启动前设置 `AGENTFLOW_DAEMON` 为其绝对路径。实验优先使用 `tools/trajectory_evolution/build-v4/trajectory_evolution`，无该文件才回退 `tools/trajectory_evolution/build/trajectory_evolution`，从头复现应完成上面的最新构建。

填好 `.env` 后，在仓库根目录使用这一条命令启动真实API全流程：

```bash
# 真实API全链路：训练→原生学习与审核→seed→32/100题验证→GEPA持续选优→500题对照→发布。
# tmux运行、每实验16路并发、禁用代理；seed门禁完整失败后自动生成不同候选。
bash tools/trajectory_evolution/experiments/appworld/reproduce.sh --fresh --seed-label strategy --gepa-max-model-calls 1024

# 复用训练、健康评分和检查点，跟随当前自动seed或原GEPA计划继续同一流程。
# 正常恢复省略 --seed-label，避免重新指定起始策略。
bash tools/trajectory_evolution/experiments/appworld/reproduce.sh --resume --gepa-max-model-calls 1024

# 从当前 published/strategy/strategy.json 重启完整流程，保留训练和发布结果。
# 旧GEPA已清空时补齐缺失32/100原生证明；seed完整失败沿同一共享流程迭代。
# 非空旧搜索先验证原32/100证明再重置；健康且来源完整的执行仍正常复用。
bash tools/trajectory_evolution/experiments/appworld/reproduce.sh --restart-gepa --all --gepa-max-model-calls 1024

# 只读查看阶段安排及实际状态，不调用模型。
bash tools/trajectory_evolution/experiments/appworld/reproduce.sh --plan
bash tools/trajectory_evolution/experiments/appworld/reproduce.sh --status

```

旧GEPA目录已按要求清空。`published/strategy/baseline.json` 与 `comparison.json` 保留原500题紧凑成绩，不能代替原生轨迹、任务版本和冻结裁判证明；旧 `seed-search-71-stage500` 原生来源已不存在，不能再次运行其交付核验。重启先补齐缺失32/100证明，不为这一步直接重跑500题，也不将旧Node13的32题对照当作当前发布seed证明。新的500题对照只在前置门禁与GEPA选优之后进行；缺失来源不能伪造为已验证缓存。

`--resume` 内部运行共享 `--all`：完成或复用134题训练采集、评分、原生学习与批准入库，再生成seed并依次验证固定32／100题，达标后运行GEPA，再对合格新节点做完整500题对照与发布。seed的32或100题完整正式失败后，程序复用训练成果，从批准TRAIN来源重新编译不同的不可变候选；所有候选沿用原L0、裁判与门槛，不能跳过100题进入GEPA。临时429、网络或评分待补齐时保留当前候选标签和健康检查点，由同一入口自动恢复；主动取消后可用同一 `--resume` 命令继续。

生成结果与历史完整策略或原生版本相同时，不重复评测该版本。程序保留重复记录，结合完整同题评测和实际L1应用记录，生成尚未尝试的L1组合；原批准经验、正文、控制和预算不变，组合的效果仍由正常32／100题门禁确认。完整正式失败后继续产生不可变候选，不再执行“连续五次失败停止”，也不需要人工重置记录。

已付费草案被本地校验拒绝时，先从原批准来源执行不调用模型的备用编译；仍没有合法条件时保存生成失败及费用，自动继续不同候选，不冒充一次32题评测失败。策略冻结后的生成回执保存在既有manifest内，恢复不依赖会被下一轮覆盖的诊断文件，也不重新支付原调用。

正常GEPA先运行30轮。没有合格新节点时，程序在无活评测、未开始holdout的闭合断点扩展原作业累计预算，保留原树、健康评分和费用记录；达到作业硬上限后保留完整旧作业并转入新候选搜索。初始seed或同版本策略不会被当作新节点重复跑500题。仅合格新候选进入后续对照与发布。

429、临时网络故障和待补评分由同一进程退避并从检查点恢复，不需要重新执行命令；取消指令仍会停止。凭据失效、批准来源撤销、数据损坏或所有合法候选组合已耗尽会保存明确的未达标状态，不能把这些状态当成成功。退出码0表示正常收尾，只有正式发布门禁通过才表示达到目标。

从头复现会使用真实模型重新采集、学习和选优，不保证重建历史批准集合或固定分数。当前正常训练范围为134题，新seed默认采用 `observed_phase_v3`，经原生来源验证生成L1条件及有真实训练支持的L3控制；模型草案及有界证据回退都必须通过原生审批、作用域和预算检查，不能手写memory。已有标签使用其不可变策略，不因追加训练数据而重新冻结。

`--gepa-max-model-calls` 接受1到1024，省略时沿用原配置。它仅控制优化器反思调用上限，不改变每题Agent预算、裁判或门槛，也不保证30轮都有合法改善。变更预算须在无Agent执行的闭合断点正常恢复，不能启动第二个控制器修改活任务。原作业保留其冻结训练范围；后续新作业使用当前训练范围。

`--fresh` 清理前检查其它会话的实际writer、训练锁和原生状态，活作业存在时拒绝清理；中断恢复使用 `--resume`。包装器直接启动Bash并保留退出pane；实际退出码位于 `training/run.log` 的 `appworld_controller_exit_code`。查看实验可用 `tmux attach -t appworld-experiment`，按 Ctrl-b、d 退出查看。

正常发布要求完整500题三核心门禁通过。GEPA按验证结果选出至多3个真实候选，实际只有1或2个时使用实际数量，不复制策略凑数；每个候选的原生报告和冻结配置来源都须通过同一门禁。正常GEPA发布与显式最佳可用交付使用同一选优写入器，按真实来源类型核验；已有更优完整结果不会被旧seed覆盖。显式最佳可用交付重新验证原策略版本、全部500题原生来源、同冻结裁判和完整L0覆盖，输出 `published/strategy/strategy.json`、`manifest.json`、`comparison.json`。未达标如实记录 `best_available_not_qualified`；`--status` 的独立 `delivery` 字段展示交付状态，不把GEPA或生命周期改为完成。导出动作的0模型调用仅指本次导出，原实验费用和未知成本仍保留。这不能代替上面的真实API全链路命令。

## 数据与输出

输入位于本目录 `datasets/`：

| 文件 | 范围 |
|---|---|
| `train.jsonl` | 134题：90道官方TRAIN＋44道公开TRAIN环境补充题 |
| `val.jsonl` | 12题，用于GEPA选优 |
| `holdout.jsonl` | 固定500题开发对照范围 |
| `comparison-500.jsonl` | 同一500题发布对照范围 |
| `manifest.json` | 数据范围、来源与计数 |

补充题训练全数量迁移、分页处理、响应容器修复、鉴权恢复、保存结果后展示及变更后定向确认，沿用公开后置条件。旧题及验证题保持不变；正常入口追加、采集、评分、学习并核验完整覆盖。134题已完成9批正常学习导入，导入完成不表示所有候选记忆已批准。当前GEPA作业使用已冻结的134题TRAIN范围，每轮父子pool采样32题。

32/100题是固定500题的开发验证子集，500内含42道官方dev任务，开发也参考过其失败结果。因此500成绩属于开发对照，不能解释为独立留出集效果。题目范围按公开任务规格固定；经验只从TRAIN来源入库，不使用测试答案生成记忆。

正式输出集中在仓库根目录 `trace_output/appworld/`，与业务实验一致：

| 目录 | 内容 |
|---|---|
| `training/` | 训练轨迹、权威memory、学习回执、配置和物化数据 |
| `gepa/` | 搜索树、检查点、节点策略、原生评测和全部尝试费用来源 |
| `published/` | 通用交付策略及说明，保留核验所需的选定原策略和L0 |

旧 `.work/`、本地投影诊断及重复逐题镜像只有在无活进程、无来源依赖且权威结果完整保留时清理。原始报告、官方IO、权威memory和实际额外执行成本不能删除或记0。Host适配代码位于 `examples/agent/business-simulator/appworld/`，正式评分沿用原生共享裁判。

## 已完整评分的最佳500题结果

下表来自保留的 `published/strategy/comparison.json` 与 `baseline.json`，记录原父71与同一L0的500题完整评分，原执行和裁判错误均为0；它是原始成绩记录，旧原生报告已清理，不能据此声称当前仍可完整复核轨迹。

| 指标 | L0 | 最佳已完整评分策略 |
|---|---:|---:|
| 严格通过 | 322/500 | 338/500 |
| 正式有效质量 | 0.644 | 0.676 |
| 严格通过及质量相对提升 | — | 4.968944% |
| 平均无效loop | 0.380 | 0.204 |
| 无效loop相对下降 | — | 46.315789% |
| 原执行平均时延 | 暂无完整统计 | 172.43秒，覆盖500题 |
| 原执行平均token | 暂无完整统计 | 172923.98，覆盖497题 |
| 原执行平均模型调用 | 暂无完整统计 | 25.72233，覆盖497题 |
| L1实际应用 | 0 | 595 |
| L2实际应用 | 0 | 0 |
| L3实际应用 | 0 | 4 |

当前保留策略为 `trace_output/appworld/published/strategy/strategy.json`，保留成绩为同目录 `comparison.json` 与 `baseline.json`。5%门槛需至少339题通过，338仍未达标，不能四舍五入算通过。loop门槛已满足。原记录中L0和候选分别有34、3次额外执行尝试；缺失费用保持未知，表中原执行均值不是完整实验成本。

## 指标与门禁

业务与AppWorld使用相同原生定义。只比较同题、同策略身份、同Host及冻结裁判的结果；批次未完成时单列有效覆盖，缺失不能补0。

| 指标 | 计算与限制 |
|---|---|
| 完成数 | 完成执行的题数；有效评分数另列，二者不能混用 |
| 严格通过 | 原生 `verdict=passed`；确定性检查与模型严格验收均须通过 |
| 正式有效质量Q | 普通目标按适用权重计算完成度；非模型确定性检查失败或关键违规使有效Q归0；逐题有效Q取平均 |
| 平均无效loop | 完整 `loop_review` 中无效迭代总数除以有效评审题数；未评审或清单冲突保留未知 |
| 平均时延 | 逐题原生 `duration_ms` 的平均，展示秒时除以1000；注明覆盖数 |
| 平均token／模型调用 | 原生执行 `metrics.tokens`／`metrics.llm_calls` 的平均；注明覆盖，裁判、学习和额外尝试费用另列 |
| 意图识别率 | 真实多目标派发的micro-F1；无金标或覆盖时为暂无 |
| L1／L2／L3应用 | 真实边界 `advice.applied` 回执计数，不能从配置或经验条数推算；L2区分动作与编排 |

当前AppWorld每题一个用户请求、一个普通验收目标，完整有效Q为0或1。模型目标完成度 `assessment.quality=1` 不保证严格通过；官方严格检查失败仍使有效Q为0。官方 `output.quality_score` 的部分检查比例只是辅助指标。多轮任务使用最新有效目标判定，关键违规保留；内部调用不是用户轮次，也不平均每一步质量。

32、100和500题采用同样三核心门槛：严格通过和正式Q相对提升至少5%，平均无效loop相对下降至少15%。时延、token、调用及费用完整性如实报告，当前不作为门槛。连续5个不同seed完整失败时正常停在 `review_required`，审查真实轨迹和修复验证后才能显式开始新轮；不以新标签绕过门槛。

32通过后，100阶段复用原32题，只执行新增68题。同一不可变策略已有完整500题时，可在来源、任务范围和冻结裁判严格一致后复用其同题子集；L0也复用完整权威来源。原生有界恢复仅补失败评分，健康执行和评分不重跑；不完整重放不能替换完整检查点，其真实额外成本仍保留。 GEPA出错、中断或取消会保留待恢复状态，按上面的 `--resume` 继续同一作业；活原生服务存在时拒绝重复启动，只有真实终态完成后才进入对比发布。最终按pool／val选优再做完整500对照，未合格策略只能作为明确标注的最佳可用结果交付。
