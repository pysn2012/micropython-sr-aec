/*
 * This file is part of the MicroPython project, http://micropython.org/
 *
 * The MIT License (MIT)
 *
 * Copyright (c) 2021 Mike Teachman
 *
 * Permission is hereby granted, free of charge, to any person obtaining a copy
 * of this software and associated documentation files (the "Software"), to deal
 * in the Software without restriction, including without limitation the rights
 * to use, copy, modify, merge, publish, distribute, sublicense, and/or sell
 * copies of the Software, and to permit persons to whom the Software is
 * furnished to do so, subject to the following conditions:
 *
 * The above copyright notice and this permission notice shall be included in
 * all copies or substantial portions of the Software.
 *
 * THE SOFTWARE IS PROVIDED "AS IS", WITHOUT WARRANTY OF ANY KIND, EXPRESS OR
 * IMPLIED, INCLUDING BUT NOT LIMITED TO THE WARRANTIES OF MERCHANTABILITY,
 * FITNESS FOR A PARTICULAR PURPOSE AND NONINFRINGEMENT. IN NO EVENT SHALL THE
 * AUTHORS OR COPYRIGHT HOLDERS BE LIABLE FOR ANY CLAIM, DAMAGES OR OTHER
 * LIABILITY, WHETHER IN AN ACTION OF CONTRACT, TORT OR OTHERWISE, ARISING FROM,
 * OUT OF OR IN CONNECTION WITH THE SOFTWARE OR THE USE OR OTHER DEALINGS IN
 * THE SOFTWARE.
 */

#ifndef ESP32_I2S_C
#define ESP32_I2S_C

#include "py/mphal.h"
#include "driver/i2s_std.h"
#include "driver/i2s_pdm.h"
#include "freertos/FreeRTOS.h"
#include "freertos/task.h"
#include "freertos/queue.h"
#include "esp_task.h"
#include "soc/soc.h"
#include "soc/system_reg.h"
#include "hal/soc_ll.h"
#include "hal/gpio_ll.h"

#define I2S_TASK_PRIORITY        (ESP_TASK_PRIO_MIN + 1)
#define I2S_TASK_STACK_SIZE      (2048)
#define DMA_BUF_LEN_IN_I2S_FRAMES (256)
#define SIZEOF_TRANSFORM_BUFFER_IN_BYTES (240)
#define DEFAULT_IBUF_SIZE        (1024)
#define DEFAULT_SAMPLE_RATE      (16000)

typedef enum {
    I2S_TX_TRANSFER,
    I2S_RX_TRANSFER,
} direction_t;

typedef struct _non_blocking_descriptor_t {
    mp_buffer_info_t appbuf;
    mp_obj_t callback;
    direction_t direction;
} non_blocking_descriptor_t;

typedef enum {
    DMA_MEMORY_FULL,
    DMA_MEMORY_NOT_FULL,
    DMA_MEMORY_EMPTY,
    DMA_MEMORY_NOT_EMPTY,
} dma_buffer_status_t;

typedef struct _machine_i2s_obj_t {
    mp_obj_base_t base;
    i2s_port_t i2s_id;
    i2s_chan_handle_t i2s_chan_handle;
    mp_hal_pin_obj_t sck;
    mp_hal_pin_obj_t ws;
    mp_hal_pin_obj_t sd;
    i2s_dir_t mode;
    i2s_data_bit_width_t bits;
    format_t format;
    int32_t rate;
    int32_t ibuf;
    mp_obj_t callback_for_non_blocking;
    io_mode_t io_mode;
    bool is_deinit;
    uint8_t transform_buffer[SIZEOF_TRANSFORM_BUFFER_IN_BYTES];
    QueueHandle_t non_blocking_mode_queue;
    TaskHandle_t non_blocking_mode_task;
    dma_buffer_status_t dma_buffer_status;
} machine_i2s_obj_t;

