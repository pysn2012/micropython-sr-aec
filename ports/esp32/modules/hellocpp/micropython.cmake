set(MICROPY_MODULE_HELLOCPP_ENABLED 1)

add_library(usermod_hellocpp
    ${CMAKE_CURRENT_LIST_DIR}/module.c
    ${CMAKE_CURRENT_LIST_DIR}/hellocpp.cpp
)

target_include_directories(usermod_hellocpp PUBLIC
    ${CMAKE_CURRENT_LIST_DIR}
    ${MICROPY_SOURCE_DIR}
)

target_compile_options(usermod_hellocpp PUBLIC -std=c++17)
