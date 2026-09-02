#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <unistd.h>
#include <errno.h>

#include <arpa/inet.h>
#include <sys/socket.h>
#include <sys/epoll.h>

#define PORT 5001

#define MAX_CLIENTS 100
#define MAX_EVENTS 32

#define USERNAME_SIZE 32
#define BUFFER_SIZE 4096


struct Client {

    int fd;

    int active;
    int logged_in;

    char username[USERNAME_SIZE];

    char input_buffer[BUFFER_SIZE];

    size_t input_len;
};


static struct Client clients[MAX_CLIENTS];

static int epoll_fd;


/*
 * Bir socket'e tüm veriyi gönder.
 */
static int send_all(
    int fd,
    const char *buffer,
    size_t length
)
{
    size_t total_sent = 0;

    while (total_sent < length) {

        ssize_t n = send(
            fd,
            buffer + total_sent,
            length - total_sent,
            0
        );

        if (n < 0) {

            if (errno == EINTR) {
                continue;
            }

            return -1;
        }

        total_sent += (size_t)n;
    }

    return 0;
}


static int send_text(
    int fd,
    const char *text
)
{
    return send_all(
        fd,
        text,
        strlen(text)
    );
}


/*
 * Boş client slotu bul.
 */
static int find_free_client(void)
{
    for (int i = 0; i < MAX_CLIENTS; i++) {

        if (clients[i].active == 0) {
            return i;
        }
    }

    return -1;
}


/*
 * fd hangi client'a ait?
 */
static int find_client_by_fd(int fd)
{
    for (int i = 0; i < MAX_CLIENTS; i++) {

        if (
            clients[i].active == 1 &&
            clients[i].fd == fd
        ) {
            return i;
        }
    }

    return -1;
}


/*
 * Aynı username zaten kullanımda mı?
 */
static int username_in_use(
    const char *username
)
{
    for (int i = 0; i < MAX_CLIENTS; i++) {

        if (
            clients[i].active == 1 &&
            clients[i].logged_in == 1 &&
            strcmp(
                clients[i].username,
                username
            ) == 0
        ) {
            return 1;
        }
    }

    return 0;
}


/*
 * Mesajı bütün login olmuş client'lara gönder.
 *
 * except_fd:
 * mesajı gönderen kişiye tekrar göndermemek için.
 *
 * -1 verilirse herkese gider.
 */
static void broadcast_message(
    const char *message,
    int except_fd
)
{
    for (int i = 0; i < MAX_CLIENTS; i++) {

        if (
            clients[i].active == 1 &&
            clients[i].logged_in == 1 &&
            clients[i].fd != except_fd
        ) {

            if (
                send_text(
                    clients[i].fd,
                    message
                ) < 0
            ) {

                perror("broadcast send");
            }
        }
    }
}


/*
 * Client bağlantısını kapat.
 */
static void disconnect_client(
    int client_index
)
{
    if (
        client_index < 0 ||
        client_index >= MAX_CLIENTS
    ) {
        return;
    }

    if (!clients[client_index].active) {
        return;
    }

    int fd = clients[client_index].fd;

    epoll_ctl(
        epoll_fd,
        EPOLL_CTL_DEL,
        fd,
        NULL
    );

    close(fd);

    memset(
        &clients[client_index],
        0,
        sizeof(struct Client)
    );

    clients[client_index].fd = -1;
}


/*
 * Kullanıcı listesini göster.
 */
static void show_user_list(
    int client_index
)
{
    send_text(
        clients[client_index].fd,
        "\n===== ONLINE USERS =====\n"
    );

    int number = 1;

    for (int i = 0; i < MAX_CLIENTS; i++) {

        if (
            clients[i].active &&
            clients[i].logged_in
        ) {

            char line[128];

            snprintf(
                line,
                sizeof(line),
                "%d. %s%s\n",
                number++,
                clients[i].username,
                i == client_index
                    ? " (you)"
                    : ""
            );

            send_text(
                clients[client_index].fd,
                line
            );
        }
    }

    send_text(
        clients[client_index].fd,
        "========================\n"
    );
}


/*
 * Login sırasında gelen username'i işle.
 */
static void process_login(
    int client_index,
    char *line
)
{
    struct Client *client =
        &clients[client_index];

    /*
     * \r varsa temizle.
     */
    line[
        strcspn(line, "\r")
    ] = '\0';

    if (line[0] == '\0') {

        send_text(
            client->fd,
            "Username cannot be empty.\n"
            "Enter your username: "
        );

        return;
    }

    if (
        strlen(line) >= USERNAME_SIZE
    ) {

        send_text(
            client->fd,
            "Username too long.\n"
            "Enter your username: "
        );

        return;
    }

    if (username_in_use(line)) {

        send_text(
            client->fd,
            "This username is already online.\n"
            "Enter another username: "
        );

        return;
    }

    strncpy(
        client->username,
        line,
        USERNAME_SIZE - 1
    );

    client->username[
        USERNAME_SIZE - 1
    ] = '\0';

    client->logged_in = 1;

    printf(
        "%s joined group chat. fd=%d\n",
        client->username,
        client->fd
    );

    send_text(
        client->fd,
        "\nLogin successful.\n"
        "Welcome to the GROUP CHAT.\n"
        "\n"
        "Commands:\n"
        "/users  -> show online users\n"
        "/quit   -> leave server\n"
        "\n"
        "Start typing your messages.\n"
        "============================\n"
    );

    show_user_list(client_index);

    /*
     * Diğer kullanıcılara katılma bildirimi.
     */
    char join_message[128];

    snprintf(
        join_message,
        sizeof(join_message),
        "\n[SYSTEM] %s joined the group.\n",
        client->username
    );

    broadcast_message(
        join_message,
        client->fd
    );
}


