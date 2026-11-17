/*
 * The Start menu entry of a ScummVM game package (GAME_ID, set when
 * compiling: "sky", "queen"). It runs the ScummVM package's scummvm-start
 * with the game folder of this package, so the game starts without adding
 * it in ScummVM first. ScummVM is its own Store package: it is looked for
 * next to this package's folder (/disk/Programs or the account's
 * Programs), then in /disk/Programs.
 */
#include <stdio.h>
#include <string.h>
#include <sys/auxv.h>
#include <unistd.h>

#ifndef GAME_ID
#error "GAME_ID is not set"
#endif

#define PATH_SIZE 1024
#define SCUMMVM_ID "org.scummvm.scummvm"

int main(int argc, char **argv) {
    (void)argc;
    const char *self = (const char *)getauxval(AT_EXECFN);
    if (self == NULL || self[0] != '/') {
        self = argv[0];
    }
    char dir[PATH_SIZE];
    if (self == NULL || snprintf(dir, sizeof dir, "%s", self) >= (int)sizeof dir) {
        fputs("game-start: program path too long\n", stderr);
        return 127;
    }
    char *slash = strrchr(dir, '/');
    if (slash != NULL) {
        *slash = '\0';
    } else {
        strcpy(dir, ".");
    }

    /* The folder holding this package's folder. */
    char parent[PATH_SIZE];
    snprintf(parent, sizeof parent, "%s", dir);
    slash = strrchr(parent, '/');
    if (slash != NULL && slash != parent) {
        *slash = '\0';
    } else {
        strcpy(parent, "/disk/Programs");
    }

    char path[PATH_SIZE];
    char scummvm[PATH_SIZE];
    if (snprintf(path, sizeof path, "--path=%s/game", dir) >= (int)sizeof path
        || snprintf(scummvm, sizeof scummvm, "%s/" SCUMMVM_ID "/scummvm-start",
                    parent) >= (int)sizeof scummvm) {
        fputs("game-start: program path too long\n", stderr);
        return 127;
    }
    if (access(scummvm, X_OK) != 0) {
        snprintf(scummvm, sizeof scummvm,
                 "/disk/Programs/" SCUMMVM_ID "/scummvm-start");
    }

    char *args[] = {scummvm, path, GAME_ID, NULL};
    execv(scummvm, args);
    fputs("This game needs ScummVM. Install ScummVM from the OPOS Store "
          "(Games), then start the game again.\n", stderr);
    return 127;
}
