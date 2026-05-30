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
typedef struct _Node Node;

typedef struct _List List;

Arena *GLOBAL_ARENA = null;
List *GLOBAL_LIST = null;
Token *TOKENS = null;

static inline void arena_free(Arena *arena);
static inline void list_free(List *l);

/// ERRORS
#define ERR_FILE_NOT_FOUND "File not found."
#define ERR_FILE_EXPECTED "Expected a file as argument."
#define ERR_UNKOWN_TOKEN "Unknown token."
#define ERR_OPEN_FILE "Failed to open file."
#define ERR_MEM_ALLOC "Memory allocation failed."
#define ERR_BIG_NUM "Number literal too long (max 23 digits)."
#define ERR_ON_NUM "Invalid number literal."
#define ERR_ON_DOUBLE "Number literal has multiple decimal points."
#define ERR_AN_ERROR "An error occurred at some stage of the compiler."
#define ERR_UINT_OVERFLOW "Uint overflow."
#define ERR_READ_FILE "An error occurred while reading the file."

#define ERR_LEXER_OOB "Lexer offset out of bounds."
#define ERR_LEXER_INVALID_CHAR "Invalid literal character."
#define ERR_LEXER_INVALID_CHAR_ESCAPE "Invalid escape character."
#define ERR_LEXER_INVALID_STRING "Invalid string."

#define ERR_PARSER_OOB "Parser offset out of bounds."

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
    if (GLOBAL_LIST != null)
        list_free(GLOBAL_LIST);
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

static inline ArenaChunk *chunk_create(uint size)
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

static inline Arena arena_create(uint size)
{
    // alinha o tamanho pra base 8
    size = align(size, 8);
    Arena arena = {chunk_create(size)};
    return arena;
}

static inline void *arena_alloc(Arena *arena, uint size)
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

static inline void arena_free(Arena *arena)
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

/// LIST
// a lista serve para dados alocados na heap que precisam ser desalocados após resize, arena não pode cobrir isso
// todos os lists serão salvos no list principal GLOBAL_LIST para serem liberados a fim do programa ou em caso de crash
#define ELEM_RAW 0  // free normal
#define ELEM_LIST 1 // list_free recursivo
#define ELEM_REF 2  // não libera, só referência

typedef struct _list_data
{
    void *ptr;
    int flag;
} list_data;

typedef struct _List
{
    list_data *data;
    uint size, cap;
} List;

static inline List *list_create(uint cap)
{
    List *l = (List *)calloc(1, sizeof(List));
    enforce(l == null, ERR_MEM_ALLOC);
    list_data *data = (list_data *)calloc(1, sizeof(list_data) * cap);
    enforce(data == null, ERR_MEM_ALLOC);
    l->cap = cap;
    l->data = data;
    l->size = 0;
    return l;
}

static inline void list_resize(List *l)
{
    uint new_cap = l->cap * 2;
    list_data *data = (list_data *)realloc(l->data, sizeof(list_data) * new_cap);
    enforce(data == null, ERR_MEM_ALLOC);
    l->data = data;
    l->cap = new_cap;
}

static inline void list_push(List *l, void *data, int flag)
{
    if (l->size == l->cap)
        list_resize(l);
    l->data[l->size++] = (list_data){.ptr = data, .flag = flag};
}

static inline void list_free(List *l)
{
    for (uint i = 0; i < l->size; i++)
        switch (l->data[i].flag)
        {
        case ELEM_LIST:
            list_free((List *)l->data[i].ptr);
            break;
        case ELEM_RAW:
            free(l->data[i].ptr);
            break;
        case ELEM_REF:
            break; // não faz nada
        }
    free(l->data);
    free(l);
}

