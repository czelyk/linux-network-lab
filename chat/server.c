#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <unistd.h>

#include <arpa/inet.h>
#include <sys/socket.h>

#define PORT 5001

int main(void)
{
    int server_fd;
    int client_fd;
    int n;

    struct sockaddr_in server_addr;
    struct sockaddr_in client_addr;
    socklen_t client_len;

    char client_ip[INET_ADDRSTRLEN];

    char username[32];

    char line[128];
    int user_id = -1;
    int max_id = 0;

    server_fd = socket(AF_INET, SOCK_STREAM, 0);

    if (server_fd < 0) {
        perror("socket");
        return 1;
    }

    memset(&server_addr, 0, sizeof(server_addr));

    server_addr.sin_family = AF_INET;
    server_addr.sin_port = htons(PORT);
    server_addr.sin_addr.s_addr = INADDR_ANY;

    if (bind(server_fd,
             (struct sockaddr *)&server_addr,
             sizeof(server_addr)) < 0) {

        perror("bind");
        close(server_fd);
        return 1;
    }

    if (listen(server_fd, 5) < 0) {
        perror("listen");
        close(server_fd);
        return 1;
    }

    client_len = sizeof(client_addr);

    client_fd = accept(
        server_fd,
        (struct sockaddr *)&client_addr,
        &client_len
    );

    if (client_fd < 0) {
        perror("accept");
        close(server_fd);
        return 1;
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

    if (send(client_fd, prompt, strlen(prompt), 0) < 0) {
        perror("send");
        close(client_fd);
        close(server_fd);
        return 1;
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
        close(server_fd);
        return 1;
    }

    if (n == 0) {
        printf("Client disconnected before sending username.\n");
        close(client_fd);
        close(server_fd);
        return 1;
    }

    username[n] = '\0';

    printf("Received username: %s\n", username);

    FILE *users_file = fopen("users.db", "a+");

    if (users_file == NULL) {
        perror("fopen");
        close(client_fd);
        close(server_fd);
        return 1;
    }

    rewind(users_file);

    while (fgets(line, sizeof(line), users_file) != NULL) {

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

            if (strcmp(file_username, username) == 0) {
                user_id = file_id;
                break;
            }
        }
    }

    fclose(users_file);

    printf("Client accepted successfully.\n");

    close(client_fd);
    close(server_fd);

    return 0;
}