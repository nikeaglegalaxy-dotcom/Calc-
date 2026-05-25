#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <unistd.h>
#include <fcntl.h>
#include <errno.h>
#include <sys/stat.h>
#include <sys/types.h>
#include <sys/wait.h>

static int cmd_info(int argc, char **argv) {
    if (argc < 1) {
        fprintf(stderr, "usage: syslab info <path>\n");
        return 1;
    }
    const char *path = argv[0];

    struct stat st;
    if (stat(path, &st) == -1) {
        fprintf(stderr, "stat(%s): %s\n", path, strerror(errno));
        return 1;
    }

    const char *type;
    if (S_ISREG(st.st_mode))       type = "regular file";
    else if (S_ISDIR(st.st_mode))  type = "directory";
    else if (S_ISLNK(st.st_mode))  type = "symbolic link";
    else if (S_ISCHR(st.st_mode))  type = "character device";
    else if (S_ISBLK(st.st_mode))  type = "block device";
    else if (S_ISFIFO(st.st_mode)) type = "FIFO";
    else if (S_ISSOCK(st.st_mode)) type = "socket";
    else                            type = "unknown";

    int readable   = access(path, R_OK) == 0;
    int writable   = access(path, W_OK) == 0;
    int executable = access(path, X_OK) == 0;

    printf("Path: %s\n", path);
    printf("Type: %s\n", type);
    printf("Size: %lld bytes\n", (long long)st.st_size);
    printf("Readable: %s\n", readable ? "yes" : "no");
    printf("Writable: %s\n", writable ? "yes" : "no");
    printf("Executable: %s\n", executable ? "yes" : "no");
    return 0;
}

static int cmd_copy(int argc, char **argv) {
    if (argc < 2) {
        fprintf(stderr, "usage: syslab copy <src> <dst>\n");
        return 1;
    }
    const char *src = argv[0];
    const char *dst = argv[1];

    int in = open(src, O_RDONLY);
    if (in == -1) {
        fprintf(stderr, "open(%s): %s\n", src, strerror(errno));
        return 1;
    }

    struct stat st;
    mode_t mode = 0644;
    if (fstat(in, &st) == 0) {
        mode = st.st_mode & 0777;
    }

    int out = open(dst, O_WRONLY | O_CREAT | O_TRUNC, mode);
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
            ssize_t w = write(out, buf + off, n - off);
            if (w == -1) {
                if (errno == EINTR) continue;
                fprintf(stderr, "write(%s): %s\n", dst, strerror(errno));
                close(in);
                close(out);
                return 1;
            }
            off += w;
        }
    }
    if (n == -1) {
        fprintf(stderr, "read(%s): %s\n", src, strerror(errno));
        close(in);
        close(out);
        return 1;
    }

    close(in);
    close(out);
    return 0;
}

static int cmd_tail(int argc, char **argv) {
    if (argc < 2) {
        fprintf(stderr, "usage: syslab tail <file> <N>\n");
        return 1;
    }
    const char *path = argv[0];
    long n_lines = strtol(argv[1], NULL, 10);
    if (n_lines <= 0) {
        fprintf(stderr, "N must be a positive integer\n");
        return 1;
    }

    int fd = open(path, O_RDONLY);
    if (fd == -1) {
        fprintf(stderr, "open(%s): %s\n", path, strerror(errno));
        return 1;
    }

    off_t end = lseek(fd, 0, SEEK_END);
    if (end == -1) {
        fprintf(stderr, "lseek: %s\n", strerror(errno));
        close(fd);
        return 1;
    }

    const size_t CHUNK = 4096;
    char buf[CHUNK];
    off_t pos = end;
    long newlines = 0;
    off_t start = 0;

    /* count newlines from the end; stop after finding n_lines+1 newlines
       (so we land just past the newline that begins the Nth-from-last line) */
    while (pos > 0) {
        size_t to_read = pos > (off_t)CHUNK ? CHUNK : (size_t)pos;
        pos -= to_read;
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
            /* ignore trailing newline at very end of file */
            if (buf[i] == '\n' && (pos + i + 1 != end)) {
                if (++newlines == n_lines) {
                    start = pos + i + 1;
                    goto found;
                }
            }
        }
    }
    start = 0;
found:

    if (lseek(fd, start, SEEK_SET) == -1) {
        fprintf(stderr, "lseek: %s\n", strerror(errno));
        close(fd);
        return 1;
    }

    ssize_t r;
    while ((r = read(fd, buf, sizeof(buf))) > 0) {
        ssize_t off = 0;
        while (off < r) {
            ssize_t w = write(STDOUT_FILENO, buf + off, r - off);
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
    if (argc < 1) {
        fprintf(stderr, "usage: syslab run <program> [args...]\n");
        return 1;
    }

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
        fprintf(stderr, "child killed by signal %d\n", WTERMSIG(status));
        return 128 + WTERMSIG(status);
    }
    return 1;
}

static void usage(void) {
    fprintf(stderr,
        "usage: syslab <command> [arguments]\n"
        "\n"
        "commands:\n"
        "  info <path>              show file information (stat, access)\n"
        "  copy <src> <dst>         copy file contents (open, read, write, close)\n"
        "  tail <file> <N>          print last N lines (open, read, lseek, write, close)\n"
        "  run  <program> [args...] run program in a child process (fork, execvp, waitpid)\n"
    );
}

int main(int argc, char **argv) {
    if (argc < 2) {
        usage();
        return 1;
    }
    const char *cmd = argv[1];
    int rest_argc = argc - 2;
    char **rest_argv = argv + 2;

    if (strcmp(cmd, "info") == 0) return cmd_info(rest_argc, rest_argv);
    if (strcmp(cmd, "copy") == 0) return cmd_copy(rest_argc, rest_argv);
    if (strcmp(cmd, "tail") == 0) return cmd_tail(rest_argc, rest_argv);
    if (strcmp(cmd, "run")  == 0) return cmd_run(rest_argc, rest_argv);

    fprintf(stderr, "unknown command: %s\n", cmd);
    usage();
    return 1;
}
