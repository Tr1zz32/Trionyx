#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <ctype.h>
#include <unistd.h>
#include <stdbool.h>
#include <errno.h>
#include <sys/wait.h>
#include <sys/stat.h>
#include <sys/types.h>
#include <sys/mount.h>
#include <pwd.h>
#include <grp.h>
#include <limits.h>
#include <libgen.h>

#define ENABLED_LIST_PATH "/services/bootstrap/enabled.list"
#define SYSTEM_PROP_PATH  "/services/system.prop"
#define MAX_LINE_LEN      2048
#define MAX_TOKENS        128
#define MAX_VAR_VAL       512
#define MAX_IF_DEPTH      32

typedef enum {
    TOKEN_CMD_SEGMENT,
    TOKEN_OP_SEQ,       // & o :then:
    TOKEN_OP_OR,        // || o :if_fails:
    TOKEN_OP_FAIL_FAIL, // !! o :abort_on_fail:
    TOKEN_OP_PROP_COND, // && o :propis:
    TOKEN_OP_PATH_COND  // $$ o :pathis:
} TokenType;

typedef struct {
    TokenType type;
    char content[MAX_LINE_LEN];
} Token;

typedef enum {
    IF_NOT_BLOCK,
    IF_HANDLED,
    IF_ERROR
} IfResult;

typedef struct {
    int if_depth;        // Profundidad de bloques 'if'
    int skip_at_depth;   // Control de omisión de ejecución
} ParserState;

// Declaración previa necesaria para evaluación recursiva de nivel 1
static int execute_base_command(char *cmd_str, bool *was_execn, char *out_buf, size_t out_size);

static void print_error(const char *msg, const char *extra) {
    pid_t pid = getpid();
    if (extra != NULL) {
        fprintf(stderr, "[%d]: action: %s %s\n", pid, msg, extra);
    } else {
        fprintf(stderr, "[%d]: action: %s\n", pid, msg);
    }
}

static void print_sys_error(const char *msg, const char *extra) {
    pid_t pid = getpid();
    if (extra != NULL) {
        fprintf(stderr, "[%d]: action: %s %s: %s\n", pid, msg, extra, strerror(errno));
    } else {
        fprintf(stderr, "[%d]: action: %s: %s\n", pid, msg, strerror(errno));
    }
}

static char *trim(char *str) {
    while (isspace((unsigned char)*str)) str++;
    if (*str == 0) return str;

    char *end = str + strlen(str) - 1;
    while (end > str && isspace((unsigned char)*end)) end--;
    end[1] = '\0';

    return str;
}

static void unquote(char *str) {
    char *s = trim(str);
    size_t len = strlen(s);
    if (len >= 2 && s[0] == '"' && s[len - 1] == '"') {
        s[len - 1] = '\0';
        memmove(str, s + 1, len - 1);
    }
}

static void unescape_string(char *str) {
    char *src = str;
    char *dst = str;

    while (*src) {
        if (*src == '\\' && *(src + 1)) {
            src++;
            switch (*src) {
                case 'n':  *dst++ = '\n'; break;
                case 't':  *dst++ = '\t'; break;
                case 'r':  *dst++ = '\r'; break;
                case '\\': *dst++ = '\\'; break;
                case '"':  *dst++ = '"';  break;
                case '0':  *dst++ = '\0'; break;
                default:   *dst++ = *src; break;
            }
        } else {
            *dst++ = *src;
        }
        src++;
    }
    *dst = '\0';
}

static void strip_comment(char *line) {
    bool in_quotes = false;
    for (char *p = line; *p != '\0'; p++) {
        if (*p == '"') {
            in_quotes = !in_quotes;
        } else if (*p == '#' && !in_quotes) {
            *p = '\0';
            break;
        }
    }
}

static bool verify_parent_process(void) {
    pid_t ppid = getppid();
    char exe_link[256];
    char resolved_path[PATH_MAX];

    snprintf(exe_link, sizeof(exe_link), "/proc/%d/exe", ppid);
    ssize_t len = readlink(exe_link, resolved_path, sizeof(resolved_path) - 1);
    if (len <= 0) return false;

    resolved_path[len] = '\0';
    char *parent_name = basename(resolved_path);

    return (strcmp(parent_name, "serviced") == 0 ||
            strcmp(parent_name, "initd") == 0 ||
            strcmp(parent_name, "qemu-x86_64") == 0 ||
            strcmp(parent_name, "bootstrap-bash") == 0 ||
            strcmp(parent_name, "action") == 0);
}

