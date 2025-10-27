"""
MicroPython 语音唤醒+命令词识别 demo
完全参照project-i2s-wakup-new，支持唤醒词（嗨，乐鑫）和命令词识别
"""
import machine
import espsr
import time
import gc
import math
import array
from machine import Pin, I2S
import wav_data  # 导入分块存储的音频数据
import network
import sys
import socket
import struct
import _thread


# 录音参数
SAMPLE_RATE = 16000  # 采样率 16kHz
RECORD_IBUF = 2048  # 录音缓冲区大小
RECORD_SECONDS = 3   # 录音时长 3 秒
BUFFER_SIZE = 1024    # 每次读取的缓冲区大小

# Wi-Fi 配置
WIFI_SSID = "LETIANPAI"
WIFI_PASSWORD = "Renhejia0801"
# WIFI_SSID = "ZTE_E64FF6"
# WIFI_PASSWORD = "1234567890"

# 测试
SERVER_IP = "192.168.110.135"  # 替换为你的服务器IP
SERVER_PORT = 9018

# SERVER_IP = "81.70.5.55"  # 替换为你的服务器IP
# SERVER_PORT = 9018

SILENCE_THRESHOLD = 200       # 静音阈值，根据实际环境调整
MIN_SILENCE_DURATION = 1.0    # 持续静音多长时间才判定为结束(秒)
SAMPLE_WINDOW_SIZE = 16000      # 每次分析的样本数(8000Hz*0.1s=800 samples)