static const int8_t i2s_frame_map[NUM_I2S_USER_FORMATS][I2S_RX_FRAME_SIZE_IN_BYTES] = {
    { 2,  3, -1, -1, -1, -1, -1, -1 },  // Mono, 16-bits
    { 0,  1,  2,  3, -1, -1, -1, -1 },  // Mono, 32-bits
    { 2,  3,  6,  7, -1, -1, -1, -1 },  // Stereo, 16-bits
    { 0,  1,  2,  3,  4,  5,  6,  7 },  // Stereo, 32-bits
};

void machine_i2s_init0() {
    for (i2s_port_t p = 0; p < I2S_NUM_AUTO; p++) {
        MP_STATE_PORT(machine_i2s_obj)[p] = NULL;
    }
}

static int8_t get_frame_mapping_index(i2s_data_bit_width_t bits, format_t format) {
    // Safeguard against invalid bits
    if (bits == 0) {
        bits = I2S_DATA_BIT_WIDTH_16BIT; // Default to 16-bit
    }
    
    if (format == MONO) {
        return (bits == I2S_DATA_BIT_WIDTH_16BIT) ? 0 : 1;
    } else if (format == STEREO) {
        return (bits == I2S_DATA_BIT_WIDTH_16BIT) ? 2 : 3;
    } else { // PDM
        return 0; // Use Mono 16-bit mapping for PDM
    }
}

static i2s_data_bit_width_t get_dma_bits(uint8_t mode, i2s_data_bit_width_t bits) {
    if (mode == MICROPY_PY_MACHINE_I2S_CONSTANT_TX) {
        return (bits == 0) ? I2S_DATA_BIT_WIDTH_16BIT : bits;
    } else { // Master Rx
        return I2S_DATA_BIT_WIDTH_32BIT;
    }
}

static i2s_slot_mode_t get_dma_format(uint8_t mode, format_t format) {
    if (mode == MICROPY_PY_MACHINE_I2S_CONSTANT_TX) {
        if (format == MONO) {
            return I2S_SLOT_MODE_MONO;
        } else if (format == STEREO) {
            return I2S_SLOT_MODE_STEREO;
        } else { // PDM
            return I2S_SLOT_MODE_MONO;
        }
    } else { // Master Rx
        return I2S_SLOT_MODE_STEREO;
    }
}

static uint32_t get_dma_buf_count(uint8_t mode, i2s_data_bit_width_t bits, format_t format, int32_t ibuf) {
    // Validate input parameters
    if (ibuf <= 0) {
        ibuf = DEFAULT_IBUF_SIZE;
    }
    
    i2s_data_bit_width_t dma_bits = get_dma_bits(mode, bits);
    uint32_t channels = (get_dma_format(mode, format) == I2S_SLOT_MODE_STEREO ? 2 : 1);
    uint32_t dma_frame_size_in_bytes = (dma_bits / 8) * channels;
    
    // Ensure frame size is at least 1 byte
    if (dma_frame_size_in_bytes == 0) {
        dma_frame_size_in_bytes = 4; // Fallback to 32-bit stereo
    }
    
    uint32_t total_frame_size = DMA_BUF_LEN_IN_I2S_FRAMES * dma_frame_size_in_bytes;
    if (total_frame_size == 0) {
        total_frame_size = 1024; // Fallback value
    }
    
    uint32_t dma_buf_count = ibuf / total_frame_size;
    return MAX(dma_buf_count, 2); // Minimum 2 buffers required by ESP-IDF
}

