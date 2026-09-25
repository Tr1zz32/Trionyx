#include<trionyx.h>
#include<stdio.h>
#include<string.h>
#include<sys/types.h>
#include<time.h>
#include<libgen.h>
#include<unistd.h>
#include<stdlib.h>
int main(int argc, char *argv[]) {
    pid_t ppid = getppid();
    pid_t pid = getpid();
    FILE *k = fopen("/dev/kmsg", "a");
    if (k == NULL) {
        _exit(1);
    }
    if (ppid != 1) {
        abort();
    }
    while (1) {
        freepb();
        if (strcmp(getprop("init.svc.bootanimation"), "2") == 0) {
            break;
        }
        usleep(500000);
    }
    printf("\n");
    fflush(stdout);
    fprintf(k, "[%d]: %s: system bootanimation in progress...\n", pid, basename(argv[0]));
    while (1) {
        printf("\rSystem is booting  —       ");
        fflush(stdout);
        usleep(200000);
        printf("\rSystem is booting  \\       ");
        fflush(stdout);
        usleep(200000);
        printf("\rSystem is booting  |        ");
        fflush(stdout);
        usleep(200000);
        printf("\rSystem is booting  /        ");
        fflush(stdout);
        usleep(200000);
	printf("\rSystem is booting  —        ");
        fflush(stdout);
        usleep(200000);
        printf("\rSystem is booting  \\       ");
        fflush(stdout);
        usleep(200000);
        printf("\rSystem is booting  |        ");
        fflush(stdout);
        usleep(200000);
        printf("\rSystem is booting  /        ");
        fflush(stdout);
        usleep(200000);
        if (strcmp(getprop("service.srv.agetty"), "0") == 0) {
            break;
        }
        freepb();
    }
    printf("\n");
    setprop("init.svc.bootanimation", "0");
    fflush(stdout);
    return 0;
}
