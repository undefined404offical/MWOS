#include "shell.h"
#include "drivers/fs/vfs.h"
#include "klog.h"
#include "memory.h"
#include "serial.h"
#include "string.h"
#include "terminal.h"

static shell_state_t g_shell;

void resolve_path(const char* path, char* full_path) {
    if (!path || !full_path)
        return;

    if (path[0] == '/') {
        strcpy(full_path, path);
    } else {
        strcpy(full_path, g_shell.cwd);
        if (strcmp(g_shell.cwd, "/") != 0) {
            strcat(full_path, "/");
        }
        strcat(full_path, path);
    }
}

static void input_init(shell_input_t* in) {
    in->buffer[0] = '\0';
    in->length = 0;
    in->cursor = 0;
}

static void input_clear(shell_input_t* in) {
    in->buffer[0] = '\0';
    in->length = 0;
    in->cursor = 0;
}

static void input_insert(shell_input_t* in, char c) {
    if (in->length >= SHELL_MAX_INPUT - 1)
        return;

    for (int i = in->length; i > in->cursor; i--) {
        in->buffer[i] = in->buffer[i - 1];
    }
    in->buffer[in->cursor] = c;
    in->cursor++;
    in->length++;
    in->buffer[in->length] = '\0';
}

static void input_delete(shell_input_t* in) {
    if (in->cursor >= in->length)
        return;

    for (int i = in->cursor; i < in->length; i++) {
        in->buffer[i] = in->buffer[i + 1];
    }
    in->length--;
    in->buffer[in->length] = '\0';
}

static void input_backspace(shell_input_t* in) {
    if (in->cursor <= 0)
        return;

    for (int i = in->cursor - 1; i < in->length; i++) {
        in->buffer[i] = in->buffer[i + 1];
    }
    in->cursor--;
    in->length--;
    in->buffer[in->length] = '\0';
}

static void input_move_left(shell_input_t* in) {
    if (in->cursor > 0)
        in->cursor--;
}

static void input_move_right(shell_input_t* in) {
    if (in->cursor < in->length)
        in->cursor++;
}

static void input_move_home(shell_input_t* in) { in->cursor = 0; }

static void input_move_end(shell_input_t* in) { in->cursor = in->length; }

static void history_init(shell_history_t* h) {
    h->count = 0;
    h->current = -1;
    h->saved_pos = -1;
}

static void history_add(shell_history_t* h, const char* entry) {
    if (!entry || !*entry)
        return;

    if (h->count > 0 && strcmp(h->entries[h->count - 1], entry) == 0) {
        return;
    }

    if (h->count < SHELL_MAX_HISTORY) {
        strcpy(h->entries[h->count], entry);
        h->count++;
    } else {
        for (int i = 0; i < SHELL_MAX_HISTORY - 1; i++) {
            strcpy(h->entries[i], h->entries[i + 1]);
        }
        strcpy(h->entries[SHELL_MAX_HISTORY - 1], entry);
    }

    h->current = h->count;
}

static const char* history_prev(shell_history_t* h) {
    if (h->count == 0)
        return NULL;

    if (h->current < 0)
        h->current = h->count - 1;
    else if (h->current > 0)
        h->current--;

    return h->entries[h->current];
}

static const char* history_next(shell_history_t* h) {
    if (h->current < 0 || h->current >= h->count - 1) {
        h->current = h->count;
        return "";
    }

    h->current++;
    return h->entries[h->current];
}

static void completion_init(shell_completion_t* c) {
    c->count = 0;
    c->index = 0;
    c->prefix[0] = '\0';
    c->prefix_len = 0;
}

static void completion_reset(shell_completion_t* c) {
    c->count = 0;
    c->index = 0;
}

static void completion_add(shell_completion_t* c, const char* match) {
    if (c->count >= SHELL_MAX_COMPLETIONS)
        return;
    strcpy(c->matches[c->count], match);
    c->count++;
}

static int str_common_prefix(const char* a, const char* b) {
    int i = 0;
    while (a[i] && b[i] && a[i] == b[i])
        i++;
    return i;
}

