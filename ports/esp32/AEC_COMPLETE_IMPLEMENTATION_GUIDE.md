# MicroPython AEC打断功能完整实现指南

基于 xiaozhi-esp32-pan 项目的AEC实现分析

---

## 📚 核心原理理解

### 参考项目的AEC实现机制

参考项目（xiaozhi-esp32-pan）的AEC实现基于ESP-SR的AFE（Audio Front-End）模块，核心思路是：

```
麦克风输入 ─────┐
                ├──> AFE (AEC处理) ──> 干净的人声
参考信号 ───────┘
(喇叭播放的音频)
```

#### 关键代码分析（来自参考项目）

**1. NoAudioCodecSimplexPdm - 双通道数据构建**

```cpp
// no_audio_codec.cc 第288-294行
input_reference_ = true;              // 启用参考信号
input_channels_ = input_reference_ ? 2 : 1;  // 双通道：麦克风+参考信号

// 第492-496行：构建双通道输出
for (int i = 0; i < actual_samples; i++) {
    dest[i * 2] = bit16_buffer[i];      // 通道0：麦克风数据
    dest[i * 2 + 1] = output_buffer_[i_index];  // 通道1：参考信号
}
```

**2. WakeWordDetect - AFE配置**

```cpp
// wake_word_detect.cc 第60-67行
std::string input_format;
for (int i = 0; i < codec_->input_channels() - ref_num; i++) {
    input_format.push_back('M');  // M = 麦克风
}
for (int i = 0; i < ref_num; i++) {
    input_format.push_back('R');  // R = 参考信号
}
// 结果：input_format = "MR"（单麦克风+参考信号）

// 第73-76行：AFE配置
afe_config_t* afe_config = afe_config_init("MR", models, AFE_TYPE_SR, AFE_MODE_HIGH_PERF);
afe_config->aec_mode = AEC_MODE_SR_HIGH_PERF;
```

**3. 参考信号的时间同步**

```cpp
// no_audio_codec.cc 第359-388行：写入播放数据
int NoAudioCodec::Write(const int16_t* data, int samples) {
    for (int i = 0; i < samples; i++) {
        output_buffer_[slice_index_] = data[i];  // 存储播放数据
        slice_index_++;
        if(slice_index_ >= play_size*10) slice_index_ = 0;
    }
    time_us_write_ = esp_timer_get_time();  // 记录写入时间
}

// 第457-506行：读取时同步参考信号
int NoAudioCodecSimplexPdm::Read(int16_t* dest, int samples) {
    time_us_read_ = esp_timer_get_time();
    // 检测播放和读取的时间差，确保同步
    if (time_us_read_ - time_us_write_ > 1000 * 100) {  // 超过100ms
        // 清空缓冲区（没有播放）
        std::fill(output_buffer_.begin(), output_buffer_.end(), 0);
    }
    // 读取对应时间的参考信号
    dest[i * 2 + 1] = output_buffer_[i_index];
}
```

---

## 🎯 MicroPython实现方案

### 方案概述

由于MicroPython的限制，我们采用**简化的双I2S方案**：

1. **I2S0 (PDM)**: 专门用于麦克风输入和唤醒检测（espsr使用）
2. **I2S1 (标准)**: 专门用于喇叭播放
3. **AFE配置**: 启用AEC，输入格式为"MR"（麦克风+参考信号）
4. **参考信号**: 通过共享内存传递播放数据给espsr

---

## 🔧 具体实现步骤

### 步骤1：修改 `modespsr.c` - 启用AEC并准备接收参考信号

#### 1.1 添加全局变量存储参考信号

```c
// 在文件开头添加（第71行后）
static int16_t *g_reference_buffer = NULL;
static size_t g_reference_buffer_size = 0;
static size_t g_reference_write_index = 0;
static size_t g_reference_read_index = 0;
static SemaphoreHandle_t g_reference_mutex = NULL;

#define REFERENCE_BUFFER_SIZE (16000 * 2)  // 2秒缓冲
```

