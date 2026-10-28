// Paul Alberti 260ADB178
// https://github.com/<ton-pseudo>/calc-lab-home
// Compile with: gcc -O2 -Wall -Wextra -std=c17 -o calc calc.c -lm
// Resources used: Claude (Anthropic), several conversations (links in the comments
//   below, plus one conversation used to review these comments: Link: <to add>);
//   man pages (mkdir, stat, strtod, readdir, feature_test_macros); the lab statement
//   (grammar hint). Details in comments where used (search "AI (" and "Resource:").

// Without this, strict -std=c17 hides mkdir/stat/opendir.
// Resource: man 7 feature_test_macros.
#define _POSIX_C_SOURCE 200809L

#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <ctype.h>
#include <math.h>
#include <errno.h>
#include <dirent.h>
#include <sys/stat.h>

// Name parts of the output files, built from the lab's naming rule
// (<input>_<name>_<lastname>_<studentid>). The case of the student ID must
// match what cli.sh expects, so run cli.sh to confirm it.
#define FILE_SUFFIX "_paul_alberti_260ADB178.txt"
#define DIR_SUFFIX  "_paul_260ADB178"
#define PATH_LEN    4096


// Types and shared state.
// AI (Claude): I asked for an example of how to declare the token types
// I wrote the declarations below myself.


typedef enum {
    T_INT, T_FLOAT,
    T_PLUS, T_MINUS, T_STAR, T_SLASH, T_POW,
    T_LPAREN, T_RPAREN,
    T_EOL,      // '\n' and end of file are treated the same: both end a line
    T_BAD       // invalid char: the tokenizer only labels it, the parser reports it
} TokType;

// One struct for int and float: is_float tells which field to read.
typedef struct {
    int is_float;
    long long i;
    double d;
} Value;

typedef struct {
    TokType type;
    size_t pos;   // 1-based position of the first character, because errors
                  // are reported at the START of the token being looked at
    Value val;
} Token;

// The whole file sits in one buffer and is never copied: a position is just
// an offset, so multi-MB files cost nothing extra.
// Globals because the parser only ever works on one file at a time.
static char *buf;     // buf[len] == '\0' (see read_file)
static size_t len;
static size_t off;    // next unread character (0-based)
static Token tok;     // current token: one token of lookahead is enough

static int has_error;
static size_t err_pos;

// Results wait here and are written at the end, because a file with any
// error must contain ONLY the error, never partial results.
static char *out;
static size_t out_len, out_cap;

// Only the first error counts: after it, every parser function stops.
static void fail(size_t pos) {
    if (!has_error) {
        has_error = 1;
        err_pos = pos;
    }
}

// The capacity doubles, so N appends cost O(N) in total instead of copying
// the whole buffer at every line.
// AI (Claude): I asked for help structuring the capacity variable and the
// while loop. I kept the doubling idea; the rest is mine.
static void out_append(const char *s, size_t n) {
    if (out_len + n > out_cap) {
        size_t cap = out_cap ? out_cap : 4096;
        while (cap < out_len + n) cap *= 2;
        char *p = realloc(out, cap);
        if (!p) {
            fprintf(stderr, "calc: out of memory\n");
            exit(1);
        }
        out = p;
        out_cap = cap;
    }
    memcpy(out + out_len, s, n);
    out_len += n;
}

// %lld for ints and %.15g for floats, as the lab requires.
static void append_value(Value v) {
    char tmp[64];   // plenty: a long long is at most 20 characters
    int n = v.is_float ? snprintf(tmp, sizeof tmp, "%.15g\n", v.d)
                       : snprintf(tmp, sizeof tmp, "%lld\n", v.i);
    out_append(tmp, (size_t)n);
}


// Tokenizer


