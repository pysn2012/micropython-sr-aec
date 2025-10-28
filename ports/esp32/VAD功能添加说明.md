# VAD (语音活动检测) 功能添加说明

## 📅 更新日期
2025-10-27

---

## 🎯 添加目的

解决两个关键问题：
1. **使用 VAD 替代能量值检测静音**：参考项目使用 ESP-SR 内置的 VAD 功能，比简单的能量值检测更准确
2. **支持普通语音打断**：不仅可以用唤醒词打断，任何说话都能打断播放

---

## 🔍 参考项目的 VAD 实现

### 参考代码分析

**`audio_processor.cc`** (第 39-43 行):
```cpp
if (realtime_chat) {
    afe_config->vad_init = false;
} else {
    afe_config->vad_init = true;
    afe_config->vad_mode = VAD_MODE_0;
    afe_config->vad_min_noise_ms = 100;
}
```

**`audio_processor.cc`** (第 124-133 行):
```cpp
// VAD state change
if (vad_state_change_callback_) {
    if (res->vad_state == VAD_SPEECH && !is_speaking_) {
        is_speaking_ = true;
        vad_state_change_callback_(true);
    } else if (res->vad_state == VAD_SILENCE && is_speaking_) {
        is_speaking_ = false;
        vad_state_change_callback_(false);
    }
}
```

### 关键发现

1. **ESP-SR AFE 内置 VAD**：通过配置启用
2. **VAD 状态**：
   - `VAD_SPEECH`：检测到语音
   - `VAD_SILENCE`：检测到静音
3. **实时检测**：每次 `afe_handle->fetch()` 都会更新 `res->vad_state`
4. **配置参数**：
   - `vad_mode = VAD_MODE_0`：模式 0（灵敏度最高）
   - `vad_min_noise_ms = 100`：最小噪音时长 100ms

---

## ✅ 当前项目的 VAD 实现

### 1. C 代码修改 (`modespsr.c`)

#### 添加 VAD 全局变量

```c
// VAD (Voice Activity Detection) 状态
static volatile bool g_vad_speaking = false;  // 当前是否检测到语音
static SemaphoreHandle_t g_vad_mutex = NULL;
```

#### 启用 VAD 配置

```c
// 启用VAD配置（语音活动检测）
afe_config->vad_init = true;  // 启用VAD
afe_config->vad_mode = VAD_MODE_0;  // VAD模式0（灵敏度最高）
afe_config->vad_min_noise_ms = 100;  // 最小噪音时长100ms
```

#### 在 detect_Task 中更新 VAD 状态

```c
// 🔥 更新 VAD 状态（语音活动检测）
if (g_vad_mutex != NULL && xSemaphoreTake(g_vad_mutex, pdMS_TO_TICKS(5)) == pdTRUE) {
    bool new_speaking = (res->vad_state == VAD_SPEECH);
    if (new_speaking != g_vad_speaking) {
        g_vad_speaking = new_speaking;
    }
    xSemaphoreGive(g_vad_mutex);
}
```

#### 新增 MicroPython 接口

```c
// MicroPython接口：检测 VAD 状态（语音活动检测）
static mp_obj_t espsr_check_vad(void) {
    if (!espsr_initialized) {
        return mp_const_false;
    }
    
    bool is_speaking = false;
    if (g_vad_mutex != NULL && xSemaphoreTake(g_vad_mutex, pdMS_TO_TICKS(10)) == pdTRUE) {
        is_speaking = g_vad_speaking;
        xSemaphoreGive(g_vad_mutex);
    }
    
    return is_speaking ? mp_const_true : mp_const_false;
}
```

#### 模块注册

```c
{ MP_ROM_QSTR(MP_QSTR_check_vad), MP_ROM_PTR(&espsr_check_vad_obj) },
```

---

### 2. Python 代码修改 (`test_logic.py`)

#### 使用 VAD 检测静音

**新函数**: `record_until_silence_vad()`

