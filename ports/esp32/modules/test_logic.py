"""
AEC 打断功能测试脚本

测试流程：
1. 播放网络音频（6秒）
2. 播放期间持续监听唤醒词
3. 检测到唤醒词立即打断
4. 开始录音直到用户停止说话
5. 重复播放音频
6. 循环测试

使用方法：
1. 将本文件上传到设备的 /flash/ 目录
2. 在 REPL 中运行：
   >>> import test_aec_interrupt
   >>> test_aec_interrupt.run_test()
"""

import network
import socket
import time
import _thread
import gc
import espsr
from machine import I2S, Pin
import urequests
import struct
import sys

# 测试音频URL
# TEST_AUDIO_URL = "http://cdn.file.letianpai.com/internal_tmp/temp_1761552110392235000.wav"
TEST_AUDIO_URL = "http://cdn.file.letianpai.com/internal_tmp/aspose_audio_merger_134061122823002959.wav"
WIFI_SSID = "LETIANPAI"
WIFI_PASSWORD = "Renhejia0801"

class AECInterruptTester:
    def __init__(self):
        # 🔥 v2.9: 不再需要Python端的I2S，C端管理播放
        # I2S播放由C端的playback_Task管理，避免冲突
        
        # 状态标志
        self.is_playing = False
        self.wakeup_interrupted = False
        self.stop_playback = False
        self.playback_active = False
        
        # 线程锁
        self.lock = _thread.allocate_lock()
        
        # 初始化 ESP-SR
        print("🔧 初始化 ESP-SR...")
        result = espsr.init()
        if result:
            print("✅ ESP-SR 初始化成功")
        else:
            print("❌ ESP-SR 初始化失败")
            raise RuntimeError("ESP-SR init failed")
        # 设置 AEC 参数（关键步骤！）
        # 参数说明：
        # delay_ms: 参考信号延迟（毫秒），典型值 20-40ms
        # gain_shift: 参考信号增益（0=不增益，1=×2，2=×4），典型值 0-2
        # energy_ratio: 能量阈值比例（参考能量/麦克风能量），典型值 4-8
        espsr.set_aec_params(20, 1, 6)
        print("AEC 参数已设置")

    
    def parse_url(self, url):
        """解析 URL"""
        # 移除 http:// 或 https://
        if url.startswith('https://'):
            url = url[8:]
            use_ssl = True
        elif url.startswith('http://'):
            url = url[7:]
            use_ssl = False
        else:
            use_ssl = False
        
        # 分离主机和路径
        parts = url.split('/', 1)
        host = parts[0]
        path = '/' + parts[1] if len(parts) > 1 else '/'
        
        return host, path, use_ssl
    
    def stream_audio_from_url(self, url):
        """流式下载音频（返回 socket）"""
        print(f"📥 流式下载音频: {url}")
        
        try:
            # 解析 URL
            host, path, use_ssl = self.parse_url(url)
            print(f"🔗 连接: {host}{path}")
            
            # 创建 socket
            addr = socket.getaddrinfo(host, 443 if use_ssl else 80)[0][-1]
            s = socket.socket()
            s.connect(addr)
            
            if use_ssl:
                import ssl
                s = ssl.wrap_socket(s, server_hostname=host)
            
            # 发送 HTTP GET 请求
            request = f"GET {path} HTTP/1.1\r\n"
            request += f"Host: {host}\r\n"
            request += "Connection: close\r\n"
            request += "\r\n"
            s.send(request.encode())
            
            # 读取 HTTP 响应头
            print("📡 读取 HTTP 响应头...")
            header = b""
            while b"\r\n\r\n" not in header:
                chunk = s.recv(1)
                if not chunk:
                    raise Exception("连接关闭")
                header += chunk
            
            # 解析响应头
            header_str = header.decode('utf-8', 'ignore')
            lines = header_str.split('\r\n')
            status_line = lines[0]
            
            if '200' not in status_line:
                print(f"❌ HTTP 错误: {status_line}")
                s.close()
                return None
            
            # 提取 Content-Length
            content_length = 0
            for line in lines:
                if line.lower().startswith('content-length:'):
                    content_length = int(line.split(':')[1].strip())
                    break
            
            print(f"✅ 连接成功，文件大小: {content_length} 字节")
            
            # 跳过 WAV 头（44 字节）
            wav_header = s.recv(44)
            if wav_header[:4] != b'RIFF':
                print("⚠️ 不是有效的 WAV 文件")
            else:
                print("✅ 已跳过 WAV 头")
            
            return s, content_length - 44
            
        except Exception as e:
            print(f"❌ 流式下载异常: {e}")
            import sys
            sys.print_exception(e)
            return None, 0
    
    def playback_stream_func(self, audio_socket, total_size):
        """🔥 v2.9: 使用C端播放线程 - Python只负责下载和传输"""
        print("\n" + "="*60)
        print("🎵 播放线程启动（v2.9 C端播放）")
        print("="*60)
        
        with self.lock:
            self.playback_active = True
            self.stop_playback = False
            self.wakeup_interrupted = False
        
        chunk_size = 4096
        data_count = 0
        interrupt_check_interval = 1  # 每个块都检测（低延迟唤醒/命令）
        received_bytes = 0
        
        try:
            # 重新启用录音模式（清空缓冲区）
            print("🔄 重新启用录音模式（清空缓冲区）...")
            espsr.stop_recording()
            time.sleep(0.05)
            espsr.start_recording()
            print("✅ 录音模式已重新启用")
            
            # 🔥 启动C端播放线程
            print("🚀 启动C端播放线程...")
            if not espsr.start_playback():
                print("❌ 启动C端播放线程失败")
                return
            print("✅ C端播放线程已启动")
            
            # 🔥 预热缓冲区：启动后立即快速喂入 >= 16KB，避免开头饿死/跳跃
            prefill_target = 32 * 1024
            prefilled = 0
            tail_buf = b""  # 累积不足960字节的尾巴
            while prefilled < prefill_target and received_bytes < total_size and not self.stop_playback:
                to_read = min(4096, total_size - received_bytes)
                audio_chunk = audio_socket.recv(to_read)
                if not audio_chunk:
                    break
                received_bytes += len(audio_chunk)
                data_count += 1
                # 分片为960字节的小块喂入
                buf = tail_buf + audio_chunk
                pos = 0
                FEED_UNIT = 960
                while pos + FEED_UNIT <= len(buf):
                    mini = buf[pos:pos+FEED_UNIT]
                    # 阻塞式重试直至该 mini 全部写入，避免部分写入导致丢块
                    sent = 0
                    retry = 0
                    while sent < len(mini):
                        try:
                            written = espsr.feed_playback(mini[sent:])
                        except Exception:
                            written = 0
                        if written > 0:
                            sent += written
                            prefilled += written
                            retry = 0
                        else:
                            time.sleep_ms(2)
                            retry += 1
                            if retry > 200:  # 最多约400ms等待，防止死等
                                break
                    # 仅当完全写入才前进指针；否则不前进，保留在 tail_buf 重试
                    if sent == len(mini):
                        pos += FEED_UNIT
                    else:
                        break
                tail_buf = buf[pos:]
                if data_count % 20 == 1:
                    progress = received_bytes / total_size * 100
                    print(f"📡 预热进度: {progress:.1f}% ({received_bytes}/{total_size}), 预热={prefilled}B")
            
            # 从网络下载音频并喂给C端（持续按960字节均匀喂入）
            vad_true_streak = 0  # 播放期VAD去抖：连续命中才触发
            while received_bytes < total_size and not self.stop_playback:
                # 检测打断（仅唤醒/命令；播放期不使用VAD打断）
                if data_count % interrupt_check_interval == 0:
                    try:
                        result = espsr.listen(1)
                        if result == "wakeup":
                            print("\n" + "🛑"*30)
                            print("🛑 检测到唤醒词打断！")
                            print("🛑"*30 + "\n")
                            self.wakeup_interrupted = True
                            self.stop_playback = True
                            break
                        elif isinstance(result, dict) and "id" in result:
                            print(f"\n🛑 检测到命令词打断: {result}")
                            self.wakeup_interrupted = True
                            self.stop_playback = True
                            break
                        # 播放期间恢复基于VAD的打断，但加严条件（Python端去抖）
                        is_speaking = espsr.check_vad()
                        if is_speaking:
                            vad_true_streak += 1
                        else:
                            vad_true_streak = 0
                        # 仅当连续3次命中（≈3*循环间隔）才判定为真实说话打断
                        if vad_true_streak >= 2:
                            print("\n" + "🗣️"*30)
                            print("🗣️ 检测到语音活动打断！（VAD，去抖）")
                            print("🗣️"*30 + "\n")
                            self.wakeup_interrupted = True
                            self.stop_playback = True
                            break
                    except:
                        pass
                
                # 从网络读取音频块
                try:
                    to_read = min(chunk_size, total_size - received_bytes)
                    audio_chunk = audio_socket.recv(to_read)
                    
                    if not audio_chunk:
                        print("📡 数据接收完成")
                        break
                    
                    received_bytes += len(audio_chunk)
                    data_count += 1
                    
                    if data_count % 20 == 1:
                        progress = received_bytes / total_size * 100
                        print(f"📡 下载进度: {progress:.1f}% ({received_bytes}/{total_size})")
                    
                    # 🔥 关键：直接喂给C端播放缓冲区
                    # 按960字节单位喂入，避免突发造成满/饿
                    buf = tail_buf + audio_chunk
                    pos = 0
                    FEED_UNIT = 960
                    total_fed = 0
                    while pos + FEED_UNIT <= len(buf):
                        mini = buf[pos:pos+FEED_UNIT]
                        # 阻塞式重试直至该 mini 全部写入
                        sent = 0
                        retry = 0
                        while sent < len(mini):
                            try:
                                written = espsr.feed_playback(mini[sent:])
                            except Exception:
                                written = 0
                            if written > 0:
                                sent += written
                                total_fed += written
                                retry = 0
                            else:
                                time.sleep_ms(2)
                                retry += 1
                                if retry > 200:
                                    break
                        if sent == len(mini):
                            pos += FEED_UNIT
                        else:
                            break
                    tail_buf = buf[pos:]
                    if total_fed == 0:
                        print("⚠️ 缓冲区拥塞，未能写入，稍后重试")
                    
                except Exception as e:
                    print(f"❌ 接收/传输异常: {e}")
                    import sys
                    sys.print_exception(e)
                    break
                
        except Exception as e:
            print(f"❌ 播放线程异常: {e}")
            import sys
            sys.print_exception(e)
        finally:
            # 🔥 停止C端播放线程
            try:
                print("🛑 停止C端播放线程...")
                espsr.stop_playback()
                print("✅ C端播放线程已停止")
            except Exception as e:
                print(f"❌ 停止播放线程异常: {e}")
            
            # 关闭 socket
            try:
                audio_socket.close()
                print("🔌 网络连接已关闭")
            except:
                pass
            
            with self.lock:
                self.playback_active = False
            
            if self.stop_playback:
                if self.wakeup_interrupted:
                    print("🤖 检测到打断，录音模式保持开启")
                else:
                    print("🛑 播放被手动停止")
            else:
                print("✅ 播放正常结束")
                # 播放正常结束，停止录音模式
                espsr.stop_recording()
            
            gc.collect()
            print("🎵 播放线程结束\n")
    
    def record_until_silence_vad(self):
        """使用 VAD 录音直到检测到静音"""
        print("\n" + "="*60)
        print("🎤 开始录音（VAD 静音检测）")
        print("="*60)
        
        # 启用录音模式
        if not espsr.start_recording():
            print("❌ 启动录音失败")
            return
        
        MIN_SILENCE_DURATION = 1.5   # 最少静音时长（秒）
        MAX_RECORD_TIME = 10         # 最大录音时长（秒）
        VAD_CHECK_INTERVAL = 50      # VAD 检测间隔（ms）
        
        buffer = bytearray(1024)
        start_time = time.time()
        total_bytes = 0
        silence_start_time = None
        has_spoken = False  # 是否检测到过说话
        
        print(f"🎙️ 录音参数:")
        print(f"  - VAD 检测间隔: {VAD_CHECK_INTERVAL}ms")
        print(f"  - 静音时长: {MIN_SILENCE_DURATION}s")
        print(f"  - 最大时长: {MAX_RECORD_TIME}s")
        
        last_status_time = time.time()
        
        while True:
            # 检查超时
            if time.time() - start_time > MAX_RECORD_TIME:
                print(f"⏰ 录音超时 ({MAX_RECORD_TIME}s)")
                break
            
            # 读取音频数据（保持录音缓冲区不满）
            bytes_read = espsr.read_audio(buffer)
            if bytes_read > 0:
                total_bytes += bytes_read
            
            # 🔥 使用 VAD 检测语音活动
            is_speaking = espsr.check_vad()
            
            if is_speaking:
                # 检测到语音
                has_spoken = True
                silence_start_time = None  # 重置静音计时
                # 打印状态（避免刷屏）
                if time.time() - last_status_time >= 0.5:
                    print(f"🎤 录音中... VAD: SPEECH")
                    last_status_time = time.time()
            else:
                # 检测到静音
                if has_spoken:  # 只有在说过话之后才开始计时静音
                    if silence_start_time is None:
                        silence_start_time = time.time()
                        print(f"🔇 检测到静音，开始计时...")
                    else:
                        silence_duration = time.time() - silence_start_time
                        if silence_duration >= MIN_SILENCE_DURATION:
                            elapsed = time.time() - start_time
                            print(f"✅ 静音持续 {silence_duration:.1f}s，结束录音")
                            print(f"📊 录音统计:")
                            print(f"  - 时长: {elapsed:.2f}s")
                            print(f"  - 数据: {total_bytes} 字节")
                            espsr.stop_recording()
                            return
                else:
                    # 还没说话，等待用户开始说话
                    if time.time() - last_status_time >= 1.0:
                        print(f"⏰ 等待用户说话...")
                        last_status_time = time.time()
            
            # VAD 检测间隔
            time.sleep_ms(VAD_CHECK_INTERVAL)
            gc.collect()
        
        # 超时结束
        espsr.stop_recording()
        print(f"📊 录音结束，共录制 {total_bytes} 字节")
    
    def run_test_loop(self, audio_url, max_loops=10):
        """运行测试循环（流式播放）"""
        print("\n" + "🚀"*30)
        print("🚀 开始 AEC 打断功能测试（流式播放）")
        print("🚀"*30)
        print(f"\n测试参数:")
        print(f"  - 音频 URL: {audio_url}")
        print(f"  - 最大循环: {max_loops} 次")
        print(f"  - 打断检测: 每个音频块")
        print(f"  - 播放模式: 流式下载播放")
        print(f"\n测试说明:")
        print(f"  1. 播放音频时说 '嗨小乐' 可以打断")
        print(f"  2. 打断后会开始录音")
        print(f"  3. 说完话停止 1.5 秒会自动结束录音")
        print(f"  4. 然后重新播放音频")
        print(f"  5. 按 Ctrl+C 可以停止测试")
        print()
        
        loop_count = 0
        
        try:
            while loop_count < max_loops:
                loop_count += 1
                print("\n" + "🔄"*30)
                print(f"🔄 第 {loop_count}/{max_loops} 轮测试")
                print("🔄"*30 + "\n")
                
                # 1. 建立流式连接
                result = self.stream_audio_from_url(audio_url)
                if result is None or result[0] is None:
                    print("❌ 无法建立流式连接，跳过本轮")
                    time.sleep(2)
                    continue
                
                audio_socket, total_size = result
                
                # 2. 启动播放线程
                _thread.start_new_thread(self.playback_stream_func, (audio_socket, total_size))
                
                # 3. 等待播放开始
                time.sleep(0.2)
                
                # 4. 等待播放完成或被打断
                while self.playback_active:
                    time.sleep(0.1)
                
                # 5. 检查是否被打断
                if self.wakeup_interrupted:
                    print("\n✅ 检测到打断，开始录音...")
                    
                    # 6. 🔥 使用 VAD 录音直到静音
                    self.record_until_silence_vad()
                    
                    # 7. 重置打断标志
                    self.wakeup_interrupted = False
                    
                    print("\n🔄 准备下一轮播放...")
                    time.sleep(1)
                else:
                    print("\n✅ 播放完成，未检测到打断")
                    print("💤 等待 2 秒后重新播放...")
                    time.sleep(2)
                
                gc.collect()
        
        except KeyboardInterrupt:
            print("\n\n⚠️ 用户中断测试")
        finally:
            print("\n" + "🏁"*30)
            print("🏁 测试结束")
            print("🏁"*30)
            
            # 清理资源
            try:
                espsr.stop_recording()
            except:
                pass
    
    def cleanup(self):
        """清理资源"""
        print("🧹 清理资源...")
        try:
            self.audio_out.deinit()
            espsr.cleanup()
        except:
            pass
        print("✅ 清理完成")


