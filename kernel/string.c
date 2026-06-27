#include <stdarg.h>
#include <stdbool.h>
#include <stddef.h>

static void append_char(char** buf, size_t* remaining, char c) {
    if (*remaining > 1) {
        **buf = c;
        (*buf)++;
        (*remaining)--;
    }
}

static void append_str(char** buf, size_t* remaining, const char* s) {
    while (*s) {
        append_char(buf, remaining, *s++);
    }
}

static void append_uint(char** buf, size_t* remaining, unsigned long long v,
                        int base, bool uppercase) {
    char tmp[32];
    int pos = 0;

    const char* digits = uppercase ? "0123456789ABCDEF" : "0123456789abcdef";

    if (v == 0) {
        tmp[pos++] = '0';
    } else {
        while (v > 0) {
            tmp[pos++] = digits[v % base];
            v /= base;
        }
    }

    while (pos--) {
        append_char(buf, remaining, tmp[pos]);
    }
}

void* memset(void* dest, int ch, size_t count) {
    unsigned char* p = dest;
    while (count--)
        *p++ = (unsigned char)ch;
    return dest;
}

void* memcpy(void* dest, const void* src, size_t count) {
    unsigned char* d = dest;
    const unsigned char* s = src;
    while (count--)
        *d++ = *s++;
    return dest;
}

int memcmp(const void* ptr1, const void* ptr2, size_t count) {
    const unsigned char* p1 = ptr1;
    const unsigned char* p2 = ptr2;
    while (count--) {
        if (*p1 != *p2)
            return *p1 - *p2;
        p1++;
        p2++;
    }
    return 0;
}

size_t strlen(const char* str) {
    size_t len = 0;
    while (str[len])
        len++;
    return len;
}

char* strcpy(char* dest, const char* src) {
    char* d = dest;
    while ((*d++ = *src++))
        ;
    return dest;
}

char* strncpy(char* dest, const char* src, size_t n) {
    char* d = dest;
    while (n-- && (*d++ = *src++))
        ;
    while (n--)
        *d++ = '\0';
    return dest;
}

char* strcat(char* dest, const char* src) {
    char* d = dest;
    while (*d)
        d++;
    while ((*d++ = *src++))
        ;
    return dest;
}

char* strncat(char* dest, const char* src, size_t n) {
    char* d = dest;
    while (*d)
        d++;
    while (n-- && (*d++ = *src++))
        ;
    *d = '\0';
    return dest;
}

int strcmp(const char* s1, const char* s2) {
    while (*s1 && (*s1 == *s2)) {
        s1++;
        s2++;
    }
    return *(unsigned char*)s1 - *(unsigned char*)s2;
}

int strncmp(const char* s1, const char* s2, size_t n) {
    if (n == 0)
        return 0;
    while (n-- && *s1 && (*s1 == *s2)) {
        s1++;
        s2++;
    }
    if ((int)n < 0)
        return 0;
    return *(unsigned char*)s1 - *(unsigned char*)s2;
}

const char* strchr(const char* str, int ch) {
    while (*str) {
        if (*str == (char)ch)
            return str;
        str++;
    }
    return NULL;
}

const char* strrchr(const char* str, int ch) {
    const char* last = NULL;
    while (*str) {
        if (*str == (char)ch)
            last = str;
        str++;
    }
    return last;
}

static int my_strlen(const char* s) {
    int n = 0;
    while (s[n])
        n++;
    return n;
}

