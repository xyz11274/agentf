"""AppWorld-only experience taxonomy, without changing Agent prompts."""
from __future__ import annotations


CLASSIFICATION = """
AppWorld 经验分类约束：环境应用的 apis.* 方法、Python 代码操作、认证 login/access_token、读取 API schema 后再调用、API 参数或分页处理，都是执行者生成代码时的行为经验，必须放入 experience_injections。即使其中有 A→B 顺序或成功依赖，也不属于 AgentFlow 的 Skill 编排。skill_orchestration_changes 只用于证据明确显示框架 run_skills 或实际命名 Skill 之间的派发关系改变；仅出现环境 API 名称不满足这个条件。停止条件建议仍须依据任务是否完成，不因业务 API 失败就要求终止其他目标。目标 scope 必须原样采用 allowed_experience_scopes；不修改官方验收要求，不固化训练题答案，不声称经验已验证。
日志投影约束：<redacted:credential_field>、redacted_password、[redacted] 等字段是日志安全投影，不等于实际执行时的参数或代码。不得仅凭脱敏展示推断实际使用了占位符、密码缺失、括号未闭合或 SyntaxError 的原因。真实失误必须由明确的 execution observation/error 以及可追溯事件支持；观察仅说明 SyntaxError 时，只能归纳报错行的语法检查，不得把安全投影解释成真实错误来源。脱敏后不能确认的因果关系标记证据不足，不生成关于占位符的行为经验。
""".strip()


def install(controller):
    """Write tiny private analysis templates; keep existing transport settings."""
    path = controller.RUNTIME / 'runtime.json'
    config = controller.read(path)
    destination = controller.RUNTIME / 'appworld-prompts'
    destination.mkdir(exist_ok=True)
    changed = []
    for key in ('dialogue_system', 'standard_dialogue_system', 'dialogue_user'):
        filename = key.replace('_', '-') + '.txt'
        source = controller.ROOT / 'tools/trajectory_evolution/config/prompts' / filename
        text = source.read_text().rstrip() + '\n\n' + CLASSIFICATION + '\n'
        target = destination / filename
        target.write_text(text)
        config['prompts'][key] = 'appworld-prompts/' + filename
        changed.append(key)
    controller.save(path, config)
    return {'scope': 'AppWorld_native_learning_only', 'analysis_prompt_keys': changed,
            'agent_prompt_changed': False, 'model_calls': 0}
