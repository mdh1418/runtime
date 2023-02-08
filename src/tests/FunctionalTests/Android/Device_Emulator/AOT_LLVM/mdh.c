#include <stdint.h>

int32_t value = 0;

void __attribute__((constructor))
init_func(void) {
    value = 7;
}

int32_t
mdh (void)
{
    return 1418;
}