static bool is_action_enabled(const char *target_path) {
    FILE *list_file = fopen(ENABLED_LIST_PATH, "r");
    if (!list_file) {
        print_sys_error("Failed to open enabled list at", ENABLED_LIST_PATH);
        return false;
    }

    char line[MAX_LINE_LEN];
    bool is_enabled = false;

    while (fgets(line, sizeof(line), list_file)) {
        strip_comment(line);
        char *registered_path = trim(line);
        if (strlen(registered_path) > 0 && strcmp(registered_path, target_path) == 0) {
            is_enabled = true;
            break;
        }
    }

    fclose(list_file);
    return is_enabled;
}

static bool get_system_prop(const char *key, char *out_val, size_t max_len) {
    FILE *f = fopen(SYSTEM_PROP_PATH, "r");
    if (!f) return false;

    char line[MAX_LINE_LEN];
    bool found = false;

    while (fgets(line, sizeof(line), f)) {
        strip_comment(line);
        char *trimmed = trim(line);
        if (trimmed[0] == '\0') continue;

        char *eq = strchr(trimmed, '=');
        if (eq) {
            *eq = '\0';
            char *k = trim(trimmed);
            char *v = trim(eq + 1);

            if (strcmp(k, key) == 0) {
                strncpy(out_val, v, max_len - 1);
                out_val[max_len - 1] = '\0';
                found = true;
                break;
            }
        }
    }

    fclose(f);
    return found;
}

static bool update_system_prop(const char *key, const char *val, bool is_set) {
    FILE *f = fopen(SYSTEM_PROP_PATH, "r");
    char temp_path[PATH_MAX];

    snprintf(temp_path, sizeof(temp_path), "%s.XXXXXX", SYSTEM_PROP_PATH);
    int fd = mkstemp(temp_path);
    if (fd == -1) {
        if (f) fclose(f);
        print_sys_error("Failed to modify system properties", SYSTEM_PROP_PATH);
        return false;
    }
    fchmod(fd, 0644);
    FILE *tf = fdopen(fd, "w");
    if (!tf) {
        close(fd);
        if (f) fclose(f);
        return false;
    }

    bool updated = false;
    if (f) {
        char line[MAX_LINE_LEN];
        while (fgets(line, sizeof(line), f)) {
            char line_copy[MAX_LINE_LEN];
            strncpy(line_copy, line, sizeof(line_copy) - 1);
            line_copy[sizeof(line_copy) - 1] = '\0';

            strip_comment(line_copy);
            char *trimmed = trim(line_copy);

            if (trimmed[0] != '\0') {
                char *eq = strchr(trimmed, '=');
                if (eq) {
                    *eq = '\0';
                    char *k = trim(trimmed);
                    if (strcmp(k, key) == 0) {
                        if (is_set) {
                            fprintf(tf, "%s = %s\n", key, val);
                            updated = true;
                        }
                        continue;
                    }
                }
            }
            fputs(line, tf);
        }
        fclose(f);
    }

    if (is_set && !updated) {
        fprintf(tf, "%s = %s\n", key, val);
    }

    fclose(tf);
    if (rename(temp_path, SYSTEM_PROP_PATH) != 0) {
        unlink(temp_path);
        return false;
    }
    return true;
}

static bool parse_tokens_quoted(const char *input, char args[MAX_TOKENS][MAX_LINE_LEN], int *arg_count) {
    *arg_count = 0;
    bool in_quotes = false;
    char current[MAX_LINE_LEN];
    int c_idx = 0;

    for (int i = 0; input[i] != '\0'; i++) {
        char ch = input[i];
        if (ch == '"') {
            in_quotes = !in_quotes;
        } else if (isspace((unsigned char)ch) && !in_quotes) {
            if (c_idx > 0) {
                if (*arg_count >= MAX_TOKENS) return false;
                current[c_idx] = '\0';
                unquote(current);
                strncpy(args[*arg_count], current, MAX_LINE_LEN - 1);
                args[*arg_count][MAX_LINE_LEN - 1] = '\0';
                (*arg_count)++;
                c_idx = 0;
            }
        } else {
            if (c_idx >= MAX_LINE_LEN - 1) {
                print_error("Syntax error: argument exceeds maximum length", NULL);
                return false;
            }
            current[c_idx++] = ch;
        }
    }

    if (c_idx > 0) {
        if (*arg_count >= MAX_TOKENS) return false;
        current[c_idx] = '\0';
        unquote(current);
        strncpy(args[*arg_count], current, MAX_LINE_LEN - 1);
        args[*arg_count][MAX_LINE_LEN - 1] = '\0';
        (*arg_count)++;
    }

    return !in_quotes;
}