```python
def record_until_silence_vad(self):
    """使用 VAD 录音直到检测到静音"""
    MIN_SILENCE_DURATION = 1.5   # 最少静音时长（秒）
    MAX_RECORD_TIME = 10         # 最大录音时长（秒）
    VAD_CHECK_INTERVAL = 50      # VAD 检测间隔（ms）
    
    silence_start_time = None
    has_spoken = False  # 是否检测到过说话
    
    while True:
        # 读取音频数据（保持录音缓冲区不满）
        bytes_read = espsr.read_audio(buffer)
        
        # 🔥 使用 VAD 检测语音活动
        is_speaking = espsr.check_vad()
        
        if is_speaking:
            # 检测到语音
            has_spoken = True
            silence_start_time = None  # 重置静音计时
            print(f"🎤 录音中... VAD: SPEECH")
        else:
            # 检测到静音
            if has_spoken:  # 只有在说过话之后才开始计时静音
                if silence_start_time is None:
                    silence_start_time = time.time()
                    print(f"🔇 检测到静音，开始计时...")
                else:
                    silence_duration = time.time() - silence_start_time
                    if silence_duration >= MIN_SILENCE_DURATION:
                        print(f"✅ 静音持续 {silence_duration:.1f}s，结束录音")
                        return
```

**优势**：
- ✅ 不需要计算能量值
- ✅ 更准确的语音活动检测
- ✅ 等待用户开始说话后才计时静音
- ✅ 使用 ESP-SR 内置算法，性能更好

#### 使用 VAD 检测语音打断

**修改**: `playback_stream_func()`

```python
# 🔥 每个块都检测打断（唤醒词 + VAD 语音活动）
if data_count % interrupt_check_interval == 0:
    # 1. 检测唤醒词
    result = espsr.listen(1)
    if result == "wakeup":
        print("🛑 检测到唤醒词打断！")
        self.wakeup_interrupted = True
        self.stop_playback = True
        break
    
    # 2. 🔥 检测 VAD 语音活动（普通说话也能打断）
    is_speaking = espsr.check_vad()
    if is_speaking:
        print("🗣️ 检测到语音活动打断！（VAD）")
        self.wakeup_interrupted = True
        self.stop_playback = True
        break
```

**优势**：
- ✅ 支持两种打断方式：唤醒词 + 普通说话
- ✅ 更自然的交互体验
- ✅ 对齐参考项目的实现方式

---

## 📊 功能对比

| 功能 | v1.0 (能量值) | v2.0 (VAD) | 改进 |
|------|--------------|-----------|------|
| **静音检测** | 能量值计算 | VAD 算法 | ✅ 更准确 |
| **检测准确性** | 受噪音影响大 | 噪音抑制好 | ✅ 更稳定 |
| **CPU 占用** | 需要计算能量 | 硬件加速 | ✅ 更高效 |
| **打断方式** | 仅唤醒词 | 唤醒词 + 普通说话 | ✅ 更自然 |
| **配置复杂度** | 需要调整阈值 | 参数少 | ✅ 更简单 |

---

## 🚀 使用方法

### 1. 编译固件

```bash
cd ports/esp32
idf.py build flash
```

### 2. 运行测试

```python
import test_logic
sensor = test_logic.SensorSystem()
sensor.run()
```

### 3. 测试场景

**场景 1: 唤醒词打断**
```
播放音频 → 说 "嗨小乐" → 立即停止播放 → 开始录音
```

**场景 2: 普通说话打断（新功能！）**
```
播放音频 → 直接说话（任何内容）→ 立即停止播放 → 开始录音
```

**场景 3: VAD 静音检测**
```
开始录音 → 说话 → 停止说话 1.5 秒 → 自动停止录音
```

---

## 📊 预期日志

### 启动日志

```
✅ ESP-SR 初始化成功
AFE config: format=MR, aec_init=true, aec_mode=1, vad_init=true  ← VAD 已启用
VAD mutex created, initial state: SILENCE  ← VAD 互斥量创建成功
```

### 播放打断日志（唤醒词）

```
📡 播放进度: 42.6% (81920/192044)

🛑🛑🛑🛑🛑🛑🛑🛑🛑🛑🛑🛑🛑🛑🛑
🛑 检测到唤醒词打断！
🛑🛑🛑🛑🛑🛑🛑🛑🛑🛑🛑🛑🛑🛑🛑

✅ 检测到打断，开始录音...
```

### 播放打断日志（VAD 语音活动）**新功能！**

```
📡 播放进度: 56.8% (110592/192044)

🗣️🗣️🗣️🗣️🗣️🗣️🗣️🗣️🗣️🗣️🗣️🗣️🗣️🗣️🗣️
🗣️ 检测到语音活动打断！（VAD）
🗣️🗣️🗣️🗣️🗣️🗣️🗣️🗣️🗣️🗣️🗣️🗣️🗣️🗣️🗣️

✅ 检测到打断，开始录音...
```

