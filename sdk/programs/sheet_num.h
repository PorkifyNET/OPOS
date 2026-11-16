/*
 * Spreadsheet: numbers, dates and number formats.
 *
 * The SDK's printf has no floating point, so numbers are read and written
 * here: 15 significant digits, like other spreadsheets. Dates are serial
 * numbers in the 1900 system (1 = 1900-01-01, with the 1900-02-29 that
 * other spreadsheets keep). Number format codes are the Office ones
 * ("#,##0.00", "0%", "yyyy-mm-dd", "[Red]-0.0", ...).
 */
#ifndef SHEET_NUM_H
#define SHEET_NUM_H

static double p10(int e) {
    static const double table[23] = {
        1e0, 1e1, 1e2, 1e3, 1e4, 1e5, 1e6, 1e7, 1e8, 1e9, 1e10, 1e11,
        1e12, 1e13, 1e14, 1e15, 1e16, 1e17, 1e18, 1e19, 1e20, 1e21, 1e22,
    };
    if (e >= 0 && e <= 22) {
        return table[e];
    }
    if (e < 0 && e >= -22) {
        return 1.0 / table[-e];
    }
    return pow(10.0, e);
}

static bool num_finite(double x) {
    return x == x && x - x == 0.0;
}

/* Reads [+-]digits[.digits][e[+-]digits] at s; returns the characters
   used (0 if there is no number). */
static size_t num_scan(const char *s, size_t n, double *out) {
    size_t i = 0;
    bool negative = false;
    if (i < n && (s[i] == '+' || s[i] == '-')) {
        negative = s[i] == '-';
        i++;
    }
    uint64_t mantissa = 0;
    int digits = 0, exponent = 0;
    bool any = false;
    while (i < n && isdigit((unsigned char)s[i])) {
        any = true;
        if (digits < 19) {
            mantissa = mantissa * 10 + (uint64_t)(s[i] - '0');
            if (mantissa != 0) {
                digits++;
            }
        } else {
            exponent++;
        }
        i++;
    }
    if (i < n && s[i] == '.') {
        size_t dot = i++;
        while (i < n && isdigit((unsigned char)s[i])) {
            any = true;
            if (digits < 19) {
                mantissa = mantissa * 10 + (uint64_t)(s[i] - '0');
                if (mantissa != 0) {
                    digits++;
                }
                exponent--;
            }
            i++;
        }
        if (!any) {
            i = dot;
        }
    }
    if (!any) {
        return 0;
    }
    if (i < n && (s[i] == 'e' || s[i] == 'E')) {
        size_t k = i + 1;
        bool eneg = false;
        if (k < n && (s[k] == '+' || s[k] == '-')) {
            eneg = s[k] == '-';
            k++;
        }
        if (k < n && isdigit((unsigned char)s[k])) {
            int e = 0;
            while (k < n && isdigit((unsigned char)s[k])) {
                if (e < 10000) {
                    e = e * 10 + (s[k] - '0');
                }
                k++;
            }
            exponent += eneg ? -e : e;
            i = k;
        }
    }
    double value = (double)mantissa;
    if (mantissa != 0) {
        if (exponent < 0) {
            int e = -exponent;
            while (e > 22) {
                value /= 1e22;
                e -= 22;
            }
            value /= p10(e);
        } else if (exponent > 0) {
            value *= p10(exponent);
        }
    }
    *out = negative ? -value : value;
    return i;
}

/* x > 0 as 15 significant digits: x ~ 0.d[0]d[1]... * 10^(e + 1), so
   digit k has the place value 10^(e - k). */
static void num_digits15(double x, char d[16], int *e_out) {
    int e = (int)floor(log10(x));
    double scaled = x * p10(14 - e);
    if (14 - e > 308) { /* tiny numbers: avoid an infinite factor */
        scaled = x * p10(300) * p10(14 - e - 300);
    }
    uint64_t v = (uint64_t)(scaled + 0.5);
    if (v >= 1000000000000000ull) {
        v = (v + 5) / 10;
        e++;
    } else if (v < 100000000000000ull) {
        v *= 10;
        e--;
    }
    if (v >= 1000000000000000ull) {
        v /= 10;
        e++;
    }
    for (int k = 14; k >= 0; k--) {
        d[k] = (char)('0' + v % 10);
        v /= 10;
    }
    d[15] = '\0';
    *e_out = e;
}

/* Rounds the 15 digits to keep digits (half up); returns the new
   exponent. */
static int num_round_digits(char d[16], int e, int keep) {
    if (keep >= 15) {
        return e;
    }
    if (keep < 0) {
        memset(d, '0', 15);
        return e;
    }
    bool up = d[keep] >= '5';
    for (int k = keep; k < 15; k++) {
        d[k] = '0';
    }
    if (!up) {
        return e;
    }
    int k = keep - 1;
    while (k >= 0 && d[k] == '9') {
        d[k] = '0';
        k--;
    }
    if (k >= 0) {
        d[k]++;
        return e;
    }
    /* 999.. rounded up to 1000.. */
    memmove(d + 1, d, 14);
    d[0] = '1';
    return e + 1;
}

/* |x| with exactly dec decimals: integer digits, then '.', then dec
   digits (no '.' when dec is 0). */
