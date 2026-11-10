/* JSON as the standard reads and writes it. See granitec/json.h.
 *
 * The reader follows serde_json 1.0.154's SliceRead and Deserializer step for step: what it
 * accepts, where it says it stopped, and how it turns digits into a double (significand times or
 * over a power of ten from POW10, not strtod: serde_json without float_roundtrip is not always
 * correctly rounded, and GraniteC reads the same doubles it does). */
#include "granitec/json.h"

#include <inttypes.h>
#include <math.h>
#include <stdint.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>

#include "granitec/lock.h"

#pragma STDC FP_CONTRACT OFF

/* --- reading ------------------------------------------------------------------------------ */

static const double POW10[309] = {
    1e0, 1e1, 1e2, 1e3, 1e4, 1e5, 1e6, 1e7,
    1e8, 1e9, 1e10, 1e11, 1e12, 1e13, 1e14, 1e15,
    1e16, 1e17, 1e18, 1e19, 1e20, 1e21, 1e22, 1e23,
    1e24, 1e25, 1e26, 1e27, 1e28, 1e29, 1e30, 1e31,
    1e32, 1e33, 1e34, 1e35, 1e36, 1e37, 1e38, 1e39,
    1e40, 1e41, 1e42, 1e43, 1e44, 1e45, 1e46, 1e47,
    1e48, 1e49, 1e50, 1e51, 1e52, 1e53, 1e54, 1e55,
    1e56, 1e57, 1e58, 1e59, 1e60, 1e61, 1e62, 1e63,
    1e64, 1e65, 1e66, 1e67, 1e68, 1e69, 1e70, 1e71,
    1e72, 1e73, 1e74, 1e75, 1e76, 1e77, 1e78, 1e79,
    1e80, 1e81, 1e82, 1e83, 1e84, 1e85, 1e86, 1e87,
    1e88, 1e89, 1e90, 1e91, 1e92, 1e93, 1e94, 1e95,
    1e96, 1e97, 1e98, 1e99, 1e100, 1e101, 1e102, 1e103,
    1e104, 1e105, 1e106, 1e107, 1e108, 1e109, 1e110, 1e111,
    1e112, 1e113, 1e114, 1e115, 1e116, 1e117, 1e118, 1e119,
    1e120, 1e121, 1e122, 1e123, 1e124, 1e125, 1e126, 1e127,
    1e128, 1e129, 1e130, 1e131, 1e132, 1e133, 1e134, 1e135,
    1e136, 1e137, 1e138, 1e139, 1e140, 1e141, 1e142, 1e143,
    1e144, 1e145, 1e146, 1e147, 1e148, 1e149, 1e150, 1e151,
    1e152, 1e153, 1e154, 1e155, 1e156, 1e157, 1e158, 1e159,
    1e160, 1e161, 1e162, 1e163, 1e164, 1e165, 1e166, 1e167,
    1e168, 1e169, 1e170, 1e171, 1e172, 1e173, 1e174, 1e175,
    1e176, 1e177, 1e178, 1e179, 1e180, 1e181, 1e182, 1e183,
    1e184, 1e185, 1e186, 1e187, 1e188, 1e189, 1e190, 1e191,
    1e192, 1e193, 1e194, 1e195, 1e196, 1e197, 1e198, 1e199,
    1e200, 1e201, 1e202, 1e203, 1e204, 1e205, 1e206, 1e207,
    1e208, 1e209, 1e210, 1e211, 1e212, 1e213, 1e214, 1e215,
    1e216, 1e217, 1e218, 1e219, 1e220, 1e221, 1e222, 1e223,
    1e224, 1e225, 1e226, 1e227, 1e228, 1e229, 1e230, 1e231,
    1e232, 1e233, 1e234, 1e235, 1e236, 1e237, 1e238, 1e239,
    1e240, 1e241, 1e242, 1e243, 1e244, 1e245, 1e246, 1e247,
    1e248, 1e249, 1e250, 1e251, 1e252, 1e253, 1e254, 1e255,
    1e256, 1e257, 1e258, 1e259, 1e260, 1e261, 1e262, 1e263,
    1e264, 1e265, 1e266, 1e267, 1e268, 1e269, 1e270, 1e271,
    1e272, 1e273, 1e274, 1e275, 1e276, 1e277, 1e278, 1e279,
    1e280, 1e281, 1e282, 1e283, 1e284, 1e285, 1e286, 1e287,
    1e288, 1e289, 1e290, 1e291, 1e292, 1e293, 1e294, 1e295,
    1e296, 1e297, 1e298, 1e299, 1e300, 1e301, 1e302, 1e303,
    1e304, 1e305, 1e306, 1e307, 1e308,
};