// 添加PDM专用的直接读取函数，与C代码一致
static uint32_t fill_appbuf_from_pdm_dma(machine_i2s_obj_t *self, mp_buffer_info_t *appbuf) {
    uint32_t a_index = 0;
    
    // 对于PDM，直接读取数据，不进行复杂的帧映射
    size_t num_bytes_requested = appbuf->len;
    size_t num_bytes_received = 0;
    
    TickType_t delay = (self->io_mode == ASYNCIO) ? 0 : portMAX_DELAY;
    
    esp_err_t ret = i2s_channel_read(
        self->i2s_chan_handle,
        appbuf->buf,  // 直接写入应用缓冲区
        num_bytes_requested,
        &num_bytes_received,
        delay);
        
    if ((self->io_mode != ASYNCIO) || (ret != ESP_ERR_TIMEOUT)) {
        check_esp_err(ret);
    }
    
    a_index = num_bytes_received;
    
    if ((self->io_mode == ASYNCIO) && (num_bytes_received < num_bytes_requested)) {
        self->dma_buffer_status = DMA_MEMORY_EMPTY;
    }
    
    return a_index;
}

static uint32_t fill_appbuf_from_dma(machine_i2s_obj_t *self, mp_buffer_info_t *appbuf) {
    // 对于PDM格式，使用简化的直接读取方式
    if (self->format == PDM) {
        return fill_appbuf_from_pdm_dma(self, appbuf);
    }
    
    // 原有的标准I2S处理逻辑
    uint32_t a_index = 0;
    uint8_t *app_p = appbuf->buf;
    
    // Safeguard against invalid bits configuration
    i2s_data_bit_width_t safe_bits = (self->bits == 0) ? I2S_DATA_BIT_WIDTH_16BIT : self->bits;
    uint8_t appbuf_sample_size_in_bytes = (safe_bits / 8) * (self->format == STEREO ? 2 : 1);
    
    if (appbuf_sample_size_in_bytes == 0) {
        appbuf_sample_size_in_bytes = 2; // Fallback to 16-bit mono
    }
    
    uint32_t num_bytes_needed_from_dma = appbuf->len * (I2S_RX_FRAME_SIZE_IN_BYTES / appbuf_sample_size_in_bytes);
    
    while (num_bytes_needed_from_dma > 0) {
        size_t num_bytes_requested_from_dma = MIN(sizeof(self->transform_buffer), num_bytes_needed_from_dma);
        size_t num_bytes_received_from_dma = 0;
        
        TickType_t delay = (self->io_mode == ASYNCIO) ? 0 : portMAX_DELAY;
        
        esp_err_t ret = i2s_channel_read(
            self->i2s_chan_handle,
            self->transform_buffer,
            num_bytes_requested_from_dma,
            &num_bytes_received_from_dma,
            delay);
            
        if ((self->io_mode != ASYNCIO) || (ret != ESP_ERR_TIMEOUT)) {
            check_esp_err(ret);
        }
        
        uint32_t t_index = 0;
        uint8_t f_index = get_frame_mapping_index(safe_bits, self->format);
        
        while (t_index < num_bytes_received_from_dma) {
            uint8_t *transform_p = self->transform_buffer + t_index;
            
            for (uint8_t i = 0; i < I2S_RX_FRAME_SIZE_IN_BYTES; i++) {
                int8_t t_to_a_mapping = i2s_frame_map[f_index][i];
                if (t_to_a_mapping != -1) {
                    *app_p++ = transform_p[t_to_a_mapping];
                    a_index++;
                }
                t_index++;
            }
        }
        
        num_bytes_needed_from_dma -= num_bytes_received_from_dma;
        
        if ((self->io_mode == ASYNCIO) && (num_bytes_received_from_dma < num_bytes_requested_from_dma)) {
            self->dma_buffer_status = DMA_MEMORY_EMPTY;
            break;
        }
    }
    
    return a_index;
}

static size_t copy_appbuf_to_dma(machine_i2s_obj_t *self, mp_buffer_info_t *appbuf) {
    size_t num_bytes_written = 0;

    // Check if channel is valid
    if (self->i2s_chan_handle == NULL) {
        mp_raise_OSError(MP_ENODEV);
    }

    // Check if channel is enabled
    if (self->is_deinit) {
        mp_raise_OSError(MP_EPERM);
    }

    TickType_t delay = (self->io_mode == ASYNCIO) ? 0 : portMAX_DELAY;

    esp_err_t ret = i2s_channel_write(self->i2s_chan_handle, appbuf->buf, appbuf->len, &num_bytes_written, delay);

    if ((self->io_mode != ASYNCIO) || (ret != ESP_ERR_TIMEOUT)) {
        check_esp_err(ret);
    }

    if ((self->io_mode == ASYNCIO) && (num_bytes_written < appbuf->len)) {
        self->dma_buffer_status = DMA_MEMORY_FULL;
    }

    return num_bytes_written;
}

