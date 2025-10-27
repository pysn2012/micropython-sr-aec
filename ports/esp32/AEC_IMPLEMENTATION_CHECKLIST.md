# AEC打断功能实施清单

基于参考项目 xiaozhi-esp32-pan 的真实AEC实现

---

## ✅ 改动清单

### 📄 文件1：`modespsr.c` - 9处改动

#### ✅ 改动1：添加全局变量（第71行后）
```c
// 在第71行 static bool espsr_initialized = false; 后添加
static int16_t *g_reference_buffer = NULL;
static size_t g_reference_buffer_size = 0;
static size_t g_reference_write_index = 0;
static size_t g_reference_read_index = 0;
static SemaphoreHandle_t g_reference_mutex = NULL;

#define REFERENCE_BUFFER_SIZE (16000 * 2)  // 2秒缓冲
```

#### ✅ 改动2：修改feed_Task函数（替换第161-178行）
<details>
<summary>点击展开完整代码</summary>

```c
void feed_Task(void *arg) {
    esp_afe_sr_data_t *afe_data = arg;
    int feed_chunksize = afe_handle->get_feed_chunksize(afe_data);
    int feed_nch = afe_handle->get_feed_channel_num(afe_data);
    int16_t *feed_buff = (int16_t *) malloc(feed_chunksize * feed_nch * sizeof(int16_t));
    
    assert(feed_buff);
    while (task_flag) {
        size_t bytesIn = 0;
        int16_t *mic_data = (int16_t *) malloc(feed_chunksize * sizeof(int16_t));
        esp_err_t result = i2s_channel_read(rx_handle, mic_data, 
            feed_chunksize * sizeof(int16_t), &bytesIn, portMAX_DELAY);
        
        if (result == ESP_OK) {
            if (xSemaphoreTake(g_reference_mutex, pdMS_TO_TICKS(10)) == pdTRUE) {
                for (int i = 0; i < feed_chunksize; i++) {
                    feed_buff[i * 2] = mic_data[i];
                    if (g_reference_buffer != NULL && g_reference_buffer_size > 0) {
                        feed_buff[i * 2 + 1] = g_reference_buffer[g_reference_read_index];
                        g_reference_read_index = (g_reference_read_index + 1) % g_reference_buffer_size;
                    } else {
                        feed_buff[i * 2 + 1] = 0;
                    }
                }
                xSemaphoreGive(g_reference_mutex);
            } else {
                for (int i = 0; i < feed_chunksize; i++) {
                    feed_buff[i * 2] = mic_data[i];
                    feed_buff[i * 2 + 1] = 0;
                }
            }
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
</details>

#### ✅ 改动3：修改AFE配置格式（第260行）
```c
// 修改前
afe_config_t *afe_config = afe_config_init("M", models, AFE_TYPE_SR, AFE_MODE_LOW_COST);

// 修改后
afe_config_t *afe_config = afe_config_init("MR", models, AFE_TYPE_SR, AFE_MODE_HIGH_PERF);
```

#### ✅ 改动4：启用AEC（第264行）
```c
// 修改前
afe_config->aec_init = false;

