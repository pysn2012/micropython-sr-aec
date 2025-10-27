import lvgl as lv
import lv_utils
import machine
import time
import struct
from micropython import const

# 屏幕参数
SCREEN_WIDTH = const(240)
SCREEN_HEIGHT = const(240)

# 引脚定义
SPI_MOSI = const(11)
SPI_CLK = const(12)
SPI_CS = const(10)
SPI_DC = const(9)
SPI_RST = const(8)

# SSD1351命令定义
CMD_SET_COLUMN = const(0x15)
CMD_SET_ROW = const(0x75)
CMD_WRITE_RAM = const(0x5C)

class oledLvgl:
    def __init__(self):
        # 初始化SPI
        self.spi = machine.SPI(1, baudrate=40_000_000, polarity=0, phase=0,
                             sck=machine.Pin(SPI_CLK), mosi=machine.Pin(SPI_MOSI))
        
        # 初始化控制引脚
        self.cs = machine.Pin(SPI_CS, machine.Pin.OUT)
        self.dc = machine.Pin(SPI_DC, machine.Pin.OUT)
        self.rst = machine.Pin(SPI_RST, machine.Pin.OUT)
        
        # 复位屏幕
        self.reset()
        
        # 初始化SSD1351
        self.init_display()
    
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
        if isinstance(data, int):
            data = bytes([data])
        self.spi.write(data)
        self.cs.value(1)
    
    def init_display(self):
        """初始化SSD1351显示控制器"""
        # 解锁命令
        self.write_cmd(0xFD)
        self.write_data(0x12)
        
        # 退出睡眠模式
        self.write_cmd(0xAF)
        time.sleep_ms(100)
        
        # 设置时钟分频
        self.write_cmd(0xB3)
        self.write_data(0xF1)
        
        # 设置复用率
        self.write_cmd(0xCA)
        self.write_data(0x7F)
        
        # 设置显示偏移
        self.write_cmd(0xA2)
        self.write_data(0x00)
        
        # 设置开始行
        self.write_cmd(0xA1)
        self.write_data(0x00)
        
        # 设置重映射和颜色深度
        self.write_cmd(0xA0)
        self.write_data(0x74)  # RGB排列
        
        # 设置对比度
        self.write_cmd(0xC1)
        self.write_data(0xC8)
        self.write_data(0x80)
        self.write_data(0xC8)
        
        # 设置主对比度
        self.write_cmd(0xC7)
        self.write_data(0x0F)
        
        # 设置预充电周期
        self.write_cmd(0xB1)
        self.write_data(0x32)
        
        # 设置VCOMH电压
        self.write_cmd(0xBE)
        self.write_data(0x05)
        
        # 设置显示模式为正常
        self.write_cmd(0xA6)
        
        # 设置显示区域为全屏
        self.set_window(0, 0, SCREEN_WIDTH-1, SCREEN_HEIGHT-1)
    
    def set_window(self, x0, y0, x1, y1):
        """设置显示窗口"""
        self.write_cmd(CMD_SET_COLUMN)
        self.write_data(bytes([x0, x1]))
        self.write_cmd(CMD_SET_ROW)
        self.write_data(bytes([y0, y1]))
        self.write_cmd(CMD_WRITE_RAM)
    
    def flush(self, disp_drv, area, color_p):
        """LVGL显示刷新回调函数"""
        # 设置显示区域
        self.set_window(area.x1, area.y1, area.x2, area.y2)
        
        # 准备发送数据
        self.dc.value(1)
        self.cs.value(0)
        
        # 转换颜色格式并发送
        size = (area.x2 - area.x1 + 1) * (area.y2 - area.y1 + 1)
        buf = bytearray(size * 2)
        for i in range(size):
            # 将LVGL的16位颜色转换为RGB565
            c = color_p[i]
            buf[i*2] = (c >> 8) & 0xFF
            buf[i*2+1] = c & 0xFF
        
        self.spi.write(buf)
        self.cs.value(1)
        
        # 通知LVGL刷新完成
        lv.disp_flush_ready(disp_drv)

    def init_lvgl(self):
        # 初始化LVGL
        lv.init()
        
        # 创建显示缓冲区
        buf1 = bytearray(240 * 10 * 2)  # 10行缓冲区
        buf2 = bytearray(240 * 10 * 2)  # 双缓冲
        
        # 注册显示驱动
        disp_buf = lv.disp_draw_buf_t()
        disp_buf.init(buf1, buf2, len(buf1) // 4)
        
        disp_drv = lv.disp_drv_t()
        disp_drv.init()
        disp_drv.draw_buf = disp_buf
        disp_drv.flush_cb = self.flush
        disp_drv.hor_res = SCREEN_WIDTH
        disp_drv.ver_res = SCREEN_HEIGHT
        disp_drv.register()

def create_demo_ui():
    # 创建简单的UI演示
    scr = lv.scr_act()
    
    # 创建一个标签
    label = lv.label(scr)
    label.set_text("Hello, LVGL!")
    label.center()
    
    # 创建一个按钮
    btn = lv.btn(scr)
    btn.set_size(100, 50)
    btn.align(lv.ALIGN.CENTER, 0, 50)
    
    btn_label = lv.label(btn)
    btn_label.set_text("Click me!")
    btn_label.center()
    
    # 按钮点击事件回调函数
    def btn_event_cb(e):
        print("Button clicked!")
        label.set_text("Button clicked!")
    
    # 添加按钮事件回调
    btn.add_event_cb(btn_event_cb, lv.EVENT.CLICKED, None)
    
    # 创建一个滑块
    slider = lv.slider(scr)
    slider.set_width(200)
    slider.align(lv.ALIGN.CENTER, 0, 100)
    slider.set_range(0, 100)
    slider.set_value(50, lv.ANIM.OFF)
    
    # 滑块值改变事件回调函数
    def slider_event_cb(e):
        val = slider.get_value()
        label.set_text(f"Slider: {val}%")
    
    # 添加滑块事件回调
    slider.add_event_cb(slider_event_cb, lv.EVENT.VALUE_CHANGED, None)

# 主程序
# def main():
  
    
#     # 初始化屏幕
#     display = oledLvgl()
    
#     # 初始化LVGL
#     display.init_lvgl()
    
#     # 创建演示UI
#     create_demo_ui()
    
#     # 主循环
#     while True:
#         lv.timer_handler()
#         time.sleep_ms(5)

# if __name__ == "__main__":
#     main()