#include <X11/Xlib.h>
#include <X11/Xutil.h>
#include <X11/keysym.h>
#include <ctype.h>
#include <dirent.h>
#include <errno.h>
#include <fcntl.h>
#include <signal.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <sys/stat.h>
#include <sys/select.h>
#include <sys/time.h>
#include <sys/wait.h>
#include <unistd.h>

#define DEFAULT_BAR_WIDTH 600
#define DEFAULT_INPUT_HEIGHT 40
#define MIN_BAR_WIDTH 120
#define MAX_CMD_LEN 256
#define TEXT_PADDING_X 12
#define TEXT_PADDING_Y 8
#define SUGGESTIONS_PADDING_TOP 6
#define SUGGESTIONS_PADDING_BOTTOM 6
#define MAX_SUGGESTIONS 8
#define MAX_TOKENS 32
#define CURSOR_BLINK_MS 500
#define OUTPUT_PADDING_TOP 8
#define OUTPUT_PADDING_BOTTOM 10

void run_command(const char *cmd) {
    if (fork() == 0) {
        setsid();
        char *args[] = {"/bin/sh", "-c", (char *)cmd, NULL};
        execvp(args[0], args);
        _exit(1);
    }
}

static int start_captured_command(const char *cmd, pid_t *child_pid, int *read_fd) {
    int pipe_fd[2];

    if (*read_fd != -1) {
        return -1;
    }
    if (pipe(pipe_fd) != 0) {
        return -1;
    }

    pid_t pid = fork();
    if (pid < 0) {
        close(pipe_fd[0]);
        close(pipe_fd[1]);
        return -1;
    }

    if (pid == 0) {
        setsid();
        close(pipe_fd[0]);
        dup2(pipe_fd[1], STDOUT_FILENO);
        dup2(pipe_fd[1], STDERR_FILENO);
        close(pipe_fd[1]);
        execl(
            "/bin/sh",
            "sh",
            "-c",
            "if command -v stdbuf >/dev/null 2>&1; then "
            "exec stdbuf -oL -eL /bin/sh -c \"$1\"; "
            "else "
            "exec /bin/sh -c \"$1\"; "
            "fi",
            "sh",
            cmd,
            (char *)NULL
        );
        _exit(127);
    }

    close(pipe_fd[1]);
    int flags = fcntl(pipe_fd[0], F_GETFL, 0);
    if (flags >= 0) {
        fcntl(pipe_fd[0], F_SETFL, flags | O_NONBLOCK);
    }

    *child_pid = pid;
    *read_fd = pipe_fd[0];
    return 0;
}

static void stop_retained_process(pid_t *child_pid) {
    if (*child_pid <= 0) {
        return;
    }

    kill(-(*child_pid), SIGTERM);
    waitpid(*child_pid, NULL, 0);
    *child_pid = -1;
}

static int parse_positive_int(const char *value, int *out) {
    char *end = NULL;
    errno = 0;
    long parsed = strtol(value, &end, 10);
    if (errno != 0 || end == value || *end != '\0' || parsed <= 0 || parsed > 10000) {
        return -1;
    }
    *out = (int)parsed;
    return 0;
}

static int append_unique_string(char ***items, size_t *count, size_t *cap, const char *value) {
    for (size_t i = 0; i < *count; i++) {
        if (strcmp((*items)[i], value) == 0) {
            return 0;
        }
    }

    if (*count == *cap) {
        size_t new_cap = (*cap == 0) ? 128 : (*cap * 2);
        char **grown = realloc(*items, new_cap * sizeof(char *));
        if (!grown) {
            return -1;
        }
        *items = grown;
        *cap = new_cap;
    }

    (*items)[*count] = strdup(value);
    if (!(*items)[*count]) {
        return -1;
    }
    (*count)++;
    return 0;
}

static int compare_string_ptrs(const void *a, const void *b) {
    const char *const *sa = (const char *const *)a;
    const char *const *sb = (const char *const *)b;
    return strcmp(*sa, *sb);
}

static void load_path_commands(char ***commands, size_t *command_count) {
    const char *path_env = getenv("PATH");
    if (!path_env || !*path_env) {
        return;
    }

    char *path_dup = strdup(path_env);
    if (!path_dup) {
        return;
    }

    size_t cap = 0;
    char *saveptr = NULL;
    for (char *dir_path = strtok_r(path_dup, ":", &saveptr);
         dir_path != NULL;
         dir_path = strtok_r(NULL, ":", &saveptr)) {
        if (*dir_path == '\0') {
            continue;
        }

        DIR *dir = opendir(dir_path);
        if (!dir) {
            continue;
        }

        struct dirent *entry = NULL;
        while ((entry = readdir(dir)) != NULL) {
            const char *name = entry->d_name;
            if (name[0] == '.' || strchr(name, '/')) {
                continue;
            }

            size_t full_len = strlen(dir_path) + 1 + strlen(name) + 1;
            char *full = malloc(full_len);
            if (!full) {
                continue;
            }
            snprintf(full, full_len, "%s/%s", dir_path, name);

            struct stat st;
            int is_exec = (access(full, X_OK) == 0);
            int is_file = (stat(full, &st) == 0 && !S_ISDIR(st.st_mode));
            free(full);

            if (is_exec && is_file) {
                append_unique_string(commands, command_count, &cap, name);
            }
        }

        closedir(dir);
    }

    if (*command_count > 1) {
        qsort(*commands, *command_count, sizeof(char *), compare_string_ptrs);
    }
    free(path_dup);
}

static void free_path_commands(char **commands, size_t command_count) {
    for (size_t i = 0; i < command_count; i++) {
        free(commands[i]);
    }
    free(commands);
}

