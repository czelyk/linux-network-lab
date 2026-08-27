#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <unistd.h>
#include <errno.h>
#include <fcntl.h>

#include <sys/socket.h>
#include <sys/epoll.h>

#include <netinet/in.h>
#include <arpa/inet.h>


#define LISTEN_PORT 5001

#define SERVER_IP   "127.0.0.1"
#define SERVER_PORT 6000

#define MAX_EVENTS 1024

#define FORWARD_BUFFER_SIZE (64 * 1024)
#define SOCKET_BUFFER_SIZE  (4 * 1024 * 1024)


struct io_buffer
{
    char data[FORWARD_BUFFER_SIZE];

    size_t start;
    size_t end;
};


struct connection;


enum endpoint_type
{
    ENDPOINT_LISTENER,
    ENDPOINT_CLIENT,
    ENDPOINT_SERVER
};


struct endpoint
{
    int fd;

    enum endpoint_type type;

    struct connection *conn;
};


struct connection
{
    struct endpoint client;
    struct endpoint server;

    struct io_buffer c2s;
    struct io_buffer s2c;

    int client_read_open;
    int server_read_open;

    int client_write_closed;
    int server_write_closed;
};


/*
 * ------------------------------------------------------------
 * NON-BLOCKING
 * ------------------------------------------------------------
 */

static int set_nonblocking(int fd)
{
    int flags;

    flags = fcntl(
        fd,
        F_GETFL,
        0
    );

    if(flags == -1)
    {
        perror("fcntl F_GETFL");
        return -1;
    }


    if(fcntl(
            fd,
            F_SETFL,
            flags | O_NONBLOCK) == -1)
    {
        perror("fcntl F_SETFL");
        return -1;
    }


    return 0;
}


/*
 * ------------------------------------------------------------
 * SOCKET BUFFER TUNING
 * ------------------------------------------------------------
 */

static int tune_socket_buffers(int fd)
{
    int size;

    size = SOCKET_BUFFER_SIZE;


    if(setsockopt(
            fd,
            SOL_SOCKET,
            SO_RCVBUF,
            &size,
            sizeof(size)) < 0)
    {
        perror("setsockopt SO_RCVBUF");
        return -1;
    }


    if(setsockopt(
            fd,
            SOL_SOCKET,
            SO_SNDBUF,
            &size,
            sizeof(size)) < 0)
    {
        perror("setsockopt SO_SNDBUF");
        return -1;
    }


    return 0;
}


/*
 * ------------------------------------------------------------
 * BUFFER HELPERS
 * ------------------------------------------------------------
 */

static int buffer_empty(
    const struct io_buffer *buf)
{
    return buf->start == buf->end;
}


static size_t buffer_pending(
    const struct io_buffer *buf)
{
    return buf->end - buf->start;
}


static size_t buffer_space(
    const struct io_buffer *buf)
{
    return FORWARD_BUFFER_SIZE - buf->end;
}


static void buffer_reset_if_empty(
    struct io_buffer *buf)
{
    if(buf->start == buf->end)
    {
        buf->start = 0;
        buf->end = 0;
    }
}


static void buffer_compact(
    struct io_buffer *buf)
{
    size_t pending;

    if(buf->start == 0)
        return;


    pending = buffer_pending(buf);


    if(pending > 0)
    {
        memmove(
            buf->data,
            buf->data + buf->start,
            pending
        );
    }


    buf->start = 0;
    buf->end = pending;
}


/*
 * ------------------------------------------------------------
 * EPOLL HELPER
 * ------------------------------------------------------------
 */

static int epoll_modify(
    int epoll_fd,
    struct endpoint *endpoint,
    uint32_t events)
{
    struct epoll_event ev;

    memset(
        &ev,
        0,
        sizeof(ev)
    );


    ev.events = events;
    ev.data.ptr = endpoint;


    if(epoll_ctl(
            epoll_fd,
            EPOLL_CTL_MOD,
            endpoint->fd,
            &ev) < 0)
    {
        perror("epoll_ctl MOD");
        return -1;
    }


    return 0;
}


/*
 * ------------------------------------------------------------
 * CALCULATE EPOLL EVENTS
 * ------------------------------------------------------------
 */

static int update_connection_events(
    int epoll_fd,
    struct connection *conn)
{
    uint32_t client_events;
    uint32_t server_events;


