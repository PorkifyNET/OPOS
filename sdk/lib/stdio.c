/* OPOS SDK C library: printf and friends (see include/stdio.h). */
#include <stdarg.h>
#include <stdbool.h>
#include <stdint.h>
#include <stdio.h>
#include <string.h>

#include <opos.h>

/* Output goes either into a buffer (snprintf) or, a chunk at a time, to a
   file descriptor (printf). */
struct sink {
    char *buffer;
    size_t size;
    size_t length; /* characters produced so far */
    int fd;
    char chunk[256];
    size_t chunk_length;
};

static void sink_flush(struct sink *sink) {
    size_t done = 0;
    while (done < sink->chunk_length) {
        long written = opos_write(sink->fd, sink->chunk + done,
                                  sink->chunk_length - done);
        if (written <= 0) {
            break;
        }
        done += (size_t)written;
    }
    sink->chunk_length = 0;
}

static void put(struct sink *sink, char character) {
    if (sink->buffer != NULL) {
        if (sink->length + 1 < sink->size) {
            sink->buffer[sink->length] = character;
        }
    } else {
        sink->chunk[sink->chunk_length++] = character;
        if (sink->chunk_length == sizeof(sink->chunk)) {
            sink_flush(sink);
        }
    }
    sink->length++;
}

static void put_repeated(struct sink *sink, char character, int count) {
    for (int i = 0; i < count; i++) {
        put(sink, character);
    }
}

struct spec {
    bool left;
    bool zero;
    bool plus;
    bool space;
    int width;
    int precision; /* -1: none */
};

static void put_padded(struct sink *sink, const struct spec *spec,
                       const char *prefix, const char *digits,
                       size_t digit_count) {
    size_t prefix_length = strlen(prefix);
    int zeros = spec->precision > (int)digit_count
        ? spec->precision - (int)digit_count : 0;
    int total = (int)(prefix_length + digit_count) + zeros;
    int padding = spec->width > total ? spec->width - total : 0;
    if (spec->zero && !spec->left && spec->precision < 0) {
        zeros += padding;
        padding = 0;
    }
    if (!spec->left) {
        put_repeated(sink, ' ', padding);
    }
    for (size_t i = 0; i < prefix_length; i++) {
        put(sink, prefix[i]);
    }
    put_repeated(sink, '0', zeros);
    for (size_t i = 0; i < digit_count; i++) {
        put(sink, digits[i]);
    }
    if (spec->left) {
        put_repeated(sink, ' ', padding);
    }
}

static void put_number(struct sink *sink, const struct spec *spec,
                       unsigned long long value, bool negative,
                       unsigned base, bool upper, const char *prefix) {
    char digits[24];
    size_t count = 0;
    const char *set = upper ? "0123456789ABCDEF" : "0123456789abcdef";
    if (value != 0 || spec->precision != 0) {
        do {
            digits[sizeof(digits) - 1 - count++] = set[value % base];
            value /= base;
        } while (value != 0);
    }
    const char *sign = negative ? "-" : spec->plus ? "+"
                     : spec->space ? " " : "";
    char full_prefix[4];
    size_t used = 0;
    for (const char *p = sign; *p != '\0'; p++) {
        full_prefix[used++] = *p;
    }
    for (const char *p = prefix; *p != '\0'; p++) {
        full_prefix[used++] = *p;
    }
    full_prefix[used] = '\0';
    put_padded(sink, spec, full_prefix, digits + sizeof(digits) - count,
               count);
}

/* Fixed notation; digits come from repeated multiplication, so the last
   ones may be off. Huge values print as "1e+N". */
