/*
 * Posts a notification from the Terminal or a script:
 *   notify TITLE [BODY...]
 * The first argument is the title; the others, joined by spaces, are the
 * body. Exits 0 when it was posted, 1 when it was refused or failed.
 */
#include <opos.h>

OPOS_MANIFEST("Notify", "1.0", OPOS_CAP_NOTIFY);

static void say(const char *text) {
    opos_write(2, text, opos_strlen(text));
}

int main(int argc, char **argv, char **envp) {
    (void)envp;
    if (argc < 2 || argv[1][0] == '\0') {
        say("usage: notify TITLE [BODY...]\n");
        return 1;
    }
    static char body[192];
    size_t used = 0;
    for (int i = 2; i < argc; i++) {
        for (const char *c = argv[i]; *c != '\0' && used + 1 < sizeof(body);
             c++) {
            body[used++] = *c;
        }
        if (i + 1 < argc && used + 1 < sizeof(body)) {
            body[used++] = ' ';
        }
    }
    body[used] = '\0';
    long result = opos_notify(argv[1], body);
    if (result >= 0) {
        return 0;
    }
    say(result == -OPOS_EACCES ? "notify: not allowed to show notifications\n"
        : result == -OPOS_EAGAIN ? "notify: too many notifications; wait a minute\n"
        : result == -OPOS_ENOSYS ? "notify: this OPOS has no notifications\n"
                                 : "notify: failed\n");
    return 1;
}
