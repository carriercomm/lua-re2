#include <ctype.h> // for tolower()
#include <stdio.h> // for snprintf()
#include <re2/re2.h>
#include <re2/stringpiece.h>

#include "re2_c.h"

using namespace std;

#define likely(x)   __builtin_expect((x),1)
#define unlikely(x) __builtin_expect((x),0)

#ifdef DEBUG
    // Usage examples: ASSERT(a > b),  ASSERT(foo() && "Opps, foo() reutrn 0");
    #define ASSERT(c) if (!(c))\
        { fprintf(stderr, "%s:%d Assert: %s\n", __FILE__, __LINE__, #c); abort(); }
#else
    #define ASSERT(c) ((void)0)
#endif

/* This data structure is used to return some variable-length results back to
 * caller.
 */
struct re2c_match_aux {
    char* errstr;
    re2::StringPiece* captures;

    unsigned short errstr_buf_len;
    unsigned short cap_vect_len; /* the capacity of captures vector */
    unsigned short ncap; /* cache of RE2::NumberOfCapturingGroups() */
};

/* Return the "idx"-th capture. NOTE: Captures are not necessarily ended with
 * '\0'.
 */
const char*
re2c_get_capture(struct re2c_match_aux* aux, unsigned idx) {
    if (unlikely(!aux->captures))
        return 0;

    if (unlikely(aux->ncap <= idx))
        return 0;

    return aux->captures[idx].data();
}

unsigned
re2c_get_capture_len(struct re2c_match_aux* aux, unsigned idx) {
    if (unlikely(!aux->captures))
        return 0;

    if (unlikely(aux->ncap <= idx))
        return 0;

    return aux->captures[idx].size();
}

struct re2c_match_aux*
re2c_alloc_aux(void) {
    struct re2c_match_aux* p = new struct re2c_match_aux;
    p->errstr = 0;
    p->captures = 0;
    p->errstr_buf_len = 0;
    p->cap_vect_len = 0;
    p->ncap = 0;
    return p;
}

void
re2c_free_aux(struct re2c_match_aux* p) {
    delete[] p->errstr;
    delete[] p->captures;
    delete p;
}

const char*
re2c_get_errstr(struct re2c_match_aux* aux) {
    return aux->errstr;
}

static void
copy_errstr(char* buffer, int buf_len, const string& src) {
    if (!buffer)
        return;

    int copy_len = src.size();
    if (copy_len > buf_len - 1)
        copy_len = buf_len - 1;

    strncpy(buffer, src.c_str(), copy_len);
    buffer[copy_len] = '\0';
}

struct re2_pattern_t*
re2c_compile(const char* pattern, int pattern_len, const char* re2_options,
             char* errstr,  int errstrlen, unsigned max_mem) {
    const char* ptn_ptr = pattern;
    int ptn_len = pattern_len;

    // Process the options
    re2::RE2::Options opts;

    opts.set_log_errors(false);
    if (re2_options) {
        const char* p = re2_options;

        bool multiline = false;
        opts.set_perl_classes(true);
        opts.set_word_boundary(true);

        while (char c = *p++) {
            bool turn_on = true;
            if (c >= 'A' && c <= 'Z') {
                turn_on = false;
                c = tolower(c);
            }

            switch (c) {
            case 'u': opts.set_utf8(turn_on); break;
            case 'p': opts.set_posix_syntax(turn_on); break;
            case 'a': opts.set_longest_match(turn_on); break;
            case 'e': opts.set_log_errors(turn_on); break;
            case 'l': opts.set_literal(turn_on); break;
            case 'n': opts.set_never_nl(turn_on); break;
            case 's': opts.set_dot_nl(turn_on); break;
            case 'c': opts.set_never_capture(turn_on); break;
            case 'i': opts.set_case_sensitive(!turn_on); break;
            case 'm': multiline = true; break;
            default:
                {
                    fprintf(stderr, "unsupport flag\n");
                    string s = "unsupport flags ";
                    s += c;
                    copy_errstr(errstr, errstrlen, s);
                    return 0;
                }
            }
        }

        if (max_mem == 0) {max_mem = 2048 * 1024; }
        opts.set_max_mem(max_mem);

        opts.set_log_errors(true);

        // FIXME:one-line mode is always turned on in non-posix mode. To
        //  workaround the problem, we enclose the pattern with "(?m:...)"
        if (multiline) {
            const char* prefix = "(?m:";
            const char* postfix = ")";

            char* t;
            t = new char[ptn_len + strlen(prefix) + strlen(postfix) + 1];

            strcpy(t, prefix);
            memcpy(t + strlen(prefix), pattern, ptn_len);
            strcpy(t + strlen(prefix) + ptn_len, postfix);

            ptn_ptr = t;
            ptn_len += strlen(prefix) + strlen(postfix);
        }
    }

    // Now compile the pattern
    RE2* pat = new RE2(re2::StringPiece(ptn_ptr, ptn_len), opts);
    if (ptn_ptr != pattern)
        delete[] ptn_ptr;

    if (pat && !pat->ok()) {
        copy_errstr(errstr, errstrlen, pat->error());
        delete pat;
        return 0;
    }

    return (re2_pattern_t*)(void*)pat;
}

void
re2c_free(struct re2_pattern_t* pat) {
    delete (RE2*)(void*)pat;
}

/* Return the number of captures of the given pattern */
int
re2c_getncap(struct re2_pattern_t* pattern) {
    RE2* pat = reinterpret_cast<RE2*>(pattern);
    return pat->NumberOfCapturingGroups();
}

/* Return 0 if the pattern matches the given text, 1 otherwise. */
int
re2c_find(const char* text, int text_len, struct re2_pattern_t* pattern) {
    RE2* re2 = (RE2*)(void*)pattern;
    if (unlikely(!re2))
        return 1;

    bool match = re2->Match(re2::StringPiece(text, text_len),
                            0 /* startpos */, text_len /* endpos*/,
                            re2::RE2::UNANCHORED, 0, 0);

    return match ? 0 : 1;
}

/* Return 0 if the pattern matches the given text, 1 otherwise; captures are
 * returned via "aux".
 */
int
re2c_match(const char* text, int text_len, struct re2_pattern_t* pattern,
           struct re2c_match_aux* aux) {
    RE2* re2 = (RE2*)(void*)pattern;
    if (unlikely(!re2))
        return 1;

    int ncap = re2->NumberOfCapturingGroups() + 1;
    if (!aux->cap_vect_len || aux->cap_vect_len < ncap) {
        delete[] aux->captures;
        aux->captures = new re2::StringPiece[ncap];
        aux->cap_vect_len = ncap;
    }
    aux->ncap = ncap;

    bool match = re2->Match(re2::StringPiece(text, text_len),
                            0 /* startpos */, text_len /* endpos*/,
                            re2::RE2::UNANCHORED, aux->captures, ncap);
    return match ? 0 : 1;
}
