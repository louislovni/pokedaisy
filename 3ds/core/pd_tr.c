#include "pd_tr.h"

#include <ctype.h>
#include <stdbool.h>
#include <stdint.h>
#include <stdio.h>
#include <string.h>

static int lang; // 0 English, 1 French

void pd_tr_set_language(const char* code) {
    lang = code && !strncmp(code, "fr", 2) ? 1 : 0;
}

const char* pd_tr_language(void) { return lang == 1 ? "fr" : "en"; }

// A small cache of what was asked: the companion draws the same few hundred strings every frame.
#define SLOTS 512
#define SRC_MAX 240
#define OUT_MAX 320
static struct {
    uint32_t hash;
    char src[SRC_MAX];
    const char* result; // the table's string, the slot's out, or NULL = no translation
    char out[OUT_MAX];
} cache[SLOTS];

static uint32_t hash_of(const char* s) {
    uint32_t h = 2166136261u;
    for (; *s; s++) h = (h ^ (uint8_t) *s) * 16777619u;
    return h;
}

// A format's conversion at [f] ('%' first): how many chars it takes, and whether it's a number (else text).
static int conversion(const char* f, bool* number) {
    int n = 1;
    while (f[n] && strchr("0123456789.-+ ", f[n])) n++;
    while (f[n] == 'l' || f[n] == 'h' || f[n] == 'z') n++;
    *number = f[n] == 'd' || f[n] == 'u' || f[n] == 'i' || f[n] == 'x' || f[n] == 'X';
    return f[n] ? n + 1 : n;
}

#define MAX_CAPS 6
// A %s takes a name (a POKéMON, an item, a place), not a sentence: "%s of %s" never eats half a paragraph.
#define MAX_TEXT_CAPTURE 40
struct caps {
    int n;
    const char* at[MAX_CAPS];
    int len[MAX_CAPS];
    bool text[MAX_CAPS]; // a %s (a name the table may have: a boss, a place), not a number
};

// [s] as [f] would print it: the arguments' text captured in order.
static bool match(const char* s, const char* f, struct caps* c) {
    while (*f) {
        if (*f == '%' && f[1] == '%') {
            if (*s != '%') return false;
            s++, f += 2;
            continue;
        }
        if (*f != '%') {
            if (*s != *f) return false;
            s++, f++;
            continue;
        }
        bool number;
        int k = conversion(f, &number);
        if (c->n >= MAX_CAPS) return false;
        const char* rest = f + k;
        if (number) {
            const char* p = s;
            if (*p == '-') p++;
            if (!isdigit((unsigned char) *p)) return false;
            while (isdigit((unsigned char) *p)) p++;
            c->at[c->n] = s;
            c->len[c->n] = (int) (p - s);
            c->text[c->n] = false;
            c->n++;
            s = p;
            f = rest;
            continue;
        }
        // Text: the shortest run (at least a character, at most a name's length) the rest then matches.
        int saved = c->n;
        const char* last = s + strlen(s);
        if (last > s + MAX_TEXT_CAPTURE) last = s + MAX_TEXT_CAPTURE;
        for (const char* p = s + 1; p <= last; p++) {
            c->n = saved;
            c->at[c->n] = s;
            c->len[c->n] = (int) (p - s);
            c->text[c->n] = true;
            c->n++;
            if (match(p, rest, c)) return true;
        }
        c->n = saved;
        return false;
    }
    return *s == 0;
}

// The translation [t] with {0}, {1}.. replaced by the captures.
static const char* exact(const char* s);

static void fill(char* out, size_t size, const char* t, const struct caps* c) {
    size_t o = 0;
    while (*t && o + 1 < size) {
        if (t[0] == '{' && t[1] >= '0' && t[1] <= '9' && t[2] == '}') {
            int i = t[1] - '0';
            if (i < c->n) {
                // A name the table knows comes in its translation too ("NEXT: LEADER ROXANNE").
                const char* from = c->at[i];
                int n = c->len[i];
                char name[MAX_TEXT_CAPTURE + 1];
                if (c->text[i] && n <= MAX_TEXT_CAPTURE) {
                    memcpy(name, from, (size_t) n);
                    name[n] = 0;
                    const char* named = exact(name);
                    if (named) from = named, n = (int) strlen(named);
                }
                for (int k = 0; k < n && o + 1 < size; k++) out[o++] = from[k];
            }
            t += 3;
            continue;
        }
        out[o++] = *t++;
    }
    out[o] = 0;
}

static const char* exact(const char* s) {
    int lo = 0, hi = pd_tr_fr_count - 1;
    while (lo <= hi) {
        int mid = (lo + hi) / 2, d = strcmp(s, pd_tr_fr[mid].en);
        if (!d) return pd_tr_fr[mid].tr;
        if (d < 0) hi = mid - 1;
        else lo = mid + 1;
    }
    return NULL;
}

const char* pd_tr(const char* s) {
    if (!lang || !s || !*s) return s;
    size_t len = strlen(s);
    if (len >= SRC_MAX) {
        const char* t = exact(s);
        return t ? t : s;
    }
    uint32_t h = hash_of(s);
    int slot = (int) (h % SLOTS);
    if (cache[slot].hash == h && !strcmp(cache[slot].src, s))
        return cache[slot].result ? cache[slot].result : s;
    cache[slot].hash = h;
    memcpy(cache[slot].src, s, len + 1);
    cache[slot].result = exact(s);
    if (!cache[slot].result) {
        for (int i = 0; i < pd_tr_fr_format_count; i++) {
            struct caps c = {0};
            // Cheap first: the format's text before its first conversion has to start the string.
            const char* f = pd_tr_fr_formats[i].en;
            const char* pct = strchr(f, '%');
            size_t head = pct ? (size_t) (pct - f) : strlen(f);
            if (strncmp(s, f, head)) continue;
            // At least a fifth of the string has to be the format's own words.
            size_t words = 0;
            for (const char* q = f; *q; q++) {
                if (*q == '%') {
                    bool number;
                    if (q[1] == '%') { words++; q++; continue; }
                    q += conversion(q, &number) - 1;
                    continue;
                }
                words++;
            }
            if (words * 5 < len) continue;
            if (match(s, f, &c)) {
                fill(cache[slot].out, OUT_MAX, pd_tr_fr_formats[i].tr, &c);
                cache[slot].result = cache[slot].out;
                break;
            }
        }
    }
    return cache[slot].result ? cache[slot].result : s;
}
