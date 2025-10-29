/*
 * MicroPython ESP-SR binding (完全参照project-i2s-wakup-new)
 * 支持唤醒词（嗨，乐鑫）和命令词识别，AFE+WakeNet+MultiNet全流程
 */
#include <stdio.h>
#include <string.h>
#include <stdlib.h>
#include <inttypes.h>
#include "freertos/FreeRTOS.h"
#include "freertos/task.h"
#include "freertos/queue.h"
#include "esp_log.h"
#include "esp_timer.h"
#include "driver/gpio.h"
#include "driver/i2s_std.h"
#include "esp_mn_models.h"
#include "model_path.h"
#include "esp_mn_iface.h"
#include "esp_wn_iface.h"
#include "esp_wn_models.h"
#include "esp_afe_sr_models.h"
#include "esp_afe_sr_iface.h"
#include "esp_mn_speech_commands.h"
#include "py/nlr.h"
#include "py/obj.h"
#include "py/runtime.h"
#include "py/mphal.h"
#include "driver/i2s_pdm.h"

#define TAG "espsr"
#define PULSE_GPIO_NUM 4
#define PULSE_DURATION_MS 500

// 自定义命令词表 (将"hai xiao le"作为首个唤醒命令)
static const char *cmd_phoneme[21] = {
    "hai xiao le",                 // 0: 嗨小乐 (作为唤醒词使用)
    "da kai kong qi jing hua qi",  // 1: 打开空气净化器
    "guan bi kong qi jing hua qi", // 2: 关闭空气净化器
    "da kai tai deng",             // 3: 打开台灯
    "guan bi tai deng",            // 4: 关闭台灯
    "tai deng tiao liang",         // 5: 台灯调亮
    "tai deng tiao an",            // 6: 台灯调暗
    "da kai deng dai",             // 7: 打开等待
    "guan bi deng dai",            // 8: 关闭等待
    "bo fang yin yue",             // 9: 播放音乐
    "ting zhi bo fang",            // 10: 停止播放
    "da kai shi jian",             // 11: 打开时间
    "da kai ri li",                // 12: 打开日历
    "xiao le xiao le",             // 13: 小乐小乐
    "hai ta ta",                    // 14: 嗨塔塔  
    "hai luo bo te",                // 15: 嗨罗伯特
    "hai xiao tian",                // 16: 嗨小天
    "hai bu ke",                    // 17: 嗨布克
    "hai bu te",                    // 18: 嗨布特
    "hai apple",                    // 19: 嗨苹果
    "hai jie ke"                    // 20: 嗨杰科
};

// 结果结构体 (参照参考工程)
typedef struct {
    wakenet_state_t     wakenet_mode;
    esp_mn_state_t      state;
    int                 command_id;
} sr_result_t;

// 全局变量 (参照参考工程)
static QueueHandle_t g_result_que = NULL;
static esp_afe_sr_iface_t *afe_handle = NULL;
static volatile int task_flag = 0;
static model_iface_data_t *model_data = NULL;
static const esp_mn_iface_t *multinet = NULL;
static esp_afe_sr_data_t *afe_data = NULL;
static bool espsr_initialized = false;

// AEC参考信号缓冲区 (用于播放打断)
static int16_t *g_reference_buffer = NULL;
static size_t g_reference_buffer_size = 0;
static size_t g_reference_write_index = 0;
static size_t g_reference_read_index = 0;
static SemaphoreHandle_t g_reference_mutex = NULL;
static int64_t g_last_reference_time_us = 0;  // 最后一次写入参考信号的时间（微秒）

#define REFERENCE_DELAY_MS 30                 // 参考延迟(ms)
#define REFERENCE_BUFFER_SIZE (16000 * 3)     // 3秒缓冲 (16kHz采样率)
#define REFERENCE_TIMEOUT_MS 100              // 参考信号超时时间（毫秒）
static int g_ref_delay_samples = (REFERENCE_DELAY_MS * 16000) / 1000;

// 🔥 诊断统计（用于排查AEC问题）
static uint32_t g_feed_count = 0;           // feed 调用总次数
static uint32_t g_ref_active_feeds = 0;     // 参考信号活跃的 feed 次数
static uint32_t g_ref_nonzero_samples = 0;  // 参考信号非零采样点数
static uint32_t g_ref_total_samples = 0;    // 参考信号总采样点数
static uint32_t g_ref_feed_calls = 0;       // espsr.feed_reference() 被调用次数
static bool g_ref_phase_initialized = false; // 参考读相位是否已建立
// 参考增益（移位），用于匹配扬声器幅度：0=不增益，1=×2，2=×4 ...
static int g_ref_gain_shift = 1;
// 播放/参考状态与能量（用于抑制播放期VAD自打断）
static volatile uint32_t g_last_mic_energy = 0;
static volatile uint32_t g_last_ref_energy = 0;
static volatile bool g_ref_active_recent = false;
static int g_vad_debounce_needed = 6; // 连续帧数(30ms*6≈180ms)后才认为语音成立
static int g_energy_threshold_ratio = 8; // 播放期能量阈值比例（默认8倍）

// 参考管理器（Deepseek方案）
typedef struct {
    int16_t *buffer;
    size_t size;
    size_t write_index;
    size_t read_index;
    int64_t last_write_time;
    int delay_samples;
    int gain_shift;
    bool phase_initialized;
} reference_manager_t;

static reference_manager_t g_ref_manager = {0};

static void write_reference_data(const int16_t *data, size_t samples) {
    if (g_ref_manager.buffer == NULL) return;
    int64_t current_time = esp_timer_get_time();
    for (size_t i = 0; i < samples; i++) {
        int32_t v = (int32_t)data[i];
        if (g_ref_manager.gain_shift > 0) v <<= g_ref_manager.gain_shift;
        if (v > 32767) v = 32767;
        if (v < -32768) v = -32768;
        g_ref_manager.buffer[g_ref_manager.write_index] = (int16_t)v;
        g_ref_manager.write_index = (g_ref_manager.write_index + 1) % g_ref_manager.size;
    }
    g_ref_manager.last_write_time = current_time;
    if (!g_ref_manager.phase_initialized) {
        g_ref_manager.read_index = (g_ref_manager.write_index + g_ref_manager.size - (size_t)g_ref_manager.delay_samples) % g_ref_manager.size;
        g_ref_manager.phase_initialized = true;
    }
}

static int16_t read_reference_sample(void) {
    if (g_ref_manager.buffer == NULL || !g_ref_manager.phase_initialized) return 0;
    int64_t current_time = esp_timer_get_time();
    int64_t time_diff_ms = (current_time - g_ref_manager.last_write_time) / 1000;
    if (time_diff_ms > REFERENCE_TIMEOUT_MS) {
        g_ref_manager.phase_initialized = false;
        return 0;
    }
    int16_t s = g_ref_manager.buffer[g_ref_manager.read_index];
    g_ref_manager.read_index = (g_ref_manager.read_index + 1) % g_ref_manager.size;
    return s;
}
// 录音数据缓冲区（供Python层读取，避免I2S冲突）
static int16_t *g_record_buffer = NULL;
static size_t g_record_buffer_size = 0;
static size_t g_record_write_index = 0;
static size_t g_record_read_index = 0;
static SemaphoreHandle_t g_record_mutex = NULL;
static bool g_recording_enabled = false;  // 录音使能标志
#define RECORD_BUFFER_SIZE (16000 * 10)  // 10秒缓冲 (16kHz采样率)