/*
 * Login sonrası normal grup mesajını işle.
 */
static int process_group_message(
    int client_index,
    char *line
)
{
    struct Client *client =
        &clients[client_index];

    /*
     * Online kullanıcıları göster.
     */
    if (
        strcmp(
            line,
            "/users"
        ) == 0
    ) {

        show_user_list(
            client_index
        );

        return 1;
    }


    /*
     * Çıkış.
     */
    if (
        strcmp(
            line,
            "/quit"
        ) == 0
    ) {

        char leave_message[128];

        snprintf(
            leave_message,
            sizeof(leave_message),
            "\n[SYSTEM] %s left the group.\n",
            client->username
        );

        broadcast_message(
            leave_message,
            client->fd
        );

        printf(
            "%s left group chat.\n",
            client->username
        );

        send_text(
            client->fd,
            "Goodbye.\n"
        );

        disconnect_client(
            client_index
        );

        return 0;
    }


    if (line[0] == '\0') {
        return 1;
    }


    /*
     * username + message
     *
     * örnek:
     *
     * ahmet: merhaba
     */
    char group_message[BUFFER_SIZE + USERNAME_SIZE + 16];

    snprintf(
        group_message,
        sizeof(group_message),
        "%s: %s\n",
        client->username,
        line
    );


    /*
     * Server terminalinde de göster.
     */
    printf(
        "%s",
        group_message
    );


    /*
     * Mesajı diğer bütün kullanıcılara gönder.
     */
    broadcast_message(
        group_message,
        client->fd
    );

    return 1;
}


/*
 * Tamamlanmış bir satırı işle.
 */
static int process_line(
    int client_index,
    char *line
)
{
    /*
     * Windows client \r\n gönderebilir.
     */
    line[
        strcspn(line, "\r")
    ] = '\0';


    if (
        clients[client_index].logged_in == 0
    ) {

        process_login(
            client_index,
            line
        );

        return 1;
    }


    return process_group_message(
        client_index,
        line
    );
}


