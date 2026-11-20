#define _POSIX_C_SOURCE 200809L

#include <dirent.h>
#include <errno.h>
#include <fcntl.h>
#include <limits.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <sys/stat.h>
#include <sys/types.h>
#include <time.h>
#include <unistd.h>

static void usage(const char *name) {
    fprintf(stderr, "usage: %s [arguments]\n", name);
}

static const char *base_name(const char *path) {
    const char *slash = strrchr(path, '/');
    return slash ? slash + 1 : path;
}

static int cmd_echo(int argc, char **argv) {
    for (int i = 1; i < argc; ++i)
        printf("%s%s", i == 1 ? "" : " ", argv[i]);
    putchar('\n');
    return 0;
}

static int cmd_cat(int argc, char **argv) {
    if (argc == 1) {
        char buf[8192];
        size_t n;
        while ((n = fread(buf, 1, sizeof buf, stdin)) != 0)
            if (fwrite(buf, 1, n, stdout) != n) return 1;
        return ferror(stdin) ? 1 : 0;
    }
    int rc = 0;
    for (int i = 1; i < argc; ++i) {
        FILE *f = fopen(argv[i], "rb");
        if (!f) {
            fprintf(stderr, "cat: %s: %s\n", argv[i], strerror(errno));
            rc = 1;
            continue;
        }
        char buf[8192];
        size_t n;
        while ((n = fread(buf, 1, sizeof buf, f)) != 0)
            if (fwrite(buf, 1, n, stdout) != n) { rc = 1; break; }
        if (ferror(f)) rc = 1;
        fclose(f);
    }
    return rc;
}

static int cmd_ls(int argc, char **argv) {
    const char *path = argc > 1 ? argv[1] : ".";
    DIR *dir = opendir(path);
    if (!dir) {
        fprintf(stderr, "ls: %s: %s\n", path, strerror(errno));
        return 1;
    }
    struct dirent *entry;
    int first = 1;
    while ((entry = readdir(dir)) != NULL) {
        if (entry->d_name[0] == '.') continue;
        printf("%s%s", first ? "" : "  ", entry->d_name);
        first = 0;
    }
    if (!first) putchar('\n');
    closedir(dir);
    return 0;
}

static int cmd_pwd(void) {
    char path[PATH_MAX];
    if (!getcwd(path, sizeof path)) {
        fprintf(stderr, "pwd: %s\n", strerror(errno));
        return 1;
    }
    puts(path);
    return 0;
}

static int cmd_env(void) {
    extern char **environ;
    for (char **p = environ; *p; ++p) puts(*p);
    return 0;
}

static int cmd_mkdir(int argc, char **argv) {
    if (argc < 2) {
        usage("mkdir");
        return 2;
    }
    int rc = 0;
    for (int i = 1; i < argc; ++i) {
        if (mkdir(argv[i], 0755) != 0) {
            struct stat st;
            const int mkdir_error = errno;
            if (mkdir_error == EEXIST && stat(argv[i], &st) == 0 && S_ISDIR(st.st_mode))
                continue;
            fprintf(stderr, "mkdir: %s: %s\n", argv[i], strerror(mkdir_error));
            rc = 1;
        }
    }
    return rc;
}

static int cmd_touch(int argc, char **argv) {
    if (argc < 2) {
        usage("touch");
        return 2;
    }
    int rc = 0;
    for (int i = 1; i < argc; ++i) {
        int fd = open(argv[i], O_WRONLY | O_CREAT, 0644);
        if (fd < 0) {
            fprintf(stderr, "touch: %s: %s\n", argv[i], strerror(errno));
            rc = 1;
            continue;
        }
        if (close(fd) != 0 || utimensat(AT_FDCWD, argv[i], NULL, 0) != 0) {
            fprintf(stderr, "touch: %s: %s\n", argv[i], strerror(errno));
            rc = 1;
        }
    }
    return rc;
}

static int cmd_rm(int argc, char **argv) {
    if (argc < 2) {
        usage("rm");
        return 2;
    }
    int rc = 0;
    for (int i = 1; i < argc; ++i)
        if (remove(argv[i]) != 0) {
            fprintf(stderr, "rm: %s: %s\n", argv[i], strerror(errno));
            rc = 1;
        }
    return rc;
}

static int cmd_cp_mv(int argc, char **argv, int move) {
    if (argc != 3) {
        fprintf(stderr, "%s: source and destination required\n", move ? "mv" : "cp");
        return 2;
    }
    if (move) {
        if (rename(argv[1], argv[2]) == 0) return 0;
        fprintf(stderr, "mv: %s: %s\n", argv[1], strerror(errno));
        return 1;
    }
    struct stat src_st, dst_st;
    if (stat(argv[1], &src_st) != 0) {
        fprintf(stderr, "cp: %s: %s\n", argv[1], strerror(errno));
        return 1;
    }
    if (stat(argv[2], &dst_st) == 0 && src_st.st_dev == dst_st.st_dev &&
        src_st.st_ino == dst_st.st_ino) {
        fprintf(stderr, "cp: '%s' and '%s' are the same file\n", argv[1], argv[2]);
        return 1;
    }
    FILE *in = fopen(argv[1], "rb");
    FILE *out = fopen(argv[2], "wb");
    if (!in || !out) {
        fprintf(stderr, "%s: %s\n", move ? "mv" : "cp", strerror(errno));
        if (in) fclose(in);
        if (out) fclose(out);
        return 1;
    }
    char buf[8192];
    size_t n;
    int rc = 0;
    while ((n = fread(buf, 1, sizeof buf, in)) != 0)
        if (fwrite(buf, 1, n, out) != n) { rc = 1; break; }
    if (ferror(in)) rc = 1;
    const int in_close_error = fclose(in);
    const int out_close_error = fclose(out);
    if (in_close_error != 0 || out_close_error != 0) rc = 1;
    if (rc == 0) (void)chmod(argv[2], src_st.st_mode & 07777);
    return rc;
}