static XFontStruct *load_scaled_font(Display *dpy, GC gc, int input_height, int forced_font_px) {
    int target_px = forced_font_px > 0 ? forced_font_px : input_height - (2 * TEXT_PADDING_Y);
    if (target_px < 8) {
        target_px = 8;
    }

    const int deltas[] = {0, -2, 2, -4, 4, -6, 6};
    char pattern[128];

    for (size_t i = 0; i < sizeof(deltas) / sizeof(deltas[0]); i++) {
        int px = target_px + deltas[i];
        if (px < 8) {
            continue;
        }

        snprintf(pattern, sizeof(pattern), "-misc-fixed-*-*-*-*-%d-*-*-*-*-*-*-*", px);
        XFontStruct *font = XLoadQueryFont(dpy, pattern);
        if (font) {
            XSetFont(dpy, gc, font->fid);
            return font;
        }

        snprintf(pattern, sizeof(pattern), "-*-*-medium-r-normal-*-*-%d-*-*-*-*-*-*", px);
        font = XLoadQueryFont(dpy, pattern);
        if (font) {
            XSetFont(dpy, gc, font->fid);
            return font;
        }
    }

    const char *fallback_fonts[] = {"12x24", "10x20", "9x15", "8x13", "fixed"};
    for (size_t i = 0; i < sizeof(fallback_fonts) / sizeof(fallback_fonts[0]); i++) {
        XFontStruct *font = XLoadQueryFont(dpy, fallback_fonts[i]);
        if (font) {
            XSetFont(dpy, gc, font->fid);
            return font;
        }
    }

    return NULL;
}

static int fit_chars_to_width(const XFontStruct *font, const char *text, int len, int max_width) {
    if (len <= 0) {
        return 0;
    }
    if (!font || max_width <= 0) {
        return len > 0 ? 1 : 0;
    }

    int lo = 1;
    int hi = len;
    int best = 1;
    while (lo <= hi) {
        int mid = lo + (hi - lo) / 2;
        int w = XTextWidth((XFontStruct *)font, text, mid);
        if (w <= max_width) {
            best = mid;
            lo = mid + 1;
        } else {
            hi = mid - 1;
        }
    }
    return best;
}

static int wrapped_line_count_for_segment(const XFontStruct *font, const char *text, int len, int max_width) {
    if (len <= 0) {
        return 1;
    }

    int lines = 0;
    int pos = 0;
    while (pos < len) {
        int take = fit_chars_to_width(font, text + pos, len - pos, max_width);
        if (take <= 0) {
            take = 1;
        }

        int draw_len = take;
        if (pos + take < len) {
            int last_space = -1;
            for (int i = 0; i < take; i++) {
                if (text[pos + i] == ' ' || text[pos + i] == '\t') {
                    last_space = i;
                }
            }
            if (last_space > 0) {
                draw_len = last_space;
            }
        }
        if (draw_len <= 0) {
            draw_len = 1;
        }

        lines++;
        pos += draw_len;
        while (pos < len && (text[pos] == ' ' || text[pos] == '\t')) {
            pos++;
        }
    }
    return lines;
}

static int wrapped_line_count_for_text(const XFontStruct *font, const char *text, size_t len, int max_width) {
    if (len == 0) {
        return 1;
    }

    int total = 0;
    size_t start = 0;
    while (start <= len) {
        size_t end = start;
        while (end < len && text[end] != '\n') {
            end++;
        }
        total += wrapped_line_count_for_segment(font, text + start, (int)(end - start), max_width);
        if (end >= len) {
            break;
        }
        start = end + 1;
    }
    return total;
}

static void draw_wrapped_segment(
    Display *dpy,
    Window win,
    GC gc,
    const XFontStruct *font,
    int x,
    int *y,
    int line_height,
    int max_width,
    const char *text,
    int len
) {
    if (len <= 0) {
        *y += line_height;
        return;
    }

    int pos = 0;
    while (pos < len) {
        int take = fit_chars_to_width(font, text + pos, len - pos, max_width);
        if (take <= 0) {
            take = 1;
        }

        int draw_len = take;
        if (pos + take < len) {
            int last_space = -1;
            for (int i = 0; i < take; i++) {
                if (text[pos + i] == ' ' || text[pos + i] == '\t') {
                    last_space = i;
                }
            }
            if (last_space > 0) {
                draw_len = last_space;
            }
        }
        if (draw_len <= 0) {
            draw_len = 1;
        }

        XDrawString(dpy, win, gc, x, *y, text + pos, draw_len);
        *y += line_height;
        pos += draw_len;

        while (pos < len && (text[pos] == ' ' || text[pos] == '\t')) {
            pos++;
        }
    }
}

static void append_output(char **output, size_t *output_len, size_t *output_cap, const char *chunk, size_t chunk_len) {
    if (chunk_len == 0) {
        return;
    }
    size_t needed = *output_len + chunk_len + 1;
    if (needed > *output_cap) {
        size_t new_cap = (*output_cap == 0) ? 512 : *output_cap;
        while (new_cap < needed) {
            new_cap *= 2;
        }
        char *grown = realloc(*output, new_cap);
        if (!grown) {
            return;
        }
        *output = grown;
        *output_cap = new_cap;
    }
    memcpy(*output + *output_len, chunk, chunk_len);
    *output_len += chunk_len;
    (*output)[*output_len] = '\0';
}

typedef struct {
    int start;
    int len;
} TokenRange;

typedef struct {
    const char **values;
    int value_count;
} SuggestionPool;

typedef struct CommandHelpCache {
    char *command;
    char **values;
    int value_count;
    int value_cap;
    struct CommandHelpCache *next;
} CommandHelpCache;

static CommandHelpCache *g_help_cache = NULL;

static int parse_tokens(const char *buf, int len, TokenRange *tokens, int max_tokens) {
    int count = 0;
    int i = 0;
    while (i < len) {
        while (i < len && isspace((unsigned char)buf[i])) {
            i++;
        }
        if (i >= len) {
            break;
        }
        int start = i;
        while (i < len && !isspace((unsigned char)buf[i])) {
            i++;
        }
        if (count < max_tokens) {
            tokens[count].start = start;
            tokens[count].len = i - start;
        }
        count++;
    }
    return count > max_tokens ? max_tokens : count;
}

