/*
 * ec - the EC (Enhanced C) preprocessor.
 *
 * EC is C with two extensions:
 *
 *   1. Single-bit access:        flags@3          -> (((flags) >> (3)) & 1)
 *                                flags@3 = 1;     -> (flags) = ((flags) & ~(...)) | (...)
 *                                flags@3 ^= 1;    -> (flags) ^= (1ULL << (3))
 *                                flags@3++;       -> (flags) ^= (1ULL << (3))    (toggle)
 *
 *   2. Inline assembler, BBC BASIC style:
 *
 *        ASM(cmp2, ULL, neg %rdi; mov %rdi, %rax; ret)         one-liner naked function
 *
 *        ASM ULL cmp3(ULL x) {          ... }                  naked function block
 *
 *        [                                                     inline block inside a
 *            movl %[a], %eax                                   C function body
 *            ret
 *        ]
 *
 * Everything else is copied through verbatim (strings, comments, preprocessor
 * directives are all respected), and #line directives keep compiler errors
 * pointing at the .ec file.
 *
 * Build: cc -O2 -o ec ec.c
 */
#include <ctype.h>
#include <stdarg.h>
#include <stdbool.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>

#define EC_VERSION "0.2.0"

/* ------------------------------------------------------------------ */
/* growable string                                                     */
/* ------------------------------------------------------------------ */

typedef struct {
    char  *p;
    size_t len, cap;
} SB;

static void *xmalloc(size_t n)
{
    void *p = malloc(n ? n : 1);
    if (!p) { fputs("ec: out of memory\n", stderr); exit(2); }
    return p;
}

static void *xrealloc(void *q, size_t n)
{
    void *p = realloc(q, n ? n : 1);
    if (!p) { fputs("ec: out of memory\n", stderr); exit(2); }
    return p;
}

static void sb_need(SB *b, size_t extra)
{
    if (b->len + extra + 1 > b->cap) {
        size_t c = b->cap ? b->cap * 2 : 256;
        while (c < b->len + extra + 1) c *= 2;
        b->p = xrealloc(b->p, c);
        b->cap = c;
    }
}

static void sb_add(SB *b, const char *s, size_t n)
{
    sb_need(b, n);
    memcpy(b->p + b->len, s, n);
    b->len += n;
    b->p[b->len] = '\0';
}

static void sb_putc(SB *b, char c) { sb_add(b, &c, 1); }
static void sb_puts(SB *b, const char *s) { sb_add(b, s, strlen(s)); }

static void sb_printf(SB *b, const char *fmt, ...)
{
    va_list ap;
    int n;
    va_start(ap, fmt);
    n = vsnprintf(NULL, 0, fmt, ap);
    va_end(ap);
    sb_need(b, (size_t)n);
    va_start(ap, fmt);
    vsnprintf(b->p + b->len, (size_t)n + 1, fmt, ap);
    va_end(ap);
    b->len += (size_t)n;
}

/* ------------------------------------------------------------------ */
/* compiler context                                                    */
/* ------------------------------------------------------------------ */

typedef struct {
    const char *file;      /* source file name (for diagnostics + #line) */
    size_t      line;      /* source line currently being read (1-based) */
    size_t      oline;     /* line currently being written (1-based)     */
    int         depth;     /* >0 inside a recursive (sub-expression) pass */
    int         nerr, nwarn;
    unsigned long uid;     /* unique id, used to mangle asm labels */
    bool         line_directives;
    bool         escape_pct;
    bool         verbose;
} EC;

/* copy source text: advances both line counters */
static void copyn(EC *ec, SB *o, const char *s, size_t n)
{
    for (size_t i = 0; i < n; i++)
        if (s[i] == '\n') { ec->line++; ec->oline++; }
    sb_add(o, s, n);
}

/* emit generated text: advances the output line counter only */
static void genn(EC *ec, SB *o, const char *s, size_t n)
{
    for (size_t i = 0; i < n; i++)
        if (s[i] == '\n') ec->oline++;
    sb_add(o, s, n);
}

static void genf(EC *ec, SB *o, const char *fmt, ...)
{
    va_list ap;
    int n;
    char *buf;
    va_start(ap, fmt);
    n = vsnprintf(NULL, 0, fmt, ap);
    va_end(ap);
    buf = xmalloc((size_t)n + 1);
    va_start(ap, fmt);
    vsnprintf(buf, (size_t)n + 1, fmt, ap);
    va_end(ap);
    genn(ec, o, buf, (size_t)n);
    free(buf);
}

/* drop source text (it was rewritten): advances the source line counter only */
static void skipn(EC *ec, const char *s, size_t n)
{
    for (size_t i = 0; i < n; i++)
        if (s[i] == '\n') ec->line++;
}

static void err(EC *ec, const char *fmt, ...)
{
    va_list ap;
    fprintf(stderr, "%s:%zu: error: ", ec->file, ec->line);
    va_start(ap, fmt);
    vfprintf(stderr, fmt, ap);
    va_end(ap);
    fputc('\n', stderr);
    ec->nerr++;
}

static void warn(EC *ec, const char *fmt, ...)
{
    va_list ap;
    fprintf(stderr, "%s:%zu: warning: ", ec->file, ec->line);
    va_start(ap, fmt);
    vfprintf(stderr, fmt, ap);
    va_end(ap);
    fputc('\n', stderr);
    ec->nwarn++;
}

/* resynchronise the two line counters with a #line directive */
static void sync_line(EC *ec, SB *o)
{
    char buf[1024];
    int k;
    if (ec->depth || !ec->line_directives) return;
    if (ec->oline == ec->line) return;
    k = snprintf(buf, sizeof buf, "#line %zu \"%s\"\n", ec->line, ec->file);
    genn(ec, o, buf, (size_t)k);
    ec->oline = ec->line - 1;   /* the directive occupies a line of its own */
}

/* ------------------------------------------------------------------ */
/* scanning helpers                                                    */
/* ------------------------------------------------------------------ */

static bool id_start(int c) { return isalpha(c) || c == '_' || c == '$'; }
static bool id_char(int c)  { return isalnum(c) || c == '_' || c == '$'; }

static size_t skip_ws(const char *s, size_t n, size_t i)
{
    while (i < n && isspace((unsigned char)s[i])) i++;
    return i;
}

/* skip whitespace and comments; stops *before* the newline ending a // comment */
static size_t skip_ws_com(const char *s, size_t n, size_t i)
{
    for (;;) {
        while (i < n && isspace((unsigned char)s[i]) && s[i] != '\n') i++;
        if (i + 1 < n && s[i] == '/' && s[i + 1] == '/') {
            while (i < n && s[i] != '\n') i++;
            continue;
        }
        if (i + 1 < n && s[i] == '/' && s[i + 1] == '*') {
            i += 2;
            while (i + 1 < n && !(s[i] == '*' && s[i + 1] == '/')) i++;
            i = (i + 1 < n) ? i + 2 : n;
            continue;
        }
        return i;
    }
}

/* skip blanks but never cross a line: prefix/postfix operators must stay put */
static size_t skip_blanks(const char *s, size_t n, size_t i)
{
    while (i < n && (s[i] == ' ' || s[i] == '\t' || s[i] == '\r')) i++;
    return i;
}

/* control keywords whose (...) is a statement header, not part of a term */
static bool header_keyword(const char *w)
{
    static const char *kw[] = { "if", "for", "while", "switch", NULL };
    for (int k = 0; kw[k]; k++) if (!strcmp(w, kw[k])) return true;
    return false;
}

static size_t line_start(const char *s, size_t i)
{
    while (i > 0 && s[i - 1] != '\n') i--;
    return i;
}

static bool at_bol(const char *s, size_t i)
{
    while (i > 0 && (s[i - 1] == ' ' || s[i - 1] == '\t' || s[i - 1] == '\r')) i--;
    return i == 0 || s[i - 1] == '\n';
}

/* is the cursor on a preprocessor directive line? */
static bool in_directive(const char *s, size_t i)
{
    size_t b = line_start(s, i);
    while (b < i && isspace((unsigned char)s[b])) b++;
    return b < i && s[b] == '#';
}

static size_t end_comment(const char *s, size_t n, size_t i)
{
    if (s[i + 1] == '/') {
        i += 2;
        while (i < n && s[i] != '\n') i++;
        return i;
    }
    i += 2;
    while (i + 1 < n && !(s[i] == '*' && s[i + 1] == '/')) i++;
    return (i + 1 < n) ? i + 2 : n;
}

static size_t end_literal(const char *s, size_t n, size_t i)
{
    char q = s[i++];
    while (i < n && s[i] != '\n') {
        if (s[i] == '\\' && i + 1 < n) { i += 2; continue; }
        if (s[i] == q) return i + 1;
        i++;
    }
    return i;
}

/* s[i] is (, [ or {: return index just past its match, or n if unbalanced */
static size_t match_group(const char *s, size_t n, size_t i)
{
    const char open = s[i], close = (open == '(') ? ')' : (open == '[') ? ']' : '}';
    int depth = 0;
    while (i < n) {
        char c = s[i];
        if (c == '"' || c == '\'') { i = end_literal(s, n, i); continue; }
        if (c == '/' && i + 1 < n && (s[i + 1] == '/' || s[i + 1] == '*')) { i = end_comment(s, n, i); continue; }
        if (c == open) depth++;
        else if (c == close) { if (--depth == 0) return i + 1; }
        i++;
    }
    return n;
}

static bool rest_of_line_blank(const char *s, size_t n, size_t i)
{
    size_t j = skip_ws_com(s, n, i);
    return j >= n || s[j] == '\n' || s[j] == '\r';
}

static size_t line_at(const char *s, size_t i)
{
    size_t l = 1;
    for (size_t k = 0; k < i; k++) if (s[k] == '\n') l++;
    return l;
}

static size_t nl_count(const char *s, size_t n)
{
    size_t c = 0;
    for (size_t i = 0; i < n; i++) if (s[i] == '\n') c++;
    return c;
}

static bool eq(const char *s, size_t n, const char *lit)
{
    size_t l = strlen(lit);
    return n >= l && memcmp(s, lit, l) == 0;
}

