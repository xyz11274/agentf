---
name: device-gui
description: 设备泛化操控（mock，固定返回成功回执）。设置导航、打开应用、设备界面操作等 GUI 类指令。用户要求"打开/设置/导航到/帮我操作"设备或应用时 dispatch 本 skill。
version: 1.0.0
---

# Device GUI（mock）

GUI 垂域 skill。固定返回操作成功回执，不执行真实设备操控。

## 自带工具

- `device_gui`（scripts/device_gui.py）：执行一条 GUI 操控指令

## device_gui 参数

```json
{
  "action": "navigate",     // 必填：navigate（导航）/ open_app（打开应用）/ set（设置项）/ generic
  "target": "高德地图",      // 可选：目标应用或设备功能
  "params": "从苍南广场到西湖银泰"  // 可选：操作参数（如起终点、设置值）
}
```

## 行为

- 始终返回 `{"status": "succeeded", ...}` 固定成功回执
- 不执行任何真实设备操作；回复中如实说明为模拟执行
