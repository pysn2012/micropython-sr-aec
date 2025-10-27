# I2S 资源冲突修复说明

## 问题描述

用户在第一次唤醒后尝试录音时遇到以下错误：

```
E (18130) i2s_common: i2s_new_channel(972): no available channel found
❌ 初始化失败: (-261, 'ESP_ERR_NOT_FOUND')
```

之后设备无法再次唤醒。

## 问题原因

**I2S 资源冲突**：ESP32-S3 的 I2S 硬件资源有限，存在以下资源占用情况：

1. **ESP-SR (modespsr.c)** 在初始化时创建了 **I2S_NUM_0** 接收通道用于麦克风输入
2. **logic.py** 的 `initRecordMic()` 也尝试创建 **I2S(0)** 用于录音
3. **logic.py** 的 `__init__` 中创建了 **I2S(1)** 用于音频播放

当 `initRecordMic()` 尝试创建第二个 I2S(0) 实例时，由于 ESP-SR 已经占用了 I2S_NUM_0，导致 "no available channel found" 错误。

## 解决方案

**使用共享录音缓冲区，避免重复创建 I2S 实例**

核心思路：
- ESP-SR 已经在使用 I2S(0) 进行音频采集
- 在 C 层添加一个录音数据缓冲区
- ESP-SR 的 `feed_Task` 将麦克风数据同时写入这个缓冲区
- Python 层通过新的接口读取这个缓冲区，而不是创建新的 I2S 实例

## 具体修改

### 1. C 层修改 (`modespsr.c`)

#### 1.1 添加全局录音缓冲区

```c
// 录音数据缓冲区（供Python层读取，避免I2S冲突）
static int16_t *g_record_buffer = NULL;
static size_t g_record_buffer_size = 0;
static size_t g_record_write_index = 0;
static size_t g_record_read_index = 0;
static SemaphoreHandle_t g_record_mutex = NULL;
static bool g_recording_enabled = false;  // 录音使能标志
#define RECORD_BUFFER_SIZE (16000 * 10)  // 10秒缓冲 (16kHz采样率)
```

#### 1.2 在 `feed_Task` 中写入录音数据

```c
// 如果录音已启用，将麦克风数据写入录音缓冲区
if (g_recording_enabled && g_record_mutex != NULL && 
    xSemaphoreTake(g_record_mutex, pdMS_TO_TICKS(5)) == pdTRUE) {
    
    if (g_record_buffer != NULL && g_record_buffer_size > 0) {
        for (int i = 0; i < feed_chunksize; i++) {
            g_record_buffer[g_record_write_index] = mic_data[i];
            g_record_write_index = (g_record_write_index + 1) % g_record_buffer_size;
            
            // 如果写指针追上读指针，说明缓冲区满了，覆盖最旧的数据
            if (g_record_write_index == g_record_read_index) {
                g_record_read_index = (g_record_read_index + 1) % g_record_buffer_size;
            }
        }
    }
    xSemaphoreGive(g_record_mutex);
}
```

#### 1.3 添加三个新的 MicroPython 接口

```python
# 启用录音模式
espsr.start_recording()  -> bool

# 停止录音模式
espsr.stop_recording()   -> bool

# 读取录音数据
bytes_read = espsr.read_audio(buffer)  -> int
```

#### 1.4 在 `espsr_init` 中初始化录音缓冲区

```c
// 初始化录音数据缓冲区
g_record_buffer = (int16_t *) heap_caps_malloc(
    RECORD_BUFFER_SIZE * sizeof(int16_t), MALLOC_CAP_SPIRAM);
g_record_mutex = xSemaphoreCreateMutex();
```

#### 1.5 在 `espsr_cleanup` 中清理录音缓冲区

```c
// 清理录音数据缓冲区
if (g_record_buffer) {
    heap_caps_free(g_record_buffer);
    g_record_buffer = NULL;
}
if (g_record_mutex) {
    vSemaphoreDelete(g_record_mutex);
    g_record_mutex = NULL;
}
```

