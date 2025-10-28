# AEC 参考信号喂入频率修复说明

## 📅 日期
2025-10-27 (v2.6 - 关键修复)

---

## ❌ 问题分析

### 诊断日志显示的问题
```
[feed_Task] 🔍 Feed#100: ref_active=1, timeout=17 ms, activity=97.5%, active_feeds=2/100
                                                                            ↑ 只有 2%！

[feed_Task] 🔍 Feed#400: ref_active=1, timeout=16 ms, activity=99.7%, active_feeds=38/400
                                                                             ↑ 只有 9.5%！
```

**说明**：
- ✅ 参考信号数据正常：`activity=97.5%`（非零采样点占比高）
- ✅ 时序正常：`timeout=17 ms`（< 100ms）
- ❌ **喂入频率太低**：`active_feeds=2/100` 只有 2% 的 feed 有参考信号！

### 根本原因：速率不匹配

| 组件 | 频率 | 每次处理 | 说明 |
|------|------|----------|------|
| **AFE feed_Task** | **~33 次/秒** | **480 采样点** | 每 30ms 消耗 960 字节 |
| **Python 喂入（修复前）** | **~8 次/秒** | **2048 采样点** | 每收到 4096 字节网络包才喂一次 |

**问题**：
1. AFE 每秒需要 ~31KB 参考信号（33 × 960）
2. Python 每秒喂入 ~32KB（8 × 4096），**但不连续**
3. `I2S.write(4096)` 阻塞 ~128ms
4. 导致 90% 的时间缓冲区是空的
5. **AEC 收到的大部分是 0，无法工作**

---

## ✅ v2.6 修复方案

### 核心思路
**增加喂入频率，匹配 AFE 消耗速率**：将大块分成小块，每 30ms 喂入一次。

### 修复代码

**文件**: `test_logic.py` 的 `playback_stream_func` 函数

```python
# 修复前：一次性喂入和播放大块
espsr.feed_reference(bytes(audio_chunk))  # 4096 字节
self.audio_out.write(audio_chunk)         # 阻塞 ~128ms

# 修复后：分成小块，每 30ms 喂入一次
FEED_CHUNK_SIZE = 960  # 480 采样点 = 30ms @ 16kHz
offset = 0

while offset < len(audio_chunk):
    mini_chunk = audio_chunk[offset:offset + FEED_CHUNK_SIZE]
    
    # 喂入参考信号（每 30ms 一次）
    espsr.feed_reference(bytes(mini_chunk))
    
    # 播放这个小块（阻塞 ~30ms）
    self.audio_out.write(mini_chunk)
    
    offset += FEED_CHUNK_SIZE
```

### 效果对比

| 指标 | 修复前 | 修复后 |
|------|--------|--------|
| 喂入频率 | ~8 次/秒 | ~33 次/秒 |
| 每次喂入 | 4096 字节 | 960 字节 |
| 阻塞时间 | ~128ms | ~30ms |
| AFE 缓冲区活跃度 | ~2% | **~95%+** |
| AEC 效果 | ❌ 无效 | ✅ **应该有效** |

---

## 🔍 预期测试结果

### 成功标志

```
[feed_Task] 🔍 Feed#100: ref_active=1, timeout=5 ms, activity=98%, active_feeds=95/100
                                                                          ↑ 应该 >90%！
```

**关键指标**：
- `active_feeds` 应该接近 100（~95%）
- `timeout` 应该 < 50ms（大部分时间 < 20ms）
- `activity` 应该 > 90%（非零采样点）

### 如果 AEC 生效

```
📡 播放进度: 2.3% (4096/179042)
📡 播放进度: 23.6% (45056/179042)
📡 播放进度: 44.9% (86016/179042)
📡 播放进度: 66.2% (127072/179042)
📡 播放进度: 87.5% (168128/179042)
✅ 播放线程正常结束  ← 完整播放，无自我打断！
```

### 如果 AEC 仍无效

即使 `active_feeds` 达到 95%，但仍然被立即打断，说明：
- **双通道交错格式问题**
- **或需要参考信号延迟补偿**

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

### 3. 观察关键日志

#### 日志 A: 喂入频率
```
🔊 Python: 已喂参考信号 1 次（每次 960 字节）
🔊 Python: 已喂参考信号 51 次（每次 960 字节）
```
- 应该看到喂入次数快速增长

#### 日志 B: AFE 状态
```
[feed_Task] 🔍 Feed#100: ref_active=1, timeout=X ms, activity=Y%, active_feeds=Z/100
```
- `Z` 应该 > 90
- `X` 应该 < 50
- `Y` 应该 > 90

#### 日志 C: 播放结果
- **如果完整播放** → AEC 成功！🎉
- **如果立即打断** → 需要进一步调整

---

## 🔧 如果仍然失败

### 方案 A: 调整喂入块大小

```python
# 尝试更小的块（15ms）
FEED_CHUNK_SIZE = 480  # 240 采样点 = 15ms

# 或更大的块（60ms，但可能还是不够频繁）
FEED_CHUNK_SIZE = 1920  # 960 采样点 = 60ms
```

### 方案 B: 增大参考缓冲区

**C 代码** (`modespsr.c`):
```c
// 从 2 秒增加到 5 秒
#define REFERENCE_BUFFER_SIZE (16000 * 5)
```

### 方案 C: 调整超时时间

```c
// 从 100ms 增加到 200ms
#define REFERENCE_TIMEOUT_MS 200
```

### 方案 D: 检查双通道格式

如果 `active_feeds` 达到 95% 但 AEC 仍无效，需要：
1. 对比参考项目的 `Feed` 实现
2. 检查双通道交错格式是否正确
3. 可能需要添加延迟补偿

---

## 📖 技术细节

### 为什么是 960 字节？

```
采样率：16kHz
AFE feed 间隔：30ms
每次 feed 需要：16000 × 0.03 = 480 采样点
每采样点：2 字节（int16_t）
总字节数：480 × 2 = 960 字节
```

### 环形缓冲区工作原理

```
Python 写入              AFE 读取
    ↓                       ↓
[--W-----R-------------]  缓冲区
   ↑     ↑
写指针  读指针

- Python 每 30ms 写入 480 采样点
- AFE 每 30ms 读取 480 采样点
- 速率匹配 → 缓冲区稳定
```

### active_feeds 计算

```c
g_feed_count++;                      // 总 feed 次数
if (ref_active) g_ref_active_feeds++; // 有参考信号的次数

// active_feeds = g_ref_active_feeds / g_feed_count
// 理想值：~95%（不是100%因为有网络延迟和启动时间）
```

---

## 🎯 成功标准

### 完全成功
- `active_feeds` > 90%
- 播放完整音频（~6秒）无自我打断
- 真实说话能正确触发打断

### 部分成功
- `active_feeds` > 90%
- 但播放1-2秒后仍被打断
- → 说明喂入频率已解决，需要调整 AEC 其他参数

### 仍然失败
- `active_feeds` < 50%
- → 说明喂入策略还需优化

---

**更新日期**: 2025-10-27  
**版本**: v2.6 (喂入频率修复)  
**状态**: ✅ 代码已修改，等待用户测试