    client_events = 0;
    server_events = 0;


    /*
     * Client'tan okuyabilir miyiz?
     *
     * c2s buffer'da yer varsa EPOLLIN iste.
     */
    if(conn->client_read_open)
    {
        if(buffer_space(&conn->c2s) > 0 ||
           conn->c2s.start > 0)
        {
            client_events |= EPOLLIN;
        }
    }


    /*
     * Server'dan okuyabilir miyiz?
     */
    if(conn->server_read_open)
    {
        if(buffer_space(&conn->s2c) > 0 ||
           conn->s2c.start > 0)
        {
            server_events |= EPOLLIN;
        }
    }


    /*
     * Server'a gönderilecek pending data varsa
     * server fd için EPOLLOUT iste.
     */
    if(!buffer_empty(&conn->c2s))
    {
        server_events |= EPOLLOUT;
    }


    /*
     * Client'a gönderilecek pending data varsa
     * client fd için EPOLLOUT iste.
     */
    if(!buffer_empty(&conn->s2c))
    {
        client_events |= EPOLLOUT;
    }


    /*
     * Connection kapanmalarını da görmek istiyoruz.
     */
    client_events |= EPOLLRDHUP;
    server_events |= EPOLLRDHUP;


    if(epoll_modify(
            epoll_fd,
            &conn->client,
            client_events) < 0)
    {
        return -1;
    }


    if(epoll_modify(
            epoll_fd,
            &conn->server,
            server_events) < 0)
    {
        return -1;
    }


    return 0;
}


/*
 * ------------------------------------------------------------
 * READ INTO FORWARD BUFFER
 * ------------------------------------------------------------
 */

static int receive_into_buffer(
    int fd,
    struct io_buffer *buf,
    const char *name)
{
    ssize_t n;
    size_t space;


    if(buffer_space(buf) == 0 &&
       buf->start > 0)
    {
        buffer_compact(buf);
    }


    space = buffer_space(buf);


    if(space == 0)
    {
        /*
         * Backpressure.
         *
         * Userspace queue dolu.
         * recv() yapmıyoruz.
         */
        return 0;
    }


    n = recv(
        fd,
        buf->data + buf->end,
        space,
        0
    );


    if(n > 0)
    {
        buf->end += (size_t)n;


        printf(
            "%s: received %zd bytes, pending=%zu\n",
            name,
            n,
            buffer_pending(buf)
        );


        return 1;
    }


    if(n == 0)
    {
        /*
         * FIN / orderly shutdown.
         */
        return 2;
    }


    if(errno == EAGAIN ||
       errno == EWOULDBLOCK)
    {
        return 0;
    }


    if(errno == EINTR)
    {
        return 0;
    }


    perror(name);

    return -1;
}


/*
 * ------------------------------------------------------------
 * FLUSH FORWARD BUFFER
 * ------------------------------------------------------------
 */

static int flush_buffer(
    int fd,
    struct io_buffer *buf,
    const char *name)
{
    ssize_t n;
    size_t pending;


    if(buffer_empty(buf))
        return 0;


    pending = buffer_pending(buf);


    n = send(
        fd,
        buf->data + buf->start,
        pending,
        MSG_NOSIGNAL
    );


    if(n > 0)
    {
        buf->start += (size_t)n;


        printf(
            "%s: sent %zd bytes, remaining=%zu\n",
            name,
            n,
            buffer_pending(buf)
        );


        buffer_reset_if_empty(buf);


        return 1;
    }


    if(n == 0)
        return 0;


    if(errno == EAGAIN ||
       errno == EWOULDBLOCK)
    {
        return 0;
    }


    if(errno == EINTR)
    {
        return 0;
    }


    perror(name);

    return -1;
}


/*
 * ------------------------------------------------------------
 * DESTROY CONNECTION
 * ------------------------------------------------------------
 */

static void destroy_connection(
    int epoll_fd,
    struct connection *conn)
{
    if(conn == NULL)
        return;


    printf(
        "Destroying connection client_fd=%d server_fd=%d\n",
        conn->client.fd,
        conn->server.fd
    );


    if(conn->client.fd >= 0)
    {
        epoll_ctl(
            epoll_fd,
            EPOLL_CTL_DEL,
            conn->client.fd,
            NULL
        );

        close(conn->client.fd);
    }


