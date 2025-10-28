# AEC VOIP模式切换和问题诊断指南

## 📅 更新日期
2025-10-27 (v2.4)

---

## ❌ 问题现状

### 用户反馈
1. ✅ 降噪配置已添加，但**不生效**
2. ✅ AEC预热机制**没有意义**：过了10个块后还是会被自己的喇叭声音打断
3. ✅ 建议使用 `AEC_MODE_VOIP_HIGH_PERF` 模型

### 根本原因
**AEC本身没有正确工作**，预热机制只是掩盖问题，不是解决方案。

---

## ✅ v2.4 关键修复

### 修复 1: 切换到 VOIP 模式

**C 代码** (`modespsr.c`，已修改)：

```c
// 之前：
afe_config->aec_mode = AEC_MODE_SR_HIGH_PERF;  // ❌ SR模式

// 现在：
afe_config->aec_mode = AEC_MODE_VOIP_HIGH_PERF;  // ✅ VOIP模式（参考项目）
```

**两种模式的区别**：

| 特性 | SR_HIGH_PERF | VOIP_HIGH_PERF |
|------|--------------|----------------|
| 用途 | 语音识别 | 语音通话 |
| AEC强度 | 中等 | **更强** |
| 回声消除 | 针对短暂交互 | 针对持续对话 |
| 参考项目使用 | ❌ | ✅ |

### 修复 2: 移除AEC预热机制

**Python 代码** (`test_logic.py`，已修改)：

```python
# 移除了:
# aec_warmup_chunks = 10
# if data_count >= aec_warmup_chunks:

# 现在：直接检测VAD
is_speaking = espsr.check_vad()
if is_speaking:
    # 打断
    break
```

**原因**：
- ✅ 如果AEC正确工作，不需要预热
- ❌ 如果AEC不工作，预热也没用

---

## 🔍 编译后检查关键日志

### 1. 降噪模型加载

**期望日志**：
```
I (xxxx) espsr: NS model found: nsnet1_quantized  ← 必须看到
I (xxxx) espsr: NS enabled with model: nsnet1_quantized  ← 必须看到
I (xxxx) espsr: AFE config: format=MR, aec_init=true, aec_mode=2, ns_init=true, vad_init=true
                                                        ↑ AEC_MODE_VOIP_HIGH_PERF = 2
```

**❌ 如果看到**：
```
W (xxxx) espsr: NS model not found, noise suppression will be disabled
W (xxxx) espsr: NS disabled (model not found)
I (xxxx) espsr: AFE config: format=MR, aec_init=true, aec_mode=2, ns_init=false, vad_init=true
```

说明**降噪模型文件缺失**，需要检查ESP-SR组件。

### 2. AFE通道数验证

**期望日志**：
```
I (xxxx) espsr: AFE feed channels: 2 (expected: 2 for MR)  ← 必须是2通道
```

**❌ 如果是1通道**：
```
I (xxxx) espsr: AFE feed channels: 1 (expected: 2 for MR)  ← 错误！
```

说明 AEC 配置失败，可能原因：
- `afe_config_init("MR", ...)` 未生效
- 模型不支持双通道

### 3. 测试时的日志

**正确的流程**：
```
✅ 录音模式已重新启用
📡 播放进度: 2.3% (4096/179042)
📡 播放进度: 23.6% (45056/179042)
📡 播放进度: 44.9% (86016/179042)
...
✅ 播放线程正常结束  ← 如果AEC工作正常，应该完整播放
```

**AEC仍然失效**：
```
✅ 录音模式已重新启用
📡 播放进度: 2.3% (4096/179042)

🗣️🗣️🗣️🗣️🗣️🗣️🗣️🗣️🗣️🗣️🗣️
🗣️ 检测到语音活动打断！（VAD）  ← 立即被自己的声音打断
🗣️🗣️🗣️🗣️🗣️🗣️🗣️🗣️🗣️🗣️🗣️
```

---

## 🔧 问题诊断步骤

### 步骤 1: 编译并查看初始化日志

```bash
cd /Users/renzhaojing/gitcode/renhejia/micropython-sr-aec/ports/esp32
idf.py build flash monitor
```

**检查项**：
- [ ] 是否有 `NS model found`？
- [ ] `aec_mode` 是否等于 2（VOIP模式）？
- [ ] `AFE feed channels` 是否等于 2？

