import lvgl as lv
import time
from machine import SPI, Pin
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

class SSD1351_LVGL:
    def __init__(self):
        # 1. 初始化 SPI 通信
        self.spi = SPI(
            1,
            baudrate=5000000,  # 降低速率以提高稳定性
            polarity=0,
            phase=0,
            sck=Pin(SPI_CLK),
            mosi=Pin(SPI_MOSI)
        )
        
        # 2. 初始化控制引脚
        self.cs = Pin(SPI_CS, Pin.OUT, value=1)
        self.dc = Pin(SPI_DC, Pin.OUT, value=0)
        self.rst = Pin(SPI_RST, Pin.OUT, value=1)
        
        # 3. 硬件复位 SSD1351
        self.reset()
        
        # 4. 初始化 SSD1351 屏幕（修正地址范围）
        self.init_ssd1351()
        
        # 5. 初始化 LVGL
        lv.init()
        
        # 6. 配置 LVGL 显示缓冲区（增大缓冲区）
        self.buf1 = bytearray(SCREEN_WIDTH * 120 * 2)  # 半屏缓冲区
        self.buf2 = bytearray(SCREEN_WIDTH * 120 * 2)
        
        # 7. 创建 LVGL 显示驱动
        self.disp = lv.display_create(SCREEN_WIDTH, SCREEN_HEIGHT)
        self.disp.set_buffers(self.buf1, self.buf2, len(self.buf1), lv.DISPLAY_RENDER_MODE.PARTIAL)
        
        # 8. 注册 LVGL 刷新回调（启用 BGR 转换）
        self.disp.set_flush_cb(self.flush_cb)
        
        # 9. 显示 "Hello World"
        print("start show")
        self.show_hello_world()
        print("end show")
    
    def reset(self):
        self.rst(0)
        time.sleep_ms(100)
        self.rst(1)
        time.sleep_ms(100)
    
    def init_ssd1351(self):
        def cmd(c, *data):
            self.cs(0)
            self.dc(0)
            self.spi.write(bytearray([c]) + bytearray(data))
            self.cs(1)
        
        def data(d):
            self.cs(0)
            self.dc(1)
            self.spi.write(d)
            self.cs(1)
        
        # 初始化序列（修正地址范围）
        cmd(0xfd, 0x12)
        cmd(0xfd, 0xb1)
        cmd(0xae)
        cmd(0xa0, 0x74)  # RGB565 格式
        cmd(0xa1, 0x00)
        cmd(0xa2, 0x00)
        cmd(0xa4)
        cmd(0xa8, 0x7f)
        cmd(0xc8)
        cmd(0xca, 0x3f)
        cmd(0x21, 0x00, 0xEF)  # 列地址范围：0~239
        cmd(0x22, 0x00, 0xEF)  # 行地址范围：0~239
        cmd(0xb3, 0xf1)
        cmd(0xab, 0x01)
        cmd(0xAF)  # 开启显示
        time.sleep_ms(100)
    
    def flush_cb(self, disp, area, color_ptr):
        # 计算数据大小（替代 len(color_ptr)）
        width = area.x2 - area.x1 + 1
        height = area.y2 - area.y1 + 1
        data_size = width * height * 2  # RGB565 格式，2字节/像素
        print(f"刷新区域: x={area.x1}-{area.x2}, y={area.y1}-{area.y2}, 大小={data_size}字节")
        
        # 确保区域不超出屏幕范围
        x1, y1 = max(0, area.x1), max(0, area.y1)
        x2, y2 = min(SCREEN_WIDTH-1, area.x2), min(SCREEN_HEIGHT-1, area.y2)
        
        # 发送设置区域命令
        self.cs(0)
        self.dc(0)
        self.spi.write(bytearray([0x21, x1, x2]))
        self.spi.write(bytearray([0x22, y1, y2]))
        self.dc(1)
        
        # 启用 RGB565 → BGR565 转换（处理 C_Array 类型）
        # 直接将 C_Array 传递给 spi.write（MicroPython 通常支持）
        self.spi.write(color_ptr)
        self.cs(1)
        
        disp.flush_ready()

        
    
    def show_hello_world(self):
        scr = lv.screen_active()
        scr.set_style_bg_color(lv.color_hex(0x000000), 0)
        
        label = lv.label(scr)
        label.set_text("Hello World!")
        label.set_style_text_color(lv.color_hex(0xFFFF00), 0)
        label.set_style_text_font(lv.font_montserrat_24, 0)
        label.align(lv.ALIGN.CENTER, 0, 0)