    if(conn->server.fd >= 0)
    {
        epoll_ctl(
            epoll_fd,
            EPOLL_CTL_DEL,
            conn->server.fd,
            NULL
        );

        close(conn->server.fd);
    }


    free(conn);
}


/*
 * ------------------------------------------------------------
 * CREATE SERVER-SIDE CONNECTION
 * ------------------------------------------------------------
 */

static int create_remote_socket(void)
{
    int fd;

    struct sockaddr_in server_addr;


    fd = socket(
        AF_INET,
        SOCK_STREAM,
        0
    );


    if(fd < 0)
    {
        perror("socket remote");
        return -1;
    }


    if(tune_socket_buffers(fd) < 0)
    {
        close(fd);
        return -1;
    }


    memset(
        &server_addr,
        0,
        sizeof(server_addr)
    );


    server_addr.sin_family =
        AF_INET;

    server_addr.sin_port =
        htons(SERVER_PORT);


    if(inet_pton(
            AF_INET,
            SERVER_IP,
            &server_addr.sin_addr) != 1)
    {
        fprintf(
            stderr,
            "Invalid server IP\n"
        );

        close(fd);

        return -1;
    }


    /*
     * Şimdilik connect() blocking.
     *
     * Bir sonraki gelişmiş sürümde bunu da
     * non-blocking connect yapabiliriz.
     */
    if(connect(
            fd,
            (struct sockaddr *)&server_addr,
            sizeof(server_addr)) < 0)
    {
        perror("connect remote");

        close(fd);

        return -1;
    }


    if(set_nonblocking(fd) < 0)
    {
        close(fd);
        return -1;
    }


    return fd;
}


/*
 * ------------------------------------------------------------
 * CREATE CONNECTION OBJECT
 * ------------------------------------------------------------
 */

static struct connection *create_connection(
    int epoll_fd,
    int client_fd)
{
    struct connection *conn;

    struct epoll_event ev;

    int server_fd;


    server_fd = create_remote_socket();

    if(server_fd < 0)
    {
        close(client_fd);
        return NULL;
    }


    if(tune_socket_buffers(client_fd) < 0)
    {
        close(server_fd);
        close(client_fd);
        return NULL;
    }


    if(set_nonblocking(client_fd) < 0)
    {
        close(server_fd);
        close(client_fd);
        return NULL;
    }


    conn = calloc(
        1,
        sizeof(*conn)
    );


    if(conn == NULL)
    {
        perror("calloc connection");

        close(server_fd);
        close(client_fd);

        return NULL;
    }


    conn->client.fd =
        client_fd;

    conn->client.type =
        ENDPOINT_CLIENT;

    conn->client.conn =
        conn;


    conn->server.fd =
        server_fd;

    conn->server.type =
        ENDPOINT_SERVER;

    conn->server.conn =
        conn;


    conn->client_read_open = 1;
    conn->server_read_open = 1;

    conn->client_write_closed = 0;
    conn->server_write_closed = 0;


    /*
     * Client endpoint'i epoll'a ekle.
     */
    memset(
        &ev,
        0,
        sizeof(ev)
    );


    ev.events =
        EPOLLIN |
        EPOLLRDHUP;

    ev.data.ptr =
        &conn->client;


    if(epoll_ctl(
            epoll_fd,
            EPOLL_CTL_ADD,
            client_fd,
            &ev) < 0)
    {
        perror("epoll_ctl ADD client");

        destroy_connection(
            epoll_fd,
            conn
        );

        return NULL;
    }


    /*
     * Server endpoint'i epoll'a ekle.
     */
    memset(
        &ev,
        0,
        sizeof(ev)
    );


    ev.events =
        EPOLLIN |
        EPOLLRDHUP;

    ev.data.ptr =
        &conn->server;


    if(epoll_ctl(
            epoll_fd,
            EPOLL_CTL_ADD,
            server_fd,
            &ev) < 0)
    {
        perror("epoll_ctl ADD server");

        destroy_connection(
            epoll_fd,
            conn
        );

        return NULL;
    }


    printf(
        "New split connection: client_fd=%d server_fd=%d\n",
        client_fd,
        server_fd
    );


    return conn;
}


/*
 * ------------------------------------------------------------
 * PROCESS CLIENT ENDPOINT EVENT
 * ------------------------------------------------------------
 */

