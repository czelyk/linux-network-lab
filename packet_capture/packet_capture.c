#include <stdio.h>
#include <stdlib.h>
#include <unistd.h>

#include <sys/socket.h>

#include <netinet/in.h>
#include <netinet/ip.h>
#include <net/ethernet.h>

#include <netinet/tcp.h>
#include <netinet/udp.h>

#define BUFFER_SIZE 65536

int main(void)
{
    int sockfd;
    unsigned char buffer[BUFFER_SIZE];
    unsigned int ip_header_len;

    ssize_t received_bytes;
    struct ethhdr *eth;
    struct iphdr *ip;

    struct tcphdr *tcp;
    struct udphdr *udp;

    char src_ip[INET_ADDRSTRLEN];
    char dst_ip[INET_ADDRSTRLEN];

    sockfd = socket(
        AF_PACKET,
        SOCK_RAW,
        htons(ETH_P_ALL)
    );

    if(sockfd < 0)
    {
        perror("socket");
        return EXIT_FAILURE;
    }

    printf("Waiting for packet...\n");

    received_bytes = recv(
            sockfd,
            buffer,
            sizeof(buffer),
            0
    );

    if(received_bytes < 0)
    {
        perror("recv");
        close(sockfd);
        return EXIT_FAILURE;
    }
    if(received_bytes <
        (ssize_t)(sizeof(struct ethhdr)))
    {
        fprintf(stderr, "Packet too small for Ethernet header\n");
        close(sockfd);
        return EXIT_FAILURE;
    }

    eth = (struct ethhdr *)buffer;

    printf("Captured packet: %zd bytes\n",
    received_bytes);


    printf("Destination MAC: %02x:%02x:%02x:%02x:%02x:%02x\n",
       eth->h_dest[0],
       eth->h_dest[1],
       eth->h_dest[2],
       eth->h_dest[3],
       eth->h_dest[4],
       eth->h_dest[5]);

    printf("Source MAC: %02x:%02x:%02x:%02x:%02x:%02x\n",
        eth->h_source[0],
        eth->h_source[1],
        eth->h_source[2],
        eth->h_source[3],
        eth->h_source[4],
        eth->h_source[5]);

    
    printf("EtherType: 0x%04x\n",
            ntohs(eth->h_proto));
    
    if(ntohs(eth->h_proto) == ETH_P_IP)
    {
        printf("IPV4 packet detected\n");

        if(received_bytes < (ssize_t)(sizeof(struct ethhdr) + sizeof(struct iphdr)))
        {
            fprintf(stderr, "Packet too small for IPv4 header\n");
            close(sockfd);
            return EXIT_FAILURE;
        }

        ip = (struct iphdr *)(buffer + sizeof(struct ethhdr));
        ip_header_len = ip->ihl * 4;

        if(inet_ntop(AF_INET,
                    &ip->saddr,
                    src_ip,
                    sizeof(src_ip)) == NULL)
        {
            perror("inet_ntop source");
            close(sockfd);
            return EXIT_FAILURE;
        }

        if(inet_ntop(AF_INET,
                &ip->daddr,
            dst_ip,
            sizeof(dst_ip)) == NULL)
        {
            perror("inet_ntop destination");
            close(sockfd);
            return EXIT_FAILURE;
        }

        printf("Source IP: %s\n", src_ip);
        printf("Destination IP %s\n", dst_ip);
        printf("IP protocol: %u\n", ip->protocol);

        if(ip->protocol == IPPROTO_TCP)
        {
            printf("TCP packet detected\n");

            ///////tcp
            
        }
    }

    close(sockfd);
    return EXIT_SUCCESS;
}