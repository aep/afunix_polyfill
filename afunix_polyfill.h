#include <stdio.h>
#include <unistd.h>
#include <pthread.h>
#include <stdlib.h>
#include <sys/socket.h>
#include <errno.h>
#include <sys/un.h>
#include <stropts.h>
#include <poll.h>
#include <fcntl.h>

//----------------- API ----------------
int afunix_socket   (int options);
int afunix_bind     (int fd, const char *name, int options);
int afunix_connect  (int fd, const char *name, int options);
int afunix_close    (int fd);
int afunix_recvfrom (int fd, void *buffer, size_t length,  int flags, int *address);
int afunix_sendto   (int fd, void *buffer, size_t length,  int flags, int address);

//----------------- mapping ----------------

#define AFUNIX_POLYFILL_MAX_CONNECTIONS 1024
#define AFUNIX_MAX_PACKAGE_SIZE 1024
#define debug_fprintf(...)
struct afunix_polyfil_t{
    int user[2];
    int address_exchange[2];
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
static struct afunix_polyfil_t *polyfills = 0;

static struct afunix_polyfil_t *mapped_polyfill(int fd);
static void map_polyfill(struct afunix_polyfil_t *pf);
static void unmap_polyfill(struct afunix_polyfil_t * pf);

//------------------- api implementation ----------

static void *polyfill_thread(void*);

//only call from thread exit! (or before thread started)
static void afunix_free_internal(struct afunix_polyfil_t *pf)
{
    close(pf->user[0]);
    close(pf->user[1]);
    close(pf->address_exchange[0]);
    close(pf->address_exchange[1]);
    for (int i = 0; i < AFUNIX_POLYFILL_MAX_CONNECTIONS != 0; i++) {
        if (pf->client_connections[i] != 0)
            close(pf->client_connections[i]);
    }
    free(pf);
}

int afunix_socket (int options)
{
    struct afunix_polyfil_t *pf = calloc(1, sizeof(struct afunix_polyfil_t));
    int ret = socketpair(AF_UNIX, SOCK_DGRAM, 0, pf->user);
    ret     = socketpair(AF_UNIX, SOCK_DGRAM, 0, pf->address_exchange);

    pf->actual = socket(AF_UNIX, SOCK_SEQPACKET, 0);
    if (pf->actual < 0) {
        afunix_free_internal(pf);
        return -errno;
    }

    if (fcntl(pf->actual, F_SETFL, fcntl(pf->actual, F_GETFL, 0) | O_NONBLOCK) == -1) {
        afunix_free_internal(pf);
        return -errno;
    }

    map_polyfill(pf);
    pthread_create (&(pf->thread), 0, polyfill_thread, pf);
    return pf->user[1];
}

int afunix_close (int fd)
{
    struct afunix_polyfil_t * pf = mapped_polyfill(fd);
    if (pf == NULL)
        return EINVAL;
    write(pf->user[1], 0, 0);
    close(pf->user[1]);
    pthread_join(pf->thread,0);
    return 0;
}

int afunix_bind (int fd, const char *name, int options)
{
    struct afunix_polyfil_t * pf = mapped_polyfill(fd);
    if (pf == NULL)
        return EINVAL;

    struct sockaddr_un sa;
    sa.sun_family = AF_UNIX;
    strcpy(sa.sun_path, name);

    //TODO: different cleanup strategy
    unlink(sa.sun_path);

    int len = strlen(sa.sun_path) + sizeof(sa.sun_family);
    if (bind(pf->actual, (struct sockaddr *)&sa, len) != 0)
        return errno;

    if (listen(pf->actual, 0) != 0)
        return errno;

    pf->actual_mode = 2;

    return 0;
}

int afunix_connect (int fd, const char *name, int options)
{
    struct afunix_polyfil_t * pf = mapped_polyfill(fd);
    if (pf == NULL)
        return EINVAL;

    struct sockaddr_un sa;
    sa.sun_family = AF_UNIX;
    strcpy(sa.sun_path, name);

    int len = strlen(sa.sun_path) + sizeof(sa.sun_family);
    if (connect(pf->actual, (struct sockaddr *)&sa, len) != 0)
        return errno;

    pf->actual_mode = 1;
    return 0;
}

int afunix_recvfrom (int fd, void *buffer, size_t length,  int flags, int *address)
{
    struct afunix_polyfil_t * pf = mapped_polyfill(fd);
    if (pf == NULL)
        return EINVAL;
    *address = 0;
    //clear pending exchanges.
    while(recv(pf->address_exchange[1], pf->buf,  sizeof(address), MSG_DONTWAIT)>0){}

    recv(pf->address_exchange[1], address,  sizeof(address), 0);
    return recv(fd, buffer, length, flags);
}

int afunix_sendto   (int fd, void *buffer, size_t length,  int flags, int address)
{
    struct afunix_polyfil_t * pf = mapped_polyfill(fd);
    if (pf == NULL)
        return EINVAL;
    send(pf->address_exchange[1], &address, sizeof(address), flags);
    return send(fd, buffer, length, flags);
}

//------------------- polyfill backend ----------

static void polyfill_backend_actual(struct afunix_polyfil_t *pf)
{
    debug_fprintf(stderr, "> actual (mode:%d)\n", pf->actual_mode);
    if (pf->actual_mode == 1) {
        int len = recv(pf->actual, pf->buf, sizeof(pf->buf), 0);
        debug_fprintf(stderr, "  got %d bytes\n", len);
        int connection = 0;
        send(pf->address_exchange[0], &connection, sizeof(connection), 0);
        len = send(pf->user[0], pf->buf, len, 0);
        debug_fprintf(stderr, "  writen %d into user\n", len);
        if (len < 1) {
            debug_fprintf(stderr, "  closed\n", len);
            pf->exit = 1;
        }
    } else if (pf->actual_mode == 2) {
        struct sockaddr_un clientname;
        int size = sizeof (clientname);
        int new = accept (pf->actual,(struct sockaddr *) &clientname, &size);
        if (fcntl(new, F_SETFL, fcntl(new, F_GETFL, 0) | O_NONBLOCK) == -1) {
            close(new);
            perror("afunix_polyfill: fcntl");
            return;
        }
        for (int i = 0; i < AFUNIX_POLYFILL_MAX_CONNECTIONS != 0; i++) {
            if (pf->client_connections[i] == 0) {
                pf->client_connections[i] = new;
                ++pf->client_connections_count;
                return;
            }
        }
        fprintf(stderr, "afunix_polyfill: too many clients\n");
        close(new);
    } else {
        fprintf(stderr, "BUG! in afunix_polyfill: unknown actual mode\n");
        read(pf->actual, pf->buf, sizeof(pf->buf));
    }
}

static void polyfill_backend_address_exchange(struct afunix_polyfil_t *pf)
{
    debug_fprintf(stderr, "> address_exchange\n");
    recv(pf->address_exchange[0], &pf->next_send_address, sizeof(pf->next_send_address), 0);
}

static void polyfill_backend_user(struct afunix_polyfil_t *pf)
{
    debug_fprintf(stderr, "> user (mode:%d)\n", pf->actual_mode);

    int len = recv(pf->user[0], pf->buf, sizeof(pf->buf), 0);
    debug_fprintf(stderr, "  got %d bytes\n", len);
    if (len == 0) {
        pf->exit = 1;
        return;
    }
    if (pf->actual_mode == 1) {
        len = send(pf->actual, pf->buf, len, 0);
        debug_fprintf(stderr, "  writen %d into actual\n", len);
    } else if (pf->actual_mode == 2) {
        debug_fprintf(stderr, "  target address is %d\n", pf->next_send_address);
        for (int i = 0; i < AFUNIX_POLYFILL_MAX_CONNECTIONS != 0; i++) {
            if (pf->client_connections[i] != 0) {
                if (pf->client_connections[i] ==  pf->next_send_address ||
                         pf->next_send_address  == 0) {
                    send(pf->client_connections[i],  pf->buf, len, 0);
                }
            }
        }
    } else {
        fprintf(stderr, "BUG! in afunix_polyfill: unknown actual mode\n");
    }
}

static void polyfill_backend_connection(struct afunix_polyfil_t *pf, int connection)
{
    debug_fprintf(stderr, "> connection\n");
    int len = recv(connection, pf->buf, sizeof(pf->buf), 0);
    if (len < 0) {
        if (errno = EAGAIN) {
            debug_fprintf(stderr, "  EAGAIN\n");
            return;
        }
        perror("polyfill_backend_connection::read");
    }
    debug_fprintf(stderr, "  got %d bytes\n", len);

    if (len < 1) {
        debug_fprintf(stderr, "  closed\n");
        close(connection);
        for (int i = 0; i < AFUNIX_POLYFILL_MAX_CONNECTIONS != 0; i++) {
            if (pf->client_connections[i] == connection) {
                pf->client_connections[i] = 0;
                pf->client_connections_count -= 1;
            }
        }
        return;
    }
    send(pf->address_exchange[0], &connection, sizeof(connection), 0);
    send(pf->user[0], pf->buf, len, 0);
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

        fds[0].fd = pf->user[0];
        fds[0].events = POLLIN;

        fds[1].fd = pf->address_exchange[0];
        fds[1].events = POLLIN;

        fds[2].fd = pf->actual;
        fds[2].events = POLLIN;

        int connections_count = 0;
        for (int i = 0; i < AFUNIX_POLYFILL_MAX_CONNECTIONS != 0; i++) {
            if (client_connections[i] != 0) {
                fds[3 + connections_count].fd = client_connections[i];
                fds[3 + connections_count].events = POLLIN;
                ++connections_count;
            }
        }

        if (connections_count != pf->client_connections_count) {
            debug_fprintf(stderr, "BUG! in afunix_polyfill: connections_count inconsistent"
                    "marked: %d counted:%d\n", pf->client_connections_count, connections_count);
            abort();
        }

        debug_fprintf(stderr, "- poll\n");
        int ret = poll(fds, numfds, -1);
        debug_fprintf(stderr, "- end\n");
        if (ret < 1) {
            perror("poll");
            continue;
        }

        connections_count = 0;
        for (int i = 0; i < AFUNIX_POLYFILL_MAX_CONNECTIONS != 0; i++) {
            if (client_connections[i] != 0) {
                if (fds[3 + connections_count].events & POLLIN) {
                    debug_fprintf(stderr, "- %d (c %d) is active\n",
                            fds[3 + connections_count].fd, connections_count);
                    polyfill_backend_connection(pf, fds[3 + connections_count].fd);
                }
            }
            ++connections_count;
        }
        if (fds[1].revents & POLLIN) {
            polyfill_backend_address_exchange(pf);
        }
        if (fds[0].revents & POLLIN) {
            polyfill_backend_user(pf);
        }
        if (fds[2].revents & POLLIN) {
            polyfill_backend_actual(pf);
        }
    }
    afunix_free_internal(pf);
}

//------------------- mapping implementation ----------
static struct afunix_polyfil_t *mapped_polyfill(int fd)
{
    struct afunix_polyfil_t *n = polyfills;
    while (n!=0) {
        if (n->user[1] == fd)
            return n;
    }
    return 0;
}

static void map_polyfill(struct afunix_polyfil_t *pf)
{
    if (polyfills == NULL) {
        polyfills = pf;
        return;
    }
    struct afunix_polyfil_t *n = polyfills;
    while (n != 0) {
        if (n->next == NULL) {
            n->next = pf;
        }
    }
    fprintf(stderr, "corrupt map\n");
    abort();
}

static void unmap_polyfill(struct afunix_polyfil_t * pf)
{
    if (polyfills == pf) {
        polyfills = 0;
        return;
    }
    struct afunix_polyfil_t *n = polyfills;
    while (n != 0) {
        if (n->next == pf) {
            n->next = pf->next;
            return;
        }
    }
}

