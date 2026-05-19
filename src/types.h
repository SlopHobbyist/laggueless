#ifndef ME_TYPES_H
#define ME_TYPES_H

#include <stdint.h>
#include <stddef.h>

typedef int32_t  i32;
typedef uint32_t u32;
typedef int64_t  i64;
typedef uint64_t u64;
typedef uint8_t  u8;
typedef uint16_t u16;

typedef struct { i32 w, h; } me_size;
typedef struct { i32 x, y, w, h; } me_rect;

#endif
