"""One public tool per skill; legacy agent names are compatibility metadata."""
from __future__ import annotations

SERVICES = {
    "web-search": ("qaagent", "综合问答；真实网页检索、天气、当前时间、写作、推理、上下文修正", ["query", "weather", "geocode", "route", "current_time", "save_document", "get_document"]),
    "health-advice": ("healthagent", "健康导诊、健康档案、用药提醒、挂号及查询", ["triage", "record_save", "record_query", "record_delete", "reminder_create", "reminder_update", "reminder_pause", "reminder_resume", "reminder_query", "reminder_cancel", "notifications", "appointment_catalog", "appointment_slots", "appointment_create", "appointment_query", "appointment_cancel", "appointment_reschedule"]),
    "smart-home": ("homelifeagent", "家电批量控制、设备状态、使用习惯", ["control", "query", "habits"]),
    "device-gui": ("GUIagent", "网站或已授权 Android 设备的实际界面操作，以及APP操作规划", ["plan", "execute", "query", "device_observe", "device_tap", "device_swipe", "device_type", "device_key", "device_open_app"]),
    "movie-query": ("movieagent", "电影筛选、推荐列表、详情、偏好、序号选片、播放控制", ["recommend", "save_recommendations", "detail", "play", "pause", "resume", "query"]),
    "knowledge-base": ("woagent", "公开资料检索与用户资料导入、版本管理、知识库检索", ["search", "import_url", "import_text", "search_local", "list_sources", "delete_source"]),
    "user-memory": ("wisdomagent", "用户显式偏好的增删改查；原 Agent 对应关系为推定", ["read", "write", "delete"]),
    "multimodal": ("multimodalagent", "调用已配置图片模型生成真实图片文件；视频能力需可用模型", ["generate_image", "generate_video", "plan", "query"]),
    "study-buddy": ("studybuddyagent", "学习辅导、分步讲解和学习计划", ["save", "query", "delete"]),
    "emotion-chat": ("emotionchatagent", "情绪陪伴和用户主动保存的自助计划", ["save", "query", "delete"]),
    "third-party": ("thirdproxyagent", "真实网页资料检索、服务能力查询及受控 Webhook", ["query", "status", "webhook_post"]),
    "office-work": ("officeagent", "真实办公文件导出、版本管理、日历、SMTP 邮件和在线办公网站操作", ["save", "query", "delete", "calendar_create", "email_send"]),
    "custom-workflow": ("customagent", "保存计划或执行由真实业务工具组成的有限流程", ["save", "query", "delete", "workflow_save", "workflow_run"]),
    "multi-device": ("multidevagent", "列举和选择真实设备，也可在有状态测试设备群中执行跨设备任务", ["list", "select", "status", "handoff", "broadcast", "sync", "set_online", "history"]),
    "travel-planner": ("travelagent", "真实旅游网站查询与操作、行程文件", ["query", "save"]),
    "foodie": ("foodieagent", "真实餐厅与外卖网站查询、菜谱和饮食偏好", ["query", "save"]),
    "shopping": ("shoppingagent", "真实购物网站搜索、商品比较、购物车与订单状态核对", ["query", "save"]),
    "service-manager": ("srvagent", "实际服务能力与缺口查询；职责为推定", ["list", "status"]),
}


def string(description: str = "") -> dict:
    return {"type": "string", "description": description}


COMMON = {"action": string(), "request_id": string("写操作重试时复用同一 id；新操作使用新 id"),
          "query": string("检索词/症状/筛选词；不得填预期答案"), "key": string("偏好或文档标识"),
          "value": string("偏好值"), "content": string("来自用户或本 skill 生成的正文；不得伪造事实"),
          "id": string("上次工具返回的记录 id"), "title": string(), "topic": string()}

