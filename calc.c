// PAUL ALBERTI 260ADB178
// (optional) https://github.com/<your-username>/calc-lab-home
// Compile with: gcc -O2 -Wall -Wextra -std=c17 -o calc calc.c -lm
// Resources used: Claude.ai (Anthropic AI assistant), conversation link: <COLLE LE LIEN DE LA CONVERSATION>; details in comments where used.

#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <ctype.h>
#include <math.h>
#include <errno.h>
#include <dirent.h>
#include <sys/stat.h>

// Identity used to build output names: <base>_<NAME>_<LASTNAME>_<ID>.txt
// and the default output folder <base>_<NAME>_<ID>.
#define STUDENT_NAME "paul"
#define STUDENT_LAST "alberti"
#define STUDENT_ID   "260ADB178"
#define FILE_SUFFIX  "_" STUDENT_NAME "_" STUDENT_LAST "_" STUDENT_ID ".txt"
#define DIR_SUFFIX   "_" STUDENT_NAME "_" STUDENT_ID

#define PATH_MAX_LEN 4096

// ---------------------------------------------------------------------------
// Types and shared parser state
// ---------------------------------------------------------------------------

typedef enum {
    T_INT,      // integer literal
    T_FLOAT,    // float literal (digits '.' digits)
    T_PLUS, T_MINUS, T_STAR, T_SLASH, T_POW,
    T_LPAREN, T_RPAREN,
    T_EOL,      // '\n' OR end of file: both end an expression, and both are
                // where "missing operand" / "missing )" errors are reported
    T_BAD       // any character that is not part of the language; the tokenizer
                // never reports errors itself, the parser decides (one place)
} TokType;

// A number is an int or a double; the flag decides the arithmetic (see apply()).
typedef struct {
    int is_float;
    long long i;
    double d;
} Value;

typedef struct {
    TokType type;
    size_t pos;     // 1-based position of the token's first character in the
                    // file; stored in the token because errors are reported
                    // at the start of the token the parser was looking at
    Value val;      // only meaningful for T_INT / T_FLOAT
} Token;

// Contenu du fichier d'entrée, lu une seule fois en mémoire.
// Tous les offsets (donc toutes les positions d'erreur) se calculent dans ce buffer.
// Not const: the tokenizer briefly writes a '\0' after a number so strtod() /
// strtoll() can read it without copying the digits anywhere (see next_token()).
static char *buf;         // whole file content, read once (buf[len] == '\0')
static size_t len;        // number of bytes in buf
static size_t off;        // 0-based offset of the next unread character
static Token tok;         // current token (one token of lookahead)

static int has_error;     // set once the first error is found
static size_t err_pos;    // position of that first error (1-based)

// Results are collected here and written only at the end: if any error
// occurs the file must contain ONLY the error, never partial results.
static char *out;
static size_t out_len, out_cap;

// Only the FIRST error matters, and the file is read front to back, so the
// first call is always the earliest position. Later calls are ignored.
static void fail(size_t pos) {
    if (!has_error) {
        has_error = 1;
        err_pos = pos;
    }
}

// Append to the output buffer. Capacity doubles, so appending N results costs
// O(N) overall instead of copying the whole buffer on every line.
// AI (Claude): prompt "<COLLE TON VRAI PROMPT>"
// MY NOTES: <explique avec tes mots pourquoi le doublement est O(1) amorti>
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

// Formate dans un petit tableau local (snprintf ne dépasse jamais sa taille),
// puis recopie dans out. %lld pour un entier, %.15g pour un flottant.
static void append_value(Value v) {
    char tmp[64];   // "%.15g" and "%lld" never need more than ~25 chars
    int n = v.is_float ? snprintf(tmp, sizeof tmp, "%.15g\n", v.d)
                       : snprintf(tmp, sizeof tmp, "%lld\n", v.i);
    out_append(tmp, (size_t)n);
}

