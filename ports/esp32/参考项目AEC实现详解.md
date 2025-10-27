# 参考项目 AEC 实现详解

## 🎯 核心问题回答

### Q1: 录音是否一直开着？

**答：是的！AudioLoop 一直在运行，但会根据不同状态处理音频数据。**

### Q2: 打断逻辑是怎样的？

**答：播放期间唤醒词检测继续运行，检测到唤醒词时调用 AbortSpeaking 打断播放。**

---

## 🏗️ 参考项目架构

### 核心组件

```cpp
class Application {
    WakeWordDetect wake_word_detect_;  // 唤醒词检测器
    DeviceState device_state_;         // 设备状态机
    AudioLoop();                       // 音频处理循环
};
```

### 设备状态 (DeviceState)

```cpp
enum DeviceState {
    kDeviceStateIdle,       // 待机
    kDeviceStateListening,  // 录音
    kDeviceStateSpeaking,   // 播放
    // ... 其他状态
};
```

---

## 🔄 AudioLoop - 持续运行的音频循环

### AudioLoop 架构

```cpp
void Application::AudioLoop() {
    auto codec = Board::GetInstance().GetAudioCodec();
    while (true) {
        OnAudioInput();    // 处理音频输入
        if (codec->output_enabled()) {
            OnAudioOutput();   // 处理音频输出
        }
    }
}
```

**关键特点**：
- ✅ **无限循环**，持续运行
- ✅ **每次循环**都调用 OnAudioInput
- ✅ **根据状态**决定如何处理音频

---

## 📥 OnAudioInput - 状态驱动的音频输入

### 完整代码

```cpp
void Application::OnAudioInput() {
    // 情况1：唤醒词检测运行中
    if (wake_word_detect_.IsDetectionRunning()) {
        std::vector<int16_t> data;
        int samples = wake_word_detect_.GetFeedSize();
        if (samples > 0) {
            ReadAudio(data, 16000, samples);
            wake_word_detect_.Feed(data);  // ← 喂给检测器
            return;
        }
    }
    
    // 情况2：录音上传模式
    if (device_state_ == kDeviceStateListening) {
        std::vector<int16_t> data;
        ReadAudio(data, 16000, 30 * 16000 / 1000);
        // 编码并发送
        opus_encoder_->Encode(data, [](opus) {
            protocol_->SendAudio(opus);  // ← 发送到服务器
        });
        return;
    }
    
    // 情况3：其他状态（如播放）
    vTaskDelay(pdMS_TO_TICKS(30));  // 延迟 30ms
}
```

### 关键理解

| 状态 | wake_word_detect 运行? | OnAudioInput 行为 |
|------|----------------------|------------------|
| **Idle (待机)** | ✅ Running | 读音频 → Feed 给检测器 |
| **Listening (录音)** | ❌ Stopped | 读音频 → 编码 → 发送服务器 |
| **Speaking (播放)** | ✅ Running | 读音频 → Feed 给检测器 |

**核心逻辑**：
1. 音频 **持续读取**（AudioLoop 一直运行）
2. 根据 **不同状态** 分发音频数据
3. 播放期间 **继续检测唤醒词**

---

## 📤 OnAudioOutput - 音频输出

```cpp
void Application::OnAudioOutput() {
    if (audio_decode_queue_.empty()) {
        return;  // 没有数据就返回
    }
    
    if (device_state_ == kDeviceStateListening) {
        audio_decode_queue_.clear();  // 录音时清空播放队列
        return;
    }
    
    // 解码并播放音频
    auto opus = audio_decode_queue_.front();
    opus_decoder_->Decode(opus, pcm);
    codec->OutputData(pcm);  // 输出到扬声器
}
```

---

## 🎯 完整流程详解

### 1. 待机阶段 (Idle)

```
[启动]
  ↓
SetDeviceState(Idle)
  ↓
wake_word_detect_.StartDetection()  ← 启动检测
  ↓
[AudioLoop 持续运行]
  ├─ OnAudioInput()
  │   └─ IsDetectionRunning() = true
  │       → ReadAudio() → wake_word_detect_.Feed()
  └─ OnAudioOutput()
      └─ (无音频数据，跳过)
```

**关键代码**（SetDeviceState）：
```cpp
case kDeviceStateIdle:
    display->SetStatus(Lang::Strings::STANDBY);
    wake_word_detect_.StartDetection();  // ← 启动检测
    break;
```

