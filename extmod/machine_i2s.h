#ifndef MICROPY_INCLUDED_EXTMOD_MACHINE_I2S_H
#define MICROPY_INCLUDED_EXTMOD_MACHINE_I2S_H

// 基础 MicroPython 头文件
#include "py/obj.h"
#include "py/mphal.h"
#include "py/runtime.h"
#include "py/qstr.h"

// 确保 STATIC 宏定义存在
#ifndef STATIC
#define STATIC static
#endif

// ESP-IDF 特定头文件
#if defined(ESP_IDF_VERSION_MAJOR)
#include "esp_idf_version.h"
#if ESP_IDF_VERSION >= ESP_IDF_VERSION_VAL(4, 2, 0)
#include "driver/i2s_std.h"
#include "driver/i2s_pdm.h"
#include "driver/gpio.h"
#include "esp_check.h"
#include "esp_log.h"
#endif // ESP_IDF_VERSION check
#endif // defined(ESP_IDF_VERSION_MAJOR)

// FreeRTOS 头文件
#include <freertos/FreeRTOS.h>
#include <freertos/task.h>
#include <freertos/semphr.h>

// 定义默认值
#define DEFAULT_SAMPLE_RATE (16000)
#define DEFAULT_BUFFER_SIZE (1024)

// I2S 设备对象声明
struct _machine_i2s_obj_t;
typedef struct _machine_i2s_obj_t machine_i2s_obj_t;

// 对象类型和函数声明
extern const mp_obj_type_t machine_i2s_type;
STATIC mp_obj_t machine_i2s_make_new(const mp_obj_type_t *type, size_t n_args, size_t n_kw, const mp_obj_t *args);

// 方法声明
STATIC mp_obj_t machine_i2s_init(mp_obj_t self_in);
STATIC mp_obj_t machine_i2s_deinit(mp_obj_t self_in);
STATIC mp_obj_t machine_i2s_irq(mp_obj_t self_in, mp_obj_t handler);
STATIC void machine_i2s_print(const mp_print_t *print, mp_obj_t self_in, mp_print_kind_t kind);

// 对象声明
MP_DECLARE_CONST_FUN_OBJ_1(machine_i2s_init_obj);
MP_DECLARE_CONST_FUN_OBJ_1(machine_i2s_deinit_obj);
MP_DECLARE_CONST_FUN_OBJ_2(machine_i2s_irq_obj);

#if MICROPY_PY_MACHINE_I2S

// ESP-IDF 相关头文件
#if defined(ESP_IDF_VERSION_MAJOR)
#include "esp_idf_version.h"
#if ESP_IDF_VERSION >= ESP_IDF_VERSION_VAL(4, 2, 0)
#include "driver/i2s_std.h"
#include "driver/i2s_pdm.h"
#include "driver/gpio.h"
#include "esp_check.h"
#include "esp_log.h"
#endif // ESP_IDF_VERSION check
#endif // defined(ESP_IDF_VERSION_MAJOR)

// FreeRTOS 相关头文件
#include "freertos/FreeRTOS.h"
#include "freertos/task.h"
#include "freertos/queue.h"
#include "freertos/semphr.h"

// I2S 模式定义
typedef enum {
    MACHINE_I2S_MODE_STANDARD = 0,  // 标准 I2S 模式
    MACHINE_I2S_MODE_PDM_TX,        // PDM 发送模式
    MACHINE_I2S_MODE_PDM_RX,        // PDM 接收模式
    MACHINE_I2S_MODE_PDM_DUPLEX,    // PDM 全双工模式
} machine_i2s_mode_t;

// 三重缓冲结构
typedef struct {
    uint8_t *buffers[3];          // 三个缓冲区
    int active_buf;               // 当前活动缓冲区
    int process_buf;              // 处理中的缓冲区
    int ready_buf;                // 准备好的缓冲区
    size_t buf_size;             // 缓冲区大小
    SemaphoreHandle_t mutex;      // 互斥量
} triple_buffer_t;

// I2S 对象结构
typedef struct _machine_i2s_obj_t {
    mp_obj_base_t base;
    
    // 基本配置
    uint8_t i2s_id;
    machine_i2s_mode_t mode;
    uint32_t sample_rate_hz;
    uint8_t bits;
    uint8_t format;
    uint8_t channel_count;
    bool is_deinit;
    bool aec_enabled;
    
    // GPIO配置
    int8_t gpio_mclk;
    int8_t gpio_bck;
    int8_t gpio_ws;
    int8_t gpio_din;
    int8_t gpio_dout;
    
    // ESP-IDF 句柄
    i2s_chan_handle_t tx_handle;
    i2s_chan_handle_t rx_handle;
    
    // 缓冲区配置
    triple_buffer_t triple_buf;
    size_t buffer_size;
    
    // 任务和同步
    TaskHandle_t task_handle;
    QueueHandle_t event_queue;
    SemaphoreHandle_t mutex;
    
    // 回调相关
    mp_obj_t callback_for_non_blocking;
    void (*callback)(void);
} machine_i2s_obj_t;

// 函数声明
extern const mp_obj_type_t machine_i2s_type;
void machine_i2s_init0(void);

// QSTR 声明
STATIC mp_obj_t machine_i2s_init(mp_obj_t self_in);
STATIC mp_obj_t machine_i2s_deinit(mp_obj_t self_in);
STATIC mp_obj_t machine_i2s_irq(mp_obj_t self_in, mp_obj_t handler);

#endif // MICROPY_PY_MACHINE_I2S
#endif // MICROPY_INCLUDED_EXTMOD_MACHINE_I2S_H