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

    server_fd = socket(AF_INET, SOCK_STREAM, 0);

    memset(&server_addr, 0, sizeof(server_addr));


    server_addr.sin_family = AF_INET;
    server_addr.sin_port = htons(PORT);
    server_addr.sin_addr.s_addr = INADDR_ANY;

    if(server_fd < 0){
        perror("socket");
        exit(EXIT_FAILURE);
    }
    
    return 0;
}