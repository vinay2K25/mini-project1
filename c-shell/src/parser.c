#include <stdio.h>
#include <stdlib.h>
#include "parser.h"

static bool is_word(Token *token) {
    return token != NULL && token->type == TOKEN_WORD;
}

static bool parse_arg(Token **current);

static bool parse_cmd(Token **current) {
    if(!is_word(*current)) {
        return false;
    }
    *current = (*current)->next;
    return parse_arg(current);
}

static bool parse_tgt(Token **current) {
    if(!is_word(*current)) {
        return false;
    }
    *current = (*current)->next;
    return parse_arg(current);
}

static bool parse_bg(Token **current) {
    // Since the grammar is right-linear, we can end the command with a single &!
    if(*current == NULL) {
        return true;
    }
    if(!is_word(*current)) {
        return false;
    }
    *current = (*current)->next;
    return parse_arg(current);
}

static bool parse_arg(Token **current) {
    if(*current == NULL) {
        return true;
    }
    switch((*current)->type) {
        case TOKEN_WORD:
            *current = (*current)->next;
            return parse_arg(current);
        case TOKEN_LT:
        case TOKEN_GT:
        case TOKEN_GTGT:
            *current = (*current)->next;
            return parse_tgt(current);
        case TOKEN_PIPE:
            *current = (*current)->next;
            return parse_cmd(current);
        case TOKEN_SEMI:
            *current = (*current)->next;
            return parse_cmd(current);
        case TOKEN_AMP:
            *current = (*current)->next;
            return parse_bg(current);
        default:
            return false;
    }
}

static bool parse_line(Token **current) {
    if(*current == NULL) {
        return true;
    }
    if(!is_word(*current)) {
        return false;
    }
    *current = (*current)->next;
    return parse_arg(current);
}

bool parse(Token *tokens) {
    Token *current = tokens;
    if(!parse_line(&current)) {
        return false;
    }
    return current == NULL;
}