# 自进化核心产物说明

业务与AppWorld共用“采集→训练反思→经验入库→策略生成→GEPA→对比发布”流程。本文只介绍最重要的8类产物，每个例子均取自现存文件，保留关键字段。长正文、深层对象和数组内容用`…`作展示占位，其余值取自原文件；`…`不是原始数据，也不是可执行配置。

输出统一位于 `trace_output/<实验>/training/`、`gepa/`、`published/`。JSON便于阅读，部分同名`.pb`为原生二进制存储。**草案、可执行策略、实际应用和评测收益是四个不同阶段。**

| 核心产物   | 回答的问题                         |
| ---------- | ---------------------------------- |
| 标准轨迹   | Agent实际执行了什么？              |
| 评测报告   | 是否通过，质量和无效loop是多少？   |
| 训练反思   | 从执行中提炼了什么经验？           |
| 经验库     | 哪些经验入库，来源与状态是什么？   |
| 策略       | 冻结了哪些经验，获准执行什么？     |
| GEPA反思   | 根据哪些事实提出了什么改动？       |
| 搜索树     | 谁是父节点，评测与选优进行到哪里？ |
| 对比与发布 | 最终策略是什么，成绩能否追溯？     |

## 1. 标准执行轨迹

按事件保存真实运行，是所有分析和实际应用统计的事实来源。正式报告也可将它嵌入evidence.trace。

真实实例：[aa425f485ecd330cbac4b1379349d1940c9dd075.trace.json](../../../trace_output/appworld/training/evolution/by-user/appworld-training/traces/010ef7123dbb2f3fc4406b65130fb132/aa425f485ecd330cbac4b1379349d1940c9dd075.trace.json)，摘录 `根对象及events[0]`。

```json
{
  "trace_kind": "trajectory.v1",
  "identity": {
    "agent_id": "appworld-agentflow",
    "run_id": "aa425f485ecd330cbac4b1379349d1940c9dd075"
  },
  "events": [
    {
      "event_id": "event-1",
      "kind": "run.start",
      "sequence": "1",
      "actor_id": "task"
    },
    {
      "event_id": "event-27",
      "kind": "action.end",
      "actor_id": "unspecified",
      "status": "failed",
      "caused_by": [
        "event-25",
        "event-26"
      ],
      "input": {
        "code": "import json\n# Review Gmail API docs\nprint(json.dumps(apis.api_docs.show_api_descriptions(\"gmail\"), indent=2))\n"
      },
      "output": {
        "observation": "Execution failed. Traceback:\n  File \"<python-input>\", line 3, in <module>\n    print(json.dumps(apis.api_docs.show_api_descriptions(\"gmail\"), indent=2))\n                     ^^^^^^^^^^^^^^^^^^^^^^^^^^^^^^^^^^^^^^^^^^^^\nTypeError: _wrap_api_request.<locals>._api_request() takes 0 positional arguments but 1 was given"
      },
      "error": {
        "code": "appworld_type_error_positional",
        "retryability": "unknown"
      },
      "attributes": {
        "executed": "…",
        "extensions": "…",
        "observed_error_code": "…",
        "observed_failure": "…",
        "semantic": "…"
      }
    }
  ]
}
```

| 关键字段                      | 含义                                                 |
| ----------------------------- | ---------------------------------------------------- |
| `identity`                  | agent、用户、运行和会话身份，关联同一次执行。        |
| `events[].kind / operation` | 事件类型和实际操作，如动作结束、loop决策、建议应用。 |
| `event_id / caused_by`      | 事件及前因引用，用于追踪动作与观察。                 |
| `input / output / error`    | 实际代码/参数、观察和错误；凭证脱敏。                |
| `application / attributes`  | 策略查询和真实应用回执；应用不等于任务成功。         |

## 2. 正式评测报告