static void num_fixed(double x, int dec, char *out, size_t capacity) {
    if (x < 0) {
        x = -x;
    }
    if (dec > 30) {
        dec = 30;
    }
    char d[16];
    int e = 0;
    bool zero = x == 0.0 || !num_finite(x);
    if (!zero) {
        num_digits15(x, d, &e);
        int keep = e + 1 + dec;
        if (keep <= 0) {
            /* Rounds to 0, or to one unit in the last place. */
            zero = !(keep == 0 && d[0] >= '5');
            if (!zero) {
                memset(d, '0', 15);
                d[0] = '1';
                e = -dec;
            }
        } else {
            e = num_round_digits(d, e, keep);
        }
    }
    size_t used = 0;
    if (zero || e < 0) {
        if (used + 1 < capacity) {
            out[used++] = '0';
        }
    } else {
        for (int place = e; place >= 0; place--) {
            int k = e - place;
            if (used + 1 < capacity) {
                out[used++] = k < 15 ? d[k] : '0';
            }
        }
    }
    if (dec > 0 && used + 1 < capacity) {
        out[used++] = '.';
        for (int place = -1; place >= -dec; place--) {
            int k = e - place;
            char c = zero || k < 0 || k >= 15 ? '0' : d[k];
            if (used + 1 < capacity) {
                out[used++] = c;
            }
        }
    }
    out[used] = '\0';
}

/* Scientific: m.mmmE+xx with dec decimals in the mantissa. */
static void num_scientific(double x, int dec, bool plus, int min_exp_digits,
                           char *out, size_t capacity) {
    bool negative = x < 0;
    if (negative) {
        x = -x;
    }
    char d[16];
    int e = 0;
    if (x == 0.0) {
        memset(d, '0', 15);
        d[15] = '\0';
    } else {
        num_digits15(x, d, &e);
        e = num_round_digits(d, e, 1 + dec);
    }
    size_t used = 0;
    if (negative && used + 1 < capacity) {
        out[used++] = '-';
    }
    if (used + 1 < capacity) {
        out[used++] = d[0];
    }
    if (dec > 0 && used + 1 < capacity) {
        out[used++] = '.';
        for (int k = 1; k <= dec && used + 1 < capacity; k++) {
            out[used++] = k < 15 ? d[k] : '0';
        }
    }
    char exp_text[16];
    int ae = e < 0 ? -e : e;
    snprintf(exp_text, sizeof(exp_text), "E%s%0*d",
             e < 0 ? "-" : plus ? "+" : "", min_exp_digits, ae);
    for (const char *c = exp_text; *c != '\0' && used + 1 < capacity; c++) {
        out[used++] = *c;
    }
    out[used] = '\0';
}

/* Removes trailing zeros (and the point) after a decimal point. */
static void num_trim_zeros(char *text) {
    char *dot = strchr(text, '.');
    if (dot == NULL) {
        return;
    }
    char *end = text + strlen(text);
    while (end > dot + 1 && end[-1] == '0') {
        *--end = '\0';
    }
    if (end == dot + 1) {
        *dot = '\0';
    }
}

/* The General format: up to `width` characters (at least 7), switching
   to scientific for very large or small numbers. */
static void num_general(double x, int width, char *out, size_t capacity) {
    if (!num_finite(x)) {
        snprintf(out, capacity, "#NUM!");
        return;
    }
    if (x == 0.0) {
        snprintf(out, capacity, "0");
        return;
    }
    if (width < 7) {
        width = 7;
    }
    if (width > 17) {
        width = 17;
    }
    char d[16];
    int e;
    num_digits15(x < 0 ? -x : x, d, &e);
    int room = width - (x < 0 ? 1 : 0);
    char text[48];
    if (e >= 0 && e + 1 <= room) {
        int dec = room - (e + 1) - 1; /* the point takes one */
        if (dec < 0) {
            dec = 0;
        }
        num_fixed(x, dec, text, sizeof(text));
        num_trim_zeros(text);
        /* Rounding may add a digit: 99999.99 -> 100000. */
        if ((int)strlen(text) <= room) {
            snprintf(out, capacity, "%s%s", x < 0 ? "-" : "", text);
            return;
        }
    } else if (e < 0 && e >= -5) {
        int dec = room - 2; /* "0." */
        num_fixed(x, dec, text, sizeof(text));
        num_trim_zeros(text);
        if (strcmp(text, "0") != 0) {
            snprintf(out, capacity, "%s%s", x < 0 ? "-" : "", text);
            return;
        }
    }
    int dec = room - 6; /* d.E+xx */
    if (dec < 0) {
        dec = 0;
    }
    if (dec > 14) {
        dec = 14;
    }
    char sci[48];
    num_scientific(x, dec, true, 2, sci, sizeof(sci));
    /* Trim zeros in the mantissa: 1.50000E+20 -> 1.5E+20. */
    char *ep = strchr(sci, 'E');
    char mant[48];
    size_t ml = (size_t)(ep - sci);
    memcpy(mant, sci, ml);
    mant[ml] = '\0';
    num_trim_zeros(mant);
    snprintf(out, capacity, "%s%s", mant, ep);
}

/* All 15 digits, for the formula bar and for saving. */
static void num_full(double x, char *out, size_t capacity) {
    num_general(x, 17, out, capacity);
}

