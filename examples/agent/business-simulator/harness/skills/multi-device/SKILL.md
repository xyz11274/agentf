---
name: multi-device
description: 真实设备连接与有状态虚拟设备群的跨设备执行
version: 1.2.0
---

# multi-device

业务角色：`multidevagent`。工具 `sim_multi_device`，action：list, select, status, handoff, broadcast, sync, set_online, history。

先用 `list(execution_target=all)` 同时读取真实连接与虚拟设备。每个设备都有 `device_kind=physical|virtual`、`simulated` 和 `physical_effect`，汇总时必须保留边界：虚拟设备的成功不能写成用户实体设备已经变化。

- `select(device_id)` 选择设备。真实 Android 或 Home Assistant 后续仍由 device-gui / smart-home 执行并回读。
- `handoff(source_device_id,target_device_id,task_kind,payload,request_id)` 在虚拟手机、平板、电脑、电视、音箱或手表之间接力导航、媒体、文档、通知等任务，并回读目标设备的 `current_activity`。
- `broadcast(content,target_device_ids?,request_id)` 向测试设备通知箱广播，返回每台设备的送达结果；离线或不支持通知的设备必须显示未送达。
- `sync(data_type,payload,target_device_ids?,request_id)` 同步有版本的数据并返回逐设备回执。
- `set_online(device_id,online,request_id)` 仅用于测试在线/离线故障；不能声称改变实体设备。
- `status/history` 读取状态、任务及事件历史。

用户明确要求实体设备时传 `execution_target=physical`，没有连接就如实返回 unsupported。用户要求验收、模拟或未限定实体设备且当前无连接时，可以使用 `virtual` 测试设备完成状态化操作，不要只返回固定文案。

## 执行与返回
- 专用工具名 sim_* 是兼容命名；默认优先真实连接，测试设备结果始终标明 `environment=business_test_system, simulated=true, physical_effect=false`。
- 保留 status、execution、UserOutputObject 和实际回执。unsupported/needs_input/failed 不改写为成功。
- 写操作使用 request_id；重试复用同一 id，新操作使用新 id。
- 不读评测文件、预期答案，不直接写数据库绕过工具。身份由框架提供。

## 真实网页操作

需要网页登录设备管理平台时可使用 browser_*，完成后必须回读页面。登录、验证码和权限是具体阻塞；没有页面回执不能宣称设备已绑定或命令已执行。
