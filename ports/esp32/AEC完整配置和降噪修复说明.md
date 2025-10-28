# AEC 完整配置和降噪修复说明

## 📅 更新日期
2025-10-27

---

## ❌ 持续存在的问题

### 症状
即使添加了时序同步修复，设备播放音频时仍然会被 VAD 误判为"语音活动"，导致播放刚开始就被打断。

### 日志示例
```
📡 播放进度: 2.3% (4096/179042)

🗣️🗣️🗣️🗣️🗣️🗣️🗣️🗣️🗣️🗣️🗣️
🗣️ 检测到语音活动打断！（VAD）  ← 仍然误触发
🗣️🗣️🗣️🗣️🗣️🗣️🗣️🗣️🗣️🗣️🗣️
```

### 深层原因分析

经过对比参考项目，发现我们缺少了几个**关键配置**：

1. ❌ **缺少降噪（NS）配置**
2. ❌ **缺少其他 AFE 优化配置**
3. ❌ **AEC 需要预热时间**

---

## 🔍 参考项目的完整配置

### 参考代码 (`audio_processor.cc`)

```cpp
srmodel_list_t *models = esp_srmodel_init("model");

// 🔥 关键1：获取降噪模型
char* ns_model_name = esp_srmodel_filter(models, ESP_NSNET_PREFIX, NULL);

afe_config_t* afe_config = afe_config_init(input_format.c_str(), NULL, 
    AFE_TYPE_VC, AFE_MODE_HIGH_PERF);

// AEC配置
afe_config->aec_init = true;
afe_config->aec_mode = AEC_MODE_VOIP_HIGH_PERF;

// 🔥 关键2：降噪（NS）配置
afe_config->ns_init = true;
afe_config->ns_model_name = ns_model_name;
afe_config->afe_ns_mode = AFE_NS_MODE_NET;

// VAD配置
afe_config->vad_init = true;
afe_config->vad_mode = VAD_MODE_0;
afe_config->vad_min_noise_ms = 100;

// 🔥 关键3：其他优化配置
afe_config->afe_perferred_core = 1;
afe_config->afe_perferred_priority = 1;
afe_config->agc_init = false;
afe_config->memory_alloc_mode = AFE_MEMORY_ALLOC_MORE_PSRAM;
```

### 关键配置说明

1. **降噪（NS）**：
   - `ns_init = true`：启用降噪
   - `ns_model_name`：指定降噪模型
   - `afe_ns_mode = AFE_NS_MODE_NET`：使用神经网络降噪

2. **CPU 和内存优化**：
   - `afe_perferred_core = 1`：指定运行在CPU核心1
   - `afe_perferred_priority = 1`：设置优先级
   - `memory_alloc_mode = AFE_MEMORY_ALLOC_MORE_PSRAM`：使用PSRAM

3. **AGC**：
   - `agc_init = false`：禁用自动增益控制

---

## ✅ v2.3 完整修复方案

### 修复 1: 添加降噪（NS）配置

**C 代码** (`modespsr.c`)：

```c
// 获取降噪模型（关键！）
char *ns_model_name = esp_srmodel_filter(models, ESP_NSNET_PREFIX, NULL);
if (ns_model_name) {
    ESP_LOGI(TAG, "NS model found: %s", ns_model_name);
} else {
    ESP_LOGW(TAG, "NS model not found");
}

// AFE配置
afe_config_t *afe_config = afe_config_init("MR", models, AFE_TYPE_SR, AFE_MODE_HIGH_PERF);

// AEC配置
afe_config->wakenet_model_name = NULL;
afe_config->aec_init = true;
afe_config->aec_mode = AEC_MODE_SR_HIGH_PERF;

// 🔥 启用降噪（NS）配置
if (ns_model_name != NULL) {
    afe_config->ns_init = true;
    afe_config->ns_model_name = ns_model_name;
    afe_config->afe_ns_mode = AFE_NS_MODE_NET;
    ESP_LOGI(TAG, "NS enabled");
} else {
    afe_config->ns_init = false;
    ESP_LOGW(TAG, "NS disabled");
}

// VAD配置
afe_config->vad_init = true;
afe_config->vad_mode = VAD_MODE_0;
afe_config->vad_min_noise_ms = 100;

// 🔥 其他优化配置
afe_config->afe_perferred_core = 1;
afe_config->afe_perferred_priority = 1;
afe_config->agc_init = false;
afe_config->memory_alloc_mode = AFE_MEMORY_ALLOC_MORE_PSRAM;
```