/* ---------------------------------------------------------- dates */

static long days_from_civil(long y, unsigned m, unsigned d) {
    y -= m <= 2;
    long era = (y >= 0 ? y : y - 399) / 400;
    unsigned yoe = (unsigned)(y - era * 400);
    unsigned doy = (153 * (m + (m > 2 ? -3 : 9)) + 2) / 5 + d - 1;
    unsigned doe = yoe * 365 + yoe / 4 - yoe / 100 + doy;
    return era * 146097 + (long)doe - 719468;
}

static void civil_from_days(long z, long *y, int *m, int *d) {
    z += 719468;
    long era = (z >= 0 ? z : z - 146096) / 146097;
    unsigned doe = (unsigned)(z - era * 146097);
    unsigned yoe = (doe - doe / 1460 + doe / 36524 - doe / 146096) / 365;
    long yy = (long)yoe + era * 400;
    unsigned doy = doe - (365 * yoe + yoe / 4 - yoe / 100);
    unsigned mp = (5 * doy + 2) / 153;
    *d = (int)(doy - (153 * mp + 2) / 5 + 1);
    *m = (int)(mp < 10 ? mp + 3 : mp - 9);
    *y = yy + (*m <= 2);
}

/* Serial number of a date (1900 system). */
static double serial_from_date(long y, int m, int d) {
    long days = days_from_civil(y, (unsigned)m, (unsigned)d);
    if (y < 1900 || (y == 1900 && m <= 2)) {
        return (double)(days - days_from_civil(1899, 12, 31));
    }
    return (double)(days - days_from_civil(1899, 12, 30));
}

struct when {
    long year;
    int month, day, hour, minute, second, weekday; /* 0 = Sunday */
    double fraction; /* of a second */
};

static bool date_from_serial(double v, struct when *w) {
    if (!num_finite(v) || v < 0 || v > 2958465.99999) {
        return false;
    }
    long days = (long)floor(v);
    double rest = (v - (double)days) * 86400.0;
    long seconds = (long)floor(rest + 1e-6);
    w->fraction = rest - (double)seconds;
    if (w->fraction < 0) {
        w->fraction = 0;
    }
    if (seconds >= 86400) {
        seconds -= 86400;
        days++;
    }
    w->hour = (int)(seconds / 3600);
    w->minute = (int)(seconds / 60 % 60);
    w->second = (int)(seconds % 60);
    if (days == 60) {
        w->year = 1900;
        w->month = 2;
        w->day = 29;
        w->weekday = 3;
        return true;
    }
    long civil = days < 60 ? days + days_from_civil(1899, 12, 31)
                           : days + days_from_civil(1899, 12, 30);
    civil_from_days(civil, &w->year, &w->month, &w->day);
    w->weekday = (int)(((civil % 7) + 11) % 7); /* 1970-01-01 = Thursday */
    return true;
}

/* -------------------------------------------------- number formats */

static const char *const month_names[12] = {
    "January", "February", "March", "April", "May", "June", "July",
    "August", "September", "October", "November", "December",
};
static const char *const day_names[7] = {
    "Sunday", "Monday", "Tuesday", "Wednesday", "Thursday", "Friday",
    "Saturday",
};

struct shown {
    char text[128];
    uint32_t colour;     /* when has_colour */
    bool has_colour;
    bool too_wide_hash;  /* numbers: show ### when it does not fit */
};

static void shown_put(struct shown *o, size_t *used, const char *text,
                      size_t length) {
    for (size_t i = 0; i < length && *used + 1 < sizeof(o->text); i++) {
        o->text[(*used)++] = text[i];
    }
    o->text[*used] = '\0';
}

/* Splits a code into up to 4 sections at ';' outside quotes and
   brackets. */
static int fmt_sections(const char *code, const char *start[4],
                        size_t length[4]) {
    int count = 0;
    const char *s = code;
    start[0] = s;
    bool quote = false, bracket = false;
    for (; *s != '\0'; s++) {
        if (*s == '\\' && s[1] != '\0' && !quote) {
            s++;
            continue;
        }
        if (*s == '"') {
            quote = !quote;
        } else if (!quote && *s == '[') {
            bracket = true;
        } else if (!quote && *s == ']') {
            bracket = false;
        } else if (!quote && !bracket && *s == ';') {
            length[count] = (size_t)(s - start[count]);
            if (count == 3) {
                return 4;
            }
            count++;
            start[count] = s + 1;
        }
    }
    length[count] = (size_t)(s - start[count]);
    return count + 1;
}

static bool fmt_word(const char *s, size_t n, size_t i, const char *word) {
    size_t length = strlen(word);
    if (i + length > n) {
        return false;
    }
    for (size_t k = 0; k < length; k++) {
        if (tolower((unsigned char)s[i + k]) != tolower((unsigned char)word[k])) {
            return false;
        }
    }
    return true;
}

static uint32_t fmt_colour_name(const char *s, size_t n) {
    static const struct {
        const char *name;
        uint32_t colour;
    } colours[] = {
        {"black", 0x000000}, {"blue", 0x0000ff}, {"cyan", 0x00ffff},
        {"green", 0x00a000}, {"magenta", 0xff00ff}, {"red", 0xff0000},
        {"white", 0xffffff}, {"yellow", 0xffff00},
    };
    for (size_t i = 0; i < sizeof(colours) / sizeof(colours[0]); i++) {
        if (strlen(colours[i].name) == n && fmt_word(s, n, 0, colours[i].name)) {
            return colours[i].colour;
        }
    }
    return 0xffffffffu;
}