// 🔥 v2.9: 播放数据缓冲区（C端独立播放线程）
static uint8_t *g_playback_buffer = NULL;       // 播放缓冲区（字节流）
static size_t g_playback_buffer_size = 0;
static size_t g_playback_write_index = 0;
static size_t g_playback_read_index = 0;
static size_t g_playback_data_size = 0;         // 缓冲区中有效字节数（避免满/空歧义）
static SemaphoreHandle_t g_playback_mutex = NULL;
static TaskHandle_t g_playback_task_handle = NULL;
static volatile bool g_playback_running = false;   // 播放线程运行标志
static volatile bool g_playback_stop_requested = false;  // 停止请求标志
static i2s_chan_handle_t g_i2s_tx_handle = NULL;   // I2S TX句柄
#define PLAYBACK_BUFFER_SIZE (128 * 1024)  // 128KB环形缓冲区，降低拥塞

// VAD (Voice Activity Detection) 状态
static volatile bool g_vad_speaking = false;  // 当前是否检测到语音
static SemaphoreHandle_t g_vad_mutex = NULL;

// 脉冲输出初始化和控制
static void init_pulse_gpio(void) {
    gpio_config_t io_conf = {
        .pin_bit_mask = (1ULL << PULSE_GPIO_NUM),
        .mode = GPIO_MODE_OUTPUT,
        .pull_up_en = GPIO_PULLUP_DISABLE,
        .pull_down_en = GPIO_PULLDOWN_DISABLE,
        .intr_type = GPIO_INTR_DISABLE,
    };
    gpio_config(&io_conf);
    gpio_set_level(PULSE_GPIO_NUM, 0);
}

static void send_pulse(void) {
    gpio_set_level(PULSE_GPIO_NUM, 1);
    vTaskDelay(pdMS_TO_TICKS(PULSE_DURATION_MS));
    gpio_set_level(PULSE_GPIO_NUM, 0);
}

// 全局I2S句柄
static i2s_chan_handle_t rx_handle = NULL;

// I2S初始化 (使用新版I2S API避免冲突)
static void init_i2s(void) {
    // I2S0通道配置（仅RX，用于麦克风）
    i2s_chan_config_t chan_cfg = {
        .id = I2S_NUM_0,
        .role = I2S_ROLE_MASTER,
        .dma_desc_num = 8,
        .dma_frame_num = 1024,
        .auto_clear = true,
    };
    
    // 创建I2S0 RX通道
    ESP_ERROR_CHECK(i2s_new_channel(&chan_cfg, NULL, &rx_handle));
    
    // I2S标准配置
    i2s_std_config_t std_cfg; // 未使用，避免未使用警告（保留占位以便需要时恢复STD RX）
    // Configure PDM RX mode
    i2s_pdm_rx_config_t pdm_rx_cfg = {
        .clk_cfg = I2S_PDM_RX_CLK_DEFAULT_CONFIG(16000),
        .slot_cfg = I2S_PDM_RX_SLOT_DEFAULT_CONFIG(I2S_DATA_BIT_WIDTH_16BIT, I2S_SLOT_MODE_MONO),
        .gpio_cfg = {
            .clk = 4,
            .din = 5,
            .invert_flags = {
                .clk_inv = false,
            },
        },
    };
    
    // 初始化I2S0 RX为PDM模式（麦克风）
    ESP_ERROR_CHECK(i2s_channel_init_pdm_rx_mode(rx_handle, &pdm_rx_cfg));
    ESP_ERROR_CHECK(i2s_channel_enable(rx_handle));
    ESP_LOGI(TAG, "I2S0 RX (PDM mic) initialized");
    
    // 🔥 v2.9: 初始化I2S1 TX（播放）
    i2s_chan_config_t tx_chan_cfg = {
        .id = I2S_NUM_1,
        .role = I2S_ROLE_MASTER,
        .dma_desc_num = 8,
        .dma_frame_num = 2048,
        .auto_clear = true,
    };
    ESP_ERROR_CHECK(i2s_new_channel(&tx_chan_cfg, &g_i2s_tx_handle, NULL));
    
    i2s_std_config_t tx_std_cfg = {
        .clk_cfg = I2S_STD_CLK_DEFAULT_CONFIG(16000),
        // 对齐参考项目：MSB + 32-bit 槽，单声道
        .slot_cfg = I2S_STD_MSB_SLOT_DEFAULT_CONFIG(I2S_DATA_BIT_WIDTH_32BIT, I2S_SLOT_MODE_MONO),
        .gpio_cfg = {
            .mclk = I2S_GPIO_UNUSED,
            .bclk = GPIO_NUM_15,
            .ws = GPIO_NUM_16,
            .dout = GPIO_NUM_7,
            .din = I2S_GPIO_UNUSED,
            .invert_flags = {
                .mclk_inv = false,
                .bclk_inv = false,
                .ws_inv = false,
            },
        },
    };
    // 单声道右声道输出
    tx_std_cfg.slot_cfg.slot_mask = I2S_STD_SLOT_RIGHT;
    ESP_ERROR_CHECK(i2s_channel_init_std_mode(g_i2s_tx_handle, &tx_std_cfg));
    ESP_ERROR_CHECK(i2s_channel_enable(g_i2s_tx_handle));
    ESP_LOGI(TAG, "I2S1 TX (playback) initialized");
    
    ESP_LOGI(TAG, "I2S initialized successfully (I2S0: PDM RX, I2S1: STD TX)");
}

