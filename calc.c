// Paul Alberti 260ADB178
// https://github.com/<ton-pseudo>/calc-lab-home
// Compile with: gcc -O2 -Wall -Wextra -std=c17 -o calc calc.c -lm
// Resources used: Claude (Anthropic), conversation: <LIEN>; <autre outil d'IA + historique>;
//   details in comments where used.

// Exposes mkdir/stat/opendir/S_ISDIR, which strict -std=c17 hides.
#define _POSIX_C_SOURCE 200809L

#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <ctype.h>
#include <math.h>
#include <errno.h>
#include <dirent.h>
#include <sys/stat.h>

// Identity used in output names. Check calc-tests/cli.sh for the expected
// case of the student ID (lowercase or not).
#define FILE_SUFFIX "_paul_alberti_260adb178.txt"
#define DIR_SUFFIX  "_paul_260adb178"
#define PATH_LEN    4096

// ---------------------------------------------------------------------------
// Types and shared state
// ---------------------------------------------------------------------------

typedef enum {
    T_INT, T_FLOAT,
    T_PLUS, T_MINUS, T_STAR, T_SLASH, T_POW,
    T_LPAREN, T_RPAREN,
    T_EOL,      // '\n' OR end of file: both end an expression and both are
                // where "missing operand" / "missing )" are reported
    T_BAD       // invalid character; the tokenizer never reports errors itself,
                // the parser decides, so errors are raised in one place
} TokType;

// The int/float flag decides the arithmetic (see apply()).
typedef struct {
    int is_float;
    long long i;
    double d;
} Value;

typedef struct {
    TokType type;
    size_t pos;   // 1-based position of the token's FIRST character in the
                  // file: errors are reported at the start of the token the
                  // parser was looking at, so the token carries it
    Value val;    // only meaningful for T_INT / T_FLOAT
} Token;

// The file is read once into one buffer and never copied: positions are
// plain offsets into it and big files cost no extra copying.
// Not const: the tokenizer briefly writes a '\0' after a float so strtod()
// can read it without copying the digits.
static char *buf;     // buf[len] == '\0' (read_file guarantees it)
static size_t len;
static size_t off;    // 0-based offset of the next unread character
static Token tok;     // current token (one token of lookahead)

static int has_error;
static size_t err_pos;

// Results are kept here and written only at the end: if any error occurs the
// file must contain ONLY the error, never partial results.
static char *out;
static size_t out_len, out_cap;

// Only the first error matters. Every parser function stops as soon as
// has_error is set, so the first call is the earliest one.
static void fail(size_t pos) {
    if (!has_error) {
        has_error = 1;
        err_pos = pos;
    }
}

// Capacity doubles when full, so N appends cost O(N) overall instead of
// copying the whole buffer on every line.
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

// %lld for integers, %.15g for floats, as the spec requires.
static void append_value(Value v) {
    char tmp[64];   // snprintf never writes more than sizeof tmp
    int n = v.is_float ? snprintf(tmp, sizeof tmp, "%.15g\n", v.d)
                       : snprintf(tmp, sizeof tmp, "%lld\n", v.i);
    out_append(tmp, (size_t)n);
}

// ---------------------------------------------------------------------------
// Tokenizer
// ---------------------------------------------------------------------------