static bool expand_nested_commands(const char *input, char *output, size_t out_max) {
    output[0] = '\0';
    size_t out_len = 0;

    const char *p = input;
    while (*p != '\0') {
        if (strncmp(p, "*(", 2) == 0) {
            const char *end = strstr(p + 2, ")*");
            if (!end) {
                print_error("Syntax error: unmatched nested command construct '*(...)'", NULL);
                return false;
            }

            char sub_cmd[MAX_LINE_LEN];
            size_t sub_len = end - (p + 2);
            if (sub_len >= sizeof(sub_cmd)) {
                print_error("Syntax error: nested command string exceeds maximum length", NULL);
                return false;
            }

            strncpy(sub_cmd, p + 2, sub_len);
            sub_cmd[sub_len] = '\0';

            if (strstr(sub_cmd, "*(")) {
                print_error("Syntax error: deeper nesting inside '*(...)*' is prohibited", NULL);
                return false;
            }

            bool dummy_execn = false;
            char sub_out[MAX_LINE_LEN] = {0};
            int res = execute_base_command(sub_cmd, &dummy_execn, sub_out, sizeof(sub_out));

            if (res != 0) {
                print_error("Execution error: nested command failed:", sub_cmd);
                return false;
            }

            // Limpia saltos de línea al final del resultado del subcomando
            size_t trim_len = strlen(sub_out);
            while (trim_len > 0 && (sub_out[trim_len - 1] == '\n' || sub_out[trim_len - 1] == '\r')) {
                sub_out[--trim_len] = '\0';
            }

            if (out_len + strlen(sub_out) >= out_max) {
                print_error("Syntax error: expanded line exceeds maximum length", NULL);
                return false;
            }

            strncat(output, sub_out, out_max - out_len - 1);
            out_len += strlen(sub_out);

            p = end + 2;
        } else {
            if (out_len + 1 >= out_max) {
                print_error("Syntax error: expanded line exceeds maximum length", NULL);
                return false;
            }
            output[out_len++] = *p;
            output[out_len] = '\0';
            p++;
        }
    }

    return true;
}

static unsigned long parse_mount_flags(const char *opts, char *data_out, size_t data_max) {
    unsigned long flags = 0;
    if (data_out && data_max > 0) data_out[0] = '\0';
    if (!opts || strlen(opts) == 0) return 0;

    char opts_copy[MAX_LINE_LEN];
    strncpy(opts_copy, opts, sizeof(opts_copy) - 1);
    opts_copy[sizeof(opts_copy) - 1] = '\0';

    char *token = strtok(opts_copy, ",");
    while (token != NULL) {
        char *opt = trim(token);
        if (strcmp(opt, "ro") == 0) flags |= MS_RDONLY;
        else if (strcmp(opt, "rw") == 0) flags &= ~MS_RDONLY;
        else if (strcmp(opt, "nosuid") == 0) flags |= MS_NOSUID;
        else if (strcmp(opt, "nodev") == 0) flags |= MS_NODEV;
        else if (strcmp(opt, "noexec") == 0) flags |= MS_NOEXEC;
        else if (strcmp(opt, "sync") == 0) flags |= MS_SYNCHRONOUS;
        else if (strcmp(opt, "remount") == 0) flags |= MS_REMOUNT;
        else if (strcmp(opt, "mand") == 0) flags |= MS_MANDLOCK;
        else if (strcmp(opt, "dirsync") == 0) flags |= MS_DIRSYNC;
        else if (strcmp(opt, "noatime") == 0) flags |= MS_NOATIME;
        else if (strcmp(opt, "nodiratime") == 0) flags |= MS_NODIRATIME;
        else if (strcmp(opt, "bind") == 0) flags |= MS_BIND;
        else if (strcmp(opt, "rec") == 0 || strcmp(opt, "recursive") == 0) flags |= MS_REC;
        else if (strcmp(opt, "defaults") != 0 && data_out) {
            if (strlen(data_out) > 0) strncat(data_out, ",", data_max - strlen(data_out) - 1);
            strncat(data_out, opt, data_max - strlen(data_out) - 1);
        }
        token = strtok(NULL, ",");
    }

    return flags;
}

