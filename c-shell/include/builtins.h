#ifndef BUILTINS_H
#define BUILTINS_H
#include <stdbool.h>
#include "lexer.h"

void initialise_builtins();
bool execute_builtin(Token *tokens);

#endif