/// LEXER
typedef enum _TokenKind
{
    // keywords
    TK_FN,
    TK_RETURN,
    TK_VAR,
    TK_CONST,
    TK_IF,
    TK_ELSE,
    TK_FOR,
    TK_WHILE,

    // literals
    TK_ID,
    TK_NUM,
    TK_NUMD, // double
    TK_NUMF, // float
    TK_STRING,
    TK_CHAR,

    // symbols
    TK_LBRACE,    // {
    TK_RBRACE,    // }
    TK_LPREN,     // (
    TK_RPREN,     // )
    TK_PLUS,      // +
    TK_MINUS,     // -
    TK_EQUALS,    // =
    TK_EEQUALS,   // ==
    TK_COLON,     // :
    TK_SEMICOLON, // ;
    TK_COMMA,     // ,
    TK_DOT,       // .
    TK_STAR,      // *
    TK_SLASH,     // /

    // eof
    TK_EOF, // só tem um caso de uso
} TokenKind;

typedef struct _Token
{
    TokenKind kind;
    union
    {
        long num; // pode armazenar 'char' sem problemas
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
    enforce(tokens == null, ERR_MEM_ALLOC);                               // os tokens em TOKENS são os antigos, sem chances de free(null)
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
                if (len == 3 && memcmp(str, "fun", 3) == 0)
                    kind = TK_FN;
                if (len == 3 && memcmp(str, "for", 3) == 0)
                    kind = TK_FOR;
                break;
            case 'i':
                if (len == 2 && memcmp(str, "if", 2) == 0)
                    kind = TK_IF;
                break;
            case 'e':
                if (len == 4 && memcmp(str, "else", 4) == 0)
                    kind = TK_ELSE;
                break;
            case 'c':
                if (len == 5 && memcmp(str, "const", 5) == 0)
                    kind = TK_CONST;
                break;
            case 'w':
                if (len == 5 && memcmp(str, "while", 5) == 0)
                    kind = TK_WHILE;
                break;
            case 'v':
                if (len == 3 && memcmp(str, "var", 3) == 0)
                    kind = TK_VAR;
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
            uint pstart = l->line_offset - 1;

            while ((isNum(lexer_peek(l)) || lexer_peek(l) == '.') && l->offset < l->filesize)
            {
                lexer_advance(l);
                if (lexer_peek(l) == '.' && isDouble == 1)
                {
                    // o pstart inicia antes do numero (pstart - 1)
                    // pstart + 1 => 67
                    // pstart + 2 => .
                    // pstart + 3 => .
                    nubc_message(ERR_ERROR, ERR_ON_DOUBLE, l->source, lexer_make_pos(l, pstart + 3)); // o + 3 move o ponteiro do erro pro '.' problematico
                    err = 1;
                }
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
            Position pos = lexer_make_pos(l, pstart);

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

        if (ch == '"')
        {
            uint start = l->line_offset;
            uint ptr_start = l->offset;

            while (lexer_peek(l) != '"' && l->offset < l->filesize)
            {
                if (lexer_match(l, '\n'))
                    break;
                if (lexer_match(l, '\\'))
                {
                    switch (lexer_peek(l))
                    {
                    case 'r':
                    case 'n':
                    case 't':
                    case '\\':
                    case '0':
                        break;
                    default:
                        nubc_message(ERR_ERROR, ERR_LEXER_INVALID_CHAR_ESCAPE, l->source, lexer_make_pos(l, start - 1));
                        err = 1;
                        break;
                    }
                }
                lexer_advance(l);
                continue;
            }

            if (!lexer_match(l, '"'))
            {
                nubc_message(ERR_ERROR, ERR_LEXER_INVALID_STRING, l->source, lexer_make_pos(l, start));
                err = 1;
                continue;
            }

            uint end = l->line_offset - 1;
            char *str = l->source + ptr_start;
            uint len = end - start;

            lexer_create_token(l, (Token){.kind = TK_STRING, .str = (NubStr){.ptr = str, .len = len}, .pos = lexer_make_pos(l, start - 1)});
            continue;
        }

        if (ch == '\'')
        {
            uint start = l->line_offset;
            char c = lexer_advance(l);

            if (c == '\\')
            {
                lexer_advance(l); // avança o caractere de escape
                switch (lexer_peek(l))
                {
                case 'n':
                    c = '\n';
                    break;
                case 'r':
                    c = '\r';
                    break;
                case 't':
                    c = '\t';
                    break;
                case '\\':
                    c = '\\';
                    break;
                case '0':
                    c = '\0';
                    break;
                default:
                    nubc_message(ERR_ERROR, ERR_LEXER_INVALID_CHAR_ESCAPE, l->source, lexer_make_pos(l, start - 1));
                    err = 1;
                    break;
                }
                lexer_advance(l);
            }

            if (!lexer_match(l, '\''))
            {
                nubc_message(ERR_ERROR, ERR_LEXER_INVALID_CHAR, l->source, lexer_make_pos(l, start - 1));
                err = 1;
                continue;
            }

            lexer_create_token(l, (Token){.kind = TK_CHAR, .num = (long)c, .pos = lexer_make_pos(l, start - 1)});
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
        case '=':
            k = TK_EQUALS;
            if (lexer_match(l, '='))
                k = TK_EEQUALS;
            break;
        case ':':
            k = TK_COLON;
            break;
        case ';':
            k = TK_SEMICOLON;
            break;
        case ',':
            k = TK_COMMA;
            break;
        case '.':
            k = TK_DOT;
            break;
        case '/':
            k = TK_SLASH;
            break;
        case '*':
            k = TK_STAR;
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
        case TK_EQUALS:
            printf("[EQUALS] ");
            continue;
        case TK_EEQUALS:
            printf("[EEQUALS] ");
            continue;
        case TK_STRING:
            printf("[STRING '%.*s'] ", tk.str.len, tk.str.ptr);
            continue;
        case TK_COLON:
            printf("[COLON] ");
            continue;
        case TK_COMMA:
            printf("[COMMA] ");
            continue;
        case TK_SEMICOLON:
            printf("[SEMICOLON] ");
            continue;
        case TK_DOT:
            printf("[DOT] ");
            continue;
        case TK_CHAR:
            printf("[CHAR '%c'] ", (char)tk.num);
            continue;
        case TK_VAR:
            printf("[VAR] ");
            continue;
        case TK_CONST:
            printf("[CONST] ");
            continue;
        case TK_IF:
            printf("[IF] ");
            continue;
        case TK_ELSE:
            printf("[ELSE] ");
            continue;
        case TK_FOR:
            printf("[FOR] ");
            continue;
        case TK_WHILE:
            printf("[WHILE] ");
            continue;
        case TK_SLASH:
            printf("[SLASH] ");
            continue;
        case TK_STAR:
            printf("[STAR] ");
            continue;
        default:
            printf("[DESCONHECIDO] ");
            continue;
        }
    }
    printf("\n");
}

/// PARSER
typedef enum _NodeKind
{
    NODE_BINARYEXPR,
    NODE_UNARYEXPR,

    NODE_INT_LIT,
    NODE_STR_LIT,
    NODE_CHAR_LIT,
    NODE_DOUBLE_LIT,
    NODE_FLOAT_LIT,

    NODE_FNDECL,

    NODE_RETURNSTMT,
} NodeKind;

typedef struct _NodeProgram
{
    Node **body;
    uint len;
} NodeProgram;

typedef struct _NodeBinaryExpr
{
    Node *left, *right;
    TokenKind op;
} NodeBinaryExpr;

typedef struct _NodeUnaryExpr
{
    Node *val;
    TokenKind op;
} NodeUnaryExpr;

typedef struct _NodeReturnStmt
{
    Node *val;
} NodeReturnStmt;

typedef struct _NodeFnArg
{
    NubStr name;
    Node *val;
} NodeFnArg;

typedef struct _NodeFnDecl
{
    NubStr name;
    List *arguments; // NodeFnArg
    List *body;      // Node*
} NodeFnDecl;

typedef struct _Node
{
    NodeKind kind;
    union
    {
        NodeBinaryExpr binaryExpr;
        NodeUnaryExpr unaryExpr;
        NodeReturnStmt retStmt;
        NodeFnDecl fnDecl;
        double doubleLit;
        float floatLit;
        NubStr strLit;
        char chLit;
        long intLit;
    } value;
    Position pos;
} Node;

typedef struct _Parser
{
    List *list;
    uint tk_len, offset;
    char *source;
} Parser;

static inline Parser parser_create(char *source, uint len)
{
    return (Parser){.source = source, .list = list_create(32), .tk_len = len, .offset = 0};
}

static inline Node *node_create()
{
    Node *node = (Node *)calloc(1, sizeof(Node));
    enforce(node == null, ERR_MEM_ALLOC);
    return node;
}

typedef enum _ParserPrecedence
{
    PRE_LOW,
    PRE_PLUS,
    PRE_MUL,
    PRE_HIGH,
} ParserPrecedence;

// parser_prototipos {{
ParserPrecedence parser_get_precedence(Token *tk);
static inline Node *parser_parse_intern(Parser *p);
// }}

static inline Token *parser_peek(Parser *p)
{
    enforce(p->offset == p->tk_len, ERR_PARSER_OOB);
    return &TOKENS[p->offset];
}

static inline Token *parser_advance(Parser *p)
{
    enforce(p->offset >= p->tk_len, ERR_PARSER_OOB);
    return &TOKENS[p->offset++];
}

static inline int parser_match(Parser *p, TokenKind kind)
{
    enforce(p->offset == p->tk_len, ERR_PARSER_OOB);
    if (TOKENS[p->offset].kind == kind)
    {
        parser_advance(p);
        return 1;
    }
    return 0;
}

static inline int parser_check(Parser *p, TokenKind kind)
{
    enforce(p->offset == p->tk_len, ERR_PARSER_OOB);
    return TOKENS[p->offset].kind == kind;
}

static inline Token *parser_future(Parser *p, uint i)
{
    enforce((p->offset + i) >= p->tk_len, ERR_PARSER_OOB);
    return &TOKENS[p->offset + i];
}

static inline Token *parser_consume(Parser *p, TokenKind kind, char *message)
{
    Token *tk = parser_peek(p);
    if (tk->kind == kind)
        return parser_advance(p);
    nubc_message(ERR_ERROR, message, p->source, tk->pos);
    enforce(1, message);
    return tk;
}

static inline int parser_is_decl(Parser *p)
{
    switch (parser_peek(p)->kind)
    {
    case TK_FN:
    case TK_VAR:
    case TK_CONST:
        return 1;
    default:
        return 0;
    }
}

static inline int parser_is_stmt(Parser *p)
{
    switch (parser_peek(p)->kind)
    {
    case TK_IF:
    case TK_FOR:
    case TK_WHILE:
    case TK_RETURN:
        return 1;
    default:
        return 0;
    }
}

Node *parser_parse_decl(Parser *p)
{
    Token *tk = parser_peek(p);
    switch (tk->kind)
    {
    case TK_FN:
    {
        parser_advance(p); // pula o fun
        Token *name = parser_consume(p, TK_ID, "Esperado um nome pra função.");

        // TODO: adicionar suporte a argumentos
        List *args = list_create(8L);
        list_push(p->list, args, ELEM_LIST);

        parser_consume(p, TK_LPREN, "Esperado '('.");
        parser_consume(p, TK_RPREN, "Esperado ')' após os argumentos.");

        List *body = list_create(16L);
        list_push(p->list, body, ELEM_LIST);

        parser_consume(p, TK_LBRACE, "Esperado '{' após os argumentos.");

        while (!parser_check(p, TK_RBRACE))
            list_push(body, parser_parse_intern(p), ELEM_RAW);

        parser_consume(p, TK_RBRACE, "Esperado '}' após a função.");

        Node *n = node_create();
        n->kind = NODE_FNDECL;
        n->value.fnDecl.name = name->str;
        n->value.fnDecl.arguments = args;
        n->value.fnDecl.body = body;
        return n;
    }
    default:
        return null;
    }
}

Node *parser_parse_stmt(Parser *p)
{
    return null;
}

// parse_expr {{
Node *parser_parse_expr_nud(Parser *p)
{
    Token *tk = parser_peek(p);
    switch (tk->kind)
    {
    case TK_NUM:
        parser_advance(p);
        Node *num = node_create();
        num->kind = NODE_INT_LIT;
        num->pos = num->pos;
        num->value.intLit = tk->num;
        return num;
    default:
        return null;
    }
}

Node *parser_parse_expr_led(Parser *p, Node *left)
{
    Token *tk = parser_peek(p);
    switch (tk->kind)
    {
    case TK_PLUS:
    case TK_MINUS:
    case TK_STAR:
    case TK_SLASH:
        return left; // TODO: binary expr
    default:
        return left;
    }
}

Node *parser_parse_expr(Parser *p, ParserPrecedence precedence)
{
    Node *left = parser_parse_expr_nud(p);
    if (left == null)
        return null;
    while (precedence < parser_get_precedence(parser_peek(p)))
        left = parser_parse_expr_led(p, left);
    return left;
}
// }}

ParserPrecedence parser_get_precedence(Token *tk)
{
    switch (tk->kind)
    {
    case TK_PLUS:
    case TK_MINUS:
        return PRE_PLUS;
    case TK_STAR:
    case TK_SLASH:
        return PRE_MUL;
    default:
        return PRE_LOW;
    }
}

static inline Node *parser_parse_intern(Parser *p)
{
    if (parser_is_decl(p))
        return parser_parse_decl(p);
    if (parser_is_stmt(p))
        return parser_parse_stmt(p);
    return parser_parse_expr(p, parser_get_precedence(parser_peek(p)));
}

void parser_parse(Parser *p)
{
    List *l = p->list;
    while (p->offset < p->tk_len)
    {
        Node *node = parser_parse_intern(p);
        if (node == null)
        {
            p->offset++;
            continue;
        }
        list_push(l, node, ELEM_RAW);
    }
}

static inline void node_debug(Node *n, uint depth)
{
    for (uint i = 0; i < depth; i++)
        printf("  ");

    switch (n->kind)
    {
    case NODE_FNDECL:
        printf("[FNDECL %.*s]\n", n->value.fnDecl.name.len, n->value.fnDecl.name.ptr);
        if (n->value.fnDecl.body)
            for (uint i = 0; i < n->value.fnDecl.body->size; i++)
                node_debug((Node *)n->value.fnDecl.body->data[i].ptr, depth + 1);
        break;
    case NODE_RETURNSTMT:
        printf("[RETURN]\n");
        if (n->value.retStmt.val)
            node_debug(n->value.retStmt.val, depth + 1);
        break;
    case NODE_BINARYEXPR:
        printf("[BINARY %d]\n", n->value.binaryExpr.op);
        node_debug(n->value.binaryExpr.left, depth + 1);
        node_debug(n->value.binaryExpr.right, depth + 1);
        break;
    case NODE_UNARYEXPR:
        printf("[UNARY %d]\n", n->value.unaryExpr.op);
        node_debug(n->value.unaryExpr.val, depth + 1);
        break;
    case NODE_INT_LIT:
        printf("[INT %ld]\n", n->value.intLit);
        break;
    case NODE_DOUBLE_LIT:
        printf("[DOUBLE %f]\n", n->value.doubleLit);
        break;
    case NODE_FLOAT_LIT:
        printf("[FLOAT %f]\n", n->value.floatLit);
        break;
    case NODE_STR_LIT:
        printf("[STRING '%.*s']\n", n->value.strLit.len, n->value.strLit.ptr);
        break;
    case NODE_CHAR_LIT:
        printf("[CHAR '%c']\n", n->value.chLit);
        break;
    }
}

static inline void parser_debug(Parser *p)
{
    for (uint i = 0; i < p->list->size; i++)
    {
        list_data d = p->list->data[i];
        if (d.flag == ELEM_RAW)
            node_debug((Node *)d.ptr, 0);
    }
}

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

    // list
    List *g_list = list_create(32L);
    GLOBAL_LIST = g_list;

    // lexer
    Lexer lexer = lexer_create(filename, file_size, source);
    err = lexer_tokenize(&lexer);
    enforce(err > 0, ERR_AN_ERROR);
    lexer_debug(&lexer);

    // parser
    Parser parser = parser_create(source, lexer.tk_size);
    list_push(GLOBAL_LIST, parser.list, ELEM_LIST);
    parser_parse(&parser);
    parser_debug(&parser);

    // end
    free(TOKENS);
    list_free(GLOBAL_LIST);
    arena_free(GLOBAL_ARENA);

    return 0;
}
