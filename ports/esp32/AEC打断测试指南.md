# AEC 打断功能测试指南（流式播放版）

## 📋 测试目的

验证 AEC (Acoustic Echo Cancellation) 打断功能是否正常工作：
- 播放音频期间能否检测到唤醒词
- 检测到唤醒词后能否立即打断播放
- 打断后能否正常录音
- 能否循环重复此流程

## 🆕 流式播放优势

**v2.0 改进**：采用流式下载播放，解决内存不足问题
- ✅ 不需要一次性下载整个音频文件
- ✅ 边下载边播放，内存占用极低（<8KB 缓冲区）
- ✅ 支持播放大文件音频（数 MB）
- ✅ 网络中断会自动重试下一轮

---

## 🔧 准备工作

### 1. 编译并烧录固件

```bash
cd /Users/renzhaojing/gitcode/renhejia/micropython-sr-aec/ports/esp32
idf.py build
idf.py flash
```

### 2. 上传测试脚本

将 `test_logic.py` 上传到设备的 `/flash/` 目录：

**方法 A: 使用 Thonny（推荐）**
1. 打开 Thonny IDE
2. 连接到 ESP32 设备
3. 打开 `modules/test_logic.py`
4. 另存为到设备的 `/flash/test_logic.py`

**方法 B: 使用 ampy**
```bash
pip install adafruit-ampy
ampy --port /dev/ttyUSB0 put modules/test_logic.py /flash/test_logic.py
```

**方法 C: 使用 mpremote**
```bash
pip install mpremote
mpremote connect /dev/ttyUSB0 fs cp modules/test_logic.py :/flash/test_logic.py
```

### 3. 配置 WiFi（可选）

如果需要修改 WiFi 信息，编辑 `test_logic.py` 开头的配置：
```python
WIFI_SSID = "你的WiFi名称"
WIFI_PASSWORD = "你的WiFi密码"
```

---

## 🚀 运行测试

### 方法 1: 使用 Thonny（推荐）

1. 打开 Thonny IDE
2. 连接到 ESP32 设备
3. 打开设备上的 `/flash/test_logic.py`
4. 点击运行按钮，或在 Shell 中输入：
```python
import test_logic
sensor = test_logic.SensorSystem()
sensor.run()
```

### 方法 2: 使用串口监视器

连接串口监视器：
```bash
idf.py monitor
```

在 REPL 中运行：
```python
>>> import sys
>>> sys.path.append('/flash')
>>> import test_logic
>>> sensor = test_logic.SensorSystem()
>>> sensor.run()
```

**测试流程（流式播放）**：
1. 自动连接 WiFi
2. 流式下载并播放测试音频（6秒，边下载边播放）
3. 播放期间说 "嗨小乐" 可以打断
4. 打断后开始录音
5. 说完话停止 1.5 秒自动结束录音
6. 重新流式播放音频
7. 重复 10 轮

---

## 📊 预期日志

### 正常启动日志

```
==============================================================
AEC 打断功能测试脚本（流式播放）
==============================================================

connectWifi
正在连接到 WiFi: LETIANPAI
network config: ('192.168.110.60', '255.255.255.0', '192.168.110.1', '221.179.155.161')
✅ WiFi 已连接: 192.168.110.60
🔧 初始化 ESP-SR...
✅ ESP-SR 初始化成功

🚀🚀🚀🚀🚀🚀🚀🚀🚀🚀🚀🚀🚀🚀🚀🚀
🚀 开始 AEC 打断功能测试（流式播放）
🚀🚀🚀🚀🚀🚀🚀🚀🚀🚀🚀🚀🚀🚀🚀🚀

测试参数:
  - 音频 URL: https://cdn.file.letianpai.com/internal_tmp/temp_1761552110392235000.wav
  - 最大循环: 10 次
  - 打断检测: 每个音频块
  - 播放模式: 流式下载播放

测试说明:
  1. 播放音频时说 '嗨小乐' 可以打断
  2. 打断后会开始录音
  3. 说完话停止 1.5 秒会自动结束录音
  4. 然后重新播放音频
  5. 按 Ctrl+C 可以停止测试
```