// 修改后
afe_config->aec_init = true;
```

#### ✅ 改动5：设置AEC模式（第264行后新增）
```c
// 在第264行后添加
afe_config->aec_mode = AEC_MODE_SR_HIGH_PERF;
```

#### ✅ 改动6：初始化参考缓冲区（在espsr_init函数中，第256行init_i2s()后）
```c
// 在 init_i2s(); 后添加
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
    heap_caps_free(g_reference_buffer);
    g_reference_buffer = NULL;
    return mp_const_false;
}
```

#### ✅ 改动7：添加feed_reference函数（第343行espsr_cleanup之前）
<details>
<summary>点击展开完整代码</summary>

```c
static mp_obj_t espsr_feed_reference(mp_obj_t data_obj) {
    if (!espsr_initialized) {
        return mp_const_false;
    }
    
    mp_buffer_info_t bufinfo;
    mp_get_buffer_raise(data_obj, &bufinfo, MP_BUFFER_READ);
    
    if (g_reference_buffer == NULL || g_reference_mutex == NULL) {
        return mp_const_false;
    }
    
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
</details>

#### ✅ 改动8：注册新接口（第385-391行）
```c
static const mp_rom_map_elem_t espsr_module_globals_table[] = {
    { MP_ROM_QSTR(MP_QSTR___name__), MP_ROM_QSTR(MP_QSTR_espsr) },
    { MP_ROM_QSTR(MP_QSTR_init), MP_ROM_PTR(&espsr_init_obj) },
    { MP_ROM_QSTR(MP_QSTR_listen), MP_ROM_PTR(&espsr_listen_obj) },
    { MP_ROM_QSTR(MP_QSTR_get_commands), MP_ROM_PTR(&espsr_get_commands_obj) },
    { MP_ROM_QSTR(MP_QSTR_cleanup), MP_ROM_PTR(&espsr_cleanup_obj) },
    { MP_ROM_QSTR(MP_QSTR_feed_reference), MP_ROM_PTR(&espsr_feed_reference_obj) },  // 新增这行
};
```

#### ✅ 改动9：清理资源（在espsr_cleanup函数末尾，第378行return之前）
```c
// 在 espsr_initialized = false; 后，return mp_const_none; 前添加
if (g_reference_buffer) {
    heap_caps_free(g_reference_buffer);
    g_reference_buffer = NULL;
    g_reference_buffer_size = 0;
}

if (g_reference_mutex) {
    vSemaphoreDelete(g_reference_mutex);
    g_reference_mutex = NULL;
}
```

---

### 📄 文件2：`logic.py` - 4处改动

#### ✅ 改动1：修改playback_thread_func（第286-345行）

关键点：在每次播放前调用`espsr.feed_reference()`

```python
# 在3个位置添加参考信号输入：

# 位置1：播放最后音频块前（约第326行）
if not self.stop_playback_thread and len(audio_buffer) > 0:
    try:
        espsr.feed_reference(bytes(audio_buffer))  # 新增
    except:
        pass
    self.audio_out.write(audio_buffer)

# 位置2：播放中间音频块前（约第334行）
audio_buffer = bytearray(buffer[:play_len])
try:
    espsr.feed_reference(bytes(audio_buffer))  # 新增
except:
    pass
self.audio_out.write(audio_buffer)

# 位置3：播放剩余音频前（约第340行）
audio_buffer = bytearray(buffer)
try:
    espsr.feed_reference(bytes(audio_buffer))  # 新增
except:
    pass
self.audio_out.write(audio_buffer)
```

#### ✅ 改动2：添加打断检测（在playback_thread_func中，约第314-325行）
```python
# 在 while not self.stop_playback_thread 循环开始处添加
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
```

#### ✅ 改动3：保持espsr运行（第728和779行）
```python
# 注释掉这两行
# espsr.cleanup()  
# self.is_wakeup_mic = False
gc.collect()
```

#### ✅ 改动4：优化主循环（第711-725行）
```python
# espsr只初始化一次，后续保持运行
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

## 🔨 编译部署流程

### 1️⃣ 修改代码
- [ ] 修改 `modespsr.c`（9处）
- [ ] 修改 `logic.py`（4处）

### 2️⃣ 编译固件
```bash
cd /Users/renzhaojing/gitcode/renhejia/micropython-sr-aec/ports/esp32
make clean
make -j8
```

### 3️⃣ 烧录固件
```bash
# 首次建议擦除
make erase
make deploy
```

### 4️⃣ 上传Python文件
- [ ] 使用Thonny上传 `logic.py` 到设备

---

## 🧪 测试验证

### 测试1：基础功能
```
✅ 说"嗨小乐" → 能唤醒
✅ 提问 → 能录音
✅ 播放回复 → 有声音
```

### 测试2：AEC效果
```
✅ 播放时说"嗨小乐" → 能检测到
✅ 日志显示：🛑 检测到唤醒词打断！
✅ 播放立即停止
✅ 自动开始新录音
```

### 关键日志检查
```
✅ ESP-SR 初始化成功（AEC模式）!
✅ AFE config: format=MR, aec_init=1
✅ AFE channels: feed=2
✅ 🎵 播放线程启动（支持AEC打断）
✅ 🛑 检测到唤醒词打断！
```

---

## 🐛 常见问题

### 问题1：编译错误
```
error: 'AEC_MODE_SR_HIGH_PERF' undeclared
```
**解决**：检查ESP-SR版本，可能需要使用 `AEC_MODE_VOIP_HIGH_PERF`

### 问题2：运行时错误
```
Failed to allocate reference buffer
```
**解决**：检查SPIRAM配置，确保有足够的SPIRAM可用

### 问题3：无法检测打断
**检查**：
1. 日志是否显示 "AFE channels: feed=2"
2. 是否调用了 `espsr.feed_reference()`
3. AEC是否正确启用

---

## 📋 核心原理回顾

```
麦克风数据 ──┐
             ├──> AFE (AEC) ──> 干净人声 ──> MultiNet ──> 唤醒词检测
参考信号 ────┘
(播放音频)
```

**关键点**：
1. ✅ AFE配置为"MR"格式（麦克风+参考）
2. ✅ feed_Task构建双通道数据
3. ✅ 播放时通过`espsr.feed_reference()`输入参考信号
4. ✅ espsr持续运行，不停止
5. ✅ 播放线程定期检测打断

---

## ✅ 完成确认

全部完成后，您应该看到：
- [x] 固件成功编译
- [x] 设备正常启动
- [x] 能正常唤醒和对话
- [x] **播放时能检测到唤醒词并打断**
- [x] 形成连续的对话循环

**恭喜！AEC打断功能实现成功！** 🎉