static void put_double(struct sink *sink, const struct spec *spec,
                       double value) {
    char digits[64];
    size_t count = 0;
    bool negative = value < 0 || (value == 0 && 1 / value < 0);
    if (negative) {
        value = -value;
    }
    const char *sign = negative ? "-" : spec->plus ? "+"
                     : spec->space ? " " : "";
    struct spec plain = *spec;
    plain.precision = -1;
    if (value != value) {
        put_padded(sink, &plain, sign, "nan", 3);
        return;
    }
    if (value > 1.7976931348623157e308) {
        put_padded(sink, &plain, sign, "inf", 3);
        return;
    }
    if (value > 1e18) {
        int exponent = 0;
        while (value >= 10) {
            value /= 10;
            exponent++;
        }
        digits[count++] = (char)('0' + (int)value);
        digits[count++] = 'e';
        digits[count++] = '+';
        char tmp[8];
        int n = 0;
        do {
            tmp[n++] = (char)('0' + exponent % 10);
            exponent /= 10;
        } while (exponent != 0 && n < 8);
        while (n > 0 && count < sizeof(digits)) {
            digits[count++] = tmp[--n];
        }
        put_padded(sink, &plain, sign, digits, count);
        return;
    }
    int precision = spec->precision < 0 ? 6 : spec->precision;
    if (precision > 30) {
        precision = 30;
    }
    double rounding = 0.5;
    for (int i = 0; i < precision; i++) {
        rounding /= 10;
    }
    value += rounding;
    unsigned long long whole = (unsigned long long)value;
    double fraction = value - (double)whole;
    char tmp[24];
    int n = 0;
    do {
        tmp[n++] = (char)('0' + whole % 10);
        whole /= 10;
    } while (whole != 0);
    while (n > 0) {
        digits[count++] = tmp[--n];
    }
    if (precision > 0) {
        digits[count++] = '.';
        for (int i = 0; i < precision; i++) {
            fraction *= 10;
            int digit = (int)fraction;
            if (digit > 9) {
                digit = 9;
            }
            digits[count++] = (char)('0' + digit);
            fraction -= digit;
        }
    }
    put_padded(sink, &plain, sign, digits, count);
}

static void format(struct sink *sink, const char *text, va_list arguments) {
    for (; *text != '\0'; text++) {
        if (*text != '%') {
            put(sink, *text);
            continue;
        }
        text++;
        struct spec spec = {.precision = -1};
        for (;; text++) {
            if (*text == '-') {
                spec.left = true;
            } else if (*text == '0') {
                spec.zero = true;
            } else if (*text == '+') {
                spec.plus = true;
            } else if (*text == ' ') {
                spec.space = true;
            } else if (*text != '#') {
                break;
            }
        }
        if (*text == '*') {
            spec.width = va_arg(arguments, int);
            if (spec.width < 0) {
                spec.left = true;
                spec.width = -spec.width;
            }
            text++;
        } else {
            while (*text >= '0' && *text <= '9') {
                spec.width = spec.width * 10 + (*text++ - '0');
            }
        }
        if (*text == '.') {
            text++;
            spec.precision = 0;
            if (*text == '*') {
                spec.precision = va_arg(arguments, int);
                text++;
            } else {
                while (*text >= '0' && *text <= '9') {
                    spec.precision = spec.precision * 10 + (*text++ - '0');
                }
            }
        }
        int size = 0; /* 0 int, 1 long, 2 long long, -1 short, -2 char */
        for (;; text++) {
            if (*text == 'l') {
                size++;
            } else if (*text == 'h') {
                size--;
            } else if (*text == 'z' || *text == 'j' || *text == 't') {
                size = 2;
            } else {
                break;
            }
        }
        switch (*text) {
        case 'd':
        case 'i': {
            long long value = size >= 2 ? va_arg(arguments, long long)
                            : size == 1 ? va_arg(arguments, long)
                                        : va_arg(arguments, int);
            if (size == -1) {
                value = (short)value;
            } else if (size <= -2) {
                value = (signed char)value;
            }
            unsigned long long magnitude = value < 0
                ? 0ULL - (unsigned long long)value
                : (unsigned long long)value;
            put_number(sink, &spec, magnitude, value < 0, 10, false, "");
            break;
        }
        case 'u':
        case 'x':
        case 'X':
        case 'o': {
            unsigned long long value = size >= 2
                ? va_arg(arguments, unsigned long long)
                : size == 1 ? va_arg(arguments, unsigned long)
                            : va_arg(arguments, unsigned int);
            if (size == -1) {
                value = (unsigned short)value;
            } else if (size <= -2) {
                value = (unsigned char)value;
            }
            unsigned base = *text == 'u' ? 10 : *text == 'o' ? 8 : 16;
            put_number(sink, &spec, value, false, base, *text == 'X', "");
            break;
        }
        case 'p': {
            uintptr_t value = (uintptr_t)va_arg(arguments, void *);
            put_number(sink, &spec, value, false, 16, false, "0x");
            break;
        }
        case 'c': {
            char character = (char)va_arg(arguments, int);
            spec.precision = -1;
            spec.zero = false;
            put_padded(sink, &spec, "", &character, 1);
            break;
        }
        case 's': {
            const char *string = va_arg(arguments, const char *);
            if (string == NULL) {
                string = "(null)";
            }
            size_t length = spec.precision >= 0
                ? strnlen(string, (size_t)spec.precision) : strlen(string);
            spec.precision = -1;
            spec.zero = false;
            put_padded(sink, &spec, "", string, length);
            break;
        }
        case 'f':
        case 'F':
        case 'e':
        case 'E':
        case 'g':
        case 'G':
        case 'a':
        case 'A':
            put_double(sink, &spec, va_arg(arguments, double));
            break;
        case '%':
            put(sink, '%');
            break;
        case '\0':
            return;
        default:
            put(sink, '%');
            put(sink, *text);
            break;
        }
    }
}

