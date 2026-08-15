#include <stdio.h>
#include <stdlib.h>
#include "parser.h"

static bool is_word(Token *token) {
    return token != NULL && token->type == TOKEN_WORD;
}

bool parse(Token *tokens) {
    Token *current = tokens;
    if(!is_word(current)) {
        return false;
    }
    while(current != NULL) {
        if(current->type == TOKEN_WORD) {
            current = current->next;
            continue;
        }
        // Detecting re-direction - >, < and >>!
        if(current->type == TOKEN_LT || current->type == TOKEN_GT || current->type == TOKEN_GTGT) {
            current = current->next;
            if(!is_word(current)) {
                return false;
            }
            current = current->next;
            continue;
        }
        // Other operators will be handled later!
        break;
    }
    return current == NULL;
}