static void shell_find_completions(const char* prefix) {
    shell_completion_t* c = &g_shell.completion;
    completion_reset(c);

    if (!prefix || !*prefix)
        return;

    strcpy(c->prefix, prefix);
    c->prefix_len = strlen(prefix);

    for (int i = 0;
         i < g_shell.command_count && c->count < SHELL_MAX_COMPLETIONS; i++) {
        if (strncmp(g_shell.commands[i].name, prefix, c->prefix_len) == 0) {
            completion_add(c, g_shell.commands[i].name);
        }
    }
}

static const char* shell_get_next_completion(void) {
    shell_completion_t* c = &g_shell.completion;

    if (c->count == 0)
        return NULL;

    const char* match = c->matches[c->index];
    c->index = (c->index + 1) % c->count;

    return match;
}

static void builtin_echo(int argc, char** argv);
static void builtin_clear(int argc, char** argv);
static void builtin_ls(int argc, char** argv);
static void builtin_cd(int argc, char** argv);
static void builtin_pwd(int argc, char** argv);
static void builtin_cat(int argc, char** argv);
static void builtin_touch(int argc, char** argv);
static void builtin_mkdir(int argc, char** argv);
static void builtin_bincat(int argc, char** argv);
static void builtin_history(int argc, char** argv);
static void builtin_exit(int argc, char** argv);
static void builtin_whoami(int argc, char** argv);
static void builtin_uname(int argc, char** argv);
static void builtin_date(int argc, char** argv);

void shell_init(void) {
    memset(&g_shell, 0, sizeof(g_shell));

    strcpy(g_shell.cwd, "/");
    strcpy(g_shell.username, "root");
    strcpy(g_shell.hostname, "mwos");

    input_init(&g_shell.input);
    history_init(&g_shell.history);
    completion_init(&g_shell.completion);

    shell_register_command("echo", "Print text to terminal", builtin_echo);
    shell_register_command("clear", "Clear terminal screen", builtin_clear);
    shell_register_command("ls", "List directory contents", builtin_ls);
    shell_register_command("cd", "Change current directory", builtin_cd);
    shell_register_command("pwd", "Print working directory", builtin_pwd);
    shell_register_command("cat", "Display file contents", builtin_cat);
    shell_register_command("touch", "Create empty file", builtin_touch);
    shell_register_command("mkdir", "Create directory", builtin_mkdir);
    shell_register_command("bincat", "Hex dump file contents", builtin_bincat);
    shell_register_command("history", "Show command history", builtin_history);
    shell_register_command("exit", "Exit the shell", builtin_exit);
    shell_register_command("whoami", "Show current user", builtin_whoami);
    shell_register_command("uname", "Show system information", builtin_uname);
    shell_register_command("date", "Show current date/time", builtin_date);

    g_shell.initialized = true;

    kinfo("SHELL", "Initialized with %d commands", g_shell.command_count);
}

void shell_print_prompt(void) {
    shell_print("\n");
    shell_printf("%s@%s:%s$ ", g_shell.username, g_shell.hostname, g_shell.cwd);
}

void shell_set_output(shell_output_fn fn) { g_shell.output = fn; }

shell_state_t* shell_get_state(void) { return &g_shell; }

void shell_print(const char* str) {
    if (g_shell.output && str) {
        g_shell.output(str);
    }
}

void shell_printf(const char* fmt, ...) {
    if (!g_shell.output)
        return;

    char buf[512];
    va_list args;
    va_start(args, fmt);
    vsnprintf(buf, sizeof(buf), fmt, args);
    va_end(args);

    g_shell.output(buf);
}

void shell_register_command(const char* name, const char* desc,
                            shell_command_fn handler) {
    if (g_shell.command_count >= SHELL_MAX_COMMANDS)
        return;
    if (!name || !handler)
        return;

    shell_command_t* cmd = &g_shell.commands[g_shell.command_count];
    strncpy(cmd->name, name, sizeof(cmd->name) - 1);
    cmd->name[sizeof(cmd->name) - 1] = '\0';

    if (desc) {
        strncpy(cmd->description, desc, sizeof(cmd->description) - 1);
        cmd->description[sizeof(cmd->description) - 1] = '\0';
    } else {
        cmd->description[0] = '\0';
    }

    cmd->handler = handler;
    g_shell.command_count++;
}

