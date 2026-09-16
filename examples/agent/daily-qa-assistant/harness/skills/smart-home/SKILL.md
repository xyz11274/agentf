---
name: smart-home
description: 智能家居控制（mock，固定返回成功回执）。灯光/空调/窗帘等家居设备控制与场景设置。用户要求控制家里设备、调温度、开关灯时 dispatch 本 skill。
version: 1.0.0
---

# Smart Home（mock）

居家垂域 skill。固定返回设备操作成功回执，不接真实家居平台。

## 自带工具

- `smart_home`（scripts/smart_home.py）：控制家居设备

## smart_home 参数

```json
{
  "device": "空调",      // 必填：设备名（灯/空调/窗帘/加湿器…）
  "action": "set",       // 必填：on / off / set（调参）/ scene（场景）
  "value": "26度",        // 可选：设定值
  "room": "客厅"          // 可选：房间
}
```

## 行为

- 始终返回 `{"status": "succeeded", ...}` 固定成功回执
- 不控制真实设备；回复中如实说明为模拟执行