// feed任务：构建双通道数据(麦克风+参考信号)并喂给AFE (支持AEC)
void feed_Task(void *arg) {
    esp_afe_sr_data_t *afe_data = arg;
    int feed_chunksize = afe_handle->get_feed_chunksize(afe_data);
    int feed_nch = afe_handle->get_feed_channel_num(afe_data);
    int16_t *feed_buff = (int16_t *) malloc(feed_chunksize * feed_nch * sizeof(int16_t));
    
    assert(feed_buff);
    ESP_LOGI(TAG, "Feed task started: chunksize=%d, channels=%d", feed_chunksize, feed_nch);
    
    while (task_flag) {
        size_t bytesIn = 0;
        
        // 分配临时缓冲区存储PDM麦克风数据
        int16_t *mic_data = (int16_t *) malloc(feed_chunksize * sizeof(int16_t));
        if (mic_data == NULL) {
            ESP_LOGE(TAG, "Failed to allocate mic_data buffer");
            vTaskDelay(pdMS_TO_TICKS(10));
            continue;
        }
        
        // 从PDM麦克风读取数据
        esp_err_t result = i2s_channel_read(rx_handle, mic_data, 
            feed_chunksize * sizeof(int16_t), &bytesIn, portMAX_DELAY);
        
        if (result == ESP_OK && bytesIn > 0) {
            g_feed_count++;  // 统计 feed 次数
            
            // 构建双通道数据：交错排列麦克风和参考信号
            if (g_reference_mutex != NULL && 
                xSemaphoreTake(g_reference_mutex, pdMS_TO_TICKS(10)) == pdTRUE) {
                
                // 🔥 检查参考信号超时（参考项目的实现）
                int64_t current_time_us = esp_timer_get_time();
                int64_t time_diff_ms = (current_time_us - g_last_reference_time_us) / 1000;
                bool ref_active = (time_diff_ms <= REFERENCE_TIMEOUT_MS);
                
                if (time_diff_ms > REFERENCE_TIMEOUT_MS) {
                    // 超过 100ms 没有新的参考信号，清空缓冲区
                    if (g_reference_buffer != NULL) {
                        memset(g_reference_buffer, 0, g_reference_buffer_size * sizeof(int16_t));
                        g_reference_write_index = 0;
                        g_reference_read_index = 0;
                    }
                    // 清相位标记，等待下一次写入时重建相位
                    g_ref_phase_initialized = false;
                }
                
                // 🔥 统计参考信号活跃度
                if (ref_active) {
                    g_ref_active_feeds++;
                }
                
                for (int i = 0; i < feed_chunksize; i++) {
                    feed_buff[i * 2] = mic_data[i];  // 通道0：麦克风数据
                    // 通道1：参考信号（通过管理器读取，含延迟/超时相位控制）
                    int16_t ref_sample = read_reference_sample();
                    feed_buff[i * 2 + 1] = ref_sample;
                    // 统计非零
                    g_ref_total_samples++;
                    if (ref_sample != 0) g_ref_nonzero_samples++;
                }

                // 统计本帧能量并更新参考活跃标记
                uint32_t mic_energy = 0;
                uint32_t ref_energy = 0;
                for (int i = 0; i < feed_chunksize; i++) {
                    int16_t m = feed_buff[i * 2];
                    int16_t r = feed_buff[i * 2 + 1];
                    mic_energy += (uint32_t)(m >= 0 ? m : -m);
                    ref_energy += (uint32_t)(r >= 0 ? r : -r);
                }
                g_last_mic_energy = mic_energy;
                g_last_ref_energy = ref_energy;
                g_ref_active_recent = ref_active;
                
                // 🔥 每3秒打印一次诊断信息（16kHz采样率，480采样点/次，约33次/秒，100次约3秒）
                if (g_feed_count % 100 == 0) {
                    float ref_activity = g_ref_total_samples > 0 ? 
                        (100.0f * g_ref_nonzero_samples / g_ref_total_samples) : 0.0f;
                    // 🔥 修复：转换为 int 避免 %lld 格式化问题
                    int timeout_ms = (int)time_diff_ms;
                    printf("[feed_Task] 🔍 Feed#%" PRIu32 ": ref_active=%d, timeout=%d ms, activity=%.1f%% (%" PRIu32 "/%" PRIu32 "), active_feeds=%" PRIu32 "/%" PRIu32 "\n",
                        g_feed_count, ref_active, timeout_ms, ref_activity, 
                        g_ref_nonzero_samples, g_ref_total_samples,
                        g_ref_active_feeds, g_feed_count);
                }
                
                xSemaphoreGive(g_reference_mutex);
            } else {
                // 如果无法获取锁，只使用麦克风数据
                for (int i = 0; i < feed_chunksize; i++) {
                    feed_buff[i * 2] = mic_data[i];
                    feed_buff[i * 2 + 1] = 0;
                }
            }
            
            // 如果录音已启用，将麦克风数据写入录音缓冲区
            if (g_recording_enabled && g_record_mutex != NULL && 
                xSemaphoreTake(g_record_mutex, pdMS_TO_TICKS(5)) == pdTRUE) {
                
                if (g_record_buffer != NULL && g_record_buffer_size > 0) {
                    for (int i = 0; i < feed_chunksize; i++) {
                        g_record_buffer[g_record_write_index] = mic_data[i];
                        g_record_write_index = (g_record_write_index + 1) % g_record_buffer_size;
                        
                        // 如果写指针追上读指针，说明缓冲区满了，覆盖最旧的数据
                        if (g_record_write_index == g_record_read_index) {
                            g_record_read_index = (g_record_read_index + 1) % g_record_buffer_size;
                        }
                    }
                }
                xSemaphoreGive(g_record_mutex);
            }
            
            // 喂给AFE进行AEC处理
        afe_handle->feed(afe_data, feed_buff);
        }
        
        free(mic_data);
    }
    
    if (feed_buff) {
        free(feed_buff);
        feed_buff = NULL;
    }
    vTaskDelete(NULL);
}

// detect任务：直接使用MultiNet检测命令词，跳过WakeNet
void detect_Task(void *arg) {
    esp_afe_sr_data_t *afe_data = arg;
    int afe_chunksize = afe_handle->get_fetch_chunksize(afe_data);
    int16_t *buff = malloc(afe_chunksize * sizeof(int16_t));
    assert(buff);
    printf("------------detect start (MultiNet only)------------\n");

    while (task_flag) {
        afe_fetch_result_t* res = afe_handle->fetch(afe_data);

        if (!res || res->ret_value == ESP_FAIL) {
            printf("fetch error!\n");
            break;
        }

        // 🔥 更新 VAD 状态（语音活动检测）
        if (g_vad_mutex != NULL && xSemaphoreTake(g_vad_mutex, pdMS_TO_TICKS(5)) == pdTRUE) {
            static int vad_true_streak = 0; // 去抖计数
            bool new_speaking = (res->vad_state == VAD_SPEECH);

            // 播放期能量抑制：参考能量显著高于麦克且参考活跃，压制为静音
            if (new_speaking && g_ref_active_recent) {
                if (g_last_ref_energy > (uint32_t)(g_last_mic_energy * (uint32_t)g_energy_threshold_ratio)) { // 默认8x，可调
                    new_speaking = false;
                }
            }

            // 去抖：需要连续 N 帧为真才拉起
            if (new_speaking) {
                vad_true_streak++;
                if (vad_true_streak < g_vad_debounce_needed) {
                    new_speaking = false;
                }
            } else {
                vad_true_streak = 0;
            }
            if (new_speaking != g_vad_speaking) {
                g_vad_speaking = new_speaking;
                // ESP_LOGI(TAG, "VAD state changed: %s", new_speaking ? "SPEECH" : "SILENCE");
            }
            xSemaphoreGive(g_vad_mutex);
        }

        // 跳过WakeNet检测，直接进行MultiNet命令词检测
        esp_mn_state_t mn_state = multinet->detect(model_data, res->data);

        if (ESP_MN_STATE_DETECTING == mn_state) {
            continue;
        }

        if (ESP_MN_STATE_TIMEOUT == mn_state) {  // 超时，继续监听
            // 不发送超时结果，保持连续监听
            continue;
        }

        if (ESP_MN_STATE_DETECTED == mn_state) {  // 检测到命令词
            esp_mn_results_t *mn_result = multinet->get_results(model_data);
            for (int i = 0; i < mn_result->num; i++) {
                ESP_LOGI(TAG, "TOP %d, command_id: %d, phrase_id: %d, prob: %f",
                        i + 1, mn_result->command_id[i], mn_result->phrase_id[i], mn_result->prob[i]);
            }

            int sr_command_id = mn_result->command_id[0];
            ESP_LOGI(TAG, "Detected command : %d", sr_command_id);
            
            // 判断是否为唤醒命令 (ID 0: "hai xiao le")
            sr_result_t result;
            if (sr_command_id == 0) {
                // "嗨小乐" 作为唤醒词
                result.wakenet_mode = WAKENET_DETECTED;
                result.state = ESP_MN_STATE_DETECTED;
                result.command_id = sr_command_id;
                printf("-----------WAKEUP: hai xiao le-----------\n");
            } else {
                // 其他命令词
                result.wakenet_mode = WAKENET_NO_DETECT;
                result.state = ESP_MN_STATE_DETECTED;
                result.command_id = sr_command_id;
            }
            
            xQueueSend(g_result_que, &result, 10);
            send_pulse();  // 检测到命令时发送脉冲
        }
    }
    if (buff) {
        free(buff);
        buff = NULL;
    }
    vTaskDelete(NULL);
}

