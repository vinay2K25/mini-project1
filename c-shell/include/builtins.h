#ifndef BUILTINS_H
#define BUILTINS_H
#include <stdbool.h>
#include "lexer.h"

void initialise_builtins();
bool is_builtin_command(Token *tokens);
bool execute_builtin(Token *tokens);
void resume_job(Token *tokens);

#endif