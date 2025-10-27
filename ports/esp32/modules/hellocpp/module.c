// module.c

#include "py/obj.h"
#include "py/runtime.h"

// 声明外部 C++ 函数
extern void say_hello_cpp();

// MicroPython 包装函数
STATIC mp_obj_t hellocpp_say_hello() {
    say_hello_cpp();
    return mp_const_none;
}
STATIC MP_DEFINE_CONST_FUN_OBJ_0(hellocpp_say_hello_obj, hellocpp_say_hello);

// 方法映射表
STATIC const mp_rom_map_elem_t hellocpp_module_globals_table[] = {
    { MP_ROM_QSTR(MP_QSTR___name__), MP_ROM_QSTR(MP_QSTR_hellocpp) },
    { MP_ROM_QSTR(MP_QSTR_say_hello), MP_ROM_PTR(&hellocpp_say_hello_obj) },
};
STATIC MP_DEFINE_CONST_DICT(hellocpp_module_globals, hellocpp_module_globals_table);

// 定义模块对象
const mp_obj_module_t hellocpp_user_cmodule = {
    .base = { &mp_type_module },
    .globals = (mp_obj_dict_t*)&hellocpp_module_globals,
};

// 注册模块
MP_REGISTER_MODULE(MP_QSTR_hellocpp, hellocpp_user_cmodule);