// Positions: off moves forward over the whole file and is never reset at a
// new line, so off + 1 is already the position the lab wants. No line or
// column counting is needed.
// AI (Claude): prompt "donne moi une piste pour la structure de next_token()"
// (English: "give me a lead for the structure of next_token()").
// I reused its skeleton; the body is mine.
// Link: <to add>
static void next_token(void) {
    while (off < len && (buf[off] == ' ' || buf[off] == '\t')) off++;

    // Recorded after the blanks but before reading the token. At end of file
    // this gives len + 1, which is the position expected for a missing operand
    // on a last line without '\n'.
    tok.pos = off + 1;

    if (off >= len) {
        tok.type = T_EOL;
        return;
    }

    char c = buf[off];

    if (isdigit((unsigned char)c)) {
        size_t start = off;
        // I only skip the digits first: adding them up before knowing whether
        // this is a float could overflow a long long on a long float.
        while (isdigit((unsigned char)buf[off])) off++;

        // A '.' makes a float only if a digit follows. Otherwise I leave it,
        // and it becomes an invalid character at its own position (so "5."
        // and the 'e' of "1.5e3" fail where the lab says).
        if (buf[off] == '.' && isdigit((unsigned char)buf[off + 1])) {
            off++;
            while (isdigit((unsigned char)buf[off])) off++;
            // strtod needs a '\0'. Writing one temporarily avoids copying
            // the digits into a new string.
            // AI (Claude): I asked it to explain how to use buf and the other
            // elements needed to run strtod on a slice of the buffer without
            // copying it. The save / write '\0' /
            // restore trick comes from that explanation; the rest of the
            // loop is mine.
            // Resource: man 3 strtod.
            char saved = buf[off];
            buf[off] = '\0';
            tok.val.is_float = 1;
            tok.val.i = 0;
            tok.val.d = strtod(buf + start, NULL);
            buf[off] = saved;
            tok.type = T_FLOAT;
        } else {
            long long n = 0;
            for (size_t k = start; k < off; k++) n = n * 10 + (buf[k] - '0');
            tok.val.is_float = 0;
            tok.val.i = n;
            tok.val.d = 0.0;
            tok.type = T_INT;
        }
        return;
    }

    off++;
    switch (c) {
        case '+':  tok.type = T_PLUS;   break;
        case '-':  tok.type = T_MINUS;  break;
        case '/':  tok.type = T_SLASH;  break;
        case '(':  tok.type = T_LPAREN; break;
        case ')':  tok.type = T_RPAREN; break;
        case '\n': tok.type = T_EOL;    break;
        case '*':
            // "**" is ONE token, otherwise 2 ** 3 would read as two '*'.
            if (buf[off] == '*') {
                off++;
                tok.type = T_POW;
            } else {
                tok.type = T_STAR;
            }
            break;
        default:   tok.type = T_BAD;    break;   // includes a '#' mid-line
    }
}


// Arithmetic


// Dummy value returned after an error. Nobody reads it: callers check
// has_error first.
static Value zero_value(void) {
    Value v;
    v.is_float = 0;
    v.i = 0;
    v.d = 0.0;
    return v;
}

// Square-and-multiply: one loop turn per bit of exp, not per unit, so
// 1 ** 1000000000 is instant. exp >= 0 is checked by the caller.
// AI (Claude): "I want us to work on the arithmetic part"
//  Claude advised to
// square only while bits remain, and this loop is the version it rewrote for
// me. I went through it line by line and can explain it.
static long long int_pow(long long base, long long exp) {
    long long result = 1;
    while (exp > 0) {
        if (exp & 1) result *= base;
        exp >>= 1;
        // A last useless squaring could overflow (undefined behaviour for
        // signed ints) even when the real result fits.
        if (exp > 0) base *= base;
    }
    return result;
}