### 步骤 2: 如果降噪模型未找到

**可能原因**：
1. ESP-SR 组件没有包含降噪模型文件
2. `esp_srmodel_filter(models, ESP_NSNET_PREFIX, NULL)` 返回 NULL

**临时禁用降噪（测试AEC本身）**：

修改 `modespsr.c`:
```c
// 强制禁用NS，先测试AEC
afe_config->ns_init = false;
```

重新编译测试，如果AEC仍然不工作，说明问题不在降噪。

### 步骤 3: 验证参考信号是否被正确喂入

**在 `feed_Task` 中添加调试日志**（临时）：

```c
// 在 feed_Task 中添加
static uint32_t feed_debug_count = 0;
feed_debug_count++;

if (feed_debug_count % 100 == 0) {  // 每100次打印一次
    int64_t time_diff_ms = (esp_timer_get_time() - g_last_reference_time_us) / 1000;
    ESP_LOGI(TAG, "Feed #%u: ref_timeout=%lld ms, has_ref=%d", 
        feed_debug_count, time_diff_ms, (time_diff_ms <= REFERENCE_TIMEOUT_MS));
}
```

**期望日志**（播放时）：
```
I (xxxx) espsr: Feed #100: ref_timeout=5 ms, has_ref=1  ← 参考信号活跃
I (xxxx) espsr: Feed #200: ref_timeout=8 ms, has_ref=1
I (xxxx) espsr: Feed #300: ref_timeout=150 ms, has_ref=0  ← 播放结束，超时清零
```

**❌ 如果始终看到**：
```
I (xxxx) espsr: Feed #100: ref_timeout=5000 ms, has_ref=0  ← 参考信号从未喂入！
```

说明 `espsr.feed_reference()` **没有被Python调用**，或者调用失败。

### 步骤 4: 检查 Python 调用

在 `test_logic.py` 中添加调试：

```python
# 在 playback_stream_func 中
ref_feed_count = 0

while received_bytes < total_size:
    audio_chunk = audio_socket.recv(to_read)
    
    try:
        espsr.feed_reference(bytes(audio_chunk))
        ref_feed_count += 1
        if ref_feed_count % 50 == 1:
            print(f"🔊 已喂参考信号 {ref_feed_count} 次")
    except Exception as e:
        print(f"❌ 喂参考信号失败: {e}")
```

**期望输出**：
```
🔊 已喂参考信号 1 次
🔊 已喂参考信号 51 次
🔊 已喂参考信号 101 次
```

**❌ 如果看到**：
```
❌ 喂参考信号失败: AttributeError: 'module' object has no attribute 'feed_reference'
```

说明 **MicroPython模块注册失败**，`feed_reference` 未导出。

---

## 🎯 可能的根本原因

### 原因 1: 双通道交错有问题

**当前实现** (`modespsr.c`):
```c
for (int i = 0; i < feed_chunksize; i++) {
    feed_buff[i * 2] = mic_data[i];          // 通道0：麦克风
    feed_buff[i * 2 + 1] = g_reference_buffer[...];  // 通道1：参考
}
```

**参考项目可能用的是不同的格式**：
- 可能是 `[L0,L1,L2...R0,R1,R2...]`（平面格式）
- 而不是 `[L0,R0,L1,R1,L2,R2...]`（交错格式）

**验证方法**：
查看参考项目的 `AudioProcessor::Feed` 实现。

### 原因 2: AEC需要特定的采样率/帧长

ESP-SR 的AEC可能要求：
- 固定的采样率（16kHz）
- 固定的帧长（512采样点 = 32ms）
- 时间严格对齐

**检查 `feed_chunksize`**：
```c
int feed_chunksize = afe_handle->get_feed_chunksize(afe_data);
ESP_LOGI(TAG, "AFE feed chunksize: %d", feed_chunksize);
```

### 原因 3: VOIP模式需要不同的配置

`AEC_MODE_VOIP_HIGH_PERF` 可能需要额外的配置参数，比如：
- 回声延迟估计
- 参考信号延迟补偿
- 特定的滤波器长度

**查看参考项目是否有额外的配置**。

---

## 🚀 下一步行动

### 立即测试

