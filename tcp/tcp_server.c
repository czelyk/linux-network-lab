#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <unistd.h>

#include <sys/socket.h>
#include <netinet/in.h>
#include <arpa/inet.h>

#define PORT 5001
#define BUFFER_SIZE 1024
#define BACKLOG 5

int main(void)
{
    int listen_fd;
    int client_fd;

    struct sockaddr_in server_addr;
    struct sockaddr_in client_addr;
    socklen_t client_addr_len;

    char buffer[BUFFER_SIZE];
    ssize_t received_bytes;
    ssize_t sent_bytes;

    const char *reply = "zdravo tcp client";

    char client_ip[INET_ADDRSTRLEN];
    unsigned short client_port;

    listen_fd = socket(AF_INET, SOCK_STREAM, 0);

    if(listen_fd < 0)
    {
        perror("socket");
        return EXIT_FAILURE;
    }

    memset(&server_addr, 0, sizeof(server_addr));

    server_addr.sin_family = AF_INET;
    server_addr.sin_port = htons(PORT);
    server_addr.sin_addr.s_addr = htonl(INADDR_ANY);

    if(bind(listen_fd,
            (struct sockaddr *)&server_addr,
            sizeof(server_addr)) < 0)
    {
        perror("bind");
        close(listen_fd);
        return EXIT_FAILURE;
    }

    if(listen(listen_fd, BACKLOG) < 0)
    {
        perror("listen");
        close(listen_fd);
        return EXIT_FAILURE;
    }

    printf("TCP server listening on port %d...\n", PORT);

    client_addr_len = sizeof(client_addr);

    client_fd = accept(
        listen_fd,
        (struct sockaddr *)&client_addr,
        &client_addr_len
    );

    if(client_fd < 0)
    {
        perror("accept");
        close(listen_fd);
        return EXIT_FAILURE;
    }

    if(inet_ntop(AF_INET,
                 &client_addr.sin_addr,
                 client_ip,
                 sizeof(client_ip)) == NULL)
    {
        perror("inet_ntop");
        close(client_fd);
        close(listen_fd);
        return EXIT_FAILURE;
    }

    client_port = ntohs(client_addr.sin_port);

    printf("Client connected from %s:%u\n",
           client_ip,
           client_port);

    received_bytes = recv(
        client_fd,
        buffer,
        sizeof(buffer) - 1,
        0
    );

    if(received_bytes < 0)
    {
        perror("recv");
        close(client_fd);
        close(listen_fd);
        return EXIT_FAILURE;
    }

    if(received_bytes == 0)
    {
        printf("Client closed the connection.\n");

        close(client_fd);
        close(listen_fd);

        return EXIT_SUCCESS;
    }

    buffer[received_bytes] = '\0';

    printf("Received %zd bytes: %s\n",
           received_bytes,
           buffer);

    sent_bytes = send(
        client_fd,
        reply,
        strlen(reply),
        0
    );

    if(sent_bytes < 0)
    {
        perror("send");
        close(client_fd);
        close(listen_fd);
        return EXIT_FAILURE;
    }

    printf("Sent %zd bytes back to client.\n",
           sent_bytes);

    close(client_fd);
    close(listen_fd);

    return EXIT_SUCCESS;
}