### 2. 检测到唤醒词

```
[AudioLoop 中]
  ↓
wake_word_detect_.Feed(data)
  ↓
[检测器内部识别到唤醒词]
  ↓
触发回调: OnWakeWordDetected(wake_word)
  ↓
Schedule([this, wake_word]() {
    if (device_state_ == kDeviceStateIdle) {
        wake_word_detect_.StopDetection();  // ← 停止检测
        
        if (!protocol_->OpenAudioChannel()) {
            wake_word_detect_.StartDetection();  // 失败则重新启动
            return;
        }
        
        // 发送唤醒词PCM数据
        while (wake_word_detect_.GetWakeWordOpus(opus)) {
            protocol_->SendAudio(opus);
        }
        protocol_->SendWakeWordDetected(wake_word);
        
        SetListeningMode(kListeningModeAutoStop);  // → 进入 Listening 状态
    } 
    else if (device_state_ == kDeviceStateSpeaking) {
        AbortSpeaking(kAbortReasonWakeWordDetected);  // ← 打断！
    }
});
```

**关键点**：
- 待机时检测到 → 停止检测，开始录音
- **播放时检测到 → AbortSpeaking 打断播放** ✨

### 3. 录音阶段 (Listening)

```
SetListeningMode()
  ↓
SetDeviceState(Listening)
  ↓
wake_word_detect_.StopDetection()  ← 停止检测
  ↓
[AudioLoop 持续运行]
  ├─ OnAudioInput()
  │   └─ device_state_ == Listening
  │       → ReadAudio() → Encode() → protocol_->SendAudio()
  └─ OnAudioOutput()
      └─ (录音时清空播放队列)
```

**关键代码**：
```cpp
case kDeviceStateListening:
    display->SetStatus(Lang::Strings::LISTENING);
    protocol_->SendStartListening(listening_mode_);
    opus_encoder_->ResetState();
    wake_word_detect_.StopDetection();  // ← 录音时停止检测
    break;
```

**为什么录音时停止检测？**
- 避免自己的声音触发唤醒
- 专注于录音上传

### 4. 服务器返回 TTS 开始

```
[收到 tts:start]
  ↓
SetDeviceState(Speaking)
  ↓
wake_word_detect_.StartDetection()  ← 重新启动检测！✨
ResetDecoder()
  ↓
[AudioLoop 持续运行]
  ├─ OnAudioInput()
  │   └─ IsDetectionRunning() = true
  │       → ReadAudio() → wake_word_detect_.Feed()  ← 检测打断
  └─ OnAudioOutput()
      └─ 解码音频队列 → codec->OutputData()
```

**关键代码**：
```cpp
case kDeviceStateSpeaking:
    display->SetStatus(Lang::Strings::SPEAKING);
    
    if (listening_mode_ != kListeningModeRealtime) {
        wake_word_detect_.StartDetection();  // ← 播放时启动检测！
    }
    ResetDecoder();
    break;
```

**关键点**：
- ✅ 播放期间 **重新启动唤醒词检测**
- ✅ OnAudioInput 继续 Feed 数据给检测器
- ✅ OnAudioOutput 同时播放音频

### 5. 播放期间检测到唤醒词（打断）

```
[AudioLoop 中]
  ↓
OnAudioInput()
  └─ IsDetectionRunning() = true
      → ReadAudio() → wake_word_detect_.Feed()
          ↓
      [检测到唤醒词]
          ↓
      OnWakeWordDetected(wake_word)
          ↓
      device_state_ == Speaking
          ↓
      AbortSpeaking(kAbortReasonWakeWordDetected)  ← 打断播放！
          ↓
      protocol_->SendAbortSpeaking(reason)
          ↓
      SetListeningMode(kListeningModeManualStop)  → 重新录音
```

**关键代码**：
```cpp
wake_word_detect_.OnWakeWordDetected([this](wake_word) {
    Schedule([this, wake_word]() {
        if (device_state_ == kDeviceStateIdle) {
            // 待机时的处理...
        } 
        else if (device_state_ == kDeviceStateSpeaking) {
            AbortSpeaking(kAbortReasonWakeWordDetected);  // ← 打断！
        }
    });
});
```

