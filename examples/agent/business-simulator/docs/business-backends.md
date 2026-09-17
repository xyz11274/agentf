# 可运行的业务测试系统

家居和挂号不再固定回复，也不再因为缺少实体连接而全部拒绝。Agent 通过 HTTP 使用两个独立后端；测试可从数据库、设备遥测、库存和操作事件验证实际结果。它们的结果同时注明 `environment=business_test_system`、`simulated=true`、`physical_effect=false`，不会向同名真实医院挂号，也不会影响用户的实体家电。

## 家庭设备

默认有16台设备，分布在客厅、卧室、书房、厨房和卫生间。当前房间明确配置为客厅。初始开启书房台灯、客厅风扇、电视和卧室灯，是测试环境的启动状态，不代表用户的使用习惯。

`smart-home query` 返回设备ID、房间、在线状态、电源、目标属性、观测属性和耗电。`control` 接收结构化 `commands`，一个批次生成一个 `commandList` 元素，所有参数验证成功才一起提交。不存在设备、多个候选、离线和越界都会产生明确结果，不留下半批控制效果。`request_id` 相同且参数相同的重试不会重复控制。

支持开关、空调16–30℃/制冷/制热/除湿/睡眠/自动/送风/风速，灯光亮度与色调，窗帘开合，电视与音箱音量等。组控制须明确 `all=true`；比如卧室全部灯包括顶灯和床头灯。

独立后台每秒推进设备世界：窗帘全程运动约20秒，空调影响房间温湿度，工作设备累计耗电。窗帘下发命令与运动到位是两个状态；`settled=false` 不能说已经到位。物理变化采用简化模型，适合测试控制和观察链路，不用于真实楼宇热力学预测。

`habits` 统计实际控制和运行历史。支持日期、时段、设备、房间过滤，返回开关时刻中位数、按小时运行时长、按标称功率积分的耗电和设备同时运行时长；无覆盖历史时明确返回空。到家/离家模式仍是开关时间启发式标签，不代表检测了人员进出。

配置 `HOME_ASSISTANT_URL/TOKEN` 后，可用 `execution_target=home_assistant` 查询真实实体，并通过 query 返回的 `entity_id` 控制。写操作要求稳定 `request_id`，执行后回读实体状态。

## 预约服务

测试目录包括市人民医院、市第一人民医院、上海瑞金医院、北京301医院、华西医院等路由别名，医生明确命名为测试医师/专家。目录和排班是测试系统配置，不是这些医院公布的实时资料。

开放今天起28天的测试号源，默认每号源容量2。普通号和专家号的时间不同，可按医院、科室、医生、日期、上午/下午及最早时刻筛选。相对日期按 Asia/Shanghai 解析；无法确定的日期请求补充，过去或超出开放窗口的日期失败，不静默改成另一天。

工具流程：

1. `appointment_catalog` 查看目录，`appointment_slots` 查询库存。
2. `appointment_create` 用已查询的 `slot_id` 预约，或者按完整约束选择最早可用号源。未指定医院使用已配置的市人民医院，返回记录说明默认值。
3. `appointment_query` 查询本人记录；其他用户不能查询或取消该记录。
4. `appointment_cancel(id)` 取消并释放库存。
5. `appointment_reschedule(id,date或slot_id)` 原子改约；新号源失败时保留旧预约。

库存由所有用户共享，预约归用户所有。SQLite 事务同时更新库存占用、本人状态、审计事件与重试缓存；并发抢最后一个号源最多一人成功。同一患者同一时段冲突会失败。指定号源不能绕过附带的日期或时段约束，查询动作不会变成预订。重置某用户的测试状态只释放该用户的预约。

## 独立调用

正常 `run.sh` 同时启动 AgentFlow、HTTP服务、设备时钟和提醒调度。也可只启动业务后端：

```bash
/root/miniconda3/envs/agentflow-dev/bin/python examples/agent/business-simulator/scripts/run_backend.py
```

默认地址 `http://127.0.0.1:17894`。身份来自框架或HTTP请求头 `X-Business-User`、`X-Business-Client`，LLM不能在工具参数里自行修改身份。服务只用于受信任本机测试，没有面向公网的用户认证系统。

```bash
curl --noproxy '*' -s http://127.0.0.1:17894/v1/services/smart-home \
  -H 'Content-Type: application/json' -H 'X-Business-User: demo' -H 'X-Business-Client: home' \
  -d '{"action":"control","command_text":"客厅灯亮度调到50%","commands":[{"device":"灯","room":"客厅","action":"set","property":"brightness","value":50}]}'

curl --noproxy '*' -s http://127.0.0.1:17894/v1/devices \
  -H 'X-Business-User: demo' -H 'X-Business-Client: home'
```

`GET /v1/state` 读完整状态，`GET /v1/events` 读操作结果，`GET /health` 查看设备时钟是否正常。运维测试可用 `POST /v1/devices/living-ac/connection` 和 `{"online":false}` 注入离线故障；该入口不暴露给模型。恢复在线传 `true`。

## 可选真实设备

`multi-device list/select` 会列出实际 ADB 和 Home Assistant 连接。Android 设备可由 `device-gui` 完成真实 UI 观察、截图、点击、滑动、输入、按键和应用启动。工具每一步重新读取 UI；操作完成仍须按页面/设备状态核对业务结果。当前环境没有已授权设备，协议适配器只通过受控测试服务验证。

## 尚缺能力

当前环境缺少真实医院/患者账号、原用户历史、用户定位与实时地图凭证、手机主动推送、私有产品账户、可用视频生成/媒体理解模型，以及各购物/办公/媒体网站的用户账号和付费权限。Home Assistant 与 ADB 适配器已经实现，但只有配置实际服务或连接实际设备后才能证明外部动作完成。网站能力逐站受登录、验证码和网络限制。