static char *strndup_(const char *s, size_t n)
{
    char *p = xmalloc(n + 1);
    memcpy(p, s, n);
    p[n] = '\0';
    return p;
}

/* allocate a trimmed copy of s[0..n) */
static char *trim_dup(const char *s, size_t n)
{
    size_t a = 0, b = n;
    while (a < b && isspace((unsigned char)s[a])) a++;
    while (b > a && isspace((unsigned char)s[b - 1])) b--;
    return strndup_(s + a, b - a);
}

/* ------------------------------------------------------------------ */
/* bit access:  expr@index                                             */
/* ------------------------------------------------------------------ */

/*
 * A bit index is one term: a number, an identifier (optionally followed by
 * member accesses / subscripts / a call), or a parenthesised expression.
 * Anything more complicated has to be parenthesised:  x@(i + 1)
 */
static size_t parse_index(const char *s, size_t n, size_t i)
{
    size_t j = i;
    if (j >= n) return i;
    if (s[j] == '(' || s[j] == '[') {
        j = match_group(s, n, j);
    } else if (id_start((unsigned char)s[j]) || isdigit((unsigned char)s[j])) {
        while (j < n && id_char((unsigned char)s[j])) j++;
    } else {
        return i;
    }
    for (;;) {
        if (j + 1 < n && s[j] == '-' && s[j + 1] == '>') {
            j += 2;
            while (j < n && id_char((unsigned char)s[j])) j++;
            continue;
        }
        if (j + 1 < n && s[j] == '.' && id_start((unsigned char)s[j + 1])) {
            j++;
            while (j < n && id_char((unsigned char)s[j])) j++;
            continue;
        }
        if (j < n && (s[j] == '[' || s[j] == '(')) { j = match_group(s, n, j); continue; }
        break;
    }
    return j;
}

/* span of a C "value" expression: up to ; , ) ] } at depth 0 */
static size_t parse_value(const char *s, size_t n, size_t i)
{
    size_t j = i;
    int depth = 0;
    while (j < n) {
        char c = s[j];
        if (c == '"' || c == '\'') { j = end_literal(s, n, j); continue; }
        if (c == '/' && j + 1 < n && (s[j + 1] == '/' || s[j + 1] == '*')) { j = end_comment(s, n, j); continue; }
        if (c == '(' || c == '[' || c == '{') { depth++; j++; continue; }
        if (c == ')' || c == ']' || c == '}') { if (depth == 0) return j; depth--; j++; continue; }
        if (depth == 0 && (c == ';' || c == ',')) return j;
        j++;
    }
    return j;
}

/* the identifier ending just before s[i], as a malloc'd string ("" if none) */
static char *prev_word(const char *s, size_t i)
{
    size_t b = i;
    while (b > 0 && isspace((unsigned char)s[b - 1])) b--;
    {
        size_t e = b;
        while (e > 0 && id_char((unsigned char)s[e - 1])) e--;
        return strndup_(s + e, b - e);
    }
}

/*
 * Where does the term ending at p[len] start?  Used to find the left operand
 * of '@' in the *output* buffer, so that nested constructs (x@1)@2 and
 * generated code inside an operand are handled without any extra bookkeeping.
 */
static size_t back_term(const char *p, size_t len)
{
    size_t j = len;

    /* blanks are tolerated between '@' and its operand ("x @3"), but never
       between the parts of the operand - that would swallow "return x" */
    while (j > 0 && isspace((unsigned char)p[j - 1])) j--;
    for (;;) {
        if (j > 0 && id_char((unsigned char)p[j - 1])) {
            while (j > 0 && id_char((unsigned char)p[j - 1])) j--;
            continue;
        }
        if (j > 0 && (p[j - 1] == ')' || p[j - 1] == ']')) {
            char close = p[j - 1], open = (close == ')') ? '(' : '[';
            int d = 0;
            size_t k = j;
            while (k > 0) {
                char c = p[k - 1];
                if (c == '"' || c == '\'') {           /* step over a literal */
                    k--;
                    while (k > 0) {
                        if (p[k - 1] == '\\' && k >= 2) { k -= 2; continue; }
                        if (p[k - 1] == c) { k--; break; }
                        k--;
                    }
                    continue;
                }
                if (c == close) d++;
                else if (c == open) { if (--d == 0) { k--; break; } }
                k--;
            }
            if (d != 0) return j;                        /* unbalanced: give up */
            {
                char *w = prev_word(p, k);
                bool hdr = header_keyword(w);
                free(w);
                if (hdr) return j;                       /* if/for/while/switch */
            }
            j = k;
            continue;
        }
        if (j >= 2 && p[j - 1] == '>' && p[j - 2] == '-') { j -= 2; continue; }
        if (j >= 2 && p[j - 1] == '.' && id_start((unsigned char)p[j - 2])) { j--; continue; }
        return j;
    }
}

/* length of the assignment operator at s[k], 0 if there is none */
static size_t assign_op_at(const char *s, size_t n, size_t k, char op[4])
{
    static const char *ops[] = { "<<=", ">>=", "+=", "-=", "*=", "/=", "%=",
                                 "&=", "^=", "|=", "=", NULL };
    for (int q = 0; ops[q]; q++) {
        size_t l = strlen(ops[q]);
        if (k + l <= n && memcmp(s + k, ops[q], l) == 0) {
            if (l == 1 && k + 1 < n && s[k + 1] == '=') continue;   /* '==' */
            memcpy(op, ops[q], l + 1);
            return l;
        }
    }
    op[0] = '\0';
    return 0;
}

/* pull the term that is about to be rewritten out of the output buffer */
static char *take_left(SB *out, size_t start)
{
    size_t a = start, b = out->len;
    char *r;
    if (a > b) a = b;
    while (a < b && isspace((unsigned char)out->p[a])) a++;
    while (b > a && isspace((unsigned char)out->p[b - 1])) b--;
    if (a == b) return NULL;
    r = strndup_(out->p + a, b - a);
    out->len = a;
    if (out->p) out->p[out->len] = '\0';
    return r;
}

static void process(EC *ec, const char *s, size_t n, SB *out);

/* recursively translate a fragment of EC source */
static void process_sub(EC *ec, const char *s, size_t n, SB *out, size_t line)
{
    size_t sl = ec->line, ol = ec->oline;

    ec->line = line;
    ec->oline = line;
    ec->depth++;
    process(ec, s, n, out);
    ec->depth--;

    ec->line = sl;
    ec->oline = ol;
}

/*
 * Rewrite one bit construct.
 *   at    - index of '@'
 *   start - index where the construct begins (== at, or the '++' for prefix ++)
 *   mode  - 0 normal, 1 toggle (++ / -- seen)
 */
static void do_bit(EC *ec, const char *s, size_t n, size_t *pi, SB *out, size_t at, size_t start, int toggle)
{
    size_t i = at + 1;
    size_t ie, k, ve = 0, end;
    char *X, *N = NULL, *V = NULL;
    char op[4] = "";
    SB val = {0};
    size_t nl;

    i = skip_ws(s, n, i);
    ie = parse_index(s, n, i);
    if (ie == i) {
        err(ec, "expected a bit index after '@' (try x@(expr))");
        copyn(ec, out, s + start, at + 1 - start);
        *pi = at + 1;
        return;
    }
    N = strndup_(s + i, ie - i);
    {
        SB tmp = {0};
        process_sub(ec, s + i, ie - i, &tmp, line_at(s, i));
        free(N);
        N = trim_dup(tmp.p ? tmp.p : "", tmp.len);
        free(tmp.p);
    }

    end = ie;
    k = skip_blanks(s, n, ie);
    if (!toggle && k + 1 < n && (s[k] == '+' || s[k] == '-') && s[k] == s[k + 1]) {
        toggle = 1;                       /* postfix ++ / -- : toggle the bit */
        end = k + 2;
    } else if (!toggle) {
        char aop[4];
        size_t al = assign_op_at(s, n, k, aop);
        if (al) {
            if (strcmp(aop, "=") && strcmp(aop, "|=") && strcmp(aop, "&=") && strcmp(aop, "^="))
                err(ec, "a single bit does not support '%s' (use '=', '|=', '&=', '^=' or '++')", aop);
            ve = parse_value(s, n, k + al);
            end = ve;
            process_sub(ec, s + k + al, ve - (k + al), &val, line_at(s, k + al));
            {
                size_t q = skip_ws_com(s, n, end);
                if (q < n && s[q] == ';') end = q + 1;
            }
            V = trim_dup(val.p ? val.p : "", val.len);
            free(val.p);
            memcpy(op, aop, al + 1);
        }
    }

    X = take_left(out, back_term(out->p, out->len));
    if (!X) {
        err(ec, "'@' with no expression on the left");
        free(N); free(V);
        copyn(ec, out, s + start, at + 1 - start);
        *pi = at + 1;
        return;
    }

    nl = nl_count(s + start, end - start);
    ec->line += nl;                     /* the newlines are re-emitted below */
    if (toggle) {
        genf(ec, out, "((%s) ^= (1ULL << (%s)))", X, N);
    } else if (op[0] == '=' && !op[1]) {
        genf(ec, out, "((%s) = ((%s) & ~(1ULL << (%s))) | ((unsigned long long)!!(%s) << (%s)))",
             X, X, N, V, N);
    } else if (op[0] == '|') {
        genf(ec, out, "((%s) |= ((unsigned long long)!!(%s) << (%s)))", X, V, N);
    } else if (op[0] == '&') {
        genf(ec, out, "((%s) &= ~((unsigned long long)!(%s) << (%s)))", X, V, N);
    } else if (op[0] == '^') {
        genf(ec, out, "((%s) ^= ((unsigned long long)!!(%s) << (%s)))", X, V, N);
    } else {
        genf(ec, out, "(((%s) >> (%s)) & 1)", X, N);
    }

    /* keep the output line count in step with the source */
    for (size_t q = 0; q < nl; q++) genn(ec, out, "\n", 1);
    if (end > start && s[end - 1] == ';') genn(ec, out, ";", 1);

    free(X); free(N); free(V);
    *pi = end;
}

/* ------------------------------------------------------------------ */
/* inline assembler                                                    */
/* ------------------------------------------------------------------ */