static int parse_args(const char* line, char** argv, int max_args) {
    int argc = 0;
    char* buf = kmalloc(strlen(line) + 1);
    strcpy(buf, line);

    char* p = buf;
    bool in_quote = false;
    char quote_char = 0;

    while (*p && argc < max_args) {
        while (*p == ' ' || *p == '\t')
            p++;
        if (!*p)
            break;

        argv[argc] = p;
        argc++;

        while (*p) {
            if (*p == '"' || *p == '\'') {
                if (!in_quote) {
                    in_quote = true;
                    quote_char = *p;
                    char* dst = p;
                    char* src = p + 1;
                    while (*src) {
                        *dst++ = *src++;
                    }
                    *dst = '\0';
                    continue;
                } else if (*p == quote_char) {
                    in_quote = false;
                    *p = '\0';
                    p++;
                    break;
                }
            }

            if (!in_quote && (*p == ' ' || *p == '\t')) {
                *p = '\0';
                p++;
                break;
            }
            p++;
        }
    }

    return argc;
}

void shell_execute(const char* line) {
    if (!line || !*line)
        return;

    char* argv[SHELL_MAX_ARGS];
    int argc = parse_args(line, argv, SHELL_MAX_ARGS);

    if (argc == 0)
        return;

    const char* cmd_name = argv[0];

    for (int i = 0; i < g_shell.command_count; i++) {
        if (strcmp(g_shell.commands[i].name, cmd_name) == 0) {
            g_shell.commands[i].handler(argc, argv);
            return;
        }
    }

    shell_printf("Command not found: %s\n", cmd_name);
}

static void refresh_input_line(void) {
    if (!g_shell.output)
        return;

    shell_print("\r\033[K");
    shell_printf("%s@%s:%s$ ", g_shell.username, g_shell.hostname, g_shell.cwd);
    shell_print(g_shell.input.buffer);

    if (g_shell.input.cursor < g_shell.input.length) {
        int move_left = g_shell.input.length - g_shell.input.cursor;
        for (int i = 0; i < move_left; i++) {
            g_shell.output("\b");
        }
    }
}

static void handle_tab(void) {
    shell_input_t* in = &g_shell.input;

    int word_start = in->cursor;
    while (word_start > 0 && in->buffer[word_start - 1] != ' ') {
        word_start--;
    }

    char word[128];
    int word_len = in->cursor - word_start;
    if (word_len >= (int)sizeof(word))
        return;

    memcpy(word, in->buffer + word_start, word_len);
    word[word_len] = '\0';

    if (!g_shell.in_completion) {
        shell_find_completions(word);
        g_shell.in_completion = true;
        g_shell.completion.index = 0;
    }

    if (g_shell.completion.count == 0) {
        return;
    }

    if (g_shell.completion.count == 1) {
        const char* match = shell_get_next_completion();
        int match_len = strlen(match);

        for (int i = word_start; i < in->cursor; i++) {
            shell_print("\b");
        }

        int to_delete = in->cursor - word_start;
        for (int i = word_start; i < in->length; i++) {
            in->buffer[i] = in->buffer[i + to_delete];
        }
        in->length -= to_delete;
        in->cursor = word_start;

        for (int i = 0; i < match_len; i++) {
            input_insert(in, match[i]);
        }
        input_insert(in, ' ');

        g_shell.in_completion = false;
        refresh_input_line();
        return;
    }

    if (g_shell.completion.count > 1) {
        shell_print("\n");
        for (int i = 0; i < g_shell.completion.count; i++) {
            shell_printf("  %s", g_shell.completion.matches[i]);
        }
        shell_print("\n");

        shell_print_prompt();
        shell_print(in->buffer);
        g_shell.in_completion = false;
    }
}

static void handle_up_arrow(void) {
    const char* hist = history_prev(&g_shell.history);
    if (hist) {
        strcpy(g_shell.input.buffer, hist);
        g_shell.input.length = strlen(hist);
        g_shell.input.cursor = g_shell.input.length;
        refresh_input_line();
    }
}

