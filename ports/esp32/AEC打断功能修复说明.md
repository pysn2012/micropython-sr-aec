# AEC 打断功能修复说明

## 问题描述

播放回复音频过程中，对着麦克风说话或唤醒，**并没有打断播放**。

从日志可以看出：
```
start recordToAI
开始流式录音+传输(带静音检测，使用ESP-SR缓冲区)...
录音完成，共发送 32768 字节
🎧 启动播放线程处理服务器响应...
🛑 停止ESP-SR录音模式...        ← 问题：录音模式被停止了
✅ 录音模式已停止
end recordToAI
📡 接收 #1, 4096字节              ← 开始播放，但录音已停止
...
📡 接收 #131, 4096字节            ← 播放期间无法检测到唤醒词
```

## 问题原因

**录音模式在播放开始前就被停止了**

1. `recordToAI()` 在录音完成后调用 `self.deinit_record_mic()`
2. `deinit_record_mic()` 停止了 ESP-SR 的录音模式
3. 播放线程启动时，录音已经停止
4. 播放线程中的 `espsr.listen()` 无法检测到唤醒词，因为没有音频输入

## 解决方案

**保持录音模式在播放期间持续开启**

核心思路：
1. 录音完成后，**不要停止录音模式**
2. 在播放开始前，**重新启用录音模式**（清空旧数据缓冲区）
3. 播放期间，录音模式持续运行，`espsr.listen()` 可以检测到唤醒词
4. 播放结束后：
   - 如果没有打断：停止录音模式
   - 如果有打断：保持录音模式开启，继续对话

## 具体修改

### 1. 修改 `recordToAI()` - 不停止录音模式

**修改前**：
```python
def recordToAI(self):
    if not self.is_init_record_mic:
        self.initRecordMic()
    
    self.record_and_send()
    
    # 正常流程：回环结束，释放mic资源
    self.deinit_record_mic()  # ❌ 这里停止了录音！
    print("end recordToAI")
```

**修改后**：
```python
def recordToAI(self):
    if not self.is_init_record_mic:
        self.initRecordMic()
    
    self.record_and_send()
    
    # ⚠️ 重要：不要在这里停止录音！播放期间需要保持录音模式以检测打断
    # 录音模式会在播放线程结束后由主循环清理
    print("end recordToAI (录音模式保持开启)")
```

### 2. 修改 `process_server_response()` - 重新启用录音模式

**修改前**：
```python
def process_server_response(self, s):
    """处理服务器响应 - 启动播放线程"""
    print("🎧 启动播放线程处理服务器响应...")
    
    # 启动播放线程
    _thread.start_new_thread(self.playback_thread_func, (s,))
```

**修改后**：
```python
def process_server_response(self, s):
    """处理服务器响应 - 启动播放线程"""
    print("🎧 启动播放线程处理服务器响应...")
    
    # 重新启用录音模式以清空旧数据，为AEC打断检测做准备
    try:
        print("🔄 重新启用录音模式（清空缓冲区）...")
        espsr.stop_recording()
        time.sleep(0.05)  # 短暂延时
        espsr.start_recording()
        print("✅ 录音模式已重新启用")
    except Exception as e:
        print(f"⚠️ 重新启用录音模式失败: {e}")
    
    # 启动播放线程
    _thread.start_new_thread(self.playback_thread_func, (s,))
```

**为什么要重新启用？**
- 清空录音缓冲区中的旧数据（录音阶段的残留）
- 确保播放期间检测的是新的音频数据（用户的打断语音）
- 避免误触发（旧数据可能被误识别为唤醒词）

### 3. 修改 `playback_thread_func()` - 播放结束时停止录音

**修改前**：
```python
def playback_thread_func(self, socket_obj):
    try:
        # ... 播放逻辑 ...
    except Exception as e:
        print(f"❌ 播放线程异常: {e}")
    finally:
        self.playback_thread_active = False
        self.is_playing_response = False
        print("✅ 播放线程正常结束")
        socket_obj.close()
        print("🎵 播放线程结束")
```