**AbortSpeaking 做了什么？**
```cpp
void Application::AbortSpeaking(AbortReason reason) {
    ESP_LOGI(TAG, "Abort speaking");
    aborted_ = true;  // 设置打断标志
    protocol_->SendAbortSpeaking(reason);  // 通知服务器
}
```

### 6. 服务器返回 TTS 结束

```
[收到 tts:stop]
  ↓
SetDeviceState(Idle)  或  SetDeviceState(Listening)
  ↓
wake_word_detect_.StartDetection()  ← 重新启动检测
  ↓
回到待机或继续对话
```

---

## 📊 状态转换图

```
Idle (检测运行)
  ↓ 检测到唤醒词
Listening (检测停止)
  ↓ 录音完成
Speaking (检测重新启动！)
  ↓ 播放完成
Idle (检测运行)

[打断路径]
Speaking (检测运行)
  ↓ 检测到唤醒词
AbortSpeaking()
  ↓
Listening (检测停止)
```

---

## 🎯 核心要点总结

### 1. AudioLoop 持续运行

**参考项目**：
```cpp
void AudioLoop() {
    while (true) {
        OnAudioInput();   // 每次循环都处理输入
        OnAudioOutput();  // 每次循环都处理输出
    }
}
```

**关键**：
- ✅ AudioLoop **永不停止**
- ✅ 音频 **持续读取**
- ✅ 根据状态 **分发数据**

### 2. 状态驱动的检测控制

| 状态 | 检测状态 | 音频数据去向 |
|------|---------|------------|
| Idle | ✅ Running | → wake_word_detect |
| Listening | ❌ Stopped | → 编码 → 服务器 |
| Speaking | ✅ Running | → wake_word_detect (同时播放) |

### 3. 播放期间的打断机制

```
播放中 (Speaking)
  ↓
wake_word_detect_.StartDetection()  ← 检测运行
  ↓
[AudioLoop]
  ├─ OnAudioInput()
  │   └─ wake_word_detect_.Feed()  ← 持续检测
  └─ OnAudioOutput()
      └─ codec->OutputData()       ← 持续播放
  ↓
检测到唤醒词
  ↓
AbortSpeaking()  ← 打断播放
  ↓
SetListeningMode()  ← 重新录音
```

---

## 🔍 与当前实现的关键差异

### 参考项目

| 特性 | 实现 |
|------|------|
| **音频循环** | AudioLoop 持续运行 |
| **状态管理** | 状态机驱动 (Idle/Listening/Speaking) |
| **检测控制** | StartDetection / StopDetection |
| **播放期间检测** | ✅ Speaking 状态下检测运行 |
| **打断机制** | OnWakeWordDetected 回调 → AbortSpeaking |
| **数据流** | OnAudioInput 根据状态分发 |

### 当前 MicroPython 实现

| 特性 | 实现 |
|------|------|
| **音频循环** | 主循环 + 播放线程 |
| **状态管理** | 标志位 (is_playing_response, playback_thread_active) |
| **检测控制** | espsr.start_recording / stop_recording |
| **播放期间检测** | ❓ 播放线程内调用 espsr.listen(1) |
| **打断机制** | 播放线程检测 + wakeup_interrupted 标志 |
| **数据流** | 按需读取 (espsr.read_audio) |

---

## ⚠️ 当前实现的问题分析

### 问题 1: 检测未持续运行

**参考项目**：
```cpp
// AudioLoop 每次都调用
OnAudioInput() {
    if (wake_word_detect_.IsDetectionRunning()) {
        ReadAudio(data);
        wake_word_detect_.Feed(data);  // ← 持续 Feed
    }
}
```

**当前实现**：
```python
# 播放线程中
if data_count % 5 == 0:
    result = espsr.listen(1)  # ← 只在特定时机调用
```

**差异**：
- 参考项目：**每次循环**都 Feed 数据给检测器
- 当前实现：**每 5 个包**才调用一次 listen

### 问题 2: Feed 数据的方式

**参考项目**：
```cpp
// 1. 读取音频
ReadAudio(data, 16000, samples);

// 2. Feed 给检测器
wake_word_detect_.Feed(data);

// 3. 检测器内部
void WakeWordDetect::Feed(data) {
    afe_iface_->feed(afe_data_, data.data());  // 喂给 AFE
}
```

**当前实现**：
```python
# 调用 listen，内部会自动从 I2S 读取
result = espsr.listen(1)
```