/*
 * Labels inside an asm block are turned into gas *numeric* local labels
 * (1:, 2:, ... referenced as 1b / 1f).  Named symbols cannot be used: gcc is
 * free to clone or inline the function holding the block, which would define
 * the same symbol twice.
 */
#define MAXLBL 128
static struct {
    const char *p;
    size_t      n;
    int         num;       /* the numeric label it was given */
    int         defline;   /* body line holding the definition, -1 until seen */
} labels[MAXLBL];
static int  nlabels;
static int  used_num[MAXLBL];      /* numeric labels the user wrote himself */
static int  nused;

static bool lbl_eq(int k, const char *w, size_t wl)
{
    return labels[k].n == wl && memcmp(labels[k].p, w, wl) == 0;
}

static int lbl_index(const char *w, size_t wl)
{
    for (int k = 0; k < nlabels; k++) if (lbl_eq(k, w, wl)) return k;
    return -1;
}

static void lbl_add(const char *p, size_t n)
{
    int num;
    bool clash;
    if (lbl_index(p, n) >= 0) return;
    if (nlabels >= MAXLBL) return;
    num = 1;
    do {
        clash = false;
        for (int k = 0; k < nused; k++) if (used_num[k] == num) clash = true;
        for (int k = 0; k < nlabels; k++) if (labels[k].num == num) clash = true;
        if (clash) num++;
    } while (clash);
    labels[nlabels].p = p;
    labels[nlabels].n = n;
    labels[nlabels].num = num;
    labels[nlabels].defline = -1;
    nlabels++;
}

static void num_add(int v)
{
    bool seen = false;
    for (int k = 0; k < nused; k++) if (used_num[k] == v) seen = true;
    if (!seen && nused < MAXLBL) used_num[nused++] = v;
}

/* the "12" of a user-written "12:" label, or -1 */
static int numeric_label(const char *p, size_t n)
{
    size_t j = 0;
    int v = 0;
    if (n < 2 || !isdigit((unsigned char)p[0])) return -1;
    while (j < n && isdigit((unsigned char)p[j])) { v = v * 10 + (p[j] - '0'); j++; }
    if (j + 1 != n || p[j] != ':') return -1;
    return v;
}

/* gas directives that must not be mistaken for BBC-style labels */
static bool gas_directive(const char *p, size_t n)
{
    static const char *d[] = {
        "text", "data", "bss", "section", "align", "p2align", "balign", "byte", "word",
        "long", "quad", "ascii", "asciz", "string", "globl", "global", "local", "type",
        "size", "zero", "space", "equ", "set", "comm", "extern", "fill", "skip", "arm",
        "thumb", "code16", "code32", "code64", "pushsection", "popsection", "hidden",
        NULL
    };
    for (int k = 0; d[k]; k++) if (eq(p, n, d[k])) return true;
    return false;
}

/* is this line a BBC-style ".label" definition? */
static bool bbc_label(const char *p, size_t n, size_t *noff, size_t *nlen)
{
    size_t j = 0;
    if (n < 2 || p[0] != '.') return false;
    j = 1;
    if (!id_start((unsigned char)p[j])) return false;
    while (j < n && id_char((unsigned char)p[j])) j++;
    if (j != n) return false;
    if (gas_directive(p + 1, n - 1)) return false;
    *noff = 1; *nlen = n - 1;
    return true;
}

/* is this line a gas-style "label:" definition? returns length of "label:" */
static size_t gas_label_prefix(const char *p, size_t n)
{
    size_t j = 0;
    bool numeric = true;
    if (n == 0) return 0;
    if (p[0] == '.') j = 1;
    if (j >= n || !(id_start((unsigned char)p[j]) || isdigit((unsigned char)p[j]))) return 0;
    while (j < n && id_char((unsigned char)p[j])) {
        if (!isdigit((unsigned char)p[j])) numeric = false;
        j++;
    }
    if (numeric) return 0;                  /* leave 1: / 2: local labels alone */
    if (j >= n || p[j] != ':') return 0;
    if (j + 1 < n && p[j + 1] == ':') return 0;    /* :: global */
    return j + 1;
}

typedef struct {
    char *expr;         /* the C expression, as written    */
    char  op[32];       /* operand name, for %[..]         */
    char  tmp[32];      /* 64-bit temporary holding it     */
    bool  flt;          /* declared float or double        */
    bool  addr;         /* used as an address: (a)         */
} Var;

static Var  vars[32];
static int  nvars;

static int var_find(const char *name, size_t l, bool addr)
{
    for (int k = 0; k < nvars; k++) {
        size_t nl = strlen(vars[k].expr);
        if (nl == l && memcmp(vars[k].expr, name, l) == 0 && vars[k].addr == addr)
            return k;
    }
    for (int k = 0; k < nvars; k++) {          /* fall back to the other kind */
        size_t nl = strlen(vars[k].expr);
        if (nl == l && memcmp(vars[k].expr, name, l) == 0) return k;
    }
    return -1;
}

static int var_add(const char *expr, size_t l, bool addr)
{
    for (int k = 0; k < nvars; k++) {
        size_t nl = strlen(vars[k].expr);
        if (nl == l && memcmp(vars[k].expr, expr, l) == 0 && vars[k].addr == addr)
            return k;
    }
    if (nvars >= (int)(sizeof vars / sizeof *vars)) return -1;
    vars[nvars].expr = strndup_(expr, l);
    vars[nvars].addr = addr;
    vars[nvars].flt = false;
    snprintf(vars[nvars].op, sizeof vars[nvars].op, "_ec_%s%d",
             addr ? "a" : "v", nvars);
    snprintf(vars[nvars].tmp, sizeof vars[nvars].tmp, "_ec_t%d", nvars);
    nvars++;
    return nvars - 1;
}

static void vars_clear(void)
{
    for (int k = 0; k < nvars; k++) free(vars[k].expr);
    nvars = 0;
}

static void emit_asm_body(EC *ec, SB *out, const char *s, size_t i, size_t end,
                          const char *indent, const char *operands);

static char *sb_join(const char *a, const char *b)
{
    size_t la = strlen(a), lb = strlen(b);
    char *r = xmalloc(la + lb + 1);
    memcpy(r, a, la);
    memcpy(r + la, b, lb + 1);
    return r;
}

static void asm_line_to_string(EC *ec, SB *out, const char *p, size_t n, bool ext, int curline)
{
    int depth = 0;
    sb_putc(out, '"');
    for (size_t i = 0; i < n; i++) {
        char c = p[i];
        if (c == '(' || c == '[') depth++;
        else if (c == ')' || c == ']') { if (depth) depth--; }
        if (c == '"' || c == '\\') {          /* keep the C string literal sane */
            sb_putc(out, '\\');
            sb_putc(out, c);
            continue;
        }
        if (c == '%' && ext && ec->escape_pct &&
            !(i + 1 < n && (p[i + 1] == '[' || p[i + 1] == '{' || p[i + 1] == '}' ||
                            p[i + 1] == '|' || p[i + 1] == '=' || p[i + 1] == '%' ||
                            p[i + 1] == 'l' || isdigit((unsigned char)p[i + 1])))) {
            sb_puts(out, "%%");
            continue;
        }
        /* symbolic labels -> mangled labels (skipping string literals) */
        if (nlabels && (id_start((unsigned char)c) || c == '.') ) {
            bool inq = false;
            for (size_t q = 0; q < i; q++) if (p[q] == '"') inq = !inq;
            if (!inq) {
                size_t w = i;
                if (c == '.') {
                    while (w < n && (id_char((unsigned char)p[w]) || p[w] == '.')) w++;
                } else {
                    while (w < n && id_char((unsigned char)p[w])) w++;
                }
                if (w < n && p[w] == ':') {
                    int li = lbl_index(p + i, w - i);
                    if (li >= 0) {                    /* a definition mid-line */
                        sb_printf(out, "%d:", labels[li].num);
                        labels[li].defline = curline;
                        i = w;                        /* ':' already emitted */
                        continue;
                    }
                } else {
                    int li = lbl_index(p + i, w - i);
                    if (li >= 0) {
                        int d = labels[li].defline;
                        sb_printf(out, "%d%c", labels[li].num,
                                  (d >= 0 && d <= curline) ? 'b' : 'f');
                        i = w - 1;
                        continue;
                    }
                }
            }
        }
        /* a C variable of the enclosing function, bound as an operand */
        if (nvars && id_start((unsigned char)c)) {
            size_t w = i + 1;                 /* a name may be s.f or p->f */
            while (w < n) {
                if (w + 1 < n && p[w] == '-' && p[w + 1] == '>') { w += 2; }
                else if (p[w] == '.' && w + 1 < n &&
                         id_start((unsigned char)p[w + 1])) { w++; }
                else if (!id_char((unsigned char)p[w])) break;
                else w++;
            }
            {
                int vi = var_find(p + i, w - i, depth > 0);
                if (vi >= 0) {
                    sb_printf(out, "%%[%s]", vars[vi].op);
                } else {
                    /* not one of ours: copy the word whole, so that the tail
                       of 'addsd' is never read as the variable 'd' */
                    sb_printf(out, "%.*s", (int)(w - i), p + i);
                }
                i = w - 1;
                continue;
            }
        }
        sb_putc(out, c);
    }
    sb_puts(out, "\\n\\t\"\n");
}

/* ------------------------------------------------------------------ */
/* registers, and the x86-64 calling convention                        */
/* ------------------------------------------------------------------ */

/*
 * Inside an assembler body EC knows the machine, so you can name registers
 * and arguments instead of writing gas punctuation:
 *
 *     mov RESULT, ARG1     instead of     mov %rax, %rdi
 *     add x, 3             (x being the first parameter of the function)
 *     mov RAX, #255        instead of     mov %rax, $255
 *
 * Only upper-case words are recognised, and only inside an assembler body,
 * so no ordinary C identifier is ever at risk.  \NAME leaves one word alone.
 */
