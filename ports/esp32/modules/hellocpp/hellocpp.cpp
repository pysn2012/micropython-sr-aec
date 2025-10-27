// hellocpp.cpp

#include <stdio.h>

extern "C" {
    void say_hello_cpp() {
        printf("hello c++\n");
    }
}
