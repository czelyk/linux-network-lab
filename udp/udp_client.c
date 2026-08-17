#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <unistd.h>
#include <poll.h>
#include <fcntl.h>
#include <errno.h>

#include <sys/socket.h>
#include <netinet/in.h>
#include <arpa/inet.h>

#define SERVER_IP "127.0.0.1"
#define SERVER_PORT 5000
#define BUFFER_SIZE 1024
#define POLL_TIMEOUT_MS 3000

int main(void)
{
    int sockfd;
    int flags;
    int poll_ret;

    struct sockaddr_in server_addr;

    struct sockaddr_in local_addr;
    socklen_t local_addr_len;
    unsigned short local_port;
    char local_ip[INET_ADDRSTRLEN];

    struct sockaddr_in peer_addr;
    socklen_t peer_addr_len;
    unsigned short peer_port;
    char peer_ip[INET_ADDRSTRLEN];

    struct pollfd pfd;

    const char *message = "merhaba";
    ssize_t sent_bytes;

    char reply_buffer[BUFFER_SIZE];
    ssize_t received_bytes;

    sockfd = socket(AF_INET, SOCK_DGRAM, 0);

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

    /*
     * UDP socket'i belirli bir peer ile ilişkilendiriyoruz.
     * UDP olduğu için TCP handshake gerçekleşmez.
     */
    if(connect(sockfd,
               (struct sockaddr *)&server_addr,
               sizeof(server_addr)) < 0)
    {
        perror("connect");
        close(sockfd);
        return EXIT_FAILURE;
    }

    /*
     * Socket'in peer endpoint'ini kernel'den öğreniyoruz.
     */
    peer_addr_len = sizeof(peer_addr);

    if(getpeername(sockfd,
                   (struct sockaddr *)&peer_addr,
                   &peer_addr_len) < 0)
    {
        perror("getpeername");
        close(sockfd);
        return EXIT_FAILURE;
    }

    peer_port = ntohs(peer_addr.sin_port);

    if(inet_ntop(AF_INET,
                 &peer_addr.sin_addr,
                 peer_ip,
                 sizeof(peer_ip)) == NULL)
    {
        perror("inet_ntop");
        close(sockfd);
        return EXIT_FAILURE;
    }

    printf("Peer endpoint: %s:%u\n",
           peer_ip,
           peer_port);

    /*
     * Socket'in local endpoint'ini kernel'den öğreniyoruz.
     * connect() sonrasında kernel route/local endpoint seçimini
     * yapmış durumda.
     */
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

    /*
     * Socket'i non-blocking moda geçiriyoruz.
     */
    flags = fcntl(sockfd, F_GETFL, 0);

    if(flags < 0)
    {
        perror("fcntl F_GETFL");
        close(sockfd);
        return EXIT_FAILURE;
    }

    if(fcntl(sockfd, F_SETFL, flags | O_NONBLOCK) < 0)
    {
        perror("fcntl F_SETFL");
        close(sockfd);
        return EXIT_FAILURE;
    }

    /*
     * Connected UDP socket olduğu için send() kullanabiliyoruz.
     * Destination zaten connect() ile socket'e kaydedildi.
     */
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

    printf("Sent %zd bytes to %s:%d\n",
           sent_bytes,
           SERVER_IP,
           SERVER_PORT);

    /*
     * recv() çağrısını busy-loop içinde sürekli denemek yerine,
     * kernel'e socket okunabilir hale geldiğinde bize haber
     * vermesini söylüyoruz.
     */
    pfd.fd = sockfd;
    pfd.events = POLLIN;
    pfd.revents = 0;

    poll_ret = poll(
        &pfd,
        1,
        POLL_TIMEOUT_MS
    );

    if(poll_ret < 0)
    {
        perror("poll");
        close(sockfd);
        return EXIT_FAILURE;
    }

    if(poll_ret == 0)
    {
        printf("Timeout: server did not reply within %d ms.\n",
               POLL_TIMEOUT_MS);

        close(sockfd);
        return EXIT_FAILURE;
    }

    if(pfd.revents & POLLIN)
    {
        received_bytes = recv(
            sockfd,
            reply_buffer,
            sizeof(reply_buffer) - 1,
            0
        );

        if(received_bytes < 0)
        {
            if(errno == EAGAIN || errno == EWOULDBLOCK)
            {
                printf("Socket is not readable right now.\n");
            }
            else
            {
                perror("recv");
            }

            close(sockfd);
            return EXIT_FAILURE;
        }

        reply_buffer[received_bytes] = '\0';

        printf("Received %zd bytes from server: %s\n",
               received_bytes,
               reply_buffer);
    }

    printf("Press Enter to close socket...\n");
    getchar();

    close(sockfd);

    return 0;
}