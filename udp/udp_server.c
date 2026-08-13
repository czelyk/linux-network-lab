#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <unistd.h>

#include <sys/socket.h>
#include <netinet/in.h>
#include <arpa/inet.h>

#define PORT 5000
#define BUFFER_SIZE 1024

int main(void)
{
    int sockfd;
    struct sockaddr_in server_addr;
    char buffer[BUFFER_SIZE];

    struct sockaddr_in client_addr;
    socklen_t client_addr_len;
    ssize_t received_bytes;

    unsigned short client_port;

    sockfd = socket(AF_INET, SOCK_DGRAM, 0);

    if(sockfd < 0)
    {
        perror("socket");
        return EXIT_FAILURE;
    }

    memset(&server_addr, 0, sizeof(server_addr));

    server_addr.sin_family = AF_INET;
    server_addr.sin_port = htons(PORT);
    server_addr.sin_addr.s_addr = htonl(INADDR_ANY);

    if(bind(sockfd,
            (struct sockaddr *)&server_addr,
            sizeof(server_addr)) < 0)
    {
        perror("bind");
        close(sockfd);
        return EXIT_FAILURE;
    }

    printf("UDP server listening on port %d...\n", PORT);
    client_addr_len=sizeof(client_addr);

    received_bytes = recvfrom(
        sockfd,
        buffer,
        sizeof(buffer) -1,
        0,
        (struct sockaddr *)&client_addr,
        &client_addr_len
    );

    if(received_bytes < 0)
    {
        perror("recvfrom");
        close(sockfd);
        return EXIT_FAILURE;
    }

    buffer[received_bytes] = '\0';


    client_port = ntohs(client_addr.sin_port);


    char client_ip[INET_ADDRSTRLEN];

    if(inet_ntop(AF_INET,
                &client_addr.sin_addr,
                client_ip,
                sizeof(client_ip)) == NULL)
    {
        perror("inet_ntop");
        close(sockfd);
        return EXIT_FAILURE;
    }

    printf("Received %zd bytes from %s:%u: %s\n",
        received_bytes,
        client_ip,
        client_port,
        buffer);

    close(sockfd);

    return 0;
}