static const char *const regnames[] = {
    "AH", "AL", "AX", "BH", "BL", "BP", "BPL", "BX", "CH", "CL", "CX", "DH",
    "DI", "DIL", "DL", "DX", "EAX", "EBP", "EBX", "ECX", "EDI", "EDX", "ESI",
    "ESP", "R10", "R10B", "R10D", "R10W", "R11", "R11B", "R11D", "R11W",
    "R12", "R12B", "R12D", "R12W", "R13", "R13B", "R13D", "R13W", "R14",
    "R14B", "R14D", "R14W", "R15", "R15B", "R15D", "R15W", "R8", "R8B",
    "R8D", "R8W", "R9", "R9B", "R9D", "R9W", "RAX", "RBP", "RBX", "RCX",
    "RDI", "RDX", "RIP", "RSI", "RSP", "SI", "SIL", "SP", "SPL", "XMM0",
    "XMM1", "XMM10", "XMM11", "XMM12", "XMM13", "XMM14", "XMM15", "XMM2",
    "XMM3", "XMM4", "XMM5", "XMM6", "XMM7", "XMM8", "XMM9", "YMM0", "YMM1",
    "YMM10", "YMM11", "YMM12", "YMM13", "YMM14", "YMM15", "YMM2", "YMM3",
    "YMM4", "YMM5", "YMM6", "YMM7", "YMM8", "YMM9"
};

static const char *const type_words[] = {
    "void", "char", "short", "int", "long", "signed", "unsigned", "float",
    "double", "const", "volatile", "restrict", "struct", "union", "enum",
    "_Bool", "_Complex", "_Atomic", "__int128", "__restrict", NULL
};

/* SysV AMD64 (Linux, macOS, BSD) and the Windows x64 convention */
typedef struct {
    const char *args[8];    int nargs;      /* integer/pointer arguments  */
    const char *fargs[8];   int nfargs;     /* floating point arguments   */
    const char *result, *fresult;
    const char *scratch[4]; int nscratch;   /* free temporaries           */
} ABI;

static const ABI abi_sysv = {
    { "%rdi", "%rsi", "%rdx", "%rcx", "%r8", "%r9" }, 6,
    { "%xmm0", "%xmm1", "%xmm2", "%xmm3",
      "%xmm4", "%xmm5", "%xmm6", "%xmm7" }, 8,
    "%rax", "%xmm0", { "%r10", "%r11" }, 2
};

static const ABI abi_win64 = {
    { "%rcx", "%rdx", "%r8", "%r9" }, 4,
    { "%xmm0", "%xmm1", "%xmm2", "%xmm3" }, 4,
    "%rax", "%xmm0", { "%r10", "%r11" }, 2
};

static const ABI *abi = &abi_sysv;
static bool opt_regnames = true;
static bool opt_intel = false;      /* .intel_syntax noprefix inside blocks */

/* name -> register, for the parameters of the naked function being emitted */
static struct { char *name; const char *reg; } pnames[16];
static int npnames;

static bool reg_lookup(const char *w, char out[24])
{
    for (size_t k = 0; k < sizeof regnames / sizeof *regnames; k++) {
        if (strcmp(w, regnames[k]) == 0) {
            size_t q, o = 0;
            if (!opt_intel) out[o++] = '%';
            for (q = 0; regnames[k][q]; q++)
                out[o + q] = (char)tolower((unsigned char)regnames[k][q]);
            out[o + q] = '\0';
            return true;
        }
    }
    return false;
}

static bool all_digits(const char *w)
{
    if (!*w) return false;
    for (const char *q = w; *q; q++) if (!isdigit((unsigned char)*q)) return false;
    return true;
}

/* ARG1..ARG8, FARG1..FARG8, SCRATCH1..SCRATCH4, RESULT, FRESULT */
static const char *abi_lookup(const char *w)
{
    static const char *pre[] = { "ARG", "FARG", "SCRATCH" };
    for (int k = 0; k < 3; k++) {
        size_t l = strlen(pre[k]);
        if (strncmp(w, pre[k], l) == 0 && all_digits(w + l)) {
            int i = atoi(w + l);
            if (k == 0) return (i >= 1 && i <= abi->nargs) ? abi->args[i - 1] : NULL;
            if (k == 1) return (i >= 1 && i <= abi->nfargs) ? abi->fargs[i - 1] : NULL;
            return (i >= 1 && i <= abi->nscratch) ? abi->scratch[i - 1] : NULL;
        }
    }
    if (strcmp(w, "RESULT") == 0)  return abi->result;
    if (strcmp(w, "FRESULT") == 0) return abi->fresult;
    return NULL;
}

/* the replacement for w, or NULL if w is not a name EC knows */
static const char *name_lookup(const char *w, char buf[24])
{
    const char *r;
    for (int k = 0; k < npnames; k++)
        if (strcmp(w, pnames[k].name) == 0) return pnames[k].reg;
    if (reg_lookup(w, buf)) return buf;
    r = abi_lookup(w);
    return r;
}

static bool is_type_word(const char *w)
{
    for (int k = 0; type_words[k]; k++) if (strcmp(w, type_words[k]) == 0) return true;
    return false;
}

/* the declared name of a parameter ("const char *s" -> "s"), or NULL */
static char *param_name(const char *t)
{
    size_t n = strlen(t), i = 0, nidents = 0;
    bool punct = strchr(t, '*') || strchr(t, '[') || strchr(t, '(');
    char *last = NULL;

    while (i < n) {
        if (id_start((unsigned char)t[i])) {
            size_t w = i;
            char *wrd;
            while (w < n && id_char((unsigned char)t[w])) w++;
            wrd = strndup_(t + i, w - i);
            nidents++;
            if (!is_type_word(wrd)) { free(last); last = wrd; }
            else free(wrd);
            i = w;
            continue;
        }
        i++;
    }
    if (!last) return NULL;
    if (nidents < 2 && !punct) { free(last); return NULL; }   /* just a type */
    return last;
}

/* 0 = integer/pointer, 1 = SSE, -1 = something EC will not guess about */
static int param_class(const char *t)
{
    if (strstr(t, "struct") || strstr(t, "union") || strstr(t, "long double"))
        return -1;
    if (strstr(t, "float") || strstr(t, "double") || strstr(t, "_Float") ||
        strstr(t, "__m"))
        return 1;
    return 0;
}

/* the (...) of a function declarator, or false if there is not one */
static bool param_range(const char *sig, size_t n, size_t *a, size_t *b)
{
    size_t k = 0, first = 0;
    int depth = 0;
    while (k < n) {
        char c = sig[k];
        if (c == '"' || c == '\'') { k = end_literal(sig, n, k); continue; }
        if (c == '(') {
            if (depth == 0 && first == 0) first = k + 1;
            depth++;
            k++;
            continue;
        }
        if (c == ')') {
            if (depth > 0 && --depth == 0) { *a = first; *b = k; return true; }
            k++;
            continue;
        }
        k++;
    }
    return false;
}

/* map the parameters of a naked function onto their ABI registers */
static void abi_params(EC *ec, const char *sig, size_t n)
{
    size_t a = 0, b = 0, p;
    int ni = 0, nf = 0;

    for (int k = 0; k < npnames; k++) free(pnames[k].name);
    npnames = 0;
    if (!param_range(sig, n, &a, &b)) return;

    p = a;
    while (p < b) {
        size_t q = p;
        int d = 0;
        while (q < b) {
            char c = sig[q];
            if (c == '"' || c == '\'') { q = end_literal(sig, b, q); continue; }
            if (c == '(' || c == '[' || c == '{') d++;
            else if (c == ')' || c == ']' || c == '}') d--;
            else if (c == ',' && d == 0) break;
            q++;
        }
        {
            char *one = trim_dup(sig + p, q - p);
            if (*one && strcmp(one, "void") && strcmp(one, "...")) {
                char *nm = param_name(one);
                int cls = param_class(one);
                const char *reg = NULL;
                if (cls == 0 && ni < abi->nargs)       reg = abi->args[ni++];
                else if (cls == 1 && nf < abi->nfargs) reg = abi->fargs[nf++];
                if (nm && reg && npnames < (int)(sizeof pnames / sizeof *pnames)) {
                    pnames[npnames].name = nm;
                    pnames[npnames].reg = reg;
                    npnames++;
                } else {
                    if (nm && !reg)
                        warn(ec, "parameter '%s' is not passed in a register; "
                                 "use its stack slot by hand", nm);
                    free(nm);
                }
            }
            free(one);
        }
        p = q + 1;
    }
}

/* rewrite one assembler line: register names, argument names, '#' immediates */
static char *asm_subst(const char *p, size_t n)
{
    SB o = {0};

    for (size_t i = 0; i < n; ) {
        char c = p[i];
        if (c == '"' || c == '\'') {                 /* literals are sacred */
            size_t e = end_literal(p, n, i);
            sb_add(&o, p + i, e - i);
            i = e;
            continue;
        }
        if (c == '\\' && i + 1 < n && id_start((unsigned char)p[i + 1])) {
            i++;                                     /* \RAX stays RAX */
            {
                size_t w = i;
                while (w < n && id_char((unsigned char)p[w])) w++;
                sb_add(&o, p + i, w - i);
                i = w;
            }
            continue;
        }
        if (c == '#' && i + 1 < n && !isspace((unsigned char)p[i + 1]) &&
            p[i + 1] != '#') {                       /* #255 -> $255 (or 255) */
            if (!opt_intel) sb_putc(&o, '$');
            i++;
            continue;
        }
        if (id_start((unsigned char)c)) {
            size_t w = i;
            while (w < n && id_char((unsigned char)p[w])) w++;
            if (w - i < 24) {
                char nm[24], rep[24];
                memcpy(nm, p + i, w - i);
                nm[w - i] = '\0';
                {
                    const char *r = name_lookup(nm, rep);
                    if (r) sb_puts(&o, (opt_intel && r[0] == '%') ? r + 1 : r);
                    else   sb_add(&o, p + i, w - i);
                }
            } else {
                sb_add(&o, p + i, w - i);
            }
            i = w;
            continue;
        }
        sb_putc(&o, c);
        i++;
    }
    if (!o.p) sb_putc(&o, '\0');
    return o.p;
}

/* ------------------------------------------------------------------ */
/* [[ ... ]] : assembler in the middle of C, talking about C variables  */
/* ------------------------------------------------------------------ */

