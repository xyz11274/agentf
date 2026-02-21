# Test Data Directory

本目录包含测试相关的数据文件。

## 目录结构

```
data/
├── README.md              # 本文档
├── expected/              # 预期测试输出文件
└── temporary/             # 临时测试数据文件
```

---

## Expected 目录

`expected/` 目录包含预期的测试输出文件，用于验证测试结果的正确性。

### 示例

```
expected/
├── hot_reload_output.txt    # 热更新测试的预期输出
├── shell_script_output.txt    # Shell脚本测试的预期输出
├── python_script_output.txt   # Python脚本测试的预期输出
└── ...
```

---

## Temporary 目录

`temporary/` 目录用于存储测试运行时生成的临时数据文件。

### 使用规则

1. 测试启动时创建临时文件
2. 测试完成后清理临时文件
3. 不要在临时目录中保存长期数据

### 自动清理

```bash
# 测试结束后清理临时文件
rm -rf data/temporary/*
```

---

## 测试数据文件格式

### 预期输出文件格式

```
[TEST_NAME]
test_case_1: expected_value
test_case_2: expected_value
...
```

### 临时数据文件

临时文件使用测试用例名称和时间戳命名：

```
hot_reload_test_20240217_143000.txt
hot_reload_test_20240217_143001.log
...
```

---

## 添加新的预期输出

1. 在 `expected/` 目录中创建对应的输出文件
2. 按照测试用例名称添加预期值
3. 在测试代码中使用这些预期值进行验证

---

## 验证测试输出

在测试代码中使用预期输出进行验证：

```cpp
#include <fstream>
#include <string>
#include "agentflow/basic/logger.h"

// 读取预期输出
std::string read_expected_output(const std::string& test_name) {
    std::ifstream file("data/expected/" + test_name + ".txt");
    std::stringstream buffer;
    buffer << file.rdbuf();
    return buffer.str();
}

TEST(MyTest, VerifyOutput) {
    std::string actual = /* 获取实际输出 */;
    std::string expected = read_expected_output("my_test_output");
    EXPECT_EQ(actual, expected);
}
```