// 🔥 v2.9: 播放任务（C端独立管理播放和AEC喂入）
void playback_Task(void *arg) {
    ESP_LOGI(TAG, "🎵 播放线程已启动");
    printf("[playback] Task started, waiting for data...\n");
    
    const size_t chunk_size = 960;  // 30ms @ 16kHz, 16bit
    uint8_t *chunk_buffer = (uint8_t *)malloc(chunk_size);
    if (!chunk_buffer) {
        ESP_LOGE(TAG, "❌ 播放线程：内存分配失败");
        g_playback_running = false;
        vTaskDelete(NULL);
        return;
    }
    
    size_t bytes_written = 0;
    uint32_t chunks_played = 0;
    uint32_t wait_count = 0;
    uint32_t idle_ms = 0;
    
    while (!g_playback_stop_requested) {
        // 1. 从播放缓冲区读取数据
        size_t available = 0;
        if (xSemaphoreTake(g_playback_mutex, pdMS_TO_TICKS(10)) == pdTRUE) {
            available = g_playback_data_size; // 直接使用有效字节数
            xSemaphoreGive(g_playback_mutex);
        }
        
        // 🔥 诊断：打印可用数据量
        if (chunks_played == 0 && available > 0) {
            printf("[playback] First data available: %u bytes\n", (unsigned)available);
        }
        
        // 如果数据不足，等待
        if (available < chunk_size) {
            wait_count++;
            idle_ms += 5;
            if (wait_count % 20 == 1) {
                printf("[playback] Waiting for data... available=%u, need=%u\n", (unsigned)available, (unsigned)chunk_size);
            }
            // 自动空闲超时退出（无数据>1500ms）
            if (idle_ms > 8000) { // 进一步放宽至8s，容忍首包/弱网
                printf("[playback] Idle timeout, no more data. Exiting playback.\n");
                break;
            }
            vTaskDelay(pdMS_TO_TICKS(5));
            continue;
        }
        
        wait_count = 0;  // 重置等待计数
        idle_ms = 0;
        
        // 2. 读取一个chunk
        if (xSemaphoreTake(g_playback_mutex, pdMS_TO_TICKS(10)) == pdTRUE) {
            for (size_t i = 0; i < chunk_size; i++) {
                chunk_buffer[i] = g_playback_buffer[g_playback_read_index];
                g_playback_read_index = (g_playback_read_index + 1) % g_playback_buffer_size;
            }
            if (g_playback_data_size >= chunk_size) {
                g_playback_data_size -= chunk_size;
            } else {
                g_playback_data_size = 0;
            }
            xSemaphoreGive(g_playback_mutex);
        }
        
        // 3. 喂入参考信号到AEC
        if (g_reference_mutex != NULL && xSemaphoreTake(g_reference_mutex, pdMS_TO_TICKS(5)) == pdTRUE) {
            int16_t *samples = (int16_t *)chunk_buffer;
            size_t sample_count = chunk_size / 2;
            // 使用管理器写参考
            write_reference_data(samples, sample_count);
            g_last_reference_time_us = esp_timer_get_time();
            xSemaphoreGive(g_reference_mutex);
        }
        
        // 4. 播放到I2S（32-bit 槽：将16-bit样本左移16位做MSB对齐）
        if (g_i2s_tx_handle == NULL) {
            printf("[playback] ❌ g_i2s_tx_handle is NULL! Skipping I2S write.\n");
            vTaskDelay(pdMS_TO_TICKS(10));
            continue;  // 跳过一次循环，等待句柄初始化
        }
        
        const size_t sample_count = chunk_size / 2; // 16-bit 样本数
        int16_t *s16 = (int16_t *)chunk_buffer;
        // 480 样本（chunk_size=960字节）
        int32_t tx_buf[480];
        for (size_t i = 0; i < sample_count && i < (sizeof(tx_buf)/sizeof(tx_buf[0])); i++) {
            tx_buf[i] = ((int32_t)s16[i]) << 16; // MSB 对齐
        }

        size_t written = 0;
        esp_err_t ret = i2s_channel_write(g_i2s_tx_handle, (const void *)tx_buf, sample_count * sizeof(int32_t), &written, portMAX_DELAY);
        if (ret == ESP_OK) {
            bytes_written += written;
            chunks_played++;
            
            if (chunks_played == 1) {
                printf("[playback] ✅ First chunk played! I2S TX working!\n");
            }
            
            if (chunks_played % 100 == 0) {
                ESP_LOGI(TAG, "🔊 已播放 %lu 块 (%.1f秒)", chunks_played, (float)bytes_written / 32000.0f);
                printf("[playback] 🔊 Played %lu chunks (%.1f sec)\n", chunks_played, (float)bytes_written / 32000.0f);
            }
        } else {
            printf("[playback] ❌ I2S write failed: ret=%d, written=%u\n", ret, (unsigned)written);
            ESP_LOGE(TAG, "❌ I2S写入失败: %d", ret);
            // 写入失败，退出播放线程
            break;
        }
    }
    
    printf("[playback] Task ending, played %lu chunks (%.1f sec)\n", chunks_played, (float)bytes_written / 32000.0f);
    ESP_LOGI(TAG, "🎵 播放线程结束，共播放 %lu 块 (%.1f秒)", chunks_played, (float)bytes_written / 32000.0f);
    
    free(chunk_buffer);
    g_playback_running = false;
    vTaskDelete(NULL);
}

