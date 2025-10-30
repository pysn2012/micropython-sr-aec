#ifndef _MODESPSR_AEC_H_
#define _MODESPSR_AEC_H_

#include "py/obj.h"
#include "py/runtime.h"

// AEC 配置结构
typedef struct {
    bool aec_enabled;                    // AEC 功能开关
    float aec_suppression_level;         // AEC 抑制水平
    uint32_t aec_filter_length;          // AEC 滤波器长度
    
    // 音频状态
    volatile uint32_t last_mic_energy;   // 麦克风能量
    volatile uint32_t last_ref_energy;   // 参考信号能量
    volatile bool ref_active_recent;      // 最近是否有参考信号活动
    
    // VAD 配置
    float vad_threshold;                 // VAD 阈值
    uint32_t vad_window_ms;             // VAD 窗口大小(ms)
    bool vad_enabled;                    // VAD 功能开关
    
    // 打断控制
    bool interrupt_enabled;              // 打断功能开关
    uint32_t min_interrupt_energy;       // 最小打断能量
    uint32_t interrupt_cooldown_ms;      // 打断冷却时间
    int64_t last_interrupt_time;         // 上次打断时间
} audio_config_t;

// 函数声明
extern void process_aec(int16_t *mic_buffer, int16_t *ref_buffer, size_t samples);
extern bool detect_voice_activity(int16_t *buffer, size_t samples);
extern bool can_interrupt(void);

// Python 接口
extern const mp_obj_type_t espsr_aec_type;

#endif // _MODESPSR_AEC_H_