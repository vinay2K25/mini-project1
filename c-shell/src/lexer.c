#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <ctype.h>
#include "lexer.h"

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
        size_t start = i;
        while(input[i] != '\0' && !isspace((unsigned char)input[i])) {
            i++;
        }
        size_t length = i - start;
        char *word = malloc(length + 1);
        if(word == NULL) {
            perror("malloc");
            free_tokens(head);
            exit(EXIT_FAILURE);
        }
        memcpy(word, input + start, length);
        word[length] = '\0';
        Token *token = create_token(TOKEN_WORD, word);
        free(word);
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