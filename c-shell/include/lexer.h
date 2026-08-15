#ifndef LEXER_H
#define LEXER_H
#include <stdbool.h>

typedef enum {
    TOKEN_WORD,
    TOKEN_PIPE,
    TOKEN_AMP,
    TOKEN_SEMI,
    TOKEN_LT,
    TOKEN_GT,
    TOKEN_GTGT
} TokenType;

typedef struct Token {
    TokenType type;
    char *value;
    struct Token *next;
} Token;

Token *lex(const char *input, bool *lex_error);

// De-bugging function to test the lexer!
void print_tokens(Token *tokens);

void free_tokens(Token *tokens);



#endif