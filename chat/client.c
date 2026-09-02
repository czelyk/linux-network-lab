#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <unistd.h>
#include <errno.h>

#include <arpa/inet.h>
#include <sys/socket.h>
#include <poll.h>

#define PORT 5001
#define SERVER_IP "127.0.0.1"

static int send_all(
    int fd,
    const char *buffer,
    size_t length
)
{
    size_t sent = 0;

    while (sent < length) {

        ssize_t n = send(
            fd,
            buffer + sent,
            length - sent,
            0
        );

        if (n < 0) {

            if (errno == EINTR) {
                continue;
            }

            return -1;
        }

        sent += (size_t)n;
    }

    return 0;
}


int main(void)
{
    int client_fd;

    struct sockaddr_in server_addr;

    client_fd = socket(
        AF_INET,
        SOCK_STREAM,
        0
    );

    if (client_fd < 0) {

        perror("socket");

        return 1;
    }

    memset(
        &server_addr,
        0,
        sizeof(server_addr)
    );

    server_addr.sin_family =
        AF_INET;

    server_addr.sin_port =
        htons(PORT);

    if (
        inet_pton(
            AF_INET,
            SERVER_IP,
            &server_addr.sin_addr
        ) != 1
    ) {

        fprintf(
            stderr,
            "Invalid server IP.\n"
        );

        close(client_fd);

        return 1;
    }

    if (
        connect(
            client_fd,
            (struct sockaddr *)
                &server_addr,
            sizeof(server_addr)
        ) < 0
    ) {

        perror("connect");

        close(client_fd);

        return 1;
    }

    printf(
        "Connected to server.\n"
    );

    /*
     * poll:
     *
     * fd 0       = keyboard/stdin
     * client_fd  = TCP socket
     */
    struct pollfd fds[2];

    fds[0].fd =
        STDIN_FILENO;

    fds[0].events =
        POLLIN;

    fds[1].fd =
        client_fd;

    fds[1].events =
        POLLIN;

    while (1) {

        int ready =
            poll(
                fds,
                2,
                -1
            );

        if (ready < 0) {

            if (errno == EINTR) {
                continue;
            }

            perror("poll");

            break;
        }

        /*
         * --------------------------------
         * KEYBOARD INPUT
         * --------------------------------
         */
        if (
            fds[0].revents &
            POLLIN
        ) {

            char buffer[1024];

            if (
                fgets(
                    buffer,
                    sizeof(buffer),
                    stdin
                ) == NULL
            ) {

                break;
            }

            if (
                send_all(
                    client_fd,
                    buffer,
                    strlen(buffer)
                ) < 0
            ) {

                perror("send");

                break;
            }
        }

        /*
         * --------------------------------
         * SERVER MESSAGE
         * --------------------------------
         */
        if (
            fds[1].revents &
            POLLIN
        ) {

            char buffer[4096];

            ssize_t n =
                recv(
                    client_fd,
                    buffer,
                    sizeof(buffer) - 1,
                    0
                );

            if (n < 0) {

                perror("recv");

                break;
            }

            if (n == 0) {

                printf(
                    "\nServer disconnected.\n"
                );

                break;
            }

            buffer[n] =
                '\0';

            printf(
                "%s",
                buffer
            );

            fflush(stdout);
        }

        if (
            fds[1].revents &
            (
                POLLHUP |
                POLLERR |
                POLLNVAL
            )
        ) {

            printf(
                "\nConnection closed.\n"
            );

            break;
        }
    }

    close(client_fd);

    return 0;
}