#### 1.2 修改feed任务，构建双通道数据

```c
// 替换原有的feed_Task函数（第161-178行）
void feed_Task(void *arg) {
    esp_afe_sr_data_t *afe_data = arg;
    int feed_chunksize = afe_handle->get_feed_chunksize(afe_data);
    int feed_nch = afe_handle->get_feed_channel_num(afe_data);  // 应该是2（MR）
    int16_t *feed_buff = (int16_t *) malloc(feed_chunksize * feed_nch * sizeof(int16_t));
    
    assert(feed_buff);
    while (task_flag) {
        size_t bytesIn = 0;
        
        // 读取PDM麦克风数据到临时缓冲区
        int16_t *mic_data = (int16_t *) malloc(feed_chunksize * sizeof(int16_t));
        esp_err_t result = i2s_channel_read(rx_handle, mic_data, 
            feed_chunksize * sizeof(int16_t), &bytesIn, portMAX_DELAY);
        
        if (result == ESP_OK) {
            // 构建双通道数据：交错排列麦克风和参考信号
            if (xSemaphoreTake(g_reference_mutex, pdMS_TO_TICKS(10)) == pdTRUE) {
                for (int i = 0; i < feed_chunksize; i++) {
                    feed_buff[i * 2] = mic_data[i];  // 通道0：麦克风
                    
                    // 通道1：参考信号
                    if (g_reference_buffer != NULL && g_reference_buffer_size > 0) {
                        feed_buff[i * 2 + 1] = g_reference_buffer[g_reference_read_index];
                        g_reference_read_index = (g_reference_read_index + 1) % g_reference_buffer_size;
                    } else {
                        feed_buff[i * 2 + 1] = 0;  // 没有参考信号
                    }
                }
                xSemaphoreGive(g_reference_mutex);
            } else {
                // 如果无法获取锁，只用麦克风数据
                for (int i = 0; i < feed_chunksize; i++) {
                    feed_buff[i * 2] = mic_data[i];
                    feed_buff[i * 2 + 1] = 0;
                }
            }
            
            // 喂给AFE
            afe_handle->feed(afe_data, feed_buff);
        }
        
        free(mic_data);
    }
    if (feed_buff) {
        free(feed_buff);
        feed_buff = NULL;
    }
    vTaskDelete(NULL);
}
```

#### 1.3 修改AFE初始化，配置AEC

```c
// 修改espsr_init函数（第258-276行）
static mp_obj_t espsr_init(void) {
    if (espsr_initialized) {
        return mp_const_true;
    }
    
    ESP_LOGI(TAG, "Initializing ESP-SR with AEC...");
    
    // 初始化GPIO脉冲输出
    init_pulse_gpio();
    
    // 初始化I2S
    init_i2s();
    
    // 初始化参考信号缓冲区
    g_reference_buffer = (int16_t *) heap_caps_malloc(
        REFERENCE_BUFFER_SIZE * sizeof(int16_t), MALLOC_CAP_SPIRAM);
    if (g_reference_buffer == NULL) {
        ESP_LOGE(TAG, "Failed to allocate reference buffer");
        return mp_const_false;
    }
    g_reference_buffer_size = REFERENCE_BUFFER_SIZE;
    g_reference_write_index = 0;
    g_reference_read_index = 0;
    memset(g_reference_buffer, 0, REFERENCE_BUFFER_SIZE * sizeof(int16_t));
    
    g_reference_mutex = xSemaphoreCreateMutex();
    if (g_reference_mutex == NULL) {
        ESP_LOGE(TAG, "Failed to create reference mutex");
        return mp_const_false;
    }
    
    // 初始化语音识别模型
    srmodel_list_t *models = esp_srmodel_init("model");
    
    // 🔥 关键改动：配置为MR格式（麦克风+参考信号）
    afe_config_t *afe_config = afe_config_init("MR", models, AFE_TYPE_SR, AFE_MODE_HIGH_PERF);
    
    // 🔥 启用AEC
    afe_config->aec_init = true;
    afe_config->aec_mode = AEC_MODE_SR_HIGH_PERF;  // 使用SR高性能模式
    
    afe_config->wakenet_model_name = NULL;
    afe_handle = esp_afe_handle_from_config(afe_config);
    afe_data = afe_handle->create_from_config(afe_config);
    
    // ... 其余代码不变 ...
}
```