static int process_client_event(
    int epoll_fd,
    struct connection *conn,
    uint32_t events)
{
    /*
     * CLIENT -> ACCELERATOR
     */
    if(events & EPOLLIN)
    {
        int result;


        result = receive_into_buffer(
            conn->client.fd,
            &conn->c2s,
            "client recv"
        );


        if(result < 0)
            return -1;


        if(result == 2)
        {
            conn->client_read_open = 0;
        }
    }


    /*
     * ACCELERATOR -> CLIENT
     */
    if(events & EPOLLOUT)
    {
        if(flush_buffer(
                conn->client.fd,
                &conn->s2c,
                "client send") < 0)
        {
            return -1;
        }
    }


    if(events &
       (EPOLLERR | EPOLLHUP))
    {
        return -1;
    }


    /*
     * Client FIN gönderdi.
     */
    if(events & EPOLLRDHUP)
    {
        conn->client_read_open = 0;
    }


    /*
     * Client'tan alınmış bütün data Server'a
     * iletildiyse Server tarafında FIN üret.
     */
    if(!conn->client_read_open &&
       buffer_empty(&conn->c2s) &&
       !conn->server_write_closed)
    {
        shutdown(
            conn->server.fd,
            SHUT_WR
        );

        conn->server_write_closed = 1;
    }


    if(update_connection_events(
            epoll_fd,
            conn) < 0)
    {
        return -1;
    }


    return 0;
}


/*
 * ------------------------------------------------------------
 * PROCESS SERVER ENDPOINT EVENT
 * ------------------------------------------------------------
 */

static int process_server_event(
    int epoll_fd,
    struct connection *conn,
    uint32_t events)
{
    /*
     * SERVER -> ACCELERATOR
     */
    if(events & EPOLLIN)
    {
        int result;


        result = receive_into_buffer(
            conn->server.fd,
            &conn->s2c,
            "server recv"
        );


        if(result < 0)
            return -1;


        if(result == 2)
        {
            conn->server_read_open = 0;
        }
    }


    /*
     * ACCELERATOR -> SERVER
     */
    if(events & EPOLLOUT)
    {
        if(flush_buffer(
                conn->server.fd,
                &conn->c2s,
                "server send") < 0)
        {
            return -1;
        }
    }


    if(events &
       (EPOLLERR | EPOLLHUP))
    {
        return -1;
    }


    /*
     * Server FIN göndermiş.
     */
    if(events & EPOLLRDHUP)
    {
        conn->server_read_open = 0;
    }


    /*
     * Server'dan alınan bütün data Client'a
     * iletildiyse Client tarafında FIN üret.
     */
    if(!conn->server_read_open &&
       buffer_empty(&conn->s2c) &&
       !conn->client_write_closed)
    {
        shutdown(
            conn->client.fd,
            SHUT_WR
        );

        conn->client_write_closed = 1;
    }


    if(update_connection_events(
            epoll_fd,
            conn) < 0)
    {
        return -1;
    }


    return 0;
}


/*
 * ------------------------------------------------------------
 * CONNECTION FINISHED?
 * ------------------------------------------------------------
 */

static int connection_finished(
    const struct connection *conn)
{
    return
        !conn->client_read_open &&
        !conn->server_read_open &&
        buffer_empty(&conn->c2s) &&
        buffer_empty(&conn->s2c);
}


/*
 * ------------------------------------------------------------
 * MAIN
 * ------------------------------------------------------------
 */

