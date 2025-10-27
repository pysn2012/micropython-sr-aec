"""
简化版I2S录音播放循环测试
快速验证INMP441和MAX98357硬件连接
"""
from machine import I2S, Pin
import time

def run_simple_loopback_test():
    """运行简单的录音播放循环测试"""
    print("🔧 简化版I2S硬件测试")
    print("=" * 30)
    
    # 音频参数
    SAMPLE_RATE = 16000
    BITS = 16
    BUFFER_LENGTH = 1024
    RECORD_DURATION = 2  # 2秒录音
    
    try:
        # 初始化录音I2S (INMP441)
        print("🎙️ 初始化麦克风...")
        i2s_in = I2S(
            0,
            sck=Pin(5), ws=Pin(4), sd=Pin(6),
            mode=I2S.RX, bits=BITS, format=I2S.MONO,
            rate=SAMPLE_RATE, ibuf=BUFFER_LENGTH * 4
        )
        print("✅ 麦克风初始化成功")
        
        # 初始化播放I2S (MAX98357)
        print("🔊 初始化功放...")
        i2s_out = I2S(
            1,
            sck=Pin(15), ws=Pin(16), sd=Pin(7),
            mode=I2S.TX, bits=BITS, format=I2S.MONO,
            rate=SAMPLE_RATE, ibuf=BUFFER_LENGTH * 4
        )
        print("✅ 功放初始化成功")
        
        cycle = 0
        
        while True:
            cycle += 1
            print(f"\n🔄 第 {cycle} 轮测试")
            
            # 录音阶段
            print(f"🎤 录音 {RECORD_DURATION} 秒，请说话...")
            audio_data = bytearray()
            target_bytes = SAMPLE_RATE * RECORD_DURATION * 2  # 16bit = 2bytes
            
            start_time = time.ticks_ms()
            while len(audio_data) < target_bytes:
                buffer = bytearray(BUFFER_LENGTH)
                bytes_read = i2s_in.readinto(buffer)
                if bytes_read > 0:
                    audio_data.extend(buffer[:bytes_read])
            
            record_time = time.ticks_ms() - start_time
            print(f"✅ 录音完成: {len(audio_data)}字节, {record_time}ms")
            
            # 检查音频能量
            energy = 0
            for i in range(0, len(audio_data)-1, 2):
                sample = int.from_bytes(audio_data[i:i+2], 'little', True)
                energy += abs(sample)
            
            avg_energy = energy / (len(audio_data) // 2)
            print(f"🎵 音频能量: {avg_energy:.0f}")
            
            if avg_energy < 50:
                print("⚠️ 音频信号很弱，请检查麦克风连接和音量")
            
            # 播放阶段
            time.sleep(0.5)  # 短暂延迟
            print("🔊 播放录制的音频...")
            
            start_time = time.ticks_ms()
            bytes_written = 0
            while bytes_written < len(audio_data):
                chunk_size = min(BUFFER_LENGTH, len(audio_data) - bytes_written)
                chunk = audio_data[bytes_written:bytes_written + chunk_size]
                bytes_sent = i2s_out.write(chunk)
                bytes_written += bytes_sent
            
            play_time = time.ticks_ms() - start_time
            print(f"✅ 播放完成: {bytes_written}字节, {play_time}ms")
            
            # 清理内存
            del audio_data
            
            # 等待下次循环
            print("⏳ 等待2秒后继续...")
            time.sleep(2)
            
    except KeyboardInterrupt:
        print("\n🛑 用户停止测试")
    except Exception as e:
        print(f"❌ 测试出错: {e}")
    finally:
        # 清理资源
        try:
            i2s_in.deinit()
            i2s_out.deinit()
            print("🧹 资源清理完成")
        except:
            pass

def run_mic_only_test():
    """只测试麦克风录音"""
    print("🎙️ 麦克风专项测试")
    print("=" * 20)
    
    try:
        i2s_in = I2S(0, sck=Pin(5), ws=Pin(4), sd=Pin(6),
                     mode=I2S.RX, bits=16, format=I2S.MONO,
                     rate=16000, ibuf=4096)
        
        print("📢 请对着麦克风说话...")
        
        for i in range(10):  # 测试10次
            buffer = bytearray(1024)
            bytes_read = i2s_in.readinto(buffer)
            
            # 计算音频能量
            energy = 0
            for j in range(0, bytes_read-1, 2):
                sample = int.from_bytes(buffer[j:j+2], 'little', True)
                energy += abs(sample)
            
            avg_energy = energy / (bytes_read // 2) if bytes_read > 0 else 0
            
            if avg_energy > 100:
                print(f"🔊 {i+1}: 检测到声音 (能量: {avg_energy:.0f})")
            else:
                print(f"🔇 {i+1}: 静音 (能量: {avg_energy:.0f})")
            
            time.sleep(0.5)
        
        i2s_in.deinit()
        print("✅ 麦克风测试完成")
        
    except Exception as e:
        print(f"❌ 麦克风测试失败: {e}")

if __name__ == "__main__":
    print("🎯 选择测试模式:")
    print("1. 完整循环测试 (录音+播放)")
    print("2. 麦克风专项测试")
    print()
    
    print("🚀 运行完整循环测试...")
    run_simple_loopback_test()
    
    # 如果需要只测试麦克风，取消下面的注释
    # print("🚀 运行麦克风专项测试...")
    # run_mic_only_test() 