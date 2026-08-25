#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <unistd.h>
#include <errno.h>
#include <fcntl.h>

#include <sys/socket.h>
#include <sys/epoll.h>
#include <netinet/in.h>

#define PORT 5001
#define MAX_EVENTS 1024
#define BUF_SIZE 4096

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
        flags | O_NONBLOCK
    ) == -1)
    {
        perror("fcntl F_SETFL");
        return -1;
    }

    return 0;
}

int main(void)
{
    int listen_fd;
    int epoll_fd;
    int yes = 1;

    struct sockaddr_in addr;

    struct epoll_event ev;
    struct epoll_event events[MAX_EVENTS];

    listen_fd = socket(
        AF_INET,
        SOCK_STREAM,
        0
    );

    if(listen_fd < 0)
    {
        perror("socket");
        return EXIT_FAILURE;
    }

    printf(
        "Listening socket created, fd=%d\n",
        listen_fd
    );

    if(setsockopt(
            listen_fd,
            SOL_SOCKET,
            SO_REUSEADDR,
            &yes,
            sizeof(yes)) < 0)
    {
        perror("setsockopt");

        close(listen_fd);
        return EXIT_FAILURE;
    }

    memset(
        &addr,
        0,
        sizeof(addr)
    );

    addr.sin_family = AF_INET;

    addr.sin_addr.s_addr =
        htonl(INADDR_ANY);

    addr.sin_port =
        htons(PORT);

    if(bind(
            listen_fd,
            (struct sockaddr *)&addr,
            sizeof(addr)) < 0)
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

    if(set_nonblocking(listen_fd) < 0)
    {
        close(listen_fd);
        return EXIT_FAILURE;
    }

    epoll_fd = epoll_create1(0);

    if(epoll_fd < 0)
    {
        perror("epoll_create1");

        close(listen_fd);
        return EXIT_FAILURE;
    }

    memset(
        &ev,
        0,
        sizeof(ev)
    );

    ev.events = EPOLLIN;
    ev.data.fd = listen_fd;

    if(epoll_ctl(
            epoll_fd,
            EPOLL_CTL_ADD,
            listen_fd,
            &ev) < 0)
    {
        perror("epoll_ctl listen");

        close(epoll_fd);
        close(listen_fd);

        return EXIT_FAILURE;
    }

    printf(
        "Server listening on 0.0.0.0:%d\n",
        PORT
    );

    while(1)
    {
        int ready;

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

        for(int i = 0; i < ready; i++)
        {
            int fd;

            fd = events[i].data.fd;

            /*
             * Yeni connection olayı.
             */
            if(fd == listen_fd)
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

                        perror("accept");
                        break;
                    }

                    printf(
                        "New client accepted, fd=%d\n",
                        client_fd
                    );

                    if(set_nonblocking(client_fd) < 0)
                    {
                        close(client_fd);
                        continue;
                    }

                    memset(
                        &ev,
                        0,
                        sizeof(ev)
                    );

                    ev.events =
                        EPOLLIN |
                        EPOLLRDHUP;

                    ev.data.fd =
                        client_fd;

                    if(epoll_ctl(
                            epoll_fd,
                            EPOLL_CTL_ADD,
                            client_fd,
                            &ev) < 0)
                    {
                        perror("epoll_ctl client");

                        close(client_fd);
                        continue;
                    }
                }
            }

            /*
             * Mevcut client'tan event geldi.
             */
            else
            {
                char buf[BUF_SIZE];
                ssize_t n;

                /*
                 * Peer bağlantının kendi write tarafını
                 * kapattıysa client'ı temizle.
                 */
                if(events[i].events &
                   (EPOLLRDHUP |
                    EPOLLHUP |
                    EPOLLERR))
                {
                    epoll_ctl(
                        epoll_fd,
                        EPOLL_CTL_DEL,
                        fd,
                        NULL
                    );

                    close(fd);

                    printf(
                        "Client disconnected, fd=%d\n",
                        fd
                    );

                    continue;
                }

                n = read(
                    fd,
                    buf,
                    sizeof(buf)
                );

                if(n > 0)
                {
                    printf(
                        "Received %zd bytes from fd=%d\n",
                        n,
                        fd
                    );

                    /*
                     * Basit echo.
                     *
                     * Non-blocking socket'ta write()
                     * tüm veriyi tek seferde yazmak
                     * zorunda değildir.
                     *
                     * Eğitim sürümünde basit tutuluyor.
                     */
                    ssize_t written;

                    written = write(
                        fd,
                        buf,
                        (size_t)n
                    );

                    if(written < 0)
                    {
                        if(errno != EAGAIN &&
                           errno != EWOULDBLOCK)
                        {
                            perror("write");

                            epoll_ctl(
                                epoll_fd,
                                EPOLL_CTL_DEL,
                                fd,
                                NULL
                            );

                            close(fd);
                        }
                    }
                }

                else if(n == 0)
                {
                    /*
                     * read() == 0
                     *
                     * Peer orderly shutdown yaptı,
                     * yani FIN aldık.
                     */
                    epoll_ctl(
                        epoll_fd,
                        EPOLL_CTL_DEL,
                        fd,
                        NULL
                    );

                    close(fd);

                    printf(
                        "Client closed connection, fd=%d\n",
                        fd
                    );
                }

                else
                {
                    if(errno != EAGAIN &&
                       errno != EWOULDBLOCK)
                    {
                        perror("read");

                        epoll_ctl(
                            epoll_fd,
                            EPOLL_CTL_DEL,
                            fd,
                            NULL
                        );

                        close(fd);
                    }
                }
            }
        }
    }

    close(epoll_fd);
    close(listen_fd);

    return EXIT_SUCCESS;
}