typedef struct parser {
    const unsigned char *s;
    size_t len;
    size_t i;
    int depth_left;
    char *error;
} parser;

static const char *const EOF_VALUE = "EOF while parsing a value";
static const char *const EOF_LIST = "EOF while parsing a list";
static const char *const EOF_OBJECT = "EOF while parsing an object";
static const char *const EOF_STRING = "EOF while parsing a string";
static const char *const EXPECTED_COLON = "expected `:`";
static const char *const EXPECTED_LIST_COMMA_OR_END = "expected `,` or `]`";
static const char *const EXPECTED_OBJECT_COMMA_OR_END = "expected `,` or `}`";
static const char *const EXPECTED_IDENT = "expected ident";
static const char *const EXPECTED_VALUE = "expected value";
static const char *const KEY_MUST_BE_A_STRING = "key must be a string";
static const char *const INVALID_ESCAPE = "invalid escape";
static const char *const INVALID_NUMBER = "invalid number";
static const char *const NUMBER_OUT_OF_RANGE = "number out of range";
static const char *const INVALID_UNICODE = "invalid unicode code point";
static const char *const CONTROL_CHARACTER = "control character (\\u0000-\\u001F) found while parsing a string";
static const char *const LONE_SURROGATE = "lone leading surrogate in hex escape";
static const char *const END_OF_HEX_ESCAPE = "unexpected end of hex escape";
static const char *const TRAILING_COMMA = "trailing comma";
static const char *const TRAILING_CHARACTERS = "trailing characters";
static const char *const RECURSION_LIMIT = "recursion limit exceeded";

/* serde's Position of an index: the line it is on, and how many bytes of that line come before it. */
static void position_of(const parser *p, size_t index, size_t *line, size_t *column) {
    size_t start_of_line = 0, lines = 1;
    for (size_t i = 0; i < index; i++) {
        if (p->s[i] == '\n') {
            start_of_line = i + 1;
            lines++;
        }
    }
    *line = lines;
    *column = index - start_of_line;
}

static bool fail_at(parser *p, size_t line, size_t column, const char *what) {
    if (!p->error) p->error = granite_strdupf("%s at line %zu column %zu", what, line, column);
    return false;
}

/* serde's `error`: where the reader is. */
static bool fail(parser *p, const char *what) {
    size_t line, column;
    position_of(p, p->i, &line, &column);
    return fail_at(p, line, column, what);
}

/* serde's `peek_error`: one past where the reader is, at most the end. */
static bool fail_peek(parser *p, const char *what) {
    size_t line, column;
    position_of(p, p->i + 1 < p->len ? p->i + 1 : p->len, &line, &column);
    return fail_at(p, line, column, what);
}

static int peek(const parser *p) {
    return p->i < p->len ? p->s[p->i] : -1;
}

/* serde's peek_or_null: the next byte, or 0 at the end. */
static int peek_or_null(const parser *p) {
    return p->i < p->len ? p->s[p->i] : 0;
}

static int peek_after_whitespace(parser *p) {
    while (p->i < p->len) {
        unsigned char c = p->s[p->i];
        if (c != ' ' && c != '\n' && c != '\t' && c != '\r') return c;
        p->i++;
    }
    return -1;
}

static bool parse_value(parser *p, json_object **out);

static bool parse_ident(parser *p, const char *rest) {
    for (const char *c = rest; *c; c++) {
        if (p->i >= p->len) return fail(p, EOF_VALUE);
        if (p->s[p->i++] != (unsigned char)*c) return fail(p, EXPECTED_IDENT);
    }
    return true;
}

/* overflow!(a * 10 + b, c) */
#define WOULD_OVERFLOW(a, b, c) ((a) >= (c) / 10 && ((a) > (c) / 10 || (b) > (c) % 10))

static bool f64_from_parts(parser *p, bool positive, uint64_t significand, int32_t exponent, double *out) {
    double f = (double)significand;
    for (;;) {
        uint64_t magnitude = exponent < 0 ? (uint64_t)(-(int64_t)exponent) : (uint64_t)exponent;
        if (magnitude <= 308) {
            if (exponent >= 0) {
                f *= POW10[magnitude];
                if (isinf(f)) return fail(p, NUMBER_OUT_OF_RANGE);
            } else {
                f /= POW10[magnitude];
            }
            break;
        }
        if (f == 0.0) break;
        if (exponent >= 0) return fail(p, NUMBER_OUT_OF_RANGE);
        f /= 1e308;
        exponent += 308;
    }
    *out = positive ? f : -f;
    return true;
}

