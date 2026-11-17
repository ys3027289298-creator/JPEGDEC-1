// SIGFPE locator wrapper (lives outside the repo tree)
#include <execinfo.h>
#include <signal.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>

static void handler(int sig)
{
    void *bt[32];
    int n = backtrace(bt, 32);
    fprintf(stderr, "SIGFPE received, backtrace:\n");
    backtrace_symbols_fd(bt, n, 2);
    _exit(136);
}

// The upstream test main() (renamed to test_main at compile time)
int test_main(int argc, const char **argv);

int main(int argc, char **argv)
{
    struct sigaction sa;
    memset(&sa, 0, sizeof(sa));
    sa.sa_handler = handler;
    sigaction(SIGFPE, &sa, NULL);
    return test_main(argc, (const char **)argv);
}
