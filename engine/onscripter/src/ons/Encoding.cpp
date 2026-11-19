/* -*- C++ -*-
 * 
 *  Encoding.cpp - Character encoding handler
 *
 *  Copyright (c) 2019-2020 Ogapee. All rights reserved.
 *
 *  ogapee@aqua.dti2.ne.jp
 *
 *  This program is free software; you can redistribute it and/or modify
 *  it under the terms of the GNU General Public License as published by
 *  the Free Software Foundation; either version 2 of the License, or
 *  (at your option) any later version.
 *
 *  This program is distributed in the hope that it will be useful,
 *  but WITHOUT ANY WARRANTY; without even the implied warranty of
 *  MERCHANTABILITY or FITNESS FOR A PARTICULAR PURPOSE.  See the
 *  GNU General Public License for more details.
 *
 *  You should have received a copy of the GNU General Public License
 *  along with this program; if not, write to the Free Software
 *  Foundation, Inc., 59 Temple Place, Suite 330, Boston, MA  02111-1307  USA
 */

#include "Encoding.h"

#include <cstdio>

extern unsigned short convSJIS2UTF16(unsigned short in);
extern unsigned short convGBK2UTF16(unsigned short in);
extern unsigned short convUTF8ToUTF16(const char **src);

/* Mirror of the active text encoding for the few helpers that cannot reach
 * ScriptHandler (file name recoding, path conversion). */
int ons_text_encoding = 0;

Encoding::Encoding()
{
    code = CODE_CP932;
}

Encoding::~Encoding()
{
}

void Encoding::setEncoding(int code)
{
    this->code = code;
    ons_text_encoding = code;
}

const char *Encoding::name(int code)
{
    switch (code) {
    case CODE_UTF8: return "UTF8";
    case CODE_GBK:  return "GBK";
    case CODE_CP932:
    default:        return "CP932";
    }
}

int Encoding::getBytes(unsigned char ch, int code)
{
    if (code == -1) code = this->code;

    if (code == CODE_CP932){
        if ((ch & 0xe0) == 0xe0 || (ch & 0xe0) == 0x80) return 2;
    }
    else if (code == CODE_GBK){
        /* GBK lead bytes: 0x81..0xFE.  The trail byte cannot be inspected
         * here, matching the CP932 heuristic above. */
        if (ch >= 0x81 && ch <= 0xfe) return 2;
    }
    else{
        if (ch < 0x80) return 1;
        if (0xc0 <= ch && ch < 0xe0) return 2;
        if (0xe0 <= ch && ch < 0xf0) return 3;
        if (0xf0 <= ch && ch < 0xf8) return 4;
    }

    return 1;
}

int Encoding::getNum(const unsigned char *buf)
{
    int n = 0;
    
    while(buf[0] != 0){
        int n2 = getBytes(buf[0]);
        n++;
        if (n2 > 1) n++;
        buf += n2;
    }

    return n;
}

char Encoding::getTextMarker()
{
    if (code == CODE_UTF8)
        return '^';
    
    return '`';
}

unsigned short Encoding::getUTF16(const char *text, int code)
{
    unsigned short unicode = 0;

    if (code == -1) code = this->code;
    
    if (code == CODE_CP932){
        if ((text[0] & 0xe0) == 0xe0 || (text[0] & 0xe0) == 0x80){
            unsigned index = ((unsigned char*)text)[0];
            index = index << 8 | ((unsigned char*)text)[1];
            unicode = convSJIS2UTF16(index);
        }
        else{
            if ((text[0] & 0xe0) == 0xa0 || (text[0] & 0xe0) == 0xc0)
                unicode = ((unsigned char*)text)[0] - 0xa0 + 0xff60;
            else
                unicode = text[0];
        }
    }
    else if (code == CODE_GBK){
        const unsigned char c0 = (unsigned char)text[0];
        const unsigned char c1 = (unsigned char)text[1];
        if (c0 >= 0x81 && c0 <= 0xfe && c1 >= 0x40 && c1 <= 0xfe && c1 != 0x7f){
            unicode = convGBK2UTF16((unsigned short)(c0 << 8 | c1));
        }
        else{
            unicode = c0;
        }
    }
    else{
        unicode = convUTF8ToUTF16(&text);
    }

    return unicode;
}