static int cmd_mountfs(int arg_count, char args[MAX_TOKENS][MAX_LINE_LEN]) {
    if (arg_count < 4) {
        print_error("Syntax error: mountfs requires \"<fstype>\" \"<source>\" \"<target>\" [\"<options>\"]", NULL);
        return 1;
    }

    const char *fstype = (strcmp(args[1], "none") == 0 || strlen(args[1]) == 0) ? NULL : args[1];
    const char *source = args[2];
    const char *target = args[3];

    char data_opts[MAX_LINE_LEN] = {0};
    unsigned long flags = 0;

    if (arg_count >= 5) {
        flags = parse_mount_flags(args[4], data_opts, sizeof(data_opts));
    }

    if (mount(source, target, fstype, flags, data_opts[0] != '\0' ? data_opts : NULL) != 0) {
        print_sys_error("Failed to mountfs target", target);
        return 1;
    }

    return 0;
}

static int cmd_mount(int arg_count, char args[MAX_TOKENS][MAX_LINE_LEN]) {
    if (arg_count < 3) {
        print_error("Syntax error: mount requires \"<source>\" \"<target>\" [\"<fstype>\"] [\"<options>\"]", NULL);
        return 1;
    }

    const char *source = args[1];
    const char *target = args[2];

    const char *fstype = NULL;
    if (arg_count >= 4 && strlen(args[3]) > 0 && strcmp(args[3], "none") != 0) {
        fstype = args[3];
    }

    char data_opts[MAX_LINE_LEN] = {0};
    unsigned long flags = 0;

    if (arg_count >= 5) {
        flags = parse_mount_flags(args[4], data_opts, sizeof(data_opts));
    }

    if (mount(source, target, fstype, flags, data_opts[0] != '\0' ? data_opts : NULL) != 0) {
        print_sys_error("Failed to mount target", target);
        return 1;
    }

    return 0;
}

static int cmd_umount(int arg_count, char args[MAX_TOKENS][MAX_LINE_LEN]) {
    if (arg_count < 2) {
        print_error("Syntax error: umount requires \"<target>\" [\"force\"]", NULL);
        return 1;
    }

    const char *target = args[1];
    int flags = 0;

    if (arg_count >= 3 && strcmp(args[2], "force") == 0) {
        flags |= MNT_FORCE;
    }

    if (umount2(target, flags) != 0) {
        print_sys_error("Failed to umount target", target);
        return 1;
    }

    return 0;
}

static int cmd_symlink(int arg_count, char args[MAX_TOKENS][MAX_LINE_LEN]) {
    if (arg_count < 3) {
        print_error("Syntax error: symlink requires \"<target>\" \"<linkpath>\"", NULL);
        return 1;
    }

    if (symlink(args[1], args[2]) != 0) {
        print_sys_error("Failed to create symlink", args[2]);
        return 1;
    }

    return 0;
}

static int cmd_mkdir(int arg_count, char args[MAX_TOKENS][MAX_LINE_LEN]) {
    if (arg_count < 2) return 1;
    mode_t mode = 0755;
    if (arg_count >= 3) mode = strtol(args[2], NULL, 8);

    if (mkdir(args[1], mode) != 0) {
        if (errno != EEXIST) {
            print_sys_error("Failed to create directory", args[1]);
            return 1;
        }

        struct stat st;
        if (stat(args[1], &st) != 0 || !S_ISDIR(st.st_mode)) {
            print_sys_error("Path exists but is not a directory", args[1]);
            return 1;
        }
    }

    if (arg_count >= 4) {
        struct passwd *pwd = getpwnam(args[3]);
        struct group *grp = (arg_count >= 5) ? getgrnam(args[4]) : NULL;

        if (pwd == NULL) {
            print_error("Unknown user:", args[3]);
            return 1;
        }

        if (arg_count >= 5 && grp == NULL) {
            print_error("Unknown group:", args[4]);
            return 1;
        }

        uid_t uid = pwd->pw_uid;
        gid_t gid = grp ? grp->gr_gid : (gid_t)-1;

        if (chown(args[1], uid, gid) != 0) {
            print_sys_error("Failed to set ownership on directory", args[1]);
            return 1;
        }
    }
    return 0;
}

