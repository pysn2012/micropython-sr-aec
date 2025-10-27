"""
ESP-SR 直接语音命令识别测试
跳过唤醒词，直接识别语音命令
"""
import espsr
import time

def test_direct_voice_commands():
    """测试直接语音命令识别"""
    print("🚀 ESP-SR 直接语音命令识别测试")
    print("=" * 45)
    
    try:
        # 初始化ESP-SR
        print("🔄 初始化ESP-SR...")
        espsr.init()
        print("✅ ESP-SR初始化完成")
        
        # 显示命令列表
        commands = espsr.get_commands()
        print("\n📝 支持的语音命令:")
        for cmd_id, cmd_text in commands.items():
            print(f"  {cmd_id}: {cmd_text}")
        
        print("\n📢 使用说明 (符合readme.md要求):")
        print("🎯 直接说以下命令:")
        print("   - '嗨小乐' (hai xiao le) - 唤醒AIBOX")
        print("   - '开灯' / '关灯'")
        print("   - '打开' / '关闭'") 
        print("   - '大声' / '小声'")
        print("   - '开始' / '停止'")
        print("   - '连接网络'")
        
        # 进行多轮测试
        for round_num in range(8):
            print(f"\n🔄 第{round_num + 1}轮测试")
            print("📢 请直接说语音命令...")
            print("⏱️  监听10秒...")
            
            try:
                result = espsr.listen(10)  # 监听10秒
                
                if result:
                    result_type = result.get('type', 'unknown')
                    command_id = result.get('command_id', -1)
                    prob = result.get('prob', 0.0)
                    
                    print(f"✅ 检测结果: {result}")
                    
                    if result_type == 'wakeup':
                        print("🎉 SUCCESS! AIBOX已唤醒!")
                        print(f"   唤醒词: {commands.get(command_id, 'unknown')}")
                        print(f"   置信度: {prob:.3f}")
                        print("🔔 AIBOX: 您好！我是小乐，有什么可以帮您的吗？")
                    
                    elif result_type == 'command':
                        print("🎯 SUCCESS! 检测到语音命令!")
                        print(f"   命令ID: {command_id}")
                        print(f"   置信度: {prob:.3f}")
                        if command_id in commands:
                            print(f"   命令内容: {commands[command_id]}")
                            
                        # 根据命令类型给出AIBOX反馈 (符合readme.md交互逻辑)
                        if command_id == 1:  # kai deng
                            print("💡 AIBOX: 好的，为您开灯")
                        elif command_id == 2:  # guan deng
                            print("💡 AIBOX: 好的，为您关灯")
                        elif command_id == 3:  # da kai
                            print("🔓 AIBOX: 已为您打开")
                        elif command_id == 4:  # guan bi
                            print("🔒 AIBOX: 已为您关闭")
                        elif command_id == 5:  # da sheng
                            print("🔊 AIBOX: 音量已调大")
                        elif command_id == 6:  # xiao sheng
                            print("🔉 AIBOX: 音量已调小")
                        elif command_id == 7:  # kai shi
                            print("▶️ AIBOX: 已开始")
                        elif command_id == 8:  # ting zhi
                            print("⏹️ AIBOX: 已停止")
                        elif command_id == 9:  # lian jie wang luo
                            print("📶 AIBOX: 请先连接网络，然后我就可以为您提供更多服务")
                    
                    elif result_type == 'timeout':
                        print("⏰ 监听超时，请重试")
                    
                    elif result_type == 'detecting':
                        print("🔄 正在检测中...")
                    
                    else:
                        print(f"❓ 未知结果类型: {result_type}")
                
                else:
                    print("❌ 无检测结果")
                
            except Exception as e:
                print(f"❌ 检测异常: {e}")
            
            if round_num < 7:  # 不是最后一轮
                print("⏱️  等待2秒后开始下一轮...")
                time.sleep(2)
        
        print("\n🏁 测试完成")
        
    except Exception as e:
        print(f"❌ 测试过程出错: {e}")
        import sys
        sys.print_exception(e)
        
    finally:
        print("\n🔄 清理资源...")
        try:
            espsr.cleanup()
            print("✅ 资源清理完成")
        except:
            print("⚠️ 资源清理异常")

class SensorSystem:
    def __init__(self):
        pass
        
    def run(self):
        test_direct_voice_commands()

if __name__ == "__main__":
    test_direct_voice_commands() 