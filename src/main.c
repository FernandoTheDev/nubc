#include <stdio.h>
#include <stdlib.h>
#include <stdint.h>
#include <string.h>
#include <limits.h>

#define null NULL
#define enforce(cond, msg) __enforce(cond, msg, __func__, __FILE__, __LINE__)

typedef int8_t byte;
typedef uint8_t ubyte;
typedef uint16_t ushort;
typedef uint32_t uint;
typedef uint64_t ulong;
typedef const char *string;

typedef struct _ArenaChunk ArenaChunk;
typedef struct _Arena Arena;

Arena *GLOBAL_ARENA = null;
Arena *LEXER_ARENA = null;

void arena_free(Arena *arena);

/// UTILS


void __enforce(int cond, string msg, string func, string file, int line)
{
    if (!cond)
        return;
    if (GLOBAL_ARENA != null)
        arena_free(GLOBAL_ARENA);
    if (LEXER_ARENA != null)
        arena_free(LEXER_ARENA);
    printf("Ocorreu um erro na função '%s()' em %s:%d\n", func, file, line);
    printf("Mensagem: %s\n", msg);
    exit(1);
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
    enforce(chunk == null, "O Chunk alocado é nulo.");

    char *data = (char *)calloc(1, size);
    enforce(data == null, "O data alocado é nulo.");

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
        ArenaChunk *c = chunk_create(chunk->cap * 2);
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

/// PARSER

/// MAIN

int main(int argc, string *argv)
{
    enforce(argc != 2, "Esperado um unico argumento como arquivo.");
    
    Arena global = arena_create(1024u);
    GLOBAL_ARENA = &global;
    
    string filename = argv[1];
    FILE* file = fopen(filename, "rb");
    enforce(file == null, "Erro ao abrir arquivo.");

    fseek(file, 0, SEEK_END);
    long file_size = ftell(file);
    rewind(file);

    string source = (string) arena_alloc(GLOBAL_ARENA, (uint) file_size);
    enforce(source == null, "Erro ao alocar memoria.");
    fread(file, 1, file_size, file);
    fclose(file);

    arena_free(GLOBAL_ARENA);
    return 0;
}