static int32_t saturating_add(int32_t a, int32_t b) {
    int64_t sum = (int64_t)a + (int64_t)b;
    return sum > INT32_MAX ? INT32_MAX : sum < INT32_MIN ? INT32_MIN : (int32_t)sum;
}

static bool parse_exponent(parser *p, bool positive, uint64_t significand, int32_t starting_exp, double *out) {
    p->i++; /* the e */
    bool positive_exp = true;
    int c = peek_or_null(p);
    if (c == '+') {
        p->i++;
    } else if (c == '-') {
        p->i++;
        positive_exp = false;
    }
    if (p->i >= p->len) return fail(p, EOF_VALUE);
    c = p->s[p->i++];
    if (c < '0' || c > '9') return fail(p, INVALID_NUMBER);
    int32_t exp = c - '0';
    while ((c = peek_or_null(p)) >= '0' && c <= '9') {
        p->i++;
        int32_t digit = c - '0';
        if (WOULD_OVERFLOW(exp, digit, INT32_MAX)) {
            /* serde's parse_exponent_overflow: out of range, or zero. */
            if (significand != 0 && positive_exp) return fail(p, NUMBER_OUT_OF_RANGE);
            while ((c = peek_or_null(p)) >= '0' && c <= '9') p->i++;
            *out = positive ? 0.0 : -0.0;
            return true;
        }
        exp = exp * 10 + digit;
    }
    int32_t final_exp = positive_exp ? saturating_add(starting_exp, exp) : saturating_add(starting_exp, -exp);
    return f64_from_parts(p, positive, significand, final_exp, out);
}

static bool parse_decimal(parser *p, bool positive, uint64_t significand, int32_t exponent_before, double *out) {
    p->i++; /* the point */
    int32_t exponent_after = 0;
    int c;
    while ((c = peek_or_null(p)) >= '0' && c <= '9') {
        uint64_t digit = (uint64_t)(c - '0');
        if (WOULD_OVERFLOW(significand, digit, UINT64_MAX)) {
            /* serde's parse_decimal_overflow: further digits are ignored. */
            while ((c = peek_or_null(p)) >= '0' && c <= '9') p->i++;
            int32_t exponent = exponent_before + exponent_after;
            if (c == 'e' || c == 'E') return parse_exponent(p, positive, significand, exponent, out);
            return f64_from_parts(p, positive, significand, exponent, out);
        }
        p->i++;
        significand = significand * 10 + digit;
        exponent_after -= 1;
    }
    if (exponent_after == 0) return fail_peek(p, peek(p) >= 0 ? INVALID_NUMBER : EOF_VALUE);
    int32_t exponent = exponent_before + exponent_after;
    c = peek_or_null(p);
    if (c == 'e' || c == 'E') return parse_exponent(p, positive, significand, exponent, out);
    return f64_from_parts(p, positive, significand, exponent, out);
}

static bool parse_long_integer(parser *p, bool positive, uint64_t significand, double *out) {
    int32_t exponent = 0;
    for (;;) {
        int c = peek_or_null(p);
        if (c >= '0' && c <= '9') {
            p->i++;
            exponent += 1;
        } else if (c == '.') {
            return parse_decimal(p, positive, significand, exponent, out);
        } else if (c == 'e' || c == 'E') {
            return parse_exponent(p, positive, significand, exponent, out);
        } else {
            return f64_from_parts(p, positive, significand, exponent, out);
        }
    }
}

static bool number_of_double(double value, json_object **out) {
    *out = json_object_new_double(value);
    return true;
}

/* serde's parse_number: after the integer part, a fraction or an exponent makes it a double. */
static bool parse_number(parser *p, bool positive, uint64_t significand, json_object **out) {
    int c = peek_or_null(p);
    double value;
    if (c == '.') {
        if (!parse_decimal(p, positive, significand, 0, &value)) return false;
        return number_of_double(value, out);
    }
    if (c == 'e' || c == 'E') {
        if (!parse_exponent(p, positive, significand, 0, &value)) return false;
        return number_of_double(value, out);
    }
    if (positive) {
        *out = significand <= (uint64_t)INT64_MAX ? json_object_new_int64((int64_t)significand)
                                                  : json_object_new_uint64(significand);
        return true;
    }
    /* A negative that underflows i64, and -0, are doubles. */
    int64_t negated = (int64_t)(0 - significand);
    if (negated >= 0) return number_of_double(-(double)significand, out);
    *out = json_object_new_int64(negated);
    return true;
}

