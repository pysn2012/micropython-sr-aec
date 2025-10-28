# AEC 打断测试脚本使用说明

## 🚀 快速开始

### 1. 烧录固件
```bash
cd /Users/renzhaojing/gitcode/renhejia/micropython-sr-aec/ports/esp32
idf.py build flash
```

### 2. 上传脚本
使用 Thonny 将 `test_logic.py` 上传到设备的 `/flash/` 目录

### 3. 修改 WiFi 配置（可选）
打开 `test_logic.py`，修改：
```python
WIFI_SSID = "你的WiFi"
WIFI_PASSWORD = "你的密码"
```

### 4. 运行测试
在 Thonny Shell 或串口监视器中：
```python
import test_logic
sensor = test_logic.SensorSystem()
sensor.run()
```

## 📋 测试说明

### 测试流程
1. ✅ 自动连接 WiFi
2. ✅ 流式下载并播放测试音频（6秒）
3. ✅ 播放期间说 **"嗨小乐"** 可以打断
4. ✅ 打断后开始录音
5. ✅ 说完话停止 1.5 秒自动结束录音
6. ✅ 重新播放音频
7. ✅ 循环 10 轮

### 成功标准
- 播放音频时说 "嗨小乐" 能立即停止播放
- 日志中出现 "🛑 检测到唤醒词打断！"
- 打断后能正常录音
- 检测到静音后自动停止录音
- 能重复多轮测试

## 🆕 v2.0 流式播放版

### 主要改进
- ✅ **解决内存溢出**：不再一次性下载整个文件
- ✅ **边下载边播放**：内存占用从 ~200KB 降低到 <8KB
- ✅ **支持大文件**：可以播放数 MB 的音频文件
- ✅ **网络容错**：连接失败会自动重试下一轮

### 技术实现
```python
# v1.0 - 预下载（内存不足）
audio_data = download_entire_file(url)  # 需要 ~200KB 内存
play(audio_data)

# v2.0 - 流式播放（节省内存）
socket = stream_from_url(url)           # 只需要 ~8KB 缓冲区
while data := socket.recv(4096):        # 边下载边播放
    play(data)
```

## 📊 预期日志

### 成功打断示例
```
🎵 播放线程启动（流式播放 + AEC 打断）
📡 播放进度: 0.0% (0/192044)
📡 播放进度: 21.3% (40960/192044)
📡 播放进度: 42.6% (81920/192044)

--- 用户说 "嗨小乐" ---

🛑🛑🛑🛑🛑🛑🛑🛑🛑🛑🛑🛑🛑🛑🛑
🛑 检测到唤醒词打断！  ← 成功！
🛑🛑🛑🛑🛑🛑🛑🛑🛑🛑🛑🛑🛑🛑🛑

🤖 检测到打断，录音模式保持开启

✅ 检测到打断，开始录音...
🎤 录音中... 能量: 2500000
🎤 录音中... 能量: 3100000
🔇 检测到静音，结束录音
📊 录音统计: 时长: 3.45s, 数据: 55296 字节

🔄 准备下一轮播放...
```

## ❌ 常见问题

### 内存不足
**问题已解决！** v2.0 已改用流式播放

### 无法打断
检查：
1. `modespsr.c` 中队列大小是否为 10
2. `test_logic.py` 中 `interrupt_check_interval` 是否为 1
3. 麦克风是否正常工作

### WiFi 连接失败
修改脚本开头的 WiFi 配置：
```python
WIFI_SSID = "你的WiFi名称"
WIFI_PASSWORD = "你的WiFi密码"
```

## 📚 详细文档

- `AEC打断测试指南.md` - 完整测试指南
- `打断功能失败根因分析.md` - 问题分析
- `最终修复总结.md` - 修复清单

## 🔧 参数调整

### 调整测试轮数
修改 `test_logic.py` 的 `run()` 方法：
```python
tester.run_test_loop(TEST_AUDIO_URL, max_loops=20)  # 改为 20 轮
```

### 调整静音检测
修改 `record_until_silence()` 方法：
```python
SILENCE_THRESHOLD = 1500000      # 静音阈值（增大 = 更难触发）
MIN_SILENCE_DURATION = 2.0       # 静音时长（秒）
MAX_RECORD_TIME = 15             # 最大录音时长（秒）
```

### 更换测试音频
修改脚本开头的 URL：
```python
TEST_AUDIO_URL = "https://你的音频URL.wav"
```

---

**最后更新**: 2025-10-27  
**版本**: v2.0（流式播放版）

