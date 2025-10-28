# AEC 诊断日志指南

## 📅 日期
2025-10-27 (v2.5 - 诊断版本)

---

## 🎯 目的

当前 AEC 完全无效（播放立即被 VAD 打断），需要通过详细日志诊断**根本原因**。

---

## ✅ v2.5 已添加的诊断日志

### 1. C 代码诊断 (`modespsr.c`)

#### 日志 A: feed_reference 被调用情况
```c
📤 feed_reference 被调用 1 次，本次 4096 字节 (2048 采样点)
📤 feed_reference 被调用 51 次，本次 4096 字节 (2048 采样点)
📤 feed_reference 被调用 101 次，本次 4096 字节 (2048 采样点)
```

**说明**:
- 统计 Python 调用 `espsr.feed_reference()` 的次数
- 每 50 次打印一次
- **如果没有这个日志**，说明 Python 没有调用

#### 日志 B: AEC 参考信号状态
```c
🔍 AEC诊断 [Feed #100]: ref_active=1, timeout=5 ms, activity=45.2% (21600/47800), active_feeds=100/100
🔍 AEC诊断 [Feed #200]: ref_active=1, timeout=8 ms, activity=46.1% (44100/95600), active_feeds=200/200
🔍 AEC诊断 [Feed #300]: ref_active=0, timeout=150 ms, activity=46.5% (66600/143400), active_feeds=200/300
```

**参数说明**:
- `Feed #100`: 第 100 次 AFE feed 调用
- `ref_active=1`: 参考信号活跃（0=超时，1=活跃）
- `timeout=5 ms`: 距离上次 feed_reference 的时间
- `activity=45.2%`: 非零采样点占比（音频有多少不是静音）
- `(21600/47800)`: 非零采样点数 / 总采样点数
- `active_feeds=100/100`: 活跃的 feed 次数 / 总 feed 次数

### 2. Python 代码诊断 (`test_logic.py`)

#### 日志 C: Python 喂参考信号
```python
🔊 Python: 已喂参考信号 1 次，4096 字节
🔊 Python: 已喂参考信号 51 次，4096 字节
🔊 Python: 已喂参考信号 101 次，4096 字节
```

**说明**:
- 每 50 次打印一次
- 确认 Python 确实在调用 `espsr.feed_reference()`

---

## 🔍 诊断场景

### 场景 1: Python 没有调用 feed_reference

**症状**:
```
📡 播放进度: 2.3% (4096/179042)

--- 没有 "🔊 Python: 已喂参考信号" 的日志 ---

🗣️ 检测到语音活动打断！（VAD）
```

**C 端日志**:
```
--- 没有 "📤 feed_reference 被调用" 的日志 ---

🔍 AEC诊断 [Feed #100]: ref_active=0, timeout=999999 ms, activity=0.0% (0/0), active_feeds=0/100
```

**说明**:
- `timeout=999999 ms`: 从未调用过 feed_reference
- `activity=0.0%`: 没有参考信号数据
- `active_feeds=0/100`: 0 次活跃

**原因**:
- `espsr.feed_reference` 函数未正确导出到 MicroPython
- 或 Python 代码路径有问题

**解决**:
检查模块注册代码。

---

### 场景 2: Python 调用了但 C 端未收到

**症状**:
```
🔊 Python: 已喂参考信号 1 次，4096 字节  ← Python 说调用了
🔊 Python: 已喂参考信号 51 次，4096 字节

--- 但 C 端没有 "📤 feed_reference 被调用" 的日志 ---

🔍 AEC诊断 [Feed #100]: ref_active=0, timeout=999999 ms, activity=0.0% (0/0), active_feeds=0/100
```

**说明**:
- Python 调用了，但 C 函数内部逻辑有问题
- 可能是 `espsr_initialized` 检查失败
- 或锁获取失败

**原因**:
```c
if (!espsr_initialized) {
    return mp_const_false;  // 但没有打印警告
}
```

**解决**:
检查 `espsr_initialized` 状态。

---

### 场景 3: 参考信号被写入但立即超时

**症状**:
```
🔊 Python: 已喂参考信号 1 次，4096 字节
📤 feed_reference 被调用 1 次，本次 4096 字节 (2048 采样点)  ← C 收到了

🔍 AEC诊断 [Feed #100]: ref_active=0, timeout=150 ms, activity=0.0% (0/0), active_feeds=0/100
                                       ↑ 虽然收到过，但已超时
```

**说明**:
- feed_reference 被调用了，但很快就超时（>100ms）
- 说明**喂入频率太低**

