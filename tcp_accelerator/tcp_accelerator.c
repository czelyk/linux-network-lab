#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <unistd.h>
#include <errno.h>

#include <sys/socket.h>
#include <sys/poll.h>
#include <netinet/in.h>
#include <arpa/inet.h>

#define LISTEN_PORT 5001
#define SERVER_PORT 6000
#define SERVER_IP   "127.0.0.1"

#define BUFFER_SIZE 4096

static int send_all(
    int fd,
    const char *buffer,
    size_t length)
{
    size_t total_sent = 0;

    while(total_sent < length)
    {
        ssize_t sent;

        sent = send(
            fd,
            buffer + total_sent,
            length - total_sent,
            0
        );

        if(sent < 0)
        {
            if(errno == EINTR)
                continue;

            perror("send");
            return -1;
        }

        if(sent == 0)
        {
            fprintf(
                stderr,
                "send returned 0\n"
            );

            return -1;
        }

        total_sent += (size_t)sent;
    }

    return 0;
}


int main(void)
{
    int listen_fd;
    int client_fd;
    int server_fd;

    int yes = 1;

    struct sockaddr_in accelerator_addr;
    struct sockaddr_in server_addr;

    struct pollfd fds[2];

    char buffer[BUFFER_SIZE];


    /*
     * 1. Accelerator listening socket oluştur.
     *
     * Client bu socket'e bağlanacak.
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


    /*
     * Programı tekrar tekrar çalıştırırken
     * listen portunu daha rahat reuse etmek için.
     */
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


    /*
     * Accelerator'ın dinleyeceği adres:
     *
     * 0.0.0.0:5001
     */
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


    /*
     * listen_fd'yi 5001 portuna bağla.
     */
    if(bind(
            listen_fd,
            (struct sockaddr *)&accelerator_addr,
            sizeof(accelerator_addr)) < 0)
    {
        perror("bind");

        close(listen_fd);

        return EXIT_FAILURE;
    }


    /*
     * Socket artık passive/listening socket.
     */
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
     * 2. Client connection kabul et.
     *
     * Bu blocking accept().
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


    /*
     * 3. Accelerator'ın gerçek server'a bağlanacağı
     * ikinci TCP socket'i oluştur.
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


    /*
     * 4. Gerçek server'a bağlan.
     *
     * Burada ikinci TCP connection kuruluyor.
     */
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
     * Artık:
     *
     * CLIENT <---- TCP #1 ----> ACCELERATOR
     *
     * ACCELERATOR <---- TCP #2 ----> SERVER
     *
     * olmak üzere iki ayrı TCP connection var.
     */


    /*
     * 5. poll() ile iki connected socket'i izle.
     *
     * fds[0] = client tarafı
     * fds[1] = server tarafı
     */
    memset(
        fds,
        0,
        sizeof(fds)
    );

    fds[0].fd = client_fd;
    fds[0].events = POLLIN;

    fds[1].fd = server_fd;
    fds[1].events = POLLIN;


    printf(
        "Accelerator forwarding traffic...\n"
    );


    /*
     * 6. Çift yönlü forwarding loop.
     */
    while(1)
    {
        int ready;


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
         * ------------------------------------------------
         * CLIENT -> ACCELERATOR -> SERVER
         * ------------------------------------------------
         */
        if(fds[0].revents & POLLIN)
        {
            ssize_t bytes_received;


            bytes_received = recv(
                client_fd,
                buffer,
                sizeof(buffer),
                0
            );


            if(bytes_received < 0)
            {
                perror("recv client");
                break;
            }


            /*
             * recv() == 0
             *
             * Client FIN göndermiş demektir.
             */
            if(bytes_received == 0)
            {
                printf(
                    "Client closed its send side.\n"
                );


                /*
                 * Client artık data göndermeyecek.
                 *
                 * Server'a da:
                 *
                 * "Benim bu yönde aktaracağım başka
                 * data kalmadı."
                 *
                 * bilgisini FIN ile taşı.
                 */
                shutdown(
                    server_fd,
                    SHUT_WR
                );


                /*
                 * Artık client_fd'den POLLIN
                 * beklememize gerek yok.
                 */
                fds[0].fd = -1;
            }
            else
            {
                printf(
                    "Client -> Accelerator: %zd bytes\n",
                    bytes_received
                );


                if(send_all(
                        server_fd,
                        buffer,
                        (size_t)bytes_received) < 0)
                {
                    break;
                }


                printf(
                    "Accelerator -> Server: %zd bytes\n",
                    bytes_received
                );
            }
        }


        /*
         * ------------------------------------------------
         * SERVER -> ACCELERATOR -> CLIENT
         * ------------------------------------------------
         */
        if(fds[1].revents & POLLIN)
        {
            ssize_t bytes_received;


            bytes_received = recv(
                server_fd,
                buffer,
                sizeof(buffer),
                0
            );


            if(bytes_received < 0)
            {
                perror("recv server");
                break;
            }


            /*
             * Server FIN göndermiş.
             */
            if(bytes_received == 0)
            {
                printf(
                    "Server closed its send side.\n"
                );


                /*
                 * Client'a da FIN üret.
                 */
                shutdown(
                    client_fd,
                    SHUT_WR
                );


                fds[1].fd = -1;
            }
            else
            {
                printf(
                    "Server -> Accelerator: %zd bytes\n",
                    bytes_received
                );


                if(send_all(
                        client_fd,
                        buffer,
                        (size_t)bytes_received) < 0)
                {
                    break;
                }


                printf(
                    "Accelerator -> Client: %zd bytes\n",
                    bytes_received
                );
            }
        }


        /*
         * Hata/hangup durumları.
         */
        if(fds[0].fd >= 0 &&
           (fds[0].revents &
            (POLLERR | POLLNVAL)))
        {
            fprintf(
                stderr,
                "Client socket error\n"
            );

            break;
        }


        if(fds[1].fd >= 0 &&
           (fds[1].revents &
            (POLLERR | POLLNVAL)))
        {
            fprintf(
                stderr,
                "Server socket error\n"
            );

            break;
        }


        /*
         * İki taraf da kendi send side'ını
         * kapattıysa işimiz bitti.
         */
        if(fds[0].fd < 0 &&
           fds[1].fd < 0)
        {
            printf(
                "Both TCP streams finished.\n"
            );

            break;
        }
    }


    /*
     * 7. Cleanup.
     */
    close(server_fd);
    close(client_fd);
    close(listen_fd);


    printf(
        "Accelerator stopped.\n"
    );


    return EXIT_SUCCESS;
}