// FreeRTOS task used for non-blocking mode
static void task_for_non_blocking_mode(void *self_in) {
    machine_i2s_obj_t *self = (machine_i2s_obj_t *)self_in;

    non_blocking_descriptor_t descriptor;

    for (;;) {
        if (xQueueReceive(self->non_blocking_mode_queue, &descriptor, portMAX_DELAY)) {
            if (descriptor.direction == I2S_TX_TRANSFER) {
                copy_appbuf_to_dma(self, &descriptor.appbuf);
            } else { // RX
                fill_appbuf_from_dma(self, &descriptor.appbuf);
            }
            mp_sched_schedule(descriptor.callback, MP_OBJ_FROM_PTR(self));
        }
    }
}

// callback indicating that a DMA buffer was just filled with samples received from an I2S port
static IRAM_ATTR bool i2s_rx_recv_callback(i2s_chan_handle_t handle, i2s_event_data_t *event, void *self_in) {
    machine_i2s_obj_t *self = (machine_i2s_obj_t *)self_in;
    self->dma_buffer_status = DMA_MEMORY_NOT_EMPTY;
    return false;
}

// callback indicating that samples in a DMA buffer were just transmitted to an I2S port
static IRAM_ATTR bool i2s_tx_sent_callback(i2s_chan_handle_t handle, i2s_event_data_t *event, void *self_in) {
    machine_i2s_obj_t *self = (machine_i2s_obj_t *)self_in;
    self->dma_buffer_status = DMA_MEMORY_NOT_FULL;
    return false;
}

i2s_event_callbacks_t i2s_callbacks = {
    .on_recv = i2s_rx_recv_callback,
    .on_recv_q_ovf = NULL,
    .on_sent = i2s_tx_sent_callback,
    .on_send_q_ovf = NULL,
};

i2s_event_callbacks_t i2s_callbacks_null = {
    .on_recv = NULL,
    .on_recv_q_ovf = NULL,
    .on_sent = NULL,
    .on_send_q_ovf = NULL,
};

static mp_obj_t machine_i2s_deinit(mp_obj_t self_in);

// 正确的硬件复位函数
static void hardware_reset_i2s(i2s_port_t i2s_id) {
    printf("执行I2S%d硬件复位...\n", i2s_id);
    
    if (i2s_id == 0) {
        // 复位I2S0
        SET_PERI_REG_MASK(SYSTEM_PERIP_RST_EN0_REG, SYSTEM_I2S0_RST);
        vTaskDelay(pdMS_TO_TICKS(10));  // 等待复位生效
        CLEAR_PERI_REG_MASK(SYSTEM_PERIP_RST_EN0_REG, SYSTEM_I2S0_RST);
        vTaskDelay(pdMS_TO_TICKS(10));  // 等待复位完成
    } else if (i2s_id == 1) {
        // 复位I2S1
        SET_PERI_REG_MASK(SYSTEM_PERIP_RST_EN0_REG, SYSTEM_I2S1_RST);
        vTaskDelay(pdMS_TO_TICKS(10));  // 等待复位生效
        CLEAR_PERI_REG_MASK(SYSTEM_PERIP_RST_EN0_REG, SYSTEM_I2S1_RST);
        vTaskDelay(pdMS_TO_TICKS(10));  // 等待复位完成
    }
    
    printf("I2S%d硬件复位完成\n", i2s_id);
}