static int completion_target(
    const char *buf,
    int len,
    const TokenRange *tokens,
    int token_count,
    int *token_index,
    int *token_start,
    int *token_len
) {
    if (len > 0 && !isspace((unsigned char)buf[len - 1]) && token_count > 0) {
        *token_index = token_count - 1;
        *token_start = tokens[*token_index].start;
        *token_len = tokens[*token_index].len;
        return 1;
    }
    *token_index = token_count;
    *token_start = len;
    *token_len = 0;
    return 1;
}

static int append_suggestions_from_path(
    char **path_commands,
    size_t path_command_count,
    const char *prefix,
    const char **suggestions,
    int count,
    int max_count
) {
    size_t prefix_len = strlen(prefix);
    for (size_t i = 0; i < path_command_count && count < max_count; i++) {
        if (strncmp(path_commands[i], prefix, prefix_len) == 0) {
            suggestions[count++] = path_commands[i];
        }
    }
    return count;
}

static int append_unique_owned_value(CommandHelpCache *cache, const char *value) {
    for (int i = 0; i < cache->value_count; i++) {
        if (strcmp(cache->values[i], value) == 0) {
            return 0;
        }
    }

    if (cache->value_count == cache->value_cap) {
        int new_cap = (cache->value_cap == 0) ? 64 : cache->value_cap * 2;
        char **grown = realloc(cache->values, (size_t)new_cap * sizeof(char *));
        if (!grown) {
            return -1;
        }
        cache->values = grown;
        cache->value_cap = new_cap;
    }

    cache->values[cache->value_count] = strdup(value);
    if (!cache->values[cache->value_count]) {
        return -1;
    }
    cache->value_count++;
    return 0;
}

static int is_common_help_word(const char *w) {
    static const char *stop[] = {
        "usage", "options", "option", "command", "commands", "available", "the", "and", "for",
        "with", "from", "this", "that", "these", "those", "show", "list", "set", "get", "run",
        "help", "version", "file", "files", "path", "paths", "name", "names", "default", "all",
        "more", "less", "print", "output", "input", "value", "values", "argument", "arguments"
    };
    for (size_t i = 0; i < sizeof(stop) / sizeof(stop[0]); i++) {
        if (strcmp(w, stop[i]) == 0) {
            return 1;
        }
    }
    return 0;
}

static int is_help_keyword(const char *w) {
    size_t len = strlen(w);
    if (len < 2 || len >= MAX_CMD_LEN) {
        return 0;
    }
    if (!isalpha((unsigned char)w[0])) {
        return 0;
    }
    int has_alpha = 0;
    for (size_t i = 0; i < len; i++) {
        unsigned char c = (unsigned char)w[i];
        if (!(isalnum(c) || c == '-' || c == '_' || c == '.')) {
            return 0;
        }
        if (isalpha(c)) {
            has_alpha = 1;
        }
        if (isupper(c)) {
            return 0;
        }
    }
    return has_alpha && !is_common_help_word(w);
}

static void extract_help_values_from_line(CommandHelpCache *cache, const char *line) {
    const char *p = line;

    while (*p) {
        if (*p == '-' && (p == line || isspace((unsigned char)p[-1]) || p[-1] == ',' || p[-1] == '(')) {
            const char *start = p;
            const char *q = p + 1;
            while (*q && !isspace((unsigned char)*q) && *q != ',' && *q != ';' && *q != ')' && *q != ']' && *q != '=') {
                q++;
            }
            int len = (int)(q - start);
            if (len >= 2 && len < MAX_CMD_LEN) {
                char opt[MAX_CMD_LEN] = {0};
                memcpy(opt, start, (size_t)len);
                opt[len] = '\0';
                append_unique_owned_value(cache, opt);
            }
            p = q;
            continue;
        }
        p++;
    }

    p = line;
    while (*p) {
        while (*p && !isalnum((unsigned char)*p)) {
            p++;
        }
        if (!*p) {
            break;
        }
        const char *start = p;
        while (*p && (isalnum((unsigned char)*p) || *p == '-' || *p == '_' || *p == '.')) {
            p++;
        }
        int len = (int)(p - start);
        if (len > 0 && len < MAX_CMD_LEN) {
            char word[MAX_CMD_LEN] = {0};
            memcpy(word, start, (size_t)len);
            word[len] = '\0';
            if (is_help_keyword(word)) {
                append_unique_owned_value(cache, word);
            }
        }
    }
}

static CommandHelpCache *find_help_cache(const char *command) {
    for (CommandHelpCache *c = g_help_cache; c; c = c->next) {
        if (strcmp(c->command, command) == 0) {
            return c;
        }
    }
    return NULL;
}

static void run_help_probe(CommandHelpCache *cache, const char *command, const char *arg) {
    int pipe_fd[2];
    if (pipe(pipe_fd) != 0) {
        return;
    }

    pid_t pid = fork();
    if (pid == 0) {
        close(pipe_fd[0]);
        dup2(pipe_fd[1], STDOUT_FILENO);
        dup2(pipe_fd[1], STDERR_FILENO);
        close(pipe_fd[1]);
        execlp(command, command, arg, (char *)NULL);
        _exit(127);
    }

    close(pipe_fd[1]);
    if (pid < 0) {
        close(pipe_fd[0]);
        return;
    }

    FILE *fp = fdopen(pipe_fd[0], "r");
    if (fp) {
        char line[1024];
        while (fgets(line, sizeof(line), fp)) {
            extract_help_values_from_line(cache, line);
        }
        fclose(fp);
    } else {
        close(pipe_fd[0]);
    }

    waitpid(pid, NULL, 0);
}