/* What kind of section: 'd' date/time, 'n' number, 't' text only, 'g'
   General. */
static char fmt_kind(const char *s, size_t n) {
    bool number = false, date = false, general = false, month = false;
    for (size_t i = 0; i < n; i++) {
        char c = s[i];
        if (c == '"') {
            i++;
            while (i < n && s[i] != '"') {
                i++;
            }
            continue;
        }
        if (c == '\\' || c == '_' || c == '*') {
            i++;
            continue;
        }
        if (c == '[') {
            size_t k = i + 1;
            if (k < n && (s[k] == 'h' || s[k] == 'H' || s[k] == 'm'
                          || s[k] == 'M' || s[k] == 's' || s[k] == 'S')) {
                date = true;
            }
            while (i < n && s[i] != ']') {
                i++;
            }
            continue;
        }
        if (fmt_word(s, n, i, "general")) {
            general = true;
            i += 6;
            continue;
        }
        char l = (char)tolower((unsigned char)c);
        if (c == '0' || c == '#' || c == '?') {
            number = true;
        } else if (l == 'y' || l == 'd' || l == 'h' || l == 's') {
            date = true;
        } else if (l == 'm') {
            month = true;
        } else if (fmt_word(s, n, i, "am/pm") || fmt_word(s, n, i, "a/p")) {
            date = true;
        }
    }
    if (date || (month && !number)) {
        return 'd';
    }
    if (number) {
        return 'n';
    }
    if (general) {
        return 'g';
    }
    return 't';
}

/* Literal text and colours of a section, for the parts around the value
   ('@' or General marks where the value goes). */
static void fmt_literals(const char *s, size_t n, struct shown *o,
                         size_t *used, const char *value, char kind) {
    for (size_t i = 0; i < n; i++) {
        char c = s[i];
        if (c == '"') {
            size_t k = i + 1;
            while (k < n && s[k] != '"') {
                k++;
            }
            shown_put(o, used, s + i + 1, k - i - 1);
            i = k;
        } else if (c == '\\' && i + 1 < n) {
            shown_put(o, used, s + i + 1, 1);
            i++;
        } else if (c == '_' && i + 1 < n) {
            shown_put(o, used, " ", 1);
            i++;
        } else if (c == '*' && i + 1 < n) {
            i++;
        } else if (c == '[') {
            size_t k = i + 1;
            while (k < n && s[k] != ']') {
                k++;
            }
            uint32_t colour = fmt_colour_name(s + i + 1, k - i - 1);
            if (colour != 0xffffffffu) {
                o->colour = colour;
                o->has_colour = true;
            } else if (i + 1 < n && s[i + 1] == '$') {
                size_t e = i + 2;
                while (e < k && s[e] != '-') {
                    e++;
                }
                shown_put(o, used, s + i + 2, e - i - 2);
            }
            i = k;
        } else if (c == '@' || (kind == 'g' && fmt_word(s, n, i, "general"))) {
            shown_put(o, used, value, strlen(value));
            if (c != '@') {
                i += 6;
            }
        } else {
            shown_put(o, used, s + i, 1);
        }
    }
}