static void mp_machine_i2s_init_helper(machine_i2s_obj_t *self, mp_arg_val_t *args) {
    // 强制清理现有资源，包括硬件复位
    if (self->i2s_chan_handle != NULL) {
        printf("检测到现有I2S/DMA资源，强制清理...\n");
        mp_machine_i2s_deinit(self);
        vTaskDelay(pdMS_TO_TICKS(200));  // 等待DMA资源释放
    } else {
        // 即使没有句柄，也执行硬件复位确保状态干净
        printf("执行硬件复位确保I2S状态干净...\n");
        hardware_reset_i2s(self->i2s_id);
        vTaskDelay(pdMS_TO_TICKS(50));
    }
    
    // 提取并设置参数
    self->sck = args[ARG_sck].u_obj == MP_OBJ_NULL ? -1 : machine_pin_get_id(args[ARG_sck].u_obj);
    self->ws = args[ARG_ws].u_obj == MP_OBJ_NULL ? -1 : machine_pin_get_id(args[ARG_ws].u_obj);
    self->sd = args[ARG_sd].u_obj == MP_OBJ_NULL ? -1 : machine_pin_get_id(args[ARG_sd].u_obj);
    self->mode = args[ARG_mode].u_int;
    self->bits = args[ARG_bits].u_int;
    self->format = args[ARG_format].u_int;
    self->rate = args[ARG_rate].u_int;
    self->ibuf = args[ARG_ibuf].u_int;
    self->callback_for_non_blocking = MP_OBJ_NULL;
    self->io_mode = BLOCKING;
    self->is_deinit = false;
    
    // Initialize PDM microphone if configured
    if (self->format == PDM) {

        if (self->mode != MICROPY_PY_MACHINE_I2S_CONSTANT_RX) {
            mp_raise_ValueError(MP_ERROR_TEXT("PDM only supports RX mode"));
        }
        
        // Validate and set default parameters
        if (self->rate <= 0) {
            self->rate = DEFAULT_SAMPLE_RATE;
        }
        if (self->ibuf <= 0) {
            self->ibuf = DEFAULT_IBUF_SIZE;
        }
        if (self->bits != I2S_DATA_BIT_WIDTH_16BIT) {
            self->bits = I2S_DATA_BIT_WIDTH_16BIT; // PDM typically uses 16-bit
        }
        
        // Clean up existing channel if any
        if (self->i2s_chan_handle != NULL) {
            i2s_del_channel(self->i2s_chan_handle);
            self->i2s_chan_handle = NULL;
        }
        
        // Configure PDM RX channel
        i2s_chan_config_t pdm_chan_cfg = I2S_CHANNEL_DEFAULT_CONFIG(self->i2s_id, I2S_ROLE_MASTER);
        pdm_chan_cfg.dma_desc_num = get_dma_buf_count(self->mode, self->bits, self->format, self->ibuf);
        pdm_chan_cfg.dma_frame_num = DMA_BUF_LEN_IN_I2S_FRAMES;
        pdm_chan_cfg.auto_clear = true;
        
        check_esp_err(i2s_new_channel(&pdm_chan_cfg, NULL, &self->i2s_chan_handle));
        
        // Configure PDM RX mode
        i2s_pdm_rx_config_t pdm_rx_cfg = {
            .clk_cfg = I2S_PDM_RX_CLK_DEFAULT_CONFIG(self->rate),
            .slot_cfg = I2S_PDM_RX_SLOT_DEFAULT_CONFIG(I2S_DATA_BIT_WIDTH_16BIT, I2S_SLOT_MODE_MONO),
            .gpio_cfg = {
                .clk = self->sck,
                .din = self->sd,
                .invert_flags = {
                    .clk_inv = false,
                },
            },
        };
        
        check_esp_err(i2s_channel_init_pdm_rx_mode(self->i2s_chan_handle, &pdm_rx_cfg));
        check_esp_err(i2s_channel_enable(self->i2s_chan_handle));
        vTaskDelay(pdMS_TO_TICKS(10)); // Short delay for PDM to stabilize
        printf("init pdm helper self->is_deinit = false\n");
        return;
    }
    
    // Validate and fix parameters for standard I2S
    if (self->bits == 0) {
        self->bits = I2S_DATA_BIT_WIDTH_16BIT; // Default to 16-bit
    }
    if (self->format != MONO && self->format != STEREO && self->format != PDM) {
        self->format = MONO; // Default to mono
    }
    if (self->ibuf <= 0) {
        self->ibuf = DEFAULT_IBUF_SIZE; // Default buffer size
    }
    if (self->rate <= 0) {
        self->rate = DEFAULT_SAMPLE_RATE; // Default sample rate
    }
    
    // Clean up existing channel if any
    if (self->i2s_chan_handle != NULL) {
        i2s_del_channel(self->i2s_chan_handle);
        self->i2s_chan_handle = NULL;
    }
    
    // Standard I2S initialization
    i2s_chan_config_t chan_config = I2S_CHANNEL_DEFAULT_CONFIG(self->i2s_id, I2S_ROLE_MASTER);
    chan_config.dma_desc_num = get_dma_buf_count(self->mode, self->bits, self->format, self->ibuf);
    chan_config.dma_frame_num = DMA_BUF_LEN_IN_I2S_FRAMES;
    chan_config.auto_clear = true;
    
    if (self->mode == MICROPY_PY_MACHINE_I2S_CONSTANT_TX) {
        check_esp_err(i2s_new_channel(&chan_config, &self->i2s_chan_handle, NULL));
    } else {
        check_esp_err(i2s_new_channel(&chan_config, NULL, &self->i2s_chan_handle));
    }
    
    i2s_std_slot_config_t slot_cfg = I2S_STD_PHILIPS_SLOT_DEFAULT_CONFIG(
        get_dma_bits(self->mode, self->bits), 
        get_dma_format(self->mode, self->format));
    slot_cfg.slot_mask = I2S_STD_SLOT_BOTH;
    
    i2s_std_config_t std_cfg = {
        .clk_cfg = I2S_STD_CLK_DEFAULT_CONFIG(self->rate),
        .slot_cfg = slot_cfg,
        .gpio_cfg = {
            .mclk = I2S_GPIO_UNUSED,
            .bclk = self->sck,
            .ws = self->ws,
            .dout = (self->mode == MICROPY_PY_MACHINE_I2S_CONSTANT_TX) ? self->sd : I2S_GPIO_UNUSED,
            .din = (self->mode == MICROPY_PY_MACHINE_I2S_CONSTANT_RX) ? self->sd : I2S_GPIO_UNUSED,
            .invert_flags = {
                .mclk_inv = false,
                .bclk_inv = false,
                .ws_inv = false,
            },
        },
    };
    
    check_esp_err(i2s_channel_init_std_mode(self->i2s_chan_handle, &std_cfg));
    check_esp_err(i2s_channel_register_event_callback(self->i2s_chan_handle, &i2s_callbacks, self));
    check_esp_err(i2s_channel_enable(self->i2s_chan_handle));
    
    // Mark as initialized
    printf("init helper self->is_deinit = false\n");
}