namespace {

bool is_cjk_like(unsigned short u)
{
    /* Hanzi, CJK punctuation, full-width forms, kana: characters a Chinese
     * or Japanese script plausibly produces. */
    return (u >= 0x4e00 && u <= 0x9fff) ||
           (u >= 0x3400 && u <= 0x4dbf) ||
           (u >= 0x3000 && u <= 0x303f) ||
           (u >= 0xff00 && u <= 0xffef) ||
           (u >= 0x3040 && u <= 0x30ff);
}

bool is_kana(unsigned short u)
{
    return (u >= 0x3040 && u <= 0x30ff);
}

bool utf8_valid(const unsigned char *s, int len, bool *has_multibyte)
{
    *has_multibyte = false;
    int i = 0;
    while (i < len) {
        const unsigned char c = s[i];
        if (c < 0x80) { ++i; continue; }

        int n = 0;
        if ((c & 0xe0) == 0xc0) n = 2;
        else if ((c & 0xf0) == 0xe0) n = 3;
        else if ((c & 0xf8) == 0xf0) n = 4;
        else return false;

        if (i + n > len) return false;
        if ((s[i + 1] & 0xc0) != 0x80) return false;
        if (n == 2 && c < 0xc2) return false;  /* overlong */

        if (n >= 3) {
            if ((s[i + 2] & 0xc0) != 0x80) return false;
            if (n == 3) {
                if (c == 0xe0 && s[i + 1] < 0xa0) return false;
                if (c == 0xed && s[i + 1] >= 0xa0) return false; /* surrogate */
            }
        }
        if (n == 4) {
            if ((s[i + 3] & 0xc0) != 0x80) return false;
            if (c == 0xf0 && s[i + 1] < 0x90) return false;
            if (c > 0xf4) return false;
        }

        *has_multibyte = true;
        i += n;
    }
    return true;
}

} // namespace

int Encoding::detect(const unsigned char *data, int len)
{
    if (data == NULL || len <= 0) {
        fprintf(stderr, "ons: encoding detect: empty script, default CP932\n");
        return CODE_CP932;
    }

    if (len >= 3 && data[0] == 0xef && data[1] == 0xbb && data[2] == 0xbf) {
        fprintf(stderr, "ons: encoding detect: UTF-8 BOM\n");
        return CODE_UTF8;
    }

    bool has_multibyte = false;
    if (utf8_valid(data, len, &has_multibyte)) {
        fprintf(stderr, "ons: encoding detect: valid UTF-8%s\n",
                has_multibyte ? "" : " (ASCII only)");
        return CODE_UTF8;
    }

    long gbk_hit = 0, gbk_cjk = 0, gbk_bad = 0;
    long sjis_hit = 0, sjis_cjk = 0, sjis_bad = 0, sjis_pair_kana = 0;

    for (int i = 0; i < len; ) {
        const unsigned char c = data[i];
        if (c < 0x80) { ++i; continue; }

        if (c >= 0x81 && c <= 0xfe && i + 1 < len &&
            data[i + 1] >= 0x40 && data[i + 1] <= 0xfe && data[i + 1] != 0x7f) {
            const unsigned short u =
                convGBK2UTF16((unsigned short)(c << 8 | data[i + 1]));
            if (u) {
                ++gbk_hit;
                if (is_cjk_like(u)) ++gbk_cjk;
                i += 2;
                continue;
            }
        }
        ++gbk_bad;
        ++i;
    }

    for (int i = 0; i < len; ) {
        const unsigned char c = data[i];
        if (c < 0x80) { ++i; continue; }

        if ((c >= 0x81 && c <= 0x9f) || (c >= 0xe0 && c <= 0xfc)) {
            if (i + 1 < len && data[i + 1] >= 0x40 &&
                data[i + 1] <= 0xfc && data[i + 1] != 0x7f) {
                const unsigned short u =
                    convSJIS2UTF16((unsigned short)(c << 8 | data[i + 1]));
                if (u) {
                    ++sjis_hit;
                    if (is_cjk_like(u)) ++sjis_cjk;
                    if (is_kana(u)) ++sjis_pair_kana;
                    i += 2;
                    continue;
                }
            }
        }
        /* Single byte half-width katakana is legal in CP932; neutral. */
        if (c >= 0xa1 && c <= 0xdf) {
            ++i;
            continue;
        }
        ++sjis_bad;
        ++i;
    }

    /* Hanzi hits dominate; malformed pairs are heavily penalised.  Two byte
     * kana is a strong CP932 signal: GBK text read as CP932 pairs almost
     * never yields kana, while Japanese text yields plenty. */
    long gbk_score = gbk_cjk * 5 + gbk_hit - gbk_bad * 8;
    long sjis_score = sjis_cjk * 5 + sjis_hit - sjis_bad * 8;
    if (sjis_pair_kana * 3 >= sjis_hit)
        sjis_score += sjis_pair_kana * 3;

    const int result = (gbk_score >= sjis_score) ? CODE_GBK : CODE_CP932;

    fprintf(stderr,
            "ons: encoding detect: gbk(hit=%ld cjk=%ld bad=%ld score=%ld) "
            "sjis(hit=%ld cjk=%ld kana=%ld bad=%ld score=%ld) -> %s\n",
            gbk_hit, gbk_cjk, gbk_bad, gbk_score,
            sjis_hit, sjis_cjk, sjis_pair_kana, sjis_bad, sjis_score,
            name(result));
    return result;
}
