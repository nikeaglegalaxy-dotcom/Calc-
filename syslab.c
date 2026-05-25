#define _POSIX_C_SOURCE 200809L

#include <errno.h>
#include <fcntl.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <sys/stat.h>
#include <sys/types.h>
#include <sys/wait.h>
#include <unistd.h>

static int cmd_info(const char *path) {
    struct stat st;
    if (stat(path, &st) == -1) {
        fprintf(stderr, "stat(%s): %s\n", path, strerror(errno));
        return 1;
    }

    const char *type;
    if (S_ISREG(st.st_mode))      type = "regular file";
    else if (S_ISDIR(st.st_mode)) type = "directory";
    else if (S_ISLNK(st.st_mode)) type = "symbolic link";
    else if (S_ISCHR(st.st_mode)) type = "character device";
    else if (S_ISBLK(st.st_mode)) type = "block device";
    else if (S_ISFIFO(st.st_mode))type = "FIFO";
    else if (S_ISSOCK(st.st_mode))type = "socket";
    else                          type = "unknown";

    int r = access(path, R_OK) == 0;
    int w = access(path, W_OK) == 0;
    int x = access(path, X_OK) == 0;

    printf("Path: %s\n", path);
    printf("Type: %s\n", type);
    printf("Size: %lld bytes\n", (long long)st.st_size);
    printf("Readable: %s\n", r ? "yes" : "no");
    printf("Writable: %s\n", w ? "yes" : "no");
    printf("Executable: %s\n", x ? "yes" : "no");
    return 0;
}

static int cmd_copy(const char *src, const char *dst) {
    int in = open(src, O_RDONLY);
    if (in == -1) {
        fprintf(stderr, "open(%s): %s\n", src, strerror(errno));
        return 1;
    }

    int out = open(dst, O_WRONLY | O_CREAT | O_TRUNC, 0644);
    if (out == -1) {
        fprintf(stderr, "open(%s): %s\n", dst, strerror(errno));
        close(in);
        return 1;
    }

    char buf[8192];
    ssize_t n;
    while ((n = read(in, buf, sizeof(buf))) > 0) {
        ssize_t off = 0;
        while (off < n) {
            ssize_t w = write(out, buf + off, (size_t)(n - off));
            if (w == -1) {
                if (errno == EINTR) continue;
                fprintf(stderr, "write: %s\n", strerror(errno));
                close(in);
                close(out);
                return 1;
            }
            off += w;
        }
    }
    if (n == -1) {
        fprintf(stderr, "read: %s\n", strerror(errno));
        close(in);
        close(out);
        return 1;
    }

    close(in);
    close(out);
    return 0;
}

static int cmd_tail(const char *path, long n_lines) {
    if (n_lines <= 0) {
        fprintf(stderr, "tail: N must be positive\n");
        return 1;
    }

    int fd = open(path, O_RDONLY);
    if (fd == -1) {
        fprintf(stderr, "open(%s): %s\n", path, strerror(errno));
        return 1;
    }

    off_t size = lseek(fd, 0, SEEK_END);
    if (size == -1) {
        fprintf(stderr, "lseek: %s\n", strerror(errno));
        close(fd);
        return 1;
    }

    off_t pos = size;
    long newlines = 0;
    const size_t CHUNK = 4096;
    char buf[4096];

    while (pos > 0 && newlines <= n_lines) {
        size_t to_read = (size_t)(pos > (off_t)CHUNK ? CHUNK : pos);
        pos -= (off_t)to_read;
        if (lseek(fd, pos, SEEK_SET) == -1) {
            fprintf(stderr, "lseek: %s\n", strerror(errno));
            close(fd);
            return 1;
        }
        ssize_t r = read(fd, buf, to_read);
        if (r == -1) {
            fprintf(stderr, "read: %s\n", strerror(errno));
            close(fd);
            return 1;
        }
        for (ssize_t i = r - 1; i >= 0; i--) {
            if (buf[i] == '\n') {
                newlines++;
                if (newlines > n_lines) {
                    pos += i + 1;
                    goto found;
                }
            }
        }
    }
found:
    if (lseek(fd, pos, SEEK_SET) == -1) {
        fprintf(stderr, "lseek: %s\n", strerror(errno));
        close(fd);
        return 1;
    }

    char out[8192];
    ssize_t r;
    while ((r = read(fd, out, sizeof(out))) > 0) {
        ssize_t off = 0;
        while (off < r) {
            ssize_t w = write(STDOUT_FILENO, out + off, (size_t)(r - off));
            if (w == -1) {
                if (errno == EINTR) continue;
                fprintf(stderr, "write: %s\n", strerror(errno));
                close(fd);
                return 1;
            }
            off += w;
        }
    }
    if (r == -1) {
        fprintf(stderr, "read: %s\n", strerror(errno));
        close(fd);
        return 1;
    }

    close(fd);
    return 0;
}

static int cmd_run(int argc, char **argv) {
    pid_t pid = fork();
    if (pid == -1) {
        fprintf(stderr, "fork: %s\n", strerror(errno));
        return 1;
    }
    if (pid == 0) {
        execvp(argv[0], argv);
        fprintf(stderr, "execvp(%s): %s\n", argv[0], strerror(errno));
        _exit(127);
    }

    int status;
    if (waitpid(pid, &status, 0) == -1) {
        fprintf(stderr, "waitpid: %s\n", strerror(errno));
        return 1;
    }
    if (WIFEXITED(status)) {
        return WEXITSTATUS(status);
    }
    if (WIFSIGNALED(status)) {
        fprintf(stderr, "child terminated by signal %d\n", WTERMSIG(status));
        return 128 + WTERMSIG(status);
    }
    (void)argc;
    return 1;
}

static void usage(const char *prog) {
    fprintf(stderr,
        "Usage:\n"
        "  %s info <path>\n"
        "  %s copy <src> <dst>\n"
        "  %s tail <file> <N>\n"
        "  %s run  <program> [args...]\n",
        prog, prog, prog, prog);
}

int main(int argc, char **argv) {
    if (argc < 2) {
        usage(argv[0]);
        return 1;
    }

    const char *cmd = argv[1];

    if (strcmp(cmd, "info") == 0) {
        if (argc != 3) { usage(argv[0]); return 1; }
        return cmd_info(argv[2]);
    }
    if (strcmp(cmd, "copy") == 0) {
        if (argc != 4) { usage(argv[0]); return 1; }
        return cmd_copy(argv[2], argv[3]);
    }
    if (strcmp(cmd, "tail") == 0) {
        if (argc != 4) { usage(argv[0]); return 1; }
        char *end;
        long n = strtol(argv[3], &end, 10);
        if (*end != '\0') { usage(argv[0]); return 1; }
        return cmd_tail(argv[2], n);
    }
    if (strcmp(cmd, "run") == 0) {
        if (argc < 3) { usage(argv[0]); return 1; }
        return cmd_run(argc - 2, argv + 2);
    }

    usage(argv[0]);
    return 1;
}
