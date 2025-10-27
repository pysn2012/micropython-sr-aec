/*
 * MicroPython ESP-SR binding (完全参照project-i2s-wakup-new)
 * 支持唤醒词（嗨，乐鑫）和命令词识别，AFE+WakeNet+MultiNet全流程
 */
#include <stdio.h>
#include <string.h>
#include <stdlib.h>
#include "freertos/FreeRTOS.h"
#include "freertos/task.h"
#include "freertos/queue.h"
#include "esp_log.h"
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

#define REFERENCE_BUFFER_SIZE (16000 * 2)  // 2秒缓冲 (16kHz采样率)

// 录音数据缓冲区（供Python层读取，避免I2S冲突）
static int16_t *g_record_buffer = NULL;
static size_t g_record_buffer_size = 0;
static size_t g_record_write_index = 0;
static size_t g_record_read_index = 0;
static SemaphoreHandle_t g_record_mutex = NULL;
static bool g_recording_enabled = false;  // 录音使能标志
#define RECORD_BUFFER_SIZE (16000 * 10)  // 10秒缓冲 (16kHz采样率)

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
    // I2S通道配置
    i2s_chan_config_t chan_cfg = {
        .id = I2S_NUM_0,
        .role = I2S_ROLE_MASTER,
        .dma_desc_num = 8,
        .dma_frame_num = 1024,
        .auto_clear = true,
    };
    
    // 创建I2S接收通道
    ESP_ERROR_CHECK(i2s_new_channel(&chan_cfg, NULL, &rx_handle));
    
    // I2S标准配置
    i2s_std_config_t std_cfg = {
        .clk_cfg = {
            .sample_rate_hz = 16000,
            .clk_src = I2S_CLK_SRC_DEFAULT,
            .mclk_multiple = I2S_MCLK_MULTIPLE_256,
        },
        .slot_cfg = {
            .data_bit_width = I2S_DATA_BIT_WIDTH_16BIT,
            .slot_bit_width = I2S_SLOT_BIT_WIDTH_16BIT,
            .slot_mode = I2S_SLOT_MODE_MONO,
            .slot_mask = I2S_STD_SLOT_LEFT,
            .ws_width = I2S_DATA_BIT_WIDTH_16BIT,
            .ws_pol = false,
            .bit_shift = true,
        },
        .gpio_cfg = {
            .mclk = I2S_GPIO_UNUSED,
            .bclk = 4,     // SCK
            .ws = 4,       // WS
            .dout = I2S_GPIO_UNUSED,
            .din = 5,      // SD
            .invert_flags = {
                .mclk_inv = false,
                .bclk_inv = false,
                .ws_inv = false,
            },
        },
    };
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
    
    // 初始化I2S标准模式
    // ESP_ERROR_CHECK(i2s_channel_init_std_mode(rx_handle, &std_cfg));
    ESP_ERROR_CHECK(i2s_channel_init_pdm_rx_mode(rx_handle, &pdm_rx_cfg));
    // 启用I2S通道
    ESP_ERROR_CHECK(i2s_channel_enable(rx_handle));
    
    ESP_LOGI(TAG, "I2S initialized successfully (new API)");
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
            // 构建双通道数据：交错排列麦克风和参考信号
            if (g_reference_mutex != NULL && 
                xSemaphoreTake(g_reference_mutex, pdMS_TO_TICKS(10)) == pdTRUE) {
                
                for (int i = 0; i < feed_chunksize; i++) {
                    feed_buff[i * 2] = mic_data[i];  // 通道0：麦克风数据
                    
                    // 通道1：参考信号（播放音频）
                    if (g_reference_buffer != NULL && g_reference_buffer_size > 0) {
                        feed_buff[i * 2 + 1] = g_reference_buffer[g_reference_read_index];
                        g_reference_read_index = (g_reference_read_index + 1) % g_reference_buffer_size;
                    } else {
                        feed_buff[i * 2 + 1] = 0;  // 没有参考信号，填充0
                    }
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
    memset(g_reference_buffer, 0, REFERENCE_BUFFER_SIZE * sizeof(int16_t));
    ESP_LOGI(TAG, "Reference buffer allocated: %d samples", REFERENCE_BUFFER_SIZE);
    
    g_reference_mutex = xSemaphoreCreateMutex();
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
    
    // 初始化语音识别模型 (使用MR格式支持AEC)
    srmodel_list_t *models = esp_srmodel_init("model");
    // MR格式：M=麦克风，R=参考信号(播放音频)
    afe_config_t *afe_config = afe_config_init("MR", models, AFE_TYPE_SR, AFE_MODE_HIGH_PERF);
    
    // 启用AEC配置
    afe_config->wakenet_model_name = NULL;  // 不加载唤醒词模型
    afe_config->aec_init = true;  // 启用AEC
    afe_config->aec_mode = AEC_MODE_SR_HIGH_PERF;  // 使用SR高性能模式
    ESP_LOGI(TAG, "AFE config: format=MR, aec_init=true, aec_mode=%d", afe_config->aec_mode);
    
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
    if (!espsr_initialized) {
        return mp_const_false;
    }
    
    mp_buffer_info_t bufinfo;
    mp_get_buffer_raise(data_obj, &bufinfo, MP_BUFFER_READ);
    
    if (g_reference_buffer == NULL || g_reference_mutex == NULL) {
        ESP_LOGW(TAG, "Reference buffer not initialized");
        return mp_const_false;
    }
    
    // 将播放数据写入参考缓冲区
    int16_t *data = (int16_t *)bufinfo.buf;
    int samples = bufinfo.len / 2;
    
    if (xSemaphoreTake(g_reference_mutex, pdMS_TO_TICKS(10)) == pdTRUE) {
        for (int i = 0; i < samples; i++) {
            g_reference_buffer[g_reference_write_index] = data[i];
            g_reference_write_index = (g_reference_write_index + 1) % g_reference_buffer_size;
        }
        xSemaphoreGive(g_reference_mutex);
        return mp_const_true;
    }
    
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

// MicroPython接口：清理资源
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
    
    espsr_initialized = false;
    ESP_LOGI(TAG, "ESP-SR cleaned up (with AEC support)");
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
};

static MP_DEFINE_CONST_DICT(espsr_module_globals, espsr_module_globals_table);

const mp_obj_module_t mp_module_espsr = {
    .base = { &mp_type_module },
    .globals = (mp_obj_dict_t *)&espsr_module_globals,
};

MP_REGISTER_MODULE(MP_QSTR_espsr, mp_module_espsr);
