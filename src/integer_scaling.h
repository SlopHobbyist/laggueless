#ifndef ME_INTEGER_SCALING_H
#define ME_INTEGER_SCALING_H

#include "types.h"

/* Port of bsnes-mt IntegerScaling (Marat Tanalin) — pure math, no deps. */

/* Square-pixel: largest integer N such that imgW*N <= areaW AND imgH*N <= areaH. Min 1. */
i32 me_iscale_ratio(i32 areaW, i32 areaH, i32 imgW, i32 imgH);

/* Aspect-corrected: writes integer ratioX,ratioY into out_rx,out_ry. */
void me_iscale_ratios(i32 areaW, i32 areaH, i32 imgW, i32 imgH,
                      i32 aspectX, i32 aspectY,
                      i32 *out_rx, i32 *out_ry);

/* "Perfect-Y": integer Y ratio, fractional X (for scanline shaders). */
void me_iscale_size_perfect_y(i32 areaW, i32 areaH, i32 imgW, i32 imgH,
                              i32 aspectX, i32 aspectY,
                              i32 *out_w, i32 *out_h);

#endif
