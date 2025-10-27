"""
INMP441麦克风 I2S硬件诊断工具
用于测试麦克风连接和I2S配置
"""
import espsr
import time

def diagnose_i2s_hardware():
    """诊断I2S硬件连接"""
    print("🔧 INMP441麦克风硬件诊断")
    print("=" * 40)
    
    try:
        # 初始化ESP-SR (会同时初始化I2S)
        print("🔄 初始化I2S硬件...")
        espsr.init()
        
        # 获取模型信息
        model_info = espsr.get_model_info()
        print(f"📊 系统信息: {model_info}")
        
        print("\n🎙️ 开始音频质量检测...")
        print("📢 请对着麦克风说话或制造声音...")
        
        # 进行短时间检测，主要查看音频质量
        for i in range(5):
            print(f"\n🔍 第{i+1}次检测 (2秒)...")
            result = espsr.listen(2)  # 短时间检测
            
            if result:
                print(f"📋 检测结果: {result}")
            else:
                print("❌ 无检测结果")
            
            time.sleep(1)
        
        print("\n✅ 硬件诊断完成")
        
    except Exception as e:
        print(f"❌ 诊断过程出错: {e}")
        import sys
        sys.print_exception(e)
        
    finally:
        try:
            espsr.cleanup()
            print("🧹 资源清理完成")
        except:
            print("⚠️ 资源清理异常")

if __name__ == "__main__":
    diagnose_i2s_hardware() 