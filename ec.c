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

#define EC_VERSION "0.1.0"

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

static void asm_line_to_string(EC *ec, SB *out, const char *p, size_t n, bool ext, int curline)
{
    sb_putc(out, '"');
    for (size_t i = 0; i < n; i++) {
        char c = p[i];
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
        sb_putc(out, c);
    }
    sb_puts(out, "\\n\\t\"\n");
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
                    asm_line_to_string(ec, out, s + a, b - a, ext, (int)nlines);
                    ec->oline++;
                    nlines++;
                }
            }
        }
        p = (e < end) ? e + 1 : e;
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
            if (q < b) sb_putc(&out, ',');
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
    char *sig, *indent;

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
        genf(ec, out, "%s__attribute__((naked)) %s%s {\n", indent, pre, rest);
    }
    ec->oline++;
    emit_asm_body(ec, out, s, brace + 1, bend, indent, NULL);
    ec->uid++;
    genf(ec, out, "%s}\n", indent);
    ec->oline++;
    sync_line(ec, out);
    free(sig); free(indent);
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
        "  --help    this message\n"
        "  --version print the version\n"
        "\n"
        "EC extensions:\n"
        "  x@N          bit N of x (read);  x@N = v;  x@N |= v;  x@N ^= v;  x@N++\n"
        "  ASM(f, T, insn; insn)             naked function  T f(T)\n"
        "  ASM T f(args) { insn ... }        naked function block\n"
        "  [  ...  ]                         inline asm block inside a function\n"
        "  .label  /  label:                 local labels inside an asm block\n");
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