/*
 * A [[ ]] block is a statement like any other, but any name in it that is not
 * a register, a label or the mnemonic is taken to be a C variable, and EC
 * binds it to an operand for you - loaded before, stored after:
 *
 *     int a = 4;
 *     [[ add #3, a ]]                 -> a += 3
 *     [[ mov a, ARG1
 *        add #3, ARG1 ]]              -> a += 3  (ARG1 stands in for a, and
 *                                        EC hands the value back at the end)
 *
 * A name inside parentheses is an address:  movb #65, (buf)  is buf[0] = 'A'.
 * Registers you use are added to the clobber list automatically.
 */
/* was this name declared float or double anywhere in the file? */
static bool declared_float(const char *s, size_t n, const char *name)
{
    size_t l = strlen(name);
    for (size_t i = 0; i < n; ) {
        if (id_start((unsigned char)s[i])) {
            size_t w = i;
            while (w < n && id_char((unsigned char)s[w])) w++;
            {
                char *kw = strndup_(s + i, w - i);
                bool isf = !strcmp(kw, "float") || !strcmp(kw, "double") ||
                           !strcmp(kw, "_Float32") || !strcmp(kw, "_Float64");
                free(kw);
                if (isf) {
                    size_t q = w;
                    while (q < n && !strchr(";){=", s[q])) {
                        if (id_start((unsigned char)s[q])) {
                            size_t e = q;
                            while (e < n && id_char((unsigned char)s[e])) e++;
                            if (e - q == l && memcmp(s + q, name, l) == 0)
                                return true;
                            q = e;
                            continue;
                        }
                        q++;
                    }
                }
            }
            i = w;
            continue;
        }
        i++;
    }
    return false;
}

static bool ieq(const char *a, const char *b, size_t l)   /* case-insensitive */
{
    for (size_t k = 0; k < l && a[k] && b[k]; k++)
        if (tolower((unsigned char)a[k]) != tolower((unsigned char)b[k])) return false;
    return strlen(b) == l;
}

/* a register name, whatever the case: keeps gas spellings inside a memory
   operand - 8(%rip), (%rsp) - out of the variable list */
static bool regname_nc(const char *w, size_t l)
{
    for (size_t k = 0; k < sizeof regnames / sizeof *regnames; k++)
        if (ieq(w, regnames[k], l)) return true;
    return false;
}

/* lines whose operands are code, not data: jumps, calls, directives */
static bool no_operands(const char *m, size_t l)
{
    if (l == 0) return true;
    if (m[0] == '.') return true;                       /* .byte, .align, ... */
    if (m[0] == 'j' || m[0] == 'J') return true;        /* jmp, jne, ...      */
    {
        static const char *kw[] = { "call", "callq", "ret", "leave", "nop",
                                    "loop", "loope", "loopne", "int", "syscall",
                                    "sysenter", "ud2", "hlt", NULL };
        char *w = strndup_(m, l);
        bool hit = false;
        for (int k = 0; kw[k]; k++) if (!strcmp(w, kw[k])) hit = true;
        free(w);
        return hit;
    }
}

static bool body_has_line(const char *body, size_t blen, const char *line)
{
    for (size_t p = 0; p <= blen; ) {
        size_t e = p;
        while (e < blen && body[e] != '\n') e++;
        {
            size_t a = p, b = e;
            while (a < b && isspace((unsigned char)body[a])) a++;
            while (b > a && isspace((unsigned char)body[b - 1])) b--;
            if (b - a == strlen(line) && !memcmp(body + a, line, b - a)) return true;
        }
        if (e >= blen) break;
        p = e + 1;
    }
    return false;
}

static void do_inline_asm(EC *ec, const char *s, size_t n, size_t *pi, SB *out)
{
    size_t i = *pi;                       /* at the first '[' */
    size_t bend = 0, after = 0, body_start, p;
    char *indent;
    SB body = {0}, ops = {0};
    char regs[64][24];
    char bor_var[32][128], bor_reg[32][24];
    int nregs = 0, nbor = 0, nval = 0;

    /* the body runs from just after '[[' to the next ']]', which may be on
       the same line or on a line of its own */
    bend = 0;
    for (p = i + 2; p + 1 < n; p++)
        if (s[p] == ']' && s[p + 1] == ']') { bend = p; break; }
    if (!bend) {
        err(ec, "'[[' block is never closed by ']]'");
        copyn(ec, out, s + i, 1);
        *pi = i + 1;
        return;
    }
    {
        size_t q = bend + 2;                  /* swallow ']]' and the rest of
                                                 its line, ';' included */
        while (q < n && s[q] != '\n') q++;
        after = (q < n) ? q + 1 : n;
    }

    /* reproduce the indentation of the opening line (the main loop copied it) */
    {
        size_t b = line_start(s, i), a = b;
        while (a < i && isspace((unsigned char)s[a]) && s[a] != '\n') a++;
        indent = strndup_(s + b, a - b);
        if (a - b > 0 && out->len >= (size_t)(a - b)) out->len -= (size_t)(a - b);
    }

    body_start = i + 2;
    if (body_start > bend) body_start = bend;

    /* ---- pass 1: labels, so that their names are not taken for variables -- */
    nlabels = 0;
    for (p = body_start; p < bend; ) {
        size_t le = p, a, b;
        while (le < bend && s[le] != '\n') le++;
        a = p; b = le;
        while (a < b && isspace((unsigned char)s[a])) a++;
        while (b > a && isspace((unsigned char)s[b - 1])) b--;
        {
            size_t noff, nlen;
            if (a < b && bbc_label(s + a, b - a, &noff, &nlen))
                lbl_add(s + a + noff, nlen);
            else if (a < b && gas_label_prefix(s + a, b - a) == b - a &&
                     gas_label_prefix(s + a, b - a) > 0)
                lbl_add(s + a, gas_label_prefix(s + a, b - a) - 1);
            else if (a < b && numeric_label(s + a, b - a) >= 0)
                num_add(numeric_label(s + a, b - a));
        }
        if (le >= bend) break;
        p = le + 1;
    }

    /* ---- pass 2: bind C names, note registers, spot borrows -------------- */
    vars_clear();
    for (p = body_start; p < bend; ) {
        size_t le = p, a, b;
        while (le < bend && s[le] != '\n') le++;
        a = p; b = le;
        while (a < b && isspace((unsigned char)s[a])) a++;
        while (b > a && isspace((unsigned char)s[b - 1])) b--;
        if (a < b) {
            size_t ae = b;                        /* where analysis stops */
            for (size_t q = a; q + 1 < b; q++) {
                if (s[q] == '/' && (s[q + 1] == '/' || s[q + 1] == '*')) { ae = q; break; }
                if (s[q] == '#' && isspace((unsigned char)s[q + 1])) { ae = q; break; }
            }
            {
                struct { const char *t; size_t l; int kind; int depth; char pre;
                         char reg[24]; } tok[24];
                int ntok = 0, depth = 0;
                size_t q = a;
                while (q < ae && ntok < 24) {
                    char c = s[q];
                    if (c == '"' || c == '\'') { q = end_literal(s, ae, q); continue; }
                    if (c == '(' || c == '[') { depth++; q++; continue; }
                    if (c == ')' || c == ']') { if (depth) depth--; q++; continue; }
                    if (c == '\\' && q + 1 < ae && id_start((unsigned char)s[q + 1])) {
                        q++;
                        while (q < ae && id_char((unsigned char)s[q])) q++;
                        continue;
                    }
                    if (id_start((unsigned char)c)) {
                        size_t w = q;
                        for (;;) {                       /* a.b and a->b too */
                            while (w < ae && id_char((unsigned char)s[w])) w++;
                            if (w + 1 < ae && s[w] == '.' && id_start((unsigned char)s[w + 1])) { w++; continue; }
                            if (w + 2 < ae && s[w] == '-' && s[w + 1] == '>' &&
                                id_start((unsigned char)s[w + 2])) { w += 2; continue; }
                            break;
                        }
                        tok[ntok].t = s + q;
                        tok[ntok].l = w - q;
                        tok[ntok].kind = 0;
                        tok[ntok].depth = depth;
                        tok[ntok].pre = 0;
                        for (size_t z = q; z > a; z--)       /* '%rip' and co */
                            if (!isspace((unsigned char)s[z - 1])) {
                                tok[ntok].pre = s[z - 1];
                                break;
                            }
                        tok[ntok].reg[0] = '\0';
                        ntok++;
                        q = w;
                        continue;
                    }
                    q++;
                }
                if (ntok && !no_operands(tok[0].t, tok[0].l)) {
                    for (int k = 1; k < ntok; k++) {
                        char nm[128], rep[24];
                        size_t l = tok[k].l < sizeof nm - 1 ? tok[k].l : sizeof nm - 1;
                        memcpy(nm, tok[k].t, l);
                        nm[l] = '\0';
                        {
                            const char *r = name_lookup(nm, rep);
                            if (r) {
                                tok[k].kind = 1;
                                snprintf(tok[k].reg, sizeof tok[k].reg, "%s",
                                         r[0] == '%' ? r + 1 : r);
                            } else if (lbl_index(nm, l) >= 0) {
                                tok[k].kind = 0;                /* a label */
                            } else if (tok[k].pre == '%' ||
                                       (tok[k].depth > 0 && regname_nc(nm, l))) {
                                tok[k].kind = 0;      /* %rip, (%rsp): gas */
                            } else {
                                tok[k].kind = 2;
                                var_add(nm, l, tok[k].depth > 0);
                            }
                        }
                    }
                }
                for (int k = 1; k < ntok; k++)
                    if (tok[k].kind == 1) {
                        bool seen = false;
                        for (int j = 0; j < nregs; j++)
                            if (!strcmp(regs[j], tok[k].reg)) seen = true;
                        if (!seen && nregs < 64 &&
                            strcmp(tok[k].reg, "rsp") && strcmp(tok[k].reg, "rbp"))
                            snprintf(regs[nregs++], sizeof regs[0], "%s", tok[k].reg);
                    }
                /* "mov var, reg" or "mov reg, var": the register stands in for
                   the variable, so hand the value back at the end of the block */
                if (ntok == 3 && ieq(tok[0].t, "mov", tok[0].l) &&
                    ((tok[1].kind == 2 && tok[2].kind == 1) ||
                     (tok[1].kind == 1 && tok[2].kind == 2))) {
                    int vk = (tok[1].kind == 2) ? 1 : 2;
                    int rk = (tok[1].kind == 2) ? 2 : 1;
                    size_t l = tok[vk].l < sizeof bor_var[0] - 1
                             ? tok[vk].l : sizeof bor_var[0] - 2;
                    if (nbor < 32 && var_find(tok[vk].t, tok[vk].l, false) >= 0) {
                        memcpy(bor_var[nbor], tok[vk].t, l);
                        bor_var[nbor][l] = '\0';
                        {
                            size_t rl = tok[rk].l < sizeof bor_reg[0] - 1
                                      ? tok[rk].l : sizeof bor_reg[0] - 2;
                            memcpy(bor_reg[nbor], tok[rk].t, rl);
                            bor_reg[nbor][rl] = '\0';
                        }
                        nbor++;
                    }
                }
            }
        }
        if (le >= bend) break;
        p = le + 1;
    }

    for (int k = 0; k < nvars; k++)
        if (!vars[k].addr) vars[k].flt = declared_float(s, n, vars[k].expr);

    /* ---- the body, with borrowed values handed back ---------------------- */
    sb_add(&body, s + body_start, bend - body_start);
    if (body.len && body.p[body.len - 1] != '\n') sb_putc(&body, '\n');
    for (int k = 0; k < nbor; k++) {
        SB ln = {0};
        if (opt_intel) sb_printf(&ln, "mov %s, %s", bor_var[k], bor_reg[k]);
        else           sb_printf(&ln, "mov %s, %s", bor_reg[k], bor_var[k]);
        if (!body_has_line(body.p, body.len, ln.p))
            sb_printf(&body, "%s\n", ln.p);
        free(ln.p);
    }

    /* ---- the operand list ------------------------------------------------ */
    sb_puts(&ops, ": ");
    for (int k = 0, first = 1; k < nvars; k++)
        if (!vars[k].addr) {
            sb_printf(&ops, "%s[%s] \"+%s\"(%s)", first ? "" : ", ",
                      vars[k].op, vars[k].flt ? "x" : "r", vars[k].tmp);
            first = 0;
        }
    sb_puts(&ops, " : ");
    for (int k = 0, first = 1; k < nvars; k++)
        if (vars[k].addr) {
            sb_printf(&ops, "%s[%s] \"r\"(&%s)", first ? "" : ", ",
                      vars[k].op, vars[k].expr);
            first = 0;
        }
    sb_puts(&ops, " : \"cc\"");
    for (int k = 0; k < nregs; k++) sb_printf(&ops, ", \"%s\"", regs[k]);
    {                                   /* (name) writes through a pointer,
                                           which the compiler cannot see */
        bool mem = false;
        for (int k = 0; k < nvars; k++) if (vars[k].addr) mem = true;
        if (mem) sb_puts(&ops, ", \"memory\"");
    }

    /* ---- emit ------------------------------------------------------------ */
    for (int k = 0; k < nvars; k++) if (!vars[k].addr) nval++;
    skipn(ec, s + i, after - i);            /* swallow the whole construct */
    if (nval) {
        genf(ec, out, "%s{\n", indent);
        for (int k = 0; k < nvars; k++)
            if (!vars[k].addr)
                genf(ec, out, "%s    %s %s = (%s)(%s);\n", indent,
                     vars[k].flt ? "double" : "unsigned long long",
                     vars[k].tmp,
                     vars[k].flt ? "double" : "unsigned long long",
                     vars[k].expr);
    }
    {
        char *deep = nval ? sb_join(indent, "    ") : indent;
        emit_asm_body(ec, out, body.p, 0, body.len, deep, ops.p);
        if (nval) free(deep);
    }
    if (nval) {
        for (int k = 0; k < nvars; k++)
            if (!vars[k].addr)
                genf(ec, out, "%s    %s = (__typeof__(%s))%s;\n", indent,
                     vars[k].expr, vars[k].expr, vars[k].tmp);
        genf(ec, out, "%s}\n", indent);
    }
    vars_clear();
    sync_line(ec, out);
    free(indent);
    free(body.p);
    free(ops.p);
    *pi = after;
}