// ---------------------------------------------------------------------------
// Tokenizer
// ---------------------------------------------------------------------------

// Position tracking: `off` only ever moves forward over the whole file and is
// never reset at line starts, so off + 1 is already the file-wide 1-based
// position (newlines and comment lines included). No separate counter needed.
// MY NOTES: <décris avec tes mots comment les positions sont suivies>
// AI (Claude): prompt "donne moi une piste pour la structure de next_token(),
// j'essaie de la compléter moi-même". Squelette repris.

static void next_token(void) {
    // 1. Sauter les espaces et tabulations (mais PAS '\n' : c'est un token).
    while (off < len && (buf[off] == ' ' || buf[off] == '\t')) {
        off++;
    }

    // 2. Position fixée APRÈS les blancs et AVANT de consommer le token :
    //    off est 0-based, les positions de l'énoncé sont 1-based.
    tok.pos = off + 1;

    // 3. Fin du buffer : T_EOL. Quand off == len, off + 1 == len + 1,
    //    exactement la position demandée pour un fichier sans '\n' final.
    if (off >= len) {
        tok.type = T_EOL;
        return;
    }

    char c = buf[off];

    // 4. Chiffres.
    if (isdigit((unsigned char)c)) {
        size_t start = off;
        while (isdigit((unsigned char)buf[off])) off++;  // buf[len] == '\0' arrête la boucle

        // strtod/strtoll veulent une chaîne terminée par '\0' : on en pose un
        // temporairement après le nombre au lieu de copier les chiffres.
        // On n'accumule PAS les chiffres à la main : un très long littéral
        // ferait déborder un long long signé (comportement indéfini), alors
        // que strtoll sature proprement.
        // Flottant seulement si '.' est suivi d'un chiffre. Sinon le '.' n'est
        // pas consommé : il devient le token suivant (T_BAD, à sa position).
        // Pareil pour le 'e' de 1.5e3 : il reste non lu.
        if (buf[off] == '.' && isdigit((unsigned char)buf[off + 1])) {
            off++;
            while (isdigit((unsigned char)buf[off])) off++;
            char saved = buf[off];
            buf[off] = '\0';
            tok.val.is_float = 1;
            tok.val.i = 0;
            tok.val.d = strtod(buf + start, NULL);
            buf[off] = saved;
            tok.type = T_FLOAT;
        } else {
            char saved = buf[off];
            buf[off] = '\0';
            tok.val.is_float = 0;
            tok.val.i = strtoll(buf + start, NULL, 10);
            tok.val.d = 0.0;
            buf[off] = saved;
            tok.type = T_INT;
        }
        return;
    }

    // 5. Opérateurs et parenthèses.
    off++;               // on consomme le caractère
    switch (c) {
        case '+': tok.type = T_PLUS;   break;
        case '-': tok.type = T_MINUS;  break;
        case '/': tok.type = T_SLASH;  break;
        case '(': tok.type = T_LPAREN; break;
        case ')': tok.type = T_RPAREN; break;
        case '\n': tok.type = T_EOL;   break;
        case '*':
            // "**" doit être UN token, sinon 2 ** 3 serait lu comme deux '*'.
            // buf[off] est sûr même en fin de buffer grâce au '\0' final.
            if (buf[off] == '*') {
                off++;
                tok.type = T_POW;
            } else {
                tok.type = T_STAR;
            }
            break;
        default:  tok.type = T_BAD;  break;   // y compris '#' ici
    }
}

// ---------------------------------------------------------------------------
// Arithmetic
// ---------------------------------------------------------------------------

// One place that builds the "no value" result returned after an error, so every
// caller gets the same harmless integer 0. The value is never used: has_error
// is already set, and each caller checks it before using a result.
static Value zero_value(void) {
    Value v;
    v.is_float = 0;
    v.i = 0;
    v.d = 0.0;
    return v;
}

