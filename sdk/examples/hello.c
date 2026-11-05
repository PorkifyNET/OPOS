/* The smallest OPOS application: greets the account that runs it. */
#include <opos.h>

int main(int argc, char **argv, char **envp) {
    (void)argc;
    (void)envp;
    struct opos_user_info user;
    opos_print("Hello, OPOS!\n");
    opos_print("Program: ");
    opos_print(argv[0]);
    opos_print("\nKernel ABI version: ");
    opos_print_number(opos_abi_version());
    opos_print("\n");
    if (opos_user_info(&user) == 0 && (user.flags & OPOS_USER_ACCOUNT)) {
        opos_print("Running as ");
        opos_print(user.name);
        opos_print((user.flags & OPOS_USER_ADMIN) ? " (administrator)\n"
                                                   : " (standard)\n");
        opos_print("Home: ");
        opos_print(user.home);
        opos_print("\n");
    } else {
        opos_print("Running without an account\n");
    }
    return 0;
}