// Computes "a op b" ('P' means "**"). It takes two positions because the lab
// reports division by zero and a negative exponent at the RIGHT OPERAND, but
// an infinite/NaN float at the OPERATOR.
static Value apply(char op, Value a, Value b, size_t op_pos, size_t rhs_pos) {
    Value r = zero_value();

    // Int or float is decided for each operation: if either side is a float,
    // I compute in double (7 / 2 is 3, 7 / 2.0 is 3.5). A "1 + 2" elsewhere
    // in the file stays an int.
    if (a.is_float || b.is_float) {
        double x = a.is_float ? a.d : (double)a.i;
        double y = b.is_float ? b.d : (double)b.i;
        r.is_float = 1;
        switch (op) {
            case '+': r.d = x + y; break;
            case '-': r.d = x - y; break;
            case '*': r.d = x * y; break;
            case '/':
                // Exact test on purpose: only a divisor of exactly 0.0 fails.
                if (y == 0.0) { fail(rhs_pos); return zero_value(); }
                r.d = x / y;
                break;
            case 'P': r.d = pow(x, y); break;   // 0.5 ** (0 - 1) is fine in float
            default: break;
        }
        // The operator produced the inf/NaN (10.0 ** 400), so it gets the blame.
        if (!isfinite(r.d)) { fail(op_pos); return zero_value(); }
        return r;
    }

    switch (op) {
        case '+': r.i = a.i + b.i; break;
        case '-': r.i = a.i - b.i; break;
        case '*': r.i = a.i * b.i; break;
        case '/':
            if (b.i == 0) { fail(rhs_pos); return zero_value(); }
            r.i = a.i / b.i;   // C already truncates toward zero
            break;
        case 'P':
            // 2 ** -1 is not an integer, so it is an error.
            if (b.i < 0) { fail(rhs_pos); return zero_value(); }
            r.i = int_pow(a.i, b.i);
            break;
        default: break;
    }
    return r;
}

// ---------------------------------------------------------------------------
// Recursive descent parser: one function per precedence level.
//   expr    := term  { ('+' | '-') term }
//   term    := power { ('*' | '/') power }
//   power   := primary [ '**' power ]
//   primary := NUMBER | '(' expr ')'
// Grammar from the lab hint. Precedence comes from the call order: in
// 3 + 2 * 5, term() finishes the product before expr() ever sees the '+'.
// AI (Claude): prompt "quelle est l'étape d'après ?" (English: "what is the
// next step?"), asked repeatedly while building the parser. I reused the
// skeletons it gave me; the function bodies are mine.
// Link: <to add>
// ---------------------------------------------------------------------------

static Value expr(void);   // forward declaration: primary() calls expr()

static Value primary(void) {
    Value v;
    switch (tok.type) {
    case T_INT:
    case T_FLOAT:
        v = tok.val;       // copied first: next_token() overwrites tok
        next_token();
        return v;
    case T_LPAREN:
        next_token();
        v = expr();
        if (has_error) return v;
        // Missing ')': the token I am looking at is where it should have been.
        if (tok.type != T_RPAREN) {
            fail(tok.pos);
            return zero_value();
        }
        next_token();
        return v;
    // Everything that cannot start an operand ends here, with one rule for
    // all cases: invalid char, "7 + * 3" (the second operator), "2 * ()" (the
    // ')') and a missing operand at line end. A '-' also lands here because
    // unary operators are not allowed, so "-5" fails at the '-'.
    default:
        fail(tok.pos);
        return zero_value();
    }
}

// power() calls itself for the exponent, so 2 ** 3 ** 2 groups as 2 ** (3 ** 2).
// A loop would group to the left and give (2 ** 3) ** 2 = 64.
static Value power(void) {
    Value base = primary();
    if (!has_error && tok.type == T_POW) {
        size_t oppos = tok.pos;
        next_token();
        size_t rpos = tok.pos;   // noted before parsing the exponent
        Value e = power();
        if (has_error) return base;
        return apply('P', base, e, oppos, rpos);
    }
    return base;
}

// Here a loop is what I want: it makes 8 - 3 - 2 group as (8 - 3) - 2.
static Value term(void) {
    Value l = power();
    while (!has_error && (tok.type == T_STAR || tok.type == T_SLASH)) {
        char op = (tok.type == T_STAR) ? '*' : '/';
        size_t oppos = tok.pos;
        next_token();
        size_t rpos = tok.pos;
        Value r = power();
        if (has_error) return l;
        l = apply(op, l, r, oppos, rpos);
    }
    return l;
}

// Same loop as term(), one precedence level lower.
static Value expr(void) {
    Value l = term();
    while (!has_error && (tok.type == T_PLUS || tok.type == T_MINUS)) {
        char op = (tok.type == T_PLUS) ? '+' : '-';
        size_t oppos = tok.pos;
        next_token();
        size_t rpos = tok.pos;
        Value r = term();
        if (has_error) return l;
        l = apply(op, l, r, oppos, rpos);
    }
    return l;
}