static bool parse_integer(parser *p, bool positive, json_object **out) {
    if (p->i >= p->len) return fail(p, EOF_VALUE);
    int c = p->s[p->i++];
    if (c == '0') {
        int next = peek_or_null(p);
        if (next >= '0' && next <= '9') return fail_peek(p, INVALID_NUMBER);
        return parse_number(p, positive, 0, out);
    }
    if (c < '1' || c > '9') return fail(p, INVALID_NUMBER);
    uint64_t significand = (uint64_t)(c - '0');
    for (;;) {
        int next = peek_or_null(p);
        if (next < '0' || next > '9') return parse_number(p, positive, significand, out);
        uint64_t digit = (uint64_t)(next - '0');
        if (WOULD_OVERFLOW(significand, digit, UINT64_MAX)) {
            double value;
            if (!parse_long_integer(p, positive, significand, &value)) return false;
            return number_of_double(value, out);
        }
        p->i++;
        significand = significand * 10 + digit;
    }
}

/* How many leading bytes of `s` are valid UTF-8, as Rust's str::from_utf8 counts them. */
static size_t utf8_valid_up_to(const unsigned char *s, size_t n) {
    size_t i = 0;
    while (i < n) {
        unsigned char c = s[i];
        if (c < 0x80) {
            i++;
            continue;
        }
        size_t width;
        unsigned char low = 0x80, high = 0xBF;
        if (c >= 0xC2 && c <= 0xDF) {
            width = 2;
        } else if (c >= 0xE0 && c <= 0xEF) {
            width = 3;
            if (c == 0xE0) low = 0xA0;
            if (c == 0xED) high = 0x9F;
        } else if (c >= 0xF0 && c <= 0xF4) {
            width = 4;
            if (c == 0xF0) low = 0x90;
            if (c == 0xF4) high = 0x8F;
        } else {
            return i;
        }
        if (i + 1 >= n || s[i + 1] < low || s[i + 1] > high) return i;
        for (size_t k = 2; k < width; k++)
            if (i + k >= n || s[i + k] < 0x80 || s[i + k] > 0xBF) return i;
        i += width;
    }
    return n;
}

static void push_codepoint(granite_buf *out, uint32_t c) {
    char bytes[4];
    size_t n;
    if (c < 0x80) {
        bytes[0] = (char)c;
        n = 1;
    } else if (c < 0x800) {
        bytes[0] = (char)(0xC0 | (c >> 6));
        bytes[1] = (char)(0x80 | (c & 0x3F));
        n = 2;
    } else if (c < 0x10000) {
        bytes[0] = (char)(0xE0 | (c >> 12));
        bytes[1] = (char)(0x80 | ((c >> 6) & 0x3F));
        bytes[2] = (char)(0x80 | (c & 0x3F));
        n = 3;
    } else {
        bytes[0] = (char)(0xF0 | (c >> 18));
        bytes[1] = (char)(0x80 | ((c >> 12) & 0x3F));
        bytes[2] = (char)(0x80 | ((c >> 6) & 0x3F));
        bytes[3] = (char)(0x80 | (c & 0x3F));
        n = 4;
    }
    granite_buf_append(out, bytes, n);
}

static int hex_value(unsigned char c) {
    if (c >= '0' && c <= '9') return c - '0';
    if (c >= 'a' && c <= 'f') return c - 'a' + 10;
    if (c >= 'A' && c <= 'F') return c - 'A' + 10;
    return -1;
}

static bool decode_hex_escape(parser *p, uint32_t *out) {
    if (p->len - p->i < 4) {
        p->i = p->len;
        return fail(p, EOF_STRING);
    }
    const unsigned char *h = p->s + p->i;
    p->i += 4;
    int a = hex_value(h[0]), b = hex_value(h[1]), c = hex_value(h[2]), d = hex_value(h[3]);
    if (a < 0 || b < 0 || c < 0 || d < 0) return fail(p, INVALID_ESCAPE);
    *out = (uint32_t)(a << 12 | b << 8 | c << 4 | d);
    return true;
}

static bool parse_unicode_escape(parser *p, granite_buf *out) {
    uint32_t n;
    if (!decode_hex_escape(p, &n)) return false;
    if (n >= 0xDC00 && n <= 0xDFFF) return fail(p, LONE_SURROGATE);
    if (n < 0xD800 || n > 0xDBFF) {
        push_codepoint(out, n);
        return true;
    }
    if (p->i >= p->len) return fail(p, EOF_STRING);
    if (p->s[p->i] != '\\') {
        p->i++;
        return fail(p, END_OF_HEX_ESCAPE);
    }
    p->i++;
    if (p->i >= p->len) return fail(p, EOF_STRING);
    if (p->s[p->i] != 'u') {
        p->i++;
        return fail(p, END_OF_HEX_ESCAPE);
    }
    p->i++;
    uint32_t n2;
    if (!decode_hex_escape(p, &n2)) return false;
    if (n2 < 0xDC00 || n2 > 0xDFFF) return fail(p, LONE_SURROGATE);
    push_codepoint(out, (((n - 0xD800) << 10) | (n2 - 0xDC00)) + 0x10000);
    return true;
}

