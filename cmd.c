#include "afunix_polyfill.h"
#include <stdio.h>
#include <pthread.h>

void usage(char **argv)
{
    fprintf(stderr,
            "usage: %s <command>\n"
            "   ls      [system|session] \n"
            "           list available channels\n"
            "   connect [system|session]:service:method \n"
            "           each line on stdin sends to the channel\n"
            "           each message received is written as line to stdout \n"
            "           newlines in mesages are NOT filtered\n"
            "   call    [system|session]:service:method call-arg0 call-arg1 ..\n"
            "           send a message containing the remaining arguments, space separated\n"
            "           wait for a message back and print it on stdout (without newline),\n"
            "           then exit\n"
            "   invoke  [system|session]:service:method call-arg0 call-arg1 ..\n"
            "           like call, but doesnt wait for a return message\n"
            "   listen  [system|session]:service:method \n"
            "           the server equivalent of connect\n"
            "   xargs   [system|session]:service:method some command\n"
            "           connect to a channel, for each message call the \n"
            "           remaining args plus the message content as more args.\n"
            "           the processes stdout is sent back to the caller\n"
            ,argv[0]
            );
}


//too lazy to multiplex D:
void *read_thread(void *_1)
{
    int fd = *(int*)_1;
    char buf[AFUNIX_MAX_PACKAGE_SIZE];
    for(;;) {
        int r = recv(fd, buf, sizeof(buf), 0);
        if (r < 1) {
            exit(0);
        }
        printf("%.*s\n", r, buf);
    }
}

int main(int argc, char **argv)
{
    if (argc < 3) {
        usage(argv);
        return 2;
    }

    const char *command = argv[1];
    if (strcmp(command, "ls") == 0) {
        char bla [1024] = {0};
        strcpy(bla,"(cd ");
        if (strcmp(argv[2], "system") == 0) {
            strcat(bla,"/var/run/unixbus/");
        } else if (
                strcmp(argv[2], "session") == 0 ||
                strcmp(argv[2], "call") == 0
                ) {
            strcat(bla,getenv("XDG_RUNTIME_DIR"));
            if (bla[0] == 0) {
                fprintf(stderr, "XDG_RUNTIME_DIR empty or something");
                return 5;
            }
            strcat(bla, "/unixbus/");
        }
        strcat(bla, "&& find -type s | cut -d '.' -f 2 | cut -d '/' -f 2-20 | tr '/' ':' )");
        system(bla);
    } else if (strcmp(command, "connect") == 0 || strcmp(command, "call") == 0 || strcmp(command, "invoke") == 0) {
        int chan = afunix_socket(0);
        if (chan < 0) {
            perror("socket");
            return 1;
        }
        if (afunix_connect(chan, argv[2],  AFUNIX_PATH_CONVENTION|AFUNIX_MAKE_PATH) != 0) {
            perror("connect");
            return 1;
        }

        if (strcmp(command, "call") == 0 || strcmp(command, "invoke") == 0 ) {
            char buf[AFUNIX_MAX_PACKAGE_SIZE] = {0};
            for (int i = 3; argv[i] != 0; i++) {
                strncat(buf, argv[i], AFUNIX_MAX_PACKAGE_SIZE - strlen(buf) - 1);
                strcat(buf, " ");
            }
            strcat(buf, " ");
            send(chan, buf, strlen(buf), 0);

            if (strcmp(command, "call") == 0){
                int r = recv(chan, buf, sizeof(buf), 0);
                printf("%.*s", r, buf);
            }
            afunix_close(chan);
            return 0;

        } else {
            pthread_t t;
            pthread_create(&t, 0, read_thread, &chan);
            char buf[AFUNIX_MAX_PACKAGE_SIZE];
            for(;;) {
                int len = read(fileno(stdin), buf, sizeof(buf));
                if (len < 1)
                    return 0;
                send(chan, buf, len, 0);
            }
        }
    } else if (strcmp(command, "listen") == 0) {
        int chan = afunix_socket(0);
        if (chan < 0) {
            perror("socket");
            return 1;
        }
        if (afunix_bind(chan, argv[2],  AFUNIX_PATH_CONVENTION|AFUNIX_MAKE_PATH) != 0) {
            perror("connect");
            return 1;
        }

        pthread_t t;
        pthread_create(&t, 0, read_thread, &chan);
        char buf[AFUNIX_MAX_PACKAGE_SIZE];
        for(;;) {
            int len = read(fileno(stdin), buf, sizeof(buf));
            if (len < 1)
                return 0;
            send(chan, buf, len, 0);
        }

    } else if (strcmp(command, "xargs") == 0) {
        int chan = afunix_socket(0);
        if (chan < 0) {
            perror("socket");
            return 1;
        }
        if (afunix_bind(chan, argv[2],  AFUNIX_PATH_CONVENTION|AFUNIX_MAKE_PATH) != 0) {
            perror("connect");
            return 1;
        }

        char buf[AFUNIX_MAX_PACKAGE_SIZE];
        for(;;) {
            char buf[AFUNIX_MAX_PACKAGE_SIZE];
            int address;
            int len = afunix_recvfrom(chan, buf, sizeof(buf), 0, &address);
            if (len < 1)
                return 0;

            char cmd[AFUNIX_MAX_PACKAGE_SIZE] = {0};
            for (int i = 3; argv[i] != 0; i++) {
                strncat(cmd, argv[i], AFUNIX_MAX_PACKAGE_SIZE - strlen(buf) - 1);
                strcat(cmd, " ");
            }
            strcat(buf, " ");
            strncat(cmd, buf, len);
            FILE *f = popen(cmd, "r");
            fread(buf, sizeof(buf), 1, f);
            afunix_sendto(chan, buf, strlen(buf), 0, address);
            pclose(f);
        }

    } else {
        usage(argv);
        return 3;
    }


    return 1;
}