**关键差异**：
- 参考项目：**显式 Feed** 数据
- 当前实现：**隐式读取**，通过 listen

### 问题 3: 录音缓冲区的作用

**参考项目**：
- 没有"录音缓冲区"的概念
- AudioLoop 直接从 codec 读取
- 根据状态决定数据去向

**当前实现**：
- `espsr.start_recording()` 启用缓冲区
- `espsr.read_audio()` 从缓冲区读取
- `espsr.listen()` 从哪里读？

**混乱点**：
- `espsr.listen()` 和 `espsr.read_audio()` 都读取音频
- 播放期间调用 `espsr.listen()`，但录音模式的数据是否被 `espsr.listen()` 使用？

---

## 💡 根本问题

### 参考项目的核心

```
[I2S 麦克风] → AudioLoop::ReadAudio()
                    ↓
        根据状态分发:
            ├─ Idle/Speaking → wake_word_detect_.Feed() → AFE → MultiNet
            └─ Listening → Encode → 服务器
```

**特点**：
- ✅ 数据流清晰
- ✅ 状态驱动明确
- ✅ Feed 持续进行

### 当前实现的混乱

```
[I2S 麦克风] → ESP-SR feed_Task → g_record_buffer
                                       ↓
                            espsr.read_audio() (用于录音上传)
                            espsr.listen()     (用于检测？)
```

**问题**：
- ❓ `espsr.listen()` 是否也从 `g_record_buffer` 读取？
- ❓ 播放期间 `g_record_buffer` 有数据吗？
- ❓ `espsr.listen()` 的数据流是什么？

---

## 🎯 正确的实现方向

### modespsr.c 的数据流应该是：

```c
void feed_Task() {
    while (task_flag) {
        // 1. 从 I2S 读取麦克风数据
        i2s_channel_read(rx_handle, mic_data, ...);
        
        // 2. 构建双通道数据 (Mic + Reference)
        for (int i = 0; i < chunksize; i++) {
            feed_buff[i * 2] = mic_data[i];      // 麦克风
            feed_buff[i * 2 + 1] = reference[i]; // 播放参考
        }
        
        // 3. 喂给 AFE (AEC 处理)
        afe_handle->feed(afe_data, feed_buff);
        
        // 4. (可选) 如果录音模式启用，同时写入 g_record_buffer
        if (g_recording_enabled) {
            写入 g_record_buffer
        }
    }
}

void detect_Task() {
    while (task_flag) {
        // 从 AFE 获取处理后的数据
        res = afe_handle->fetch(afe_data);
        
        // MultiNet 检测
        mn_state = multinet->detect(model_data, res->data);
        
        if (mn_state == ESP_MN_STATE_DETECTED) {
            // 检测到唤醒词/命令词
            放入结果队列
        }
    }
}

mp_obj_t espsr_listen(timeout) {
    // 从结果队列读取检测结果
    if (xQueueReceive(g_result_que, &result, timeout)) {
        return result;
    }
    return "timeout";
}
```

**关键点**：
1. ✅ feed_Task **持续运行**，持续喂给 AFE
2. ✅ detect_Task **持续运行**，持续检测
3. ✅ espsr.listen() 只是**读取检测结果**
4. ✅ `g_record_buffer` 只用于**录音上传**

---

## 📌 总结

### 参考项目的精髓

1. **AudioLoop 持续运行** - 音频处理永不停止
2. **状态驱动分发** - 根据状态决定数据去向
3. **检测与播放并行** - Speaking 状态下检测继续运行
4. **显式 Feed** - 明确喂给检测器
5. **回调打断** - 检测到唤醒词时调用 AbortSpeaking

### 当前实现需要验证的

1. ❓ `espsr.listen()` 的数据来源是什么？
2. ❓ feed_Task 是否持续运行？
3. ❓ detect_Task 是否持续运行？
4. ❓ 播放期间 AFE 是否持续接收数据？
5. ❓ 播放期间 MultiNet 是否持续检测？

### 可能的根本问题

**如果 feed_Task 或 detect_Task 没有持续运行，那么：**
- `espsr.listen()` 调用时才触发检测
- 播放期间间隔太长（每 5 个包），可能错过唤醒词
- AEC 的参考信号可能没有正确工作

---

## 更新日期

2025-10-27

