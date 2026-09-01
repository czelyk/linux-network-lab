#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <unistd.h>

#include <arpa/inet.h>
#include <sys/socket.h>

#define PORT 5001
#define SERVER_IP "127.0.0.1"

int main(void)
{
    int client_fd;

    struct sockaddr_in server_addr;

    char buffer[1024];
    char username[32];

    client_fd = socket(AF_INET, SOCK_STREAM, 0);

    if(client_fd < 0){
        perror("socket");
        return 1;
    }

    memset(&server_addr, 0, sizeof(server_addr));

    server_addr.sin_family = AF_INET;
    server_addr.sin_port = htons(PORT);
    server_addr.sin_addr.s_addr = inet_addr(SERVER_IP);

    if(connect(client_fd, (struct sockaddr *)&server_addr, sizeof(server_addr)) < 0){
        perror("connect");
        close(client_fd);
        return 1;
    }

    printf("Connected to server.\n");

    int n = recv(client_fd, buffer, sizeof(buffer) - 1, 0);

    if(n < 0){
        perror("recv");
        close(client_fd);
        return 1;
    }

    buffer[n] = '\0';
    printf("%s\n", buffer);

    if(fgets(username, sizeof(username), stdin) == NULL){
        perror("fgets");
        close(client_fd);
        return 1;
    }

    username[strcspn(username, "\n")] = '\0';

    if(send(client_fd, username, strlen(username), 0) < 0){
        perror("send");
        close(client_fd);
        return 1;
    }

    close(client_fd);

    return 0;
}