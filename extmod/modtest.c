// micropython/extmod/modtest.c
#include "py/obj.h"
#include "py/runtime.h"

STATIC mp_obj_t test_hello(void) {
    mp_printf(&mp_plat_print, "Hello from test module!\n");
    return mp_const_none;
}
STATIC MP_DEFINE_CONST_FUN_OBJ_0(test_hello_obj, test_hello);

STATIC const mp_rom_map_elem_t test_module_globals_table[] = {
    { MP_ROM_QSTR(MP_QSTR___name__), MP_ROM_QSTR(MP_QSTR_test) },
    { MP_ROM_QSTR(MP_QSTR_hello), MP_ROM_PTR(&test_hello_obj) },
};
STATIC MP_DEFINE_CONST_DICT(test_module_globals, test_module_globals_table);

const mp_obj_module_t test_module = {
    .base = { &mp_type_module },
    .globals = (mp_obj_dict_t *)&test_module_globals,
};

MP_REGISTER_MODULE(MP_QSTR_test, test_module);
