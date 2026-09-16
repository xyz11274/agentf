---
name: user-memory
description: 用户偏好记忆（mock，固定返回示例偏好）。读取/记录用户偏好与习惯（如"我喜欢科幻片"、"记住我不吃辣"）。用户要求记住偏好、或回答需要参考历史偏好时 dispatch 本 skill。
version: 1.0.0
---

# User Memory（mock）

记忆垂域 skill。固定返回示例偏好数据，不接真实记忆存储。

## 自带工具

- `user_memory`（scripts/user_memory.py）：读写用户偏好

## user_memory 参数

```json
{
  "action": "read",       // 必填：read（读取偏好）/ write（记录偏好）
  "key": "电影偏好",       // 可选：偏好项名称
  "value": "科幻"          // 可选：write 时的偏好值
}
```

## 行为

- 始终返回 `{"status": "succeeded", ...}`；read 返回固定示例偏好，write 返回固定记录成功回执
- 不做真实持久化