// MicroPython接口：初始化 (参照参考工程完整流程)
static mp_obj_t espsr_init(void) {
    if (espsr_initialized) {
        return mp_const_true;
    }
    
    ESP_LOGI(TAG, "Initializing ESP-SR with AEC...");
    
    // 初始化GPIO脉冲输出
    init_pulse_gpio();
    
    // 初始化I2S
    init_i2s();
    
    // 初始化参考信号缓冲区 (用于AEC)
    g_reference_buffer = (int16_t *) heap_caps_malloc(
        REFERENCE_BUFFER_SIZE * sizeof(int16_t), MALLOC_CAP_SPIRAM);
    if (g_reference_buffer == NULL) {
        ESP_LOGE(TAG, "Failed to allocate reference buffer");
        return mp_const_false;
    }
    g_reference_buffer_size = REFERENCE_BUFFER_SIZE;
    g_reference_write_index = 0;
    g_reference_read_index = 0;
    g_last_reference_time_us = 0;  // 初始化时间戳
    memset(g_reference_buffer, 0, REFERENCE_BUFFER_SIZE * sizeof(int16_t));
    ESP_LOGI(TAG, "Reference buffer allocated: %d samples", REFERENCE_BUFFER_SIZE);
    
    g_reference_mutex = xSemaphoreCreateMutex();
    // 初始化参考管理器
    g_ref_manager.buffer = (int16_t *) heap_caps_malloc(
        REFERENCE_BUFFER_SIZE * sizeof(int16_t), MALLOC_CAP_SPIRAM);
    if (g_ref_manager.buffer) {
        g_ref_manager.size = REFERENCE_BUFFER_SIZE;
        g_ref_manager.write_index = 0;
        g_ref_manager.read_index = 0;
        g_ref_manager.delay_samples = g_ref_delay_samples;
        g_ref_manager.gain_shift = g_ref_gain_shift;
        g_ref_manager.phase_initialized = false;
        g_ref_manager.last_write_time = 0;
        memset(g_ref_manager.buffer, 0, REFERENCE_BUFFER_SIZE * sizeof(int16_t));
        ESP_LOGI(TAG, "Reference manager initialized: delay=%d samples", g_ref_manager.delay_samples);
    }
    if (g_reference_mutex == NULL) {
        ESP_LOGE(TAG, "Failed to create reference mutex");
        heap_caps_free(g_reference_buffer);
        g_reference_buffer = NULL;
        return mp_const_false;
    }
    
    // 初始化录音数据缓冲区
    g_record_buffer = (int16_t *) heap_caps_malloc(
        RECORD_BUFFER_SIZE * sizeof(int16_t), MALLOC_CAP_SPIRAM);
    if (g_record_buffer == NULL) {
        ESP_LOGE(TAG, "Failed to allocate record buffer");
        heap_caps_free(g_reference_buffer);
        vSemaphoreDelete(g_reference_mutex);
        g_reference_buffer = NULL;
        g_reference_mutex = NULL;
        return mp_const_false;
    }
    g_record_buffer_size = RECORD_BUFFER_SIZE;
    g_record_write_index = 0;
    g_record_read_index = 0;
    g_recording_enabled = false;  // 默认关闭录音
    memset(g_record_buffer, 0, RECORD_BUFFER_SIZE * sizeof(int16_t));
    ESP_LOGI(TAG, "Record buffer allocated: %d samples (%.1f seconds)", 
        RECORD_BUFFER_SIZE, (float)RECORD_BUFFER_SIZE / 16000.0);
    
    g_record_mutex = xSemaphoreCreateMutex();
    if (g_record_mutex == NULL) {
        ESP_LOGE(TAG, "Failed to create record mutex");
        heap_caps_free(g_record_buffer);
        heap_caps_free(g_reference_buffer);
        vSemaphoreDelete(g_reference_mutex);
        g_record_buffer = NULL;
        g_reference_buffer = NULL;
        g_reference_mutex = NULL;
        return mp_const_false;
    }
    
    // 创建 VAD 互斥量
    g_vad_mutex = xSemaphoreCreateMutex();
    if (g_vad_mutex == NULL) {
        ESP_LOGE(TAG, "Failed to create VAD mutex");
        heap_caps_free(g_record_buffer);
        heap_caps_free(g_reference_buffer);
        vSemaphoreDelete(g_reference_mutex);
        vSemaphoreDelete(g_record_mutex);
        g_record_buffer = NULL;
        g_reference_buffer = NULL;
        g_reference_mutex = NULL;
        g_record_mutex = NULL;
        return mp_const_false;
    }
    g_vad_speaking = false;
    
    // 🔥 v2.9: 初始化播放缓冲区
    g_playback_buffer = (uint8_t *) heap_caps_malloc(PLAYBACK_BUFFER_SIZE, MALLOC_CAP_SPIRAM);
    if (g_playback_buffer == NULL) {
        ESP_LOGE(TAG, "Failed to allocate playback buffer");
        heap_caps_free(g_record_buffer);
        heap_caps_free(g_reference_buffer);
        vSemaphoreDelete(g_reference_mutex);
        vSemaphoreDelete(g_record_mutex);
        vSemaphoreDelete(g_vad_mutex);
        return mp_const_false;
    }
    g_playback_buffer_size = PLAYBACK_BUFFER_SIZE;
    g_playback_write_index = 0;
    g_playback_read_index = 0;
    g_playback_data_size = 0;
    g_playback_running = false;
    g_playback_stop_requested = false;
    memset(g_playback_buffer, 0, PLAYBACK_BUFFER_SIZE);
    ESP_LOGI(TAG, "Playback buffer allocated: %d bytes (%.1f seconds)", 
        PLAYBACK_BUFFER_SIZE, (float)PLAYBACK_BUFFER_SIZE / 32000.0);
    
    g_playback_mutex = xSemaphoreCreateMutex();
    if (g_playback_mutex == NULL) {
        ESP_LOGE(TAG, "Failed to create playback mutex");
        heap_caps_free(g_playback_buffer);
        heap_caps_free(g_record_buffer);
        heap_caps_free(g_reference_buffer);
        vSemaphoreDelete(g_reference_mutex);
        vSemaphoreDelete(g_record_mutex);
        vSemaphoreDelete(g_vad_mutex);
        return mp_const_false;
    }
    
    // 初始状态：无语音
    ESP_LOGI(TAG, "VAD mutex created, initial state: SILENCE");
    
    // 初始化语音识别模型 (使用MR格式支持AEC)
    srmodel_list_t *models = esp_srmodel_init("model");
    
    // 🔥 获取降噪模型（关键！）
    char *ns_model_name = esp_srmodel_filter(models, ESP_NSNET_PREFIX, NULL);
    if (ns_model_name) {
        ESP_LOGI(TAG, "NS model found: %s", ns_model_name);
    } else {
        ESP_LOGW(TAG, "NS model not found, noise suppression will be disabled");
    }
    
    // MR格式：M=麦克风，R=参考信号(播放音频)
    // 对齐参考项目：AEC场景使用 VC 类型以获得更强抑制
    afe_config_t *afe_config = afe_config_init("MR", models, AFE_TYPE_SR, AFE_MODE_HIGH_PERF);
    
    // 启用AEC配置
    afe_config->wakenet_model_name = NULL;  // 不加载唤醒词模型
    afe_config->aec_init = true;  // 启用AEC
    afe_config->aec_mode = AEC_MODE_VOIP_HIGH_PERF;  // 🔥 使用VOIP高性能模式（参考项目）
    
    // 🔥 启用降噪（NS）配置（参考项目的关键配置！）
    if (ns_model_name != NULL) {
        afe_config->ns_init = true;  // 启用降噪
        afe_config->ns_model_name = ns_model_name;  // 设置降噪模型
        afe_config->afe_ns_mode = AFE_NS_MODE_NET;  // 使用神经网络降噪模式
        ESP_LOGI(TAG, "NS enabled with model: %s", ns_model_name);
    } else {
        afe_config->ns_init = false;
        ESP_LOGW(TAG, "NS disabled (model not found)");
    }
    
    // 🔥 启用VAD配置（语音活动检测）
    afe_config->vad_init = true;  // 启用VAD
    afe_config->vad_mode = VAD_MODE_0;  // VAD模式0（灵敏度最高）
    afe_config->vad_min_noise_ms = 100;  // 最小噪音时长100ms
    
    // 🔥 其他关键配置（参考项目）
    afe_config->afe_perferred_core = 1;  // 指定CPU核心
    afe_config->afe_perferred_priority = 1;  // 设置优先级
    afe_config->agc_init = false;  // 禁用AGC（自动增益控制）
    afe_config->memory_alloc_mode = AFE_MEMORY_ALLOC_MORE_PSRAM;  // 使用PSRAM分配内存
    
    ESP_LOGI(TAG, "AFE config: format=MR, aec_init=true, aec_mode=%d, ns_init=%s, vad_init=true", 
        afe_config->aec_mode, ns_model_name ? "true" : "false");
    
    afe_handle = esp_afe_handle_from_config(afe_config);
    afe_data = afe_handle->create_from_config(afe_config);
    
    // 验证通道数
    int feed_channels = afe_handle->get_feed_channel_num(afe_data);
    ESP_LOGI(TAG, "AFE feed channels: %d (expected: 2 for MR)", feed_channels);
    
    // 初始化MultiNet
    char *mn_name = esp_srmodel_filter(models, ESP_MN_CHINESE, NULL);
    if (NULL == mn_name) {
        printf("No multinet model found");
        return mp_const_false;
    }
    multinet = esp_mn_handle_from_name(mn_name);
    model_data = multinet->create(mn_name, 5760);  // 设置唤醒超时时间
    printf("load multinet:%s\n", mn_name);
    
    // 清除并添加命令词 (完全参照参考工程)
    esp_mn_commands_clear();
    for (int i = 0; i < sizeof(cmd_phoneme) / sizeof(cmd_phoneme[0]); i++) {
        esp_mn_commands_add(i, (char *)cmd_phoneme[i]);
    }
    esp_mn_commands_update();
    esp_mn_commands_print();
    multinet->print_active_speech_commands(model_data);
    
    afe_config_free(afe_config);
    
    // 创建结果队列 (增大到10，避免结果丢失)
    g_result_que = xQueueCreate(10, sizeof(sr_result_t));
    
    // 启动任务
    task_flag = 1;
    xTaskCreatePinnedToCore(&feed_Task, "feed", 8 * 1024, (void*)afe_data, 5, NULL, 0);
    xTaskCreatePinnedToCore(&detect_Task, "detect", 4 * 1024, (void*)afe_data, 5, NULL, 1);
    
    espsr_initialized = true;
    ESP_LOGI(TAG, "ESP-SR initialized successfully");
    
    // 🔥 测试：确认 printf 输出是否工作
    printf("\n[TEST] ========================================\n");
    printf("[TEST] ESP-SR initialized! printf is working!\n");
    printf("[TEST] espsr_initialized = %d\n", espsr_initialized);
    printf("[TEST] g_reference_buffer = %p\n", g_reference_buffer);
    printf("[TEST] g_reference_mutex = %p\n", g_reference_mutex);
    printf("[TEST] ========================================\n\n");
    
    return mp_const_true;
}
static MP_DEFINE_CONST_FUN_OBJ_0(espsr_init_obj, espsr_init);