### 修复 2: AEC 预热机制

**Python 代码** (`test_logic.py`)：

```python
aec_warmup_chunks = 10  # 前10个块不检测VAD，约1秒

while received_bytes < total_size:
    # 检测打断
    if data_count % interrupt_check_interval == 0:
        # 1. 始终检测唤醒词
        result = espsr.listen(1)
        if result == "wakeup":
            # 打断
            break
        
        # 2. 🔥 AEC预热后才检测VAD
        if data_count >= aec_warmup_chunks:
            is_speaking = espsr.check_vad()
            if is_speaking:
                # 打断
                break
    
    # 接收和播放音频...
```

**原理**：
- 前 10 个块（约 1 秒）：只检测唤醒词，不检测 VAD
- 给 AEC 时间适应和建立回声模型
- 1 秒后：开始检测 VAD 语音打断

---

## 📊 完整配置对比

| 配置项 | v2.0 | v2.1 | v2.2 | v2.3 (完整) |
|--------|------|------|------|------------|
| AEC | ✅ | ✅ | ✅ | ✅ |
| VAD | ❌ | ✅ | ✅ | ✅ |
| 时序同步 | ❌ | ❌ | ✅ | ✅ |
| 超时清零 | ❌ | ❌ | ✅ | ✅ |
| **降噪（NS）** | ❌ | ❌ | ❌ | ✅ |
| **CPU优化** | ❌ | ❌ | ❌ | ✅ |
| **PSRAM** | ❌ | ❌ | ❌ | ✅ |
| **AEC预热** | ❌ | ❌ | ❌ | ✅ |

---

## 🚀 编译和测试

### 1. 编译固件

```bash
cd /Users/renzhaojing/gitcode/renhejia/micropython-sr-aec/ports/esp32
idf.py build flash
```

### 2. 查看启动日志

**预期日志**：
```
✅ ESP-SR 初始化成功
NS model found: nsnet1_quantized  ← 应该找到降噪模型
NS enabled with model: nsnet1_quantized  ← 降噪已启用
AFE config: format=MR, aec_init=true, aec_mode=1, ns_init=true, vad_init=true
VAD mutex created, initial state: SILENCE
```

**⚠️ 如果看到**：
```
NS model not found, noise suppression will be disabled  ← 模型缺失
NS disabled (model not found)
```

说明降噪模型文件不存在，需要检查 ESP-SR 组件的模型文件。

### 3. 运行测试

```python
import test_logic
sensor = test_logic.SensorSystem()
sensor.run()
```

### 4. 预期结果

**成功标志**：
```
📡 播放进度: 2.3% (4096/179042)
📡 播放进度: 21.3% (40960/179042)  ← AEC预热期，不检测VAD
📡 播放进度: 42.6% (81920/179042)  ← 开始检测VAD
📡 播放进度: 63.9% (122880/179042)  ← 播放流畅，无误触发
📡 播放进度: 85.2% (163840/179042)

✅ 播放线程正常结束  ← 完整播放，未被自己打断
```

**真实说话打断**：
```
📡 播放进度: 42.6% (81920/179042)

--- 用户真实说话（播放1秒后）---

🗣️🗣️🗣️🗣️🗣️🗣️🗣️🗣️🗣️🗣️🗣️
🗣️ 检测到语音活动打断！（VAD）  ← 正确：真实说话触发
🗣️🗣️🗣️🗣️🗣️🗣️🗣️🗣️🗣️🗣️🗣️
```

---

## 🔧 问题排查

### 问题 1: 仍然立即被打断

**可能原因**：
1. 降噪模型未加载（检查日志）
2. AEC 预热时间太短（增加 `aec_warmup_chunks`）
3. 音量过大导致 AEC 饱和

**解决方法**：
```python
# 调整预热时间
aec_warmup_chunks = 20  # 增加到20个块（约2秒）

# 或者禁用 VAD 打断，只用唤醒词
# 注释掉 VAD 检测代码
```

### 问题 2: NS 模型未找到

**检查步骤**：
1. 确认 ESP-SR 组件包含降噪模型
2. 检查 `model` 目录是否存在
3. 查看编译日志中的模型加载信息

