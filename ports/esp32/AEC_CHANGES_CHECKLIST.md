# AEC打断功能改动清单

## 📋 必须修改的文件

### ✅ 1. `modespsr.c` - 1处修改

**位置：**第264行

```c
// 修改前
afe_config->aec_init = false;

// 修改后  
afe_config->aec_init = true;  // 启用AEC回声消除
```

**说明：**启用ESP-SR的AEC功能

---

### ✅ 2. `logic.py` - 已完成所有修改

#### 改动点1：第304行 - 启用音量降低
```python
volume_reduction_factor = 0.5  # 从1.0改为0.5，帮助AEC识别
```

#### 改动点2：第314-325行 - 添加打断检测
```python
# 🔥 每隔一定次数检测打断信号
if data_count % interrupt_check_interval == 0:
    # 检测espsr是否有新的唤醒或命令词（非阻塞）
    try:
        import espsr
        result = espsr.listen(1)  # 1ms超时，非阻塞检测
        if result == "wakeup" or (isinstance(result, dict) and "id" in result):
            print("🛑 检测到唤醒词打断！")
            self.wakeup_interrupted = True
            self.stop_playback_thread = True
            break
    except:
        pass
```

#### 改动点3：第728和779行 - 保持espsr运行
```python
# 注释掉cleanup调用
# espsr.cleanup()  # 注释掉
# self.is_wakeup_mic = False  # 注释掉
```

#### 改动点4：第752-757和794-799行 - 处理打断
```python
# 🔥 检查是否被打断，如果被打断则立即开始新的录音
if self.wakeup_interrupted:
    print("🔄 检测到播放被打断，立即开始新的录音...")
    self.wakeup_interrupted = False
    # espsr保持运行，直接开始录音
    self.recordToAI()
```

#### 改动点5：第711-725行 - 主循环优化
```python
# 🔥 AEC模式：espsr始终保持运行，不进行清理和重新初始化
if not self.is_wakeup_mic:
    init_result = espsr.init()
    if init_result:
        print("✅ ESP-SR 初始化成功!")
        self.is_wakeup_mic = True
    else:
        print("❌ ESP-SR 初始化失败!")
        return

# 🔥 AEC模式：播放时不暂停监听，而是降低检测频率
if self.is_playing_response or self.playback_thread_active:
    # 播放时继续监听但降低频率，由播放线程内部进行打断检测
    time.sleep(0.1)  # 短暂休眠，让播放线程执行
    continue
```

---

## 🔨 编译和部署

### 步骤1：编译固件（修改modespsr.c后需要）
```bash
cd /Users/renzhaojing/gitcode/renhejia/micropython-sr-aec/ports/esp32
make clean
make -j8
```

### 步骤2：烧录固件
```bash
make erase       # 首次烧录建议先擦除
make deploy      # 烧录固件
```

### 步骤3：上传logic.py
使用Thonny IDE：
1. 打开 `ports/esp32/modules/logic.py`
2. 连接设备
3. 上传文件到设备的 `/` 目录

---

## 🧪 测试验证

### 测试1：正常唤醒和对话
```
1. 说"嗨小乐"
2. 听到"我在"
3. 说问题（如"今天天气怎么样"）
4. 等待并听取回复

预期：✅ 正常工作
```

### 测试2：播放时打断
```
1. 说"嗨小乐"并提问
2. 等待开始播放回复
3. 播放过程中再次说"嗨小乐"
4. 观察播放是否立即停止
5. 观察是否开始新的录音

预期：✅ 播放立即停止，开始新录音
```

### 测试3：连续打断
```
1. 说"嗨小乐"并提问
2. 回复播放时打断
3. 再次提问
4. 再次打断
5. 重复3-4次

预期：✅ 每次打断都能正常工作
```

---

## 📊 关键日志标记

### 正常播放日志
```
🎵 播放线程启动（支持AEC打断）
📡 接收 #1, 4096字节
📡 接收 #6, 4096字节
...
🎵 检测到结束标记
✅ 播放线程正常结束
```

### 打断成功日志
```
🎵 播放线程启动（支持AEC打断）
📡 接收 #1, 4096字节
🛑 检测到唤醒词打断！
🛑 播放线程被停止
🤖 小乐：您好，请继续说话...
🔄 检测到播放被打断，立即开始新的录音...
start recordToAI
```

---

## ⚙️ 可调参数

### 如果打断不够灵敏
```python
# logic.py 第304行
volume_reduction_factor = 0.25  # 降低到25%

# logic.py 第309行
interrupt_check_interval = 3  # 每3个包检测一次
```

### 如果有误触发
```python
# logic.py 第309行
interrupt_check_interval = 10  # 每10个包检测一次
```

### 如果播放卡顿
```python
# logic.py 第306行
MIN_PLAY_BUFFER = 8192  # 增大到8KB

# logic.py 第61行（__init__方法）
ibuf=16384  # 增大到16KB
```

---

## 📝 文件清单

### 需要修改
- ✅ `ports/esp32/modespsr.c` - 1行修改
- ✅ `ports/esp32/modules/logic.py` - 5处修改（已完成）

### 需要上传到设备
- ✅ `logic.py` - 上传到设备根目录

### 参考文档
- ✅ `ports/esp32/AEC_INTERRUPT_IMPLEMENTATION_GUIDE.md` - 详细实现指南
- ✅ `ports/esp32/AEC_CHANGES_CHECKLIST.md` - 本清单

---

## 🎯 下一步行动

1. [ ] 修改 `modespsr.c` 第264行：`afe_config->aec_init = true;`
2. [ ] 编译固件：`make clean && make -j8`
3. [ ] 烧录固件：`make deploy`
4. [ ] 上传 `logic.py` 到设备
5. [ ] 测试唤醒功能
6. [ ] 测试打断功能
7. [ ] 根据效果调整参数

---

## ✅ 完成标志

当你看到以下日志，说明AEC打断功能已经正常工作：

```
🎵 播放线程启动（支持AEC打断）
📡 接收 #1, 4096字节
📡 接收 #6, 4096字节
🛑 检测到唤醒词打断！          ← 这行表示成功检测到打断
🛑 播放线程被停止
🔄 检测到播放被打断，立即开始新的录音...  ← 这行表示开始新对话
start recordToAI
```

**祝测试顺利！** 🚀