/* A number with a number section ("#,##0.00", "0%", "0.0E+00", ...). */
static void fmt_number(double v, const char *s, size_t n, struct shown *o,
                       bool minus) {
    /* First pass: the shape. */
    int int_places = 0, int_zeros = 0, dec_zeros = 0, dec_places = 0;
    int percent = 0, scale = 0, exp_places = 0;
    bool group = false, point = false, exponent = false, exp_plus = false;
    size_t first = n, last = 0; /* placeholder span */
    for (size_t i = 0; i < n; i++) {
        char c = s[i];
        if (c == '"') {
            i++;
            while (i < n && s[i] != '"') {
                i++;
            }
            continue;
        }
        if (c == '\\' || c == '_' || c == '*') {
            i++;
            continue;
        }
        if (c == '[') {
            while (i < n && s[i] != ']') {
                i++;
            }
            continue;
        }
        if (c == '0' || c == '#' || c == '?') {
            if (i < first) {
                first = i;
            }
            last = i;
            if (exponent) {
                exp_places++;
            } else if (point) {
                dec_places++;
                if (c != '#') {
                    dec_zeros = dec_places;
                }
            } else {
                int_places++;
                if (c != '#') {
                    int_zeros++;
                }
            }
        } else if (c == '.' && !point && !exponent) {
            point = true;
            if (i < first) {
                first = i;
            }
            last = i;
        } else if (c == ',' && !point && !exponent) {
            /* Between placeholders: thousands; after the last: / 1000. */
            size_t k = i + 1;
            while (k < n && s[k] == ',') {
                k++;
            }
            if (k < n && (s[k] == '0' || s[k] == '#' || s[k] == '?')
                && int_places > 0) {
                group = true;
            } else if (int_places > 0) {
                scale += (int)(k - i);
            }
            i = k - 1;
        } else if (c == '%') {
            percent++;
        } else if ((c == 'E' || c == 'e') && i + 1 < n
                   && (s[i + 1] == '+' || s[i + 1] == '-')) {
            exponent = true;
            exp_plus = s[i + 1] == '+';
            i++;
            last = i;
        }
    }
    double x = v < 0 ? -v : v;
    for (int k = 0; k < percent; k++) {
        x *= 100;
    }
    for (int k = 0; k < scale; k++) {
        x /= 1000;
    }
    char number[96];
    if (exponent) {
        num_scientific(x, dec_places, exp_plus, exp_places > 0 ? exp_places : 1,
                       number, sizeof(number));
    } else {
        num_fixed(x, dec_places, number, sizeof(number));
        char *dot = strchr(number, '.');
        /* Optional decimals ('#') drop their trailing zeros. */
        if (dot != NULL) {
            char *end = number + strlen(number);
            while (end > dot + 1 + dec_zeros && end[-1] == '0') {
                *--end = '\0';
            }
            if (end == dot + 1 && !point) {
                *dot = '\0';
            }
        }
        /* Integer digits: no leading 0 unless the format asks for one. */
        char *ip = number;
        size_t il = dot != NULL ? (size_t)(strchr(number, '.') - number)
                                : strlen(number);
        if (il == 1 && ip[0] == '0' && int_zeros == 0) {
            memmove(ip, ip + 1, strlen(ip));
            il = 0;
        }
        /* Pad with zeros to int_zeros digits. */
        if ((int)il < int_zeros) {
            int pad = int_zeros - (int)il;
            size_t length = strlen(number);
            if (length + (size_t)pad < sizeof(number)) {
                memmove(number + pad, number, length + 1);
                memset(number, '0', (size_t)pad);
                il += (size_t)pad;
            }
        }
        if (group && il > 3) {
            char grouped[128];
            size_t g = 0;
            for (size_t k = 0; k < il; k++) {
                if (k > 0 && (il - k) % 3 == 0) {
                    grouped[g++] = ',';
                }
                grouped[g++] = number[k];
            }
            snprintf(grouped + g, sizeof(grouped) - g, "%s", number + il);
            snprintf(number, sizeof(number), "%s", grouped);
        }
    }
    /* Second pass: literals before, the number, literals after. */
    size_t used = strlen(o->text);
    if (first == n) {
        first = last = n;
    }
    fmt_literals(s, first, o, &used, "", 'n');
    if (minus) {
        shown_put(o, &used, "-", 1);
    }
    shown_put(o, &used, number, strlen(number));
    if (last < n) {
        /* Literals between placeholders (000-0000) are dropped; the
           rest after the last placeholder are kept. */
        size_t after = last + 1;
        for (size_t i = first; i <= last && i < n; i++) {
            if (s[i] == '%') {
                shown_put(o, &used, "%", 1);
            }
        }
        fmt_literals(s + after, n - after, o, &used, "", 'n');
    }
    o->too_wide_hash = true;
}