static bool parse_escape(parser *p, granite_buf *out) {
    if (p->i >= p->len) return fail(p, EOF_STRING);
    unsigned char c = p->s[p->i++];
    switch (c) {
    case '"': granite_buf_append_char(out, '"'); return true;
    case '\\': granite_buf_append_char(out, '\\'); return true;
    case '/': granite_buf_append_char(out, '/'); return true;
    case 'b': granite_buf_append_char(out, '\b'); return true;
    case 'f': granite_buf_append_char(out, '\f'); return true;
    case 'n': granite_buf_append_char(out, '\n'); return true;
    case 'r': granite_buf_append_char(out, '\r'); return true;
    case 't': granite_buf_append_char(out, '\t'); return true;
    case 'u': return parse_unicode_escape(p, out);
    default: return fail(p, INVALID_ESCAPE);
    }
}

/* After the opening quote: the string's bytes into `out`, validated as UTF-8 at its end. */
static bool parse_string(parser *p, granite_buf *out) {
    for (;;) {
        size_t start = p->i;
        while (p->i < p->len && p->s[p->i] != '"' && p->s[p->i] != '\\' && p->s[p->i] >= 0x20) p->i++;
        granite_buf_append(out, p->s + start, p->i - start);
        if (p->i == p->len) return fail(p, EOF_STRING);
        unsigned char c = p->s[p->i];
        if (c == '"') {
            p->i++;
            size_t valid = utf8_valid_up_to((const unsigned char *)granite_buf_str(out), out->len);
            if (valid < out->len) {
                size_t line, column;
                position_of(p, p->i, &line, &column);
                size_t back = out->len - valid;
                return fail_at(p, line, column > back ? column - back : 0, INVALID_UNICODE);
            }
            return true;
        }
        if (c == '\\') {
            p->i++;
            if (!parse_escape(p, out)) return false;
            continue;
        }
        p->i++;
        return fail(p, CONTROL_CHARACTER);
    }
}

static bool parse_array(parser *p, json_object **out) {
    json_object *array = json_object_new_array();
    bool first = true;
    for (;;) {
        int c = peek_after_whitespace(p);
        if (c == ']') break;
        if (c == ',' && !first) {
            p->i++;
            c = peek_after_whitespace(p);
        } else if (c >= 0) {
            if (first) {
                first = false;
            } else {
                fail_peek(p, EXPECTED_LIST_COMMA_OR_END);
                goto failed;
            }
        } else {
            fail_peek(p, EOF_LIST);
            goto failed;
        }
        if (c == ']' || c < 0) {
            fail_peek(p, c == ']' ? TRAILING_COMMA : EOF_VALUE);
            goto failed;
        }
        json_object *element = NULL;
        if (!parse_value(p, &element)) goto failed;
        json_object_array_add(array, element);
    }
    p->i++; /* the ] */
    *out = array;
    return true;
failed:
    json_object_put(array);
    return false;
}

static bool parse_object(parser *p, json_object **out) {
    json_object *object = json_object_new_object();
    granite_buf key = {0};
    bool first = true;
    for (;;) {
        int c = peek_after_whitespace(p);
        if (c == '}') break;
        if (c == ',' && !first) {
            p->i++;
            c = peek_after_whitespace(p);
        } else if (c >= 0) {
            if (first) {
                first = false;
            } else {
                fail_peek(p, EXPECTED_OBJECT_COMMA_OR_END);
                goto failed;
            }
        } else {
            fail_peek(p, EOF_OBJECT);
            goto failed;
        }
        if (c != '"') {
            fail_peek(p, c == '}' ? TRAILING_COMMA : c < 0 ? EOF_VALUE : KEY_MUST_BE_A_STRING);
            goto failed;
        }
        p->i++;
        granite_buf_reset(&key);
        if (!parse_string(p, &key)) goto failed;
        c = peek_after_whitespace(p);
        if (c != ':') {
            fail_peek(p, c >= 0 ? EXPECTED_COLON : EOF_OBJECT);
            goto failed;
        }
        p->i++;
        json_object *member = NULL;
        if (!parse_value(p, &member)) goto failed;
        /* A key said twice keeps the last value, as serde's map does. */
        json_object_object_add(object, granite_buf_str(&key), member);
    }
    p->i++; /* the } */
    granite_buf_free(&key);
    *out = object;
    return true;
failed:
    granite_buf_free(&key);
    json_object_put(object);
    return false;
}

