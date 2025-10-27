import machine
import time
from micropython import const

# 屏幕参数
SCREEN_WIDTH = const(128)
SCREEN_HEIGHT = const(128)

# 引脚定义 (根据实际连接修改)
SPI_MOSI = const(11)  # DIN
SPI_CLK = const(12)   # CLK/SCK
SPI_CS = const(10)    # 片选
SPI_DC = const(9)     # 数据/命令选择
SPI_RST = const(8)    # 复位

# SSD1351命令定义
CMD_SET_COLUMN = const(0x15)
CMD_SET_ROW = const(0x75)
CMD_WRITE_RAM = const(0x5C)
CMD_SET_REMAP = const(0xA0)
CMD_START_LINE = const(0xA1)
CMD_DISPLAY_OFFSET = const(0xA2)
CMD_DISPLAY_MODE_ALL_ON = const(0xA4)
CMD_DISPLAY_MODE_ALL_OFF = const(0xA5)
CMD_DISPLAY_MODE_NORMAL = const(0xA6)
CMD_DISPLAY_MODE_INVERSE = const(0xA7)
CMD_FUNCTION_SELECT = const(0xAB)
CMD_SLEEP_MODE_ON = const(0xAE)
CMD_SLEEP_MODE_OFF = const(0xAF)
CMD_PRECHARGE = const(0xB1)
CMD_CLOCKDIV = const(0xB3)
CMD_SET_VSL = const(0xB4)
CMD_SET_GPIO = const(0xB5)
CMD_PRECHARGE2 = const(0xB6)
CMD_GRAY_SCALE = const(0xB8)
CMD_LUT = const(0xB9)
CMD_SET_VPP = const(0xBB)
CMD_SET_VSH = const(0xBE)
CMD_CONTRAST_ABC = const(0xC1)
CMD_CONTRAST_MASTER = const(0xC7)
CMD_MUX_RATIO = const(0xCA)
CMD_COMMAND_LOCK = const(0xFD)


