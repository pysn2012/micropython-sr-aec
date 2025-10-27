"""
ESP-SR完整演示程序
基于乐鑫ESP-SR框架 + MicroPython
完全复现C代码的功能和逻辑

硬件要求:
- ESP32-S3开发板 (8MB Flash + PSRAM)
- I2S数字麦克风 (INMP441): SCK=GPIO5, WS=GPIO4, SD=GPIO6
- 脉冲输出引脚: GPIO4 (可连接LED或其他指示器)

使用前准备:
1. 确保已烧录包含ESP-SR的MicroPython固件
2. 确保已烧录模型数据到model分区
3. 运行: exec(open('espsr_demo.py').read())
"""

import espsr
import time
import sys

class ESPSRDemo:
    def __init__(self):
        self.initialized = False
        self.commands = {}
        self.wakenet_detected = False
        
    def print_banner(self):
        """打印程序横幅"""
        print("=" * 70)
        print("🎤 ESP32-S3 语音识别完整演示")
        print("基于乐鑫ESP-SR框架 - 复现C代码功能")
        print("=" * 70)
    
    def init_system(self):
        """初始化ESP-SR系统"""
        try:
            print("🔄 正在初始化ESP-SR系统...")
            print("   - 初始化I2S音频接口")
            print("   - 加载模型分区数据")
            print("   - 配置MultiNet中文模型")
            print("   - 设置默认命令词")
            
            espsr.init()
            self.initialized = True
            
            print("✅ ESP-SR系统初始化成功!")
            return True
            
        except Exception as e:
            print(f"❌ ESP-SR初始化失败: {e}")
            print("\n💡 请检查:")
            print("   1. 是否烧录了包含ESP-SR的MicroPython固件")
            print("   2. 是否烧录了模型数据到model分区")
            print("   3. I2S麦克风硬件连接是否正确")
            print("   4. ESP32-S3是否有足够的PSRAM")
            return False
    
    def show_system_info(self):
        """显示系统信息"""
        print("\n📊 系统详细信息:")
        print("-" * 50)
        
        try:
            # 显示命令列表
            self.commands = espsr.get_commands()
            print(f"📝 支持的命令数量: {len(self.commands)}")
            print("📋 命令词列表:")
            for cmd_id, cmd_text in self.commands.items():
                cmd_type = "唤醒词" if cmd_id <= 1 else "控制指令"
                print(f"   [{cmd_id:2d}] {cmd_text:<20} ({cmd_type})")
            
            # 显示模型信息
            model_info = espsr.get_model_info()
            print(f"\n🔧 模型配置:")
            for key, value in model_info.items():
                print(f"   {key}: {value}")
            
            print(f"\n🎯 硬件配置:")
            print(f"   I2S SCK引脚: GPIO5")
            print(f"   I2S WS引脚:  GPIO4") 
            print(f"   I2S SD引脚:  GPIO6")
            print(f"   脉冲输出:    GPIO4")
            
        except Exception as e:
            print(f"❌ 获取系统信息失败: {e}")
    
    def test_single_detection(self):
        """测试单次检测功能"""
        print("\n🔍 单次检测测试")
        print("-" * 30)
        print("📢 请说话...")
        
        for i in range(5):
            print(f"第 {i+1} 次检测...", end=" ")
            result = espsr.detect_once()
            
            if result:
                self.process_detection_result(result)
                return result
            else:
                print("⭕ 无检测结果")
            
            time.sleep_ms(300)
        
        print("单次检测测试完成")
        return None
    
    def continuous_detection_demo(self):
        """连续检测演示 - 复现C代码的双任务逻辑"""
        print("\n🎤 连续语音检测模式")
        print("=" * 50)
        print("📢 语音检测说明:")
        print("   1️⃣  首先说唤醒词: '嗨，小乐' 或 '小乐小乐'")
        print("   2️⃣  听到确认音后，可说控制指令")
        print("   3️⃣  支持的指令请参考上方命令列表")
        print("   ⏹️  按 Ctrl+C 退出检测")
        print("-" * 50)
        
        try:
            detection_count = 0
            wakeup_count = 0
            command_count = 0
            
            while True:
                detection_count += 1
                print(f"\n[{detection_count:04d}] 监听中...", end=" ")
                
                result = espsr.detect_once()
                
                if result:
                    result_type = result.get('type', 'unknown')
                    
                    if result_type == 'wakeup':
                        wakeup_count += 1
                        self.wakenet_detected = True
                        print(f"\n🎉 [唤醒 #{wakeup_count}] 检测到唤醒词!")
                        self.process_wakeup_result(result)
                        
                        # 唤醒后进入命令监听模式
                        print("🔊 系统已唤醒，等待指令...")
                        self.wait_for_command()
                        
                    elif result_type == 'command':
                        command_count += 1
                        print(f"\n🔧 [指令 #{command_count}] 检测到命令!")
                        self.process_command_result(result)
                        
                    elif result_type == 'detecting':
                        print("🔍", end="")  # 检测中指示
                        
                    elif result_type == 'timeout':
                        print("⏰ 超时")
                        self.wakenet_detected = False
                        
                    elif result_type == 'channel_verified':
                        print("✓ 通道验证")
                        
                    else:
                        print(f"❓ 未知结果: {result}")
                else:
                    print("⭕", end="")  # 无结果指示
                
                time.sleep_ms(50)  # 减少CPU使用率
                
        except KeyboardInterrupt:
            print(f"\n\n⏹️  检测结束")
            print(f"📊 统计结果:")
            print(f"   总检测次数: {detection_count}")
            print(f"   唤醒次数: {wakeup_count}")
            print(f"   命令次数: {command_count}")
            
        except Exception as e:
            print(f"\n❌ 检测过程出错: {e}")
    
    def wait_for_command(self):
        """等待命令词 - 类似C代码中的命令监听逻辑"""
        print("   等待命令词 (5秒超时)...")
        
        for i in range(50):  # 5秒，每100ms检测一次
            result = espsr.detect_once()
            
            if result:
                result_type = result.get('type', 'unknown')
                
                if result_type == 'command':
                    print("   ✅ 收到命令!")
                    self.process_command_result(result)
                    return True
                elif result_type == 'timeout':
                    print("   ⏰ 命令监听超时")
                    return False
            
            time.sleep_ms(100)
        
        print("   ❌ 未收到有效命令")
        return False
    
    def process_detection_result(self, result):
        """处理检测结果"""
        result_type = result.get('type', 'unknown')
        
        if result_type == 'wakeup':
            self.process_wakeup_result(result)
        elif result_type == 'command':
            self.process_command_result(result)
        else:
            print(f"📋 其他结果: {result}")
    
    def process_wakeup_result(self, result):
        """处理唤醒词结果 - 基于C代码逻辑"""
        model_index = result.get('model_index', -1)
        word_index = result.get('word_index', -1)
        
        print(f"   模型索引: {model_index}")
        print(f"   词汇索引: {word_index}")
        print(f"   状态: {result.get('state', 'UNKNOWN')}")
        
        # 发送确认脉冲 (基于C代码的send_pulse功能)
        try:
            espsr.send_pulse()
            print("   📡 已发送确认脉冲信号")
        except:
            print("   ⚠️  脉冲信号发送失败")
    
    def process_command_result(self, result):
        """处理命令词结果 - 基于C代码逻辑"""
        command_id = result.get('command_id', -1)
        phrase_id = result.get('phrase_id', -1)
        prob = result.get('prob', 0.0)
        
        print(f"   命令ID: {command_id}")
        print(f"   短语ID: {phrase_id}")
        print(f"   置信度: {prob:.3f}")
        
        # 查找命令文本
        if command_id in self.commands:
            command_text = self.commands[command_id]
            print(f"   命令内容: '{command_text}'")
            
            # 执行相应动作
            self.execute_command(command_id, command_text)
        else:
            print(f"   ❌ 未知命令ID: {command_id}")
        
        # 发送确认脉冲 (C代码中检测到命令后会调用send_pulse)
        try:
            espsr.send_pulse()
            print("   📡 已发送确认脉冲信号")
        except:
            print("   ⚠️  脉冲信号发送失败")
    
    def execute_command(self, command_id, command_text):
        """执行具体命令 - 基于sdkconfig中的预设命令"""
        print(f"   🔧 执行命令: {command_text}")
        
        if "kong tiao" in command_text:  # 空调相关
            if "da kai" in command_text:
                print("   ❄️  空调已开启")
            elif "guan bi" in command_text:
                print("   🔌 空调已关闭")
        elif "feng su" in command_text:  # 风速相关
            if "zeng da" in command_text:
                print("   💨 风速已增大")
            elif "jian xiao" in command_text:
                print("   💨 风速已减小")
        elif "yi du" in command_text:  # 温度相关
            if "sheng gao" in command_text:
                print("   🌡️  温度已升高")
            elif "jiang di" in command_text:
                print("   🌡️  温度已降低")
        elif "mo shi" in command_text:  # 模式相关
            if "zhi re" in command_text:
                print("   🔥 已切换到制热模式")
            elif "zhi leng" in command_text:
                print("   ❄️  已切换到制冷模式")
        elif "deng" in command_text:  # 灯光相关
            if "da kai" in command_text:
                print("   💡 灯已打开")
            elif "guan bi" in command_text:
                print("   🔌 灯已关闭")
        else:
            print(f"   ✅ 已执行自定义命令")
    
    def add_custom_command_demo(self):
        """添加自定义命令演示"""
        print("\n➕ 添加自定义命令演示")
        print("-" * 30)
        
        try:
            # 添加一些测试命令
            test_commands = [
                (20, "ni hao shi jie"),
                (21, "zai jian"),
                (22, "xie xie")
            ]
            
            for cmd_id, cmd_text in test_commands:
                espsr.add_command(cmd_id, cmd_text)
                print(f"✅ 添加命令: {cmd_id} -> '{cmd_text}'")
            
            print("自定义命令添加完成，可在连续检测中测试")
            
        except Exception as e:
            print(f"❌ 添加自定义命令失败: {e}")
    
    def show_menu(self):
        """显示主菜单"""
        print("\n" + "=" * 50)
        print("🎤 ESP-SR 语音识别演示菜单")
        print("=" * 50)
        print("1. 显示系统信息")
        print("2. 单次检测测试")
        print("3. 连续语音识别演示")
        print("4. 添加自定义命令")
        print("5. 发送测试脉冲")
        print("6. 退出程序")
        print("-" * 50)
    
    def test_pulse(self):
        """测试脉冲输出"""
        print("\n📡 测试脉冲输出")
        print("GPIO4将输出500ms高电平脉冲...")
        
        try:
            espsr.send_pulse()
            print("✅ 脉冲信号发送完成")
        except Exception as e:
            print(f"❌ 脉冲信号发送失败: {e}")
    
    def run(self):
        """运行主程序"""
        self.print_banner()
        
        # 初始化系统
        if not self.init_system():
            return
        
        # 显示系统信息
        self.show_system_info()
        
        # 主菜单循环
        while True:
            self.show_menu()
            
            try:
                choice = input("请选择 (1-6): ").strip()
                
                if choice == '1':
                    self.show_system_info()
                elif choice == '2':
                    self.test_single_detection()
                elif choice == '3':
                    self.continuous_detection_demo()
                elif choice == '4':
                    self.add_custom_command_demo()
                elif choice == '5':
                    self.test_pulse()
                elif choice == '6':
                    print("👋 程序退出")
                    break
                else:
                    print("❌ 无效选择，请重新输入")
                    
            except KeyboardInterrupt:
                print("\n👋 程序被中断，退出")
                break
            except Exception as e:
                print(f"❌ 操作错误: {e}")
    
    def cleanup(self):
        """清理资源"""
        if self.initialized:
            try:
                print("🔄 正在清理ESP-SR资源...")
                espsr.cleanup()
                print("✅ 资源清理完成")
            except:
                pass

def main():
    """主函数"""
    demo = ESPSRDemo()
    
    try:
        demo.run()
    finally:
        demo.cleanup()

if __name__ == "__main__":
    main() 