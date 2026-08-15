#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <ctype.h>
#include <stdbool.h>
#include "lexer.h"
static void append_char(char *word, size_t *length, char c) {
    word[*length] = c;
    (*length)++;
}

static bool is_operator(char c) {
    return c == '|' || c == '&' || c == ';' || c == '<' || c == '>';
}

static Token *create_token(TokenType type, const char *value) {
    // Allocating memory for the token struct!
    Token *token = malloc(sizeof(Token));
    if(token == NULL) {
        perror("malloc");
        exit(EXIT_FAILURE);
    }
    token->type = type;
    token->next = NULL;
    if(value != NULL) {
        // Allocating memory for the string within the token struct!
        token->value = malloc(strlen(value) + 1);
        if(token->value == NULL) {
            perror("malloc");
            free(token);
            exit(EXIT_FAILURE);
        }
        strcpy(token->value, value);
    }
    else {
        token->value = NULL;
    }
    return token;
}

static void append_token(Token **head, Token **tail, Token *token) {
    if(*head == NULL) {
        *head = token;
        *tail = token;
    }
    else {
        (*tail)->next = token;
        *tail = token;
    }
}

Token *lex(const char *input) {
    Token *head = NULL;
    Token *tail = NULL;
    size_t i = 0;
    while(input[i] != '\0') {
        if(isspace((unsigned char)input[i])) {
            i++;
            continue;
        }
        // Detecting the various operators!
        if(input[i] == '|') {
            append_token(&head, &tail, create_token(TOKEN_PIPE, NULL));
            i++;
            continue;
        }
        if(input[i] == '&') {
            append_token(&head, &tail, create_token(TOKEN_AMP, NULL));
            i++;
            continue;
        }
        if(input[i] == ';') {
            append_token(&head, &tail, create_token(TOKEN_SEMI, NULL));
            i++;
            continue;
        }
        if(input[i] == '<') {
            append_token(&head, &tail, create_token(TOKEN_LT, NULL));
            i++;
            continue;
        }
        // Special case - if TOKEN_GT is detected, it could either be TOKEN_GT | TOKEN_GTGT!
        if(input[i] == '>') {
            if(input[i + 1] == '>') {
                append_token(&head, &tail, create_token(TOKEN_GTGT, NULL));
                i += 2;
            }
            else {
                append_token(&head, &tail, create_token(TOKEN_GT, NULL));
                i++;
            }
            continue;
        }

        char word[4096];
        size_t word_length = 0;
        
        while(input[i] != '\0') {
            if(isspace((unsigned char)input[i]) || is_operator(input[i])) {
                break;
            }
            if(input[i] == '\'') {
                i++;
                while(input[i] != '\0' && input[i] != '\'') {
                    append_char(word, &word_length, input[i]);
                    i++;
                }
                // We've reached the end of the input without detecting a closing single quote, this is a lexical error! 
                if(input[i] == '\0') {
                    free_tokens(head);
                    return NULL;
                }
                i++;
                continue;
            }
            if(input[i] == '"') {
                i++;
                while(input[i] != '\0' && input[i] != '"') {
                    // Two special escape sequences exist - "\\" which translates to "\" and "\"" which translates to """!
                    if(input[i] == '\\') {
                        if(input[i + 1] == '\\') {
                            append_char(word, &word_length, '\\');
                            i += 2;
                            continue;
                        }
                        if(input[i + 1] == '"') {
                            append_char(word, &word_length, '"');
                            i += 2;
                            continue;
                        }
                        // Say "\n" is the input - then both \ and 'n' must be appended to word!
                        append_char(word, &word_length, '\\');
                        i++;
                        if(input[i] != '\0') {
                            append_char(word, &word_length, input[i]);
                            i++;
                        }
                    }
                    append_char(word, &word_length, input[i]);
                    i++;
                }
                // No closing " was detected, a lexical error!
                if(input[i] == '\0') {
                    free_tokens(head);
                    return NULL;
                }
                i++;
                continue;
            }
            append_char(word, &word_length, input[i]);
            i++;
        }
        word[word_length] = '\0';
        Token *token = create_token(TOKEN_WORD, word);
        append_token(&head, &tail, token);
    }
    return head;
}

// De-bugging function, to test the lexer!
void print_tokens(Token *tokens) {
    while(tokens != NULL) {
        printf("TOKEN: type = %d, value = %s\n", tokens->type, tokens->value);
        tokens = tokens->next;
    }
}

void free_tokens(Token *tokens) {
    while(tokens != NULL) {
        Token *next = tokens->next;
        free(tokens->value);
        free(tokens);
        tokens = next;
    }
}