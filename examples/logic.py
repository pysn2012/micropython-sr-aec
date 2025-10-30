import espsr
import machine
import time

class VoiceAssistant:
    def __init__(self):
        # 初始化 I2S
        self.i2s = machine.I2S(0,
            mode=machine.I2S.PDM_DUPLEX,
            rate=16000,
            bits=16,
            bck=18,  # PDM CLK
            ws=19,   # PDM DAT
            din=23,  # MIC DAT
            dout=25, # SPK DAT
        )
        
        # 初始化 ESP-SR
        espsr.start()
        
        # 配置 AEC 参数
        espsr.enable_aec(True)
        espsr.set_aec_params(0.6, 512)  # 抑制水平和滤波器长度
        
        # 配置打断参数
        espsr.enable_interrupt(True)
        espsr.set_interrupt_params(500000, 500)  # 最小能量和冷却时间
        
        # 状态标志
        self.is_playing = False
        self.is_recording = False
        
    def start(self):
        """启动语音助手主循环"""
        print("语音助手启动...")
        
        while True:
            # 等待语音命令
            result = espsr.wait_for_result()
            
            if result is None:
                continue
                
            wakenet_mode, state, command_id = result
            
            # 处理打断事件
            if command_id == -1:  # 特殊 ID 表示打断
                if self.is_playing:
                    print("检测到语音打断")
                    self.stop_playback()
                    self.start_recording()
                continue
            
            # 处理唤醒词
            if wakenet_mode == espsr.DETECTED:
                print("唤醒词已触发")
                self.start_recording()
                continue
            
            # 处理命令词
            if state == espsr.DETECTED:
                self.handle_command(command_id)
    
    def start_recording(self):
        """开始录音"""
        if not self.is_recording:
            print("开始录音...")
            espsr.start_recording()
            self.is_recording = True
    
    def stop_recording(self):
        """停止录音"""
        if self.is_recording:
            print("停止录音")
            espsr.stop_recording()
            self.is_recording = False
    
    def start_playback(self, audio_data):
        """开始播放"""
        if not self.is_playing:
            print("开始播放...")
            espsr.start_playback(audio_data)
            self.is_playing = True
    
    def stop_playback(self):
        """停止播放"""
        if self.is_playing:
            print("停止播放")
            espsr.stop_playback()
            self.is_playing = False
    
    def handle_command(self, command_id):
        """处理命令词"""
        print(f"处理命令: {command_id}")
        # 在这里添加命令处理逻辑
        
# 创建并启动语音助手
assistant = VoiceAssistant()
assistant.start()