### 2. Python 层修改 (`logic.py`)

#### 2.1 简化 `initRecordMic()`

**修改前**：创建 I2S(0) 实例
```python
def initRecordMic(self):
    self.mic = machine.I2S(
        0,
        sck=4, ws=4, sd=5,
        mode=machine.I2S.RX,
        format=machine.I2S.PDM,
        rate=16000,
        ibuf=1024
    )
```

**修改后**：仅启用 ESP-SR 的录音模式
```python
def initRecordMic(self):
    print("🎙️ 启用ESP-SR录音模式...")
    result = espsr.start_recording()
    if result:
        print("✅ 录音模式已启用")
        self.is_init_record_mic = True
```

#### 2.2 修改 `record_and_send()`

**修改前**：从 I2S 实例读取
```python
bytes_read = i2s_mic.readinto(buffer)
```

**修改后**：从 ESP-SR 缓冲区读取
```python
bytes_read = espsr.read_audio(buffer)

# 如果没有数据，等待一下再读取
if bytes_read == 0:
    time.sleep_ms(10)
    continue
```

#### 2.3 简化 `deinit_record_mic()`

**修改前**：清理 I2S 实例
```python
def deinit_record_mic(self):
    if self.mic:
        self.mic.deinit()
        self.mic = None
```

**修改后**：停止 ESP-SR 录音模式
```python
def deinit_record_mic(self):
    print("🛑 停止ESP-SR录音模式...")
    espsr.stop_recording()
    print("✅ 录音模式已停止")
```

#### 2.4 移除 `self.mic` 变量

- 从 `__init__` 中移除 `self.mic = None`
- 更新所有相关函数，移除 `i2s_mic` 参数

## 技术优势

1. **资源高效**：共享同一个 I2S 通道，避免资源冲突
2. **简化逻辑**：Python 层不需要管理 I2S 硬件初始化
3. **更好的稳定性**：减少硬件初始化/清理操作，降低出错概率
4. **支持 AEC**：ESP-SR 的 I2S 仍然可以正常工作，支持 AEC 功能
5. **实时性好**：环形缓冲区设计，支持连续数据流

## 数据流程

```
麦克风 (PDM)
    ↓
ESP-SR I2S(0) 读取
    ↓
feed_Task
    ├─→ 构建双通道数据 (Mic + Reference) → AFE处理 → 唤醒词/命令词检测
    └─→ 写入录音缓冲区 (g_record_buffer)
            ↓
        espsr.read_audio()
            ↓
        logic.py 读取并发送到服务器
```

## 测试验证

修改后，设备应该：
1. ✅ 正常唤醒
2. ✅ 成功录音并上传到服务器
3. ✅ 播放服务器返回的音频
4. ✅ 支持播放时的 AEC 打断
5. ✅ 可以连续多次唤醒和对话

## 编译指令

```bash
cd ports/esp32
idf.py build
idf.py flash
```

## 注意事项

1. **缓冲区大小**：当前设置为 10 秒 (160000 样本)，可根据实际需求调整 `RECORD_BUFFER_SIZE`
2. **PSRAM 依赖**：录音缓冲区使用 PSRAM，确保设备有足够的 PSRAM
3. **线程安全**：使用 mutex 保护缓冲区读写，确保线程安全
4. **环形缓冲区**：写指针追上读指针时会覆盖旧数据，确保及时读取

## 相关文件

- `ports/esp32/modespsr.c` - ESP-SR C 模块
- `ports/esp32/modules/logic.py` - 主要业务逻辑
- `ports/esp32/编译指南.md` - 编译详细步骤

## 问题排查

如果仍然遇到问题，检查：

1. **ESP-SR 是否正常初始化**：查看启动日志中的 "Initializing ESP-SR with AEC..."
2. **录音缓冲区是否分配成功**：查看 "Record buffer allocated" 日志
3. **录音模式是否成功启用**：查看 "Recording started" 日志
4. **是否有数据可读**：检查 `espsr.read_audio()` 的返回值

## 更新日期

2025-10-27