**临时方案**：
如果降噪模型确实缺失，可以暂时禁用 NS：
```c
// 强制禁用 NS
afe_config->ns_init = false;
```

### 问题 3: 真实说话也无法打断

**可能原因**：
- AEC 预热时间太长
- VAD 灵敏度太低

**解决方法**：
```python
# 减少预热时间
aec_warmup_chunks = 5  # 减少到5个块（约0.5秒）

# 或调整 VAD 模式（C代码）
afe_config->vad_mode = VAD_MODE_0;  // 0最灵敏，3最不灵敏
```

---

## 📚 技术原理

### 降噪（NS）的作用

1. **减少环境噪音**：
   - 消除背景噪音（风扇、空调等）
   - 提高语音清晰度

2. **辅助 AEC**：
   - 降噪后的信号更纯净
   - AEC 更容易区分回声和真实语音

3. **提高 VAD 准确性**：
   - 减少噪音干扰
   - 降低误触发率

### AEC 预热机制

**为什么需要预热**：
1. AEC 是自适应算法，需要时间学习回声特性
2. 播放刚开始时，AEC 模型还未收敛
3. 此时的 VAD 检测容易误判

**预热时间选择**：
- 太短（<0.5秒）：AEC 未充分学习，仍会误触发
- 太长（>2秒）：真实打断响应慢
- **推荐：1秒**（10个4KB块）

### 配置优先级

1. **核心配置**（必须）：
   - AEC: `aec_init = true`
   - 参考信号时序对齐
   - 超时清零机制

2. **增强配置**（强烈推荐）：
   - 降噪: `ns_init = true`
   - VAD: `vad_init = true`
   - AEC 预热

3. **优化配置**（可选）：
   - CPU 核心绑定
   - 优先级设置
   - PSRAM 分配

---

## 🎯 最佳实践

### 1. 播放音频时

```python
# 1. 先喂参考信号
espsr.feed_reference(audio_chunk)

# 2. 再播放音频
audio_out.write(audio_chunk)

# 3. AEC预热后才检测VAD
if data_count >= aec_warmup_chunks:
    is_speaking = espsr.check_vad()
```

### 2. 录音时

```python
# 使用VAD检测静音
is_speaking = espsr.check_vad()

if is_speaking:
    # 正在说话
    silence_start_time = None
else:
    # 静音
    if silence_duration >= 1.5:
        # 停止录音
        break
```

### 3. 参数调优

```python
# 可调参数
aec_warmup_chunks = 10      # AEC预热块数（5-20）
MIN_SILENCE_DURATION = 1.5  # 静音时长（1.0-2.0秒）
VAD_CHECK_INTERVAL = 50     # VAD检测间隔（30-100ms）
```

---

## 📖 相关文档

- `VAD功能添加说明.md` - VAD 功能详解
- `AEC时序同步修复说明.md` - 时序同步原理
- `modespsr.c` - ESP-SR 模块源码
- `test_logic.py` - 测试脚本

---

## 🎉 总结

### v2.3 完整修复内容

1. ✅ **启用降噪（NS）**：使用神经网络降噪模型
2. ✅ **添加 AFE 优化配置**：CPU核心、优先级、PSRAM
3. ✅ **实现 AEC 预热机制**：播放1秒后才检测VAD
4. ✅ **保持时序同步**：先喂参考信号，再播放
5. ✅ **超时清零机制**：100ms超时自动清空缓冲区

### 关键要素

| 要素 | 说明 | 重要性 |
|------|------|--------|
| AEC | 回声消除 | ⭐⭐⭐⭐⭐ |
| NS | 降噪 | ⭐⭐⭐⭐⭐ |
| 时序同步 | 参考信号对齐 | ⭐⭐⭐⭐⭐ |
| AEC预热 | 避免误触发 | ⭐⭐⭐⭐ |
| 超时清零 | 防止残留 | ⭐⭐⭐ |

### 预期效果

- ✅ 播放音频时不会被自己打断
- ✅ 真实说话能准确触发VAD
- ✅ AEC有效消除回声
- ✅ 降噪提高语音清晰度

---

**更新日期**: 2025-10-27  
**版本**: v2.3 (完整配置版)  
**状态**: ✅ 已实现，待测试