static mp_obj_t machine_i2s_deinit(mp_obj_t self_in) {
    printf("I2S deinit...\n");
    machine_i2s_obj_t *self = MP_OBJ_TO_PTR(self_in);
    mp_machine_i2s_deinit(self);
    printf("I2S deinit finished\n");
    return mp_const_none;
}

static machine_i2s_obj_t *mp_machine_i2s_make_new_instance(mp_int_t i2s_id) {
    if (i2s_id < 0 || i2s_id >= I2S_NUM_AUTO) {
        mp_raise_ValueError(MP_ERROR_TEXT("invalid id"));
    }

    machine_i2s_obj_t *self;
    if (MP_STATE_PORT(machine_i2s_obj)[i2s_id] == NULL) {
        self = mp_obj_malloc_with_finaliser(machine_i2s_obj_t, &machine_i2s_type);
        MP_STATE_PORT(machine_i2s_obj)[i2s_id] = self;
        self->i2s_id = i2s_id;
        self->i2s_chan_handle = NULL;
        self->non_blocking_mode_queue = NULL;
        self->non_blocking_mode_task = NULL;
        self->dma_buffer_status = DMA_MEMORY_EMPTY;
        printf("instance 1 , is_deinit = true");
    } else {
        self = MP_STATE_PORT(machine_i2s_obj)[i2s_id];
        machine_i2s_deinit(self);

        // memset(self, 0, sizeof(machine_i2s_obj_t));  // 重置内存
        // self->i2s_id = i2s_id;  // 重新设置ID
        // self->dma_buffer_status = DMA_MEMORY_EMPTY;
        self->i2s_id = i2s_id;
        self->i2s_chan_handle = NULL;
        self->non_blocking_mode_queue = NULL;
        self->non_blocking_mode_task = NULL;
        self->dma_buffer_status = DMA_MEMORY_EMPTY;
        printf("instance 2 , is_deinit = true");
    }

    return self;
}