int main(void)
{
    int server_fd;

    struct sockaddr_in server_addr;

    struct epoll_event event;

    struct epoll_event events[MAX_EVENTS];


    /*
     * Client tablosunu başlangıç durumuna getir.
     */
    memset(
        clients,
        0,
        sizeof(clients)
    );

    for (int i = 0; i < MAX_CLIENTS; i++) {

        clients[i].fd = -1;
    }


    /*
     * TCP socket.
     */
    server_fd = socket(
        AF_INET,
        SOCK_STREAM,
        0
    );

    if (server_fd < 0) {

        perror("socket");

        return 1;
    }


    /*
     * Server yeniden hızlı başlatılabilsin.
     */
    int reuse = 1;

    if (
        setsockopt(
            server_fd,
            SOL_SOCKET,
            SO_REUSEADDR,
            &reuse,
            sizeof(reuse)
        ) < 0
    ) {

        perror("setsockopt");
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

    /*
     * Wi-Fi + Ethernet + localhost
     * dahil bütün interface'leri dinle.
     */
    server_addr.sin_addr.s_addr =
        INADDR_ANY;


    if (
        bind(
            server_fd,
            (struct sockaddr *)&server_addr,
            sizeof(server_addr)
        ) < 0
    ) {

        perror("bind");

        close(server_fd);

        return 1;
    }


    if (
        listen(
            server_fd,
            16
        ) < 0
    ) {

        perror("listen");

        close(server_fd);

        return 1;
    }


    /*
     * epoll oluştur.
     */
    epoll_fd =
        epoll_create1(0);

    if (epoll_fd < 0) {

        perror("epoll_create1");

        close(server_fd);

        return 1;
    }


    /*
     * Listening socket'i epoll'a ekle.
     */
    memset(
        &event,
        0,
        sizeof(event)
    );

    event.events =
        EPOLLIN;

    event.data.fd =
        server_fd;


    if (
        epoll_ctl(
            epoll_fd,
            EPOLL_CTL_ADD,
            server_fd,
            &event
        ) < 0
    ) {

        perror("epoll_ctl");

        close(epoll_fd);
        close(server_fd);

        return 1;
    }


    printf(
        "GROUP CHAT SERVER\n"
    );

    printf(
        "Listening on port %d...\n",
        PORT
    );


    while (1) {

        int event_count =
            epoll_wait(
                epoll_fd,
                events,
                MAX_EVENTS,
                -1
            );


        if (event_count < 0) {

            if (errno == EINTR) {
                continue;
            }

            perror("epoll_wait");

            break;
        }


        for (
            int i = 0;
            i < event_count;
            i++
        ) {

            int current_fd =
                events[i].data.fd;


            /*
             * ==================================
             * NEW TCP CONNECTION
             * ==================================
             */
            if (
                current_fd ==
                server_fd
            ) {

                struct sockaddr_in client_addr;

                socklen_t client_len =
                    sizeof(client_addr);


                int client_fd =
                    accept(
                        server_fd,
                        (struct sockaddr *)
                            &client_addr,
                        &client_len
                    );


                if (client_fd < 0) {

                    perror("accept");

                    continue;
                }


                int client_index =
                    find_free_client();


                if (
                    client_index == -1
                ) {

                    send_text(
                        client_fd,
                        "Server is full.\n"
                    );

                    close(client_fd);

                    continue;
                }


                memset(
                    &clients[client_index],
                    0,
                    sizeof(struct Client)
                );


                clients[client_index].fd =
                    client_fd;

                clients[client_index].active =
                    1;

                clients[client_index].logged_in =
                    0;

                clients[client_index].input_len =
                    0;


                char client_ip[
                    INET_ADDRSTRLEN
                ];


                inet_ntop(
                    AF_INET,
                    &client_addr.sin_addr,
                    client_ip,
                    sizeof(client_ip)
                );


                printf(
                    "New connection: %s:%d fd=%d\n",
                    client_ip,
                    ntohs(
                        client_addr.sin_port
                    ),
                    client_fd
                );


                /*
                 * Yeni socket'i epoll'a ekle.
                 */
                memset(
                    &event,
                    0,
                    sizeof(event)
                );

                event.events =
                    EPOLLIN;

                event.data.fd =
                    client_fd;


                if (
                    epoll_ctl(
                        epoll_fd,
                        EPOLL_CTL_ADD,
                        client_fd,
                        &event
                    ) < 0
                ) {

                    perror(
                        "epoll_ctl client"
                    );

                    disconnect_client(
                        client_index
                    );

                    continue;
                }


                send_text(
                    client_fd,
                    "Enter your username: "
                );


                continue;
            }


            /*
             * ==================================
             * EXISTING CLIENT
             * ==================================
             */

            int client_index =
                find_client_by_fd(
                    current_fd
                );


            if (
                client_index == -1
            ) {

                continue;
            }


            char recv_buffer[1024];


            ssize_t n =
                recv(
                    current_fd,
                    recv_buffer,
                    sizeof(recv_buffer),
                    0
                );


            if (n < 0) {

                if (errno == EINTR) {
                    continue;
                }


                perror("recv");


                disconnect_client(
                    client_index
                );


                continue;
            }


            /*
             * TCP FIN geldi.
             */
            if (n == 0) {

                if (
                    clients[
                        client_index
                    ].logged_in
                ) {

                    char leave_message[128];


                    snprintf(
                        leave_message,
                        sizeof(leave_message),
                        "\n[SYSTEM] %s disconnected.\n",
                        clients[
                            client_index
                        ].username
                    );


                    broadcast_message(
                        leave_message,
                        current_fd
                    );


                    printf(
                        "%s disconnected.\n",
                        clients[
                            client_index
                        ].username
                    );
                }


                disconnect_client(
                    client_index
                );


                continue;
            }


            struct Client *client =
                &clients[client_index];


            /*
             * TCP bir BYTE STREAM'dir.
             *
             * recv() = message varsaymıyoruz.
             *
             * Gelen byte'ları client'a özel
             * input_buffer içerisine ekliyoruz.
             */
            if (
                client->input_len +
                (size_t)n >=
                sizeof(
                    client->input_buffer
                )
            ) {

                send_text(
                    current_fd,
                    "Input too long.\n"
                );


                client->input_len = 0;


                continue;
            }


            memcpy(
                client->input_buffer +
                    client->input_len,
                recv_buffer,
                (size_t)n
            );


            client->input_len +=
                (size_t)n;


            /*
             * Buffer içinde birden fazla
             * tamamlanmış satır bulunabilir.
             */
            while (
                client->active
            ) {

                char *newline =
                    memchr(
                        client->input_buffer,
                        '\n',
                        client->input_len
                    );


                if (
                    newline == NULL
                ) {

                    break;
                }


                size_t line_length =
                    (size_t)(
                        newline -
                        client->input_buffer
                    );


                char line[BUFFER_SIZE];


                memcpy(
                    line,
                    client->input_buffer,
                    line_length
                );


                line[line_length] =
                    '\0';


                size_t consumed =
                    line_length + 1;


                memmove(
                    client->input_buffer,
                    client->input_buffer +
                        consumed,
                    client->input_len -
                        consumed
                );


                client->input_len -=
                    consumed;


                if (
                    !process_line(
                        client_index,
                        line
                    )
                ) {

                    break;
                }
            }
        }
    }


    close(epoll_fd);

    close(server_fd);


    return 0;
}