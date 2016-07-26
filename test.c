#define AFUNIX_DEBUGGING ". "
#define _GNU_SOURCE 
#include "afunix_polyfill.h"
#include <stdio.h>
#include <unistd.h>
#include <signal.h>
#include <pthread.h>

#define TEST_PATH "session:unixbus-test"

static int total_must  = 0;
static int passed_must = 0;
#define MM { __attribute__((constructor)) void initialize(void) {  total_must++; } }
#define TEST_MUST2(assertion, description) \
    MM \
    if ((assertion)) { \
        ++passed_must; \
        fprintf(stderr, "[PASS] %s:%d %s\n",  __func__, __LINE__, description); \
    } else {\
        fprintf(stderr, "[FAIL] %s:%d %s\n",  __func__, __LINE__, description); \
        fflush(stderr); \
        abort(); \
    }
#define TEST_MUST1(assertion) \
    TEST_MUST2(assertion, #assertion)
#define TEST_MUST0(description) \
    TEST_MUST2(1, description)



static int is_server_up = 0;
static int is_server_ends = 0;
#define SPIN(lock) \
  { while(lock == 0) {usleep(10000); } usleep(10000);}


void *echo_server(void*_)
{
    int chan = afunix_socket(0);
    int r =  afunix_bind(chan, TEST_PATH ":echo", 0);
    TEST_MUST1(r == 0);

    fprintf(stderr, "server ready\n");
    is_server_up = 1;

    char buf[1024];
    int address;
    int len = afunix_recvfrom(chan, &buf, 1024, 0, &address);
    TEST_MUST2(len>0,"first message");
    fprintf(stderr, "server received '%.*s' from %d\n", len, buf, address);
    afunix_sendto(chan, "first!",6 , 0, address);

    len = afunix_recvfrom(chan, &buf, 1024, 0, &address);
    TEST_MUST2(len>0,"second message");
    fprintf(stderr, "server received '%.*s' from %d\n", len, buf, address);
    afunix_sendto(chan, "sec!", 4, 0, address);

    TEST_MUST0("server end");
    usleep(2000);
    is_server_ends = 1;

    afunix_close(chan);
    TEST_MUST0("server closed");
    return 0;
}

void *test_call1(void*_)
{
    int chan = afunix_socket(0);
    TEST_MUST2(chan > 0, "afunix_socket");

    SPIN(is_server_up);
    int r = afunix_connect(chan, TEST_PATH ":echo", 0);
    TEST_MUST2(r == 0, "connected");

    r = write(chan, "foo", 3);
    TEST_MUST1(r == 3);

    char buf[1024];
    int len = read(chan, &buf, 1024);
    TEST_MUST1(len > 0);

    fprintf(stderr, "client1 received '%.*s'\n", len, buf);
    TEST_MUST0("client end");
    afunix_close(chan);
    TEST_MUST0("client closed");
    return 0;
}

void *test_call2(void*_)
{
    int chan = afunix_socket(0);
    TEST_MUST2(chan > 0, "afunix_socket");

    SPIN(is_server_up);
    int r = afunix_connect(chan, TEST_PATH ":echo", 0);
    TEST_MUST2(r == 0, "connected");
    r = write(chan, "bar", 3);
    TEST_MUST1(r == 3);


    char buf[1024];
    int len = read(chan, &buf, 1024);
    TEST_MUST1(len > 0);

    fprintf(stderr, "client2 received '%.*s'\n", len, buf);
    TEST_MUST0("client end");
    afunix_close(chan);
    TEST_MUST0("client closed");
    return 0;
}


void test_map()
{
    struct afunix_polyfil_t *pf1 = (struct afunix_polyfil_t *)calloc(1, sizeof(struct afunix_polyfil_t));
    pf1->user[1] = 1;

    struct afunix_polyfil_t *pf2 = (struct afunix_polyfil_t *)calloc(1, sizeof(struct afunix_polyfil_t));
    pf2->user[1] = 2;

    struct afunix_polyfil_t *pf3 = (struct afunix_polyfil_t *)calloc(1, sizeof(struct afunix_polyfil_t));
    pf3->user[1] = 3;

    map_polyfill(pf1);
    unmap_polyfill(pf1);

    map_polyfill(pf2);
    map_polyfill(pf1);
    unmap_polyfill(pf1);
    unmap_polyfill(pf2);

    map_polyfill(pf1);
    map_polyfill(pf2);
    unmap_polyfill(pf1);
    unmap_polyfill(pf2);

    map_polyfill(pf1);
    map_polyfill(pf3);
    unmap_polyfill(pf1);
    map_polyfill(pf2);
    unmap_polyfill(pf3);
    unmap_polyfill(pf2);

    map_polyfill(pf2);
    map_polyfill(pf3);
    map_polyfill(pf1);
    unmap_polyfill(pf3);
    unmap_polyfill(pf1);
    unmap_polyfill(pf2);


    free(pf1);
    free(pf2);
    free(pf3);
}


void testset_summary(int ok)
{
    if (passed_must == total_must && ok) {
        fprintf(stderr, "[PASS] %d/%d checkpoints passed\n", passed_must, total_must);
    } else {
        fprintf(stderr, "[FAIL] %d/%d checkpoints passed\n", passed_must, total_must);
        exit(3);
    }
}


void timeout(int sig)
{
    fprintf(stderr, "!!timeout\n");
    testset_summary(0);
    exit(SIGALRM);
}


int main(int argc, char **argv)
{
    signal(SIGALRM, timeout);
    alarm(10);

    for (int i = 0; i < 50; i++) {
        is_server_up = 0;
        is_server_ends = 0;
        passed_must = 0;

        pthread_t t1;
        pthread_t t2;
        pthread_create(&t1, 0, echo_server, 0);
        pthread_setname_np(t1, "echo_server");
        pthread_create(&t2, 0, test_call1, 0);
        pthread_setname_np(t2, "test_call1");

        test_call2(0);

        pthread_join(t1, 0);
        pthread_join(t2, 0);

        testset_summary(1);

        fprintf(stderr, "\n\n\n\n\n");

        //FIXME malloc dies otherwise. some more thread stuff?
        usleep(10);
    }
    test_map();


    return 0;

#ifdef TEST_CALL
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