class OLEDDisplay:
    

    def __init__(self):
        # 初始化SPI
        self.spi = machine.SPI(1, baudrate=20_000_000, polarity=0, phase=0,
                              sck=machine.Pin(SPI_CLK), mosi=machine.Pin(SPI_MOSI))
        
        # 初始化控制引脚
        self.cs = machine.Pin(SPI_CS, machine.Pin.OUT)
        self.dc = machine.Pin(SPI_DC, machine.Pin.OUT)
        self.rst = machine.Pin(SPI_RST, machine.Pin.OUT)
        
        # 复位屏幕
        self.reset()
        
        # 初始化SSD1351
        self.init_display()
        
        # 清屏
        self.fill(0)
    
    def reset(self):
        """复位屏幕"""
        self.rst.value(0)
        time.sleep_ms(100)
        self.rst.value(1)
        time.sleep_ms(100)
    
    def write_cmd(self, cmd):
        """写入命令"""
        self.dc.value(0)
        self.cs.value(0)
        self.spi.write(bytes([cmd]))
        self.cs.value(1)
    
    def write_data(self, data):
        """写入数据"""
        self.dc.value(1)
        self.cs.value(0)
        self.spi.write(data)
        self.cs.value(1)
    
    def init_display(self):
        """初始化SSD1351显示控制器"""
        # 解锁命令
        self.write_cmd(CMD_COMMAND_LOCK)
        self.write_data(b'\x12')
        
        # 退出睡眠模式
        self.write_cmd(CMD_SLEEP_MODE_OFF)
        time.sleep_ms(100)
        
        # 设置时钟分频/振荡器频率
        self.write_cmd(CMD_CLOCKDIV)
        self.write_data(b'\xF1')  # 7:4 = Oscillator Frequency, 3:0 = CLK Div Ratio
        
        # 设置复用率
        self.write_cmd(CMD_MUX_RATIO)
        self.write_data(b'\x7F')  # 127 + 1 = 128
        
        # 设置显示偏移
        self.write_cmd(CMD_DISPLAY_OFFSET)
        self.write_data(b'\x00')
        
        # 设置开始行
        self.write_cmd(CMD_START_LINE)
        self.write_data(b'\x00')
        
        # 设置GPIO
        self.write_cmd(CMD_SET_GPIO)
        self.write_data(b'\x00')  # GPIO pins输入禁用
        
        # 设置功能选择
        self.write_cmd(CMD_FUNCTION_SELECT)
        self.write_data(b'\x01')  # 使能内部VDD稳压器
        
        # 设置重映射和颜色深度
        self.write_cmd(CMD_SET_REMAP)
        self.write_data(b'\x70')  # 水平地址递增，垂直地址递增，RGB排列
        
        # 设置VSL
        self.write_cmd(CMD_SET_VSL)
        self.write_data(b'\xA0\xB5\x55')
        
        # 设置对比度
        self.write_cmd(CMD_CONTRAST_ABC)
        self.write_data(b'\xC8\x80\xC8')
        
        # 设置主对比度
        self.write_cmd(CMD_CONTRAST_MASTER)
        self.write_data(b'\x0F')
        
        # 设置预充电周期
        self.write_cmd(CMD_PRECHARGE)
        self.write_data(b'\x32')  # Phase 2: 5, Phase 1: 3
        
        # 设置预充电电压
        self.write_cmd(CMD_SET_VPP)
        self.write_data(b'\x17')  # 0.6 * Vcc
        
        # 设置VCOMH电压
        self.write_cmd(CMD_SET_VSH)
        self.write_data(b'\x05')  # 0.82 * Vcc
        
        # 设置显示模式为正常
        self.write_cmd(CMD_DISPLAY_MODE_NORMAL)
        
        # 开启显示
        self.write_cmd(CMD_SLEEP_MODE_OFF)
    
    def set_window(self, x0, y0, x1, y1):
        """设置显示窗口"""
        self.write_cmd(CMD_SET_COLUMN)
        self.write_data(bytes([x0, x1]))
        self.write_cmd(CMD_SET_ROW)
        self.write_data(bytes([y0, y1]))
        self.write_cmd(CMD_WRITE_RAM)
    
    def fill(self, color):
        """填充整个屏幕"""
        self.set_window(0, 0, SCREEN_WIDTH-1, SCREEN_HEIGHT-1)
        
        # 将16位颜色拆分为高字节和低字节
        hi = color >> 8
        lo = color & 0xFF
        
        # 准备填充数据 (一次发送多个像素数据以提高速度)
        data = bytes([hi, lo]) * (SCREEN_WIDTH * SCREEN_HEIGHT)
        
        # 发送数据
        self.dc.value(1)
        self.cs.value(0)
        self.spi.write(data)
        self.cs.value(1)
    
    def draw_test_pattern(self):
        """绘制测试图案"""
        colors = [
            0xF800,  # 红色
            0x07E0,  # 绿色
            0x001F,  # 蓝色
            0xFFFF,  # 白色
            0xF81F,  # 品红
            0xFFE0,  # 黄色
            0x07FF,  # 青色
            0x0000   # 黑色
        ]
        
        block_height = SCREEN_HEIGHT // len(colors)
        
        for i, color in enumerate(colors):
            y0 = i * block_height
            y1 = y0 + block_height - 1 if i < len(colors)-1 else SCREEN_HEIGHT-1
            self.set_window(0, y0, SCREEN_WIDTH-1, y1)
            
            # 准备数据
            hi = color >> 8
            lo = color & 0xFF
            data = bytes([hi, lo]) * (SCREEN_WIDTH * (y1 - y0 + 1))
            
            # 发送数据
            self.dc.value(1)
            self.cs.value(0)
            self.spi.write(data)
            self.cs.value(1)

# 主程序
# def main():
#     display = oledDisplay()
#     print("SSD1351初始化完成!")

#     # 显示测试图案
#     display.draw_test_pattern()
#     print("显示测试图案")

#     # 延时后填充红色
#     time.sleep(3)
#     display.fill(0xF800)  # 红色
#     print("填充红色")

#     # 延时后填充绿色
#     time.sleep(3)
#     display.fill(0x07E0)  # 绿色
#     print("填充绿色")

#     # 延时后填充蓝色
#     time.sleep(3)
#     display.fill(0x001F)  # 蓝色
#     print("填充蓝色")

# if __name__ == "__main__":
#     main()