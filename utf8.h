#ifndef _UTF8_H
#define _UTF8_H

#include <stdbool.h>

/* Length of the well-formed UTF-8 sequence starting at s, or 0 if the bytes there are not one (RFC
 * 3629: no overlong forms, no surrogates, nothing above U+10FFFF). Never reads past a NUL.
 */
int utf8_seq_len(const char *s);
bool utf8_valid(const char *s);

#endif /* _UTF8_H */