// MicroPython接口：监听结果
static mp_obj_t espsr_listen(mp_obj_t timeout_obj) {
    if (!espsr_initialized) {
        return mp_obj_new_str("not_initialized", 15);
    }
    
    int timeout_ms = mp_obj_get_int(timeout_obj);
    sr_result_t result;
    
    if (xQueueReceive(g_result_que, &result, pdMS_TO_TICKS(timeout_ms)) == pdTRUE) {
        if (result.wakenet_mode == WAKENET_DETECTED) {
            return mp_obj_new_str("wakeup", 6);
        } else if (result.state == ESP_MN_STATE_DETECTED) {
            // 返回命令ID和命令词
            mp_obj_t command_info = mp_obj_new_dict(2);
            mp_obj_dict_store(command_info, mp_obj_new_str("id", 2), mp_obj_new_int(result.command_id));
            if (result.command_id >= 0 && result.command_id < sizeof(cmd_phoneme)/sizeof(cmd_phoneme[0])) {
                mp_obj_dict_store(command_info, mp_obj_new_str("command", 7), 
                                mp_obj_new_str(cmd_phoneme[result.command_id], strlen(cmd_phoneme[result.command_id])));
            }
            return command_info;
        } else if (result.state == ESP_MN_STATE_TIMEOUT) {
            return mp_obj_new_str("timeout", 7);
        }
    }
    
    return mp_obj_new_str("timeout", 7);
}
static MP_DEFINE_CONST_FUN_OBJ_1(espsr_listen_obj, espsr_listen);

// MicroPython接口：获取命令词列表
static mp_obj_t espsr_get_commands(void) {
    mp_obj_t command_dict = mp_obj_new_dict(sizeof(cmd_phoneme)/sizeof(cmd_phoneme[0]));
    for (int i = 0; i < sizeof(cmd_phoneme)/sizeof(cmd_phoneme[0]); i++) {
        mp_obj_dict_store(command_dict, mp_obj_new_int(i), mp_obj_new_str(cmd_phoneme[i], strlen(cmd_phoneme[i])));
    }
    return command_dict;
}
static MP_DEFINE_CONST_FUN_OBJ_0(espsr_get_commands_obj, espsr_get_commands);

// MicroPython接口：输入参考信号 (播放音频数据用于AEC)
static mp_obj_t espsr_feed_reference(mp_obj_t data_obj) {
    // 🔥 诊断：入口日志（使用 printf 确保输出）
    printf("[feed_ref] 🚀 Called, espsr_init=%d\n", espsr_initialized);
    
    if (!espsr_initialized) {
        printf("[feed_ref] ❌ ESP-SR not initialized!\n");
        return mp_const_false;
    }
    
    mp_buffer_info_t bufinfo;
    mp_get_buffer_raise(data_obj, &bufinfo, MP_BUFFER_READ);
    printf("[feed_ref] ✅ Buffer: %d bytes\n", bufinfo.len);
    
    if (g_reference_buffer == NULL || g_reference_mutex == NULL) {
        printf("[feed_ref] ❌ Ref buffer NULL (buf=%p, mutex=%p)\n", g_reference_buffer, g_reference_mutex);
        ESP_LOGW(TAG, "Reference buffer not initialized");
        return mp_const_false;
    }
    printf("[feed_ref] ✅ Ref buffer OK\n");
    
    // 将播放数据写入参考缓冲区
    int16_t *data = (int16_t *)bufinfo.buf;
    int samples = bufinfo.len / 2;
    
    // 🔥 诊断：检查数据是否全是零
    int nonzero_count = 0;
    int16_t max_val = 0;
    for (int i = 0; i < samples && i < 100; i++) {  // 检查前100个采样点
        if (data[i] != 0) nonzero_count++;
        if (abs(data[i]) > abs(max_val)) max_val = data[i];
    }
    
    if (xSemaphoreTake(g_reference_mutex, pdMS_TO_TICKS(10)) == pdTRUE) {
        for (int i = 0; i < samples; i++) {
            g_reference_buffer[g_reference_write_index] = data[i];
            g_reference_write_index = (g_reference_write_index + 1) % g_reference_buffer_size;
        }
        // 🔥 更新最后写入时间（用于超时检测）
        g_last_reference_time_us = esp_timer_get_time();
        
        // 🔥 诊断：统计 feed_reference 调用次数
        g_ref_feed_calls++;
        if (g_ref_feed_calls % 50 == 1) {  // 每50次打印一次
            printf("[feed_ref] ✅ #%" PRIu32 ": %d bytes, %d samples, nonzero=%d/100, max_val=%d\n", 
                g_ref_feed_calls, bufinfo.len, samples, nonzero_count, max_val);
        }
        
        xSemaphoreGive(g_reference_mutex);
        return mp_const_true;
    }
    
    printf("[feed_ref] ❌ Failed to acquire mutex\n");
    ESP_LOGW(TAG, "Failed to acquire reference mutex");
    return mp_const_false;
}
static MP_DEFINE_CONST_FUN_OBJ_1(espsr_feed_reference_obj, espsr_feed_reference);

// MicroPython接口：启用录音模式
static mp_obj_t espsr_start_recording(void) {
    if (!espsr_initialized) {
        ESP_LOGW(TAG, "ESP-SR not initialized");
        return mp_const_false;
    }
    
    if (g_record_buffer == NULL || g_record_mutex == NULL) {
        ESP_LOGW(TAG, "Record buffer not initialized");
        return mp_const_false;
    }
    
    // 清空录音缓冲区
    if (xSemaphoreTake(g_record_mutex, pdMS_TO_TICKS(10)) == pdTRUE) {
        g_record_write_index = 0;
        g_record_read_index = 0;
        memset(g_record_buffer, 0, g_record_buffer_size * sizeof(int16_t));
        g_recording_enabled = true;
        xSemaphoreGive(g_record_mutex);
        ESP_LOGI(TAG, "Recording started");
        return mp_const_true;
    }
    
    ESP_LOGW(TAG, "Failed to acquire record mutex");
    return mp_const_false;
}
static MP_DEFINE_CONST_FUN_OBJ_0(espsr_start_recording_obj, espsr_start_recording);

// MicroPython接口：停止录音模式
static mp_obj_t espsr_stop_recording(void) {
    if (!espsr_initialized) {
        return mp_const_false;
    }
    
    g_recording_enabled = false;
    ESP_LOGI(TAG, "Recording stopped");
    return mp_const_true;
}
static MP_DEFINE_CONST_FUN_OBJ_0(espsr_stop_recording_obj, espsr_stop_recording);