/* A date or time section ("yyyy-mm-dd", "h:mm AM/PM", "[h]:mm:ss"). */
static void fmt_date(double v, const char *s, size_t n, struct shown *o) {
    struct when w;
    if (!date_from_serial(v, &w)) {
        size_t used = strlen(o->text);
        shown_put(o, &used, "#######", 7);
        return;
    }
    /* Is there an AM/PM? Then hours are 1-12. */
    bool ampm = false;
    for (size_t i = 0; i < n; i++) {
        if (fmt_word(s, n, i, "am/pm") || fmt_word(s, n, i, "a/p")) {
            ampm = true;
        }
    }
    size_t used = strlen(o->text);
    char last_unit = 0; /* the previous date part, for m = minutes */
    for (size_t i = 0; i < n; i++) {
        char c = s[i];
        char l = (char)tolower((unsigned char)c);
        size_t run = 1;
        while (i + run < n && tolower((unsigned char)s[i + run]) == l) {
            run++;
        }
        char part[48];
        part[0] = '\0';
        if (c == '"') {
            size_t k = i + 1;
            while (k < n && s[k] != '"') {
                k++;
            }
            shown_put(o, &used, s + i + 1, k - i - 1);
            i = k;
            continue;
        }
        if (c == '\\' && i + 1 < n) {
            shown_put(o, &used, s + i + 1, 1);
            i++;
            continue;
        }
        if (c == '_' && i + 1 < n) {
            shown_put(o, &used, " ", 1);
            i++;
            continue;
        }
        if (c == '*' && i + 1 < n) {
            i++;
            continue;
        }
        if (c == '[') {
            size_t k = i + 1;
            while (k < n && s[k] != ']') {
                k++;
            }
            char unit = (char)tolower((unsigned char)s[i + 1]);
            double total = v * 24.0;
            long value = unit == 'h' ? (long)floor(total + 1e-9)
                : unit == 'm' ? (long)floor(total * 60 + 1e-7)
                : (long)floor(total * 3600 + 1e-5);
            uint32_t colour = fmt_colour_name(s + i + 1, k - i - 1);
            if (colour != 0xffffffffu) {
                o->colour = colour;
                o->has_colour = true;
            } else if (unit == 'h' || unit == 'm' || unit == 's') {
                snprintf(part, sizeof(part), "%0*ld", (int)(k - i - 1), value);
                last_unit = unit;
            }
            shown_put(o, &used, part, strlen(part));
            i = k;
            continue;
        }
        if (fmt_word(s, n, i, "am/pm")) {
            shown_put(o, &used, w.hour < 12 ? "AM" : "PM", 2);
            i += 4;
            continue;
        }
        if (fmt_word(s, n, i, "a/p")) {
            shown_put(o, &used, w.hour < 12 ? "A" : "P", 1);
            i += 2;
            continue;
        }
        if (l == 'y') {
            if (run <= 2) {
                snprintf(part, sizeof(part), "%02ld", w.year % 100);
            } else {
                snprintf(part, sizeof(part), "%ld", w.year);
            }
        } else if (l == 'm') {
            /* Minutes after an hour or before seconds. */
            bool minutes = last_unit == 'h';
            for (size_t k = i + run; k < n && !minutes; k++) {
                char kl = (char)tolower((unsigned char)s[k]);
                if (kl == 's') {
                    minutes = true;
                } else if (isalpha((unsigned char)s[k])) {
                    break;
                }
            }
            if (minutes && run <= 2) {
                snprintf(part, sizeof(part), run == 1 ? "%d" : "%02d", w.minute);
            } else if (run <= 2) {
                snprintf(part, sizeof(part), run == 1 ? "%d" : "%02d", w.month);
            } else if (run == 3) {
                snprintf(part, sizeof(part), "%.3s", month_names[w.month - 1]);
            } else if (run == 5) {
                snprintf(part, sizeof(part), "%.1s", month_names[w.month - 1]);
            } else {
                snprintf(part, sizeof(part), "%s", month_names[w.month - 1]);
            }
        } else if (l == 'd') {
            if (run <= 2) {
                snprintf(part, sizeof(part), run == 1 ? "%d" : "%02d", w.day);
            } else if (run == 3) {
                snprintf(part, sizeof(part), "%.3s", day_names[w.weekday]);
            } else {
                snprintf(part, sizeof(part), "%s", day_names[w.weekday]);
            }
        } else if (l == 'h') {
            int hour = w.hour;
            if (ampm) {
                hour = hour % 12 == 0 ? 12 : hour % 12;
            }
            snprintf(part, sizeof(part), run == 1 ? "%d" : "%02d", hour);
        } else if (l == 's') {
            snprintf(part, sizeof(part), run == 1 ? "%d" : "%02d", w.second);
            /* Fractions of a second: ss.0 or ss.00 */
            if (i + run < n && s[i + run] == '.' && i + run + 1 < n
                && s[i + run + 1] == '0') {
                size_t z = i + run + 1;
                int places = 0;
                while (z < n && s[z] == '0') {
                    places++;
                    z++;
                }
                char frac[24];
                num_fixed(w.fraction, places, frac, sizeof(frac));
                strncat(part, frac + 1, sizeof(part) - strlen(part) - 1);
                run = z - i;
            }
        } else {
            shown_put(o, &used, s + i, 1);
            continue;
        }
        if (l == 'y' || l == 'd' || l == 'h' || l == 's'
            || (l == 'm' && part[0] != '\0')) {
            last_unit = l;
        }
        shown_put(o, &used, part, strlen(part));
        i += run - 1;
    }
}

/* Shows a number with a format code ("" or "General" for General);
   width is the General width in characters. */
static void format_number(double v, const char *code, int width,
                          struct shown *o) {
    o->text[0] = '\0';
    o->has_colour = false;
    o->too_wide_hash = false;
    if (code == NULL || code[0] == '\0') {
        code = "General";
    }
    const char *start[4];
    size_t length[4];
    int count = fmt_sections(code, start, length);
    int section = 0;
    bool minus = false;
    if (v < 0 && count >= 2) {
        section = 1;
    } else if (v == 0 && count >= 3) {
        section = 2;
    } else if (v < 0) {
        minus = true;
    }
    if (count >= 2 && v < 0) {
        v = -v;
    }
    const char *s = start[section];
    size_t n = length[section];
    char kind = fmt_kind(s, n);
    if (kind == 'd') {
        if (minus) {
            snprintf(o->text, sizeof(o->text), "#######");
            return;
        }
        fmt_date(v, s, n, o);
        return;
    }
    if (kind == 'n') {
        fmt_number(minus ? -v : v, s, n, o, minus);
        return;
    }
    char general[48];
    num_general(v, width, general, sizeof(general));
    if (minus && general[0] == '-') {
        /* fmt_literals puts the value in place; keep its sign. */
    }
    size_t used = 0;
    if (kind == 'g') {
        fmt_literals(s, n, o, &used, general, 'g');
    } else {
        /* Only literals: "Yes";"No" style sections. */
        fmt_literals(s, n, o, &used, general, 't');
        if (count == 1) {
            /* A text-only format like "@" shows the number itself. */
            if (used == 0 || strchr(code, '@') != NULL) {
                used = 0;
                o->text[0] = '\0';
                shown_put(o, &used, general, strlen(general));
            }
        }
    }
    o->too_wide_hash = true;
}

/* Shows text with a format code: only the 4th section (or one with '@')
   changes it. */