class SensorSystem:
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
        """运行测试（流式播放）"""
        print("\n" + "="*60)
        print("AEC 打断功能测试脚本（流式播放）")
        print("="*60 + "\n")
        self.connectWifi()
        
        # 检查 WiFi 连接
        wlan = network.WLAN(network.STA_IF)
        if not wlan.isconnected():
            print("❌ WiFi 未连接，请先连接 WiFi")
            print("提示：可以运行以下命令连接 WiFi:")
            print("  >>> import network")
            print("  >>> wlan = network.WLAN(network.STA_IF)")
            print("  >>> wlan.active(True)")
            print("  >>> wlan.connect('SSID', 'PASSWORD')")
            return
        
        print(f"✅ WiFi 已连接: {wlan.ifconfig()[0]}")
        
        # 创建测试器
        tester = AECInterruptTester()
        
        try:
            # 运行测试循环（流式播放，不需要预下载）
            tester.run_test_loop(TEST_AUDIO_URL, max_loops=10)
            
        finally:
            # 清理资源
            tester.cleanup()


# def quick_test():
#     """快速测试 - 只测试一次"""
#     print("🚀 快速测试模式（只测试一轮）\n")
    
#     wlan = network.WLAN(network.STA_IF)
#     if not wlan.isconnected():
#         print("❌ WiFi 未连接")
#         return
    
#     tester = AECInterruptTester()
    
#     try:
#         audio_data = tester.download_audio(TEST_AUDIO_URL)
#         if audio_data:
#             tester.run_test_loop(audio_data, max_loops=1)
#     finally:
#         tester.cleanup()


# if __name__ == "__main__":
#     print("\n使用方法:")
#     print("  >>> import test_aec_interrupt")
#     print("  >>> test_aec_interrupt.run_test()      # 完整测试（10轮）")
#     print("  >>> test_aec_interrupt.quick_test()    # 快速测试（1轮）")
#     print()

