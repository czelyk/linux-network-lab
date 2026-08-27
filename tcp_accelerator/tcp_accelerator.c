#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <unistd.h>
#include <errno.h>
#include <fcntl.h>

#include <sys/socket.h>
#include <sys/poll.h>

#include <netinet/in.h>
#include <arpa/inet.h>


#define LISTEN_PORT 5001
#define SERVER_PORT 6000
#define SERVER_IP   "127.0.0.1"

#define SOCKET_BUFFER_SIZE   (4 * 1024 * 1024)
#define FORWARD_BUFFER_SIZE  (64 * 1024)


struct io_buffer
{
    char data[FORWARD_BUFFER_SIZE];

    size_t start;
    size_t end;
};


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


static int receive_into_buffer(
    int fd,
    struct io_buffer *buf,
    const char *name)
{
    ssize_t n;
    size_t space;

    /*
     * Eğer buffer'ın sonunda yer kalmadıysa ama
     * baş tarafında gönderilmiş data nedeniyle boşluk varsa
     * compact etmeyi dene.
     */
    if(buffer_space(buf) == 0 &&
       buf->start > 0)
    {
        buffer_compact(buf);
    }

    space = buffer_space(buf);

    /*
     * Userspace queue tamamen dolu.
     *
     * Bu durumda artık recv() yapmıyoruz.
     * Böylece TCP backpressure doğal şekilde oluşacak.
     */
    if(space == 0)
        return 0;

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
        printf(
            "%s: peer performed orderly shutdown\n",
            name
        );

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
        0
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
    {
        return 0;
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


int main(void)
{
    int listen_fd;
    int client_fd;
    int server_fd;

    int yes;

    int client_read_open;
    int server_read_open;

    struct sockaddr_in accelerator_addr;
    struct sockaddr_in server_addr;

    struct pollfd fds[2];

    struct io_buffer c2s;
    struct io_buffer s2c;


    yes = 1;

    client_read_open = 1;
    server_read_open = 1;

    memset(
        &c2s,
        0,
        sizeof(c2s)
    );

    memset(
        &s2c,
        0,
        sizeof(s2c)
    );


    /*
     * --------------------------------------------------
     * 1. Listening socket
     * --------------------------------------------------
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


    memset(
        &accelerator_addr,
        0,
        sizeof(accelerator_addr)
    );

    accelerator_addr.sin_family =
        AF_INET;

    accelerator_addr.sin_addr.s_addr =
        htonl(INADDR_ANY);

    accelerator_addr.sin_port =
        htons(LISTEN_PORT);


    if(bind(
            listen_fd,
            (struct sockaddr *)&accelerator_addr,
            sizeof(accelerator_addr)) < 0)
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


    printf(
        "Accelerator listening on 0.0.0.0:%d\n",
        LISTEN_PORT
    );


    /*
     * --------------------------------------------------
     * 2. Client accept
     * --------------------------------------------------
     */
    client_fd = accept(
        listen_fd,
        NULL,
        NULL
    );

    if(client_fd < 0)
    {
        perror("accept");

        close(listen_fd);

        return EXIT_FAILURE;
    }


    printf(
        "Client connected. client_fd=%d\n",
        client_fd
    );


    if(tune_socket_buffers(client_fd) < 0)
    {
        close(client_fd);
        close(listen_fd);

        return EXIT_FAILURE;
    }


    /*
     * --------------------------------------------------
     * 3. Server-side socket
     * --------------------------------------------------
     */
    server_fd = socket(
        AF_INET,
        SOCK_STREAM,
        0
    );

    if(server_fd < 0)
    {
        perror("socket server");

        close(client_fd);
        close(listen_fd);

        return EXIT_FAILURE;
    }


    if(tune_socket_buffers(server_fd) < 0)
    {
        close(server_fd);
        close(client_fd);
        close(listen_fd);

        return EXIT_FAILURE;
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
            "Invalid server IP: %s\n",
            SERVER_IP
        );

        close(server_fd);
        close(client_fd);
        close(listen_fd);

        return EXIT_FAILURE;
    }


    printf(
        "Connecting to real server %s:%d...\n",
        SERVER_IP,
        SERVER_PORT
    );


    if(connect(
            server_fd,
            (struct sockaddr *)&server_addr,
            sizeof(server_addr)) < 0)
    {
        perror("connect server");

        close(server_fd);
        close(client_fd);
        close(listen_fd);

        return EXIT_FAILURE;
    }


    printf(
        "Connected to real server.\n"
    );


    /*
     * connect() blocking olarak tamamlandıktan sonra
     * iki connected socket'i non-blocking yapıyoruz.
     */
    if(set_nonblocking(client_fd) < 0)
    {
        close(server_fd);
        close(client_fd);
        close(listen_fd);

        return EXIT_FAILURE;
    }


    if(set_nonblocking(server_fd) < 0)
    {
        close(server_fd);
        close(client_fd);
        close(listen_fd);

        return EXIT_FAILURE;
    }


    /*
     * --------------------------------------------------
     * 4. poll setup
     * --------------------------------------------------
     */
    memset(
        fds,
        0,
        sizeof(fds)
    );

    fds[0].fd = client_fd;
    fds[1].fd = server_fd;


    printf(
        "Non-blocking accelerator forwarding traffic...\n"
    );


    /*
     * --------------------------------------------------
     * 5. Event loop
     * --------------------------------------------------
     */
    while(1)
    {
        int ready;


        /*
         * Her iteration'da hangi event'lere ihtiyacımız
         * olduğunu yeniden hesaplıyoruz.
         */
        fds[0].events = 0;
        fds[1].events = 0;


        /*
         * Client'tan data okumak için c2s buffer'da
         * boş yer olması gerekir.
         */
        if(client_read_open)
        {
            if(buffer_space(&c2s) > 0 ||
               c2s.start > 0)
            {
                fds[0].events |= POLLIN;
            }
        }


        /*
         * Server'dan data okumak için s2c buffer'da
         * boş yer olması gerekir.
         */
        if(server_read_open)
        {
            if(buffer_space(&s2c) > 0 ||
               s2c.start > 0)
            {
                fds[1].events |= POLLIN;
            }
        }


        /*
         * Server'a gönderilecek data varsa
         * server socket writable event'i iste.
         */
        if(!buffer_empty(&c2s))
        {
            fds[1].events |= POLLOUT;
        }


        /*
         * Client'a gönderilecek data varsa
         * client socket writable event'i iste.
         */
        if(!buffer_empty(&s2c))
        {
            fds[0].events |= POLLOUT;
        }


        ready = poll(
            fds,
            2,
            -1
        );


        if(ready < 0)
        {
            if(errno == EINTR)
                continue;

            perror("poll");
            break;
        }


        /*
         * --------------------------------------------------
         * CLIENT -> ACCELERATOR
         * --------------------------------------------------
         */
        if(client_read_open &&
           (fds[0].revents & POLLIN))
        {
            int result;

            result = receive_into_buffer(
                client_fd,
                &c2s,
                "client recv"
            );

            if(result < 0)
            {
                break;
            }

            if(result == 2)
            {
                client_read_open = 0;

                /*
                 * c2s buffer hemen boş olmak zorunda değil.
                 *
                 * Önce pending data server'a aktarılmalı.
                 * FIN'i biraz sonra, buffer tamamen boşalınca
                 * propagate edeceğiz.
                 */
            }
        }


        /*
         * --------------------------------------------------
         * SERVER -> ACCELERATOR
         * --------------------------------------------------
         */
        if(server_read_open &&
           (fds[1].revents & POLLIN))
        {
            int result;

            result = receive_into_buffer(
                server_fd,
                &s2c,
                "server recv"
            );

            if(result < 0)
            {
                break;
            }

            if(result == 2)
            {
                server_read_open = 0;
            }
        }


        /*
         * --------------------------------------------------
         * ACCELERATOR -> SERVER
         * --------------------------------------------------
         */
        if(fds[1].revents & POLLOUT)
        {
            if(flush_buffer(
                    server_fd,
                    &c2s,
                    "server send") < 0)
            {
                break;
            }
        }


        /*
         * --------------------------------------------------
         * ACCELERATOR -> CLIENT
         * --------------------------------------------------
         */
        if(fds[0].revents & POLLOUT)
        {
            if(flush_buffer(
                    client_fd,
                    &s2c,
                    "client send") < 0)
            {
                break;
            }
        }


        /*
         * --------------------------------------------------
         * FIN propagation
         * --------------------------------------------------
         *
         * Client FIN göndermişse ve client'tan alınmış
         * bütün data server'a iletildiyse artık server_fd'nin
         * write side'ını kapatabiliriz.
         */
        if(!client_read_open &&
           buffer_empty(&c2s))
        {
            shutdown(
                server_fd,
                SHUT_WR
            );
        }


        /*
         * Server FIN göndermişse ve server'dan alınmış
         * bütün data client'a iletildiyse client tarafında
         * FIN oluştur.
         */
        if(!server_read_open &&
           buffer_empty(&s2c))
        {
            shutdown(
                client_fd,
                SHUT_WR
            );
        }


        /*
         * --------------------------------------------------
         * Error handling
         * --------------------------------------------------
         */
        if(fds[0].revents &
           (POLLERR | POLLNVAL))
        {
            fprintf(
                stderr,
                "Client socket error\n"
            );

            break;
        }


        if(fds[1].revents &
           (POLLERR | POLLNVAL))
        {
            fprintf(
                stderr,
                "Server socket error\n"
            );

            break;
        }


        /*
         * --------------------------------------------------
         * Connection completely finished?
         * --------------------------------------------------
         */
        if(!client_read_open &&
           !server_read_open &&
           buffer_empty(&c2s) &&
           buffer_empty(&s2c))
        {
            printf(
                "Both directions completed.\n"
            );

            break;
        }
    }


    /*
     * --------------------------------------------------
     * 6. Cleanup
     * --------------------------------------------------
     */
    close(server_fd);
    close(client_fd);
    close(listen_fd);


    printf(
        "Accelerator stopped.\n"
    );


    return EXIT_SUCCESS;
}