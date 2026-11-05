#include <signal.h>
#include <unistd.h>

static volatile sig_atomic_t handled_signal;

static void handle_signal(int signal_number) {
    static const char entered[] = "OPOS: musl signal handler entered\n";
    handled_signal = signal_number;
    (void)write(1, entered, sizeof(entered) - 1);
}

int main(void) {
    static const char started[] = "OPOS: musl signal test started\n";
    static const char returned[] = "OPOS: musl signal raise returned\n";
    static const char passed[] = "OPOS: real musl signal passed\n";
    struct sigaction action = {0};
    action.sa_handler = handle_signal;
    sigemptyset(&action.sa_mask);
    if (write(1, started, sizeof(started) - 1) != sizeof(started) - 1
        || sigaction(SIGUSR1, &action, 0) != 0) {
        return 1;
    }
    int raised = raise(SIGUSR1);
    (void)write(1, returned, sizeof(returned) - 1);
    if (raised != 0 || handled_signal != SIGUSR1) {
        return 1;
    }
    return write(1, passed, sizeof(passed) - 1) == sizeof(passed) - 1 ? 0 : 1;
}
