#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <unistd.h>

#include <arpa/inet.h>
#include <sys/socket.h>
#include <sys/epoll.h>

#define PORT 5001
#define MAX_EVENTS 10

#define MAX_CLIENTS 100

struct Client {
    int fd;
    int user_id;
    char username[32];
    int active;
};

int main(void)
{
    int server_fd;
    int client_fd;
    int epoll_fd;
    int n;

    struct sockaddr_in server_addr;
    struct sockaddr_in client_addr;
    socklen_t client_len;

    struct epoll_event event;
    struct epoll_event events[MAX_EVENTS];
    struct Client clients[MAX_CLIENTS];

    char client_ip[INET_ADDRSTRLEN];
    char username[32];
    char response[128];
    char message[1024];
    char line[128];

    int user_id;
    int max_id;

    server_fd = socket(AF_INET, SOCK_STREAM, 0);

    if (server_fd < 0) {
        perror("socket");
        return 1;
    }

    memset(&server_addr, 0, sizeof(server_addr));

    server_addr.sin_family = AF_INET;
    server_addr.sin_port = htons(PORT);
    server_addr.sin_addr.s_addr = INADDR_ANY;

    if (bind(
            server_fd,
            (struct sockaddr *)&server_addr,
            sizeof(server_addr)
        ) < 0) {

        perror("bind");
        close(server_fd);
        return 1;
    }

    if (listen(server_fd, 5) < 0) {
        perror("listen");
        close(server_fd);
        return 1;
    }

    epoll_fd = epoll_create1(0);

    if (epoll_fd < 0) {
        perror("epoll_create1");
        close(server_fd);
        return 1;
    }

    memset(&event, 0, sizeof(event));

    event.events = EPOLLIN;
    event.data.fd = server_fd;

    if (epoll_ctl(
            epoll_fd,
            EPOLL_CTL_ADD,
            server_fd,
            &event
        ) < 0) {

        perror("epoll_ctl");
        close(epoll_fd);
        close(server_fd);
        return 1;
    }

    printf("Server listening on port %d...\n", PORT);

    while (1) {

        int event_count = epoll_wait(
            epoll_fd,
            events,
            MAX_EVENTS,
            -1
        );

        if (event_count < 0) {
            perror("epoll_wait");
            break;
        }

        for (int i = 0; i < event_count; i++) {

            int current_fd = events[i].data.fd;

            /*
             * server_fd hazırsa:
             * yeni TCP bağlantısı geliyor.
             */
            if (current_fd == server_fd) {

                client_len = sizeof(client_addr);

                client_fd = accept(
                    server_fd,
                    (struct sockaddr *)&client_addr,
                    &client_len
                );

                if (client_fd < 0) {
                    perror("accept");
                    continue;
                }

                inet_ntop(
                    AF_INET,
                    &client_addr.sin_addr,
                    client_ip,
                    sizeof(client_ip)
                );

                printf(
                    "Client connected: %s:%d\n",
                    client_ip,
                    ntohs(client_addr.sin_port)
                );

                const char *prompt = "Enter your username: ";

                if (send(
                        client_fd,
                        prompt,
                        strlen(prompt),
                        0
                    ) < 0) {

                    perror("send");
                    close(client_fd);
                    continue;
                }

                n = recv(
                    client_fd,
                    username,
                    sizeof(username) - 1,
                    0
                );

                if (n < 0) {
                    perror("recv");
                    close(client_fd);
                    continue;
                }

                if (n == 0) {
                    printf(
                        "Client disconnected before sending username.\n"
                    );

                    close(client_fd);
                    continue;
                }

                username[n] = '\0';

                printf(
                    "Received username: %s\n",
                    username
                );

                FILE *users_file = fopen(
                    "users.db",
                    "a+"
                );

                if (users_file == NULL) {
                    perror("fopen");
                    close(client_fd);
                    continue;
                }

                user_id = -1;
                max_id = 0;

                rewind(users_file);

                while (
                    fgets(
                        line,
                        sizeof(line),
                        users_file
                    ) != NULL
                ) {

                    int file_id;
                    char file_username[32];

                    if (sscanf(
                            line,
                            "%d|%31s",
                            &file_id,
                            file_username
                        ) == 2) {

                        if (file_id > max_id) {
                            max_id = file_id;
                        }

                        if (
                            strcmp(
                                file_username,
                                username
                            ) == 0
                        ) {
                            user_id = file_id;
                        }
                    }
                }

                if (user_id == -1) {

                    user_id = max_id + 1;

                    fprintf(
                        users_file,
                        "%d|%s\n",
                        user_id,
                        username
                    );

                    printf(
                        "New user created. ID: %d\n",
                        user_id
                    );
                }
                else {

                    printf(
                        "Existing user found. ID: %d\n",
                        user_id
                    );
                }

                snprintf(
                    response,
                    sizeof(response),
                    "Login successful. User ID: %d\n",
                    user_id
                );

                if (send(
                        client_fd,
                        response,
                        strlen(response),
                        0
                    ) < 0) {

                    perror("send");
                    fclose(users_file);
                    close(client_fd);
                    continue;
                }

                fclose(users_file);

                /*
                 * Artık bu client_fd'yi de epoll'a ekliyoruz.
                 */
                memset(&event, 0, sizeof(event));

                event.events = EPOLLIN;
                event.data.fd = client_fd;

                if (epoll_ctl(
                        epoll_fd,
                        EPOLL_CTL_ADD,
                        client_fd,
                        &event
                    ) < 0) {

                    perror("epoll_ctl client");
                    close(client_fd);
                    continue;
                }

                printf(
                    "Client fd %d added to epoll.\n",
                    client_fd
                );
            }

            /*
             * server_fd değilse:
             * mevcut clientlardan biri veri göndermiştir.
             */
            else {

                n = recv(
                    current_fd,
                    message,
                    sizeof(message) - 1,
                    0
                );

                if (n < 0) {
                    perror("recv");
                    continue;
                }

                if (n == 0) {

                    printf(
                        "Client fd %d disconnected.\n",
                        current_fd
                    );

                    epoll_ctl(
                        epoll_fd,
                        EPOLL_CTL_DEL,
                        current_fd,
                        NULL
                    );

                    close(current_fd);

                    continue;
                }

                message[n] = '\0';

                printf(
                    "fd %d: %s",
                    current_fd,
                    message
                );
            }
        }
    }

    close(epoll_fd);
    close(server_fd);

    return 0;
}