/*
 * Translate an assembler body (source range [i,end)) and emit an asm statement.
 *   indent   - leading whitespace to reproduce
 *   operands - NULL or the operand list for extended asm
 *   tail     - what to emit after the closing paren (e.g. ");" or ");")
 */
static void emit_asm_body(EC *ec, SB *out, const char *s, size_t i, size_t end,
                          const char *indent, const char *operands)
{
    size_t p = i;
    bool ext = operands && *operands;
    int nlines = 0;

    /* pass 1: collect label definitions */
    nlabels = 0;
    nused = 0;
    for (size_t q = i; q < end; ) {
        size_t e = q;
        while (e < end && s[e] != '\n') e++;
        {
            size_t a = q, b = e;
            while (a < b && isspace((unsigned char)s[a])) a++;
            while (b > a && isspace((unsigned char)s[b - 1])) b--;
            size_t noff, nlen;
            if (a < b && bbc_label(s + a, b - a, &noff, &nlen)) {
                lbl_add(s + a + noff, nlen);
            } else if (a < b && gas_label_prefix(s + a, b - a) == b - a &&
                       gas_label_prefix(s + a, b - a) > 0) {
                size_t pl = gas_label_prefix(s + a, b - a);
                lbl_add(s + a, pl - 1);
            } else if (a < b && numeric_label(s + a, b - a) >= 0) {
                num_add(numeric_label(s + a, b - a));
            }
        }
        q = (e < end) ? e + 1 : e;
    }

    /* pass 2: emit */
    sb_printf(out, "%s__asm__ volatile(\n", indent);
    ec->oline++;
    if (opt_intel) {
        sb_printf(out, "%s    \".intel_syntax noprefix\\n\\t\"\n", indent);
        ec->oline++;
        nlines++;
        if (ext && (strstr(operands, "\"m\"") || strstr(operands, "=m\"") ||
                    strstr(operands, "+m\"")))
            warn(ec, "intel syntax with a memory operand: gcc writes those in "
                     "AT&T syntax, so 'm' constraints will not assemble");
    }
    p = i;
    while (p < end) {
        size_t e = p;
        while (e < end && s[e] != '\n') e++;
        {
            size_t a = p, b = e;
            while (a < b && isspace((unsigned char)s[a])) a++;
            while (b > a && isspace((unsigned char)s[b - 1])) b--;
            if (a < b) {
                size_t noff, nlen, pl;
                sb_printf(out, "%s    ", indent);
                if (bbc_label(s + a, b - a, &noff, &nlen)) {
                    int li = lbl_index(s + a + noff, nlen);
                    sb_printf(out, "\"%d:\\n\\t\"\n", li >= 0 ? labels[li].num : 1);
                    if (li >= 0) labels[li].defline = (int)nlines;
                    ec->oline++;
                    nlines++;
                } else if ((pl = gas_label_prefix(s + a, b - a)) && pl == b - a) {
                    int li = lbl_index(s + a, pl - 1);
                    sb_printf(out, "\"%d:\\n\\t\"\n", li >= 0 ? labels[li].num : 1);
                    if (li >= 0) labels[li].defline = (int)nlines;
                    ec->oline++;
                    nlines++;
                } else {
                    char *line = opt_regnames ? asm_subst(s + a, b - a)
                                              : strndup_(s + a, b - a);
                    asm_line_to_string(ec, out, line, strlen(line), ext, (int)nlines);
                    free(line);
                    ec->oline++;
                    nlines++;
                }
            }
        }
        p = (e < end) ? e + 1 : e;
    }
    if (opt_intel) {
        sb_printf(out, "%s    \".att_syntax\\n\\t\"\n", indent);
        ec->oline++;
        nlines++;
    }
    if (nlines == 0) {
        sb_printf(out, "%s    \"\\n\"\n", indent);
        ec->oline++;
    }
    if (ext) {
        if (*operands == '"') sb_printf(out, "%s    : ", indent);
        else                  sb_printf(out, "%s    ", indent);
        genn(ec, out, operands, strlen(operands));
        genn(ec, out, "\n", 1);
    }
    sb_printf(out, "%s);\n", indent);
    ec->oline++;
}

/* find the end of an asm body: the line whose first token is `close` */
static bool find_body_end(const char *s, size_t n, size_t i, char close,
                          size_t *bend, size_t *after)
{
    size_t p = i;
    while (p <= n) {
        size_t e = p, a, b;
        while (e < n && s[e] != '\n') e++;
        a = p; b = e;
        while (a < b && isspace((unsigned char)s[a])) a++;
        while (b > a && isspace((unsigned char)s[b - 1])) b--;
        if (a < b && s[a] == close) {
            *bend = p;
            *after = (e < n) ? e + 1 : e;
            return true;
        }
        if (e >= n) return false;
        p = e + 1;
    }
    return false;
}

static void do_inline_asm(EC *ec, const char *s, size_t n, size_t *pi, SB *out);

