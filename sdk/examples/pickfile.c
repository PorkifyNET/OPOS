/* Copy this program to /disk/Programs and launch it from Start > Programs.
   Files asks the user to select one document in their home folder. */
#include <opos.h>

int main(void) {
    char path[256];
    long picked = opos_pick_file(path, sizeof(path));
    if (picked == -OPOS_ECANCELED) {
        opos_print("File selection cancelled\n");
        return 0;
    }
    if (picked < 0) {
        opos_print("Could not select a file\n");
        return 1;
    }
    long file = opos_open(path, OPOS_O_RDONLY);
    if (file < 0) {
        opos_print("Could not read selected file\n");
        return 1;
    }
    char sample[65];
    long count = opos_read((int)file, sample, sizeof(sample) - 1);
    opos_close((int)file);
    if (count < 0) {
        opos_print("Read failed\n");
        return 1;
    }
    sample[count] = '\0';
    opos_print("Selected: ");
    opos_print(path);
    opos_print("\nFirst bytes: ");
    opos_print(sample);
    opos_print("\n");
    return 0;
}
