/*
 * Copyright (c) 2006 Kirill Simonov
 *
 * Permission is hereby granted, free of charge, to any person obtaining a copy of
 * this software and associated documentation files (the "Software"), to deal in
 * the Software without restriction, including without limitation the rights to
 * use, copy, modify, merge, publish, distribute, sublicense, and/or sell copies
 * of the Software, and to permit persons to whom the Software is furnished to do
 * so, subject to the following conditions:
 *
 * The above copyright notice and this permission notice shall be included in all
 * copies or substantial portions of the Software.
 *
 * THE SOFTWARE IS PROVIDED "AS IS", WITHOUT WARRANTY OF ANY KIND, EXPRESS OR
 * IMPLIED, INCLUDING BUT NOT LIMITED TO THE WARRANTIES OF MERCHANTABILITY,
 * FITNESS FOR A PARTICULAR PURPOSE AND NONINFRINGEMENT. IN NO EVENT SHALL THE
 * AUTHORS OR COPYRIGHT HOLDERS BE LIABLE FOR ANY CLAIM, DAMAGES OR OTHER
 * LIABILITY, WHETHER IN AN ACTION OF CONTRACT, TORT OR OTHERWISE, ARISING FROM,
 * OUT OF OR IN CONNECTION WITH THE SOFTWARE OR THE USE OR OTHER DEALINGS IN THE
 * SOFTWARE.
 */
#ifndef UTF8_H
#define UTF8_H

#define IS_PRINTABLE(string)                                      \
    ((pointer[0] == 0x0A)         /* . == #x0A */                 \
     || (pointer[0] >= 0x20       /* #x20 <= . <= #x7E */         \
         && pointer[0] <= 0x7E)                                   \
     || (pointer[0] == 0xC2       /* #0xA0 <= . <= #xD7FF */      \
         && width > 0 && pointer[1] >= 0xA0)                      \
     || (pointer[0] > 0xC2                                        \
         && pointer[0] < 0xED)                                    \
     || (pointer[0] == 0xED && width > 0                          \
         && pointer[1] < 0xA0)                                    \
     || (pointer[0] == 0xEE)                                      \
     || (pointer[0] == 0xEF      /* #xE000 <= . <= #xFFFD */      \
         && !(pointer[1] == 0xBB        /* && . != #xFEFF */      \
         && width > 1 && pointer[2] == 0xBF)                      \
         && !(width > 0 && pointer[1] == 0xBF                     \
             && width > 1 && (pointer[2] == 0xBE                  \
                 || pointer[2] == 0xBF))))

static int
check_utf8(const unsigned char *start, size_t length)
{
    const unsigned char *end = start + length;
    const unsigned char *pointer = start;

    while (pointer < end) {
        unsigned char octet;
        unsigned int width;
        unsigned int value;
        size_t k;

        octet = pointer[0];
        width = (octet & 0x80) == 0x00 ? 1 :
                (octet & 0xE0) == 0xC0 ? 2 :
                (octet & 0xF0) == 0xE0 ? 3 :
                (octet & 0xF8) == 0xF0 ? 4 : 0;
        value = (octet & 0x80) == 0x00 ? octet & 0x7F :
                (octet & 0xE0) == 0xC0 ? octet & 0x1F :
                (octet & 0xF0) == 0xE0 ? octet & 0x0F :
                (octet & 0xF8) == 0xF0 ? octet & 0x07 : 0;
        if (!width) return 0;
        if (pointer + width > end) return 0;
        for (k = 1; k < width; k++) {
            octet = pointer[k];
            if ((octet & 0xC0) != 0x80) return 0;
            value = (value << 6) + (octet & 0x3F);
        }
        if (!((width == 1) ||
            (width == 2 && value >= 0x80) ||
            (width == 3 && value >= 0x800) ||
            (width == 4 && value >= 0x10000))) return 0;

        /* gh-354: yaml incorrectly escapes special characters in a string */
        if (*pointer > 0x7F && !IS_PRINTABLE(pointer))
           return 0;

        pointer += width;
    }
    return 1;
}
#endif //UTF8_H