**修改后**：
```python
def playback_thread_func(self, socket_obj):
    try:
        # ... 播放逻辑 ...
    except Exception as e:
        print(f"❌ 播放线程异常: {e}")
    finally:
        self.playback_thread_active = False
        self.is_playing_response = False
        
        if self.stop_playback_thread:
            if self.wakeup_interrupted:
                print("🤖 小乐：检测到打断，录音模式保持开启...")
            else:
                print("🤖 小乐：播放被手动停止")
        else:
            print("✅ 播放线程正常结束")
            # 播放正常结束且没有打断，停止录音模式
            if not self.wakeup_interrupted:
                print("🛑 播放完成，停止录音模式...")
                self.deinit_record_mic()
        
        socket_obj.close()
        print("🎵 播放线程结束")
```

## 完整数据流程

```
用户唤醒 "嗨小乐"
    ↓
启动录音模式 (espsr.start_recording)
    ↓
录音并上传到服务器 (record_and_send)
    ↓
录音完成，保持录音模式开启 ✅
    ↓
重新启用录音模式（清空缓冲区）
    ↓
启动播放线程
    ↓
播放回复音频 + 检测打断
    ├─→ 检测到唤醒词 → 停止播放 → 保持录音模式 → 重新录音
    └─→ 正常播放完成 → 停止录音模式
```

## 关键日志对比

### 修复前（无法打断）：
```
start recordToAI
开始流式录音+传输...
录音完成
🛑 停止ESP-SR录音模式...     ← 录音停止
✅ 录音模式已停止
end recordToAI
📡 接收 #1, 4096字节          ← 播放中无法检测唤醒
...
✅ 播放线程正常结束
```

### 修复后（可以打断）：
```
start recordToAI
开始流式录音+传输...
录音完成
end recordToAI (录音模式保持开启)  ← 录音继续
🔄 重新启用录音模式（清空缓冲区）...
✅ 录音模式已重新启用
📡 接收 #1, 4096字节
📡 接收 #11, 4096字节
🛑 检测到唤醒词打断！              ← 成功检测到打断 ✅
🤖 小乐：检测到打断，录音模式保持开启...
🔄 检测到打断，准备重新录音
```

## 技术要点

### 1. 为什么要清空缓冲区？

录音阶段会在缓冲区留下音频数据，如果不清空：
- 播放期间首次检测可能使用旧数据
- 可能导致误触发或延迟响应
- 影响 AEC 效果

### 2. 为什么播放结束要判断 `wakeup_interrupted`？

- **有打断**：用户还想继续对话，保持录音模式
- **无打断**：对话结束，释放资源

### 3. 录音模式的生命周期

```
唤醒 → 启动录音 → 录音 → [保持] → 清空缓冲 → 播放+检测 → 结束/打断
```

## 测试验证

修复后应该看到：

1. **正常对话**：
   ```
   唤醒 → 录音 → 播放 → 结束 → 停止录音
   ```

2. **打断场景**：
   ```
   唤醒 → 录音 → 播放 → 检测到打断 → 停止播放 → 重新录音
   ```

3. **连续打断**：
   ```
   唤醒 → 录音 → 播放 → 打断1 → 录音 → 播放 → 打断2 → 录音 → ...
   ```

## 编译和测试

```bash
cd /Users/renzhaojing/gitcode/renhejia/micropython-sr-aec/ports/esp32
idf.py build
idf.py flash
idf.py monitor
```

测试步骤：
1. 说 "嗨小乐" 唤醒设备
2. 提问并等待回复
3. **在播放回复的过程中**，再次说 "嗨小乐" 或直接说话
4. 观察设备是否停止播放并重新开始录音

## 预期效果

✅ 播放期间可以随时打断
✅ 打断后立即重新录音
✅ 支持连续多次打断
✅ 播放正常结束后自动停止录音
✅ 资源管理正确（无泄漏）

## 相关文件

- `ports/esp32/modules/logic.py` - 主要修改文件
- `ports/esp32/modespsr.c` - ESP-SR 录音接口
- `I2S资源冲突修复说明.md` - 相关问题修复

## 更新日期

2025-10-27

