[![Unix CI badge](https://github.com/micropython/micropython/actions/workflows/ports_unix.yml/badge.svg)](https://github.com/micropython/micropython/actions?query=branch%3Amaster+event%3Apush) [![STM32 CI badge](https://github.com/micropython/micropython/actions/workflows/ports_stm32.yml/badge.svg)](https://github.com/micropython/micropython/actions?query=branch%3Amaster+event%3Apush) [![Docs CI badge](https://github.com/micropython/micropython/actions/workflows/docs.yml/badge.svg)](https://docs.micropython.org/) [![codecov](https://codecov.io/gh/micropython/micropython/branch/master/graph/badge.svg?token=I92PfD05sD)](https://codecov.io/gh/micropython/micropython)

The MicroPython project
=======================
<p align="center">
  <img src="https://raw.githubusercontent.com/micropython/micropython/master/logo/upython-with-micro.jpg" alt="MicroPython Logo"/>
</p>

This is the MicroPython project, which aims to put an implementation
of Python 3.x on microcontrollers and small embedded systems.
You can find the official website at [micropython.org](http://www.micropython.org).

WARNING: this project is in beta stage and is subject to changes of the
code-base, including project-wide name changes and API changes.

MicroPython implements the entire Python 3.4 syntax (including exceptions,
`with`, `yield from`, etc., and additionally `async`/`await` keywords from
Python 3.5 and some select features from later versions). The following core
datatypes are provided: `str`(including basic Unicode support), `bytes`,
`bytearray`, `tuple`, `list`, `dict`, `set`, `frozenset`, `array.array`,
`collections.namedtuple`, classes and instances. Builtin modules include
`os`, `sys`, `time`, `re`, and `struct`, etc. Some ports have support for
`_thread` module (multithreading), `socket` and `ssl` for networking, and
`asyncio`. Note that only a subset of Python 3 functionality is implemented
for the data types and modules.

MicroPython can execute scripts in textual source form (.py files) or from
precompiled bytecode (.mpy files), in both cases either from an on-device
filesystem or "frozen" into the MicroPython executable.

MicroPython also provides a set of MicroPython-specific modules to access
hardware-specific functionality and peripherals such as GPIO, Timers, ADC,
DAC, PWM, SPI, I2C, CAN, Bluetooth, and USB.

Getting started
---------------

See the [online documentation](https://docs.micropython.org/) for the API
reference and information about using MicroPython and information about how
it is implemented.

We use [GitHub Discussions](https://github.com/micropython/micropython/discussions)
as our forum, and [Discord](https://discord.gg/RB8HZSAExQ) for chat. These
are great places to ask questions and advice from the community or to discuss your
MicroPython-based projects.

For bugs and feature requests, please [raise an issue](https://github.com/micropython/micropython/issues/new/choose)
and follow the templates there.

For information about the [MicroPython pyboard](https://store.micropython.org/pyb-features),
the officially supported board from the
[original Kickstarter campaign](https://www.kickstarter.com/projects/214379695/micro-python-python-for-microcontrollers),
see the [schematics and pinouts](http://github.com/micropython/pyboard) and
[documentation](https://docs.micropython.org/en/latest/pyboard/quickref.html).

Contributing
------------

MicroPython is an open-source project and welcomes contributions. To be
productive, please be sure to follow the
[Contributors' Guidelines](https://github.com/micropython/micropython/wiki/ContributorGuidelines)
and the [Code Conventions](https://github.com/micropython/micropython/blob/master/CODECONVENTIONS.md).
Note that MicroPython is licenced under the MIT license, and all contributions
should follow this license.

About this repository
---------------------

This repository contains the following components:
- [py/](py/) -- the core Python implementation, including compiler, runtime, and
  core library.
- [mpy-cross/](mpy-cross/) -- the MicroPython cross-compiler which is used to turn scripts
  into precompiled bytecode.
- [ports/](ports/) -- platform-specific code for the various ports and architectures that MicroPython runs on.
- [lib/](lib/) -- submodules for external dependencies.
- [tests/](tests/) -- test framework and test scripts.
- [docs/](docs/) -- user documentation in Sphinx reStructuredText format. This is used to generate the [online documentation](http://docs.micropython.org).
- [extmod/](extmod/) -- additional (non-core) modules implemented in C.
- [tools/](tools/) -- various tools, including the pyboard.py module.
- [examples/](examples/) -- a few example Python scripts.

"make" is used to build the components, or "gmake" on BSD-based systems.
You will also need bash, gcc, and Python 3.3+ available as the command `python3`
(if your system only has Python 2.7 then invoke make with the additional option
`PYTHON=python2`). Some ports (rp2 and esp32) additionally use CMake.

Supported platforms & architectures
-----------------------------------

MicroPython runs on a wide range of microcontrollers, as well as on Unix-like
(including Linux, BSD, macOS, WSL) and Windows systems.

Microcontroller targets can be as small as 256kiB flash + 16kiB RAM, although
devices with at least 512kiB flash + 128kiB RAM allow a much more
full-featured experience.

The [Unix](ports/unix) and [Windows](ports/windows) ports allow both
development and testing of MicroPython itself, as well as providing
lightweight alternative to CPython on these platforms (in particular on
embedded Linux systems).

The ["minimal"](ports/minimal) port provides an example of a very basic
MicroPython port and can be compiled as both a standalone Linux binary as
well as for ARM Cortex M4. Start with this if you want to port MicroPython to
another microcontroller. Additionally the ["bare-arm"](ports/bare-arm) port
is an example of the absolute minimum configuration, and is used to keep
track of the code size of the core runtime and VM.

In addition, the following ports are provided in this repository:
 - [cc3200](ports/cc3200) -- Texas Instruments CC3200 (including PyCom WiPy).
 - [esp32](ports/esp32) -- Espressif ESP32 SoC (including ESP32S2, ESP32S3, ESP32C3, ESP32C6).
 - [esp8266](ports/esp8266) -- Espressif ESP8266 SoC.
 - [mimxrt](ports/mimxrt) -- NXP m.iMX RT (including Teensy 4.x).
 - [nrf](ports/nrf) -- Nordic Semiconductor nRF51 and nRF52.
 - [pic16bit](ports/pic16bit) -- Microchip PIC 16-bit.
 - [powerpc](ports/powerpc) -- IBM PowerPC (including Microwatt)
 - [qemu](ports/qemu) -- QEMU-based emulated target (for testing)
 - [renesas-ra](ports/renesas-ra) -- Renesas RA family.
 - [rp2](ports/rp2) -- Raspberry Pi RP2040 (including Pico and Pico W).
 - [samd](ports/samd) -- Microchip (formerly Atmel) SAMD21 and SAMD51.
 - [stm32](ports/stm32) -- STMicroelectronics STM32 family (including F0, F4, F7, G0, G4, H7, L0, L4, WB)
 - [webassembly](ports/webassembly) -- Emscripten port targeting browsers and NodeJS.
 - [zephyr](ports/zephyr) -- Zephyr RTOS.

The MicroPython cross-compiler, mpy-cross
-----------------------------------------

Most ports require the [MicroPython cross-compiler](mpy-cross) to be built
first.  This program, called mpy-cross, is used to pre-compile Python scripts
to .mpy files which can then be included (frozen) into the
firmware/executable for a port.  To build mpy-cross use:

    $ cd mpy-cross
    $ make

External dependencies
---------------------

The core MicroPython VM and runtime has no external dependencies, but a given
port might depend on third-party drivers or vendor HALs. This repository
includes [several submodules](lib/) linking to these external dependencies.
Before compiling a given port, use

    $ cd ports/name
    $ make submodules

to ensure that all required submodules are initialised.

---

## 🎯 ESP32 + ESP-SR (语音识别) 集成版本

本仓库是 MicroPython 的 ESP32 + ESP-SR 集成版本，支持语音唤醒和 AEC（回声消除）打断功能。

### ✨ 主要功能

- ✅ **ESP-SR 语音识别**：支持唤醒词检测和命令词识别
- ✅ **AEC 回声消除**：播放音频时支持语音打断
- ✅ **流式音频播放**：低内存占用的流式下载播放
- ✅ **共享录音缓冲区**：解决 I2S 资源冲突
- ✅ **完整测试脚本**：开箱即用的测试程序

### 📚 快速开始

#### 1. 编译固件
```bash
cd ports/esp32
idf.py build
idf.py flash
```

详细编译说明见：[ports/esp32/编译指南.md](ports/esp32/编译指南.md)

#### 2. 运行 AEC 打断测试
```bash
# 上传测试脚本到设备
# 使用 Thonny 将 ports/esp32/modules/test_logic.py 上传到 /flash/

# 在 REPL 中运行
>>> import test_logic
>>> sensor = test_logic.SensorSystem()
>>> sensor.run()
```

快速使用指南见：[ports/esp32/modules/README_TEST.md](ports/esp32/modules/README_TEST.md)

### 📖 文档索引

#### 核心文档
- [编译指南](ports/esp32/编译指南.md) - 固件编译和烧录
- [AEC 打断测试指南](ports/esp32/AEC打断测试指南.md) - 完整测试说明
- [快速使用指南](ports/esp32/modules/README_TEST.md) - 快速开始
- [文件管理清单](ports/esp32/文件管理清单.md) - 文件上传和删除清单

#### 技术文档
- **[v2.9.1 I2S冲突修复](ports/esp32/v2.9.1_I2S冲突修复.md)** - 🔥🔥🔥 **v2.9.1 最新！修复I2S冲突**
- [v2.9 C端播放线程方案](ports/esp32/v2.9_C端播放线程方案.md) - v2.9 终极AEC解决方案
- [v2.8 内存优化和C端喂入策略](ports/esp32/v2.8_内存优化和C端喂入策略.md) - v2.8 避免内存累积
- [AEC VOIP模式和诊断指南](ports/esp32/AEC_VOIP模式和诊断指南.md) - v2.4 VOIP模式和诊断
- [AEC 完整配置和降噪修复说明](ports/esp32/AEC完整配置和降噪修复说明.md) - v2.3 降噪配置
- [AEC 时序同步修复说明](ports/esp32/AEC时序同步修复说明.md) - v2.2 时序同步
- [VAD 功能添加说明](ports/esp32/VAD功能添加说明.md) - v2.1 VAD 集成
- [流式播放版本更新说明](ports/esp32/流式播放版本更新说明.md) - v2.0 流式播放
- [打断功能失败根因分析](ports/esp32/打断功能失败根因分析.md) - 问题分析
- [AEC 打断功能修复说明](ports/esp32/AEC打断功能修复说明.md) - 修复记录
- [I2S 资源冲突修复说明](ports/esp32/I2S资源冲突修复说明.md) - I2S 冲突解决
- [参考项目 AEC 实现详解](ports/esp32/参考项目AEC实现详解.md) - 参考实现分析
- [最终修复总结](ports/esp32/最终修复总结.md) - 完整修复清单

### 🔧 核心修改

#### C 代码修改
1. **`modespsr.c`**
   - 实现 AEC 参考信号缓冲区
   - 实现共享录音缓冲区
   - 增加队列大小（1 → 10）

2. **`machine_i2s.c`**
   - 增加 I2S 输出缓冲区（2048 → 8192）

#### Python 代码
1. **`modules/logic.py`**
   - 主应用逻辑（用于正式产品）
   - 支持 AEC 打断和共享录音

2. **`modules/test_logic.py`**
   - AEC 打断测试脚本（v2.0 流式播放版）
   - 边下载边播放，解决内存溢出

### 🚀 v2.0 流式播放版

**主要改进**：
- ✅ 解决内存溢出问题（内存占用从 ~200KB 降至 <8KB）
- ✅ 支持播放任意大小的音频文件
- ✅ 边下载边播放，延迟更低
- ✅ 网络容错，自动重试

**技术实现**：
```python
# v1.0 - 预下载（内存不足）
audio_data = download_entire_file(url)  # 需要 ~200KB
play(audio_data)

# v2.0 - 流式播放
socket = stream_from_url(url)           # 只需 ~8KB
while data := socket.recv(4096):
    play(data)
```

### 📦 需要上传的文件

**必须上传到设备**：
- `modules/test_logic.py` → `/flash/test_logic.py`（AEC 测试脚本）

**可选上传**：
- `modules/README_TEST.md` → `/flash/README_TEST.md`（使用说明）

### 🗑️ 已删除的文件

以下旧版本/过时文档已删除：
- ❌ `test_aec_interrupt.py`（v1.0，内存溢出）
- ❌ `AEC_IMPLEMENTATION_CHECKLIST.md`（已完成）
- ❌ `AEC_COMPLETE_IMPLEMENTATION_GUIDE.md`（已替代）
- ❌ `AEC实现方案总结.md`（已替代）

### 🆘 技术支持

遇到问题？
1. 查看 [AEC 打断测试指南](ports/esp32/AEC打断测试指南.md)
2. 查看 [打断功能失败根因分析](ports/esp32/打断功能失败根因分析.md)
3. 检查 [文件管理清单](ports/esp32/文件管理清单.md)

### 📝 更新日志

**2025-10-27 - v2.0**
- ✅ 实现流式音频播放，解决内存溢出
- ✅ 优化 AEC 打断检测（队列 1→10，检测间隔 5→1）
- ✅ 完善文档和测试脚本
- ✅ 删除过时文档

---