static bool parse_value(parser *p, json_object **out) {
    *out = NULL;
    int c = peek_after_whitespace(p);
    switch (c) {
    case -1: return fail_peek(p, EOF_VALUE);
    case 'n': p->i++; return parse_ident(p, "ull");
    case 't':
        p->i++;
        if (!parse_ident(p, "rue")) return false;
        *out = json_object_new_boolean(1);
        return true;
    case 'f':
        p->i++;
        if (!parse_ident(p, "alse")) return false;
        *out = json_object_new_boolean(0);
        return true;
    case '-': p->i++; return parse_integer(p, false, out);
    case '0': case '1': case '2': case '3': case '4': case '5': case '6': case '7': case '8': case '9':
        return parse_integer(p, true, out);
    case '"': {
        p->i++;
        granite_buf text = {0};
        bool ok = parse_string(p, &text);
        if (ok) *out = json_object_new_string_len(granite_buf_str(&text), (int)text.len);
        granite_buf_free(&text);
        return ok;
    }
    case '[':
    case '{': {
        if (--p->depth_left == 0) return fail_peek(p, RECURSION_LIMIT);
        p->i++;
        bool ok = c == '[' ? parse_array(p, out) : parse_object(p, out);
        p->depth_left++;
        return ok;
    }
    default: return fail_peek(p, EXPECTED_VALUE);
    }
}

bool granite_json_read(const char *bytes, size_t len, json_object **value, char **error) {
    parser p = {(const unsigned char *)bytes, len, 0, 128, NULL};
    json_object *read = NULL;
    *value = NULL;
    if (error) *error = NULL;
    if (parse_value(&p, &read)) {
        if (peek_after_whitespace(&p) < 0) {
            *value = read;
            return true;
        }
        fail_peek(&p, TRAILING_CHARACTERS);
        json_object_put(read);
    }
    if (error) *error = p.error;
    else free(p.error);
    return false;
}

/* --- writing ------------------------------------------------------------------------------ */

void granite_json_append_string(granite_buf *buf, const char *text, size_t n) {
    static const char hex[] = "0123456789abcdef";
    granite_buf_append_char(buf, '"');
    size_t start = 0;
    for (size_t i = 0; i < n; i++) {
        unsigned char c = (unsigned char)text[i];
        const char *escape = NULL;
        char unicode[7];
        switch (c) {
        case '"': escape = "\\\""; break;
        case '\\': escape = "\\\\"; break;
        case '\b': escape = "\\b"; break;
        case '\f': escape = "\\f"; break;
        case '\n': escape = "\\n"; break;
        case '\r': escape = "\\r"; break;
        case '\t': escape = "\\t"; break;
        default:
            if (c < 0x20) {
                memcpy(unicode, "\\u00", 4);
                unicode[4] = hex[c >> 4];
                unicode[5] = hex[c & 0xF];
                unicode[6] = '\0';
                escape = unicode;
            }
        }
        if (escape) {
            granite_buf_append(buf, text + start, i - start);
            granite_buf_append_str(buf, escape);
            start = i + 1;
        }
    }
    granite_buf_append(buf, text + start, n - start);
    granite_buf_append_char(buf, '"');
}

/* The shortest digits that read back as `value` (positive, finite, not zero), and the decimal
 * exponent of the first: value = d.ddd x 10^exponent. */
static size_t shortest_digits(double value, char digits[24], int *exponent) {
    char text[40];
    for (int precision = 1; precision <= 17; precision++) {
        snprintf(text, sizeof text, "%.*e", precision - 1, value);
        if (strtod(text, NULL) == value) break;
    }
    size_t n = 0;
    const char *c = text;
    for (; *c && *c != 'e'; c++)
        if (*c >= '0' && *c <= '9') digits[n++] = *c;
    *exponent = *c == 'e' ? atoi(c + 1) : 0;
    while (n > 1 && digits[n - 1] == '0') n--;
    digits[n] = '\0';
    return n;
}

