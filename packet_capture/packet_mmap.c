#include <stdio.h>
#include <stdlib.h>
#include <unistd.h>
#include <string.h>
#include <sys/mman.h>
#include <sys/socket.h>

#include <arpa/inet.h>
#include <net/ethernet.h>
#include <linux/if_packet.h>
#include <netinet/ip.h>

#define BLOCK_SIZE (1<< 20)
#define BLOCK_NR 4
#define FRAME_SIZE 2048

int main(void)
{
    int sockfd;
    int version;

    struct tpacket_req3 req;

    void *ring;
    size_t ring_size;

    unsigned int block_num = 0;
    struct tpacket_block_desc *block;

    struct tpacket3_hdr *packet;
    unsigned int packet_num;

    unsigned char *frame;
    struct ethhdr *eth;

    sockfd = socket(
        AF_PACKET,
        SOCK_RAW,
        htons(ETH_P_ALL)
    );

    if(sockfd < 0)
    {
        perror("sockfd < 0");
        return EXIT_FAILURE;
    }
    
    version = TPACKET_V3;

    if(setsockopt(
            sockfd,
            SOL_PACKET,
            PACKET_VERSION,
            &version,
            sizeof(version)) < 0)
    {
        perror("setsockopt PACKET_VERSION");
        close(sockfd);
        return EXIT_FAILURE;
    }

    memset(&req, 0, sizeof(req));

    req.tp_block_size = BLOCK_SIZE;
    req.tp_block_nr = BLOCK_NR;
    req.tp_frame_size = FRAME_SIZE;

    req.tp_frame_nr = 
        (req.tp_block_size *
        req.tp_block_nr) /
        req.tp_frame_size;

    req.tp_retire_blk_tov = 100;
    
    if (setsockopt(
            sockfd,
            SOL_PACKET,
            PACKET_RX_RING,
            &req,
            sizeof(req)) < 0)
    {
        perror("setsockopt PACKET_RX_RING");
        close(sockfd);
        return EXIT_FAILURE;
    }


    ring_size = 
        req.tp_block_size *
        req.tp_block_nr;

    ring = mmap(
        NULL,
        ring_size,
        PROT_READ | PROT_WRITE,
        MAP_SHARED,
        sockfd,
        0
    );

    if(ring == MAP_FAILED)
    {
        perror("mmap");
        close(sockfd);
        return EXIT_FAILURE;
    }

    printf("PACKET_MMAP ring created successfully\n");

    printf("Block size : %u bytes\n",
            req.tp_block_size);

    printf("Block count: %u\n",
            req.tp_block_nr);

    printf("Frame count: %u\n",
            req.tp_frame_nr);

    printf("Frame size: %u\n",
            req.tp_frame_size);

    printf("Ring size : %zu bytes\n",
            ring_size);

    printf("Waiting for packets...\n");

    while(1)
    {
        block = (struct tpacket_block_desc *)
                ((unsigned char *)ring +
                (block_num * req.tp_block_size));

        if(!(block->hdr.bh1.block_status & TP_STATUS_USER))
        {
            usleep(1000);
            continue;
        }

        printf("Block ready!\n");

        printf("Packets in block: %u\n",
                block->hdr.bh1.num_pkts);

        packet = (struct tpacket3_hdr *)
                ((unsigned char *)block + 
                block->hdr.bh1.offset_to_first_pkt);
        
        for(packet_num = 0;
            packet_num < block->hdr.bh1.num_pkts;
            packet_num++)
        {
            printf("Packet %u\n", packet_num + 1);

            printf(" Captured length: %u bytes\n",
                    packet->tp_snaplen);
            
            printf(" Original length: %u bytes\n",
                    packet->tp_len);

            frame = (unsigned char *)packet + packet->tp_mac;

            eth = (struct ethhdr *)frame;

            if(ntohs(eth->h_proto) == ETH_P_IP)
            {
                struct iphdr *ip;

                ip = (struct iphdr *)
                    (frame + sizeof(struct ethhdr));

                printf("IPv4 packet\n");
                printf("IP protocol: %u\n",
                    ip->protocol);
            }

            printf("  Destination MAC: %02x:%02x:%02x:%02x:%02x:%02x\n",
                eth->h_dest[0],
                eth->h_dest[1],
                eth->h_dest[2],
                eth->h_dest[3],
                eth->h_dest[4],
                eth->h_dest[5]);

            printf("  Source MAC: %02x:%02x:%02x:%02x:%02x:%02x\n",
                eth->h_source[0],
                eth->h_source[1],
                eth->h_source[2],
                eth->h_source[3],
                eth->h_source[4],
                eth->h_source[5]);

            printf("  EtherType: 0x%04x\n",
                ntohs(eth->h_proto));

            if(packet->tp_next_offset == 0)
                break;

            packet = (struct tpacket3_hdr *)
                    ((unsigned char *)packet +
                    packet->tp_next_offset);
        }

        block->hdr.bh1.block_status = TP_STATUS_KERNEL;

        block_num = 
            (block_num + 1) % req.tp_block_nr;

    }


    munmap(ring, ring_size);
    close(sockfd);

    return EXIT_SUCCESS;
}