"""
I2S硬件测试程序 - 录音播放循环测试
用于测试INMP441麦克风和MAX98357功放硬件连接
"""
from machine import I2S, Pin
import time
import gc

class I2SHardwareTester:
    def __init__(self):
        """初始化I2S硬件测试器"""
        # 音频参数
        self.SAMPLE_RATE = 16000
        self.BITS = 16
        self.BUFFER_LENGTH = 2048
        self.RECORD_DURATION = 3  # 录音时长(秒)
        
        # I2S句柄
        self.i2s_in = None
        self.i2s_out = None
        
        print("🔧 I2S硬件测试器初始化")
        print("=" * 40)
        
    def init_i2s_devices(self):
        """初始化I2S录音和播放设备"""
        try:
            print("🎙️ 初始化INMP441麦克风...")
            self.i2s_in = I2S(
                0,  # I2S ID
                sck=Pin(5),   # SCK → GPIO 5
                ws=Pin(4),    # WS  → GPIO 4
                sd=Pin(6),    # SD  → GPIO 6
                mode=I2S.RX,  # 接收模式
                bits=self.BITS,
                format=I2S.MONO,
                rate=self.SAMPLE_RATE,
                ibuf=self.BUFFER_LENGTH * 4
            )
            print("✅ INMP441麦克风初始化成功")
            
            print("🔊 初始化MAX98357功放...")
            self.i2s_out = I2S(
                1,  # I2S ID
                sck=Pin(15),  # SCK → GPIO 15
                ws=Pin(16),   # WS  → GPIO 16
                sd=Pin(7),    # SD  → GPIO 7
                mode=I2S.TX,  # 发送模式
                bits=self.BITS,
                format=I2S.MONO,
                rate=self.SAMPLE_RATE,
                ibuf=self.BUFFER_LENGTH * 4
            )
            print("✅ MAX98357功放初始化成功")
            
            return True
            
        except Exception as e:
            print(f"❌ I2S设备初始化失败: {e}")
            return False
    
    def record_audio(self):
        """录音函数"""
        print(f"\n🎤 开始录音 ({self.RECORD_DURATION}秒)...")
        print("📢 请对着麦克风说话...")
        
        audio_data = bytearray()
        samples_to_record = self.SAMPLE_RATE * self.RECORD_DURATION * (self.BITS // 8)
        
        start_time = time.ticks_ms()
        
        while len(audio_data) < samples_to_record:
            try:
                audio_buffer = bytearray(self.BUFFER_LENGTH)
                num_bytes_read = self.i2s_in.readinto(audio_buffer)
                
                if num_bytes_read > 0:
                    audio_data.extend(audio_buffer[:num_bytes_read])
                    
                    # 显示进度
                    progress = int(len(audio_data) / samples_to_record * 100)
                    if progress % 25 == 0:
                        print(f"📊 录音进度: {progress}%")
                        
            except Exception as e:
                print(f"❌ 录音错误: {e}")
                break
        
        end_time = time.ticks_ms()
        actual_duration = (end_time - start_time) / 1000
        
        print(f"✅ 录音完成! 时长: {actual_duration:.1f}秒, 数据: {len(audio_data)}字节")
        
        # 检查音频质量
        audio_quality = self.check_audio_quality(audio_data)
        
        return audio_data, audio_quality
    
    def check_audio_quality(self, audio_data):
        """检查音频质量"""
        if len(audio_data) < 4:
            return {"energy": 0, "quality": "无数据"}
        
        # 计算音频能量
        total_energy = 0
        sample_count = len(audio_data) // 2
        
        for i in range(0, len(audio_data) - 1, 2):
            # 转换为有符号16位整数
            sample = int.from_bytes(audio_data[i:i+2], 'little', True)
            total_energy += abs(sample)
        
        avg_energy = total_energy / sample_count if sample_count > 0 else 0
        
        # 判断音频质量
        if avg_energy > 500:
            quality = "优秀"
        elif avg_energy > 200:
            quality = "良好"
        elif avg_energy > 50:
            quality = "一般"
        else:
            quality = "较弱"
        
        print(f"🎵 音频能量: {avg_energy:.1f} (质量: {quality})")
        
        return {"energy": avg_energy, "quality": quality}
    
    def play_audio(self, audio_data):
        """播放音频"""
        if not audio_data:
            print("❌ 无音频数据可播放")
            return False
        
        print(f"\n🔊 开始播放录制的音频...")
        print("📢 您应该能听到刚才录制的声音...")
        
        try:
            bytes_written = 0
            total_bytes = len(audio_data)
            
            start_time = time.ticks_ms()
            
            while bytes_written < total_bytes:
                chunk_size = min(self.BUFFER_LENGTH, total_bytes - bytes_written)
                chunk = audio_data[bytes_written:bytes_written + chunk_size]
                
                bytes_sent = self.i2s_out.write(chunk)
                bytes_written += bytes_sent
                
                # 显示播放进度
                progress = int(bytes_written / total_bytes * 100)
                if progress % 25 == 0:
                    print(f"📊 播放进度: {progress}%")
            
            end_time = time.ticks_ms()
            playback_duration = (end_time - start_time) / 1000
            
            print(f"✅ 播放完成! 时长: {playback_duration:.1f}秒")
            return True
            
        except Exception as e:
            print(f"❌ 播放错误: {e}")
            return False
    
    def run_test_cycle(self):
        """运行一次测试循环"""
        print(f"\n{'='*50}")
        print(f"🔄 开始新的测试循环 - {time.ticks_ms()}ms")
        print(f"{'='*50}")
        
        # 录音
        audio_data, quality = self.record_audio()
        
        if not audio_data:
            print("❌ 录音失败，跳过播放")
            return False
        
        # 短暂延迟
        time.sleep(1)
        
        # 播放
        play_success = self.play_audio(audio_data)
        
        # 清理内存
        del audio_data
        gc.collect()
        
        # 测试结果
        test_result = quality["quality"] != "较弱" and play_success
        
        if test_result:
            print("✅ 本次测试循环成功!")
        else:
            print("⚠️ 本次测试循环存在问题")
        
        return test_result
    
    def run_continuous_test(self, max_cycles=None):
        """运行连续测试"""
        if not self.init_i2s_devices():
            print("❌ I2S设备初始化失败，无法进行测试")
            return
        
        print("\n🚀 开始I2S硬件连续测试")
        print("🔄 按Ctrl+C停止测试")
        print("\n📋 硬件连接检查:")
        print("   INMP441麦克风:")
        print("     SCK → GPIO 5")
        print("     WS  → GPIO 4") 
        print("     SD  → GPIO 6")
        print("     VDD → 3.3V")
        print("     GND → GND")
        print("\n   MAX98357功放:")
        print("     SCK → GPIO 15")
        print("     WS  → GPIO 16")
        print("     SD  → GPIO 7")
        print("     VIN → 5V")
        print("     GND → GND")
        
        cycle_count = 0
        success_count = 0
        
        try:
            while True:
                cycle_count += 1
                
                print(f"\n🔢 第 {cycle_count} 次测试循环")
                
                if self.run_test_cycle():
                    success_count += 1
                
                success_rate = (success_count / cycle_count) * 100
                print(f"📊 成功率: {success_count}/{cycle_count} ({success_rate:.1f}%)")
                
                # 检查是否达到最大循环次数
                if max_cycles and cycle_count >= max_cycles:
                    print(f"\n🎯 已完成 {max_cycles} 次测试循环")
                    break
                
                # 循环间隔
                print("⏳ 等待3秒后开始下次测试...")
                time.sleep(3)
                
        except KeyboardInterrupt:
            print(f"\n🛑 用户中断测试")
        
        finally:
            self.cleanup()
            print(f"\n📊 测试总结:")
            print(f"   总循环次数: {cycle_count}")
            print(f"   成功次数: {success_count}")
            print(f"   成功率: {success_rate:.1f}%")
            
            if success_rate >= 80:
                print("✅ 硬件工作状态良好!")
            elif success_rate >= 50:
                print("⚠️ 硬件工作状态一般，建议检查连接")
            else:
                print("❌ 硬件存在问题，请检查连接和配置")
    
    def cleanup(self):
        """清理资源"""
        try:
            if self.i2s_in:
                self.i2s_in.deinit()
                print("🧹 录音I2S已清理")
        except:
            pass
            
        try:
            if self.i2s_out:
                self.i2s_out.deinit()
                print("🧹 播放I2S已清理")
        except:
            pass

def main():
    """主函数"""
    tester = I2SHardwareTester()
    
    print("🎯 选择测试模式:")
    print("1. 单次测试")
    print("2. 连续测试 (无限循环)")
    print("3. 限定次数测试")
    
    # 默认运行连续测试
    print("\n🚀 运行连续测试模式...")
    tester.run_continuous_test()

if __name__ == "__main__":
    main() 