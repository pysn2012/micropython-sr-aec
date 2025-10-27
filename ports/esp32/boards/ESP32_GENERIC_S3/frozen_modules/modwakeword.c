// micropython/extmod/modwakeword.c

#include "py/obj.h"
#include "py/runtime.h"
#include "driver/i2s.h"
#include "esp_mn_models.h"
#include "esp_wn_iface.h"
#include "esp_afe_sr_iface.h"

#warning "Compiling test module!"  // 添加这行

// 声明你的C函数
extern void app_main(void);
extern void sr_handler_task(void *pvParam);
extern void detect_Task(void *arg);
extern void feed_Task(void *arg);

// MicroPython 函数实现：初始化唤醒词识别
STATIC mp_obj_t wakeword_init(void) {
    // 初始化NVS和I2S
    app_main();
    return mp_const_none;
}
STATIC MP_DEFINE_CONST_FUN_OBJ_0(wakeword_init_obj, wakeword_init);

// MicroPython 函数实现：开始监听唤醒词
STATIC mp_obj_t wakeword_start(void) {
    // 创建并启动任务
    xTaskCreatePinnedToCore(&feed_Task, "feed", 8 * 1024, NULL, 5, NULL, 0);
    xTaskCreatePinnedToCore(&detect_Task, "detect", 4 * 1024, NULL, 5, NULL, 1);
    xTaskCreatePinnedToCore(&sr_handler_task, "SR Handler Task", 4 * 1024, NULL, 1, NULL, 1);
    return mp_const_none;
}
STATIC MP_DEFINE_CONST_FUN_OBJ_0(wakeword_start_obj, wakeword_start);

// MicroPython 函数实现：检查是否检测到唤醒词
STATIC mp_obj_t wakeword_detected(void) {
    // 返回detect_flag的值
    return mp_obj_new_bool(detect_flag);
}
STATIC MP_DEFINE_CONST_FUN_OBJ_0(wakeword_detected_obj, wakeword_detected);

// 定义模块方法表
STATIC const mp_rom_map_elem_t wakeword_module_globals_table[] = {
    { MP_ROM_QSTR(MP_QSTR___name__), MP_ROM_QSTR(MP_QSTR_wakeword) },
    { MP_ROM_QSTR(MP_QSTR_init), MP_ROM_PTR(&wakeword_init_obj) },
    { MP_ROM_QSTR(MP_QSTR_start), MP_ROM_PTR(&wakeword_start_obj) },
    { MP_ROM_QSTR(MP_QSTR_detected), MP_ROM_PTR(&wakeword_detected_obj) },
};
STATIC MP_DEFINE_CONST_DICT(wakeword_module_globals, wakeword_module_globals_table);

// 定义模块对象
const mp_obj_module_t wakeword_module = {
    .base = { &mp_type_module },
    .globals = (mp_obj_dict_t *)&wakeword_module_globals,
};

// 模块注册
MP_REGISTER_MODULE(MP_QSTR_wakeword, wakeword_module);
