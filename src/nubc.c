#include <stdio.h>
#include <stdlib.h>
#include <stdint.h>
#include <string.h>
#include <limits.h>
#include <errno.h>

#ifdef _WIN32
#include <windows.h>
#endif

#define null NULL
#define enforce(cond, msg) __enforce(cond, msg, __func__, __FILE__, __LINE__)

#ifndef _WIN32
typedef int8_t byte;
#endif
typedef uint8_t ubyte;
typedef uint16_t ushort;
typedef uint32_t uint;
typedef uint64_t ulong;

typedef struct _ArenaChunk ArenaChunk;
typedef struct _Arena Arena;
typedef struct _Lexer Lexer;
typedef struct _Token Token;

Arena *GLOBAL_ARENA = null;
Token* TOKENS = null; // salva de forma global mas genérica para não utilizar

void arena_free(Arena *arena);

/// ERRORS
#define ERR_FILE_NOT_FOUND "File not found."
#define ERR_UNKOWN_TOKEN "Unknown token."
#define ERR_FILE_EXPECTED "Expected a file as argument."
#define ERR_OPEN_FILE "Failed to open file."
#define ERR_MEM_ALLOC "Memory allocation failed."
#define ERR_BIG_NUM "Number literal too long (max 23 digits)."
#define ERR_ON_NUM "Invalid number literal."
#define ERR_ON_DOUBLE "Number literal has multiple decimal points."
#define ERR_LEXER_OOB "Lexer offset out of bounds."
#define ERR_AN_ERROR "An error occurred."
#define ERR_UINT_OVERFLOW "Uint overflow."
#define ERR_READ_FILE "An error occurred while reading the file."

/// UTILS
typedef struct _NubStr
{
    char *ptr;
    ushort len;
} NubStr;

static inline void nubstr_print(NubStr *str)
{
    printf("%.*s", str->len, str->ptr);
}

static inline void nubstr_println(NubStr *str)
{
    nubstr_print(str);
    printf("\n");
}

void __enforce(int cond, const char *msg, const char *func, const char *file, int line)
{
    if (!cond)
        return;
    if (GLOBAL_ARENA != null)
        arena_free(GLOBAL_ARENA);
    if (TOKENS != null)
        free(TOKENS);
    printf("An error occurred in the function '%s()' in %s:%d\n", func, file, line);
    printf("Message: %s\n", msg);
    exit(1);
}

typedef enum _ErrKind
{
    ERR_WARNING,
    ERR_ERROR,
} ErrKind;

typedef struct _Position
{
    char *filename, *dir;
    struct
    {
        uint offset, line;
    } start, end;
} Position;

void nubc_message(ErrKind kind, char *message, char *source, Position pos)
{
    /*
     * file:l:o: ErrKind: 'MSG'
     *
     *     LINE
     *     ^~~~
     */

    uint tab = 4;
    size_t src_size = strlen(source);
    uint current_line = 1;
    char *line_start = source;

    for (size_t i = 0; i < src_size; i++)
    {
        if (current_line == pos.start.line)
            break;
        if (source[i] == '\n')
        {
            current_line++;
            line_start = &source[i + 1];
        }
    }

    size_t line_len = 0;
    while (line_start[line_len] != '\n' && line_start[line_len] != '\0')
        line_len++;

    fprintf(stderr, "%s:%u:%u: %s: '%s'\n",
            pos.filename,
            pos.start.line,
            pos.start.offset + 2, // resolve o cursor do editor de código
            kind == ERR_ERROR ? "error" : "warning",
            message);

    fprintf(stderr, "\n%*s%.*s\n", tab, "", (int)line_len, line_start);
    fprintf(stderr, "%*s^", tab + (int)pos.start.offset, "");

    uint span = pos.end.offset > pos.start.offset
                    ? pos.end.offset - pos.start.offset - 1
                    : 0;

    for (uint i = 0; i < span; i++)
        fputc('~', stderr);

    fputc('\n', stderr);
}

// ARENA
typedef struct _ArenaChunk
{
    char *data;
    uint cap;  // capacidade
    uint size; // tamanho atual
    ArenaChunk *next;
} ArenaChunk;

typedef struct _Arena
{
    ArenaChunk *chunk; // chunk atual
} Arena;

static inline uint align(uint size, uint base)
{
    return (size + (base - 1)) & ~(base - 1);
}

ArenaChunk *chunk_create(uint size)
{
    ArenaChunk *chunk = (ArenaChunk *)calloc(1, sizeof(ArenaChunk));
    enforce(chunk == null, ERR_MEM_ALLOC);

    char *data = (char *)calloc(1, size);
    enforce(data == null, ERR_MEM_ALLOC);

    chunk->cap = size;
    chunk->size = 0;
    chunk->data = data;
    chunk->next = null;

    return chunk;
}

Arena arena_create(uint size)
{
    // alinha o tamanho pra base 8
    size = align(size, 8);
    Arena arena = {chunk_create(size)};
    return arena;
}