static void format_text(const char *text, const char *code, struct shown *o) {
    o->text[0] = '\0';
    o->has_colour = false;
    o->too_wide_hash = false;
    size_t used = 0;
    const char *start[4];
    size_t length[4];
    int count = code != NULL && code[0] != '\0'
        ? fmt_sections(code, start, length) : 0;
    if (count == 4) {
        fmt_literals(start[3], length[3], o, &used, text, 't');
        return;
    }
    if (count == 1 && memchr(start[0], '@', length[0]) != NULL) {
        fmt_literals(start[0], length[0], o, &used, text, 't');
        return;
    }
    shown_put(o, &used, text, strlen(text));
}

/* Is this a date or time format? (for typing and the formula bar) */
static bool format_is_date(const char *code) {
    if (code == NULL || code[0] == '\0') {
        return false;
    }
    const char *start[4];
    size_t length[4];
    fmt_sections(code, start, length);
    return fmt_kind(start[0], length[0]) == 'd';
}

static bool format_is_text(const char *code) {
    return code != NULL && strcmp(code, "@") == 0;
}

/* Adds (+1) or removes (-1) a decimal place in every section. */
static void format_change_decimals(const char *code, int delta, char *out,
                                   size_t capacity) {
    if (code == NULL || code[0] == '\0' || fmt_word(code, strlen(code), 0,
                                                    "general")) {
        snprintf(out, capacity, delta > 0 ? "0.0" : "0");
        return;
    }
    size_t used = 0;
    bool quote = false, bracket = false;
    for (const char *s = code; *s != '\0' && used + 2 < capacity; s++) {
        char c = *s;
        if (c == '"') {
            quote = !quote;
        } else if (!quote && c == '[') {
            bracket = true;
        } else if (!quote && c == ']') {
            bracket = false;
        }
        bool digit = c == '0' || c == '#' || c == '?';
        /* The end of a number: after its last digit placeholder. */
        bool end_of_number = !quote && !bracket && digit
            && !(s[1] == '0' || s[1] == '#' || s[1] == '?' || s[1] == '.'
                 || s[1] == ',');
        out[used++] = c;
        if (!end_of_number) {
            continue;
        }
        /* Find whether this number has a point. */
        const char *b = s;
        while (b > code && (b[-1] == '0' || b[-1] == '#' || b[-1] == '?'
                            || b[-1] == ',' || b[-1] == '.')) {
            b--;
        }
        const char *dot = NULL;
        for (const char *k = b; k <= s; k++) {
            if (*k == '.') {
                dot = k;
            }
        }
        if (delta > 0) {
            if (dot == NULL) {
                out[used++] = '.';
            }
            out[used++] = '0';
        } else if (dot != NULL) {
            used--; /* drop this last decimal */
            if (s - dot == 1) {
                used--; /* and the point */
            }
        }
    }
    out[used] = '\0';
}

/* ---------------------------------------------------- typed values */

/* What typing text into a cell gives: a number (with a format that fits
   how it was typed, "" when General is right) or not. */