// MicroPython接口：读取录音数据
// 用法: bytes_read = espsr.read_audio(buffer)
// 返回实际读取的字节数
static mp_obj_t espsr_read_audio(mp_obj_t buffer_obj) {
    if (!espsr_initialized || !g_recording_enabled) {
        return mp_obj_new_int(0);
    }
    
    if (g_record_buffer == NULL || g_record_mutex == NULL) {
        return mp_obj_new_int(0);
    }
    
    mp_buffer_info_t bufinfo;
    mp_get_buffer_raise(buffer_obj, &bufinfo, MP_BUFFER_WRITE);
    
    int16_t *dest = (int16_t *)bufinfo.buf;
    int max_samples = bufinfo.len / 2;  // 16位样本
    int bytes_read = 0;
    
    if (xSemaphoreTake(g_record_mutex, pdMS_TO_TICKS(10)) == pdTRUE) {
        // 计算可读取的样本数
        int available_samples;
        if (g_record_write_index >= g_record_read_index) {
            available_samples = g_record_write_index - g_record_read_index;
        } else {
            available_samples = g_record_buffer_size - g_record_read_index + g_record_write_index;
        }
        
        // 读取数据
        int samples_to_read = (available_samples < max_samples) ? available_samples : max_samples;
        for (int i = 0; i < samples_to_read; i++) {
            dest[i] = g_record_buffer[g_record_read_index];
            g_record_read_index = (g_record_read_index + 1) % g_record_buffer_size;
        }
        
        bytes_read = samples_to_read * 2;  // 转换为字节数
        xSemaphoreGive(g_record_mutex);
    }
    
    return mp_obj_new_int(bytes_read);
}
static MP_DEFINE_CONST_FUN_OBJ_1(espsr_read_audio_obj, espsr_read_audio);

// 动态调整AEC参数（延迟、增益、能量比例）
static mp_obj_t espsr_set_aec_params(mp_obj_t delay_ms_obj, mp_obj_t gain_shift_obj, mp_obj_t energy_ratio_obj) {
    if (!espsr_initialized) {
        return mp_const_false;
    }
    int new_delay_ms = mp_obj_get_int(delay_ms_obj);
    int new_gain = mp_obj_get_int(gain_shift_obj);
    int new_ratio = mp_obj_get_int(energy_ratio_obj);

    if (g_reference_mutex != NULL && xSemaphoreTake(g_reference_mutex, pdMS_TO_TICKS(10)) == pdTRUE) {
        g_ref_delay_samples = (new_delay_ms * 16000) / 1000;
        g_ref_gain_shift = new_gain;
        g_energy_threshold_ratio = new_ratio;
        // 同步管理器参数
        if (g_ref_manager.buffer) {
            g_ref_manager.delay_samples = g_ref_delay_samples;
            g_ref_manager.gain_shift = g_ref_gain_shift;
            g_ref_manager.phase_initialized = false; // 触发重建相位
        }
        xSemaphoreGive(g_reference_mutex);
        ESP_LOGI(TAG, "AEC params updated: delay=%dms(%d samples), gain_shift=%d, energy_ratio=%d",
                 new_delay_ms, g_ref_delay_samples, g_ref_gain_shift, g_energy_threshold_ratio);
        return mp_const_true;
    }
    return mp_const_false;
}
static MP_DEFINE_CONST_FUN_OBJ_3(espsr_set_aec_params_obj, espsr_set_aec_params);

// MicroPython接口：清理资源
// MicroPython接口：检测 VAD 状态（语音活动检测）
static mp_obj_t espsr_check_vad(void) {
    if (!espsr_initialized) {
        return mp_const_false;
    }
    
    bool is_speaking = false;
    if (g_vad_mutex != NULL && xSemaphoreTake(g_vad_mutex, pdMS_TO_TICKS(10)) == pdTRUE) {
        is_speaking = g_vad_speaking;
        xSemaphoreGive(g_vad_mutex);
    }
    
    return is_speaking ? mp_const_true : mp_const_false;
}
static MP_DEFINE_CONST_FUN_OBJ_0(espsr_check_vad_obj, espsr_check_vad);

// 🔥 v2.9: MicroPython接口 - 启动播放线程
static mp_obj_t espsr_start_playback(void) {
    if (!espsr_initialized) {
        mp_raise_msg(&mp_type_RuntimeError, "ESP-SR not initialized");
    }
    
    if (g_playback_running) {
        ESP_LOGW(TAG, "Playback already running");
        return mp_const_false;
    }
    
    // 清空播放缓冲区
    if (g_playback_mutex != NULL && xSemaphoreTake(g_playback_mutex, pdMS_TO_TICKS(100)) == pdTRUE) {
        g_playback_write_index = 0;
        g_playback_read_index = 0;
        memset(g_playback_buffer, 0, g_playback_buffer_size);
        xSemaphoreGive(g_playback_mutex);
    }
    
    // 创建播放线程
    g_playback_stop_requested = false;
    g_playback_running = true;
    
    BaseType_t ret = xTaskCreatePinnedToCore(
        playback_Task,
        "playback",
        4096,
        NULL,
        5,
        &g_playback_task_handle,
        0  // CPU0
    );
    
    if (ret != pdPASS) {
        ESP_LOGE(TAG, "Failed to create playback task");
        g_playback_running = false;
        return mp_const_false;
    }
    
    ESP_LOGI(TAG, "✅ 播放线程已启动");
    return mp_const_true;
}
static MP_DEFINE_CONST_FUN_OBJ_0(espsr_start_playback_obj, espsr_start_playback);

// 🔥 v2.9: MicroPython接口 - 喂入播放数据
static mp_obj_t espsr_feed_playback(mp_obj_t data_obj) {
    static uint32_t feed_count = 0;
    
    if (!espsr_initialized) {
        mp_raise_msg(&mp_type_RuntimeError, "ESP-SR not initialized");
    }
    
    if (!g_playback_running) {
        mp_raise_msg(&mp_type_RuntimeError, "Playback not running");
    }
    
    mp_buffer_info_t bufinfo;
    mp_get_buffer_raise(data_obj, &bufinfo, MP_BUFFER_READ);
    
    if (bufinfo.len == 0) {
        return mp_obj_new_int(0);
    }
    
    size_t written = 0;
    uint8_t *data = (uint8_t *)bufinfo.buf;
    
    if (g_playback_mutex != NULL && xSemaphoreTake(g_playback_mutex, pdMS_TO_TICKS(100)) == pdTRUE) {
        size_t before_write = g_playback_write_index;
        
        for (size_t i = 0; i < bufinfo.len; i++) {
            // 满则停止写入，返回已写字节数（不覆盖未读数据）
            if (g_playback_data_size >= g_playback_buffer_size) {
                ESP_LOGW(TAG, "⚠️ 播放缓冲区满，停止本次写入");
                printf("[feed_playback] Buffer full! write_idx=%u, read_idx=%u\n", 
                       (unsigned)g_playback_write_index, (unsigned)g_playback_read_index);
                break;
            }
            g_playback_buffer[g_playback_write_index] = data[i];
            g_playback_write_index = (g_playback_write_index + 1) % g_playback_buffer_size;
            g_playback_data_size++;
            written++;
        }
        
        feed_count++;
        if (feed_count == 1) {
            printf("[feed_playback] ✅ First feed: %u bytes, write_idx: %u->%u\n", 
                   (unsigned)written, (unsigned)before_write, (unsigned)g_playback_write_index);
        } else if (feed_count % 10 == 0) {
            printf("[feed_playback] Feed #%lu: %u/%u bytes, buffer usage: %u/%u\n",
                   feed_count, (unsigned)written, (unsigned)bufinfo.len, 
                   (unsigned)g_playback_data_size,
                   (unsigned)g_playback_buffer_size);
        }
        
        xSemaphoreGive(g_playback_mutex);
    }
    
    return mp_obj_new_int(written);
}
static MP_DEFINE_CONST_FUN_OBJ_1(espsr_feed_playback_obj, espsr_feed_playback);