### 播放开始日志（流式）

```
🔄🔄🔄🔄🔄🔄🔄🔄🔄🔄🔄🔄🔄🔄🔄🔄
🔄 第 1/10 轮测试
🔄🔄🔄🔄🔄🔄🔄🔄🔄🔄🔄🔄🔄🔄🔄🔄

📥 流式下载音频: https://cdn.file.letianpai.com/internal_tmp/temp_1761552110392235000.wav
🔗 连接: cdn.file.letianpai.com/internal_tmp/temp_1761552110392235000.wav
📡 读取 HTTP 响应头...
✅ 连接成功，文件大小: 192088 字节
✅ 已跳过 WAV 头

============================================================
🎵 播放线程启动（流式播放 + AEC 打断）
============================================================
🔄 重新启用录音模式（清空缓冲区）...
✅ 录音模式已重新启用
📡 播放进度: 0.0% (0/192044)
📡 播放进度: 21.3% (40960/192044)
📡 播放进度: 42.6% (81920/192044)
```

### 检测到打断日志（关键！）

```
📡 播放进度: 56.8% (110592/192044)

🛑🛑🛑🛑🛑🛑🛑🛑🛑🛑🛑🛑🛑🛑🛑
🛑 检测到唤醒词打断！
🛑🛑🛑🛑🛑🛑🛑🛑🛑🛑🛑🛑🛑🛑🛑

🤖 检测到打断，录音模式保持开启
🎵 播放线程结束

✅ 检测到打断，开始录音...

============================================================
🎤 开始录音（等待用户说话）
============================================================
🎙️ 录音参数:
  - 静音阈值: 1000000
  - 静音时长: 1.5s
  - 最大时长: 10s
🎤 录音中... 能量: 2500000
🎤 录音中... 能量: 3100000
🔇 检测到静音，结束录音
📊 录音统计:
  - 时长: 3.45s
  - 数据: 55296 字节

🔄 准备下一轮播放...
```

### 未检测到打断日志

```
📡 播放进度: 94.6% (184320/192044)
✅ 播放线程正常结束
🎵 播放线程结束

✅ 播放完成，未检测到打断
💤 等待 2 秒后重新播放...
```

---

## ✅ 成功标准

### 必须满足的条件

1. ✅ **播放音频时能检测到打断**
   - 日志中出现 "🛑 检测到唤醒词打断！"

2. ✅ **打断后立即停止播放**
   - 播放进度不再继续增加
   - 日志中出现 "🤖 检测到打断"

3. ✅ **打断后能正常录音**
   - 日志中出现 "🎤 开始录音"
   - 能看到 "🎤 录音中... 能量: XXXX"

4. ✅ **检测到静音后停止录音**
   - 日志中出现 "🔇 检测到静音，结束录音"

5. ✅ **能循环重复测试**
   - 能看到 "🔄 第 2/10 轮测试"
   - 多轮测试都能正常工作

---

## ❌ 问题排查

### 问题 1: 无法检测到打断

**症状**：
- 播放期间说 "嗨小乐"，没有出现打断日志
- 一直播放到结束

**可能原因**：
1. 队列未增大（仍为 1）
2. 检测间隔过大
3. 麦克风未工作
4. AEC 未正确配置

**排查步骤**：

1. **检查队列大小**
   ```bash
   grep "xQueueCreate" ports/esp32/modespsr.c
   # 应该看到：g_result_que = xQueueCreate(10, sizeof(sr_result_t));
   ```

2. **检查检测间隔**
   ```bash
   grep "interrupt_check_interval" ports/esp32/modules/logic.py
   # 应该看到：interrupt_check_interval = 1
   ```

3. **检查麦克风**
   - 在播放前先测试唤醒词检测
   - 确保麦克风硬件正常

4. **查看详细日志**
   - 检查是否有 "detect start" 日志
   - 检查是否有 feed_Task 运行日志

### 问题 2: WiFi 未连接