static bool parse_typed_number(const char *text, double *out,
                               char *format, size_t capacity) {
    format[0] = '\0';
    size_t n = strlen(text);
    while (n > 0 && text[n - 1] == ' ') {
        n--;
    }
    size_t i = 0;
    while (i < n && text[i] == ' ') {
        i++;
    }
    if (i >= n) {
        return false;
    }
    const char *s = text + i;
    n -= i;
    /* Dates: yyyy-mm-dd or yyyy/mm/dd, optional time. Times: h:mm[:ss]
       [AM|PM]. */
    if (n >= 8 && isdigit((unsigned char)s[0]) && isdigit((unsigned char)s[3])
        && (s[4] == '-' || s[4] == '/')) {
        long y = 0;
        int m = 0, d = 0;
        size_t k = 0;
        while (k < 4 && isdigit((unsigned char)s[k])) {
            y = y * 10 + (s[k++] - '0');
        }
        char sep = s[k];
        if (k == 4 && (sep == '-' || sep == '/')) {
            k++;
            while (k < n && isdigit((unsigned char)s[k]) && m < 100) {
                m = m * 10 + (s[k++] - '0');
            }
            if (k < n && s[k] == sep) {
                k++;
                while (k < n && isdigit((unsigned char)s[k]) && d < 100) {
                    d = d * 10 + (s[k++] - '0');
                }
                static const int month_days[12] = {31, 29, 31, 30, 31, 30,
                                                   31, 31, 30, 31, 30, 31};
                if (m >= 1 && m <= 12 && d >= 1 && d <= month_days[m - 1]
                    && y >= 1900 && y <= 9999
                    && !(m == 2 && d == 29 && !(y % 4 == 0 && (y % 100 != 0 || y % 400 == 0)))
                    && (k == n || s[k] == ' ')) {
                    double serial = serial_from_date(y, m, d);
                    if (k == n) {
                        *out = serial;
                        snprintf(format, capacity, "yyyy-mm-dd");
                        return true;
                    }
                    /* A time follows. */
                    double t;
                    char tf[32];
                    if (parse_typed_number(s + k + 1, &t, tf, sizeof(tf))
                        && format_is_date(tf) && t < 1) {
                        *out = serial + t;
                        snprintf(format, capacity, "yyyy-mm-dd h:mm");
                        return true;
                    }
                }
            }
        }
        return false;
    }
    if (n >= 3 && isdigit((unsigned char)s[0])) {
        size_t k = 0;
        int h = 0, mi = 0, sec = 0;
        while (k < n && isdigit((unsigned char)s[k]) && h < 10000) {
            h = h * 10 + (s[k++] - '0');
        }
        if (k < n && s[k] == ':' && k + 2 < n + 1 && k + 1 < n
            && isdigit((unsigned char)s[k + 1])) {
            k++;
            size_t mk = k;
            while (k < n && isdigit((unsigned char)s[k])) {
                mi = mi * 10 + (s[k++] - '0');
            }
            bool seconds = false;
            if (k - mk == 2 && k < n && s[k] == ':') {
                k++;
                size_t sk = k;
                while (k < n && isdigit((unsigned char)s[k])) {
                    sec = sec * 10 + (s[k++] - '0');
                }
                seconds = k - sk == 2;
                if (!seconds) {
                    return false;
                }
            }
            bool am = false, pm = false;
            size_t z = k;
            while (z < n && s[z] == ' ') {
                z++;
            }
            if (z + 2 == n && (fmt_word(s, n, z, "am") || fmt_word(s, n, z, "pm"))) {
                am = tolower((unsigned char)s[z]) == 'a';
                pm = !am;
                k = n;
            }
            if (k != n || k - mk < 2 || mi > 59 || sec > 59
                || ((am || pm) && (h < 1 || h > 12))) {
                return false;
            }
            if (pm && h != 12) {
                h += 12;
            } else if (am && h == 12) {
                h = 0;
            }
            *out = (h * 3600.0 + mi * 60.0 + sec) / 86400.0;
            snprintf(format, capacity, "%s",
                     am || pm ? (seconds ? "h:mm:ss AM/PM" : "h:mm AM/PM")
                              : (seconds ? "h:mm:ss" : "h:mm"));
            return true;
        }
    }
    /* Numbers: [-+][$|€]digits with thousands commas[.digits][e..][%] */
    size_t k = 0;
    bool negative = false, paren = false;
    if (k < n && (s[k] == '-' || s[k] == '+')) {
        negative = s[k] == '-';
        k++;
    } else if (k < n && s[k] == '(' && s[n - 1] == ')') {
        paren = true;
        negative = true;
        k++;
        n--;
    }
    const char *currency = NULL;
    if (k < n && s[k] == '$') {
        currency = "$";
        k++;
    } else if (k + 3 <= n && memcmp(s + k, "\xe2\x82\xac", 3) == 0) {
        currency = "\xe2\x82\xac";
        k += 3;
    }
    if (k < n && (s[k] == '-' || s[k] == '+') && currency != NULL && !negative) {
        negative = s[k] == '-';
        k++;
    }
    /* Copy without valid thousands separators. */
    char plain[64];
    size_t p = 0;
    bool grouped = false;
    int since_comma = -1, int_digits = 0;
    bool point = false;
    for (; k < n && p + 1 < sizeof(plain); k++) {
        char c = s[k];
        if (c == ',' && !point && int_digits > 0
            && (since_comma == -1 ? int_digits <= 3 : since_comma == 3)) {
            grouped = true;
            since_comma = 0;
            continue;
        }
        if (isdigit((unsigned char)c) && !point) {
            int_digits++;
            if (since_comma >= 0) {
                since_comma++;
            }
        } else if (c == '.') {
            point = true;
        } else if (!(isdigit((unsigned char)c) || c == 'e' || c == 'E'
                     || c == '+' || c == '-')) {
            break;
        }
        plain[p++] = c;
    }
    if (grouped && since_comma != 3) {
        return false;
    }
    plain[p] = '\0';
    bool percent = false;
    size_t rest = k;
    if (rest < n && s[rest] == '%') {
        percent = true;
        rest++;
    }
    if (rest < n && currency == NULL && rest + 3 <= n
        && memcmp(s + rest, "\xe2\x82\xac", 3) == 0) {
        currency = "\xe2\x82\xac";
        rest += 3;
    }
    if (rest != n) {
        return false;
    }
    double value;
    size_t got = num_scan(plain, p, &value);
    if (got == 0 || got != p || (plain[0] == '-' || plain[0] == '+')) {
        return false;
    }
    if (percent) {
        value /= 100;
    }
    *out = negative ? -value : value;
    char *dot = strchr(plain, '.');
    int decimals = dot != NULL ? (int)strlen(dot + 1) : 0;
    if (strchr(plain, 'e') != NULL || strchr(plain, 'E') != NULL) {
        snprintf(format, capacity, "0.00E+00");
    } else if (percent) {
        snprintf(format, capacity, decimals > 0 ? "0.%.*s%%" : "0%%",
                 decimals, "000000000000000");
    } else if (currency != NULL) {
        if (currency[0] == '$') {
            snprintf(format, capacity, "$#,##0.00");
        } else {
            snprintf(format, capacity, "#,##0.00 \"\xe2\x82\xac\"");
        }
    } else if (grouped) {
        snprintf(format, capacity, decimals > 0 ? "#,##0.00" : "#,##0");
    }
    (void)paren;
    return true;
}

#endif