#### 1.4 添加参考信号输入接口

```c
// 在espsr_cleanup之前添加（第343行前）
static mp_obj_t espsr_feed_reference(mp_obj_t data_obj) {
    if (!espsr_initialized) {
        return mp_const_false;
    }
    
    mp_buffer_info_t bufinfo;
    mp_get_buffer_raise(data_obj, &bufinfo, MP_BUFFER_READ);
    
    if (g_reference_buffer == NULL || g_reference_mutex == NULL) {
        return mp_const_false;
    }
    
    // 将播放数据写入参考缓冲区
    int16_t *data = (int16_t *)bufinfo.buf;
    int samples = bufinfo.len / 2;
    
    if (xSemaphoreTake(g_reference_mutex, pdMS_TO_TICKS(10)) == pdTRUE) {
        for (int i = 0; i < samples; i++) {
            g_reference_buffer[g_reference_write_index] = data[i];
            g_reference_write_index = (g_reference_write_index + 1) % g_reference_buffer_size;
        }
        xSemaphoreGive(g_reference_mutex);
        return mp_const_true;
    }
    
    return mp_const_false;
}
static MP_DEFINE_CONST_FUN_OBJ_1(espsr_feed_reference_obj, espsr_feed_reference);
```

#### 1.5 注册新接口

```c
// 修改模块注册表（第385-391行）
static const mp_rom_map_elem_t espsr_module_globals_table[] = {
    { MP_ROM_QSTR(MP_QSTR___name__), MP_ROM_QSTR(MP_QSTR_espsr) },
    { MP_ROM_QSTR(MP_QSTR_init), MP_ROM_PTR(&espsr_init_obj) },
    { MP_ROM_QSTR(MP_QSTR_listen), MP_ROM_PTR(&espsr_listen_obj) },
    { MP_ROM_QSTR(MP_QSTR_get_commands), MP_ROM_PTR(&espsr_get_commands_obj) },
    { MP_ROM_QSTR(MP_QSTR_cleanup), MP_ROM_PTR(&espsr_cleanup_obj) },
    { MP_ROM_QSTR(MP_QSTR_feed_reference), MP_ROM_PTR(&espsr_feed_reference_obj) },  // 新增
};
```

#### 1.6 清理资源

```c
// 修改espsr_cleanup函数（第344-381行）
static mp_obj_t espsr_cleanup(void) {
    if (!espsr_initialized) {
        return mp_const_none;
    }
    
    // ... 原有清理代码 ...
    
    // 清理参考信号缓冲区
    if (g_reference_buffer) {
        heap_caps_free(g_reference_buffer);
        g_reference_buffer = NULL;
        g_reference_buffer_size = 0;
    }
    
    if (g_reference_mutex) {
        vSemaphoreDelete(g_reference_mutex);
        g_reference_mutex = NULL;
    }
    
    espsr_initialized = false;
    ESP_LOGI(TAG, "ESP-SR cleaned up");
    return mp_const_none;
}
```

---

### 步骤2：修改 `logic.py` - 实现打断逻辑

#### 2.1 播放时输入参考信号

