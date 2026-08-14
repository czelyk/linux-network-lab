#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <unistd.h>

#include <sys/socket.h>
#include <netinet/in.h>
#include <arpa/inet.h>

#define SERVER_IP "127.0.0.1"
#define SERVER_PORT 5000


int main(void)
{
    int sockfd;
    struct sockaddr_in server_addr;
    const char *message = "merhaba";
    ssize_t sent_bytes;

    sockfd = socket(AF_INET, SOCK_DGRAM, 0);

    if(sockfd < 0)
    {
        perror("socket");
        return EXIT_FAILURE;
    }

    memset(&server_addr, 0, sizeof(server_addr));

    server_addr.sin_family = AF_INET;
    server_addr.sin_port = htons(SERVER_PORT);

    if (inet_pton(AF_INET,
                SERVER_IP,
                &server_addr.sin_addr) != 1)
    {
        perror("inet_pton");
        close(sockfd);
        return EXIT_FAILURE;
    }

    sent_bytes = sendto(
        sockfd,
        message,
        strlen(message),
        0,
        (struct sockaddr *)&server_addr,
        sizeof(server_addr)
    );

    if(sent_bytes < 0)
    {
        perror("sendto");
        close(sockfd);
        return EXIT_FAILURE;
    }

    printf("Sent %zd bytes to %s:%d\n",
            sent_bytes,
            SERVER_IP,
            SERVER_PORT);

    close(sockfd);

    printf("Press Enter to close socket...\n");
    getchar();
    return 0;
}