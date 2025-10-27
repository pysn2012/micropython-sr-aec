import machine
import time
import gc
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

class SSD1351:
    def __init__(self):
        # 硬件SPI初始化
        self.spi = machine.SPI(1,
                             baudrate=40_000_000,
                             sck=machine.Pin(SPI_CLK),
                             mosi=machine.Pin(SPI_MOSI))
        self.cs = machine.Pin(SPI_CS, machine.Pin.OUT)
        self.dc = machine.Pin(SPI_DC, machine.Pin.OUT)
        self.rst = machine.Pin(SPI_RST, machine.Pin.OUT)
        
        # 初始化屏幕
        self.reset()
        self._init_display()

    def reset(self):
        self.rst(0)
        time.sleep_ms(100)
        self.rst(1)
        time.sleep_ms(100)

    def _init_display(self):
        init_cmds = [
            (0xFD, [0x12]),  # 解锁命令
            (0xFD, [0xB1]),  # 锁定命令
            (0xAE, []),      # 关闭显示
            (0xB3, [0xF1]),  # 时钟分频
            (0xCA, [0x7F]),  # 多路复用比例
            (0xA0, [0x74]),  # 颜色格式 (RGB)
            (0xA1, [0x00]),  # 显示起始行
            (0xA2, [0x00]),  # 显示偏移
            (0xAB, [0x01]),  # VDD调节
            (0xB1, [0x32]),  # 相位长度
            (0xBE, [0x05]),  # VCOMH电压
            (0xA6, []),      # 正常显示模式
            (0xAF, []),     # 开启显示
        ]
        
        for cmd, args in init_cmds:
            self._write_cmd(cmd)
            if args:
                self._write_data(bytearray(args))

    def _write_cmd(self, cmd):
        self.dc(0)
        self.cs(0)
        self.spi.write(bytearray([cmd]))
        self.cs(1)

    def _write_data(self, buf):
        self.dc(1)
        self.cs(0)
        self.spi.write(buf)
        self.cs(1)

    def flush(self, buf):
        """刷新整个屏幕"""
        self._write_cmd(0x15)  # 列地址
        self._write_data(bytearray([0, SCREEN_WIDTH-1]))
        self._write_cmd(0x75)  # 行地址
        self._write_data(bytearray([0, SCREEN_HEIGHT-1]))
        self._write_cmd(0x5C)  # 写RAM命令
        self._write_data(buf)