// 🔎 播放运行状态查询
static mp_obj_t espsr_is_playback_running(void) {
    return g_playback_running ? mp_const_true : mp_const_false;
}
static MP_DEFINE_CONST_FUN_OBJ_0(espsr_is_playback_running_obj, espsr_is_playback_running);

// 🔥 v2.9: MicroPython接口 - 停止播放线程
static mp_obj_t espsr_stop_playback(void) {
    if (!espsr_initialized) {
        mp_raise_msg(&mp_type_RuntimeError, "ESP-SR not initialized");
    }
    
    if (!g_playback_running) {
        ESP_LOGW(TAG, "Playback not running");
        return mp_const_false;
    }
    
    ESP_LOGI(TAG, "🛑 请求停止播放线程...");
    g_playback_stop_requested = true;
    
    // 等待播放线程退出（最多2秒）
    int timeout = 20;  // 20 * 100ms = 2s
    while (g_playback_running && timeout > 0) {
        vTaskDelay(pdMS_TO_TICKS(100));
        timeout--;
    }
    
    if (g_playback_running) {
        ESP_LOGE(TAG, "❌ 播放线程未能正常退出");
        return mp_const_false;
    }
    
    ESP_LOGI(TAG, "✅ 播放线程已停止");
    return mp_const_true;
}
static MP_DEFINE_CONST_FUN_OBJ_0(espsr_stop_playback_obj, espsr_stop_playback);

static mp_obj_t espsr_cleanup(void) {
    if (!espsr_initialized) {
        return mp_const_none;
    }
    
    // 停止任务
    task_flag = 0;
    vTaskDelay(pdMS_TO_TICKS(100));
    
    // 清理模型
    if (model_data && multinet) {
        multinet->destroy(model_data);
        model_data = NULL;
    }
    
    // 停止并删除I2S通道
    if (rx_handle) {
        i2s_channel_disable(rx_handle);
        i2s_del_channel(rx_handle);
        rx_handle = NULL;
    }
    
    // 清理AFE
    if (afe_handle && afe_data) {
        afe_handle = NULL;
        afe_data = NULL;
    }
    
    // 删除队列
    if (g_result_que) {
        vQueueDelete(g_result_que);
        g_result_que = NULL;
    }
    
    // 清理参考信号缓冲区
    if (g_reference_buffer) {
        heap_caps_free(g_reference_buffer);
        g_reference_buffer = NULL;
        g_reference_buffer_size = 0;
        g_reference_write_index = 0;
        g_reference_read_index = 0;
        ESP_LOGI(TAG, "Reference buffer freed");
    }
    if (g_ref_manager.buffer) {
        heap_caps_free(g_ref_manager.buffer);
        g_ref_manager.buffer = NULL;
        g_ref_manager.size = 0;
        g_ref_manager.phase_initialized = false;
    }
    
    if (g_reference_mutex) {
        vSemaphoreDelete(g_reference_mutex);
        g_reference_mutex = NULL;
    }
    
    // 清理录音数据缓冲区
    if (g_record_buffer) {
        heap_caps_free(g_record_buffer);
        g_record_buffer = NULL;
        g_record_buffer_size = 0;
        g_record_write_index = 0;
        g_record_read_index = 0;
        g_recording_enabled = false;
        ESP_LOGI(TAG, "Record buffer freed");
    }
    
    if (g_record_mutex) {
        vSemaphoreDelete(g_record_mutex);
        g_record_mutex = NULL;
    }
    
    // 清理 VAD 互斥量
    if (g_vad_mutex) {
        vSemaphoreDelete(g_vad_mutex);
        g_vad_mutex = NULL;
    }
    g_vad_speaking = false;
    
    // 🔥 v2.9: 清理播放资源
    if (g_playback_running) {
        g_playback_stop_requested = true;
        vTaskDelay(pdMS_TO_TICKS(200));  // 等待播放线程退出
    }
    
    if (g_i2s_tx_handle) {
        i2s_channel_disable(g_i2s_tx_handle);
        i2s_del_channel(g_i2s_tx_handle);
        g_i2s_tx_handle = NULL;
        ESP_LOGI(TAG, "I2S TX channel freed");
    }
    
    if (g_playback_buffer) {
        heap_caps_free(g_playback_buffer);
        g_playback_buffer = NULL;
        g_playback_buffer_size = 0;
        g_playback_write_index = 0;
        g_playback_read_index = 0;
        ESP_LOGI(TAG, "Playback buffer freed");
    }
    
    if (g_playback_mutex) {
        vSemaphoreDelete(g_playback_mutex);
        g_playback_mutex = NULL;
    }
    
    espsr_initialized = false;
    ESP_LOGI(TAG, "ESP-SR cleaned up (with AEC+VAD+Playback support)");
    return mp_const_none;
}
static MP_DEFINE_CONST_FUN_OBJ_0(espsr_cleanup_obj, espsr_cleanup);

// MicroPython模块注册表
static const mp_rom_map_elem_t espsr_module_globals_table[] = {
    { MP_ROM_QSTR(MP_QSTR___name__), MP_ROM_QSTR(MP_QSTR_espsr) },
    { MP_ROM_QSTR(MP_QSTR_init), MP_ROM_PTR(&espsr_init_obj) },
    { MP_ROM_QSTR(MP_QSTR_listen), MP_ROM_PTR(&espsr_listen_obj) },
    { MP_ROM_QSTR(MP_QSTR_get_commands), MP_ROM_PTR(&espsr_get_commands_obj) },
    { MP_ROM_QSTR(MP_QSTR_cleanup), MP_ROM_PTR(&espsr_cleanup_obj) },
    { MP_ROM_QSTR(MP_QSTR_feed_reference), MP_ROM_PTR(&espsr_feed_reference_obj) },
    { MP_ROM_QSTR(MP_QSTR_start_recording), MP_ROM_PTR(&espsr_start_recording_obj) },
    { MP_ROM_QSTR(MP_QSTR_stop_recording), MP_ROM_PTR(&espsr_stop_recording_obj) },
    { MP_ROM_QSTR(MP_QSTR_read_audio), MP_ROM_PTR(&espsr_read_audio_obj) },
    { MP_ROM_QSTR(MP_QSTR_check_vad), MP_ROM_PTR(&espsr_check_vad_obj) },
    // 🔥 v2.9: C端播放接口
    { MP_ROM_QSTR(MP_QSTR_start_playback), MP_ROM_PTR(&espsr_start_playback_obj) },
    { MP_ROM_QSTR(MP_QSTR_feed_playback), MP_ROM_PTR(&espsr_feed_playback_obj) },
    { MP_ROM_QSTR(MP_QSTR_stop_playback), MP_ROM_PTR(&espsr_stop_playback_obj) },
    { MP_ROM_QSTR(MP_QSTR_is_playback_running), MP_ROM_PTR(&espsr_is_playback_running_obj) },
    // 调参接口
    { MP_ROM_QSTR(MP_QSTR_set_aec_params), MP_ROM_PTR(&espsr_set_aec_params_obj) },
};

static MP_DEFINE_CONST_DICT(espsr_module_globals, espsr_module_globals_table);

const mp_obj_module_t mp_module_espsr = {
    .base = { &mp_type_module },
    .globals = (mp_obj_dict_t *)&espsr_module_globals,
};

MP_REGISTER_MODULE(MP_QSTR_espsr, mp_module_espsr);