### VAD 录音日志

```
============================================================
🎤 开始录音（VAD 静音检测）
============================================================
🎙️ 录音参数:
  - VAD 检测间隔: 50ms
  - 静音时长: 1.5s
  - 最大时长: 10s

⏰ 等待用户说话...
🎤 录音中... VAD: SPEECH
🎤 录音中... VAD: SPEECH
🔇 检测到静音，开始计时...
✅ 静音持续 1.5s，结束录音
📊 录音统计:
  - 时长: 3.45s
  - 数据: 55296 字节
```

---

## 🔧 参数调整

### VAD 灵敏度

修改 `modespsr.c`:
```c
afe_config->vad_mode = VAD_MODE_0;  // 0: 最灵敏, 1/2/3: 逐渐降低灵敏度
```

### VAD 检测间隔

修改 `test_logic.py`:
```python
VAD_CHECK_INTERVAL = 50  # 50ms（推荐）
# 更快：30ms（更快响应，更高 CPU）
# 更慢：100ms（更低 CPU，稍慢响应）
```

### 静音时长

修改 `test_logic.py`:
```python
MIN_SILENCE_DURATION = 1.5  # 1.5 秒（推荐）
# 更短：1.0 秒（更快结束，可能误判）
# 更长：2.0 秒（更稳定，稍慢）
```

---

## ✅ 验证清单

### C 代码验证
- [ ] VAD 全局变量已添加
- [ ] AFE 配置中启用 VAD
- [ ] detect_Task 中更新 VAD 状态
- [ ] VAD 互斥量创建和销毁
- [ ] `espsr.check_vad()` 接口已注册
- [ ] 代码编译成功

### Python 代码验证
- [ ] `record_until_silence_vad()` 已实现
- [ ] 播放线程中添加 VAD 打断检测
- [ ] `run_test_loop()` 使用 VAD 录音函数
- [ ] 代码无语法错误

### 功能验证
- [ ] 启动日志显示 `vad_init=true`
- [ ] 播放时说话能打断（不需要唤醒词）
- [ ] 录音时 VAD 状态正确显示
- [ ] 静音检测准确，能自动停止录音

---

## 🆚 与参考项目对比

| 功能 | 参考项目 | 当前实现 | 状态 |
|------|---------|---------|------|
| VAD 启用 | ✅ | ✅ | 对齐 |
| VAD 模式 | VAD_MODE_0 | VAD_MODE_0 | 对齐 |
| VAD 状态检测 | ✅ | ✅ | 对齐 |
| 语音打断播放 | ✅ | ✅ | 对齐 |
| 静音检测 | ✅ VAD | ✅ VAD | 对齐 |
| 实现语言 | C++ | MicroPython + C | 不同 |

---

## 📚 相关文档

- `modespsr.c` - ESP-SR C 模块（已更新）
- `test_logic.py` - 测试脚本（已更新）
- `AEC打断测试指南.md` - 测试指南
- `流式播放版本更新说明.md` - v2.0 更新说明

---

## 🔄 升级步骤

### 从 v2.0（无 VAD）升级到 v2.1（VAD）

1. **重新编译固件**
   ```bash
   cd ports/esp32
   idf.py build flash
   ```

2. **上传新的测试脚本**
   - 上传 `test_logic.py` 到 `/flash/`

3. **运行测试**
   ```python
   import test_logic
   sensor = test_logic.SensorSystem()
   sensor.run()
   ```

4. **验证 VAD 功能**
   - 检查启动日志是否有 `vad_init=true`
   - 测试播放时直接说话能否打断
   - 测试录音是否能自动检测静音

---

## 🎉 总结

### 主要改进
✅ **启用 ESP-SR 内置 VAD**：更准确的语音活动检测  
✅ **支持普通语音打断**：不仅是唤醒词，任何说话都能打断  
✅ **优化静音检测**：使用 VAD 替代能量值计算  
✅ **对齐参考项目**：实现方式与参考项目保持一致  
✅ **性能提升**：硬件加速，CPU 占用更低  

### 用户体验提升
- 🚀 更自然的交互：随时说话即可打断
- 🚀 更准确的检测：减少误判和漏判
- 🚀 更简单的配置：参数更少，更易使用

---

**更新日期**: 2025-10-27  
**版本**: v2.1 (VAD版)  
**状态**: ✅ 已实现，待测试