// Fast exponentiation (square-and-multiply): the loop runs once per BIT of
// exp (about 30 times for 1e9) instead of exp times, so 1 ** 1000000000
// stays instant even on multi-megabyte inputs with many such lines.
// The caller guarantees exp >= 0; exp == 0 gives 1, including 0 ** 0.
// AI (Claude): prompt "je veux qu'on travaille sur la partie arithmétique".
// MY NOTES: <explique square-and-multiply avec tes mots>
static long long int_pow(long long base, long long exp) {
    long long result = 1;
    while (exp > 0) {
        if (exp & 1) result *= base;   // this bit of the exponent is set
        exp >>= 1;
        // Square only if bits remain: a final useless squaring could
        // overflow (undefined behaviour for signed ints) even though the
        // real result fits in a long long.
        if (exp > 0) base *= base;
    }
    return result;
}

// Computes "a op b". op_pos is the operator's position, rhs_pos the start of
// the right operand: the spec reports division by zero and a negative
// exponent at the RIGHT OPERAND, but an infinite/NaN float result at the
// OPERATOR, so the caller must give both positions.
// After an error the returned value is meaningless: fail() has set
// has_error and every caller checks it right after the call and unwinds,
// so the zeroed Value is never used or printed.
static Value apply(char op, Value a, Value b, size_t op_pos, size_t rhs_pos) {
    Value r = zero_value();

    // Int or float is decided per operation: if EITHER operand is a float
    // the whole operation is done in double (7 / 2 is 3, 7 / 2.0 is 3.5).
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
                // Exact comparison on purpose: the spec only forbids a
                // divisor that is exactly 0.0 (also true for -0.0).
                if (y == 0.0) { fail(rhs_pos); return zero_value(); }
                r.d = x / y;
                break;
            case 'P':   // '**'; negative exponents are allowed on floats
                r.d = pow(x, y);
                break;
            default: break;
        }
        // inf or NaN (10.0 ** 400, (0 - 8) ** 0.5) was produced by the
        // operator itself, so the error goes at the operator's position.
        if (!isfinite(r.d)) { fail(op_pos); return zero_value(); }
        return r;
    }

    // Integer arithmetic (long long). The tests never overflow.
    switch (op) {
        case '+': r.i = a.i + b.i; break;
        case '-': r.i = a.i - b.i; break;
        case '*': r.i = a.i * b.i; break;
        case '/':
            if (b.i == 0) { fail(rhs_pos); return zero_value(); }
            r.i = a.i / b.i;   // C truncates toward zero, as the spec requires
            break;
        case 'P':
            // An integer result cannot represent 2 ** -1, hence the error.
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
// Lower precedence = higher in the call chain: expr calls term, which calls
// power. So in 3 + 2 * 5 the product is finished inside term() before expr()
// sees the '+', which gives * and / priority over + and -.
// Every function stops quickly once has_error is set.
// AI (Claude): prompt "<COLLE TON VRAI PROMPT>"
// MY NOTES: <explique la grammaire avec tes mots>
// ---------------------------------------------------------------------------

static Value expr(void);   // prototype: primary calls expr, expr (via term, power) calls primary

static Value primary(void) {
    Value v;
    switch (tok.type) {
    case T_INT:
    case T_FLOAT:
        v = tok.val;       // copy BEFORE next_token(), which overwrites tok
        next_token();
        return v;
    case T_LPAREN:
        next_token();
        v = expr();
        if (has_error) return v;
        // Missing ')' : the token we are looking at (usually the line end)
        // is the place where the ')' should have been.
        if (tok.type != T_RPAREN) {
            fail(tok.pos);
            return zero_value();
        }
        next_token();
        return v;
    // Everything that cannot start an operand ends up here.
    // One fail(tok.pos) covers four rows of the spec's error table:
    //   T_BAD   -> invalid character, reported at that character
    //   operator where an operand is expected (7 + * 3) -> the second operator
    //   T_RPAREN -> ')' right after '(' as in "2 * ()", reported at the ')'
    //   T_EOL   -> missing operand at end of line, reported at the '\n'
    // The offending token is NOT consumed: the whole line is abandoned anyway.
    default:
        fail(tok.pos);
        return zero_value();
    }
}

// power calls itself for the exponent, so 2 ** 3 ** 2 groups as 2 ** (3 ** 2).
// A loop (like in expr/term) would group to the left; the recursion on the
// right-hand side is what makes ** right-associative.
static Value power(void) {
    Value base = primary();
    if (!has_error && tok.type == T_POW) {
        size_t oppos = tok.pos;
        next_token();
        size_t rpos = tok.pos;      // start of the exponent, noted before parsing it
        Value e = power();
        if (has_error) return base;
        return apply('P', base, e, oppos, rpos);
    }
    return base;
}

// Loops give left associativity: 8 - 3 - 2 is (8 - 3) - 2.
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

// Evaluate the whole buffer line by line. Stops at the first error.
static void evaluate_all(void) {
    has_error = 0;
    err_pos = 0;
    out_len = 0;
    off = 0;

    while (!has_error) {
        // Look at the first non-blank character to classify the line.
        while (off < len && (buf[off] == ' ' || buf[off] == '\t')) off++;
        if (off >= len) break;
        if (buf[off] == '\n') {         // blank line: no output
            off++;
            continue;
        }
        if (buf[off] == '#') {          // comment: only valid as first non-blank char
            while (off < len && buf[off] != '\n') off++;
            if (off < len) off++;
            continue;
        }

        next_token();
        Value v = expr();
        // After a full expression only the line end is allowed. A leftover
        // ')' ("(1 + 2))") or any other token is reported where it starts.
        if (!has_error && tok.type != T_EOL) fail(tok.pos);
        if (!has_error) append_value(v);
    }
}

// ---------------------------------------------------------------------------
// Files, folders and command line
// ---------------------------------------------------------------------------

// Lit tout le fichier en mémoire d'un coup (taille via fseek/ftell, un seul
// fread, donc pas de recopie répétée), terminé par '\0'.
// Le '\0' final est indispensable : le tokenizer lit buf[off] et buf[off + 1]
// sans tester len, et strtod/strtoll veulent une fin de chaîne.
// Retourne NULL en cas d'erreur (message déjà affiché sur stderr).
// AI (Claude): prompt "<COLLE TON VRAI PROMPT>"
// MY NOTES: <dis quelle partie est de toi>
static char *read_file(const char *path, size_t *n) {
    struct stat st;
    if (stat(path, &st) != 0) {
        fprintf(stderr, "calc: cannot open '%s': %s\n", path, strerror(errno));
        return NULL;
    }
    if (S_ISDIR(st.st_mode)) {
        fprintf(stderr, "calc: '%s' is a directory, not a file\n", path);
        return NULL;
    }

    FILE *f = fopen(path, "rb");
    if (!f) {
        fprintf(stderr, "calc: cannot open '%s': %s\n", path, strerror(errno));
        return NULL;
    }

    if (fseek(f, 0, SEEK_END) != 0) {
        fprintf(stderr, "calc: cannot read '%s'\n", path);
        fclose(f);
        return NULL;
    }
    long size = ftell(f);
    if (size < 0 || fseek(f, 0, SEEK_SET) != 0) {
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
    if (ferror(f)) {
        fprintf(stderr, "calc: error reading '%s'\n", path);
        free(b);
        fclose(f);
        return NULL;
    }
    fclose(f);

    b[r] = '\0';
    *n = r;
    return b;
}

// "dir/task1.txt" -> "task1", "labs/" -> "labs". Résultat écrit dans dst.
static void base_name(const char *path, char *dst, size_t cap) {
    char tmp[PATH_MAX_LEN];
    snprintf(tmp, sizeof tmp, "%s", path);

    // enlever les '/' de fin ("labs/" -> "labs")
    size_t end = strlen(tmp);
    while (end > 1 && tmp[end - 1] == '/') {
        tmp[end - 1] = '\0';
        end--;
    }

    // garder ce qui suit le dernier '/'
    char *slash = strrchr(tmp, '/');
    char *name = slash ? slash + 1 : tmp;

    // enlever ".txt" à la fin
    size_t n = strlen(name);
    if (n > 4 && strcmp(name + n - 4, ".txt") == 0) n -= 4;

    snprintf(dst, cap, "%.*s", (int)n, name);
}

// Crée le dossier de sortie s'il n'existe pas. Retourne 0 si OK.
static int make_outdir(const char *d) {
    if (mkdir(d, 0777) != 0 && errno != EEXIST) {
        fprintf(stderr, "calc: cannot create folder '%s': %s\n", d, strerror(errno));
        return 1;
    }
    return 0;
}

// Évalue un fichier d'entrée et écrit son fichier de sortie. Retourne 0 si OK.
// Un résultat ERROR:<pos> n'est PAS un échec ici (le code de retour reste 0).
static int process_file(const char *in_path, const char *base, const char *outdir) {
    size_t n = 0;
    char *data = read_file(in_path, &n);
    if (!data) return 1;

    buf = data;
    len = n;
    evaluate_all();

    // Dossier créé seulement après la lecture : une entrée absente
    // ne laisse pas de dossier vide.
    if (make_outdir(outdir) != 0) {
        free(data);
        buf = NULL;
        return 1;
    }

    char path[PATH_MAX_LEN];
    snprintf(path, sizeof path, "%s/%s%s", outdir, base, FILE_SUFFIX);

    // Une erreur remplace tout : ERROR:<pos> seul, aucun résultat partiel,
    // car out n'est écrit que si has_error vaut 0.
    int rc = 0;
    FILE *f = fopen(path, "wb");
    if (!f) {
        fprintf(stderr, "calc: cannot write '%s': %s\n", path, strerror(errno));
        rc = 1;
    } else {
        if (has_error) {
            if (fprintf(f, "ERROR:%zu\n", err_pos) < 0) rc = 1;
        } else if (out_len > 0) {
            if (fwrite(out, 1, out_len, f) != out_len) rc = 1;
        }
        if (fclose(f) != 0) rc = 1;
        if (rc) fprintf(stderr, "calc: error writing '%s'\n", path);
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
        } else if (argv[i][0] == '-' && argv[i][1] != '\0') {
            fprintf(stderr, "calc: unknown option '%s'\n", argv[i]);
            return 1;
        } else if (input) {
            fprintf(stderr, "calc: more than one input file\n");
            return 1;
        } else {
            input = argv[i];
        }
    }

    // Exactement un des deux : un fichier d'entrée, ou -d DIR.
    if ((dir == NULL) == (input == NULL)) {
        fprintf(stderr, "usage: calc [-d DIR] [-o OUTDIR] [input.txt]\n");
        return 1;
    }

    char base[PATH_MAX_LEN];
    // Plus grand que base : le suffixe DIR_SUFFIX est ajouté après le nom de
    // base, donc il faut de la place en plus pour que rien ne soit tronqué.
    char default_out[PATH_MAX_LEN + 64];
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
            // Seulement les "*.txt" directement dans DIR ; sous-dossiers
            // (même nommés x.txt) et autres fichiers ignorés.
            if (n <= 4 || strcmp(e->d_name + n - 4, ".txt") != 0) continue;

            char p[PATH_MAX_LEN];
            snprintf(p, sizeof p, "%s/%s", dir, e->d_name);

            struct stat st;
            if (stat(p, &st) == 0 && S_ISREG(st.st_mode)) {
                char b[PATH_MAX_LEN];
                base_name(e->d_name, b, sizeof b);
                if (process_file(p, b, outdir) != 0) rc = 1;
            }
        }
        closedir(d);
    }

    free(out);
    return rc;
}