static int cmd_grep(int argc, char **argv) {
    if (argc < 2) {
        usage("grep");
        return 2;
    }
    const char *needle = argv[1];
    int found = 1;
    int files = argc - 2;
    for (int i = 0; i <= files; ++i) {
        FILE *f = files ? fopen(argv[i + 2], "r") : stdin;
        if (!f) {
            fprintf(stderr, "grep: %s: %s\n", argv[i + 2], strerror(errno));
            found = 2;
            continue;
        }
        char *line = NULL;
        size_t cap = 0;
        while (getline(&line, &cap, f) >= 0) {
            if (strstr(line, needle)) {
                if (files > 1) printf("%s:", argv[i + 2]);
                fputs(line, stdout);
                found = 0;
            }
        }
        if (ferror(f)) found = 2;
        free(line);
        if (f != stdin) fclose(f);
    }
    return found;
}

static int cmd_wc(int argc, char **argv) {
    if (argc > 2) {
        usage("wc");
        return 2;
    }
    FILE *f = argc > 1 ? fopen(argv[1], "rb") : stdin;
    if (!f) {
        fprintf(stderr, "wc: %s: %s\n", argv[1], strerror(errno));
        return 1;
    }
    unsigned long long lines = 0, words = 0, bytes = 0;
    int in_word = 0, c;
    while ((c = fgetc(f)) != EOF) {
        ++bytes;
        if (c == '\n') ++lines;
        if (c == ' ' || c == '\t' || c == '\n' || c == '\r') in_word = 0;
        else if (!in_word) { in_word = 1; ++words; }
    }
    const int read_error = ferror(f);
    printf("%llu %llu %llu\n", lines, words, bytes);
    if (f != stdin) fclose(f);
    return read_error ? 1 : 0;
}

static int dispatch(int argc, char **argv);

static int cmd_sh(void) {
    char *line = NULL;
    size_t cap = 0;
    while (1) {
        if (isatty(STDIN_FILENO)) {
            fputs("domix$ ", stdout);
            fflush(stdout);
        }
        ssize_t n = getline(&line, &cap, stdin);
        if (n < 0) break;
        if (n && line[n - 1] == '\n') line[n - 1] = '\0';
        char *args[64];
        int argc = 0;
        for (char *tok = strtok(line, " \t"); tok && argc < 63;
             tok = strtok(NULL, " \t")) args[argc++] = tok;
        args[argc] = NULL;
        if (argc && strcmp(args[0], "exit") == 0) break;
        if (argc) (void)dispatch(argc, args);
    }
    free(line);
    return 0;
}

static int dispatch(int argc, char **argv) {
    if (argc <= 0 || !argv || !argv[0]) return 2;
    const char *name = base_name(argv[0]);
    if (strcmp(name, "domixbox") == 0 && argc > 1) {
        ++argv;
        --argc;
        name = argv[0];
    }
    if (strcmp(name, "echo") == 0 || strcmp(name, "printf") == 0) return cmd_echo(argc, argv);
    if (strcmp(name, "cat") == 0) return cmd_cat(argc, argv);
    if (strcmp(name, "ls") == 0) return cmd_ls(argc, argv);
    if (strcmp(name, "pwd") == 0) return cmd_pwd();
    if (strcmp(name, "env") == 0) return cmd_env();
    if (strcmp(name, "true") == 0) return 0;
    if (strcmp(name, "false") == 0) return 1;
    if (strcmp(name, "mkdir") == 0) return cmd_mkdir(argc, argv);
    if (strcmp(name, "touch") == 0) return cmd_touch(argc, argv);
    if (strcmp(name, "rm") == 0) return cmd_rm(argc, argv);
    if (strcmp(name, "cp") == 0) return cmd_cp_mv(argc, argv, 0);
    if (strcmp(name, "mv") == 0) return cmd_cp_mv(argc, argv, 1);
    if (strcmp(name, "grep") == 0) return cmd_grep(argc, argv);
    if (strcmp(name, "wc") == 0) return cmd_wc(argc, argv);
    if (strcmp(name, "basename") == 0) { puts(base_name(argc > 1 ? argv[1] : "")); return 0; }
    if (strcmp(name, "dirname") == 0) {
        char *copy = strdup(argc > 1 ? argv[1] : ".");
        if (!copy) return 1;
        char *slash = strrchr(copy, '/');
        if (slash && slash != copy) *slash = '\0';
        else strcpy(copy, slash ? "/" : ".");
        puts(copy);
        free(copy);
        return 0;
    }
    if (strcmp(name, "which") == 0) {
        char path[PATH_MAX];
        const char *p = getenv("PATH");
        if (argc < 2 || !p) return 1;
        for (const char *start = p; *start;) {
            const char *end = strchr(start, ':');
            size_t len = end ? (size_t)(end - start) : strlen(start);
            if (len + 1 + strlen(argv[1]) + 1 < sizeof path) {
                memcpy(path, start, len);
                path[len] = '/';
                strcpy(path + len + 1, argv[1]);
                if (access(path, X_OK) == 0) { puts(path); return 0; }
            }
            if (!end) break;
            start = end + 1;
        }
        return 1;
    }
    if (strcmp(name, "domixbox") == 0 || strcmp(name, "init") == 0 ||
        strcmp(name, "sh") == 0 || strcmp(name, "bash") == 0) return cmd_sh();
    fprintf(stderr, "%s: command not found\n", name);
    return 127;
}

int main(int argc, char **argv) {
    return dispatch(argc, argv);
}
