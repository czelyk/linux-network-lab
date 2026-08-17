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

    struct sockaddr_in local_addr;
    socklen_t local_addr_len;
    unsigned short local_port;

    struct sockaddr_in peer_addr;
    socklen_t peer_addr_len;
    unsigned short peer_port;
    char peer_ip[INET_ADDRSTRLEN];
    
    char local_ip[INET_ADDRSTRLEN];
    local_port = ntohs(local_addr.sin_port);

    if (inet_ntop(AF_INET,
                &local_addr.sin_addr,
                local_ip,
                sizeof(local_ip)) == NULL)
    {
        perror("inet_ntop");
        close(sockfd);
        return EXIT_FAILURE;
    }

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

    if(connect(sockfd,
                (struct sockaddr *)&server_addr,
                sizeof(server_addr)) < 0)
    {
        perror("connect");
        close(sockfd);
        return EXIT_FAILURE;
    }

    peer_addr_len = sizeof(peer_addr);

    if(getpeername(
            sockfd,
            (struct sockaddr *)&peer_addr,
            &peer_addr_len) <0)
    {
        perror("getpeername");
        close(sockfd);
        return EXIT_FAILURE;
    }

    peer_port = ntohs(peer_addr.sin_addr);

    if(inet_ntop(AF_INET,
                &peer_addr.sin_addr,
                peer_ip,
                sizeof(peer_ip)) == NULL)
    {
        perror("inet_ntop");
        close(sockfd);
        return EXIT_FAILURE;
    }

    printf("Peer endpoint: %s: %u\n",
            peer_ip,
            peer_port);

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

    local_addr_len = sizeof(local_addr);

    if(getsockname(sockfd,
                    (struct sockaddr *)&local_addr,
                    &local_addr_len) < 0)
    {
        perror("getsockname");
        close(sockfd);
        return EXIT_FAILURE;
    }

    local_port = ntohs(local_addr.sin_port);

    printf("Client local endpoint: %s: %u\n", 
        local_ip,
        local_port);

    printf("Sent %zd bytes to %s:%d\n",
            sent_bytes,
            SERVER_IP,
            SERVER_PORT);

    printf("Press Enter to close socket...\n");
    getchar();

    close(sockfd);

    return 0;
}