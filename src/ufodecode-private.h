#ifndef LIB_UFODECODE_PRIVATE_H
#define LIB_UFODECODE_PRIVATE_H

#include <stdbool.h>

struct _UfoDecoder {
    int32_t     height;
    uint32_t    width;
    uint32_t   *raw;
    size_t      num_bytes; 
    uint32_t    current_pos;
};


#endif