int vsnprintf(char *buffer, size_t size, const char *text,
              va_list arguments) {
    static char dummy;
    struct sink sink = {.buffer = size != 0 ? buffer : &dummy, .size = size};
    format(&sink, text, arguments);
    if (size != 0) {
        buffer[sink.length < size ? sink.length : size - 1] = '\0';
    }
    return (int)sink.length;
}

int snprintf(char *buffer, size_t size, const char *text, ...) {
    va_list arguments;
    va_start(arguments, text);
    int length = vsnprintf(buffer, size, text, arguments);
    va_end(arguments);
    return length;
}

int sprintf(char *buffer, const char *text, ...) {
    va_list arguments;
    va_start(arguments, text);
    int length = vsnprintf(buffer, (size_t)1 << 30, text, arguments);
    va_end(arguments);
    return length;
}

static int print_to(int fd, const char *text, va_list arguments) {
    struct sink sink = {.fd = fd};
    format(&sink, text, arguments);
    sink_flush(&sink);
    return (int)sink.length;
}

int vprintf(const char *text, va_list arguments) {
    return print_to(OPOS_STDOUT, text, arguments);
}

int printf(const char *text, ...) {
    va_list arguments;
    va_start(arguments, text);
    int length = print_to(OPOS_STDOUT, text, arguments);
    va_end(arguments);
    return length;
}

int eprintf(const char *text, ...) {
    va_list arguments;
    va_start(arguments, text);
    int length = print_to(OPOS_STDERR, text, arguments);
    va_end(arguments);
    return length;
}

int puts(const char *text) {
    printf("%s\n", text);
    return 0;
}

int putchar(int character) {
    char c = (char)character;
    opos_write(OPOS_STDOUT, &c, 1);
    return (unsigned char)c;
}

/* FILE streams: only standard output and standard error. */
struct opos_file {
    int fd;
};

static struct opos_file standard_output = {OPOS_STDOUT};
static struct opos_file standard_error = {OPOS_STDERR};
FILE *stdout = &standard_output;
FILE *stderr = &standard_error;

int vfprintf(FILE *stream, const char *text, va_list arguments) {
    return print_to(stream->fd, text, arguments);
}

int fprintf(FILE *stream, const char *text, ...) {
    va_list arguments;
    va_start(arguments, text);
    int length = print_to(stream->fd, text, arguments);
    va_end(arguments);
    return length;
}

size_t fwrite(const void *data, size_t size, size_t count, FILE *stream) {
    size_t total = size * count;
    size_t done = 0;
    while (done < total) {
        size_t chunk = total - done > 256 ? 256 : total - done;
        long written = opos_write(stream->fd, (const char *)data + done,
                                  chunk);
        if (written <= 0) {
            break;
        }
        done += (size_t)written;
    }
    return size != 0 ? done / size : 0;
}

int fputc(int character, FILE *stream) {
    char c = (char)character;
    return fwrite(&c, 1, 1, stream) == 1 ? (unsigned char)c : EOF;
}

int putc(int character, FILE *stream) {
    return fputc(character, stream);
}

int fputs(const char *text, FILE *stream) {
    size_t length = strlen(text);
    return fwrite(text, 1, length, stream) == length ? 0 : EOF;
}

int fflush(FILE *stream) {
    (void)stream;
    return 0;
}