report.json汇总一批单题结果；trials/*.json或.json.gz保存相同单题结构。

真实实例：[report.json](../../../trace_output/appworld/training/full-captures/train-full-09/report.json)，摘录 `trials[0]`。

```json
{
  "task_id": "appworld-training-amazon_collection_quantity_summary-debug-07b42fd_1",
  "execution_status": "completed",
  "verdict": "failed",
  "evidence": {
    "assessment": {
      "quality": 0.0,
      "coverage": 1.0,
      "critical_failures": 0.0,
      "goals": [
        {
          "id": "task",
          "status": "failed",
          "weight": 1.0,
          "critical_failure": false,
          "evidence_paths": [
            "/traces"
          ],
          "reason": "…③“Then inspect the exact public response schema in a separate code block and repair the container access”——show_wish_list schema 在 event-46（故障前）已检查，故障后未在单独代码块中重新检查；training_feedback 记录失败项“exact_public_schema_inspected_after_failure_before_successful_read”，缺失。…因必需项③缺失，判 failed。"
        }
      ]
    },
    "loop_review": {
      "status": "complete",
      "observed_iterations": 24.0,
      "count": 0.0,
      "iteration_reviews": [
        "…"
      ]
    },
    "trace": {
      "events": [
        "…"
      ]
    }
  },
  "checks": [
    {
      "check_id": "budget.steps",
      "evidence_paths": [
        "/metrics/steps"
      ],
      "reason": "确定性检查通过。",
      "status": "passed"
    },
    {
      "check_id": "strict",
      "evidence_paths": [
        "/output/appworld_reward"
      ],
      "reason": "实际证据不满足预设条件。",
      "status": "failed"
    }
  ],
  "duration_ms": 81279.998219,
  "metrics": {
    "tokens": 75559.0,
    "llm_calls": 16.0,
    "loop_count": 24.0
  }
}
```

| 关键字段                                           | 含义                                                                 |
| -------------------------------------------------- | -------------------------------------------------------------------- |
| `task_id / trial_id / loaded_version`            | 题目、评测尝试和实际加载策略，用于同题同口径比较。                   |
| `execution_status / verdict / checks`            | 执行状态、严格结论及验收依据；completed仍可能failed。                |
| `assessment.quality / goals / critical_failures` | 目标完成度、逐目标判定和关键违规；正式有效Q还受硬检查约束。          |
| `loop_review.count / iteration_reviews`          | 裁判确认的无效迭代数及逐次依据；metrics.loop_count只是原始循环次数。 |
| `duration_ms / metrics`                          | Agent时延、token、模型/工具调用；注明覆盖，额外尝试另计。            |

严格通过以verdict=passed为准。平均有效Q取各题正式有效质量的平均；硬检查失败或关键违规可使Q归0。平均无效loop=有效评审题的无效迭代总数÷有效评审题数。缺失评分不能补0。

## 3. 训练反思结果

训练分析把轨迹切成episode，评判片段并提炼经验。该文件是学习结果，正式题目成绩在上一类报告中。

真实实例：[001f19c7ee63c894e10a9aed81c1a6aad8043ec1.evaluation.json](../../../trace_output/appworld/training/evolution/by-user/appworld-training/evaluation/e85b62b4413eb3413322546337e5f358/001f19c7ee63c894e10a9aed81c1a6aad8043ec1.evaluation.json)，摘录 `reflection.experience_details[0]`。

```json
{
  "assessments": [
    "…"
  ],
  "dialogue_assessment": {
    "status": "…",
    "task_outcome": "…",
    "reason": "…"
  },
  "reflection": {
    "root_causes": [
      "调用 file_system API 时参数名错误（path 应为 directory_path/file_path），导致两次 422 失败"
    ],
    "summary": "任务最终写入 CSV 并删除账户，但官方测试 3 项失败：歌曲标题到艺术家映射不匹配，业务未完全成功。",
    "experience_details": [
      {
        "evidence_event_ids": [
          "event-135",
          "event-162"
        ],
        "proposed_change": "先通过 api_docs 查询该 API 的准确参数名（如 directory_path、file_path），再按文档传参，避免凭猜测使用 path。",
        "target_scope": "agent:appworld-agentflow/actor:default",
        "trigger_pattern": "调用 file_system 的 create_directory 或 create_file 时收到 422 参数校验错误"
      }
    ]
  }
}
```

| 关键字段                                 | 含义                                     |
| ---------------------------------------- | ---------------------------------------- |
| `assessments / dialogue_assessment`    | 片段及整段任务的评判、置信度和事件证据。 |
| `reflection.root_causes`               | 模型归因，需用原始事件核验。             |
| `target_scope / trigger_pattern`       | 经验适用范围和触发条件。                 |
| `proposed_change / evidence_event_ids` | 建议做法与支持事件，供批量综合生成候选。 |

## 4. 经验库

evolution-memory.pb保存原生经验，JSON为可读镜像；共享经验另在learning/shared-memory.json聚合。

真实实例：[evolution-memory.json](../../../trace_output/appworld/training/evolution/by-user/appworld-training/memory/evolution-memory.json)，摘录 `entries[0]`。

```json
{
  "memory_id": "memory:candidate:011fefecdcebc58762c9e6f13b643956e040b0f1f1ca17ab144571f26ef8bdfa",
  "status": "EVOLUTION_MEMORY_STATUS_CANDIDATE",
  "candidate": {
    "target_type": "CANDIDATE_TARGET_TYPE_EXPERIENCE_INJECTION",
    "target_scope": "agent:appworld-agentflow/actor:default",
    "trigger_pattern": "调用 show_api_doc 查询 API 文档时返回 422 错误，提示 API 名称不存在",
    "proposed_change": "先调用 show_api_descriptions 获取该应用的全部 API 名称列表，再查询具体 API 文档，避免使用臆测的 API 名称。",
    "support_episode_count": "2",
    "observed_problem": "任务成功完成。执行者通过 phone 联系人识别室友/同事，批准了 10 笔待处理 Venmo 付款请求，并调用 complete_task 标记完成。过程中多次因 API 参数错误失败后重试，最终成功。",
    "support_evidence": [
      "…"
    ]
  },
  "source_candidate_ids": [
    "…"
  ],
  "observed_batch_ids": [
    "…"
  ],
  "revisions": [
    "…"
  ],
  "relations": [
    "…"
  ]
}
```

| 关键字段                                       | 含义                                                        |
| ---------------------------------------------- | ----------------------------------------------------------- |
| `memory_id / status`                         | 经验身份与管理状态；示例是CANDIDATE，已入库不等于获准运行。 |
| `candidate`                                  | 作用域、触发条件、问题、建议和置信度。                      |
| `support_evidence / source_candidate_ids`    | 支持轨迹、事件和候选来源，避免无依据经验。                  |
| `observed_batch_ids / revisions / relations` | 训练来源、内容历史及经验关系。                              |
| `shared-memory.entries / state`              | 共享经验与支持/争议状态；审核进度保存在.curation.json。     |

## 5. 可执行策略与冻结快照

seed与GEPA节点策略都采用这一结构。经验库先冻结成snapshot，再编译为可执行注入和控制规则。

真实实例：[strategy.json](../../../trace_output/appworld/published/strategy/strategy.json)，摘录 `根对象及policy.options`。

```json
{
  "level": "L3",
  "agent": {
    "name": "appworld-agentflow",
    "revision": "appworld-agentflow-v22"
  },
  "policy": {
    "effective_level": "L3",
    "snapshot": {
      "experiences": [
        {
          "target_scope": "agent:appworld-agentflow/actor:default",
          "trigger_pattern": "需要统计推荐结果中的某个实体（如艺术家）出现次数时，分页获取全部数据后使用 Counter 统计",
          "memory_id": "…",
          "content_revision": "…",
          "content": "当需要统计推荐结果中某个实体的出现次数时，应分页获取全部数据（循环调用直到返回空），再使用 Counter 统计各实体出现次数，取出现次数最多的作为答案。",
          "match_terms": [
            "…"
          ]
        }
      ]
    },
    "options": {
      "requested_level": "L3",
      "fallback": "strict",
      "aggressive_controls": false,
      "approved_experiences": [
        "…"
      ],
      "experience_overrides": [
        "…"
      ],
      "action_rules": [
        "…"
      ],
      "schedule_rules": [],
      "loop_rules": [
        {
          "max_iterations": 30,
          "max_no_progress": 3,
          "max_replans": 1,
          "error_codes": [
            "appworld_code_error",
            "appworld_type_error_positional",
            "appworld_auth_401",
            "appworld_validation_422",
            "policy_action_rejected"
          ]
        }
      ]
    }
  }
}
```

| 关键字段                                        | 含义                                             |
| ----------------------------------------------- | ------------------------------------------------ |
| `policy.snapshot.experiences`                 | 冻结经验正文、触发条件、匹配词、来源和内容版本。 |
| `approved_experiences / experience_overrides` | 获准来源及子策略正文/触发改写。                  |
| `action_rules / schedule_rules / loop_rules`  | L2动作、L2编排和L3循环控制规则。                 |
| `target_scope / source_experience / actor_id` | 规则适用范围、授权来源和执行者。                 |
| `level / effective_level`                     | 能力等级，不代表实际应用次数或效果达标。         |

策略是否实际起作用须查轨迹回执。agent-version通常在搜索节点、发布清单或loaded_version中；本文件没有顶层version字段。

## 6. GEPA反思请求与改写草案

request.json保存模型输入，draft.json保存模型建议。分组map后进行reduce归并和counterexample-review复核，再经原生预检与评测。

真实实例：[3.pool-map-8.draft.json](../../../trace_output/appworld/gepa/workspace/agent-evaluation/22cdfd1e31890e6b8e8cad5e84ff91ebe34739a44f08b165a5a6453642743534/search/reflections/3.pool-map-8.draft.json)，摘录 `control_updates[0]，省略其余规则字段`。

```json
{
  "control_updates": [
    {
      "component": "loop_rules",
      "op": "add",
      "value": {
        "actor_id": "unspecified",
        "error_codes": [
          "appworld_auth_401"
        ],
        "max_no_progress": 3.0,
        "max_replans": 0.0
      }
    }
  ]
}
```

| 关键字段                                                    | 含义                                                     |
| ----------------------------------------------------------- | -------------------------------------------------------- |
| `request.input.training_batch_summary`                    | 完整pool的正式通过、质量、无效loop事实。                 |
| `request.input.feedback / selected_components / controls` | 分组轨迹、可改组件、原规则与授权预算。                   |
| `draft.control_updates / experience_updates`              | 建议改写的规则或经验；component、op、value明确具体操作。 |
| `selected_proposals / reason`                             | 选中提案及模型解释；reason不是正式运行计数。             |

示例actor="unspecified"与真实运行一致。草案须通过差分预检才可能生成versions/<node></node>.json；该Node3没有可执行版本。

## 7. GEPA搜索树与检查点

tree.json记录父子关系、反思、评测和选优；原生state.json负责作业调度与恢复。

真实实例：[tree.json](../../../trace_output/appworld/gepa/workspace/agent-evaluation/22cdfd1e31890e6b8e8cad5e84ff91ebe34739a44f08b165a5a6453642743534/search/tree.json)，摘录 `nodes中的Node3`。

```json
{
  "id": 3.0,
  "parent": 0.0,
  "status": "untriggerable_controls",
  "reason": "no_differential_training_trigger",
  "components": [
    "loop_rules"
  ],
  "parent_evaluation": {
    "missing_task_ids": [],
    "status": "complete"
  },
  "tasks": [
    "…"
  ],
  "control_preflight": {
    "execution_changes": "…",
    "missing": "…",
    "passed": "…",
    "planning_hints": "…",
    "rules": "…"
  },
  "experience_preflight": {
    "basis": "…",
    "coverage": "…",
    "missing": "…",
    "passed": "…",
    "sources": "…",
    "status": "…"
  }
}
```

| 关键字段                                                 | 含义                                         |
| -------------------------------------------------------- | -------------------------------------------- |
| `id / parent / version`                                | 节点、父节点和已生成策略身份。               |
| `status / reason`                                      | 阶段或终态及原因，失败草案不能当已发布节点。 |
| `tasks / parent_evaluation / evaluations`              | 同题pool、父评测及attempt/report缓存引用。   |
| `screening / control_preflight / experience_preflight` | 筛选、真实差分及适用条件依据。               |
| `frontier / publication_frontier`                      | 可继续探索的父节点与可发布候选，资格不同。   |

## 8. 对比结果与发布产物

comparison.json是正式报告的精简视图；发布目录同时保留strategy.json与manifest.json，将最终策略、验收和原始来源关联。

真实实例：[comparison.json](../../../trace_output/appworld/published/strategy/comparison.json)，摘录 `metrics`。

```json
{
  "metrics": {
    "L0_reused": {
      "completed": 500,
      "scored": 500,
      "strict_passed": 322,
      "quality": 0.644,
      "avg_invalid_loop": 0.38
    },
    "seed_candidate_typeerror_causal_current": {
      "completed": 500,
      "scored": 500,
      "strict_passed": 338,
      "quality": 0.676,
      "avg_invalid_loop": 0.204
    }
  },
  "items": [
    {
      "task_id": "appworld-024c982_1",
      "trial_id": "f14dc68033a08c414aa131a2bbc45d95",
      "verdict": "passed",
      "quality": 1.0,
      "invalid_loop_count": 0.0,
      "loaded_version": "agent-version-v1:3cf539a85ea45457a3040faa3b6881e92a25b0b32be56b5a03c92da302595bb5",
      "source_shard": "shard-00"
    }
  ],
  "source_l0": "…",
  "seed": "…"
}
```

| 关键字段                                                       | 含义                                              |
| -------------------------------------------------------------- | ------------------------------------------------- |
| `comparison.items / metrics`                                 | 逐题精简结果及L0/候选聚合指标；只比较同题同口径。 |
| `*_coverage / tokens_complete / execution_costs_complete`    | 各项评分和资源统计是否完整。                      |
| `manifest.version / source_strategy / native_source_reports` | 发布策略及权威来源。                              |
| `manifest.goal_passed / release_status`                      | 验收与发布状态；写出文件不等于达标。              |
| `manifest.model_calls_scope / independent_holdout`           | 费用范围及是否独立留出测试。                      |

通过/质量相对提升=(候选−L0)÷L0；loop相对下降=(L0−候选)÷L0，分母为0时不能直接计算。门禁使用代码原值，不能按展示四舍五入判定。

## 字段之间怎样关联

`comparison.items.task_id / trial_id` → 原`report.trials` → `evidence.trace`及`loop_review` → 真实应用经验身份 → 策略`snapshot` → 经验库`support_evidence`与训练事件。

`tree.nodes.version` → `versions/<node>.json` → 父子策略差异 → `reflections`及同题评测；最终发布再通过`manifest.native_source_reports`指向权威成绩。

日志、锁、配置、启动清单与审核恢复文件是辅助产物，阅读核心设计时无需逐一展开。源结构可查[标准轨迹](../proto/trajectory_trace.proto)、[运行策略](../proto/runtime_policy.proto)和[训练反思](../proto/hybrid_evaluation.proto)。