// Evaluates the file line by line and stops at the first error.
static void evaluate_all(void) {
    // Reset, so with -d one file never leaks state into the next.
    has_error = 0;
    err_pos = 0;
    out_len = 0;
    off = 0;

    while (!has_error) {
        // '#' is handled here, not in the tokenizer: it is a comment only as
        // the first non-blank character of a line; elsewhere it is an invalid
        // character.
        while (off < len && (buf[off] == ' ' || buf[off] == '\t')) off++;
        if (off >= len) break;
        if (buf[off] == '\n') {          // blank line: no output
            off++;
            continue;
        }
        if (buf[off] == '#') {           // comment: skipped but still counted
            while (off < len && buf[off] != '\n') off++;
            if (off < len) off++;
            continue;
        }

        next_token();
        Value v = expr();
        // Only the line end may follow: this catches the extra ')' in "(1 + 2))".
        if (!has_error && tok.type != T_EOL) fail(tok.pos);
        if (!has_error) append_value(v);
    }
}

// ---------------------------------------------------------------------------
// Files, folders and command line
// ---------------------------------------------------------------------------

// One fread for the whole file (no repeated copying). The final '\0' matters:
// the tokenizer peeks at buf[off + 1] without checking len.
// AI (Claude): prompt "Montre moi la structure et indique les endroits que je
// dois compléter" (English: "show me the structure and point out the places I
// have to fill in"). It reminded me to use fseek with SEEK_END to get the file
// size; the rest, including the error handling, is mine.
// Link: <to add>
// Resource: man 2 stat.
static char *read_file(const char *path, size_t *n) {
    struct stat st;

    if (stat(path, &st) != 0 || S_ISDIR(st.st_mode)) {
        fprintf(stderr, "calc: cannot read '%s'\n", path);
        return NULL;
    }
    FILE *f = fopen(path, "rb");   // binary, so '\n' is never translated
    if (!f) {
        fprintf(stderr, "calc: cannot open '%s': %s\n", path, strerror(errno));
        return NULL;
    }
    fseek(f, 0, SEEK_END);
    long size = ftell(f);
    rewind(f);
    if (size < 0) {
        fprintf(stderr, "calc: cannot read '%s'\n", path);
        fclose(f);
        return NULL;
    }
    char *b = malloc((size_t)size + 1);
    if (!b) {
        fprintf(stderr, "calc: out of memory\n");
        fclose(f);
        return NULL;
    }
    size_t r = fread(b, 1, (size_t)size, f);
    fclose(f);
    b[r] = '\0';
    *n = r;
    return b;
}

// "dir/task1.txt" -> "task1", "labs/" -> "labs". Trailing slashes go first,
// otherwise strrchr would find that last '/' and return an empty name.
static void base_name(const char *path, char *dst, size_t cap) {
    char tmp[PATH_LEN];
    snprintf(tmp, sizeof tmp, "%s", path);
    size_t end = strlen(tmp);
    while (end > 1 && tmp[end - 1] == '/') tmp[--end] = '\0';
    char *slash = strrchr(tmp, '/');
    char *name = slash ? slash + 1 : tmp;
    size_t n = strlen(name);
    if (n > 4 && strcmp(name + n - 4, ".txt") == 0) n -= 4;
    snprintf(dst, cap, "%.*s", (int)n, name);
}

// The lab says to reuse an existing folder, so EEXIST is not an error.
// Resource: man 2 mkdir.
static int make_outdir(const char *d) {
    if (mkdir(d, 0777) != 0 && errno != EEXIST) {
        fprintf(stderr, "calc: cannot create '%s': %s\n", d, strerror(errno));
        return 1;
    }
    return 0;
}

