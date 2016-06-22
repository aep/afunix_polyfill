#include "afunix_polyfill.h"
#include <stdio.h>

int main(int argc, char **argv)
{
    if (argc < 2) { fprintf(stderr, "usage: %s /path/to/sock\n", argv[0]); return 3;}
#ifdef TEST_SERVER
    int chan = afunix_socket(0);
    if (afunix_bind(chan, argv[1], 0) !=0) {
        perror("bind");
        return 1;
    }
    debug_fprintf(stderr, "server ready\n");

    for(;;) {
        char buf[1024];
        int address;
        int len = afunix_recvfrom(chan, &buf, 1024, 0, &address);
        fprintf(stderr, "%.*s", len, buf);
        afunix_sendto(chan, "ok!\n", 4, 0, address);
    }
    afunix_close(chan);
#endif
#ifdef TEST_CALL
    int chan = afunix_socket(0);
    if (chan < 0) {
        perror("socket");
        return 1;
    }
    if (afunix_connect(chan, argv[1], 0) !=0) {
        perror("connect");
        return 1;
    }
    fprintf(stderr, "client connected\n");

    write(chan, "yo!\n", 4);

    char buf[1024];
    int len = read(chan, &buf, 1024);
    if (len < 1) {
        perror("chan");
        return 0;
    }
    fprintf(stderr, "%.*s", len, buf);
    afunix_close(chan);
#endif
#ifdef TEST_BROADCAST
    int chan = afunix_socket(0);
    if (afunix_bind(chan, argv[1], 0) !=0) {
        perror("bind");
        return 1;
    }
    fprintf(stderr, "server ready\n");
    for(;;) {
        write(chan, "tick!\n", 6);
        sleep(1);
    }
    afunix_close(chan);
#endif
#ifdef TEST_LISTEN
    int chan = afunix_socket(0);
    if (chan < 0) {
        perror("socket");
        return 1;
    }
    if (afunix_connect(chan, argv[1], 0) !=0) {
        perror("connect");
        return 1;
    }
    fprintf(stderr, "client connected\n");

    char buf[1024];
    for(;;){
        int len = recv(chan, &buf, 1024, 0);
        if (len < 1) {
            perror("chan");
            break;
        }
        fprintf(stderr, "%.*s", len, buf);
    }
    afunix_close(chan);
#endif
}

