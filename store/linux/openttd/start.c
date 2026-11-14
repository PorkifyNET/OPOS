/*
 * openttd-start: the Start menu entry of the OpenTTD package. It runs
 * openttd from its own folder with FluidSynth as the music driver and the
 * SoundFont shipped in the package, which OpenTTD would otherwise look for
 * only under /usr/share. Arguments are passed on after the -m option, so a
 * later -m still wins.
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
        fputs("openttd-start: program path too long\n", stderr);
        return 127;
    }
    char *slash = strrchr(dir, '/');
    if (slash != NULL) {
        *slash = '\0';
    } else {
        strcpy(dir, ".");
    }

    char game[PATH_SIZE];
    char music[PATH_SIZE];
    if (snprintf(game, sizeof game, "%s/openttd", dir) >= (int)sizeof game
        || snprintf(music, sizeof music,
                    "fluidsynth:soundfont=%s/soundfont/GeneralUser-GS.sf2", dir)
               >= (int)sizeof music) {
        fputs("openttd-start: program path too long\n", stderr);
        return 127;
    }

    char *args[argc + 3];
    args[0] = game;
    args[1] = "-m";
    args[2] = music;
    for (int i = 1; i < argc; i++) {
        args[i + 2] = argv[i];
    }
    args[argc + 2] = NULL;
    execv(game, args);
    perror("openttd-start: openttd");
    return 127;
}