// Evaluates one file and writes its result. Returns 0 on success.
// An ERROR:<pos> result is normal output, so the exit code stays 0.
static int process_file(const char *in_path, const char *base, const char *outdir) {
    size_t n = 0;
    char *data = read_file(in_path, &n);
    if (!data) return 1;
    buf = data;
    len = n;
    evaluate_all();

    // Created after reading, so a missing input leaves no empty folder.
    if (make_outdir(outdir) != 0) {
        free(data);
        return 1;
    }

    char path[PATH_LEN];
    int m = snprintf(path, sizeof path, "%s/%s%s", outdir, base, FILE_SUFFIX);
    int rc = 0;
    if (m < 0 || (size_t)m >= sizeof path) {
        fprintf(stderr, "calc: path too long\n");
        rc = 1;
    } else {
        FILE *f = fopen(path, "wb");
        if (!f) {
            fprintf(stderr, "calc: cannot write '%s': %s\n", path, strerror(errno));
            rc = 1;
        } else {
            // On error ONLY the error is written. fclose is checked too:
            // a disk-full failure can show up only when the data is flushed.
            if (has_error) {
                if (fprintf(f, "ERROR:%zu\n", err_pos) < 0) rc = 1;
            } else if (out_len > 0 && fwrite(out, 1, out_len, f) != out_len) {
                rc = 1;
            }
            if (fclose(f) != 0) rc = 1;
            if (rc) fprintf(stderr, "calc: error writing '%s'\n", path);
        }
    }
    free(data);
    buf = NULL;
    return rc;
}

// Options can come in any order, so I parse them all in one loop and check
// the combination afterwards.
// AI (Claude): prompt "Quelle est l'étape suivante ? Oriente moi avec le
// squelette" (English: "what is the next step? Guide me with the skeleton").
// I also asked for help on a few smaller parts of main() whose exact prompts I
// no longer have. I wrote the checks myself from section 4 of the lab.
// Link: <to add>
int main(int argc, char **argv) {
    const char *dir = NULL, *outdir = NULL, *input = NULL;

    for (int i = 1; i < argc; i++) {
        if (strcmp(argv[i], "-d") == 0 || strcmp(argv[i], "--dir") == 0) {
            if (i + 1 >= argc) {
                fprintf(stderr, "calc: option '%s' needs a value\n", argv[i]);
                return 1;
            }
            dir = argv[++i];
        } else if (strcmp(argv[i], "-o") == 0 || strcmp(argv[i], "--output-dir") == 0) {
            if (i + 1 >= argc) {
                fprintf(stderr, "calc: option '%s' needs a value\n", argv[i]);
                return 1;
            }
            outdir = argv[++i];
        } else if (argv[i][0] == '-') {
            fprintf(stderr, "calc: unknown option '%s'\n", argv[i]);
            return 1;
        } else if (input) {
            fprintf(stderr, "calc: more than one input file\n");
            return 1;
        } else {
            input = argv[i];
        }
    }

    // Exactly one of input file or -d DIR: this test is true when both or
    // neither are given, and both are usage errors.
    if ((dir == NULL) == (input == NULL)) {
        fprintf(stderr, "usage: calc [-d DIR] [-o OUTDIR] [input.txt]\n");
        return 1;
    }

    char base[PATH_LEN];
    char default_out[PATH_LEN + 64];   // room for DIR_SUFFIX after the base
    int rc = 0;

    if (input) {
        base_name(input, base, sizeof base);
        if (!outdir) {
            snprintf(default_out, sizeof default_out, "%s%s", base, DIR_SUFFIX);
            outdir = default_out;
        }
        rc = process_file(input, base, outdir);
    } else {
        DIR *d = opendir(dir);
        if (!d) {
            fprintf(stderr, "calc: cannot open folder '%s': %s\n", dir, strerror(errno));
            return 1;
        }
        base_name(dir, base, sizeof base);
        if (!outdir) {
            snprintf(default_out, sizeof default_out, "%s%s", base, DIR_SUFFIX);
            outdir = default_out;
        }
        rc = make_outdir(outdir);

        // One file at a time, freed each time, so memory is bounded by the
        // largest file.
        // Resource: man 3 readdir.
        struct dirent *e;
        while (rc == 0 && (e = readdir(d)) != NULL) {
            size_t n = strlen(e->d_name);
            // Only "*.txt" files directly in DIR: a folder named x.txt or any
            // other file is ignored (the S_ISREG test below).
            if (n <= 4 || strcmp(e->d_name + n - 4, ".txt") != 0) continue;
            char p[PATH_LEN];
            snprintf(p, sizeof p, "%s/%s", dir, e->d_name);
            struct stat st;
            if (stat(p, &st) == 0 && S_ISREG(st.st_mode)) {
                char b[PATH_LEN];
                base_name(e->d_name, b, sizeof b);
                if (process_file(p, b, outdir) != 0) rc = 1;
            }
        }
        closedir(d);
    }

    free(out);
    return rc;
}