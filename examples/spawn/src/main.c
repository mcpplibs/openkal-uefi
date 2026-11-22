/* A UEFI application in C beneath openkal-platform's C library, over this implementation's
 * openkal.process (0.13.0): posix_spawn starts this program again, waitpid answers the status, a
 * variable set with setenv and the directory set with chdir reach the copy, and a name that is not
 * there is ENOENT. Started by the UEFI Shell as `spawn.efi' from the volume's root. */
#include <errno.h>
#include <spawn.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <sys/stat.h>
#include <sys/wait.h>
#include <unistd.h>

extern char** environ;

static int failures;
static void check(int ok, const char* what) {
    printf("  %s %s\n", ok ? "ok  " : "FAIL", what);
    if (!ok) ++failures;
}

/* This program's file, from the root: the last component of the name the Shell started it by. */
static char self[64] = "/spawn.efi";

static int start(char* const* argv, int* status) {
    pid_t pid;
    const int rc = posix_spawn(&pid, self, NULL, NULL, argv, environ);
    if (rc != 0) return rc;
    if (waitpid(pid, status, 0) != pid) return errno;
    return 0;
}

int main(int argc, char** argv) {
    if (argc >= 2 && strcmp(argv[1], "child") == 0) {
        char where[256];
        const char* v = getenv("OKSPAWN");
        const int vector = argc == 4 && strcmp(argv[2], "a b") == 0 && strcmp(argv[3], "") == 0;
        const int variable = v && strcmp(v, "set") == 0;
        const int directory = getcwd(where, sizeof where) && strcmp(where, "/okd") == 0;
        return vector && variable && directory ? 3 : 1;
    }
    const char* base = strrchr(argv[0], '\\');
    if (base && strlen(base) < sizeof self) { self[0] = '/'; strcpy(self + 1, base + 1); }
    printf("hello from the C library over openkal-uefi, started as %s\n", self);

    setenv("OKSPAWN", "set", 1);
    mkdir("/okd", 0777);
    check(chdir("/okd") == 0, "chdir moves the C library's directory");
    char* child[] = { argv[0], "child", "a b", "", NULL };
    int status = -1;
    const int rc = start(child, &status);
    check(rc == 0 && WIFEXITED(status) && WEXITSTATUS(status) == 3,
          "posix_spawn starts a copy, which receives its vector, the variable and the directory, and "
          "waitpid answers its status");
    pid_t pid;
    char* absent[] = { "absent.efi", NULL };
    check(posix_spawn(&pid, "/absent.efi", NULL, NULL, absent, environ) == ENOENT,
          "a name that is not there is ENOENT");
    puts(failures ? "FAILED" : "all held");
    return failures ? 1 : 42;
}