static void handle_down_arrow(void) {
    const char* hist = history_next(&g_shell.history);
    if (hist) {
        strcpy(g_shell.input.buffer, hist);
        g_shell.input.length = strlen(hist);
        g_shell.input.cursor = g_shell.input.length;
        refresh_input_line();
    }
}

static void handle_left_arrow(void) {
    input_move_left(&g_shell.input);
    if (g_shell.output) {
        g_shell.output("\033[D");
    }
}

static void handle_right_arrow(void) {
    input_move_right(&g_shell.input);
    if (g_shell.output) {
        g_shell.output("\033[C");
    }
}

void shell_process_char(char c) {
    if (!g_shell.initialized || !g_shell.output)
        return;

    g_shell.in_completion = false;

    if (c == '\n' || c == '\r') {
        shell_print("\n");

        if (g_shell.input.length > 0) {
            g_shell.input.buffer[g_shell.input.length] = '\0';
            history_add(&g_shell.history, g_shell.input.buffer);
            shell_execute(g_shell.input.buffer);
            input_clear(&g_shell.input);
        }

        shell_printf("%s@%s:%s$ ", g_shell.username, g_shell.hostname,
                     g_shell.cwd);
        terminal_refresh();
        return;
    }

    if (c == '\b' || c == 127) {
        if (g_shell.input.cursor > 0) {
            input_backspace(&g_shell.input);
            shell_print("\b \b");
        }
        return;
    }

    if (c == '\t') {
        handle_tab();
        return;
    }

    if (c >= 32 && c < 127) {
        input_insert(&g_shell.input, c);

        if (g_shell.input.cursor < g_shell.input.length) {
            refresh_input_line();
        } else {
            char buf[2] = {c, 0};
            g_shell.output(buf);
        }
    }
}

void shell_process_key(uint8_t scancode, uint8_t ch) {
    (void)scancode;

    if (ch) {
        shell_process_char(ch);
    }
}

static void builtin_echo(int argc, char** argv) {
    for (int i = 1; i < argc; i++) {
        shell_print(argv[i]);
        if (i < argc - 1)
            shell_print(" ");
    }
    shell_print("\n");
}

static void builtin_clear(int argc, char** argv) {
    (void)argc;
    (void)argv;
    shell_print("\033[2J\033[H");
}

static void builtin_ls(int argc, char** argv) {
    (void)argc;
    (void)argv;

    const char* path = (argc > 1) ? argv[1] : g_shell.cwd;

    // 解析为绝对路径
    char abs_path[SHELL_MAX_PATH];
    resolve_path(path, abs_path);

    vfs_file_t dir;
    if (!vfs_open(abs_path, &dir, VFS_READ)) {
        shell_printf("ls: cannot access '%s': %s\n", path, vfs_get_error());
        return;
    }

    if (!dir.is_directory) {
        shell_printf("ls: '%s' is not a directory\n", path);
        vfs_close(&dir);
        return;
    }

    int count = 0;
    vfs_dirent_t entry;

    while (vfs_read_dir(&dir, &entry)) {
        // 跳过 . 和 ..
        if (strcmp(entry.name, ".") == 0 || strcmp(entry.name, "..") == 0)
            continue;

        if (entry.is_directory) {
            shell_printf("  \033[1;34m%s\033[0m/    [DIR]\n", entry.name);
        } else {
            // 文件大小用 KB/MB 表示
            uint32_t sz = entry.size;
            if (sz >= 1048576)
                shell_printf("  %-13s %u.%u MB\n", entry.name, sz / 1048576,
                             (sz % 1048576) / 104858);
            else if (sz >= 1024)
                shell_printf("  %-13s %u.%u KB\n", entry.name, sz / 1024,
                             (sz % 1024) / 103);
            else
                shell_printf("  %-13s %u B\n", entry.name, sz);
        }
        count++;
    }

    vfs_close(&dir);
    shell_printf("\n  total %d entries\n", count);
}

