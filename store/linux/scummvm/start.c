/*
 * scummvm-start: the Start menu entry of the ScummVM package. It runs
 * scummvm from its own folder with the package's data/ folder as the theme
 * and extra path, where ScummVM finds its themes, translations and the
 * engine data files (sky.cpt, queen.tbl, ...), which it would otherwise
 * look for only under /usr/local/share/scummvm. Arguments are passed on, so
 * the game packages (game-start.c) can start a game directly.
 */
#include <stdio.h>
#include <string.h>
#include <sys/auxv.h>
#include <unistd.h>

#define PATH_SIZE 1024

int main(int argc, char **argv) {
    const char *self = (const char *)getauxval(AT_EXECFN);
    if (self == NULL || self[0] != '/') {
        self = argv[0];
    }
    char dir[PATH_SIZE];
    if (self == NULL || snprintf(dir, sizeof dir, "%s", self) >= (int)sizeof dir) {
        fputs("scummvm-start: program path too long\n", stderr);
        return 127;
    }
    char *slash = strrchr(dir, '/');
    if (slash != NULL) {
        *slash = '\0';
    } else {
        strcpy(dir, ".");
    }

    char program[PATH_SIZE];
    char themes[PATH_SIZE];
    char extra[PATH_SIZE];
    if (snprintf(program, sizeof program, "%s/scummvm", dir) >= (int)sizeof program
        || snprintf(themes, sizeof themes, "--themepath=%s/data", dir)
               >= (int)sizeof themes
        || snprintf(extra, sizeof extra, "--extrapath=%s/data", dir)
               >= (int)sizeof extra) {
        fputs("scummvm-start: program path too long\n", stderr);
        return 127;
    }

    char *args[argc + 3];
    args[0] = program;
    args[1] = themes;
    args[2] = extra;
    for (int i = 1; i < argc; i++) {
        args[i + 2] = argv[i];
    }
    args[argc + 2] = NULL;
    execv(program, args);
    perror("scummvm-start: scummvm");
    return 127;
}