class SensorSystem:
    def __init__(self):

        # I2S引脚配置（根据你的硬件调整）
        I2S_BCK_PIN = 15
        I2S_WS_PIN = 16
        I2S_SD_PIN = 7

        # 初始化I2S
        self.audio_out = I2S(
            1,
            sck=Pin(I2S_BCK_PIN),
            ws=Pin(I2S_WS_PIN),
            sd=Pin(I2S_SD_PIN),
            mode=I2S.TX,
            bits=16,
            format=I2S.MONO,
            rate=16000,  # 根据你的WAV文件采样率调整
            ibuf=2048    # 缓冲区大小
        )

        self.is_init_record_mic = False
        self.mic = None
        self.sample_rate = 16000
        self.buffer = bytearray(512)  # 小缓冲区
        self.volume_gain = 1.0
        self.clip_threshold = 32000

        # 初始化原有硬件...
        self.is_playing = False  # 播放状态标志
        self.is_recording = False  # 录音状态标志
        self.should_stop = False  # 中断标志
        
        # 新增：播放打断相关状态
        self.is_playing_response = False  # 是否正在播放回复
        self.wakeup_interrupted = False   # 是否被唤醒词打断
        self.interrupt_check_interval = 0.1  # 打断检测间隔（秒）
        self.last_interrupt_check = 0    # 上次检测时间
        
        # 线程控制相关
        self.playback_thread_active = False  # 播放线程是否活跃
        self.stop_playback_thread = False    # 停止播放线程标志
        self.playback_thread_lock = _thread.allocate_lock()  # 线程锁



    def play_wav_chunked(self, wav_chunks):
        # 跳过WAV文件头（假设前44字节是头信息）
        header_size = 44
        bytes_played = 0

        for chunk in wav_chunks:
            # 如果是第一个块，跳过头部
            if bytes_played == 0:
                if len(chunk) > header_size:
                    self.audio_out.write(chunk[header_size:])
                    bytes_played += len(chunk) - header_size
                else:
                    bytes_played += len(chunk)
            else:
                self.audio_out.write(chunk)
                bytes_played += len(chunk)

        # 等待播放完成
        time.sleep(0.1)


    def playWozai(self):
        print("play wozai")
        # 播放音频（分块播放）
        self.play_wav_chunked(wav_data.wav_data)
        gc.collect()

    def initRecordMic(self):
        """初始化麦克风和扬声器"""
        try:
            # 先清理可能存在的旧实例
            self.deinit_record_mic()
            time.sleep(0.1)

            print("初始化PDM麦克风...")
            self.mic = machine.I2S(
                0,
                sck=4,      # PDM麦克风时钟
                ws=4,       # PDM不需要WS，但需要设置
                sd=5,       # PDM麦克风数据
                mode=machine.I2S.RX,
                bits=16,
                format=machine.I2S.PDM,  # 使用PDM格式
                rate=self.sample_rate,
                ibuf=1024
            )
            print("✅ PDM麦克风初始化成功")
            self.is_init_record_mic = True
        except Exception as e:
            print(f"❌ 初始化失败: {e}")
            self.deinit_record_mic()
            raise

    def calculate_energy(self, audio_chunk):
        """计算音频片段的能量"""
        samples = array.array('h', audio_chunk)  # 将字节转换为16位有符号整数数组
        sum_squares = sum(sample*sample for sample in samples)
        return sum_squares / len(samples) if len(samples) > 0 else 0

    def record_and_send(self, i2s_mic, i2s_spk):
        """带静音检测的流式录音和传输"""
        self.is_recording = True
        self.record_finish = False
        print("connect tcp server ...")

        try:
            try:
                # 创建TCP连接
                s = socket.socket(socket.AF_INET, socket.SOCK_STREAM)
                s.connect((SERVER_IP, SERVER_PORT))
            except Exception as e:
                print("connect_tct except:", str(e))
                sys.print_exception(e)
                return

            # 录音初始化
            # self.init_i2s_in()
            # time.sleep_ms(100)

            total_bytes = SAMPLE_RATE * RECORD_SECONDS * 2
            recorded_bytes = 0
            start_time = time.ticks_ms()
            buffer = bytearray(1024)

            # 静音检测相关变量
            silent_samples = 0
            audio_window = bytearray()
            silence_threshold_windows = int(MIN_SILENCE_DURATION / (SAMPLE_WINDOW_SIZE/SAMPLE_RATE))

            print("开始流式录音+传输(带静音检测)...")
            while recorded_bytes < total_bytes and not self.should_stop:

                # 读取音频数据
                bytes_read = i2s_mic.readinto(buffer)
                if bytes_read > 0:
                    try:
                        # 发送音频数据
                        s.send(struct.pack('<I', bytes_read))
                        s.send(buffer[:bytes_read])
                        recorded_bytes += bytes_read

                        # 静音检测处理
                        audio_window.extend(buffer[:bytes_read])

                        # 当积累足够样本时进行检测
                        while len(audio_window) >= SAMPLE_WINDOW_SIZE * 2:  # 16位=2字节
                            chunk = audio_window[:SAMPLE_WINDOW_SIZE * 2]
                            audio_window = audio_window[SAMPLE_WINDOW_SIZE * 2:]

                            energy = self.calculate_energy(chunk)
                            print("len(chunk):",len(chunk),  "energy:", energy)
                            if energy < SILENCE_THRESHOLD:
                                silent_samples += 1
                                if silent_samples >= silence_threshold_windows:
                                    print("检测到静音，结束录音")
                                    self.record_finish = True
                                    break
                            else:
                                silent_samples = 0

                    except Exception as e:
                        print("发送失败:", e)
                        break

                gc.collect()

                # 超时或静音检测停止
                if time.ticks_ms() - start_time > RECORD_SECONDS * 1100 or self.should_stop or self.record_finish:
                    break

            # 发送结束标记
            if not self.should_stop:
                s.send(struct.pack('<I', 0))
                print(f"录音完成，共发送 {recorded_bytes} 字节")

                # 接收和播放响应
                self.process_server_response(s)
            else:
                # 如果是静音中断，也发送结束标记
                s.send(struct.pack('<I', 0))
                print(f"静音中断，共发送 {recorded_bytes} 字节")

        except Exception as e:
            print("record_and_send 异常:", str(e))
            sys.print_exception(e)
        finally:
            self.is_recording = False
            if 's' in locals():
                s.close()
            gc.collect()
            self.should_stop = False

    def apply_volume_reduction(self, audio_data, factor):
        """降低音频音量以减少对麦克风的干扰"""
        if len(audio_data) % 2 != 0:
            return  # 确保是16位音频数据
            
        # 将字节数据转换为16位整数数组
        samples = array.array('h')
        for i in range(0, len(audio_data), 2):
            sample = int.from_bytes(audio_data[i:i+2], 'little', signed=True)
            # 应用音量降低
            reduced_sample = int(sample * factor)
            # 防止溢出
            if reduced_sample > 32767:
                reduced_sample = 32767
            elif reduced_sample < -32768:
                reduced_sample = -32768
            samples.append(reduced_sample)
        
        # 将处理后的数据写回原数组
        for i, sample in enumerate(samples):
            audio_data[i*2:i*2+2] = sample.to_bytes(2, 'little', signed=True)

    def playback_thread_func(self, socket_obj):
        """播放线程函数 - 独立处理音频播放"""
        print("🎵 播放线程启动")
        
        with self.playback_thread_lock:
            self.playback_thread_active = True
            self.stop_playback_thread = False
            self.is_playing_response = True
        
        end_marker = b"END_OF_STREAM\n"
        marker_len = len(end_marker)
        buffer = bytearray()
        found_marker = False
        data_count = 0
        # 降低播放音量以减少对麦克风的干扰
        volume_reduction_factor = 0.6  # 降低到60%音量
        
        try:
            while not self.stop_playback_thread:
                data = socket_obj.recv(1024)
                if data:
                    data_count += 1
                    print(f"📡 播放线程接收数据包 #{data_count}, 大小: {len(data)} 字节")
                if not data:
                    print("📡 播放线程：服务器连接结束")
                    break

                buffer.extend(data)

                # 检查结束标记
                if not found_marker and len(buffer) >= marker_len:
                    if buffer[-marker_len:] == end_marker:
                        found_marker = True
                        print("🎵 播放线程：检测到音频结束标记")
                        if len(buffer) > marker_len:
                            audio_data = buffer[:-marker_len]
                            print(f"🔊 播放线程：播放最后音频块: {len(audio_data)} 字节")
                            if not self.stop_playback_thread:
                                self.audio_out.write(audio_data)
                                audio_buffer = bytearray(audio_data)
                                self.apply_volume_reduction(audio_buffer, volume_reduction_factor)
                                self.audio_out.write(audio_buffer)
                        break
                    elif len(buffer) > 512:
                        play_len = len(buffer) - marker_len
                        if play_len > 0 and not self.stop_playback_thread:
                            print(f"🔊 播放线程：播放音频块: {play_len} 字节")
                            #self.audio_out.write(buffer[:play_len])
                            # 应用音量降低
                            audio_buffer = bytearray(buffer[:play_len])
                            self.apply_volume_reduction(audio_buffer, volume_reduction_factor)
                            self.audio_out.write(audio_buffer)
                        buffer = buffer[play_len:]

                if found_marker and len(buffer) > 0 and not self.stop_playback_thread:
                    print(f"🔊 播放线程：播放剩余音频: {len(buffer)} 字节")
                    # self.audio_out.write(buffer)
                    audio_buffer = bytearray(buffer)
                    self.apply_volume_reduction(audio_buffer, volume_reduction_factor)
                    self.audio_out.write(audio_buffer)
                    buffer = bytearray()

        except Exception as e:
            print(f"❌ 播放线程异常: {e}")
        finally:
            with self.playback_thread_lock:
                self.playback_thread_active = False
                self.is_playing_response = False
                
            if self.stop_playback_thread:
                print("🛑 播放线程被停止")
                print("🤖 小乐：您好，请继续说话...")
            else:
                print("✅ 播放线程正常结束")
            
            # 关闭socket连接
            try:
                socket_obj.close()
            except:
                pass
            
            gc.collect()
            print("🎵 播放线程结束")

    def stop_playback(self):
        """停止播放线程"""
        print("🛑 请求停止播放线程...")
        with self.playback_thread_lock:
            if self.playback_thread_active:
                self.stop_playback_thread = True
                print("✅ 播放停止信号已发送")
                return True
            else:
                print("ℹ️ 播放线程未运行")
                return False

    def wait_for_playback_completion(self, timeout=30):
        """等待播放线程完成（带超时）"""
        start_time = time.time()
        while self.playback_thread_active and (time.time() - start_time) < timeout:
            time.sleep(0.1)
        
        if self.playback_thread_active:
            print(f"⚠️ 播放线程等待超时({timeout}秒)，强制停止")
            self.stop_playback()
            return False
        return True

    def check_wakeup_interrupt(self):
        """检查是否有唤醒词打断 - 实际可行的方案"""
        # 限制检测频率，避免过度消耗资源
        current_time = time.time()
        if current_time - self.last_interrupt_check < self.interrupt_check_interval:
            return False
        self.last_interrupt_check = current_time
        
        print(f"🔍 检查打断信号... (播放时长: {current_time - getattr(self, 'playback_start_time', current_time):.1f}秒)")
        
        # 简单的超时机制：播放超过10秒自动停止检测
        if hasattr(self, 'playback_start_time'):
            if current_time - self.playback_start_time > 10:
                print("⏰ 播放超时，停止打断检测")
                return False
        
        # 方案1：检查GPIO按键中断（推荐方案）
        try:
            # 使用GPIO0作为打断按键（ESP32-S3的BOOT按键）
            button_pin = Pin(0, Pin.IN, Pin.PULL_UP)
            if not button_pin.value():  # 按键按下（低电平）
                print("🛑 检测到BOOT按键中断")
                time.sleep(0.1)  # 简单防抖
                return True
        except Exception as e:
            print(f"⚠️ GPIO检测异常: {e}")
        
        # 方案2：检查文件标志位（用于远程控制）
        try:
            import os
            if 'interrupt.flag' in os.listdir('.'):
                print("🛑 检测到中断标志文件")
                os.remove('interrupt.flag')  # 删除标志文件
                return True
        except:
            pass
        
        # 方案3：临时测试 - 在播放3秒后自动触发（仅用于验证逻辑）
        if hasattr(self, 'playback_start_time'):
            if current_time - self.playback_start_time > 3.0 and current_time - self.playback_start_time < 3.2:
                print("🧪 自动测试打断触发（3秒后）")
                return True
        
        return False

    def process_server_response(self, s):
        """处理服务器响应 - 启动播放线程"""
        print("🎧 启动播放线程处理服务器响应...")
        
        # 启动播放线程
        try:
            _thread.start_new_thread(self.playback_thread_func, (s,))
            print("✅ 播放线程已启动")
        except Exception as e:
            print(f"❌ 启动播放线程失败: {e}")
            # 如果线程启动失败，关闭socket
            try:
                s.close()
            except:
                pass

    def recordToAI(self):
        print("start recordToAI")
        # 配置参数
        LOOPBACK_TIME = 10  # 回环10秒
        VOLUME_GAIN = 2.0  # 音量增益

        if not self.is_init_record_mic:
            self.initRecordMic()

        self.record_and_send(self.mic, self.audio_out)

        # 检查是否被打断
        if self.wakeup_interrupted:
            print("🔄 检测到打断，准备重新录音")
            # 重置打断标志但不清理资源，直接准备下一轮录音
            self.wakeup_interrupted = False
            # 重新开始录音流程
            print("🎤 重新开始录音...")
            self.record_and_send(self.mic, self.audio_out)
            
            # 如果再次被打断，则进行资源清理
            if self.wakeup_interrupted:
                print("🔄 再次被打断，进行资源清理")
                self.wakeup_interrupted = False
                self.deinit_record_mic()
                return
        
        # 正常流程：回环结束，释放mic资源
        self.deinit_record_mic()
        print("end recordToAI")



    def recordToAIDemo(self):
        print("start recordToAI")
        # 配置参数
        LOOPBACK_TIME = 10  # 回环10秒
        VOLUME_GAIN = 2.0  # 音量增益

        if not self.is_init_record_mic:
            self.initRecordMic()

        # 运行实时回环
        self.run_loopback(LOOPBACK_TIME, VOLUME_GAIN)

        # 回环结束， 释放mic 资源
        self.deinit_record_mic()
        print("end recordToAI")

    def apply_gain(self, audio_array):
        """应用音量增益"""
        for i in range(len(audio_array)):
            sample = int(audio_array[i] * self.volume_gain)
            # 防止削波
            if sample > self.clip_threshold:
                sample = self.clip_threshold
            elif sample < -self.clip_threshold:
                sample = -self.clip_threshold
            audio_array[i] = sample

    def calculate_rms(self, audio_array):
        """计算RMS音量"""
        if len(audio_array) == 0:
            return 0
        sum_squares = sum(sample * sample for sample in audio_array)
        rms = (sum_squares / len(audio_array)) ** 0.5
        return rms

    def run_loopback(self, duration, volume_gain=1.0):
        """运行实时回环"""
        if not self.is_init_record_mic:
            raise RuntimeError("I2S未初始化")

        self.volume_gain = volume_gain
        print(f"开始实时回环 {duration} 秒 (音量增益: {volume_gain})...")
        print("请对着麦克风说话，应该能听到实时回放...")

        start_time = time.time()
        total_bytes_processed = 0
        audio_detected_count = 0
        error_count = 0

        while time.time() - start_time < duration:
            try:
                # 从麦克风读取音频
                bytes_read = self.mic.readinto(self.buffer)

                if bytes_read > 0:
                    # 转换为数组进行处理
                    audio_array = array.array('h', self.buffer[:bytes_read])

                    # 计算音量
                    rms = self.calculate_rms(audio_array)

                    # 应用音量增益
                    if volume_gain != 1.0:
                        self.apply_gain(audio_array)

                    # 播放到扬声器
                    try:
                        processed_data = audio_array.tobytes()
                    except AttributeError:
                        processed_data = bytes(audio_array)

                    bytes_written = self.audio_out.write(processed_data)

                    total_bytes_processed += bytes_read

                    # 检测声音并显示
                    if rms > 100:  # 音量阈值
                        audio_detected_count += 1
                        if audio_detected_count % 20 == 0:  # 每20次显示一次
                            print(f"🎤 检测到声音 - RMS: {rms:.0f}, 处理: {bytes_read} 字节")

                    # 定期显示进度
                    elapsed = time.time() - start_time
                    if int(elapsed) % 2 == 0 and int(elapsed) != int(elapsed - 0.1):
                        progress = elapsed / duration * 100
                        print(f"回环进度: {progress:.1f}% ({elapsed:.1f}/{duration}秒)")

                # 强制垃圾回收
                gc.collect()

            except Exception as e:
                error_count += 1
                print(f"回环处理时出错: {e}")
                if error_count > 5:  # 如果错误太多就退出
                    print("错误次数过多，停止回环")
                    break
                time.sleep(0.01)  # 短暂等待

        loopback_time = time.time() - start_time
        print(f"✅ 实时回环完成!")
        print(f"  运行时间: {loopback_time:.2f}秒")
        print(f"  处理字节: {total_bytes_processed}")
        print(f"  声音检测次数: {audio_detected_count}")
        print(f"  错误次数: {error_count}")

    def deinit_record_mic(self):
        print("micropython deint start")
        """清理资源"""
        self.is_init_record_mic = False

        try:
            if self.mic:
                self.mic.deinit()
                self.mic = None
        except Exception as e:
            print(f"清理麦克风时出错: {e}")

        # 等待一下确保资源释放
        time.sleep(0.2)
        print("✅ 录音 mic清理完成")

    def connect_wifi(self, ssid, password, timeout=10):
        wlan = network.WLAN(network.STA_IF)
        wlan.active(True)
        if wlan.isconnected():
            wlan.disconnect()
            print("已断开当前 WiFi 连接")
        wlan.connect(ssid, password)
        print(f"正在连接到 WiFi: {ssid}")
        start_time = time.time()
        while not wlan.isconnected() and (time.time() - start_time) < timeout:
            time.sleep(1)
        if wlan.isconnected():
            print('network config:', wlan.ifconfig())
            return True
        else:
            print("连接 WiFi 失败")
            return False

    def connectWifi(self):
        print("connectWifi")
        # 尝试连接WiFi
        wlan = network.WLAN(network.STA_IF)
        wlan.active(True)

        # 循环尝试连接WiFi，直到成功为止
        while not wlan.isconnected():
            try:
                success = self.connect_wifi(WIFI_SSID, WIFI_PASSWORD)
                if success:
                    break  # 连接成功，退出循环
                else:
                    print("Failed to connect to WiFi")
            except Exception as e:
                print("connect_wifi except:", str(e))
                sys.print_exception(e)

            # 连接失败，等待2秒后重试
            print("Retrying in 5 seconds...")
            time.sleep(5)        
    
    def run(self):
        self.connectWifi()

        print("\n🚀 MicroPython 语音唤醒+命令词识别 (参照project-i2s-wakup-new)")
        print("=" * 50)
        print("🔄 初始化espsr模块...")

        try:

            init_result = espsr.init()
            if init_result:
                print("✅ ESP-SR 初始化成功!")
            else:
                print("❌ ESP-SR 初始化失败!")
                return
            self.is_wakeup_mic = True
        except Exception as e:
            print(f"❌ 初始化异常: {e}")
            return

        # 显示支持的命令词
        try:
            commands = espsr.get_commands()
            print(f"\n📝 支持的命令词 (共{len(commands)}个):")
            for cid, cmd in commands.items():
                print(f"  {cid:2}: {cmd}")
        except Exception as e:
            print(f"⚠️ 获取命令词失败: {e}")

        print("\n🎯 唤醒词: '嗨小乐' (hai xiao le) - 使用命令词实现")
        print("🎤 直接说'嗨小乐'唤醒，或说其他命令词...")
        print("⏱️  连续监听模式，无需等待唤醒")
        print("🔄 按Ctrl+C退出程序")
        print("💡 播放回复时支持多种打断方式:")
        print("   - 按住BOOT按键(GPIO0)打断")
        print("   - 创建interrupt.flag文件打断")
        print("   - 自动测试：播放3秒后自动打断")
        print("-" * 50)

        wakeup_count = 0
        command_count = 0

        try:
            while True:

                if not self.is_wakeup_mic:
                    init_result = espsr.init()
                    if init_result:
                        print("✅ ESP-SR 初始化成功!")
                    else:
                        print("❌ ESP-SR 初始化失败!")
                        return

                print(f"\n🔍 开始监听 (1秒)... [唤醒:{wakeup_count} 命令:{command_count}]")

                try:
                    result = espsr.listen(40)
                    gc.collect()
                    if result == "wakeup":
                        wakeup_count += 1
                        print(f"🎉 检测到唤醒词'嗨小乐'! (第{wakeup_count}次)")
                        print("   🤖 小乐：您好，有什么可以帮您的吗?")
                        self.stop_playback()
                        self.playWozai()

                        # 清理资源，打开录音 i2s
                        espsr.cleanup()
                        gc.collect()
                        self.is_wakeup_mic = False

                        # 开始调用录音+识别
                        self.recordToAI()

                        # 检查是否被打断，如果被打断则立即重新开始监听
                        if self.wakeup_interrupted:
                            print("🔄 检测到播放被打断，立即重新开始唤醒监听...")
                            self.wakeup_interrupted = False
                            # 重新初始化唤醒监听
                            try:
                                init_result = espsr.init()
                                if init_result:
                                    self.is_wakeup_mic = True
                                    print("✅ 重新初始化ESP-SR成功，继续监听...")
                                    continue
                                else:
                                    print("❌ 重新初始化ESP-SR失败!")
                            except Exception as e:
                                print(f"❌ 重新初始化异常: {e}")

                    elif result == "timeout":
                        print("⏰ 监听超时，继续等待...")

                    elif result == "not_initialized":
                        print("❌ ESP-SR未初始化!")
                        break

                    elif isinstance(result, dict) and "id" in result:
                        command_id = result["id"]
                        command_text = result.get("command", "未知")

                        if command_id == 0:  # hai xiao le (唤醒词)
                            wakeup_count += 1
                            print(f"🎉 检测到唤醒词'嗨小乐'! (第{wakeup_count}次)")
                            print("   🤖 小乐：您好，有什么可以帮您的吗?")
                            self.stop_playback()
                            self.playWozai()

                        else:
                            command_count += 1
                            print(f"🎵 检测到命令词! (第{command_count}次)")
                            print(f"   ID: {command_id}")
                            print(f"   命令: {command_text}")
                            self.stop_playback()
                            self.playWozai()
                            print(f"   ⚙️  执行命令ID: {command_id}")

                        # 清理资源，打开录音 i2s
                        espsr.cleanup()
                        gc.collect()
                        self.is_wakeup_mic = False

                        # 开始调用录音+识别
                        self.recordToAI()

                        # 检查是否被打断，如果被打断则立即重新开始监听
                        if self.wakeup_interrupted:
                            print("🔄 检测到播放被打断，立即重新开始唤醒监听...")
                            self.wakeup_interrupted = False
                            # 重新初始化唤醒监听
                            try:
                                init_result = espsr.init()
                                if init_result:
                                    self.is_wakeup_mic = True
                                    print("✅ 重新初始化ESP-SR成功，继续监听...")
                                    continue
                                else:
                                    print("❌ 重新初始化ESP-SR失败!")
                            except Exception as e:
                                print(f"❌ 重新初始化异常: {e}")

                    else:
                        print(f"❓ 未知结果: {result}")

                    time.sleep_ms(40)

                except Exception as e:
                    print(f"❌ 监听异常: {e}")
                    time.sleep(1)

        except KeyboardInterrupt:
            print("\n🛑 用户中断，正在清理资源...")
            try:
                espsr.cleanup()
                self.is_wakeup_mic = False

                # 清理资源
                self.audio_out.deinit()
                print("✅ 资源清理完成")
            except Exception as e:
                print(f"⚠️ 清理异常: {e}")

        print("\n👋 程序结束")