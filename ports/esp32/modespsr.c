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

// feed任务：不断采集I2S数据并喂给AFE (完全参照参考工程)
void feed_Task(void *arg) {
    esp_afe_sr_data_t *afe_data = arg;
    int feed_chunksize = afe_handle->get_feed_chunksize(afe_data);
    int feed_nch = afe_handle->get_feed_channel_num(afe_data);
    int16_t *feed_buff = (int16_t *) malloc(feed_chunksize * feed_nch * sizeof(int16_t));
    
    assert(feed_buff);
    while (task_flag) {
        size_t bytesIn = 0;
        esp_err_t result = i2s_channel_read(rx_handle, feed_buff, feed_chunksize * feed_nch * sizeof(int16_t), &bytesIn, portMAX_DELAY);
        afe_handle->feed(afe_data, feed_buff);
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
    
    ESP_LOGI(TAG, "Initializing ESP-SR...");
    
    // 初始化GPIO脉冲输出
    init_pulse_gpio();
    
    // 初始化I2S
    init_i2s();
    
    // 初始化语音识别模型 (跳过WakeNet，只使用MultiNet)
    srmodel_list_t *models = esp_srmodel_init("model");
    afe_config_t *afe_config = afe_config_init("M", models, AFE_TYPE_SR, AFE_MODE_LOW_COST);
    
    // 跳过WakeNet配置，只使用AFE进行音频预处理
    afe_config->wakenet_model_name = NULL;  // 不加载唤醒词模型
    afe_config->aec_init = false;
    afe_handle = esp_afe_handle_from_config(afe_config);
    afe_data = afe_handle->create_from_config(afe_config);
    
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
    
    // 创建结果队列
    g_result_que = xQueueCreate(1, sizeof(sr_result_t));
    
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
    
    espsr_initialized = false;
    ESP_LOGI(TAG, "ESP-SR cleaned up");
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
};

static MP_DEFINE_CONST_DICT(espsr_module_globals, espsr_module_globals_table);

const mp_obj_module_t mp_module_espsr = {
    .base = { &mp_type_module },
    .globals = (mp_obj_dict_t *)&espsr_module_globals,
};

MP_REGISTER_MODULE(MP_QSTR_espsr, mp_module_espsr);
