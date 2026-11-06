#include <unistd.h>

int main(void) {
    static const char message[] = "OPOS: real musl libc process passed\n";
    static char path[] = "/tests/musl-pthread-test";
    char *const arguments[] = {path, 0};
    char *const environment[] = {0};
    if (write(STDOUT_FILENO, message, sizeof(message) - 1)
        != (ssize_t)(sizeof(message) - 1)) {
        return 1;
    }
    execve(path, arguments, environment);
    return 2;
}
