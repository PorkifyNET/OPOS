#include <signal.h>
#include <unistd.h>

static volatile sig_atomic_t handled_signal;
static volatile sig_atomic_t info_signal;
static volatile sig_atomic_t info_context;

static void handle_signal(int signal_number) {
    static const char entered[] = "OPOS: musl signal handler entered\n";
    handled_signal = signal_number;
    (void)write(1, entered, sizeof(entered) - 1);
}

/* SA_SIGINFO: siginfo_t with the signal number, and a context pointer. */
static void handle_info(int signal_number, siginfo_t *info, void *context) {
    info_signal = info != 0 && info->si_signo == signal_number
        ? signal_number : -1;
    info_context = context != 0;
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
    struct sigaction info_action = {0};
    info_action.sa_sigaction = handle_info;
    info_action.sa_flags = SA_SIGINFO;
    sigemptyset(&info_action.sa_mask);
    if (sigaction(SIGUSR2, &info_action, 0) != 0 || raise(SIGUSR2) != 0
        || info_signal != SIGUSR2 || !info_context) {
        return 1;
    }
    return write(1, passed, sizeof(passed) - 1) == sizeof(passed) - 1 ? 0 : 1;
}
