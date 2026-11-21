// The companion's text in another language: every string the canvas draws (pd_text and its kin) goes through pd_tr,
// which gives its translation when a language is set (English otherwise, and for what has none). A string the code
// formats is matched against the translated formats too ("%d steps left" -> "%d pas restants").
#ifndef PD_TR_H
#define PD_TR_H

// "fr", or NULL / "en" for English (the default).
void pd_tr_set_language(const char* code);
const char* pd_tr_language(void);
// [s] in the language set, or [s] itself. The result stays valid until about 256 other strings are translated.
const char* pd_tr(const char* s);

struct pd_tr_entry {
    const char* en;
    const char* tr;
};
// pd_tr_fr_gen.c: sorted by en (strcmp) for the exact strings; the formats apart ({0}, {1}.. in the translation take
// the English's %s / %d .. in order).
extern const struct pd_tr_entry pd_tr_fr[];
extern const int pd_tr_fr_count;
extern const struct pd_tr_entry pd_tr_fr_formats[];
extern const int pd_tr_fr_format_count;

#endif