static int cmd_chmod(int arg_count, char args[MAX_TOKENS][MAX_LINE_LEN]) {
    if (arg_count < 3) return 1;
    mode_t mode = strtol(args[1], NULL, 8);
    if (chmod(args[2], mode) != 0) {
        print_sys_error("Failed to chmod", args[2]);
        return 1;
    }
    return 0;
}

static int cmd_chown(int arg_count, char args[MAX_TOKENS][MAX_LINE_LEN]) {
    if (arg_count < 4) return 1;
    struct passwd *pwd = getpwnam(args[1]);
    struct group *grp = getgrnam(args[2]);

    uid_t uid = pwd ? pwd->pw_uid : (uid_t)-1;
    gid_t gid = grp ? grp->gr_gid : (gid_t)-1;

    if (uid == (uid_t)-1 && isdigit((unsigned char)args[1][0])) uid = atoi(args[1]);
    if (gid == (gid_t)-1 && isdigit((unsigned char)args[2][0])) gid = atoi(args[2]);

    if (chown(args[3], uid, gid) != 0) {
        print_sys_error("Failed to chown", args[3]);
        return 1;
    }
    return 0;
}

static int cmd_write(int arg_count, char args[MAX_TOKENS][MAX_LINE_LEN]) {
    if (arg_count < 3) return 1;
    unescape_string(args[1]);
    FILE *f = fopen(args[2], "w");
    if (!f) {
        print_sys_error("Failed to open file for writing", args[2]);
        return 1;
    }
    fputs(args[1], f);
    fclose(f);
    return 0;
}

static int cmd_report(int arg_count, char args[MAX_TOKENS][MAX_LINE_LEN]) {
    if (arg_count < 3) return 1;
    unescape_string(args[1]);
    FILE *f = fopen(args[2], "a");
    if (!f) {
        print_sys_error("Failed to open file for appending", args[2]);
        return 1;
    }
    fputs(args[1], f);
    fclose(f);
    return 0;
}

static int cmd_text(int arg_count, char args[MAX_TOKENS][MAX_LINE_LEN], char *out_buf, size_t out_size) {
    if (arg_count < 2) return 1;
    unescape_string(args[1]);

    if (out_buf != NULL && out_size > 0) {
        strncpy(out_buf, args[1], out_size - 1);
        out_buf[out_size - 1] = '\0';
        return 0;
    }

    if (arg_count >= 3) {
        FILE *f = fopen(args[2], "w");
        if (!f) {
            print_sys_error("Failed to write text to file", args[2]);
            return 1;
        }
        fputs(args[1], f);
        fclose(f);
    } else {
        fputs(args[1], stdout);
        fflush(stdout);
    }
    return 0;
}

static int cmd_exec(int arg_count, char args[MAX_TOKENS][MAX_LINE_LEN], bool wait_for_exit, bool *was_execn, char *out_buf, size_t out_size) {
    if (arg_count < 2) return 1;
    if (!wait_for_exit) *was_execn = true;

    char *exec_args[MAX_TOKENS];
    for (int i = 1; i < arg_count; i++) {
        exec_args[i - 1] = args[i];
    }
    exec_args[arg_count - 1] = NULL;

    int pipefd[2];
    if (out_buf != NULL && pipe(pipefd) != 0) {
        print_sys_error("Failed to create pipe for subshell execution", NULL);
        return 1;
    }

    pid_t pid = fork();
    if (pid == 0) {
        if (out_buf != NULL) {
            close(pipefd[0]);
            dup2(pipefd[1], STDOUT_FILENO);
            close(pipefd[1]);
        }
        execvp(exec_args[0], exec_args);
        print_sys_error("Failed to execute binary", exec_args[0]);
        _exit(EXIT_FAILURE);
    } else if (pid > 0) {
        if (out_buf != NULL) {
            close(pipefd[1]);
            ssize_t bytes = read(pipefd[0], out_buf, out_size - 1);
            if (bytes >= 0) out_buf[bytes] = '\0';
            else out_buf[0] = '\0';
            close(pipefd[0]);
        }

        if (wait_for_exit) {
            int status;
            if (waitpid(pid, &status, 0) == -1) return 1;
            return WIFEXITED(status) ? WEXITSTATUS(status) : 1;
        } else {
            return 0;
        }
    }
    print_sys_error("Failed to fork process", NULL);
    return 1;
}