static CommandHelpCache *load_help_cache_for_command(const char *command) {
    CommandHelpCache *cache = calloc(1, sizeof(CommandHelpCache));
    if (!cache) {
        return NULL;
    }
    cache->command = strdup(command);
    if (!cache->command) {
        free(cache);
        return NULL;
    }

    run_help_probe(cache, command, "--help");
    if (cache->value_count == 0) {
        run_help_probe(cache, command, "-h");
    }

    cache->next = g_help_cache;
    g_help_cache = cache;
    return cache;
}

static int append_suggestions_from_command_help(
    const char *command,
    const char *prefix,
    int want_options,
    const char **suggestions,
    int count,
    int max_count
) {
    if (!command || !*command) {
        return count;
    }

    CommandHelpCache *cache = find_help_cache(command);
    if (!cache) {
        cache = load_help_cache_for_command(command);
    }
    if (!cache || cache->value_count == 0) {
        return count;
    }

    for (int i = 0; i < cache->value_count && count < max_count; i++) {
        const char *v = cache->values[i];
        int is_opt = (v[0] == '-');
        if ((want_options && !is_opt) || (!want_options && is_opt)) {
            continue;
        }
        if (strncmp(v, prefix, strlen(prefix)) != 0) {
            continue;
        }
        int exists = 0;
        for (int j = 0; j < count; j++) {
            if (strcmp(suggestions[j], v) == 0) {
                exists = 1;
                break;
            }
        }
        if (!exists) {
            suggestions[count++] = v;
        }
    }
    return count;
}

static int append_suggestions_from_filesystem(
    const char *prefix,
    const char **suggestions,
    int count,
    int max_count
) {
    if (!prefix || prefix[0] == '\0') {
        return count;
    }

    static char fs_buf[MAX_SUGGESTIONS][MAX_CMD_LEN];
    int fs_index = 0;

    const char *slash = strrchr(prefix, '/');
    char dir_path[MAX_CMD_LEN] = ".";
    char base[MAX_CMD_LEN] = {0};
    char lead[MAX_CMD_LEN] = {0};

    if (slash) {
        int dir_len = (int)(slash - prefix);
        if (dir_len == 0) {
            strcpy(dir_path, "/");
        } else if (dir_len < MAX_CMD_LEN) {
            memcpy(dir_path, prefix, (size_t)dir_len);
            dir_path[dir_len] = '\0';
        }

        strncpy(base, slash + 1, sizeof(base) - 1);
        if (dir_len > 0 && dir_len < MAX_CMD_LEN) {
            memcpy(lead, prefix, (size_t)(slash - prefix + 1));
            lead[slash - prefix + 1] = '\0';
        } else if (dir_len == 0) {
            strcpy(lead, "/");
        }
    } else {
        strncpy(base, prefix, sizeof(base) - 1);
    }

    DIR *dir = opendir(dir_path);
    if (!dir) {
        return count;
    }

    struct dirent *entry = NULL;
    size_t base_len = strlen(base);
    while ((entry = readdir(dir)) != NULL && count < max_count && fs_index < MAX_SUGGESTIONS) {
        if (strncmp(entry->d_name, base, base_len) != 0) {
            continue;
        }
        if (base[0] != '.' && entry->d_name[0] == '.') {
            continue;
        }

        char candidate[MAX_CMD_LEN] = {0};
        size_t lead_len = strlen(lead);
        size_t name_len = strlen(entry->d_name);
        size_t needed = lead_len + name_len + 1;
        if (needed > sizeof(candidate)) {
            continue;
        }
        memcpy(candidate, lead, lead_len);
        memcpy(candidate + lead_len, entry->d_name, name_len);
        candidate[lead_len + name_len] = '\0';

        int exists = 0;
        for (int i = 0; i < count; i++) {
            if (strcmp(suggestions[i], candidate) == 0) {
                exists = 1;
                break;
            }
        }
        if (exists) {
            continue;
        }

        strncpy(fs_buf[fs_index], candidate, MAX_CMD_LEN - 1);
        suggestions[count++] = fs_buf[fs_index++];
    }

    closedir(dir);
    return count;
}

static long long now_ms(void) {
    struct timeval tv;
    gettimeofday(&tv, NULL);
    return ((long long)tv.tv_sec * 1000LL) + (tv.tv_usec / 1000);
}

static void free_help_cache(void) {
    CommandHelpCache *c = g_help_cache;
    while (c) {
        CommandHelpCache *next = c->next;
        free(c->command);
        for (int i = 0; i < c->value_count; i++) {
            free(c->values[i]);
        }
        free(c->values);
        free(c);
        c = next;
    }
    g_help_cache = NULL;
}

static int build_suggestions_for_input(
    char **path_commands,
    size_t path_command_count,
    const char *cmd_buf,
    int cmd_len,
    const char **suggestions,
    int max_suggestions,
    int *token_start_out,
    int *token_len_out,
    int *token_index_out
) {
    TokenRange tokens[MAX_TOKENS] = {0};
    int token_count = parse_tokens(cmd_buf, cmd_len, tokens, MAX_TOKENS);
    int token_index = 0;
    int token_start = 0;
    int token_len = 0;
    completion_target(cmd_buf, cmd_len, tokens, token_count, &token_index, &token_start, &token_len);

    if (token_start_out) {
        *token_start_out = token_start;
    }
    if (token_len_out) {
        *token_len_out = token_len;
    }
    if (token_index_out) {
        *token_index_out = token_index;
    }

    char prefix[MAX_CMD_LEN] = {0};
    if (token_len > 0) {
        int copy_len = token_len >= MAX_CMD_LEN ? MAX_CMD_LEN - 1 : token_len;
        memcpy(prefix, cmd_buf + token_start, (size_t)copy_len);
        prefix[copy_len] = '\0';
    }

    int count = 0;
    if (token_index == 0) {
        return append_suggestions_from_path(path_commands, path_command_count, prefix, suggestions, count, max_suggestions);
    }

    if (token_count <= 0) {
        return 0;
    }

    char command[MAX_CMD_LEN] = {0};
    int command_len = tokens[0].len >= MAX_CMD_LEN ? MAX_CMD_LEN - 1 : tokens[0].len;
    memcpy(command, cmd_buf + tokens[0].start, (size_t)command_len);
    command[command_len] = '\0';

    if (prefix[0] == '-') {
        count = append_suggestions_from_command_help(command, prefix, 1, suggestions, count, max_suggestions);
        return count;
    }

    count = append_suggestions_from_command_help(command, prefix, 0, suggestions, count, max_suggestions);
    if (count == 0) {
        count = append_suggestions_from_filesystem(prefix, suggestions, count, max_suggestions);
    }
    return count;
}