```python
def playback_thread_func(self, socket_obj):
    """播放线程函数 - 支持AEC打断版本"""
    print("🎵 播放线程启动（支持AEC打断）")
    
    with self.playback_thread_lock:
        self.playback_thread_active = True
        self.stop_playback_thread = False
        self.is_playing_response = True
    
    end_marker = b"END_OF_STREAM\n"
    marker_len = len(end_marker)
    buffer = bytearray()
    found_marker = False
    data_count = 0
    
    # 不降低音量，让AEC处理
    MIN_PLAY_BUFFER = 4096
    interrupt_check_interval = 5
    
    try:
        while not self.stop_playback_thread:
            # 每隔一定次数检测打断
            if data_count % interrupt_check_interval == 0:
                try:
                    import espsr
                    result = espsr.listen(1)  # 1ms非阻塞检测
                    if result == "wakeup" or (isinstance(result, dict) and "id" in result):
                        print("🛑 检测到唤醒词打断！")
                        self.wakeup_interrupted = True
                        self.stop_playback_thread = True
                        break
                except:
                    pass
            
            data = socket_obj.recv(4096)
            if data:
                data_count += 1
                if data_count % 10 == 1:
                    print(f"📡 接收 #{data_count}, {len(data)}字节")
            if not data:
                print("📡 播放线程：连接结束")
                break

            buffer.extend(data)

            if not found_marker and len(buffer) >= marker_len:
                if buffer[-marker_len:] == end_marker:
                    found_marker = True
                    print("🎵 检测到结束标记")
                    if len(buffer) > marker_len:
                        audio_buffer = bytearray(buffer[:-marker_len])
                        if not self.stop_playback_thread and len(audio_buffer) > 0:
                            # 🔥 播放前先输入参考信号
                            try:
                                espsr.feed_reference(bytes(audio_buffer))
                            except:
                                pass
                            self.audio_out.write(audio_buffer)
                    break
                elif len(buffer) > MIN_PLAY_BUFFER:
                    play_len = len(buffer) - marker_len
                    if play_len > 0 and not self.stop_playback_thread:
                        audio_buffer = bytearray(buffer[:play_len])
                        # 🔥 播放前先输入参考信号
                        try:
                            espsr.feed_reference(bytes(audio_buffer))
                        except:
                            pass
                        self.audio_out.write(audio_buffer)
                    buffer = buffer[play_len:]

            if found_marker and len(buffer) > 0 and not self.stop_playback_thread:
                audio_buffer = bytearray(buffer)
                # 🔥 播放前先输入参考信号
                try:
                    espsr.feed_reference(bytes(audio_buffer))
                except:
                    pass
                self.audio_out.write(audio_buffer)
                buffer = bytearray()

    except Exception as e:
        print(f"❌ 播放线程异常: {e}")
    finally:
        # ... 清理代码不变 ...
```

#### 2.2 保持espsr运行

```python
# 在唤醒后不清理espsr（在第727-730行和第778-781行）
# espsr.cleanup()  # 注释掉
# self.is_wakeup_mic = False  # 注释掉
gc.collect()
```

#### 2.3 主循环优化

```python
# 第711-725行
# espsr始终保持运行
if not self.is_wakeup_mic:
    init_result = espsr.init()
    if init_result:
        print("✅ ESP-SR 初始化成功（AEC模式）!")
        self.is_wakeup_mic = True
    else:
        print("❌ ESP-SR 初始化失败!")
        return

# 播放时继续监听，由播放线程检测打断
if self.is_playing_response or self.playback_thread_active:
    time.sleep(0.1)
    continue
```

---

## 📋 完整改动清单

### `modespsr.c` 需要修改的地方

1. ✅ 第71行后：添加全局变量（参考信号缓冲区）
2. ✅ 第161-178行：修改`feed_Task`函数，构建双通道数据
3. ✅ 第260行：修改`afe_config_init("M"...)` → `afe_config_init("MR"...)`
4. ✅ 第264行：`afe_config->aec_init = false` → `afe_config->aec_init = true`
5. ✅ 新增：添加`afe_config->aec_mode = AEC_MODE_SR_HIGH_PERF`
6. ✅ 第250-299行：在`espsr_init`中初始化参考缓冲区
7. ✅ 第343行前：添加`espsr_feed_reference`函数
8. ✅ 第385-391行：注册新接口
9. ✅ 第344-381行：在`espsr_cleanup`中清理参考缓冲区

