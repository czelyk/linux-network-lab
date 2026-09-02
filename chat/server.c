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
    int logged_in;
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
    char response[128];
    char message[1024];
    char line[128];

    memset(clients, 0, sizeof(clients));

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
             * Listening socket hazır:
             * yeni TCP bağlantısı var.
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

                /*
                 * clients[] içinde boş slot bul.
                 */
                int client_index = -1;

                for (int j = 0; j < MAX_CLIENTS; j++) {

                    if (clients[j].active == 0) {
                        client_index = j;
                        break;
                    }
                }

                if (client_index == -1) {

                    printf("Maximum client limit reached.\n");

                    close(client_fd);
                    continue;
                }

                /*
                 * Yeni bağlantının başlangıç state'i.
                 */
                clients[client_index].fd = client_fd;
                clients[client_index].user_id = -1;
                clients[client_index].username[0] = '\0';
                clients[client_index].active = 1;
                clients[client_index].logged_in = 0;

                /*
                 * Username iste.
                 */
                const char *prompt = "Enter your username: ";

                if (send(
                        client_fd,
                        prompt,
                        strlen(prompt),
                        0
                    ) < 0) {

                    perror("send");

                    clients[client_index].active = 0;
                    clients[client_index].fd = -1;

                    close(client_fd);

                    continue;
                }

                /*
                 * Client socket'ini epoll'a ekle.
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

                    clients[client_index].active = 0;
                    clients[client_index].fd = -1;

                    close(client_fd);

                    continue;
                }

                printf(
                    "Client fd %d added to epoll, waiting for username.\n",
                    client_fd
                );
            }

            /*
             * Listening socket değil:
             * mevcut bir client veri gönderdi.
             */
            else {

                /*
                 * current_fd hangi client'a ait?
                 */
                int client_index = -1;

                for (int j = 0; j < MAX_CLIENTS; j++) {

                    if (
                        clients[j].active == 1 &&
                        clients[j].fd == current_fd
                    ) {

                        client_index = j;
                        break;
                    }
                }

                if (client_index == -1) {

                    printf(
                        "Unknown client fd: %d\n",
                        current_fd
                    );

                    continue;
                }

                /*
                 * Client'tan veri oku.
                 */
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

                /*
                 * TCP bağlantısı kapandı.
                 */
                if (n == 0) {

                    if (clients[client_index].logged_in) {

                        printf(
                            "Client disconnected: %s [%d], fd=%d\n",
                            clients[client_index].username,
                            clients[client_index].user_id,
                            current_fd
                        );
                    }
                    else {

                        printf(
                            "Client disconnected before login: fd=%d\n",
                            current_fd
                        );
                    }

                    epoll_ctl(
                        epoll_fd,
                        EPOLL_CTL_DEL,
                        current_fd,
                        NULL
                    );

                    clients[client_index].active = 0;
                    clients[client_index].logged_in = 0;
                    clients[client_index].fd = -1;
                    clients[client_index].user_id = -1;
                    clients[client_index].username[0] = '\0';

                    close(current_fd);

                    continue;
                }

                message[n] = '\0';

                /*
                 * LOGIN AŞAMASI
                 */
                if (clients[client_index].logged_in == 0) {

                    strncpy(
                        clients[client_index].username,
                        message,
                        sizeof(clients[client_index].username) - 1
                    );

                    clients[client_index].username[
                        sizeof(clients[client_index].username) - 1
                    ] = '\0';

                    /*
                     * Client fgets() kullandığı için
                     * sondaki newline'ı temizle.
                     */
                    clients[client_index].username[
                        strcspn(
                            clients[client_index].username,
                            "\r\n"
                        )
                    ] = '\0';

                    printf(
                        "Username received from fd %d: %s\n",
                        current_fd,
                        clients[client_index].username
                    );

                    FILE *users_file = fopen(
                        "users.db",
                        "a+"
                    );

                    if (users_file == NULL) {

                        perror("fopen");
                        continue;
                    }

                    int found_user_id = -1;
                    int max_id = 0;

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
                                    clients[client_index].username
                                ) == 0
                            ) {

                                found_user_id = file_id;
                            }
                        }
                    }

                    /*
                     * Kullanıcı yoksa yeni ID oluştur.
                     */
                    if (found_user_id == -1) {

                        found_user_id = max_id + 1;

                        fprintf(
                            users_file,
                            "%d|%s\n",
                            found_user_id,
                            clients[client_index].username
                        );

                        printf(
                            "New user created: %s, ID=%d\n",
                            clients[client_index].username,
                            found_user_id
                        );
                    }
                    else {

                        printf(
                            "Existing user found: %s, ID=%d\n",
                            clients[client_index].username,
                            found_user_id
                        );
                    }

                    clients[client_index].user_id =
                        found_user_id;

                    clients[client_index].logged_in = 1;

                    fclose(users_file);

                    snprintf(
                        response,
                        sizeof(response),
                        "Login successful. User ID: %d\n",
                        clients[client_index].user_id
                    );

                    if (send(
                            current_fd,
                            response,
                            strlen(response),
                            0
                        ) < 0) {

                        perror("send");
                    }
                }

                /*
                 * LOGIN TAMAMLANMIŞ:
                 * gelen veri artık chat mesajıdır.
                 */
                else {

                    printf(
                        "%s [%d]: %s",
                        clients[client_index].username,
                        clients[client_index].user_id,
                        message
                    );
                }
            }
        }
    }

    close(epoll_fd);
    close(server_fd);

    return 0;
}