int main(void)
{
    int listen_fd;
    int epoll_fd;

    int yes;

    struct sockaddr_in listen_addr;

    struct epoll_event ev;

    struct epoll_event events[MAX_EVENTS];

    struct endpoint listener_endpoint;


    yes = 1;


    /*
     * ============================================================
     * LISTEN SOCKET
     * ============================================================
     */

    listen_fd = socket(
        AF_INET,
        SOCK_STREAM,
        0
    );


    if(listen_fd < 0)
    {
        perror("socket listen");
        return EXIT_FAILURE;
    }


    if(setsockopt(
            listen_fd,
            SOL_SOCKET,
            SO_REUSEADDR,
            &yes,
            sizeof(yes)) < 0)
    {
        perror("setsockopt SO_REUSEADDR");

        close(listen_fd);

        return EXIT_FAILURE;
    }


    if(set_nonblocking(listen_fd) < 0)
    {
        close(listen_fd);
        return EXIT_FAILURE;
    }


    memset(
        &listen_addr,
        0,
        sizeof(listen_addr)
    );


    listen_addr.sin_family =
        AF_INET;

    listen_addr.sin_addr.s_addr =
        htonl(INADDR_ANY);

    listen_addr.sin_port =
        htons(LISTEN_PORT);


    if(bind(
            listen_fd,
            (struct sockaddr *)&listen_addr,
            sizeof(listen_addr)) < 0)
    {
        perror("bind");

        close(listen_fd);

        return EXIT_FAILURE;
    }


    if(listen(
            listen_fd,
            SOMAXCONN) < 0)
    {
        perror("listen");

        close(listen_fd);

        return EXIT_FAILURE;
    }


    /*
     * ============================================================
     * EPOLL INSTANCE
     * ============================================================
     */

    epoll_fd = epoll_create1(0);


    if(epoll_fd < 0)
    {
        perror("epoll_create1");

        close(listen_fd);

        return EXIT_FAILURE;
    }


    /*
     * Listener için endpoint wrapper.
     */
    listener_endpoint.fd =
        listen_fd;

    listener_endpoint.type =
        ENDPOINT_LISTENER;

    listener_endpoint.conn =
        NULL;


    memset(
        &ev,
        0,
        sizeof(ev)
    );


    ev.events =
        EPOLLIN;

    ev.data.ptr =
        &listener_endpoint;


    if(epoll_ctl(
            epoll_fd,
            EPOLL_CTL_ADD,
            listen_fd,
            &ev) < 0)
    {
        perror("epoll_ctl ADD listener");

        close(epoll_fd);
        close(listen_fd);

        return EXIT_FAILURE;
    }


    printf(
        "Multi-client TCP accelerator listening on 0.0.0.0:%d\n",
        LISTEN_PORT
    );


    /*
     * ============================================================
     * MAIN EVENT LOOP
     * ============================================================
     */

    while(1)
    {
        int ready;
        int i;


        ready = epoll_wait(
            epoll_fd,
            events,
            MAX_EVENTS,
            -1
        );


        if(ready < 0)
        {
            if(errno == EINTR)
                continue;

            perror("epoll_wait");

            break;
        }


        for(i = 0; i < ready; i++)
        {
            struct endpoint *endpoint;


            endpoint =
                events[i].data.ptr;


            /*
             * ----------------------------------------------------
             * NEW CLIENT
             * ----------------------------------------------------
             */
            if(endpoint->type ==
               ENDPOINT_LISTENER)
            {
                while(1)
                {
                    int client_fd;


                    client_fd = accept(
                        listen_fd,
                        NULL,
                        NULL
                    );


                    if(client_fd < 0)
                    {
                        if(errno == EAGAIN ||
                           errno == EWOULDBLOCK)
                        {
                            break;
                        }


                        if(errno == EINTR)
                            continue;


                        perror("accept");

                        break;
                    }


                    create_connection(
                        epoll_fd,
                        client_fd
                    );
                }


                continue;
            }


            /*
             * Endpoint artık bir connection'a ait.
             */
            if(endpoint->conn == NULL)
                continue;


            /*
             * Dikkat:
             *
             * process fonksiyonu connection'ı henüz free etmiyor.
             * Önce event'i işliyoruz.
             */
            if(endpoint->type ==
               ENDPOINT_CLIENT)
            {
                if(process_client_event(
                        epoll_fd,
                        endpoint->conn,
                        events[i].events) < 0)
                {
                    destroy_connection(
                        epoll_fd,
                        endpoint->conn
                    );

                    continue;
                }
            }


            else if(endpoint->type ==
                    ENDPOINT_SERVER)
            {
                if(process_server_event(
                        epoll_fd,
                        endpoint->conn,
                        events[i].events) < 0)
                {
                    destroy_connection(
                        epoll_fd,
                        endpoint->conn
                    );

                    continue;
                }
            }


            /*
             * İki yön de bitti mi?
             */
            if(connection_finished(
                    endpoint->conn))
            {
                destroy_connection(
                    epoll_fd,
                    endpoint->conn
                );
            }
        }
    }


    close(epoll_fd);
    close(listen_fd);


    return EXIT_SUCCESS;
}