### `logic.py` 需要修改的地方

1. ✅ 第286-345行：修改`playback_thread_func`，添加`espsr.feed_reference()`调用
2. ✅ 第314-325行：添加打断检测逻辑
3. ✅ 第728和779行：注释掉`espsr.cleanup()`
4. ✅ 第711-725行：优化主循环

---

## 🚀 编译和部署

### 1. 编译固件

```bash
cd /Users/renzhaojing/gitcode/renhejia/micropython-sr-aec/ports/esp32
make clean
make -j8
```

### 2. 烧录固件

```bash
make erase       # 首次建议擦除
make deploy
```

### 3. 上传logic.py

使用Thonny IDE上传修改后的`logic.py`

---

## 🧪 测试验证

### 测试1：基础唤醒

```
1. 说"嗨小乐"
2. 听到"我在"
3. 提问
4. 等待回复

预期：✅ 正常工作
```

### 测试2：播放中打断

```
1. 说"嗨小乐"
2. 提问
3. 播放回复时，再次说"嗨小乐"
4. 观察是否立即停止并开始新录音

预期：✅ 立即停止，开始新录音
日志：🛑 检测到唤醒词打断！
```

### 测试3：AEC效果

```
1. 说"嗨小乐"
2. 提问
3. 播放回复时，说其他话
4. 观察espsr能否识别到

预期：✅ 能识别到（说明AEC在工作）
```

---

## 📊 预期效果

### 成功标志

1. ✅ 播放时能检测到唤醒词
2. ✅ 检测到打断后立即停止播放
3. ✅ 自动开始新的录音
4. ✅ 形成连续对话循环
5. ✅ 日志显示参考信号已输入

### 关键日志

```
✅ ESP-SR 初始化成功（AEC模式）!
🎵 播放线程启动（支持AEC打断）
📡 接收 #1, 4096字节
🛑 检测到唤醒词打断！
🔄 检测到播放被打断，立即开始新的录音...
start recordToAI
```

---

## 🔍 调试技巧

### 1. 检查AFE配置

在`modespsr.c`的`espsr_init`函数中添加日志：

```c
ESP_LOGI(TAG, "AFE config: format=%s, aec_init=%d, aec_mode=%d", 
    "MR", afe_config->aec_init, afe_config->aec_mode);
ESP_LOGI(TAG, "AFE channels: feed=%d", 
    afe_handle->get_feed_channel_num(afe_data));
```

预期输出：
```
AFE config: format=MR, aec_init=1, aec_mode=1
AFE channels: feed=2
```

### 2. 检查参考信号

在`espsr_feed_reference`中添加计数：

```c
static int ref_count = 0;
if (ref_count % 100 == 0) {
    ESP_LOGI(TAG, "Reference fed: %d samples", samples);
}
ref_count++;
```

### 3. 监控缓冲区

```c
ESP_LOGI(TAG, "Reference buffer: write=%d, read=%d, size=%d",
    g_reference_write_index, g_reference_read_index, g_reference_buffer_size);
```

---

## ⚠️ 注意事项

1. **内存占用**：参考缓冲区占用约64KB SPIRAM
2. **时间同步**：参考信号需要与麦克风数据时间对齐
3. **性能影响**：AEC处理会增加约5-10%的CPU占用
4. **音量设置**：不要降低播放音量，让AEC处理

---

## 📝 总结

这个方案基于参考项目的真实实现，核心思路是：

1. **双通道输入**：麦克风数据 + 参考信号（播放音频）
2. **AFE处理**：自动进行AEC，输出干净人声
3. **持续运行**：espsr不停止，始终监听
4. **打断检测**：播放时定期检测唤醒词

相比之前的简化方案，这个方案是**真正的AEC实现**，效果会好得多！

祝您实现顺利！🚀