static int execute_base_command(char *cmd_str, bool *was_execn, char *out_buf, size_t out_size) {
    *was_execn = false;
    char args[MAX_TOKENS][MAX_LINE_LEN];
    int arg_count = 0;

    if (!parse_tokens_quoted(cmd_str, args, &arg_count)) {
        print_error("Syntax error: unmatched quote or invalid tokens in segment:", cmd_str);
        return -1;
    }
    if (arg_count == 0) return 0;

    if (strcmp(args[0], "exit") == 0) {
        exit((arg_count > 1) ? atoi(args[1]) : 0);
    }
    if (strcmp(args[0], "wait") == 0) {
        if (arg_count < 2) return 1;
        sleep(atoi(args[1]));
        return 0;
    }
    if (strcmp(args[0], "mountfs") == 0) return cmd_mountfs(arg_count, args);
    if (strcmp(args[0], "mount") == 0) return cmd_mount(arg_count, args);
    if (strcmp(args[0], "umount") == 0) return cmd_umount(arg_count, args);
    if (strcmp(args[0], "symlink") == 0) return cmd_symlink(arg_count, args);
    if (strcmp(args[0], "exec") == 0) return cmd_exec(arg_count, args, true, was_execn, out_buf, out_size);
    if (strcmp(args[0], "execn") == 0) return cmd_exec(arg_count, args, false, was_execn, NULL, 0);
    if (strcmp(args[0], "write") == 0) return cmd_write(arg_count, args);
    if (strcmp(args[0], "report") == 0) return cmd_report(arg_count, args);
    if (strcmp(args[0], "text") == 0) return cmd_text(arg_count, args, out_buf, out_size);
    if (strcmp(args[0], "mkdir") == 0) return cmd_mkdir(arg_count, args);
    if (strcmp(args[0], "chmod") == 0) return cmd_chmod(arg_count, args);
    if (strcmp(args[0], "chown") == 0) return cmd_chown(arg_count, args);
    if (strcmp(args[0], "set") == 0) {
        if (arg_count < 4 || strcmp(args[2], "==") != 0) return 1;
        return update_system_prop(args[1], args[3], true) ? 0 : 1;
    }
    if (strcmp(args[0], "unset") == 0 || strcmp(args[0], "rmprop") == 0) {
        if (arg_count < 2) return 1;
        return update_system_prop(args[1], NULL, false) ? 0 : 1;
    }

    print_error("Unknown command:", args[0]);
    return 1;
}

