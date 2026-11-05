/*
 * Checks the file permission rules from a program's point of view.
 * Prints one line per check and exits with 0 when every check passes.
 * Run it from an installed OPOS (it needs /disk and an account).
 */
#include <opos.h>

static int failures;

static void check(const char *what, int ok) {
    opos_print(ok ? "PASS " : "FAIL ");
    opos_print(what);
    opos_print("\n");
    if (!ok) {
        failures++;
    }
}

static void join(char *out, const char *a, const char *b) {
    size_t n = 0;
    for (size_t i = 0; a[i] != '\0'; i++) {
        out[n++] = a[i];
    }
    for (size_t i = 0; b[i] != '\0'; i++) {
        out[n++] = b[i];
    }
    out[n] = '\0';
}

int main(void) {
    struct opos_user_info user;
    if (opos_user_info(&user) != 0 || !(user.flags & OPOS_USER_ACCOUNT)) {
        opos_print("permtest needs a logged-in account\n");
        return 2;
    }

    check("password file is unreadable",
          opos_open("/disk/OPOS/ACCOUNTS.DAT", OPOS_O_RDONLY) == -OPOS_EACCES);
    check("password file, other spelling",
          opos_open("/disk/opos/accounts.dat", OPOS_O_RDONLY) == -OPOS_EACCES);
    char sneaky[200];
    join(sneaky, user.home, "/../../OPOS/ACCOUNTS.DAT");
    check("password file through ..",
          opos_open(sneaky, OPOS_O_RDONLY) == -OPOS_EACCES);
    check("settings file cannot be written",
          opos_open("/disk/OPOS.CFG", OPOS_O_WRONLY) == -OPOS_EACCES);
    long settings = opos_open("/disk/OPOS.CFG", OPOS_O_RDONLY);
    check("settings file can be read", settings >= 0);
    if (settings >= 0) {
        opos_close((int)settings);
    }
    check("no new files in /disk/boot",
          opos_open("/disk/boot/x.txt", OPOS_O_WRONLY | OPOS_O_CREAT)
              == -OPOS_EACCES);
    check("kernel cannot be deleted",
          opos_unlink("/disk/boot/kernel.elf") == -OPOS_EACCES);
    check("Users folder cannot be removed",
          opos_rmdir("/disk/Users") == -OPOS_EACCES);
    check("no folders for others in /disk/Users",
          opos_mkdir("/disk/Users/intruder") == -OPOS_EACCES);
    check("own home cannot be removed",
          opos_rmdir(user.home) == -OPOS_EACCES);

    char file[200];
    join(file, user.home, "/permtest.txt");
    long fd = opos_open(file, OPOS_O_WRONLY | OPOS_O_CREAT | OPOS_O_TRUNC);
    check("can create a file in own home", fd >= 0);
    if (fd >= 0) {
        check("can write it", opos_write((int)fd, "ok\n", 3) == 3);
        opos_close((int)fd);
        check("can delete it", opos_unlink(file) == 0);
    }

    fd = opos_open("/disk/permtest.txt", OPOS_O_WRONLY | OPOS_O_CREAT);
    check("can create a shared file", fd >= 0);
    if (fd >= 0) {
        opos_close((int)fd);
        check("can delete a shared file",
              opos_unlink("/disk/permtest.txt") == 0);
    }

    opos_print(failures == 0 ? "ALL CHECKS PASSED\n" : "SOME CHECKS FAILED\n");
    return failures == 0 ? 0 : 1;
}