FIELDS = {
    "health-advice": {"record_fields": {"type": "object", "properties": {
        "symptoms": string("用户提供的症状"), "diagnosis": string("用户转述的诊断"), "medication": string("用户提供的用药信息"),
        "doctor_advice": string("用户转述的医嘱"), "medical_history": string("用户提供的既往病史"),
        "allergies": string("用户提供的过敏史"), "visit_date": string("用户提供的就诊日期")}, "additionalProperties": False}, "medicine": string("仅用户或既存医嘱提供的药名"), "dosage": string("仅记录给定用量，不推导处方"),
                      "times": {"type": "array", "items": string("HH:MM")}, "period": string("用户指定周期，缺省未指定"),
                      "hospital": string(), "department": string(), "date": string("保留明天/下周一等原始表述"),
                      "patient": string(), "doctor": string(), "appointment_type": string(), "timezone": {"type":"string","enum":["Asia/Shanghai","UTC"]},
                      "slot_id": string("appointment_slots 返回的号源id"), "time_period": string("上午/下午；保留用户要求"),
                      "not_before": string("就诊时刻不得早于 HH:MM"),
                      "start_date": string("YYYY-MM-DD"), "end_date": string("YYYY-MM-DD；每日重复截止日期")},
    "smart-home": {"execution_target":{"type":"string","enum":["test_home","home_assistant"]}, "commands": {"type": "array", "items": {"type": "object", "properties": {
        "device": string("灯/空调/窗帘/风扇/电视/加湿器/空气净化器/扫地机器人/插座/热水器/音箱/台灯"),
        "room": string("客厅/卧室/书房/厨房"), "action": {"type": "string", "enum": ["on", "off", "set"]},
        "device_id": string("query 返回的设备id"), "all": {"type": "boolean", "description": "用户要求控制匹配的全部设备"},
        "property": {"type": "string", "enum": ["temperature", "brightness", "position", "volume", "mode", "fan_speed", "color"]},
        "value": {"type": ["number", "string"]}}, "required": ["device", "action"], "additionalProperties": False}},
        "room": string(), "device": string(), "device_id": string(), "command_text": string("完整用户指令，仅作为审计文本，不驱动解析")},
    "movie-query": {"genre": string(), "mood": string(), "actor": string(), "exclude": {"type": "array", "items": string()},
                    "count": {"type": "integer", "minimum": 1, "maximum": 10}, "index": {"type": "integer", "minimum": 1},
                    "family_friendly": {"type": "boolean"},
                    "recommendations": {"type":"array", "items":{"type":"object","properties":{
                        "title":string(),"highlight":string(),"source_url":string(),"play_url":string("已知合法播放页，不能把介绍页冒充播放源"),"year":{"type":"integer"},"genres":{"type":"array","items":string()},"actors":{"type":"array","items":string()},"rating":{"type":"number"},"family_friendly":{"type":"boolean"}},
                        "required":["title","highlight","source_url"],"additionalProperties":False}}},
    "device-gui": {"device_id":string("multi-device 返回的 Android serial"), "ref":string("最近 device_observe 返回的 adb ref"), "x":{"type":"integer","minimum":0}, "y":{"type":"integer","minimum":0}, "x2":{"type":"integer","minimum":0}, "y2":{"type":"integer","minimum":0}, "duration_ms":{"type":"integer","minimum":50,"maximum":5000}, "keycode":string(), "package":string(), "app": string(), "operation": {"type": "string", "enum": ["open", "search", "play", "navigate", "setting", "order", "recharge", "send", "favorite", "unfavorite", "install", "uninstall", "checkin", "comment", "taxi", "download", "history", "playlist", "like", "page", "query_balance", "login"]},
                   "episode": {"type": "integer", "minimum": 1}, "origin": string(), "destination": string(),
                   "amount": {"type": "number", "minimum": 0}, "confirmed": {"type": "boolean"},
                   "avoid_congestion": {"type":"boolean"}, "route_preference":{"type":"string","enum":["fastest","recommended"]},
                   "city": string(), "phone_number": string(),
                   "origin_coordinates":string("已知的高德 GCJ-02 经度,纬度，不得猜测用户定位"),
                   "destination_coordinates":string("已知的高德 GCJ-02 经度,纬度，不得用天气地点坐标替代")},
    "web-search": {"location": string("具体城市或地区，如川西、成都"), "date": string("本周/未来一周/今天/明天/YYYY-MM-DD"),
                   "start_date":string("YYYY-MM-DD"),"end_date":string("YYYY-MM-DD"), "timezone":string(),
                   "city":string(), "origin":string("实际起点地址；当前位置不可猜测"), "destination":string("实际终点地址"),
                   "origin_coordinates_wgs84":string("WGS84 经度,纬度"), "destination_coordinates_wgs84":string("WGS84 经度,纬度"),
                   "travel_mode":{"type":"string","enum":["driving","walking","cycling"]}},
    "multimodal": {"size":{"type":"string","enum":["2K","3K"]}},
    "multi-device": {
        "device_id": string(),
        "execution_target": {"type":"string", "enum":["all", "physical", "virtual"]},
        "source_device_id": string(), "target_device_id": string(),
        "target_device_ids": {"type":"array", "items":string()},
        "task_kind": {"type":"string", "enum":["generic", "navigation", "media", "document", "notification", "message", "smart_home"]},
        "data_type": string(), "payload": {"type":"object", "additionalProperties":True},
        "online": {"type":"boolean"},
    },
}

# Explicit business constraints: unknown arguments must never disappear silently.
FIELDS['health-advice'].update({
    'not_after':string('最晚就诊 HH:MM'), 'include_history':{'type':'boolean'}, 'status':string(),
    'weekdays':{'type':'array','items':{'type':'integer','minimum':0,'maximum':6}},
    'interval_days':{'type':'integer','minimum':1,'maximum':365},
    'meal_times':{'type':'array','items':string('实际饭点 HH:MM，不得猜测')},
    'meal_offset_minutes':{'type':'integer','minimum':-180,'maximum':180},
    'active':{'type':'boolean'}, 'include_inactive':{'type':'boolean'}, 'include_deleted':{'type':'boolean'},
    'allow_duplicate':{'type':'boolean'}, 'source_evidence':{'type':'array','items':string('用户原话或实际资料来源')},
})
FIELDS['device-gui'].update({
    'recipient':string(), 'page':string(), 'playlist':string(), 'vehicle_type':string(), 'pickup_point':string(),
    'variant':string(), 'quantity':{'type':'integer','minimum':1,'maximum':999}, 'address':string(),
    'index':{'type':'integer','minimum':1},
})
FIELDS['device-gui']['operations']={'type':'array','items':{'type':'object','properties':{**COMMON,**FIELDS['device-gui']},
    'required':['app','operation'],'additionalProperties':False}}