static int tokenize_line(const char *line, Token tokens[MAX_TOKENS]) {
    int token_count = 0;
    bool in_quotes = false;
    char buffer[MAX_LINE_LEN];
    int b_idx = 0;

    for (int i = 0; line[i] != '\0'; i++) {
        if (token_count >= MAX_TOKENS - 1) {
            print_error("Syntax error: maximum tokens limit reached", NULL);
            return -1;
        }

        char ch = line[i];

        if (ch == '"') {
            in_quotes = !in_quotes;
            if (b_idx >= MAX_LINE_LEN - 1) {
                print_error("Syntax error: maximum segment length reached", NULL);
                return -1;
            }
            buffer[b_idx++] = ch;
        } else if (!in_quotes) {
            TokenType type;
            int op_len = 0;

            if (strncmp(&line[i], "!!", 2) == 0) { type = TOKEN_OP_FAIL_FAIL; op_len = 2; }
            else if (strncmp(&line[i], ":abort_on_fail:", 15) == 0) { type = TOKEN_OP_FAIL_FAIL; op_len = 15; }
            else if (strncmp(&line[i], "||", 2) == 0) { type = TOKEN_OP_OR; op_len = 2; }
            else if (strncmp(&line[i], ":if_fails:", 10) == 0) { type = TOKEN_OP_OR; op_len = 10; }
            else if (strncmp(&line[i], "&&", 2) == 0) { type = TOKEN_OP_PROP_COND; op_len = 2; }
            else if (strncmp(&line[i], ":propis:", 8) == 0) { type = TOKEN_OP_PROP_COND; op_len = 8; }
            else if (strncmp(&line[i], "$$", 2) == 0) { type = TOKEN_OP_PATH_COND; op_len = 2; }
            else if (strncmp(&line[i], ":pathis:", 8) == 0) { type = TOKEN_OP_PATH_COND; op_len = 8; }
            else if (line[i] == '&') { type = TOKEN_OP_SEQ; op_len = 1; }
            else if (strncmp(&line[i], ":then:", 6) == 0) { type = TOKEN_OP_SEQ; op_len = 6; }

            if (op_len > 0) {
                if (b_idx > 0) {
                    buffer[b_idx] = '\0';
                    tokens[token_count].type = TOKEN_CMD_SEGMENT;
                    strncpy(tokens[token_count].content, trim(buffer), MAX_LINE_LEN - 1);
                    tokens[token_count].content[MAX_LINE_LEN - 1] = '\0';
                    token_count++;
                    b_idx = 0;
                }
                tokens[token_count++].type = type;
                i += (op_len - 1);
                continue;
            }

            if (b_idx >= MAX_LINE_LEN - 1) {
                print_error("Syntax error: maximum segment length reached", NULL);
                return -1;
            }
            buffer[b_idx++] = ch;
        } else {
            if (b_idx >= MAX_LINE_LEN - 1) {
                print_error("Syntax error: maximum segment length reached", NULL);
                return -1;
            }
            buffer[b_idx++] = ch;
        }
    }

    if (in_quotes) {
        print_error("Syntax error: unmatched quote at end of line", NULL);
        return -1;
    }

    if (b_idx > 0 && token_count < MAX_TOKENS) {
        buffer[b_idx] = '\0';
        tokens[token_count].type = TOKEN_CMD_SEGMENT;
        strncpy(tokens[token_count].content, trim(buffer), MAX_LINE_LEN - 1);
        tokens[token_count].content[MAX_LINE_LEN - 1] = '\0';
        token_count++;
    }

    return token_count;
}

static bool eval_condition(TokenType type, char *expr) {
    char clean[MAX_LINE_LEN];
    strncpy(clean, expr, sizeof(clean) - 1);
    clean[sizeof(clean) - 1] = '\0';

    char *cleaned = trim(clean);

    if (type == TOKEN_OP_PROP_COND) {
        char *eq = strstr(cleaned, "==");
        if (!eq) return false;
        *eq = '\0';

        char *k = trim(cleaned);
        char *expected_val = trim(eq + 2);
        unquote(expected_val);

        char actual_val[MAX_VAR_VAL] = {0};
        if (!get_system_prop(k, actual_val, sizeof(actual_val))) return false;
        return (strcmp(actual_val, expected_val) == 0);
    }
    else if (type == TOKEN_OP_PATH_COND) {
        unquote(cleaned);
        return (access(cleaned, F_OK) == 0);
    }
    return true;
}

static IfResult process_if_block(char *cmd, ParserState *state) {
    if (strcmp(cmd, ":fexit:") == 0 || strcmp(cmd, "fi") == 0 || strcmp(cmd, "endif") == 0) {
        if (state->if_depth <= 0) {
            print_error("Syntax error: unexpected closure (:fexit:) without matching 'if'", NULL);
            return IF_ERROR;
        }

        if (state->skip_at_depth > 0 && state->if_depth <= state->skip_at_depth) {
            state->skip_at_depth = 0;
        }

        state->if_depth--;
        return IF_HANDLED;
    }

    if (state->skip_at_depth > 0) {
        if (strncmp(cmd, "if ", 3) == 0) {
            if (state->if_depth >= MAX_IF_DEPTH) {
                print_error("Syntax error: maximum 'if' nesting depth exceeded", NULL);
                return IF_ERROR;
            }
            state->if_depth++;
        }
        return IF_HANDLED;
    }

    if (strncmp(cmd, "if ", 3) == 0) {
        if (state->if_depth >= MAX_IF_DEPTH) {
            print_error("Syntax error: maximum 'if' nesting depth exceeded", NULL);
            return IF_ERROR;
        }

        state->if_depth++;

        char *expr = trim(cmd + 3);
        TokenType cond_type;
        char *cond_body = NULL;

        if (strncmp(expr, "&&", 2) == 0) {
            cond_type = TOKEN_OP_PROP_COND;
            cond_body = expr + 2;
        } else if (strncmp(expr, ":propis:", 8) == 0) {
            cond_type = TOKEN_OP_PROP_COND;
            cond_body = expr + 8;
        } else if (strncmp(expr, "$$", 2) == 0) {
            cond_type = TOKEN_OP_PATH_COND;
            cond_body = expr + 2;
        } else if (strncmp(expr, ":pathis:", 8) == 0) {
            cond_type = TOKEN_OP_PATH_COND;
            cond_body = expr + 8;
        } else {
            print_error("Syntax error: 'if' block header only accepts '&&' (:propis:) or '$$' (:pathis:)", expr);
            return IF_ERROR;
        }

        bool result = eval_condition(cond_type, cond_body);
        if (!result) {
            state->skip_at_depth = state->if_depth;
        }
        return IF_HANDLED;
    }

    return IF_NOT_BLOCK;
}