**症状**：
```
❌ WiFi 未连接，请先连接 WiFi
```

**解决**：
```python
>>> import network
>>> wlan = network.WLAN(network.STA_IF)
>>> wlan.active(True)
>>> wlan.connect('你的WiFi名称', '你的WiFi密码')
>>> # 等待连接
>>> import time
>>> while not wlan.isconnected():
...     time.sleep(1)
>>> print(wlan.ifconfig())
```

### 问题 3: 流式连接失败

**症状**：
```
❌ 流式下载异常: OSError: [Errno 113] EHOSTUNREACH
```

**解决**：
- 检查网络连接
- 检查 URL 是否正确
- 确保 DNS 解析正常
- 尝试 ping cdn.file.letianpai.com

### 问题 4: SSL 证书错误

**症状**：
```
❌ 流式下载异常: SSL handshake failed
```

**解决**：
- 使用 HTTP 代替 HTTPS（修改 URL）
- 或者更新固件中的证书

### 问题 5: 内存不足（已解决）

**v1.0 问题**：
```
❌ 下载异常: memory allocation failed, allocating 51968 bytes
```

**v2.0 解决方案**：
- ✅ 已改用流式播放，不再需要预下载整个文件
- ✅ 内存占用从 ~200KB 降低到 <8KB
- ✅ 支持播放任意大小的音频文件

---

## 🔍 调试技巧

### 1. 查看实时能量值

修改 `test_aec_interrupt.py`，在录音时打印能量值：
```python
print(f"🎤 能量: {energy:.0f}, 阈值: {SILENCE_THRESHOLD}")
```

### 2. 调整静音阈值

如果太容易触发静音，增大阈值：
```python
SILENCE_THRESHOLD = 2000000  # 改大
```

如果不容易触发静音，减小阈值：
```python
SILENCE_THRESHOLD = 500000   # 改小
```

### 3. 查看检测结果

在播放线程中添加日志：
```python
result = espsr.listen(1)
print(f"检测结果: {result}")  # 添加这行
```

### 4. 测试 ESP-SR 是否运行

在 REPL 中：
```python
>>> import espsr
>>> espsr.init()
>>> espsr.listen(1000)  # 等待 1 秒
>>> # 说 "嗨小乐" 看是否返回 "wakeup"
```

---

## 📝 修改参数

### 调整测试轮数

```python
tester.run_test_loop(audio_data, max_loops=20)  # 改为 20 轮
```

### 调整检测间隔

在 `playback_thread_func` 中：
```python
interrupt_check_interval = 2  # 每 2 个块检测一次
```

### 调整录音参数

```python
SILENCE_THRESHOLD = 1500000      # 静音阈值
MIN_SILENCE_DURATION = 2.0       # 静音时长（秒）
MAX_RECORD_TIME = 15             # 最大录音时长（秒）
```

---

## 🎯 测试场景

### 场景 1: 基本打断测试

1. 运行 `run_test()`
2. 等待播放开始（约 1 秒）
3. 说 "嗨小乐"
4. 观察是否立即停止播放
5. 说一句话（如 "今天天气怎么样"）
6. 停止说话 1.5 秒
7. 观察是否重新播放

### 场景 2: 连续打断测试

1. 第一轮播放时打断
2. 录音后等待重新播放
3. 第二轮播放时再次打断
4. 重复多次
5. 验证每次都能正常打断

### 场景 3: 边界测试

1. 播放刚开始时立即打断
2. 播放快结束时打断
3. 打断后不说话（测试超时）
4. 打断后说很长的话（测试最大时长）

---

## 📚 参考文档

- `打断功能失败根因分析.md` - 问题根因分析
- `参考项目AEC实现详解.md` - 参考项目实现
- `最终修复总结.md` - 完整修复清单

---

## 🆘 需要帮助

如果测试仍然失败，请提供以下信息：

1. **完整日志**：从启动到失败的完整日志
2. **固件版本**：编译日期和版本
3. **测试步骤**：具体的操作步骤
4. **预期结果** vs **实际结果**

---

## 更新日期

2025-10-27