/* [ ... ] block inside a C function body */
static void do_bracket_asm(EC *ec, const char *s, size_t n, size_t *pi, SB *out)
{
    size_t i = *pi;                     /* at '[' */
    size_t bend, after, body_start, e;
    char *indent, *operands = NULL;

    if (!find_body_end(s, n, i + 1, ']', &bend, &after)) {
        err(ec, "'[' assembler block is never closed by a line starting with ']'");
        copyn(ec, out, s + i, 1);
        *pi = i + 1;
        return;
    }

    /* reproduce the indentation of the opening line (the main loop already
       copied it, so take it back before re-emitting it) */
    {
        size_t b = line_start(s, i), a = b;
        while (a < i && isspace((unsigned char)s[a]) && s[a] != '\n') a++;
        indent = strndup_(s + b, a - b);
        if (a - b > 0 && out->len >= (size_t)(a - b)) out->len -= (size_t)(a - b);
    }

    /* an optional operand list may sit on the opening line, after the '[' */
    {
        size_t j = skip_ws_com(s, n, i + 1);
        size_t k = j;
        while (k < n && s[k] != '\n' && s[k] != '\r') k++;
        while (k > j && isspace((unsigned char)s[k - 1])) k--;
        if (k > j) {
            if (s[j] == '"' || s[j] == ':') operands = strndup_(s + j, k - j);
            else warn(ec, "ignoring text after '[' (asm operands must start with '\"' or ':')");
        }
    }

    /* the body starts on the line after the '[' and ends before the ']' line */
    e = i;
    while (e < n && s[e] != '\n') e++;
    body_start = (e < n) ? e + 1 : n;
    if (body_start > bend) body_start = bend;

    /* the operand list may also be the first line of the body, or - the way
       gcc writes extended asm - the last one */
    if (!operands) {
        size_t a = body_start, b = a, q;
        while (b < bend && s[b] != '\n') b++;
        q = a;
        while (q < b && isspace((unsigned char)s[q])) q++;
        if (q < b && (s[q] == ':' || s[q] == '"')) {
            size_t t = b;
            while (t > q && isspace((unsigned char)s[t - 1])) t--;
            operands = strndup_(s + q, t - q);
            body_start = (b < bend) ? b + 1 : bend;
        }
    }
    if (!operands && bend > body_start) {
        size_t le = bend, ls, q;
        while (le > body_start && s[le - 1] == '\n') le--;   /* drop blank lines */
        if (le > body_start) {
            ls = le;
            while (ls > body_start && s[ls - 1] != '\n') ls--;
            q = ls;
            while (q < le && isspace((unsigned char)s[q])) q++;
            if (q < le && (s[q] == ':' || s[q] == '"')) {
                size_t t = le;
                while (t > q && isspace((unsigned char)s[t - 1])) t--;
                operands = strndup_(s + q, t - q);
                bend = ls;
            }
        }
    }

    npnames = 0;                        /* no parameter names in scope */
    skipn(ec, s + i, after - i);        /* swallow the whole construct */
    emit_asm_body(ec, out, s, body_start, bend, indent, operands);
    ec->uid++;
    sync_line(ec, out);
    free(indent);
    free(operands);
    *pi = after;
}

/* ASM(...) one-liner:  ASM(name, type, insn; insn; ...) */
static void do_asm_oneliner(EC *ec, const char *s, size_t n, size_t *pi, SB *out)
{
    size_t i = *pi;                     /* at 'A' of ASM */
    size_t j = skip_ws_com(s, n, i + 3);
    size_t open, close, inner, c1, c2;
    char *name = NULL, *type = NULL, *body = NULL;
    SB lines = {0};
    size_t after;

    if (j >= n || s[j] != '(') {
        err(ec, "expected '(' after ASM");
        copyn(ec, out, s + i, 4);
        *pi = i + 4;
        return;
    }
    open = j;
    close = match_group(s, n, open);
    if (close > n || s[close - 1] != ')') {
        err(ec, "unterminated ASM(...) construct");
        copyn(ec, out, s + i, 4);
        *pi = i + 4;
        return;
    }
    inner = open + 1;
    after = close;

    /* split on the first two commas at depth 0 */
    {
        int depth = 0;
        size_t k = inner;
        c1 = c2 = (size_t)-1;
        while (k < close - 1) {
            char c = s[k];
            if (c == '"' || c == '\'') { k = end_literal(s, n, k); continue; }
            if (c == '(' || c == '[' || c == '{') { depth++; k++; continue; }
            if (c == ')' || c == ']' || c == '}') { depth--; k++; continue; }
            if (c == ',' && depth == 0) {
                if (c1 == (size_t)-1) c1 = k;
                else if (c2 == (size_t)-1) { c2 = k; break; }
            }
            k++;
        }
        if (c1 == (size_t)-1 || c2 == (size_t)-1) {
            err(ec, "ASM() needs three parts: ASM(name, type, instructions)");
            copyn(ec, out, s + i, close - i);
            *pi = close;
            return;
        }
    }

    name = trim_dup(s + inner, c1 - inner);
    type = trim_dup(s + c1 + 1, c2 - c1 - 1);
    body = trim_dup(s + c2 + 1, (close - 1) - (c2 + 1));

    if (!*name || !*type) {
        err(ec, "ASM() needs a name and a type");
        copyn(ec, out, s + i, close - i);
        *pi = close;
        goto done;
    }

    /* split the instruction list on ';' at depth 0 */
    {
        int depth = 0;
        size_t k = 0, start = 0;
        size_t bl = strlen(body);
        for (k = 0; k <= bl; k++) {
            char c = (k < bl) ? body[k] : ';';
            if (c == '"' || c == '\'') { k = end_literal(body, bl, k) - 1; continue; }
            if (c == '(' || c == '[' || c == '{') { depth++; continue; }
            if (c == ')' || c == ']' || c == '}') { depth--; continue; }
            if (c == ';' && depth == 0) {
                char *one = trim_dup(body + start, k - start);
                if (*one) sb_printf(&lines, "%s\n", one);
                free(one);
                start = k + 1;
            }
        }
    }

    after = close;
    {
        size_t q = skip_ws(s, n, after);
        if (q < n && s[q] == ';') after = q + 1;
        q = after;
        while (q < n && s[q] != '\n') q++;
        after = (q < n) ? q + 1 : n;        /* swallow the rest of the line */
    }
    npnames = 0;                        /* the one argument is unnamed,
                                           so it is ARG1 plain and simple */
    skipn(ec, s + i, after - i);
    if (strcmp(type, "void") == 0)
        genf(ec, out, "__attribute__((naked)) void %s(void) {\n", name);
    else
        genf(ec, out, "__attribute__((naked)) %s %s(%s) {\n", type, name, type);
    ec->oline++;
    {
        emit_asm_body(ec, out, lines.p ? lines.p : "", 0, lines.len, "", NULL);
        ec->uid++;
    }
    genf(ec, out, "}\n");
    ec->oline++;
    sync_line(ec, out);

done:
    free(name); free(type); free(body);
    if (lines.p) free(lines.p);
    *pi = after;
}

/* mark the named parameters of a naked function as unused: they live in
   registers and are only ever reached through the ABI, so gcc would warn */
static char *unused_params(const char *sig, size_t n)
{
    size_t a = (size_t)-1, b = (size_t)-1, k = 0;
    int depth = 0;
    SB out = {0};

    while (k < n) {
        char c = sig[k];
        if (c == '"' || c == '\'') { k = end_literal(sig, n, k); continue; }
        if (c == '(') { if (a == (size_t)-1) { a = k; depth = 1; } else depth++; k++; continue; }
        if (c == ')') { if (depth > 0 && --depth == 0) { b = k; break; } k++; continue; }
        k++;
    }
    if (a == (size_t)-1 || b == (size_t)-1) return strndup_(sig, n);

    sb_add(&out, sig, a + 1);
    {
        size_t p = a + 1, q;
        int d = 0;
        while (p < b) {
            q = p; d = 0;
            while (q < b) {
                char c = sig[q];
                if (c == '"' || c == '\'') { q = end_literal(sig, b, q); continue; }
                if (c == '(' || c == '[' || c == '{') d++;
                else if (c == ')' || c == ']' || c == '}') d--;
                else if (c == ',' && d == 0) break;
                q++;
            }
            {
                char *one = trim_dup(sig + p, q - p);
                if (*one && strcmp(one, "void") && strcmp(one, "...") &&
                    id_char((unsigned char)one[strlen(one) - 1]))
                    sb_printf(&out, "%s __attribute__((unused))", one);
                else
                    sb_add(&out, sig + p, q - p);
                free(one);
            }
            if (q < b) sb_puts(&out, ", ");
            p = q + 1;
        }
    }
    sb_putc(&out, ')');
    sb_add(&out, sig + b + 1, n - b - 1);
    return out.p;
}

/* ASM <declaration> { ... }  -- a naked function written as a block */
static void do_asm_func(EC *ec, const char *s, size_t n, size_t *pi, SB *out)
{
    size_t i = *pi;
    size_t j = i + 3, brace = (size_t)-1;
    size_t bend, after;
    char *sig, *indent, *rawsig = NULL;

    /* find the '{' that opens the body */
    while (j < n) {
        char c = s[j];
        if (c == '"' || c == '\'') { j = end_literal(s, n, j); continue; }
        if (c == '/' && j + 1 < n && (s[j + 1] == '/' || s[j + 1] == '*')) { j = end_comment(s, n, j); continue; }
        if (c == '{') { brace = j; break; }
        if (c == ';' || c == '=') break;          /* not a function definition */
        j++;
    }
    if (brace == (size_t)-1 || s[j] != '{') {
        err(ec, "expected a '{' after the ASM function declaration");
        copyn(ec, out, s + i, 3);
        *pi = i + 3;
        return;
    }
    if (!find_body_end(s, n, brace + 1, '}', &bend, &after)) {
        err(ec, "ASM function body is never closed by a line starting with '}'");
        copyn(ec, out, s + i, 3);
        *pi = i + 3;
        return;
    }
    {
        size_t b = line_start(s, i), a = b;
        while (a < i && isspace((unsigned char)s[a]) && s[a] != '\n') a++;
        indent = strndup_(s + b, a - b);
    }
    {
        size_t a = i + 3, b = brace;
        while (a < b && isspace((unsigned char)s[a])) a++;
        while (b > a && isspace((unsigned char)s[b - 1])) b--;
        rawsig = strndup_(s + a, b - a);      /* as written: for the parameters */
        sig = unused_params(s + a, b - a);
    }

    skipn(ec, s + i, after - i);
    {
        static const char *sc[] = { "static", "extern", "inline", NULL };
        char pre[256] = "", *rest = sig;
        bool moved = true;
        while (moved) {
            moved = false;
            for (int q = 0; sc[q]; q++) {
                size_t l = strlen(sc[q]);
                if (strncmp(rest, sc[q], l) == 0 && isspace((unsigned char)rest[l])) {
                    if (strlen(pre) + l + 2 < sizeof pre) { strcat(pre, sc[q]); strcat(pre, " "); }
                    rest += l;
                    while (*rest && isspace((unsigned char)*rest)) rest++;
                    moved = true;
                    break;
                }
            }
        }
        abi_params(ec, rawsig ? rawsig : rest, rawsig ? strlen(rawsig) : strlen(rest));
        genf(ec, out, "%s__attribute__((naked)) %s%s {\n", indent, pre, rest);
    }
    ec->oline++;
    emit_asm_body(ec, out, s, brace + 1, bend, indent, NULL);
    ec->uid++;
    genf(ec, out, "%s}\n", indent);
    ec->oline++;
    sync_line(ec, out);
    for (int k = 0; k < npnames; k++) free(pnames[k].name);
    npnames = 0;
    free(sig); free(indent); free(rawsig);
    *pi = after;
}