void granite_json_append_double(granite_buf *buf, double value) {
    if (!isfinite(value)) {
        granite_buf_append_str(buf, "null");
        return;
    }
    if (signbit(value)) granite_buf_append_char(buf, '-');
    double magnitude = fabs(value);
    if (magnitude == 0.0) {
        granite_buf_append_str(buf, "0.0");
        return;
    }
    char digits[24];
    int exponent;
    size_t length = shortest_digits(magnitude, digits, &exponent);
    if (exponent >= -5 && exponent <= 15) {
        if ((int)length - 1 <= exponent) {
            /* 1234e7 -> 12340000000.0 */
            granite_buf_append(buf, digits, length);
            for (int i = (int)length; i <= exponent; i++) granite_buf_append_char(buf, '0');
            granite_buf_append_str(buf, ".0");
        } else if (exponent >= 0) {
            /* 1234e-2 -> 12.34 */
            granite_buf_append(buf, digits, (size_t)exponent + 1);
            granite_buf_append_char(buf, '.');
            granite_buf_append(buf, digits + exponent + 1, length - (size_t)exponent - 1);
        } else {
            /* 1234e-6 -> 0.001234 */
            granite_buf_append_str(buf, "0.");
            for (int i = 1; i < -exponent; i++) granite_buf_append_char(buf, '0');
            granite_buf_append(buf, digits, length);
        }
        return;
    }
    /* 1234e30 -> 1.234e+33 */
    granite_buf_append_char(buf, digits[0]);
    if (length > 1) {
        granite_buf_append_char(buf, '.');
        granite_buf_append(buf, digits + 1, length - 1);
    }
    granite_buf_appendf(buf, "e%c%d", exponent < 0 ? '-' : '+', exponent < 0 ? -exponent : exponent);
}

static int compare_keys(const void *a, const void *b) {
    return strcmp(*(const char *const *)a, *(const char *const *)b);
}

/* The object's keys, sorted bytewise (Rust's String order). The caller frees the array. */
static const char **sorted_keys(const json_object *object, size_t *count) {
    size_t n = (size_t)json_object_object_length((json_object *)object);
    const char **keys = granite_malloc((n ? n : 1) * sizeof *keys);
    size_t i = 0;
    json_object_object_foreach((json_object *)object, key, unused) {
        (void)unused;
        keys[i++] = key;
    }
    qsort(keys, i, sizeof *keys, compare_keys);
    *count = i;
    return keys;
}

static void append_integer(granite_buf *buf, const json_object *value) {
    int64_t i = json_object_get_int64((json_object *)value);
    if (i == INT64_MAX) granite_buf_appendf(buf, "%" PRIu64, json_object_get_uint64((json_object *)value));
    else granite_buf_appendf(buf, "%" PRId64, i);
}

static void indent(granite_buf *buf, int depth) {
    for (int i = 0; i < depth; i++) granite_buf_append_str(buf, "  ");
}

static void append_value(granite_buf *buf, const json_object *value, int pretty, int depth) {
    switch (json_object_get_type((json_object *)value)) {
    case json_type_null: granite_buf_append_str(buf, "null"); return;
    case json_type_boolean:
        granite_buf_append_str(buf, json_object_get_boolean((json_object *)value) ? "true" : "false");
        return;
    case json_type_int: append_integer(buf, value); return;
    case json_type_double: granite_json_append_double(buf, json_object_get_double((json_object *)value)); return;
    case json_type_string:
        granite_json_append_string(buf, json_object_get_string((json_object *)value),
                                   (size_t)json_object_get_string_len((json_object *)value));
        return;
    case json_type_array: {
        size_t n = json_object_array_length((json_object *)value);
        granite_buf_append_char(buf, '[');
        for (size_t i = 0; i < n; i++) {
            if (i) granite_buf_append_char(buf, ',');
            if (pretty) {
                granite_buf_append_char(buf, '\n');
                indent(buf, depth + 1);
            }
            append_value(buf, json_object_array_get_idx((json_object *)value, i), pretty, depth + 1);
        }
        if (pretty && n) {
            granite_buf_append_char(buf, '\n');
            indent(buf, depth);
        }
        granite_buf_append_char(buf, ']');
        return;
    }
    case json_type_object: {
        size_t n;
        const char **keys = sorted_keys(value, &n);
        granite_buf_append_char(buf, '{');
        for (size_t i = 0; i < n; i++) {
            if (i) granite_buf_append_char(buf, ',');
            if (pretty) {
                granite_buf_append_char(buf, '\n');
                indent(buf, depth + 1);
            }
            granite_json_append_string(buf, keys[i], strlen(keys[i]));
            granite_buf_append_str(buf, pretty ? ": " : ":");
            json_object *member = NULL;
            json_object_object_get_ex((json_object *)value, keys[i], &member);
            append_value(buf, member, pretty, depth + 1);
        }
        if (pretty && n) {
            granite_buf_append_char(buf, '\n');
            indent(buf, depth);
        }
        granite_buf_append_char(buf, '}');
        free(keys);
        return;
    }
    }
}

