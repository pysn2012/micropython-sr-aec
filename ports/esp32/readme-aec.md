@modespsr.c @machine_i2s.c @logic.py 
这是当前实现的语音逻辑的 micropython 的 c 代码，通过 multinet 模型来实现自定义唤醒词来唤醒，然后通过“嗨小乐” 或 “嗨小天” 唤醒，然后录音问话，然后云端返回回复音频，播放回复音频。
我的需求是在播放回复音频的时候可以随时AEC打断，比如说任何话，或者唤醒词都可以打断对话并继续录音，在请求云端进行回复。
目前遇到问题是：当前的 micropython 的代码无法实现打断的逻辑，通过唤醒词或其他语音问话，无法打断当前的播放，需要解决。

硬件为 PDM 麦克风，喇叭 IIS 输出。
现在是硬件不变，另一个C++代码的项目实现了AEC 打断的逻辑，当播放回复音频时，任何问话或语音都可打断当前播放，同时获得打断的问话录音并继续进入下一轮对话播放。

# 可实现 AEC 的项目代码为：
/Users/renzhaojing/gitcode/renhejia/source/xiaozhi-esp32-pan/
代码支持多个板子，基于板子为zhengchen-1.54tft-wifi 
语音唤醒监听部分代码见： /Users/renzhaojing/gitcode/renhejia/source/xiaozhi-esp32-pan/main/audio_processing/
实现 pdm 和 喇叭初始化的方法为：NoAudioCodecSimplexPdm::NoAudioCodecSimplexPdm(int input_sample_rate, int output_sample_rate, gpio_num_t spk_bclk, gpio_num_t spk_ws, gpio_num_t spk_dout, gpio_num_t mic_sck, gpio_num_t mic_din) 

需求：
需要查看这个项目zhengchen-1.54tft-wifi  是如何实现 AEC 的，我需要将当前 micropython 的代码改为支持 AEC，在播放回复音频时可以任意语音打断，并录音进行下一轮对话。
给出改动建议。

