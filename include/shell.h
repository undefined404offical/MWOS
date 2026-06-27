#ifndef SHELL_H
#define SHELL_H

#include <stdint.h>
#include <stdbool.h>
#include <stddef.h>
#include <stdarg.h>

#define SHELL_MAX_INPUT       512
#define SHELL_MAX_HISTORY     100
#define SHELL_MAX_COMMANDS    64
#define SHELL_MAX_ARGS        32
#define SHELL_MAX_PATH        256
#define SHELL_MAX_NAME        32
#define SHELL_MAX_COMPLETIONS 64

typedef void (*shell_output_fn)(const char* str);
typedef void (*shell_command_fn)(int argc, char** argv);

typedef struct {
    char name[32];
    char description[128];
    shell_command_fn handler;
} shell_command_t;

typedef struct {
    char buffer[SHELL_MAX_INPUT];
    int length;
    int cursor;
} shell_input_t;

typedef struct {
    char entries[SHELL_MAX_HISTORY][SHELL_MAX_INPUT];
    int count;
    int current;
    int saved_pos;
} shell_history_t;

typedef struct {
    char matches[SHELL_MAX_COMPLETIONS][SHELL_MAX_INPUT];
    int count;
    int index;
    char prefix[SHELL_MAX_INPUT];
    int prefix_len;
} shell_completion_t;

typedef struct {
    char cwd[SHELL_MAX_PATH];
    char username[SHELL_MAX_NAME];
    char hostname[SHELL_MAX_NAME];

    shell_input_t input;
    shell_history_t history;
    shell_completion_t completion;

    shell_command_t commands[SHELL_MAX_COMMANDS];
    int command_count;

    shell_output_fn output;
    bool initialized;
    bool in_completion;
} shell_state_t;

void shell_init(void);
void shell_set_output(shell_output_fn fn);
void shell_process_char(char c);
void shell_process_key(uint8_t scancode, uint8_t ch);

void shell_print(const char* str);
void shell_printf(const char* fmt, ...);
void shell_print_prompt(void);

void shell_register_command(const char* name, const char* desc, shell_command_fn handler);
void shell_execute(const char* line);

shell_state_t* shell_get_state(void);

#endif