void *arena_alloc(Arena *arena, uint size)
{
    // alinha o tamanho pra base 8
    size = align(size, 8);
    ArenaChunk *chunk = arena->chunk;
    // caso de criar um novo chunk (REALLOC)
    if ((chunk->size + size) > chunk->cap)
    {
        ArenaChunk *c = chunk_create((chunk->cap + size) * 2);
        c->next = chunk;
        chunk = c;
        arena->chunk = chunk;
    }
    char *data = chunk->data + chunk->size;
    chunk->size += size;
    return (void *)data;
}

void arena_free(Arena *arena)
{
    ArenaChunk *chunk = arena->chunk;
    while (chunk != null)
    {
        free(chunk->data);
        ArenaChunk *next = chunk->next;
        free(chunk);
        chunk = next;
    }
}

/// LEXER
typedef enum _TokenKind
{
    // keywords
    TK_FN,
    TK_RETURN,

    // literals
    TK_ID,
    TK_NUM,
    TK_NUMD, // double
    TK_NUMF, // float

    // symbols
    TK_LBRACE, // {
    TK_RBRACE, // }
    TK_LPREN,  // (
    TK_RPREN,  // )
    TK_PLUS,   // +
    TK_MINUS,  // -

    // eof
    TK_EOF, // só tem um caso de uso
} TokenKind;

typedef struct _Token
{
    TokenKind kind;
    union
    {
        long num;
        double numd;
        float numf;
        NubStr str;
    };
    Position pos;
} Token;

typedef struct _Lexer
{
    Token *tokens;
    char *filename, *source;
    uint offset, line_offset, line, tk_cap, tk_size, filesize;
} Lexer;

Lexer lexer_create(char *filename, uint filesize, char *source)
{
    const uint CAP_INICIAL = 64u;
    Token *tokens = (Token *)calloc(1, sizeof(Token) * CAP_INICIAL);
    enforce(tokens == null, ERR_MEM_ALLOC);
    TOKENS = tokens;
    return (Lexer){
        .filename = filename,
        .line = 1,
        .line_offset = 0,
        .offset = 0,
        .tk_cap = CAP_INICIAL,
        .tk_size = 0,
        .filesize = filesize,
        .source = source,
        .tokens = tokens};
}

static inline void lexer_resize(Lexer *l)
{
    uint new_cap = l->tk_cap * 2;
    Token *tokens = (Token *)realloc(l->tokens, sizeof(Token) * new_cap); // o realloc ja vai liberar os TOKENS antigos se for um sucessos
    enforce(tokens == null, ERR_MEM_ALLOC); // os tokens em TOKENS são os antigos, sem chances de free(null)
    l->tokens = tokens;
    TOKENS = tokens;
    l->tk_cap = new_cap;
}

static inline void lexer_create_token(Lexer *l, Token tk)
{
    if (l->tk_size == l->tk_cap)
        lexer_resize(l);
    l->tokens[l->tk_size++] = tk;
}

static inline char lexer_advance(Lexer *l)
{
    l->line_offset++;
    enforce(l->offset >= l->filesize, ERR_LEXER_OOB);
    return l->source[l->offset++];
}

static inline char lexer_peek(Lexer *l)
{
    return l->source[l->offset];
}

static inline int lexer_match(Lexer *l, char ch)
{
    if (l->source[l->offset] == ch)
    {
        lexer_advance(l);
        return 1;
    }
    return 0;
}

static inline Position lexer_make_pos(Lexer *l, uint start)
{
    return (Position){
        .dir = ".", // TODO: improve
        .filename = l->filename,
        .start.offset = start,
        .start.line = l->line,
        .end.offset = l->line_offset,
        .end.line = l->line,
    };
}

// lexer utils {{
static inline int isNum(char ch)
{
    return ch >= '0' && ch <= '9';
}

static inline int isAlpha(char ch)
{
    return (ch >= 'a' && ch <= 'z') || (ch >= 'A' && ch <= 'Z') || ch == '_';
}

static inline int isAlphanum(char ch)
{
    return isAlpha(ch) || isNum(ch);
}
// }}