FIELDS['smart-home'].update({'analysis':{'type':'string','enum':['habits','energy','usage','cooccurrence']},
    'date':string(), 'start_date':string(), 'end_date':string(), 'compare_device':string(), 'time_period':string()})

FIELDS['custom-workflow']={'steps':{'type':'array','items':{'type':'object','properties':{'skill':{'type':'string','enum':[s for s in SERVICES if s!='custom-workflow']},'args':{'type':'object','additionalProperties':True}},'required':['skill','args'],'additionalProperties':False}}}

FIELDS['office-work'] = {
    'format':{'type':'string','enum':['md','txt','html','docx','xlsx','csv','pdf','pptx']},
    'sheets':{'type':'array','items':{'type':'object','properties':{'name':string(),'rows':{'type':'array','items':{'type':'array','items':{'type':['string','number','boolean']}}}},'required':['rows'],'additionalProperties':False}},
    'slides':{'type':'array','items':{'type':'object','properties':{'title':string(),'content':string()},'required':['title','content'],'additionalProperties':False}},
    'start_time':string('ISO8601 含时区，例如 2026-09-20T10:00:00+08:00'),
    'end_time':string('ISO8601 含时区'), 'location':string(),
    'to':{'type':['string','array'],'items':string()}, 'cc':{'type':['string','array'],'items':string()},
    'subject':string(), 'confirmed':{'type':'boolean'},
}

FIELDS['third-party'] = {
    'payload':{'type':'object','additionalProperties':True}, 'confirmed':{'type':'boolean'},
}

from .browser import ACTIONS as BROWSER_ACTIONS, FIELDS as BROWSER_FIELDS
for _skill, (_legacy, _description, _actions) in list(SERVICES.items()):
    SERVICES[_skill] = (_legacy, _description + '；可使用真实浏览器观察网页、点击、填写、下载和核验结果', _actions + BROWSER_ACTIONS)
    FIELDS.setdefault(_skill, {}).update(BROWSER_FIELDS)


def contract(skill: str) -> tuple[str, str, dict]:
    legacy, description, actions = SERVICES[skill]
    properties = {**COMMON, **FIELDS.get(skill, {})}
    properties["action"] = {"type": "string", "enum": actions}
    if skill == "smart-home":
        description += "。首次query不传room/device过滤，先获取全屋清单；将用户所有房间的动作组成完整commands后只调用一次control。set自动开机，成功后禁止再用on做确认，只能query"
    return ("sim_" + skill.replace("-", "_"),
            f"{description}。执行 {legacy} 能力；家居、挂号和虚拟多设备使用可运行的独立业务测试系统，返回实际状态、库存或设备回执但无物理外部效果。其他未连接能力返回 unsupported。sim_ 仅为兼容工具名。",
            {"type": "object", "properties": properties, "required": ["action"], "additionalProperties": False})


def validate(value, schema: dict, path: str = "args") -> None:
    """Validate our small JSON Schema subset without third-party dependencies."""
    types = schema.get("type", [])
    types = [types] if isinstance(types, str) else types
    checks = {"object": isinstance(value, dict), "array": isinstance(value, list),
              "string": isinstance(value, str), "boolean": isinstance(value, bool),
              "integer": isinstance(value, int) and not isinstance(value, bool),
              "number": isinstance(value, (int, float)) and not isinstance(value, bool)}
    if types and not any(checks.get(t, False) for t in types):
        raise ValueError(f"{path}: expected {'/'.join(types)}")
    if "enum" in schema and value not in schema["enum"]:
        raise ValueError(f"{path}: not one of {schema['enum']}")
    if isinstance(value, dict):
        props = schema.get("properties", {})
        for name in schema.get("required", []):
            if name not in value:
                raise ValueError(f"{path}.{name}: required")
        for name, item in value.items():
            if name not in props and schema.get("additionalProperties") is False:
                raise ValueError(f"{path}.{name}: unknown field")
            if name in props:
                validate(item, props[name], f"{path}.{name}")
    elif isinstance(value, list):
        for i, item in enumerate(value):
            validate(item, schema.get("items", {}), f"{path}[{i}]")
    elif isinstance(value, (int, float)) and not isinstance(value, bool):
        import math
        if not math.isfinite(value) or value < schema.get("minimum", float('-inf')) or value > schema.get("maximum", float('inf')):
            raise ValueError(f"{path}: outside allowed range")