int vsnprintf(char* str, size_t size, const char* fmt, va_list args) {
    char* buf = str;
    size_t remaining = size;

    while (*fmt) {
        if (*fmt != '%') {
            append_char(&buf, &remaining, *fmt++);
            continue;
        }

        fmt++; // skip '%'

        // flags
        bool left_align = false;
        bool zero_pad = false;

        if (*fmt == '-') {
            left_align = true;
            fmt++;
        }
        if (*fmt == '0') {
            zero_pad = true;
            fmt++;
        }

        // width
        int width = 0;
        while (*fmt >= '0' && *fmt <= '9') {
            width = width * 10 + (*fmt - '0');
            fmt++;
        }

        char temp[64];
        char* t = temp;
        temp[0] = 0;

        switch (*fmt) {
        case 'd': {
            long v = va_arg(args, int);
            if (v < 0) {
                *t++ = '-';
                v = -v;
            }
            append_uint(&t, &(size_t){sizeof(temp)}, (unsigned long long)v, 10,
                        false);
            break;
        }

        case 'u': {
            unsigned v = va_arg(args, unsigned);
            append_uint(&t, &(size_t){sizeof(temp)}, v, 10, false);
            break;
        }

        case 'x': {
            unsigned v = va_arg(args, unsigned);
            append_uint(&t, &(size_t){sizeof(temp)}, v, 16, false);
            break;
        }

        case 'X': {
            unsigned v = va_arg(args, unsigned);
            append_uint(&t, &(size_t){sizeof(temp)}, v, 16, true);
            break;
        }

        case 'p': {
            unsigned long long v = (unsigned long long)va_arg(args, void*);
            *t++ = '0';
            *t++ = 'x';
            append_uint(&t, &(size_t){sizeof(temp)}, v, 16, false);
            break;
        }

        case 'l': { // long / long long
            fmt++;
            if (*fmt == 'l') { // long long
                fmt++;
                long long v = va_arg(args, long long);
                if (v < 0) {
                    *t++ = '-';
                    v = -v;
                }
                append_uint(&t, &(size_t){sizeof(temp)}, (unsigned long long)v,
                            10, false);
            } else { // long
                long v = va_arg(args, long);
                if (v < 0) {
                    *t++ = '-';
                    v = -v;
                }
                append_uint(&t, &(size_t){sizeof(temp)}, (unsigned long long)v,
                            10, false);
            }
            break;
        }

        case 's': {
            const char* s = va_arg(args, const char*);
            if (!s)
                s = "(null)";
            append_str(&buf, &remaining, s);
            fmt++;
            continue;
        }

        case 'c': {
            char c = (char)va_arg(args, int);
            append_char(&buf, &remaining, c);
            fmt++;
            continue;
        }

        case '%': {
            append_char(&buf, &remaining, '%');
            fmt++;
            continue;
        }

        default:
            append_char(&buf, &remaining, '%');
            append_char(&buf, &remaining, *fmt);
            fmt++;
            continue;
        }

        *t = 0;
        int len = my_strlen(temp);
        int pad = width > len ? width - len : 0;

        if (!left_align) {
            char pad_char = zero_pad ? '0' : ' ';
            while (pad--)
                append_char(&buf, &remaining, pad_char);
        }

        append_str(&buf, &remaining, temp);

        if (left_align) {
            while (pad--)
                append_char(&buf, &remaining, ' ');
        }

        fmt++;
    }

    append_char(&buf, &remaining, '\0');
    return buf - str;
}

int snprintf(char* str, size_t size, const char* fmt, ...) {
    va_list ap;
    va_start(ap, fmt);
    int r = vsnprintf(str, size, fmt, ap);
    va_end(ap);
    return r;
}

int sprintf(char* str, const char* fmt, ...) {
    va_list ap;
    va_start(ap, fmt);
    int r = vsnprintf(str, (size_t)-1, fmt, ap);
    va_end(ap);
    return r;
}

int atoi(const char* str) {
    int result = 0;
    int sign = 1;

    while (*str == ' ' || *str == '\t')
        str++;

    if (*str == '-') {
        sign = -1;
        str++;
    } else if (*str == '+') {
        str++;
    }

    while (*str >= '0' && *str <= '9') {
        result = result * 10 + (*str - '0');
        str++;
    }

    return sign * result;
}

const char* strstr(const char* haystack, const char* needle) {
    if (!*needle)
        return haystack;

    while (*haystack) {
        const char* h = haystack;
        const char* n = needle;

        while (*h && *n && *h == *n) {
            h++;
            n++;
        }

        if (!*n)
            return haystack;

        haystack++;
    }

    return NULL;
}
