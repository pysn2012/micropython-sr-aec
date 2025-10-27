# ESP-SR MicroPython 集成指南 (参照project-i2s-wakup-new)

## 📋 更新内容 (最新)

### 1. 关键配置调整 ✅

参照成功项目 `project-i2s-wakup-new`，已完成以下调整：

#### 1.1 sdkconfig.board 配置
```ini
# ESP-SR配置 (参照project-i2s-wakup-new)
CONFIG_SR_NSN_WEBRTC=y
CONFIG_SR_VADN_VADNET1_MEDIUM=y  
CONFIG_SR_WN_WN9_HILEXIN=y        # "嗨，乐鑫"唤醒词
CONFIG_SR_MN_CN_MULTINET7_QUANT=y # 中文命令词
CONFIG_SR_MN_EN_NONE=y
```

#### 1.2 分区表配置
```csv
# factory: 2500K (与参考项目一致)
# model: 5460K (略大于参考项目5168K)
factory,  app,  factory, 0x10000, 2500K,
model,    data, spiffs,  ,        5460K, 
```

### 2. modespsr.c 核心改进 ✅

完全参照参考项目实现，关键特性：

#### 2.1 任务架构
- **feed_Task**: 持续从I2S读取音频并喂给AFE
- **detect_Task**: 处理AFE结果，检测唤醒词和命令词
- **队列通信**: 使用 `g_result_que` 传递结果到MicroPython

#### 2.2 I2S配置
```c
// 使用I2S_NUM_0 (与参考项目一致)
i2s_config_t i2s_config = {
    .mode = I2S_MODE_MASTER | I2S_MODE_RX,
    .sample_rate = 16000,
    .bits_per_sample = I2S_BITS_PER_SAMPLE_16BIT,
    .channel_format = I2S_CHANNEL_FMT_ONLY_LEFT,
    // 引脚: SCK=5, WS=4, SD=6
};
```

#### 2.3 命令词表 (14个)
```c
static const char *cmd_phoneme[14] = {
    "da kai kong qi jing hua qi",  // 0: 打开空气净化器
    "guan bi kong qi jing hua qi", // 1: 关闭空气净化器
    "da kai tai deng",             // 2: 打开台灯
    "guan bi tai deng",            // 3: 关闭台灯
    "tai deng tiao liang",         // 4: 台灯调亮
    "tai deng tiao an",            // 5: 台灯调暗
    "da kai deng dai",             // 6: 打开等待
    "guan bi deng dai",            // 7: 关闭等待
    "bo fang yin yue",             // 8: 播放音乐
    "ting zhi bo fang",            // 9: 停止播放
    "da kai shi jian",             // 10: 打开时间
    "da kai ri li",                // 11: 打开日历
    "xiao le xiao le",             // 12: 小乐小乐
    "hai xiao le"                  // 13: 嗨小乐
};
```

### 3. Python接口优化 ✅

#### 3.1 返回值类型
- `"wakeup"`: 检测到唤醒词"嗨，乐鑫"
- `{"id": N, "command": "xxx"}`: 检测到命令词
- `"timeout"`: 监听超时
- `"not_initialized"`: 模块未初始化

#### 3.2 使用示例
```python
import espsr

# 初始化
espsr.init()

# 监听循环
while True:
    result = espsr.listen(10)  # 10秒超时
    
    if result == "wakeup":
        print("检测到唤醒词!")
    elif isinstance(result, dict):
        command_id = result["id"]
        command_text = result["command"]
        print(f"命令: {command_text} (ID: {command_id})")
    elif result == "timeout":
        print("监听超时")
```

### 4. 构建和测试 🚀

#### 4.1 构建固件
```bash
cd /Users/renzhaojing/gitcode/renhejia/micropython-sr/ports/esp32
make BOARD=ESP32_GENERIC_S3 clean
make BOARD=ESP32_GENERIC_S3
```

#### 4.2 烧录固件
```bash
make BOARD=ESP32_GENERIC_S3 deploy PORT=/dev/cu.usbserial-*
```

#### 4.3 测试语音识别
```python
# 在MicroPython REPL中运行
exec(open('logic.py').read())
```

### 5. 硬件连接 🔌

#### 5.1 INMP441 麦克风
```
ESP32-S3    INMP441
GPIO 5  →   SCK
GPIO 4  →   WS
GPIO 6  →   SD
3.3V    →   VDD
GND     →   GND
```

#### 5.2 GPIO 4 脉冲输出
- 唤醒词检测时: HIGH 500ms
- 命令词检测时: HIGH 500ms

### 6. 预期效果 🎯

#### 6.1 唤醒流程 (跳过WakeNet模式)
1. 说 **"嗨小乐"** → 返回 `{"id": 0, "command": "hai xiao le"}` (当作唤醒)，GPIO4脉冲
2. 说其他命令词 → 返回 `{"id": N, "command": "xxx"}`，GPIO4脉冲
3. 连续监听模式，无超时，持续检测

#### 6.2 命令词示例
- **"嗨小乐"** → `{"id": 0, "command": "hai xiao le"}` (唤醒词)
- **"打开台灯"** → `{"id": 3, "command": "da kai tai deng"}`
- **"小乐小乐"** → `{"id": 13, "command": "xiao le xiao le"}`

### 7. 故障排除 🔧

#### 7.1 常见问题
1. **模型未找到**: 确认 `model` 分区和文件完整
2. **I2S无数据**: 检查麦克风连接和引脚配置
3. **无唤醒响应**: 验证 `CONFIG_SR_WN_WN9_HILEXIN=y`
4. **命令词不识别**: 确认 `CONFIG_SR_MN_CN_MULTINET7_QUANT=y`

#### 7.2 调试方法
```python
# 查看支持的命令词
commands = espsr.get_commands()
print(commands)

# 监听前检查初始化
result = espsr.init()
print(f"Init result: {result}")
```

### 8. 模型文件验证 ✅

确认以下目录完整：
```
ports/esp32/components/esp-sr/model/
├── wakenet_model/wn9_hilexin/     # "嗨，乐鑫"唤醒词
├── multinet_model/mn7_cn/         # 中文命令词
├── multinet_model/fst/            # FST语法
└── multinet_model/srmodels.bin    # 模型二进制
```

---

## 📝 更新记录

- **2024-XX-XX**: 完全参照 `project-i2s-wakup-new` 重构
- **配置对齐**: 添加 `CONFIG_SR_NSN_WEBRTC=y`
- **代码重构**: 使用 I2S_NUM_0，实现双任务架构
- **接口优化**: 标准化返回值，支持命令词详情
- **测试验证**: 与成功项目保持一致的实现逻辑 