**原因**:
- Python 喂入速率慢于 AFE 消耗速率
- 播放线程可能有长时间阻塞

**解决**:
- 检查 `self.audio_out.write()` 是否阻塞太久
- 增加 `REFERENCE_TIMEOUT_MS` 到 200ms 或 300ms

---

### 场景 4: 参考信号活跃但全是零

**症状**:
```
🔊 Python: 已喂参考信号 51 次，4096 字节
📤 feed_reference 被调用 51 次，本次 4096 字节 (2048 采样点)

🔍 AEC诊断 [Feed #200]: ref_active=1, timeout=5 ms, activity=0.0% (0/47800), active_feeds=200/200
                                                             ↑ 全是 0！
```

**说明**:
- 参考信号被正确写入且活跃
- 但所有采样点都是 0（静音）

**原因**:
- WAV 数据可能有偏移（跳过头部时有问题）
- 或播放音频本身就是静音

**解决**:
检查 WAV 头部跳过逻辑。

---

### 场景 5: 参考信号正常但 AEC 仍无效

**症状**:
```
🔊 Python: 已喂参考信号 51 次，4096 字节
📤 feed_reference 被调用 51 次，本次 4096 字节 (2048 采样点)

🔍 AEC诊断 [Feed #200]: ref_active=1, timeout=8 ms, activity=45.2% (21600/47800), active_feeds=200/200
                                                             ↑ 参考信号正常！

--- 但播放仍然被立即打断 ---

🗣️ 检测到语音活动打断！（VAD）
```

**说明**:
- 参考信号被正确喂入
- 数据不是静音（activity > 40%）
- 时序也正常（timeout < 10ms）
- **但 AEC 仍然无效**

**可能原因**:
1. **双通道交错格式错误** - 最可能
2. **AEC 模式配置问题**
3. **时序延迟（参考信号与播放不同步）**
4. **ESP-SR 版本或模型问题**

**下一步诊断**:
- 对比参考项目的 `Feed` 实现
- 检查是否需要延迟补偿
- 尝试不同的 `afe_config` 参数

---

## 🚀 测试步骤

### 1. 编译并烧录

```bash
cd /Users/renzhaojing/gitcode/renhejia/micropython-sr-aec/ports/esp32
idf.py build flash monitor
```

### 2. 运行测试

```python
import test_logic
sensor = test_logic.SensorSystem()
sensor.run()
```

### 3. 观察日志

**关键检查点**:

#### ✅ 检查点 1: Python 调用
```
🔊 Python: 已喂参考信号 1 次，4096 字节  ← 必须有
🔊 Python: 已喂参考信号 51 次，4096 字节
```

#### ✅ 检查点 2: C 端接收
```
📤 feed_reference 被调用 1 次，本次 4096 字节 (2048 采样点)  ← 必须有
📤 feed_reference 被调用 51 次，本次 4096 字节 (2048 采样点)
```

#### ✅ 检查点 3: AEC 状态
```
🔍 AEC诊断 [Feed #100]: ref_active=1, timeout=5 ms, activity=45.2% (21600/47800), active_feeds=100/100
                       ↑ 必须=1    ↑ 必须<100    ↑ 必须>30%      ↑ 必须接近100%
```

**如果都满足，但 AEC 仍无效**，问题在于：
- 双通道交错格式
- 或 ESP-SR 内部配置

---

## 📊 日志模板（用于反馈）

请复制以下日志发送给我：

```
========== 播放开始 ==========

✅ 录音模式已重新启用
📡 播放进度: 2.3% (4096/179042)

--- Python 日志 ---
🔊 Python: ... (如果有)

--- C 端 feed_reference 日志 ---
📤 feed_reference ... (如果有)

--- C 端 AEC 诊断日志 ---
🔍 AEC诊断 [Feed #100]: ... (如果有)

--- 打断情况 ---
🗣️ 检测到语音活动打断！（VAD）  (或完整播放)

========== 播放结束 ==========
```

---

## 🔧 下一步根据日志决定

### 如果没有 Python 日志
→ 检查 `espsr.feed_reference` 导出

### 如果 Python 有但 C 没有
→ 检查 `espsr_initialized` 或锁问题

### 如果 C 有但超时
→ 增加 `REFERENCE_TIMEOUT_MS` 或优化播放线程

### 如果全是零
→ 检查 WAV 头部跳过逻辑

### 如果一切正常但 AEC 无效
→ **双通道格式问题**，需要对比参考项目

---

**更新日期**: 2025-10-27  
**版本**: v2.5 (诊断版本)  
**状态**: ✅ 等待用户测试并提供日志