static void builtin_cd(int argc, char** argv) {
    if (argc < 2) {
        strcpy(g_shell.cwd, "/");
        return;
    }

    const char* target = argv[1];

    // 处理 .. 特殊情况
    if (strcmp(target, "..") == 0) {
        char* last_slash = (char*)strrchr(g_shell.cwd, '/');
        if (last_slash && last_slash != g_shell.cwd) {
            *last_slash = '\0';
        } else {
            strcpy(g_shell.cwd, "/");
        }
        return;
    }

    // 解析绝对路径
    char abs_path[SHELL_MAX_PATH];
    resolve_path(target, abs_path);

    // 验证目标目录存在
    vfs_file_t dir;
    if (!vfs_open(abs_path, &dir, VFS_READ)) {
        shell_printf("cd: %s: %s\n", target, vfs_get_error());
        return;
    }

    if (!dir.is_directory) {
        shell_printf("cd: %s: Not a directory\n", target);
        vfs_close(&dir);
        return;
    }
    vfs_close(&dir);

    // 更新 cwd
    strncpy(g_shell.cwd, abs_path, SHELL_MAX_PATH - 1);
    g_shell.cwd[SHELL_MAX_PATH - 1] = '\0';
}

static void builtin_pwd(int argc, char** argv) {
    (void)argc;
    (void)argv;
    shell_printf("%s\n", g_shell.cwd);
}

static void builtin_cat(int argc, char** argv) {
    if (argc < 2) {
        shell_print("Usage: cat <filename>\n");
        return;
    }

    const char* path = argv[1];

    char abs_path[SHELL_MAX_PATH];
    resolve_path(path, abs_path);

    vfs_file_t file;
    if (!vfs_open(abs_path, &file, VFS_READ)) {
        shell_printf("cat: %s: %s\n", path, vfs_get_error());
        return;
    }

    if (file.is_directory) {
        shell_printf("cat: %s: Is a directory\n", path);
        vfs_close(&file);
        return;
    }

    if (file.file_size == 0) {
        shell_print("(empty)\n");
        vfs_close(&file);
        return;
    }

    // 读取并输出
    uint8_t* buf = (uint8_t*)kmalloc(file.file_size + 1);
    if (!buf) {
        shell_print("cat: out of memory\n");
        vfs_close(&file);
        return;
    }

    // 分块读取
    uint32_t total = 0;
    while (total < file.file_size) {
        uint32_t chunk = file.file_size - total;
        if (chunk > 512)
            chunk = 512;
        uint32_t bytes_read = 0;
        if (!vfs_read(&file, buf + total, chunk, &bytes_read)) {
            break;
        }
        if (bytes_read == 0) break;
        total += bytes_read;
    }

    buf[total] = '\0';

    // 确保输出以换行结尾
    bool has_trailing_newline = (total > 0 && buf[total - 1] == '\n');

    shell_print((const char*)buf);

    if (!has_trailing_newline) {
        shell_print("\n");
    }

    kfree(buf);
    vfs_close(&file);
}

static void builtin_touch(int argc, char** argv) {
    if (argc < 2) {
        shell_print("Usage: touch <filename>\n");
        return;
    }

    const char* path = argv[1];

    char abs_path[SHELL_MAX_PATH];
    resolve_path(path, abs_path);

    // 检查文件是否已存在
    if (vfs_file_exists(abs_path)) {
        return; // 已存在，touch 传统行为是更新时间戳，这里简化处理
    }

    if (!vfs_create_file(abs_path)) {
        shell_printf("touch: cannot create '%s': %s\n", path, vfs_get_error());
    }
}

static void builtin_mkdir(int argc, char** argv) {
    if (argc < 2) {
        shell_print("Usage: mkdir <directory>\n");
        return;
    }

    const char* path = argv[1];

    char abs_path[SHELL_MAX_PATH];
    resolve_path(path, abs_path);

    if (!vfs_create_dir(abs_path)) {
        shell_printf("mkdir: cannot create directory '%s': %s\n", path, vfs_get_error());
    }
}

