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

static int usage(void) {
    fprintf(stderr,
            "Usage:\n"
            "  syslab info <path>\n"
            "  syslab copy <src> <dst>\n"
            "  syslab tail <path> <N>\n"
            "  syslab run  <program> [args...]\n");
    return 1;
}

static const char *file_type(mode_t m) {
    if (S_ISREG(m))  return "regular file";
    if (S_ISDIR(m))  return "directory";
    if (S_ISLNK(m))  return "symbolic link";
    if (S_ISCHR(m))  return "character device";
    if (S_ISBLK(m))  return "block device";
    if (S_ISFIFO(m)) return "fifo";
    if (S_ISSOCK(m)) return "socket";
    return "unknown";
}

static int cmd_info(int argc, char **argv) {
    if (argc != 1) {
        fprintf(stderr, "info: expected exactly 1 argument\n");
        return 1;
    }
    const char *path = argv[0];

    struct stat st;
    if (stat(path, &st) == -1) {
        fprintf(stderr, "stat(%s): %s\n", path, strerror(errno));
        return 1;
    }

    printf("Path: %s\n", path);
    printf("Type: %s\n", file_type(st.st_mode));
    printf("Size: %lld bytes\n", (long long)st.st_size);
    printf("Readable: %s\n",   access(path, R_OK) == 0 ? "yes" : "no");
    printf("Writable: %s\n",   access(path, W_OK) == 0 ? "yes" : "no");
    printf("Executable: %s\n", access(path, X_OK) == 0 ? "yes" : "no");
    return 0;
}

static int cmd_copy(int argc, char **argv) {
    if (argc != 2) {
        fprintf(stderr, "copy: expected <src> <dst>\n");
        return 1;
    }
    const char *src = argv[0];
    const char *dst = argv[1];

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
    if (argc != 2) {
        fprintf(stderr, "tail: expected <file> <N>\n");
        return 1;
    }
    const char *path = argv[0];
    char *end = NULL;
    long nlines = strtol(argv[1], &end, 10);
    if (end == argv[1] || *end != '\0' || nlines < 0) {
        fprintf(stderr, "tail: N must be a non-negative integer\n");
        return 1;
    }
    if (nlines == 0) return 0;

    int fd = open(path, O_RDONLY);
    if (fd == -1) {
        fprintf(stderr, "open(%s): %s\n", path, strerror(errno));
        return 1;
    }

    off_t size = lseek(fd, 0, SEEK_END);
    if (size == (off_t)-1) {
        fprintf(stderr, "lseek(%s): %s\n", path, strerror(errno));
        close(fd);
        return 1;
    }

    /* Walk backwards counting newlines. We need (nlines) newlines that
       precede the data we want to print, so the printed region starts
       right after the (nlines)-th newline from the end (not counting a
       trailing newline at EOF). */
    off_t pos = size;
    long newlines_seen = 0;
    char buf[4096];

    while (pos > 0) {
        off_t chunk = (pos > (off_t)sizeof(buf)) ? (off_t)sizeof(buf) : pos;
        pos -= chunk;
        if (lseek(fd, pos, SEEK_SET) == (off_t)-1) {
            fprintf(stderr, "lseek(%s): %s\n", path, strerror(errno));
            close(fd);
            return 1;
        }
        ssize_t got = 0;
        while (got < chunk) {
            ssize_t r = read(fd, buf + got, (size_t)(chunk - got));
            if (r == -1) {
                if (errno == EINTR) continue;
                fprintf(stderr, "read(%s): %s\n", path, strerror(errno));
                close(fd);
                return 1;
            }
            if (r == 0) break;
            got += r;
        }

        for (ssize_t i = got - 1; i >= 0; i--) {
            /* Skip a trailing newline at the very end of the file so
               that "tail 1" on "a\nb\n" prints "b\n", not "". */
            if (buf[i] == '\n' && !(pos + i == size - 1 && size > 0)) {
                newlines_seen++;
                if (newlines_seen == nlines) {
                    off_t start = pos + i + 1;
                    if (lseek(fd, start, SEEK_SET) == (off_t)-1) {
                        fprintf(stderr, "lseek(%s): %s\n", path, strerror(errno));
                        close(fd);
                        return 1;
                    }
                    ssize_t n;
                    while ((n = read(fd, buf, sizeof(buf))) > 0) {
                        ssize_t off = 0;
                        while (off < n) {
                            ssize_t w = write(STDOUT_FILENO, buf + off, (size_t)(n - off));
                            if (w == -1) {
                                if (errno == EINTR) continue;
                                fprintf(stderr, "write: %s\n", strerror(errno));
                                close(fd);
                                return 1;
                            }
                            off += w;
                        }
                    }
                    if (n == -1) {
                        fprintf(stderr, "read(%s): %s\n", path, strerror(errno));
                        close(fd);
                        return 1;
                    }
                    close(fd);
                    return 0;
                }
            }
        }
    }

    /* Fewer than N newlines: print the whole file. */
    if (lseek(fd, 0, SEEK_SET) == (off_t)-1) {
        fprintf(stderr, "lseek(%s): %s\n", path, strerror(errno));
        close(fd);
        return 1;
    }
    ssize_t n;
    while ((n = read(fd, buf, sizeof(buf))) > 0) {
        ssize_t off = 0;
        while (off < n) {
            ssize_t w = write(STDOUT_FILENO, buf + off, (size_t)(n - off));
            if (w == -1) {
                if (errno == EINTR) continue;
                fprintf(stderr, "write: %s\n", strerror(errno));
                close(fd);
                return 1;
            }
            off += w;
        }
    }
    if (n == -1) {
        fprintf(stderr, "read(%s): %s\n", path, strerror(errno));
        close(fd);
        return 1;
    }
    close(fd);
    return 0;
}

static int cmd_run(int argc, char **argv) {
    if (argc < 1) {
        fprintf(stderr, "run: expected <program> [args...]\n");
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

    int status = 0;
    if (waitpid(pid, &status, 0) == -1) {
        fprintf(stderr, "waitpid: %s\n", strerror(errno));
        return 1;
    }

    if (WIFEXITED(status))   return WEXITSTATUS(status);
    if (WIFSIGNALED(status)) return 128 + WTERMSIG(status);
    return 1;
}

int main(int argc, char **argv) {
    if (argc < 2) return usage();

    const char *cmd = argv[1];
    int subargc = argc - 2;
    char **subargv = argv + 2;

    if      (strcmp(cmd, "info") == 0) return cmd_info(subargc, subargv);
    else if (strcmp(cmd, "copy") == 0) return cmd_copy(subargc, subargv);
    else if (strcmp(cmd, "tail") == 0) return cmd_tail(subargc, subargv);
    else if (strcmp(cmd, "run")  == 0) return cmd_run(subargc, subargv);

    fprintf(stderr, "syslab: unknown command '%s'\n", cmd);
    return usage();
}