1. **编译固件**：
```bash
cd /Users/renzhaojing/gitcode/renhejia/micropython-sr-aec/ports/esp32
idf.py build flash monitor
```

2. **查看初始化日志**，确认：
   - [ ] `aec_mode=2` (VOIP模式)
   - [ ] `NS model found` 和 `NS enabled`（或 `NS disabled` 但预期内）
   - [ ] `AFE feed channels: 2`

3. **运行测试**：
```python
import test_logic
sensor = test_logic.SensorSystem()
sensor.run()
```

4. **观察行为**：
   - 是否还是立即被打断？
   - 播放几秒后被打断（说明AEC部分工作）？
   - 完整播放（AEC完全工作）？

### 如果仍然失败

**方案 A：添加详细诊断日志**

在 `modespsr.c` 中添加：
```c
// 统计参考信号的活跃度
static uint32_t ref_nonzero_samples = 0;
static uint32_t ref_total_samples = 0;

for (int i = 0; i < feed_chunksize; i++) {
    int16_t ref_sample = g_reference_buffer[g_reference_read_index];
    if (ref_sample != 0) ref_nonzero_samples++;
    ref_total_samples++;
    feed_buff[i * 2 + 1] = ref_sample;
}

if (ref_total_samples > 0 && ref_total_samples % 16000 == 0) {  // 每1秒
    ESP_LOGI(TAG, "Ref signal activity: %u/%u (%.1f%%)", 
        ref_nonzero_samples, ref_total_samples, 
        100.0 * ref_nonzero_samples / ref_total_samples);
}
```

**方案 B：对比参考项目的完整 AFE 配置**

```bash
# 查看参考项目的 audio_processor.cc 中的所有 afe_config 设置
cd /Users/renzhaojing/gitcode/renhejia/source/xiaozhi-esp32-pan
grep -n "afe_config->" main/audio_processing/audio_processor.cc
```

**方案 C：暂时禁用 VAD 打断，只用唤醒词**

在 `test_logic.py` 中：
```python
# 注释掉 VAD 检测
# is_speaking = espsr.check_vad()
# if is_speaking:
#     break
```

这样只用唤醒词打断，可以测试播放本身是否流畅。

---

## 📚 技术要点

### VOIP模式 vs SR模式

| 维度 | VOIP模式 | SR模式 |
|------|---------|--------|
| 设计目标 | 双向通话 | 单向识别 |
| AEC强度 | **强** | 中 |
| 回声尾部 | 长（300-500ms） | 短（50-100ms） |
| 适用场景 | 持续对话 | 短暂唤醒 |
| 计算量 | 高 | 中 |

### 降噪（NS）的作用

1. **不是AEC的替代品**：
   - NS 消除稳态噪音（风扇、马达）
   - AEC 消除动态回声（喇叭播放）
   - **两者互补，都需要**

2. **提高AEC性能**：
   - NS 先清理背景噪音
   - 让AEC专注处理回声
   - 提高整体语音质量

3. **如果NS缺失**：
   - AEC仍应工作
   - 但效果会打折扣
   - 特别是嘈杂环境

---

## 🎉 成功标志

### 完全成功

```
📡 播放进度: 2.3% (4096/179042)
📡 播放进度: 21.3% (40960/179042)
📡 播放进度: 42.6% (81920/179042)
📡 播放进度: 63.9% (122880/179042)
📡 播放进度: 85.2% (163840/179042)
✅ 播放线程正常结束

--- 用户真实说话 ---
🗣️🗣️🗣️🗣️
🗣️ 检测到语音活动打断！（VAD）  ← 只有真实说话才打断
```

### 部分成功

```
📡 播放进度: 2.3% (4096/179042)
📡 播放进度: 21.3% (40960/179042)
🗣️ 检测到语音活动打断！（VAD）  ← 播放1-2秒后才被打断
```

说明 AEC 有一定效果，但还不够强。

### 完全失败

```
📡 播放进度: 2.3% (4096/179042)
🗣️ 检测到语音活动打断！（VAD）  ← 立即被打断
```

说明 AEC 基本无效。

---

**更新日期**: 2025-10-27  
**版本**: v2.4 (VOIP模式 + 诊断)  
**状态**: ✅ 代码已修改，等待用户测试反馈