// Positions: off only moves forward over the WHOLE file and is never reset at
// line starts, so off + 1 is already the file-wide 1-based position (newlines
// and comment lines included).
// AI (Claude): prompt "donne moi une piste pour la structure de next_token()".
// Skeleton reused, body written by me.
static void next_token(void) {
    while (off < len && (buf[off] == ' ' || buf[off] == '\t')) off++;

    // Fixed after the blanks and BEFORE consuming the token.
    // When off == len, off + 1 == len + 1: the position the spec wants for a
    // missing operand on a last line without '\n'.
    tok.pos = off + 1;

    if (off >= len) {
        tok.type = T_EOL;
        return;
    }

    char c = buf[off];

    if (isdigit((unsigned char)c)) {
        size_t start = off;
        // Only skip digits here: accumulating before knowing whether this is
        // a float could overflow a long long on a long float literal (UB).
        while (isdigit((unsigned char)buf[off])) off++;   // '\0' stops it

        // Float only if '.' is followed by a digit. Otherwise the '.' is not
        // consumed and becomes the next token (T_BAD at its own position);
        // same for the 'e' of 1.5e3.
        if (buf[off] == '.' && isdigit((unsigned char)buf[off + 1])) {
            off++;
            while (isdigit((unsigned char)buf[off])) off++;
            char saved = buf[off];
            buf[off] = '\0';                  // temporary end of string
            tok.val.is_float = 1;
            tok.val.i = 0;
            tok.val.d = strtod(buf + start, NULL);
            buf[off] = saved;                 // restore
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

    off++;   // consume the character
    switch (c) {
        case '+':  tok.type = T_PLUS;   break;
        case '-':  tok.type = T_MINUS;  break;
        case '/':  tok.type = T_SLASH;  break;
        case '(':  tok.type = T_LPAREN; break;
        case ')':  tok.type = T_RPAREN; break;
        case '\n': tok.type = T_EOL;    break;
        case '*':
            // "**" must be ONE token, otherwise 2 ** 3 would read as two '*'.
            // buf[off] is safe at the end of the buffer thanks to the '\0'.
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

// ---------------------------------------------------------------------------
// Arithmetic
// ---------------------------------------------------------------------------

// The "no value" result returned after an error. It is never used: has_error
// is set and every caller checks it before using a result.
static Value zero_value(void) {
    Value v;
    v.is_float = 0;
    v.i = 0;
    v.d = 0.0;
    return v;
}

// Square-and-multiply: the loop runs once per BIT of exp (about 30 for 1e9)
// instead of exp times, so 1 ** 1000000000 stays instant on huge inputs.
// Caller guarantees exp >= 0; exp == 0 gives 1, including 0 ** 0.
// AI (Claude): prompt "je veux qu'on travaille sur la partie arithmétique"
// (advice: square only if bits remain). Code from the full rewrite I asked for.
static long long int_pow(long long base, long long exp) {
    long long result = 1;
    while (exp > 0) {
        if (exp & 1) result *= base;
        exp >>= 1;
        // Square only if bits remain: a last useless squaring could overflow
        // (UB for signed ints) although the real result fits.
        if (exp > 0) base *= base;
    }
    return result;
}

// Computes "a op b". Two positions because the spec reports division by zero
// and a negative exponent at the RIGHT OPERAND, but an infinite/NaN float
// result at the OPERATOR.
static Value apply(char op, Value a, Value b, size_t op_pos, size_t rhs_pos) {
    Value r = zero_value();

    // Int or float is decided per operation: if EITHER operand is a float the
    // whole operation is done in double (7 / 2 is 3, 7 / 2.0 is 3.5).
    if (a.is_float || b.is_float) {
        // An integer Value keeps its content in .i, not .d: convert it.
        double x = a.is_float ? a.d : (double)a.i;
        double y = b.is_float ? b.d : (double)b.i;
        r.is_float = 1;
        switch (op) {
            case '+': r.d = x + y; break;
            case '-': r.d = x - y; break;
            case '*': r.d = x * y; break;
            case '/':
                // Exact comparison on purpose: only a divisor exactly 0.0
                // (or -0.0) is an error.
                if (y == 0.0) { fail(rhs_pos); return zero_value(); }
                r.d = x / y;
                break;
            case 'P': r.d = pow(x, y); break;   // negative exponents allowed
            default: break;
        }
        // inf/NaN (10.0 ** 400, (0 - 8) ** 0.5) was produced by the operator.
        if (!isfinite(r.d)) { fail(op_pos); return zero_value(); }
        return r;
    }

    switch (op) {
        case '+': r.i = a.i + b.i; break;
        case '-': r.i = a.i - b.i; break;
        case '*': r.i = a.i * b.i; break;
        case '/':
            if (b.i == 0) { fail(rhs_pos); return zero_value(); }
            r.i = a.i / b.i;   // C truncates toward zero, as the spec says
            break;
        case 'P':
            // An integer result cannot represent 2 ** -1: error.
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
// expr calls term, which calls power: in 3 + 2 * 5 the product is finished
// inside term() before expr() sees the '+', so * and / bind tighter.
// AI (Claude): prompts "quelle est l'étape d'après ?", "réécris le code en
// complétant ce que j'ai oublié", "maintenant qu'est ce qu'on doit faire ?".
// Skeletons reused, bodies written by me.
// ---------------------------------------------------------------------------

static Value expr(void);   // primary -> expr is a cycle: needs a prototype

static Value primary(void) {
    Value v;
    switch (tok.type) {
    case T_INT:
    case T_FLOAT:
        v = tok.val;       // copy BEFORE next_token() overwrites tok
        next_token();
        return v;
    case T_LPAREN:
        next_token();
        v = expr();
        if (has_error) return v;
        // Missing ')': the token we look at (usually the line end) is where
        // the ')' should have been, hence its position.
        if (tok.type != T_RPAREN) {
            fail(tok.pos);
            return zero_value();
        }
        next_token();
        return v;
    // Anything that cannot start an operand lands here; one fail covers:
    //   T_BAD -> invalid character; operator after operator (7 + * 3) -> the
    //   second operator; ')' as in "2 * ()" -> that ')'; T_EOL -> missing
    //   operand at the line end. The token is not consumed: the run stops.
    default:
        fail(tok.pos);
        return zero_value();
    }
}

// power calls itself for the exponent, so 2 ** 3 ** 2 groups as 2 ** (3 ** 2).
// A loop would group to the left ((2 ** 3) ** 2 = 64); recursion on the right
// makes ** right-associative.
static Value power(void) {
    Value base = primary();
    if (!has_error && tok.type == T_POW) {
        size_t oppos = tok.pos;
        next_token();
        size_t rpos = tok.pos;   // start of the exponent, noted before parsing
        Value e = power();
        if (has_error) return base;
        return apply('P', base, e, oppos, rpos);
    }
    return base;
}

// A loop updates the left value each turn: 8 - 3 - 2 is (8 - 3) - 2.
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

// Evaluates the buffer line by line and stops at the first error.
static void evaluate_all(void) {
    // Reset here so several files with -d never leak state into each other.
    has_error = 0;
    err_pos = 0;
    out_len = 0;
    off = 0;

    while (!has_error) {
        // Classify the line by its first non-blank character. '#' is handled
        // here and not in the tokenizer because it is a comment ONLY there;
        // anywhere else it reaches the tokenizer and is an invalid character.
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
        // After a full expression only the line end is allowed: a leftover
        // ')' as in "(1 + 2))" is reported where it starts.
        if (!has_error && tok.type != T_EOL) fail(tok.pos);
        if (!has_error) append_value(v);
    }
}

// ---------------------------------------------------------------------------
// Files, folders and command line
// ---------------------------------------------------------------------------

// Reads the whole file at once (one fread, no repeated copying). The final
// '\0' is required: the tokenizer reads buf[off + 1] without testing len.
// Returns NULL on error (message already printed on stderr).
static char *read_file(const char *path, size_t *n) {
    struct stat st;
    // fopen succeeds on a folder on Linux, so refuse folders explicitly.
    if (stat(path, &st) != 0 || S_ISDIR(st.st_mode)) {
        fprintf(stderr, "calc: cannot read '%s'\n", path);
        return NULL;
    }
    FILE *f = fopen(path, "rb");   // binary: '\n' must not be translated
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

// "dir/task1.txt" -> "task1", "labs/" -> "labs".
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

// An existing folder is reused, so EEXIST is not an error.
static int make_outdir(const char *d) {
    if (mkdir(d, 0777) != 0 && errno != EEXIST) {
        fprintf(stderr, "calc: cannot create '%s': %s\n", d, strerror(errno));
        return 1;
    }
    return 0;
}

// Evaluates one file and writes its output. Returns 0 on success.
// ERROR:<pos> in the result is NOT a failure: the exit code stays 0.
static int process_file(const char *in_path, const char *base, const char *outdir) {
    size_t n = 0;
    char *data = read_file(in_path, &n);
    if (!data) return 1;
    buf = data;
    len = n;
    evaluate_all();

    // Folder created after reading, so a missing input leaves no empty folder.
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
            // On error, ONLY the error is written (no partial results).
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

    // Exactly one of: an input file, or -d DIR.
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

        struct dirent *e;
        while (rc == 0 && (e = readdir(d)) != NULL) {
            size_t n = strlen(e->d_name);
            // Only "*.txt" directly inside DIR; subfolders (even x.txt) and
            // other files are ignored.
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