ubyte lexer_tokenize(Lexer *l)
{
    ubyte err = 0;
    while (l->offset < l->filesize)
    {
        TokenKind k = TK_EOF;
        char ch = lexer_advance(l);

        if (ch == ' ' || ch == '\t' || ch == '\r')
            continue;

        if (ch == '\n')
        {
            l->line++;
            l->line_offset = 0;
            continue;
        }

        if (isAlpha(ch))
        {
            uint start = l->offset - 1;

            while (isAlphanum(lexer_peek(l)) && l->offset < l->filesize)
            {
                lexer_advance(l);
                continue;
            }

            uint end = l->offset;
            char *str = l->source + start;
            uint len = end - start;
            TokenKind kind = TK_ID;

            switch (str[0])
            {
            case 'f':
                if (len == 4 && memcmp(str, "func", 4) == 0)
                    kind = TK_FN;
                break;
            case 'r':
                if (len == 6 && memcmp(str, "return", 6) == 0)
                    kind = TK_RETURN;
                break;
            default:
                break;
            }

            lexer_create_token(l, (Token){.kind = kind, .str = (NubStr){.ptr = str, .len = len}, .pos = lexer_make_pos(l, start)});
            continue;
        }

        if (isNum(ch))
        {
            int isDouble = 0;
            uint start = l->offset - 1;

            while ((isNum(lexer_peek(l)) || lexer_peek(l) == '.') && l->offset < l->filesize)
            {
                lexer_advance(l);
                enforce(lexer_peek(l) == '.' && isDouble == 1, ERR_ON_DOUBLE);
                if (lexer_peek(l) == '.' && isDouble == 0)
                    isDouble = 1;
                continue;
            }

            uint end = l->offset;
            uint len = end - start;
            enforce(len >= 24, ERR_BIG_NUM);

            char *str_num = l->source + start;
            char buffer[24];
            memcpy(buffer, str_num, len);
            buffer[len] = '\0';

            int isFloat = (lexer_match(l, 'f') || lexer_match(l, 'F')) ? 1 : 0;
            if (isFloat)
                isDouble = 0;

            char *endptr;
            errno = 0;
            Token tk;
            Position pos = lexer_make_pos(l, start);

            if (isDouble)
                tk = (Token){.kind = TK_NUMD, .numd = strtod(buffer, &endptr), .pos = pos};
            else if (isFloat)
                tk = (Token){.kind = TK_NUMF, .numf = strtof(buffer, &endptr), .pos = pos};
            else
                tk = (Token){.kind = TK_NUM, .num = strtol(buffer, &endptr, 10), .pos = pos};

            enforce(buffer == endptr || errno == ERANGE, ERR_ON_NUM);
            lexer_create_token(l, tk);
            continue;
        }

        switch (ch)
        {
        case '(':
            k = TK_LPREN;
            break;
        case ')':
            k = TK_RPREN;
            break;
        case '{':
            k = TK_LBRACE;
            break;
        case '}':
            k = TK_RBRACE;
            break;
        case '+':
            k = TK_PLUS;
            break;
        case '-':
            k = TK_MINUS;
            break;
        default:
            break;
        }

        if (k != TK_EOF)
        {
            lexer_create_token(l, (Token){.kind = k});
            continue;
        }

        nubc_message(ERR_ERROR, ERR_UNKOWN_TOKEN, l->source, lexer_make_pos(l, l->line_offset - 1));
        err = 1;
    }
    return err;
}

void lexer_debug(Lexer *l)
{
    for (uint i = 0; i < l->tk_size; i++)
    {
        Token tk = l->tokens[i];
        switch (tk.kind)
        {
        case TK_FN:
            printf("[FN] ");
            continue;
        case TK_RETURN:
            printf("[RETURN] ");
            continue;
        case TK_ID:
            printf("[ID ");
            nubstr_print(&tk.str);
            printf("] ");
            continue;
        case TK_LBRACE:
            printf("[LBRACE] ");
            continue;
        case TK_RBRACE:
            printf("[RBRACE] ");
            continue;
        case TK_LPREN:
            printf("[LPAREN] ");
            continue;
        case TK_RPREN:
            printf("[RPAREN] ");
            continue;
        case TK_PLUS:
            printf("[PLUS] ");
            continue;
        case TK_MINUS:
            printf("[MINUS] ");
            continue;
        case TK_NUM:
            printf("[LONG %ld] ", tk.num);
            continue;
        case TK_NUMD:
            printf("[DOUBLE %f] ", tk.numd);
            continue;
        case TK_NUMF:
            printf("[FLOAT %f] ", tk.numf);
            continue;
        default:
            printf("[DESCONHECIDO]");
            continue;
        }
    }
    printf("\n");
}

/// PARSER

/// MAIN
int main(int argc, char **argv)
{
#ifdef _WIN32
    SetConsoleOutputCP(65001); // UTF-8
#endif

    enforce(argc != 2, ERR_FILE_EXPECTED);

    // arenas
    const uint ARENA_CAP = 1024u;

    Arena a_global = arena_create(ARENA_CAP);
    GLOBAL_ARENA = &a_global;

    // readfile
    char *filename = argv[1];
    FILE *file = fopen(filename, "rb");
    enforce(file == null, ERR_OPEN_FILE);

    fseek(file, 0, SEEK_END);
    uint file_size = (uint)ftell(file);
    enforce(file_size == UINT32_MAX, ERR_UINT_OVERFLOW); // valida se o cast foi valido
    rewind(file);

    char *source = (char *)arena_alloc(GLOBAL_ARENA, (uint)file_size);
    enforce(source == null, ERR_MEM_ALLOC);
    fread(source, 1, file_size, file);
    enforce(strlen(source) != file_size, ERR_READ_FILE); // garante que o numero de bytes lidos foi o total do file_size
    fclose(file);

    // errors
    ubyte err = 0;

    // lexer
    Lexer lexer = lexer_create(filename, file_size, source);
    err = lexer_tokenize(&lexer);
    enforce(err > 0, ERR_AN_ERROR);
    lexer_debug(&lexer);

    // end
    free(TOKENS);
    arena_free(GLOBAL_ARENA);
    return 0;
}
