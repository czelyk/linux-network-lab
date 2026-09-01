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

    struct sockaddr_in server_addr;

    char client_ip[INET_ADDRSTRLEN];

    server_fd = socket(AF_INET, SOCK_STREAM, 0);

    if(server_fd < 0){
    perror("socket");
    return 1;
    }

    memset(&server_addr, 0, sizeof(server_addr));

    server_addr.sin_family = AF_INET;
    server_addr.sin_port = htons(PORT);
    server_addr.sin_addr.s_addr = INADDR_ANY;


    if(bind(server_fd, (struct sockaddr *)&server_addr, sizeof(server_addr)) < 0){
        perror("bind");
        close(server_fd);
        return 1;
    }

    if(listen(server_fd, 5) < 0){
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

    inet_ntop(AF_INET, &client_addr.sin_addr, client_ip, sizeof(client_ip));
    printf("Client connected: %s:%d\n", client_ip, ntohs(client_addr.sin_port));

    printf("Client accepted successfully.\n");

    close(client_fd);
    close(server_fd);

    
    return 0;
}