static void builtin_bincat(int argc, char** argv) {
    if (argc < 2) {
        shell_print("Usage: bincat <filename>\n");
        return;
    }

    const char* path = argv[1];

    char abs_path[SHELL_MAX_PATH];
    resolve_path(path, abs_path);

    vfs_file_t file;
    if (!vfs_open(abs_path, &file, VFS_READ)) {
        shell_printf("bincat: %s: %s\n", path, vfs_get_error());
        return;
    }

    if (file.is_directory) {
        shell_printf("bincat: %s: Is a directory\n", path);
        vfs_close(&file);
        return;
    }

    if (file.file_size == 0) {
        shell_print("bincat: (empty file)\n");
        vfs_close(&file);
        return;
    }

    uint32_t fsize = file.file_size;
    uint8_t* buf = (uint8_t*)kmalloc(fsize);
    if (!buf) {
        shell_print("bincat: out of memory\n");
        vfs_close(&file);
        return;
    }

    // 分块读取
    uint32_t total = 0;
    while (total < fsize) {
        uint32_t chunk = fsize - total;
        if (chunk > 512) chunk = 512;
        uint32_t bytes_read = 0;
        if (!vfs_read(&file, buf + total, chunk, &bytes_read))
            break;
        if (bytes_read == 0) break;
        total += bytes_read;
    }
    vfs_close(&file);

    // 十六进制输出
    uint32_t size = fsize;
    shell_printf("Hex dump of '%s' (%u bytes):\n", path, size);
    shell_print("  Offset      00 01 02 03 04 05 06 07  08 09 0A 0B 0C 0D 0E 0F  ASCII\n");
    shell_print("  --------    -----------------------------------------------  ----------------\n");

    for (uint32_t offset = 0; offset < size; offset += 16) {
        // 偏移
        char off_str[12];
        sprintf(off_str, "  %08X", offset);
        shell_print(off_str);
        shell_print("    ");

        // 十六进制区域
        for (int i = 0; i < 16; i++) {
            if (offset + i < size) {
                char hex_byte[3];
                sprintf(hex_byte, "%02X", buf[offset + i]);
                shell_print(hex_byte);
            } else {
                shell_print("  ");
            }

            if (i == 7)
                shell_print("  ");
            else
                shell_print(" ");
        }

        shell_print(" ");

        // ASCII 区域
        for (int i = 0; i < 16; i++) {
            if (offset + i < size) {
                char c = (char)buf[offset + i];
                if (c >= 32 && c <= 126) {
                    char s[2] = {c, '\0'};
                    shell_print(s);
                } else {
                    shell_print(".");
                }
            }
        }

        shell_print("\n");
    }

    shell_printf("\n  %u bytes\n", size);
    kfree(buf);
}

static void builtin_history(int argc, char** argv) {
    int count = g_shell.history.count;

    if (argc > 1) {
        count = atoi(argv[1]);
        if (count <= 0 || count > g_shell.history.count) {
            count = g_shell.history.count;
        }
    }

    int start = g_shell.history.count - count;

    shell_print("\033[1mCommand History:\033[0m\n");
    shell_print("================\n");

    for (int i = start; i < g_shell.history.count; i++) {
        shell_printf("%4d  %s\n", i + 1, g_shell.history.entries[i]);
    }
}

static void builtin_exit(int argc, char** argv) {
    (void)argc;
    (void)argv;
    shell_print("Goodbye!\n");
}

static void builtin_whoami(int argc, char** argv) {
    (void)argc;
    (void)argv;
    shell_printf("%s\n", g_shell.username);
}

static void builtin_uname(int argc, char** argv) {
    bool show_all = false;

    for (int i = 1; i < argc; i++) {
        if (strcmp(argv[i], "-a") == 0) {
            show_all = true;
            break;
        }
    }

    if (show_all || argc == 1) {
        shell_print("MWOS 1.0.0 mwos-kernel x86_64\n");
        return;
    }

    for (int i = 1; i < argc; i++) {
        if (strcmp(argv[i], "-s") == 0)
            shell_print("MWOS\n");
        else if (strcmp(argv[i], "-n") == 0)
            shell_print("mwos\n");
        else if (strcmp(argv[i], "-r") == 0)
            shell_print("1.0.0\n");
        else if (strcmp(argv[i], "-m") == 0)
            shell_print("x86_64\n");
    }
}

static void builtin_date(int argc, char** argv) {
    (void)argc;
    (void)argv;
    shell_print("Date/Time: (RTC integration coming soon...)\n");
}