static int apply_suggestion_to_input(
    char *cmd_buf,
    int *cmd_len,
    int token_start,
    int token_len,
    const char *suggestion
) {
    if (!cmd_buf || !cmd_len || !suggestion || suggestion[0] == '\0') {
        return -1;
    }
    if (token_start < 0 || token_len < 0 || token_start + token_len > *cmd_len) {
        return -1;
    }

    int suggestion_len = (int)strlen(suggestion);
    int suffix_start = token_start + token_len;
    int suffix_len = *cmd_len - suffix_start;
    int new_len = token_start + suggestion_len + suffix_len;
    if (new_len >= MAX_CMD_LEN) {
        return -1;
    }

    char new_buf[MAX_CMD_LEN] = {0};
    if (token_start > 0) {
        memcpy(new_buf, cmd_buf, (size_t)token_start);
    }
    memcpy(new_buf + token_start, suggestion, (size_t)suggestion_len);
    if (suffix_len > 0) {
        memcpy(new_buf + token_start + suggestion_len, cmd_buf + suffix_start, (size_t)suffix_len);
    }

    memcpy(cmd_buf, new_buf, (size_t)new_len);
    cmd_buf[new_len] = '\0';
    *cmd_len = new_len;
    return 0;
}

static void measure_wrapped_segment_end(
    const XFontStruct *font,
    const char *text,
    int len,
    int max_width,
    int *line_count,
    int *last_line_width
) {
    if (len <= 0) {
        *line_count = 1;
        *last_line_width = 0;
        return;
    }

    int lines = 0;
    int last_width = 0;
    int pos = 0;
    while (pos < len) {
        int take = fit_chars_to_width(font, text + pos, len - pos, max_width);
        if (take <= 0) {
            take = 1;
        }

        int draw_len = take;
        if (pos + take < len) {
            int last_space = -1;
            for (int i = 0; i < take; i++) {
                if (text[pos + i] == ' ' || text[pos + i] == '\t') {
                    last_space = i;
                }
            }
            if (last_space > 0) {
                draw_len = last_space;
            }
        }
        if (draw_len <= 0) {
            draw_len = 1;
        }

        lines++;
        last_width = XTextWidth((XFontStruct *)font, text + pos, draw_len);
        pos += draw_len;
        while (pos < len && (text[pos] == ' ' || text[pos] == '\t')) {
            pos++;
        }
    }

    *line_count = lines;
    *last_line_width = last_width;
}

static void redraw(
    Display *dpy,
    Window win,
    GC gc,
    unsigned long black,
    int bar_width,
    int input_height,
    int text_x,
    int text_padding_y,
    const XFontStruct *font,
    int font_ascent,
    int line_height,
    const char *cmd_buf,
    int cmd_len,
    int cursor_visible,
    char **path_commands,
    size_t path_command_count,
    int tab_cycle_active,
    const char *tab_cycle_query,
    int retain_mode,
    const char *output,
    size_t output_len,
    int *window_height
) {
    int text_width = bar_width - (2 * text_x);
    if (text_width < 1) {
        text_width = 1;
    }

    int input_lines = wrapped_line_count_for_text(font, cmd_buf, (size_t)cmd_len, text_width);
    int min_input_content_height = (2 * text_padding_y) + (input_lines * line_height);
    int input_area_height = input_height;
    if (input_area_height < min_input_content_height) {
        input_area_height = min_input_content_height;
    }

    const char *suggestions[MAX_SUGGESTIONS] = {0};
    const char *query_buf = cmd_buf;
    int query_len = cmd_len;
    if (tab_cycle_active && tab_cycle_query && tab_cycle_query[0] != '\0') {
        query_buf = tab_cycle_query;
        query_len = (int)strlen(tab_cycle_query);
    }
    int suggestion_count = build_suggestions_for_input(
        path_commands,
        path_command_count,
        query_buf,
        query_len,
        suggestions,
        MAX_SUGGESTIONS,
        NULL,
        NULL,
        NULL
    );
    int suggestion_lines = 0;
    for (int i = 0; i < suggestion_count; i++) {
        suggestion_lines += wrapped_line_count_for_segment(
            font,
            suggestions[i],
            (int)strlen(suggestions[i]),
            text_width
        );
    }
    int suggestions_height = 0;
    if (suggestion_lines > 0) {
        suggestions_height = SUGGESTIONS_PADDING_TOP + (suggestion_lines * line_height) + SUGGESTIONS_PADDING_BOTTOM;
    }

    int output_lines = 0;
    if (retain_mode && output_len > 0) {
        output_lines = wrapped_line_count_for_text(font, output, output_len, text_width);
    }

    int needed_height = input_area_height + suggestions_height;

    if (output_lines > 0) {
        needed_height += OUTPUT_PADDING_TOP + (output_lines * line_height) + OUTPUT_PADDING_BOTTOM;
    }
    if (needed_height != *window_height) {
        *window_height = needed_height;
        XResizeWindow(dpy, win, bar_width, needed_height);
    }

    XClearWindow(dpy, win);
    XSetForeground(dpy, gc, black);

    int input_y = text_padding_y + font_ascent;
    size_t input_start = 0;
    while (input_start <= (size_t)cmd_len) {
        size_t input_end = input_start;
        while (input_end < (size_t)cmd_len && cmd_buf[input_end] != '\n') {
            input_end++;
        }
        draw_wrapped_segment(
            dpy, win, gc, font, text_x, &input_y, line_height, text_width,
            cmd_buf + input_start, (int)(input_end - input_start)
        );
        if (input_end >= (size_t)cmd_len) {
            break;
        }
        input_start = input_end + 1;
    }

    if (cursor_visible) {
        int cursor_total_lines = 0;
        int cursor_last_width = 0;
        size_t cursor_start = 0;

        while (cursor_start <= (size_t)cmd_len) {
            size_t cursor_end = cursor_start;
            while (cursor_end < (size_t)cmd_len && cmd_buf[cursor_end] != '\n') {
                cursor_end++;
            }

            int seg_lines = 0;
            int seg_last_width = 0;
            measure_wrapped_segment_end(
                font,
                cmd_buf + cursor_start,
                (int)(cursor_end - cursor_start),
                text_width,
                &seg_lines,
                &seg_last_width
            );

            cursor_total_lines += seg_lines;
            cursor_last_width = seg_last_width;

            if (cursor_end >= (size_t)cmd_len) {
                break;
            }
            cursor_start = cursor_end + 1;
        }

        if (cursor_total_lines < 1) {
            cursor_total_lines = 1;
        }
        int cursor_top = text_padding_y + ((cursor_total_lines - 1) * line_height);
        int cursor_x = text_x + cursor_last_width;
        XDrawLine(dpy, win, gc, cursor_x, cursor_top, cursor_x, cursor_top + line_height - 2);
    }

    if (suggestion_lines > 0) {
        int sy = input_area_height + SUGGESTIONS_PADDING_TOP + font_ascent;
        for (int i = 0; i < suggestion_count; i++) {
            draw_wrapped_segment(
                dpy, win, gc, font, text_x, &sy, line_height, text_width,
                suggestions[i], (int)strlen(suggestions[i])
            );
        }
    }

    if (retain_mode && output_len > 0) {
        int y = input_area_height + suggestions_height + OUTPUT_PADDING_TOP + font_ascent;
        size_t end = output_len;

        while (end > 0) {
            size_t line_end = end;
            if (output[line_end - 1] == '\n') {
                line_end--;
            }

            size_t start = line_end;
            while (start > 0 && output[start - 1] != '\n') {
                start--;
            }

            draw_wrapped_segment(
                dpy, win, gc, font, text_x, &y, line_height, text_width,
                output + start, (int)(line_end - start)
            );

            if (start == 0) {
                break;
            }
            end = start - 1;
        }
    }

    XFlush(dpy);
}