/* ------------------------------------------------------------------ */
/* main pass                                                           */
/* ------------------------------------------------------------------ */

static void process(EC *ec, const char *s, size_t n, SB *out)
{
    size_t i = 0;

    while (i < n) {
        char c = s[i];

        /* comments */
        if (c == '/' && i + 1 < n && (s[i + 1] == '/' || s[i + 1] == '*')) {
            size_t e = end_comment(s, n, i);
            copyn(ec, out, s + i, e - i);
            i = e;
            continue;
        }
        /* string / char literals */
        if (c == '"' || c == '\'') {
            size_t e = end_literal(s, n, i);
            copyn(ec, out, s + i, e - i);
            i = e;
            continue;
        }

        /* ASM keyword at the start of a line */
        if (!in_directive(s, i) && at_bol(s, i) && eq(s + i, n - i, "ASM") &&
            !(i + 3 < n && (id_char((unsigned char)s[i + 3])))) {
            size_t j = skip_ws_com(s, n, i + 3);
            if (j < n && s[j] == '(') {
                do_asm_oneliner(ec, s, n, &i, out);
                continue;
            }
            do_asm_func(ec, s, n, &i, out);
            continue;
        }

        /* bit access */
        if (c == '@') {
            do_bit(ec, s, n, &i, out, i, i, 0);
            continue;
        }

        /* prefix ++ / -- applied to a bit (operand must be on the same line) */
        if ((c == '+' || c == '-') && i + 1 < n && s[i + 1] == c) {
            size_t j = skip_blanks(s, n, i + 2);
            bool isbit = false;
            if (j < n && (s[j] == '(' || id_start((unsigned char)s[j]) || isdigit((unsigned char)s[j]))) {
                size_t e = parse_index(s, n, j);
                size_t k = skip_blanks(s, n, e);
                if (e > j && k < n && s[k] == '@') isbit = true;
            }
            if (isbit) {
                size_t k = skip_blanks(s, n, i + 2);
                size_t oe = parse_index(s, n, k);
                size_t at = skip_blanks(s, n, oe);
                /* the operand itself still has to reach the output: do_bit()
                   picks its left operand up from there */
                copyn(ec, out, s + k, oe - k);
                do_bit(ec, s, n, &i, out, at, i, 1);
                continue;
            }
        }

        /* [[ ... ]] : assembler in the middle of C, naming C variables.
           A C23 attribute such as [[nodiscard]] is left alone because an
           identifier immediately follows the brackets. */
        if (c == '[' && i + 1 < n && s[i + 1] == '[' && at_bol(s, i) &&
            !in_directive(s, i) &&
            !(i + 2 < n && id_start((unsigned char)s[i + 2]))) {
            do_inline_asm(ec, s, n, &i, out);
            continue;
        }

        /* [ opening an inline assembler block: first token on its line,
           with nothing after it but an optional operand list */
        if (c == '[' && at_bol(s, i) && !in_directive(s, i)) {
            size_t j = skip_ws_com(s, n, i + 1);
            bool opener = rest_of_line_blank(s, n, i + 1) ||
                          (j < n && (s[j] == '"' || s[j] == ':'));
            if (opener) {
                do_bracket_asm(ec, s, n, &i, out);
                continue;
            }
        }

        copyn(ec, out, s + i, 1);
        i++;
    }
}

/* ------------------------------------------------------------------ */
/* driver                                                              */
/* ------------------------------------------------------------------ */

static char *read_all(FILE *f, size_t *len)
{
    SB b = {0};
    char buf[8192];
    size_t r;
    while ((r = fread(buf, 1, sizeof buf, f)) > 0) sb_add(&b, buf, r);
    if (!b.p) sb_add(&b, "", 1);
    *len = b.len;
    return b.p;
}

static void usage(FILE *f)
{
    fprintf(f,
        "ec - the EC (Enhanced C) preprocessor, version " EC_VERSION "\n"
        "\n"
        "usage: ec [options] input.ec\n"
        "\n"
        "  -o FILE   write the generated C to FILE (default: input with .c; - = stdout)\n"
        "  -E        write to standard output\n"
        "  -v        be chatty\n"
        "  --no-lines    do not emit #line directives\n"
        "  --no-pct      do not escape '%%' in extended asm (operands given)\n"
        "  --no-regs     do not rewrite RAX/ARG1/#255 style names in asm\n"
        "  --abi=NAME    x86-64 calling convention for ARGn: sysv (default) or win64\n"
        "  --intel       write asm bodies in intel syntax: mov RESULT, ARG1\n"
        "  --help    this message\n"
        "  --version print the version\n"
        "\n"
        "EC extensions:\n"
        "  x@N          bit N of x (read);  x@N = v;  x@N |= v;  x@N ^= v;  x@N++\n"
        "  ASM(f, T, insn; insn)             naked function  T f(T)\n"
        "  ASM T f(args) { insn ... }        naked function block\n"
        "  [  ...  ]                         inline asm block inside a function\n"
        "  [[ ... ]]                         the same, naming C variables directly\n"
        "  .label  /  label:                 local labels inside an asm block\n"
        "\n"
        "Inside an assembler body EC also knows the machine:\n"
        "  RAX RBX ... XMM0 ...   registers, without gas's '%%'\n"
        "  ARG1..ARG6             integer arguments  (%%rdi %%rsi %%rdx %%rcx %%r8 %%r9)\n"
        "  FARG1..FARG8           floating point arguments (%%xmm0..%%xmm7)\n"
        "  SCRATCH1, SCRATCH2     free temporaries   (%%r10, %%r11)\n"
        "  RESULT, FRESULT        return value       (%%rax, %%xmm0)\n"
        "  #255                   an immediate, the BBC way (becomes $255)\n"
        "and in a naked function the declared parameter names are their\n"
        "registers:  ASM ULL add(ULL x) { mov RESULT, x; add RESULT, #3; ret }\n");
}

int main(int argc, char **argv)
{
    const char *input = NULL, *output = NULL;
    bool to_stdout = false;
    EC ec;
    SB out = {0};
    char *src, *outname = NULL;
    size_t len;
    FILE *f;

    memset(&ec, 0, sizeof ec);
    ec.line = 1;
    ec.oline = 1;
    ec.line_directives = true;
    ec.escape_pct = true;

    for (int a = 1; a < argc; a++) {
        if (!strcmp(argv[a], "-o") && a + 1 < argc) output = argv[++a];
        else if (!strcmp(argv[a], "-E")) to_stdout = true;
        else if (!strcmp(argv[a], "-v") || !strcmp(argv[a], "--verbose")) ec.verbose = true;
        else if (!strcmp(argv[a], "--no-lines")) ec.line_directives = false;
        else if (!strcmp(argv[a], "--no-pct")) ec.escape_pct = false;
        else if (!strncmp(argv[a], "--abi=", 6)) {
            if (!strcmp(argv[a] + 6, "sysv")) abi = &abi_sysv;
            else if (!strcmp(argv[a] + 6, "win64")) abi = &abi_win64;
            else { fprintf(stderr, "ec: unknown ABI '%s' (try sysv or win64)\n", argv[a] + 6); return 2; }
        }
        else if (!strcmp(argv[a], "--no-regs")) opt_regnames = false;
        else if (!strcmp(argv[a], "--intel")) opt_intel = true;
        else if (!strcmp(argv[a], "--help") || !strcmp(argv[a], "-h")) { usage(stdout); return 0; }
        else if (!strcmp(argv[a], "--version")) { printf("ec " EC_VERSION "\n"); return 0; }
        else if (argv[a][0] == '-' && argv[a][1] && strcmp(argv[a], "-")) {
            fprintf(stderr, "ec: unknown option '%s'\n", argv[a]);
            return 2;
        } else if (!input) input = argv[a];
        else { fprintf(stderr, "ec: too many input files\n"); return 2; }
    }

    if (!input) { usage(stderr); return 2; }

    if (!strcmp(input, "-")) {
        src = read_all(stdin, &len);
        ec.file = "<stdin>";
        to_stdout = true;
    } else {
        f = fopen(input, "rb");
        if (!f) { fprintf(stderr, "ec: cannot open '%s'\n", input); return 2; }
        src = read_all(f, &len);
        fclose(f);
        ec.file = input;
    }

    process(&ec, src, len, &out);

    if (ec.nerr) {
        fprintf(stderr, "ec: %d error(s)\n", ec.nerr);
        return 1;
    }

    if (output && !strcmp(output, "-")) to_stdout = true;
    if (!to_stdout) {
        if (output) outname = strndup_(output, strlen(output));
        else {
            size_t l = strlen(input);
            outname = xmalloc(l + 3);
            strcpy(outname, input);
            if (l > 3 && !strcmp(input + l - 3, ".ec")) strcpy(outname + l - 3, ".c");
            else strcat(outname, ".c");
        }
        f = fopen(outname, "wb");
        if (!f) { fprintf(stderr, "ec: cannot write '%s'\n", outname); return 2; }
        fwrite(out.p, 1, out.len, f);
        fclose(f);
        if (ec.verbose) fprintf(stderr, "ec: %s -> %s (%zu bytes)\n", input, outname, out.len);
    } else {
        fwrite(out.p, 1, out.len, stdout);
    }

    if (ec.verbose && ec.nwarn) fprintf(stderr, "ec: %d warning(s)\n", ec.nwarn);
    free(outname);
    free(src);
    free(out.p);
    return 0;
}