static bool process_line_instruction(char *line_str, ParserState *state) {
    strip_comment(line_str);
    char *raw_cmd = trim(line_str);
    if (strlen(raw_cmd) == 0) return true;

    IfResult if_res = process_if_block(raw_cmd, state);
    if (if_res == IF_ERROR) return false;
    if (if_res == IF_HANDLED) return true;

    if (state->skip_at_depth > 0) return true;

    char cmd[MAX_LINE_LEN];
    if (!expand_nested_commands(raw_cmd, cmd, sizeof(cmd))) {
        return false;
    }

    Token tokens[MAX_TOKENS];
    int token_count = tokenize_line(cmd, tokens);

    if (token_count < 0) {
        return false;
    }

    int last_status = 0;
    bool execute_next = true;
    bool last_was_execn = false;
    bool guard_active = false;

    for (int i = 0; i < token_count; i++) {
        if (tokens[i].type == TOKEN_CMD_SEGMENT) {
            if (execute_next) {
                last_status = execute_base_command(tokens[i].content, &last_was_execn, NULL, 0);
                if (last_status < 0) return false;
            }
        }
        else if (tokens[i].type == TOKEN_OP_FAIL_FAIL) {
            if (last_was_execn) {
                print_error("Syntax error: Operator !! is incompatible with execn", NULL);
                return false;
            }
            if (last_status != 0) exit(last_status);
            execute_next = true;
            guard_active = false;
        }
        else if (tokens[i].type == TOKEN_OP_SEQ) {
            if (!guard_active) {
                execute_next = true;
            }
        }
        else if (tokens[i].type == TOKEN_OP_OR) {
            execute_next = (last_status != 0);
            guard_active = false;
        }
        else if (tokens[i].type == TOKEN_OP_PROP_COND || tokens[i].type == TOKEN_OP_PATH_COND) {
            if (i + 1 < token_count && tokens[i + 1].type == TOKEN_CMD_SEGMENT) {
                bool result = eval_condition(tokens[i].type, tokens[i + 1].content);
                last_status = result ? 0 : 1;
                execute_next = result;
                guard_active = !result;
                i++;
            }
        }
    }
    return true;
}

static void process_action_file(const char *filepath) {
    FILE *file = fopen(filepath, "r");
    if (!file) {
        print_sys_error("Failed to open action file", filepath);
        exit(EXIT_FAILURE);
    }

    ParserState state = {
        .if_depth = 0,
        .skip_at_depth = 0
    };

    char line[MAX_LINE_LEN];
    while (fgets(line, sizeof(line), file)) {
        if (!process_line_instruction(line, &state)) {
            fclose(file);
            exit(EXIT_FAILURE);
        }
    }

    if (state.if_depth > 0) {
        print_error("Syntax error: unclosed 'if' block at end of file (missing :fexit:)", filepath);
        fclose(file);
        exit(EXIT_FAILURE);
    }

    fclose(file);
}

int main(int argc, char *argv[]) {
    if (argc < 2) {
        print_error("Missing action path argument", NULL);
        return EXIT_FAILURE;
    }

    const char *target_action = argv[1];

    if (!is_action_enabled(target_action)) {
        print_error("Action is disabled or not found in enabled list:", target_action);
        return EXIT_FAILURE;
    }

    process_action_file(target_action);

    return EXIT_SUCCESS;
}
