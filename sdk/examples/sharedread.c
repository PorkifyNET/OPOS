/* Tries to read each path given on the command line and prints whether
   OPOS allowed it. Copy it to /disk/Programs (or a folder below it) to see
   the files.shared rules from docs/app-sandbox.md:

     sharedread /disk/notes.txt            asks for files.shared once;
                                           refused if denied
     sharedread /disk/Programs/Me/data.txt allowed when run from
                                           /disk/Programs/Me/sharedread */
#include <opos.h>

/* Declares files.shared, so a refused read asks the person once. */
OPOS_MANIFEST("Shared Read", "1.0", OPOS_CAP_FILES_SHARED);

int main(int argc, char **argv) {
    if (argc < 2) {
        opos_print("usage: sharedread PATH...\n");
        return 2;
    }
    int refused = 0;
    for (int i = 1; i < argc; i++) {
        long file = opos_open(argv[i], OPOS_O_RDONLY);
        opos_print(argv[i]);
        if (file >= 0) {
            opos_close((int)file);
            opos_print(": allowed\n");
        } else if (file == -OPOS_EACCES) {
            opos_print(": refused\n");
            refused++;
        } else {
            opos_print(": not found or unreadable\n");
        }
    }
    return refused;
}