// 修复 mp_machine_i2s_deinit 函数，添加DMA资源释放
static void mp_machine_i2s_deinit(machine_i2s_obj_t *self) {
    if (!self->is_deinit) {
        printf("开始清理I2S和DMA资源...\n");
        
        printf("开始清理I2S资源...\n");
        
        if (self->i2s_chan_handle) {
            // 清理 I2S通道 和 DMA资源
            ESP_ERROR_CHECK(i2s_channel_disable(self->i2s_chan_handle));
            ESP_ERROR_CHECK(i2s_channel_register_event_callback(self->i2s_chan_handle, &i2s_callbacks_null, self));
            ESP_ERROR_CHECK(i2s_del_channel(self->i2s_chan_handle));
            
            self->i2s_chan_handle = NULL;
            printf("I2S通道清理完成\n");
        }

        // 清理FreeRTOS任务
        if (self->non_blocking_mode_task != NULL) {
            vTaskDelete(self->non_blocking_mode_task);
            self->non_blocking_mode_task = NULL;
        }

        // 清理队列
        if (self->non_blocking_mode_queue != NULL) {
            vQueueDelete(self->non_blocking_mode_queue);
            self->non_blocking_mode_queue = NULL;
        }
        
        // 标记为已清理
        self->is_deinit = true;
        printf("I2S资源清理完成\n");
    }
} 

static void mp_machine_i2s_irq_update(machine_i2s_obj_t *self) {
    if (self->io_mode == NON_BLOCKING) {
        // create a queue linking the MicroPython task to a FreeRTOS task
        // that manages the non blocking mode of operation
        self->non_blocking_mode_queue = xQueueCreate(1, sizeof(non_blocking_descriptor_t));

        // non-blocking mode requires a background FreeRTOS task
        if (xTaskCreatePinnedToCore(
            task_for_non_blocking_mode,
            "i2s_non_blocking",
            I2S_TASK_STACK_SIZE,
            self,
            I2S_TASK_PRIORITY,
            (TaskHandle_t *)&self->non_blocking_mode_task,
            MP_TASK_COREID) != pdPASS) {

            mp_raise_msg(&mp_type_RuntimeError, MP_ERROR_TEXT("failed to create I2S task"));
        }
    } else {
        if (self->non_blocking_mode_task != NULL) {
            vTaskDelete(self->non_blocking_mode_task);
            self->non_blocking_mode_task = NULL;
        }

        if (self->non_blocking_mode_queue != NULL) {
            vQueueDelete(self->non_blocking_mode_queue);
            self->non_blocking_mode_queue = NULL;
        }
    }
}

MP_REGISTER_ROOT_POINTER(struct _machine_i2s_obj_t *machine_i2s_obj[I2S_NUM_AUTO]);

#endif // ESP32_I2S_C