int main(int argc, char **argv) {
    int retain_mode = 0;
    int dark_mode = 0;
    int bar_width = DEFAULT_BAR_WIDTH;
    int input_height = DEFAULT_INPUT_HEIGHT;
    int forced_font_px = 0;
    for (int i = 1; i < argc; i++) {
        if (strcmp(argv[i], "-r") == 0) {
            retain_mode = 1;
        } else if (strcmp(argv[i], "-d") == 0) {
            dark_mode = 1;
        } else if (strcmp(argv[i], "-w") == 0) {
            if (i + 1 >= argc || parse_positive_int(argv[i + 1], &bar_width) != 0) {
                fprintf(stderr, "Usage: %s [-r] [-d] [-w width] [-h height] [-f size]\n", argv[0]);
                return 1;
            }
            i++;
        } else if (strcmp(argv[i], "-h") == 0) {
            if (i + 1 >= argc || parse_positive_int(argv[i + 1], &input_height) != 0) {
                fprintf(stderr, "Usage: %s [-r] [-d] [-w width] [-h height] [-f size]\n", argv[0]);
                return 1;
            }
            i++;
        } else if (strcmp(argv[i], "-f") == 0) {
            if (i + 1 >= argc || parse_positive_int(argv[i + 1], &forced_font_px) != 0) {
                fprintf(stderr, "Usage: %s [-r] [-d] [-w width] [-h height] [-f size]\n", argv[0]);
                return 1;
            }
            i++;
        } else {
            fprintf(stderr, "Usage: %s [-r] [-d] [-w width] [-h height] [-f size]\n", argv[0]);
            return 1;
        }
    }
    if (bar_width < MIN_BAR_WIDTH) {
        bar_width = MIN_BAR_WIDTH;
    }

    Display *dpy = XOpenDisplay(NULL);
    if (!dpy) {
        fprintf(stderr, "Cannot open display\n");
        return 1;
    }

    Window root = DefaultRootWindow(dpy);
    int screen = DefaultScreen(dpy);
    Window previous_focus = None;
    int previous_revert_to = RevertToPointerRoot;
    int text_x = TEXT_PADDING_X;
    int font_ascent = 12;
    int font_descent = 4;
    int line_height = font_ascent + font_descent + 2;

    int screen_width = DisplayWidth(dpy, screen);
    int x = (screen_width - bar_width) / 2;
    int y = 100; 

    unsigned long black = BlackPixel(dpy, screen);
    unsigned long white = WhitePixel(dpy, screen);
    unsigned long text_color = black;
    unsigned long bg_color = white;
    unsigned long border_color = black;

    if (dark_mode) {
        XColor dark_gray, exact;
        Colormap colormap = DefaultColormap(dpy, screen);
        text_color = white;
        border_color = white;
        if (XAllocNamedColor(dpy, colormap, "#404040", &dark_gray, &exact)) {
            bg_color = dark_gray.pixel;
        } else {
            bg_color = black;
        }
    }

    Window win = XCreateSimpleWindow(dpy, root, x, y, bar_width, input_height, 1, border_color, bg_color);

    XSetWindowAttributes attrs;
    attrs.override_redirect = True;
    XChangeWindowAttributes(dpy, win, CWOverrideRedirect, &attrs);

    XSelectInput(dpy, win, ExposureMask | KeyPressMask);
    GC gc = XCreateGC(dpy, win, 0, NULL);
    XFontStruct *loaded_font = load_scaled_font(dpy, gc, input_height, forced_font_px);
    XFontStruct *font = XQueryFont(dpy, XGContextFromGC(gc));
    if (font) {
        font_ascent = font->ascent;
        font_descent = font->descent;
        line_height = font_ascent + font_descent + 2;
    }

    int min_input_height = font_ascent + font_descent + (2 * TEXT_PADDING_Y);
    if (input_height < min_input_height) {
        input_height = min_input_height;
        XResizeWindow(dpy, win, bar_width, input_height);
    }
    XGetInputFocus(dpy, &previous_focus, &previous_revert_to);
    XMapRaised(dpy, win);
    XSetInputFocus(dpy, win, RevertToParent, CurrentTime);
    XFlush(dpy);

    int grab_attempts = 0;
    while (XGrabKeyboard(dpy, win, False, GrabModeAsync, GrabModeAsync, CurrentTime) != GrabSuccess) {
        usleep(1000); 
        if (++grab_attempts > 500) { 
            fprintf(stderr, "Failed to grab keyboard input.\n");
            if (previous_focus != None && previous_focus != PointerRoot && previous_focus != win) {
                XSetInputFocus(dpy, previous_focus, previous_revert_to, CurrentTime);
                XFlush(dpy);
            }
            XDestroyWindow(dpy, win);
            XCloseDisplay(dpy);
            return 1;
        }
    }

    XEvent ev;
    char cmd_buf[MAX_CMD_LEN] = {0};
    int cmd_len = 0;
    int running = 1;
    int window_height = input_height;
    int output_fd = -1;
    pid_t child_pid = -1;
    char *output = NULL;
    size_t output_len = 0;
    size_t output_cap = 0;
    char **path_commands = NULL;
    size_t path_command_count = 0;
    char tab_cycle_query[MAX_CMD_LEN] = {0};
    int tab_cycle_index = -1;
    int tab_cycle_active = 0;
    int tab_cycle_token_index = -1;
    int cursor_visible = 1;
    long long cursor_last_toggle = now_ms();
    int x_fd = ConnectionNumber(dpy);
    load_path_commands(&path_commands, &path_command_count);

    while (running) {
        fd_set read_fds;
        FD_ZERO(&read_fds);
        FD_SET(x_fd, &read_fds);
        int max_fd = x_fd;

        if (retain_mode && output_fd != -1) {
            FD_SET(output_fd, &read_fds);
            if (output_fd > max_fd) {
                max_fd = output_fd;
            }
        }

        struct timeval timeout;
        long long elapsed = now_ms() - cursor_last_toggle;
        long long wait_ms = CURSOR_BLINK_MS - elapsed;
        if (wait_ms < 0) {
            wait_ms = 0;
        }
        timeout.tv_sec = wait_ms / 1000;
        timeout.tv_usec = (wait_ms % 1000) * 1000;

        int ready = select(max_fd + 1, &read_fds, NULL, NULL, &timeout);
        if (ready < 0) {
            if (errno == EINTR) {
                continue;
            }
            break;
        }
        if (ready == 0) {
            cursor_visible = !cursor_visible;
            cursor_last_toggle = now_ms();
            redraw(
                dpy, win, gc, text_color, bar_width, input_height, text_x, TEXT_PADDING_Y, font, font_ascent, line_height,
                cmd_buf, cmd_len, cursor_visible, path_commands, path_command_count, tab_cycle_active, tab_cycle_query,
                retain_mode, output, output_len, &window_height
            );
            continue;
        }

        if (retain_mode && output_fd != -1 && FD_ISSET(output_fd, &read_fds)) {
            char chunk[512];
            ssize_t nread = 0;

            while ((nread = read(output_fd, chunk, sizeof(chunk))) > 0) {
                append_output(&output, &output_len, &output_cap, chunk, (size_t)nread);
            }
            if (nread == 0 || (nread < 0 && errno != EAGAIN && errno != EWOULDBLOCK)) {
                close(output_fd);
                output_fd = -1;
                if (child_pid > 0) {
                    waitpid(child_pid, NULL, 0);
                    child_pid = -1;
                }
            }
            redraw(
                dpy, win, gc, text_color, bar_width, input_height, text_x, TEXT_PADDING_Y, font, font_ascent, line_height,
                cmd_buf, cmd_len, cursor_visible, path_commands, path_command_count, tab_cycle_active, tab_cycle_query,
                retain_mode, output, output_len, &window_height
            );
        }

        if (!FD_ISSET(x_fd, &read_fds)) {
            continue;
        }

        while (XPending(dpy) > 0) {
            XNextEvent(dpy, &ev);

            if (ev.type == Expose) {
                redraw(
                    dpy, win, gc, text_color, bar_width, input_height, text_x, TEXT_PADDING_Y, font, font_ascent, line_height,
                    cmd_buf, cmd_len, cursor_visible, path_commands, path_command_count, tab_cycle_active, tab_cycle_query,
                    retain_mode, output, output_len, &window_height
                );
            } else if (ev.type == KeyPress) {
                KeySym ks;
                char buf[32] = {0};
                int len = XLookupString(&ev.xkey, buf, sizeof(buf) - 1, &ks, NULL);
                cursor_visible = 1;
                cursor_last_toggle = now_ms();

                if (ks == XK_Escape) {
                    if (retain_mode) {
                        stop_retained_process(&child_pid);
                    }
                    tab_cycle_query[0] = '\0';
                    tab_cycle_index = -1;
                    tab_cycle_active = 0;
                    tab_cycle_token_index = -1;
                    running = 0;
                } else if (ks == XK_Return) {
                    if (cmd_len > 0) {
                        if (retain_mode) {
                            if (start_captured_command(cmd_buf, &child_pid, &output_fd) != 0) {
                                const char *msg = "[failed to start command]\n";
                                append_output(&output, &output_len, &output_cap, msg, strlen(msg));
                            }
                            cmd_buf[0] = '\0';
                            cmd_len = 0;
                            tab_cycle_query[0] = '\0';
                            tab_cycle_index = -1;
                            tab_cycle_active = 0;
                            tab_cycle_token_index = -1;
                            redraw(
                                dpy, win, gc, text_color, bar_width, input_height, text_x, TEXT_PADDING_Y, font, font_ascent, line_height,
                                cmd_buf, cmd_len, cursor_visible, path_commands, path_command_count, tab_cycle_active, tab_cycle_query,
                                retain_mode, output, output_len, &window_height
                            );
                        } else {
                            run_command(cmd_buf);
                            running = 0;
                        }
                    } else if (!retain_mode) {
                        running = 0;
                    }
                } else if (ks == XK_Tab || ks == XK_ISO_Left_Tab) {
                    const char *suggestions[MAX_SUGGESTIONS] = {0};
                    int direction = ((ks == XK_ISO_Left_Tab) || (ev.xkey.state & ShiftMask)) ? -1 : 1;
                    int token_start = 0;
                    int token_len = 0;

                    if (!tab_cycle_active) {
                        strncpy(tab_cycle_query, cmd_buf, sizeof(tab_cycle_query) - 1);
                        tab_cycle_query[sizeof(tab_cycle_query) - 1] = '\0';
                        tab_cycle_index = -1;
                        tab_cycle_active = 1;
                        build_suggestions_for_input(
                            path_commands,
                            path_command_count,
                            tab_cycle_query,
                            (int)strlen(tab_cycle_query),
                            suggestions,
                            MAX_SUGGESTIONS,
                            &token_start,
                            &token_len,
                            &tab_cycle_token_index
                        );
                    }

                    if (tab_cycle_token_index < 0) {
                        tab_cycle_query[0] = '\0';
                        tab_cycle_index = -1;
                        tab_cycle_active = 0;
                        continue;
                    }

                    int suggestion_count = build_suggestions_for_input(
                        path_commands,
                        path_command_count,
                        tab_cycle_query,
                        (int)strlen(tab_cycle_query),
                        suggestions,
                        MAX_SUGGESTIONS,
                        NULL,
                        NULL,
                        NULL
                    );

                    if (suggestion_count > 0) {
                        if (tab_cycle_index < 0) {
                            tab_cycle_index = (direction > 0) ? 0 : (suggestion_count - 1);
                        } else {
                            tab_cycle_index = (tab_cycle_index + direction + suggestion_count) % suggestion_count;
                        }

                        TokenRange tokens[MAX_TOKENS] = {0};
                        int token_count = parse_tokens(cmd_buf, cmd_len, tokens, MAX_TOKENS);
                        if (tab_cycle_token_index >= token_count) {
                            token_start = cmd_len;
                            token_len = 0;
                        } else {
                            token_start = tokens[tab_cycle_token_index].start;
                            token_len = tokens[tab_cycle_token_index].len;
                        }

                        if (apply_suggestion_to_input(cmd_buf, &cmd_len, token_start, token_len, suggestions[tab_cycle_index]) == 0) {
                            redraw(
                                dpy, win, gc, text_color, bar_width, input_height, text_x, TEXT_PADDING_Y, font, font_ascent, line_height,
                                cmd_buf, cmd_len, cursor_visible, path_commands, path_command_count, tab_cycle_active, tab_cycle_query,
                                retain_mode, output, output_len, &window_height
                            );
                        }
                    } else {
                        tab_cycle_query[0] = '\0';
                        tab_cycle_index = -1;
                        tab_cycle_active = 0;
                        tab_cycle_token_index = -1;
                    }
                } else if (ks == XK_BackSpace) {
                    if (cmd_len > 0) {
                        cmd_buf[--cmd_len] = '\0';
                    }
                    tab_cycle_query[0] = '\0';
                    tab_cycle_index = -1;
                    tab_cycle_active = 0;
                    tab_cycle_token_index = -1;
                    redraw(
                        dpy, win, gc, text_color, bar_width, input_height, text_x, TEXT_PADDING_Y, font, font_ascent, line_height,
                        cmd_buf, cmd_len, cursor_visible, path_commands, path_command_count, tab_cycle_active, tab_cycle_query,
                        retain_mode, output, output_len, &window_height
                    );
                } else if (len > 0 && cmd_len + len < MAX_CMD_LEN - 1) {
                    memcpy(cmd_buf + cmd_len, buf, (size_t)len);
                    cmd_len += len;
                    cmd_buf[cmd_len] = '\0';
                    tab_cycle_query[0] = '\0';
                    tab_cycle_index = -1;
                    tab_cycle_active = 0;
                    tab_cycle_token_index = -1;
                    redraw(
                        dpy, win, gc, text_color, bar_width, input_height, text_x, TEXT_PADDING_Y, font, font_ascent, line_height,
                        cmd_buf, cmd_len, cursor_visible, path_commands, path_command_count, tab_cycle_active, tab_cycle_query,
                        retain_mode, output, output_len, &window_height
                    );
                }
            }
        }
    }

    if (output_fd != -1) {
        close(output_fd);
    }
    XUngrabKeyboard(dpy, CurrentTime);
    if (previous_focus != None && previous_focus != PointerRoot && previous_focus != win) {
        XSetInputFocus(dpy, previous_focus, previous_revert_to, CurrentTime);
    }
    XFlush(dpy);
    XFreeGC(dpy, gc);
    XDestroyWindow(dpy, win);
    if (font) {
        XFreeFontInfo(NULL, font, 1);
    }
    if (loaded_font && loaded_font != font) {
        XFreeFont(dpy, loaded_font);
    }
    XCloseDisplay(dpy);
    free_path_commands(path_commands, path_command_count);
    free_help_cache();
    free(output);
    return 0;
}
