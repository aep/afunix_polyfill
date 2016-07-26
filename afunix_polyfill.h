#include <stdio.h>
#include <unistd.h>
#include <pthread.h>
#include <stdlib.h>
#include <string.h>
#include <sys/socket.h>
#include <errno.h>
#include <sys/un.h>
#include <stropts.h>
#include <poll.h>
#include <fcntl.h>
#include <sys/stat.h>

#ifdef AFUNIX_POLYFILL_INCLUDED
#error "cant include afunix_polyfill.h multiple times. only include in .c/.cpp file"
#endif
#define AFUNIX_POLYFILL_INCLUDED

#ifdef __cplusplus
extern "C" {
#endif

//----------------- API ----------------
#define AFUNIX_PATH_CONVENTION 0x1
#define AFUNIX_MAKE_PATH       0x2

static int afunix_socket   (int options);
static int afunix_bind     (int fd, const char *name, int options);
static int afunix_connect  (int fd, const char *name, int options);
static int afunix_close    (int fd);
static int afunix_recvfrom (int fd, void *buffer, size_t length,  int flags, int *address);
static int afunix_sendto   (int fd, void *buffer, size_t length,  int flags, int address);

//----------------- mapping ----------------

#define AFUNIX_POLYFILL_MAX_CONNECTIONS 1024
#define AFUNIX_MAX_PACKAGE_SIZE 1024

#ifdef AFUNIX_DEBUGGING
#define afunix_debug_printf(...) {fprintf(stderr, "%s", AFUNIX_DEBUGGING); fprintf(stderr, __VA_ARGS__);}
#else
#define afunix_debug_printf(...)
#endif
struct afunix_polyfil_t{
    int user[2];
    int address_exchange[2];
    int address_ack[2];
    pthread_t thread;

    int actual;
    int actual_mode;

    int client_connections[AFUNIX_POLYFILL_MAX_CONNECTIONS];
    int client_connections_count;

    int exit;

    int next_send_address;

    struct afunix_polyfil_t *next;
    char buf[AFUNIX_MAX_PACKAGE_SIZE];
};


//TODO: have a thread safe map instead
static pthread_mutex_t afunix_polyfill_mutex = PTHREAD_MUTEX_INITIALIZER;
static struct afunix_polyfil_t *polyfills = 0;

static struct afunix_polyfil_t *mapped_polyfill(int fd);
static void map_polyfill(struct afunix_polyfil_t *pf);
static void unmap_polyfill(struct afunix_polyfil_t * pf);

//------------------- api implementation ----------

static void *polyfill_thread(void*);

//only call from thread exit! (or before thread started)
static void afunix_free_internal(struct afunix_polyfil_t *pf)
{
    afunix_debug_printf("[%d] closing fds:\n"
            "\tpf->actual = %d\n"
            "\tpf->user[0] = %d\n"
            "\tpf->user[1] = %d\n"
            "\tpf->address_exchange[0] = %d\n"
            "\tpf->address_exchange[1] = %d\n"
            "\tpf->address_ack[0] = %d\n"
            "\tpf->address_ack[1] = %d\n"
            ,pf->user[1]
            ,pf->actual
            ,pf->user[1], pf->user[1],
            pf->address_exchange[0], pf->address_exchange[1],
            pf->address_ack[0], pf->address_ack[1]);

    if (pf->user[0] != 0)
        close(pf->user[0]);
    if (pf->user[1] != 0)
        close(pf->user[1]);
    if (pf->address_exchange[0]  != 0)
        close(pf->address_exchange[0]);
    if (pf->address_exchange[1]  != 0)
        close(pf->address_exchange[1]);
    if (pf->address_ack[0]  != 0)
        close(pf->address_ack[0]);
    if (pf->address_ack[1]  != 0)
        close(pf->address_ack[1]);
    if (pf->actual != 0)
        close(pf->actual);

    for (int i = 0; i < AFUNIX_POLYFILL_MAX_CONNECTIONS; i++) {
        if (pf->client_connections[i] != 0) {
            afunix_debug_printf("\tpf->client_connections[%d] = %d\n",
                    i, pf->client_connections[i]);
            close(pf->client_connections[i]);
        }
    }
    unmap_polyfill(pf);
    free(pf);
}



static struct afunix_polyfil_t *afunix_new_afunix_polyfil_t()
{
    void *mem = malloc(sizeof(struct afunix_polyfil_t));
    memset(mem, 0, sizeof(struct afunix_polyfil_t));
    return (struct afunix_polyfil_t *) mem;
}

static int afunix_socket (int options)
{
    struct afunix_polyfil_t *pf =  afunix_new_afunix_polyfil_t();

    int ret = socketpair(AF_UNIX, SOCK_DGRAM, 0, pf->user);
    if (ret != 0) {
        afunix_free_internal(pf);
        return -errno;
    }
    ret     = socketpair(AF_UNIX, SOCK_DGRAM, 0, pf->address_exchange);
    if (ret != 0) {
        afunix_free_internal(pf);
        return -errno;
    }

    ret     = socketpair(AF_UNIX, SOCK_DGRAM, 0, pf->address_ack);
    if (ret != 0) {
        afunix_free_internal(pf);
        return -errno;
    }

    pf->actual = socket(AF_UNIX, SOCK_SEQPACKET, 0);
    if (pf->actual < 0) {
        afunix_free_internal(pf);
        return -errno;
    }

    afunix_debug_printf("[%d] created fds:\n"
            "\tpf->actual = %d\n"
            "\tpf->user[0] = %d\n"
            "\tpf->user[1] = %d\n"
            "\tpf->address_exchange[0] = %d\n"
            "\tpf->address_exchange[1] = %d\n"
            "\tpf->address_ack[0] = %d\n"
            "\tpf->address_ack[1] = %d\n",
            pf->user[1],
            pf->actual, pf->user[0], pf->user[1],
            pf->address_exchange[0], pf->address_exchange[1],
            pf->address_ack[0], pf->address_ack[1]);

    map_polyfill(pf);
    return pf->user[1];
}

static int afunix_close (int fd)
{
    struct afunix_polyfil_t * pf = mapped_polyfill(fd);
    if (pf == NULL)
        return EINVAL;

    if (pf->actual_mode == 0) {
        //didnt start thread yet
        afunix_free_internal(pf);
        return 0;
    }
    pthread_t thread = pf->thread;
    write(pf->address_exchange[1], 0, 0);
    //dont close. message may be lost. probably race condition
    //close(pf->address_exchange[1]);
    pf->address_exchange[1] = 0;
    pthread_join(thread,0);
    return 0;
}

static void afunix_make_path(struct sockaddr_un *sa, const char *name, int options);

static int afunix_bind (int fd, const char *name, int options)
{
    struct afunix_polyfil_t * pf = mapped_polyfill(fd);
    if (pf == NULL)
        return -EINVAL;

    struct sockaddr_un sa;
    sa.sun_family = AF_UNIX;
    afunix_make_path(&sa, name, options);

    //TODO: different cleanup strategy
    unlink(sa.sun_path);

    int len = strlen(sa.sun_path) + sizeof(sa.sun_family);
    if (bind(pf->actual, (struct sockaddr *)&sa, len) != 0)
        return -errno;

    if (listen(pf->actual, 0) != 0)
        return -errno;

    if (fcntl(pf->actual, F_SETFL, fcntl(pf->actual, F_GETFL, 0) | O_NONBLOCK) == -1) {
        afunix_free_internal(pf);
        return -errno;
    }

    pf->actual_mode = 2;
    pthread_create (&(pf->thread), 0, polyfill_thread, pf);
#ifdef _GNU_SOURCE
    pthread_setname_np(pf->thread, "polyfill_thread");
#endif
    return 0;
}

static int afunix_connect (int fd, const char *name, int options)
{
    struct afunix_polyfil_t * pf = mapped_polyfill(fd);
    if (pf == NULL)
        return -EINVAL;

    struct sockaddr_un sa;
    sa.sun_family = AF_UNIX;
    afunix_make_path(&sa, name, options);

    int len = strlen(sa.sun_path) + sizeof(sa.sun_family);
    if (connect(pf->actual, (struct sockaddr *)&sa, len) != 0)
        return -errno;

    if (fcntl(pf->actual, F_SETFL, fcntl(pf->actual, F_GETFL, 0) | O_NONBLOCK) == -1) {
        afunix_free_internal(pf);
        return -errno;
    }

    pf->actual_mode = 1;
    pthread_create (&(pf->thread), 0, polyfill_thread, pf);
    return 0;
}

static int afunix_recvfrom (int fd, void *buffer, size_t length,  int flags, int *address)
{
    struct afunix_polyfil_t * pf = mapped_polyfill(fd);
    if (pf == NULL)
        return -EINVAL;
    *address = 0;
    //clear pending exchanges. FIXME this is racy
    //while(recv(pf->address_exchange[1], pf->buf,  sizeof(address), MSG_DONTWAIT) > 0){
    //    fprintf(stderr, "WARNING: afunix_recvfrom cleared address_exchange. dont mix recvfrom and recv\n");
    //}

    recv(pf->address_exchange[1], address,  sizeof(address), flags);
    return recv(fd, buffer, length, flags);
}

static int afunix_sendto   (int fd, void *buffer, size_t length,  int flags, int address)
{
    struct afunix_polyfil_t * pf = mapped_polyfill(fd);
    if (pf == NULL)
        return -EINVAL;
    send(pf->address_exchange[1], &address, sizeof(address), flags);
    int r = recv(pf->address_ack[1], &address, sizeof(address), 0);
    if (r != 1) {
        fprintf(stderr, "BUG afunix_sendto address_exchange ack'd with %d instead of 1\n", r);
        perror("recv");
    }
    return send(fd, buffer, length, flags);
}

//------------------- polyfill backend ----------

static void polyfill_backend_connection(struct afunix_polyfil_t *pf, int connection, int revents)
{
    afunix_debug_printf("[%d] connection: %d flags: %x\n", pf->user[1], connection, revents);
    errno = 0;
    int len = recv(connection, pf->buf, sizeof(pf->buf), 0);
    afunix_debug_printf("[%d] \tgot %d bytes\n", pf->user[1], len);
    if (len == 0 ){ // || (len < 0 && (revents & POLLHUP))) {
        afunix_debug_printf("[%d] \tclosed\n", pf->user[1]);
        close(connection);
        for (int i = 0; i < AFUNIX_POLYFILL_MAX_CONNECTIONS; i++) {
            if (pf->client_connections[i] == connection) {
                pf->client_connections[i] = 0;
                pf->client_connections_count -= 1;
            }
        }
        return;
    }
    if (len < 0) {
        if (errno == EAGAIN) {
            afunix_debug_printf("[%d] \tEAGAIN\n", pf->user[1]);
            return;
        }
        perror("polyfill_backend_connection::read");
    }

    send(pf->address_exchange[0], &connection, sizeof(connection), 0);
    send(pf->user[0], pf->buf, len, 0);
}

static void polyfill_backend_actual(struct afunix_polyfil_t *pf)
{
    afunix_debug_printf("[%d] actual (mode:%d)\n", pf->user[1], pf->actual_mode);
    if (pf->actual_mode == 1) {
        int len = recv(pf->actual, pf->buf, sizeof(pf->buf), 0);
        afunix_debug_printf("\tgot %d bytes\n", len);
        int connection = 0;
        send(pf->address_exchange[0], &connection, sizeof(connection), 0);
        len = send(pf->user[0], pf->buf, len, 0);
        afunix_debug_printf("\twriten %d into user\n", len);
        if (len < 0) {
            perror("polyfill_backend_actual::read");
        }
    } else if (pf->actual_mode == 2) {
        struct sockaddr_un clientname;
        unsigned int size = sizeof (clientname);
        int nuw = accept (pf->actual,(struct sockaddr *) &clientname, &size);
        afunix_debug_printf("[%d] \taccept %d\n", pf->user[1], nuw);


        if (fcntl(nuw, F_SETFL, fcntl(nuw, F_GETFL, 0) | O_NONBLOCK) == -1) {
            close(nuw);
            perror("afunix_polyfill: fcntl");
            return;
        }
        for (int i = 0; i < AFUNIX_POLYFILL_MAX_CONNECTIONS; i++) {
            if (pf->client_connections[i] == 0) {
                pf->client_connections[i] = nuw;
                ++pf->client_connections_count;
                return;
            }
        }
        fprintf(stderr, "afunix_polyfill: too many clients\n");
        close(nuw);
    } else {
        fprintf(stderr, "BUG! in afunix_polyfill: unknown actual mode %d\n", pf->actual_mode);
        read(pf->actual, pf->buf, sizeof(pf->buf));
    }
}

static void polyfill_backend_address_exchange(struct afunix_polyfil_t *pf)
{
    afunix_debug_printf("[%d] address_exchange\n", pf->user[1]);
    int ret = recv(pf->address_exchange[0], &pf->next_send_address, sizeof(pf->next_send_address), 0);
    if (ret < sizeof(pf->next_send_address)) {
        afunix_debug_printf("[%d]\taddress_exchange closed. going to exit\n",  pf->user[1]);
        pf->exit = 1;
    }
    send(pf->address_ack[0], "!", 1, 0);
}

static void polyfill_backend_user(struct afunix_polyfil_t *pf)
{
    afunix_debug_printf("[%d] user (mode:%d)\n", pf->user[1], pf->actual_mode);

    int len = recv(pf->user[0], pf->buf, sizeof(pf->buf), 0);
    afunix_debug_printf("[%d]\tgot %d bytes\n",  pf->user[1], len);
    if (len < 1) {
        pf->exit = 1;
        return;
    }
    if (pf->actual_mode == 1) {
        len = send(pf->actual, pf->buf, len, 0);
        afunix_debug_printf("[%d]\twriten %d into actual\n",  pf->user[1],len);
    } else if (pf->actual_mode == 2) {
        afunix_debug_printf("[%d]\ttarget address is %d\n",  pf->user[1], pf->next_send_address);
        for (int i = 0; i < AFUNIX_POLYFILL_MAX_CONNECTIONS; i++) {
            if (pf->client_connections[i] != 0) {
                if (pf->client_connections[i] ==  pf->next_send_address ||
                         pf->next_send_address  == 0) {
                    send(pf->client_connections[i],  pf->buf, len, 0);
                    pf->next_send_address = 0;
                    return;
                }
            }
        }
    } else {
        fprintf(stderr, "BUG! in afunix_polyfill: unknown actual mode %d\n", pf->actual_mode);
    }
}

static void *polyfill_thread(void *_pf)
{
    struct afunix_polyfil_t *pf = (struct afunix_polyfil_t *)_pf;
    for (;;) {
        if (pf->exit) {
            break;
        }

        int client_connections[AFUNIX_POLYFILL_MAX_CONNECTIONS];
        memcpy(client_connections, pf->client_connections,
                sizeof(int) * AFUNIX_POLYFILL_MAX_CONNECTIONS);

        int numfds = 3 + pf->client_connections_count;
        struct pollfd fds[numfds];
        memset(&fds, 0, sizeof(struct pollfd) * numfds);

        fds[0].fd = pf->user[0];
        fds[0].events = POLLIN;

        fds[1].fd = pf->address_exchange[0];
        fds[1].events = POLLIN;

        fds[2].fd = pf->actual;
        fds[2].events = POLLIN;

        int connections_count = 0;
        for (int i = 0; i < AFUNIX_POLYFILL_MAX_CONNECTIONS; i++) {
            if (client_connections[i] != 0) {
                fds[3 + connections_count].fd = client_connections[i];
                fds[3 + connections_count].events = POLLIN;
                ++connections_count;
            }
        }

        if (connections_count != pf->client_connections_count) {
            afunix_debug_printf("BUG! in afunix_polyfill: connections_count inconsistent"
                    "marked: %d counted:%d\n", pf->client_connections_count, connections_count);
            abort();
        }

        afunix_debug_printf("[%d]- poll\n", pf->user[1]);
        int ret = poll(fds, numfds, -1);
        afunix_debug_printf("[%d]- end with %d active\n", pf->user[1], ret);
        if (ret < 1) {
            perror("poll");
            continue;
        }

        int actually_active = 0;

        for (int i = 0; i < connections_count; i++) {
            if (!fds[3 + i].revents) {
                continue;
            }
            int active = fds[3 + i].fd;
            int found = 0;
            for (int i = 0; i < AFUNIX_POLYFILL_MAX_CONNECTIONS; i++) {
                if (client_connections[i] == active) {
                    ++actually_active;
                    afunix_debug_printf("[%d] - %d (c %d) is active\n",
                            pf->user[1], active, i);
                    polyfill_backend_connection(pf, active, fds[3 + i].revents);
                    found = 1;
                    break;
                }
            }
            if (!found) {
                fprintf(stderr, "afunix_polyfill fd %d is not in client_connections\n", active);
            }
        }
        if (fds[0].revents) {
            ++actually_active;
            afunix_debug_printf("[%d] - user is active\n", pf->user[1]);
            polyfill_backend_user(pf);
        }
        if (fds[2].revents) {
            ++actually_active;
            afunix_debug_printf("[%d] - actual is active\n", pf->user[1]);
            polyfill_backend_actual(pf);
        }
        if (fds[1].revents) {
            ++actually_active;
            afunix_debug_printf("[%d] - address_exchange is active\n", pf->user[1]);
            polyfill_backend_address_exchange(pf);
        }
        if (actually_active != ret) {
            fprintf(stderr, "afunix_polyfill BUG active fd not found. will probably busy loop\n");

        }
    }
    afunix_debug_printf("[%d] polythread backend exits\n", pf->user[1]);
    afunix_free_internal(pf);
    return 0;
}

static void afunix_make_path(struct sockaddr_un *sa, const char *name, int options)
{
    if (!(options & AFUNIX_PATH_CONVENTION)) {
        strcpy(sa->sun_path, name);
        return;
    }

    char *name_ = strdup(name);
    char *saveptr;
    char * n = strtok_r(name_, ":", &saveptr);
    if (strcmp(n,name) == 0 || n == 0) {
        strcpy(sa->sun_path, name);
        return;
    }
    if (strcmp(n, "system") == 0) {
        strncpy(sa->sun_path, "/var/run/unixbus", sizeof(sa->sun_path) - 2);
    } else if (strcmp(n, "session") == 0) {
        strncpy(sa->sun_path, getenv("XDG_RUNTIME_DIR"), sizeof(sa->sun_path) - 2);
        strncat(sa->sun_path, "/unixbus/",
                sizeof(sa->sun_path) - strlen(sa->sun_path) - 2);
    }

    for(;;) {
        n = strtok_r(NULL, ":", &saveptr);
        if (n == NULL) {
            break;
        }
        if (options & AFUNIX_MAKE_PATH){
            afunix_debug_printf("mkdir %s\n", sa->sun_path);
            mkdir(sa->sun_path, 0755);
        }
        strcat(sa->sun_path, "/");
        strncat(sa->sun_path, n,
                sizeof(sa->sun_path) - strlen(sa->sun_path) - 2);
    }
    strncat(sa->sun_path, ".seqpacket", sizeof(sa->sun_path) - strlen(sa->sun_path) - 2);
    free(name_);
}

//------------------- mapping implementation ----------
static struct afunix_polyfil_t *mapped_polyfill(int fd)
{
    pthread_mutex_lock(&afunix_polyfill_mutex);
    struct afunix_polyfil_t *n = polyfills;
    while (n!=0) {
        if (n->user[1] == fd) {
            pthread_mutex_unlock(&afunix_polyfill_mutex);
            return n;
        }
        n = n->next;
    }
    pthread_mutex_unlock(&afunix_polyfill_mutex);
    return 0;
}

static void map_polyfill(struct afunix_polyfil_t *pf)
{
    pthread_mutex_lock(&afunix_polyfill_mutex);
    afunix_debug_printf("map %p [%d]\n", pf, pf->user[1]);
    if (polyfills == NULL) {
        afunix_debug_printf("  inserted at 0\n");
        polyfills = pf;
        pthread_mutex_unlock(&afunix_polyfill_mutex);
        return;
    }
    struct afunix_polyfil_t *n = polyfills;
    for(int i = 0; n != 0; i++) {
        afunix_debug_printf("  %d >> %p [%d]\n", i, n, n->user[1]);
        if (n->next == NULL) {
            n->next = pf;
            afunix_debug_printf("  inserted at %d\n", i+1);
            pthread_mutex_unlock(&afunix_polyfill_mutex);
            return;
        }
        if (n == n->next) {
            fprintf(stderr, "polyfill: corrupt map. next is identical.\n");
            abort();
        }
        n = n->next;
    }
    pthread_mutex_unlock(&afunix_polyfill_mutex);
    fprintf(stderr, "polyfill: corrupt map\n");
    abort();
}

static void unmap_polyfill(struct afunix_polyfil_t * pf)
{
    pthread_mutex_lock(&afunix_polyfill_mutex);
    afunix_debug_printf("unmap_polyfill %p [%d]\n", pf, pf->user[1]);
    if (polyfills == pf) {
        afunix_debug_printf("  deleted at 0\n");
        polyfills = polyfills->next;
        pthread_mutex_unlock(&afunix_polyfill_mutex);
        pf->next = NULL;
        return;
    }
    struct afunix_polyfil_t *n = polyfills;
    for (int i = 1; n != 0; i++) {
        afunix_debug_printf("  %d >> %p [%d]\n", i, n, n->user[1]);
        if (n->next == pf) {
            n->next = pf->next;
            afunix_debug_printf("  deleted at %d\n", i);
            pthread_mutex_unlock(&afunix_polyfill_mutex);
            pf->next = NULL;
            return;
        }
        afunix_debug_printf("unmap_polyfill > %p\n", n);
        n = n->next;
    }
    fprintf(stderr, "polyfill: unnmap not found\n");
    pthread_mutex_unlock(&afunix_polyfill_mutex);
}

#ifdef __cplusplus
}
#endif
