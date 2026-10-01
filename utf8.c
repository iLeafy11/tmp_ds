#include "utf8.h"

int utf8_seq_len(const char *s)
{
    const unsigned char *p = (const unsigned char *)s;
    unsigned char lo = 0x80, hi = 0xBF;   /* bounds for the second byte; the rest are 80..BF */
    int n;

    if (p[0] < 0x80)
        return 1;
    if (p[0] >= 0xC2 && p[0] <= 0xDF) {
        n = 2;
    } else if (p[0] == 0xE0) {
        n = 3, lo = 0xA0;                 /* below A0 would be an overlong 2-byte value */
    } else if (p[0] >= 0xE1 && p[0] <= 0xEC) {
        n = 3;
    } else if (p[0] == 0xED) {
        n = 3, hi = 0x9F;                 /* A0..BF would be a surrogate */
    } else if (p[0] >= 0xEE && p[0] <= 0xEF) {
        n = 3;
    } else if (p[0] == 0xF0) {
        n = 4, lo = 0x90;                 /* below 90 would be an overlong 3-byte value */
    } else if (p[0] >= 0xF1 && p[0] <= 0xF3) {
        n = 4;
    } else if (p[0] == 0xF4) {
        n = 4, hi = 0x8F;                 /* 90.. would be above U+10FFFF */
    } else {
        return 0;                         /* continuation byte, C0, C1, F5..FF */
    }
    if (p[1] < lo || p[1] > hi)
        return 0;
    for (int i = 2; i < n; i++)
        if ((p[i] & 0xC0) != 0x80)
            return 0;
    return n;
}

bool utf8_valid(const char *s)
{
    for (int n; *s; s += n)
        if (!(n = utf8_seq_len(s)))
            return false;
    return true;
}
