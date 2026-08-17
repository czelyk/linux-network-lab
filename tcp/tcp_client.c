#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <unistd.h>

#include <sys/socket.h>
#include <netinet/in.h>
#include <arpa/inet.h>

#define SERVER_IP "127.0.0.1"
#define SERVER_PORT 5001
#define BUFFER_SIZE 1024

int main(void)
{
    int sockfd;

    struct sockaddr_in server_addr;

    struct sockaddr_in local_addr;
    socklen_t local_addr_len;

    char local_ip[INET_ADDRSTRLEN];
    unsigned short local_port;

    char buffer[BUFFER_SIZE];

    const char *message = "merhaba tcp server";

    ssize_t sent_bytes;
    ssize_t received_bytes;

    sockfd = socket(AF_INET, SOCK_STREAM, 0);

    if(sockfd < 0)
    {
        perror("socket");
        return EXIT_FAILURE;
    }

    memset(&server_addr, 0, sizeof(server_addr));

    server_addr.sin_family = AF_INET;
    server_addr.sin_port = htons(SERVER_PORT);

    if(inet_pton(AF_INET,
                 SERVER_IP,
                 &server_addr.sin_addr) != 1)
    {
        perror("inet_pton");
        close(sockfd);
        return EXIT_FAILURE;
    }

    if(connect(sockfd,
               (struct sockaddr *)&server_addr,
               sizeof(server_addr)) < 0)
    {
        perror("connect");
        close(sockfd);
        return EXIT_FAILURE;
    }

    printf("Connected to %s:%d\n",
           SERVER_IP,
           SERVER_PORT);

    local_addr_len = sizeof(local_addr);

    if(getsockname(
            sockfd,
            (struct sockaddr *)&local_addr,
            &local_addr_len) < 0)
    {
        perror("getsockname");
        close(sockfd);
        return EXIT_FAILURE;
    }

    local_port = ntohs(local_addr.sin_port);

    if(inet_ntop(AF_INET,
                 &local_addr.sin_addr,
                 local_ip,
                 sizeof(local_ip)) == NULL)
    {
        perror("inet_ntop");
        close(sockfd);
        return EXIT_FAILURE;
    }

    printf("Client local endpoint: %s:%u\n",
           local_ip,
           local_port);

    sent_bytes = send(
        sockfd,
        message,
        strlen(message),
        0
    );

    if(sent_bytes < 0)
    {
        perror("send");
        close(sockfd);
        return EXIT_FAILURE;
    }

    printf("Sent %zd bytes: %s\n",
           sent_bytes,
           message);

    received_bytes = recv(
        sockfd,
        buffer,
        sizeof(buffer) - 1,
        0
    );

    if(received_bytes < 0)
    {
        perror("recv");
        close(sockfd);
        return EXIT_FAILURE;
    }

    if(received_bytes == 0)
    {
        printf("Server closed the connection.\n");
        close(sockfd);
        return EXIT_SUCCESS;
    }

    buffer[received_bytes] = '\0';

    printf("Received %zd bytes from server: %s\n",
           received_bytes,
           buffer);

    close(sockfd);

    return EXIT_SUCCESS;
}