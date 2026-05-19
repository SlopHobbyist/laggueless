#include "integer_scaling.h"
#include <math.h>

i32 me_iscale_ratio(i32 areaW, i32 areaH, i32 imgW, i32 imgH) {
    i32 r = ((i64)areaH * imgW < (i64)areaW * imgH) ? (areaH / imgH) : (areaW / imgW);
    return r < 1 ? 1 : r;
}

void me_iscale_ratios(i32 areaW, i32 areaH, i32 imgW, i32 imgH,
                      i32 aspectX, i32 aspectY,
                      i32 *out_rx, i32 *out_ry) {
    i32 maxRx = areaW / imgW;
    i32 maxRy = areaH / imgH;
    if (maxRx < 1) maxRx = 1;
    if (maxRy < 1) maxRy = 1;

    /* Decide pinned axis: whichever would overshoot the target aspect first. */
    i64 lhs = (i64)maxRx * imgW * aspectY;
    i64 rhs = (i64)maxRy * imgH * aspectX;

    i32 rx, ry;
    if (lhs < rhs) {
        /* Pin X to max; search Y. */
        rx = maxRx;
        double frac = (double)rx * imgW * aspectY / ((double)aspectX * imgH);
        i32 lo = (i32)floor(frac), hi = (i32)ceil(frac);
        if (lo < 1) lo = 1;
        if (hi < 1) hi = 1;
        double tgt = (double)aspectX / aspectY;
        double aLo = (double)(rx * imgW) / (double)(lo * imgH);
        double aHi = (double)(rx * imgW) / (double)(hi * imgH);
        double eLo = fabs(aLo - tgt), eHi = fabs(aHi - tgt);
        if (fabs(eLo - eHi) < 0.001)
            ry = (abs(lo - rx) <= abs(hi - rx)) ? lo : hi;
        else
            ry = (eLo < eHi) ? lo : hi;
    } else {
        ry = maxRy;
        double frac = (double)ry * imgH * aspectX / ((double)aspectY * imgW);
        i32 lo = (i32)floor(frac), hi = (i32)ceil(frac);
        if (lo < 1) lo = 1;
        if (hi < 1) hi = 1;
        double tgt = (double)aspectX / aspectY;
        double aLo = (double)(lo * imgW) / (double)(ry * imgH);
        double aHi = (double)(hi * imgW) / (double)(ry * imgH);
        double eLo = fabs(aLo - tgt), eHi = fabs(aHi - tgt);
        if (fabs(eLo - eHi) < 0.001)
            rx = (abs(lo - ry) <= abs(hi - ry)) ? lo : hi;
        else
            rx = (eLo < eHi) ? lo : hi;
    }

    *out_rx = rx < 1 ? 1 : rx;
    *out_ry = ry < 1 ? 1 : ry;
}

void me_iscale_size_perfect_y(i32 areaW, i32 areaH, i32 imgW, i32 imgH,
                              i32 aspectX, i32 aspectY,
                              i32 *out_w, i32 *out_h) {
    i32 ry = areaH / imgH;
    if (ry < 1) ry = 1;
    i32 h = imgH * ry;
    /* Target aspect for the whole image: width = h * aspectX / aspectY. */
    i32 w = (i32)((double)h * aspectX / aspectY + 0.5);
    if (w > areaW) w = areaW;
    *out_w = w;
    *out_h = h;
}