void granite_json_append(granite_buf *buf, const json_object *value) {
    append_value(buf, value, 0, 0);
}

char *granite_json_write(const json_object *value, size_t *len) {
    granite_buf buf = {0};
    append_value(&buf, value, 0, 0);
    if (len) *len = buf.len;
    return granite_buf_take(&buf);
}

char *granite_json_write_pretty(const json_object *value, size_t *len) {
    granite_buf buf = {0};
    append_value(&buf, value, 1, 0);
    if (len) *len = buf.len;
    return granite_buf_take(&buf);
}

/* --- values ------------------------------------------------------------------------------- */

void granite_json_put(json_object *value) {
    if (!value) return;
    granite_lock_check("freed a JSON value");
    json_object_put(value);
}

json_object *granite_json_number(double value) {
    if (isfinite(value) && value == trunc(value) && fabs(value) < 9007199254740992.0)
        return json_object_new_int64((int64_t)value);
    return json_object_new_double(value);
}

json_object *granite_json_copy(const json_object *value) {
    if (!value) return NULL;
    json_object *copy = NULL;
    if (json_object_deep_copy((json_object *)value, &copy, NULL) != 0) {
        fprintf(stderr, "granite: a JSON value could not be copied\n");
        abort();
    }
    return copy;
}

bool granite_json_is_integer(const json_object *value) {
    return json_object_get_type((json_object *)value) == json_type_int;
}

bool granite_json_is_number(const json_object *value) {
    json_type type = json_object_get_type((json_object *)value);
    return type == json_type_int || type == json_type_double;
}

static bool numbers_equal(const json_object *a, const json_object *b) {
    if (granite_json_is_integer(a) && granite_json_is_integer(b)) {
        int64_t x = json_object_get_int64((json_object *)a), y = json_object_get_int64((json_object *)b);
        if (x == INT64_MAX || y == INT64_MAX)
            return x == y && json_object_get_uint64((json_object *)a) == json_object_get_uint64((json_object *)b);
        return x == y;
    }
    return json_object_get_double((json_object *)a) == json_object_get_double((json_object *)b);
}

bool granite_json_equal(const json_object *a, const json_object *b) {
    json_type ta = json_object_get_type((json_object *)a), tb = json_object_get_type((json_object *)b);
    if (granite_json_is_number(a) && granite_json_is_number(b)) return numbers_equal(a, b);
    if (ta != tb) return false;
    switch (ta) {
    case json_type_null: return true;
    case json_type_boolean:
        return !json_object_get_boolean((json_object *)a) == !json_object_get_boolean((json_object *)b);
    case json_type_string:
        return json_object_get_string_len((json_object *)a) == json_object_get_string_len((json_object *)b) &&
               memcmp(json_object_get_string((json_object *)a), json_object_get_string((json_object *)b),
                      (size_t)json_object_get_string_len((json_object *)a)) == 0;
    case json_type_array: {
        size_t n = json_object_array_length((json_object *)a);
        if (n != json_object_array_length((json_object *)b)) return false;
        for (size_t i = 0; i < n; i++)
            if (!granite_json_equal(json_object_array_get_idx((json_object *)a, i),
                                    json_object_array_get_idx((json_object *)b, i)))
                return false;
        return true;
    }
    case json_type_object: {
        if (json_object_object_length((json_object *)a) != json_object_object_length((json_object *)b)) return false;
        json_object_object_foreach((json_object *)a, key, member) {
            json_object *other = NULL;
            if (!json_object_object_get_ex((json_object *)b, key, &other)) return false;
            if (!granite_json_equal(member, other)) return false;
        }
        return true;
    }
    default: return false;
    }
}

json_object *granite_json_get(const json_object *object, const char *key) {
    json_object *value = NULL;
    if (json_object_get_type((json_object *)object) != json_type_object) return NULL;
    json_object_object_get_ex((json_object *)object, key, &value);
    return value;
}

bool granite_json_has(const json_object *object, const char *key) {
    if (json_object_get_type((json_object *)object) != json_type_object) return false;
    return json_object_object_get_ex((json_object *)object, key, NULL);
}

const char *granite_json_get_string(const json_object *object, const char *key) {
    json_object *value = granite_json_get(object, key);
    return json_object_get_type(value) == json_type_string ? json_object_get_string(value) : NULL;
}

void granite_json_set(json_object *object, const char *key, json_object *value) {
    json_object_object_add(object, key, value);
}

void granite_json_set_string(json_object *object, const char *key, const char *text) {
    if (text) json_object_object_add(object, key, json_object_new_string(text));
}
