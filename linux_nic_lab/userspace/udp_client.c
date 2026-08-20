/*
 * udp_client.c - minimal UDP client used to drive traffic through the
 * edunic driver.
 *
 * Usage: udp_client <server-ip> [port] [message]
 *
 * TX path exercised by sendto() below:
 *   sendto() -> UDP checksum/header -> IP header + routing table lookup
 *   for a route to <server-ip> -> neighbour subsystem (ARP resolve of the
 *   next hop's link-layer address, if not already cached) -> Ethernet
 *   header prepended -> skb handed to the qdisc layer -> dequeued into
 *   ndo_start_xmit() (edunic_start_xmit(), nic_tx.c) -> TX descriptor ->
 *   DMA -> device -> wire.
 *
 * RX path exercised by the recvfrom() that follows, when the server on the
 * other end echoes the datagram back: wire -> NIC DMA -> RX descriptor ->
 * IRQ -> NAPI poll -> skb -> eth_type_trans() -> IP -> UDP -> this
 * process's socket receive queue -> recvfrom() returns.
 */
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <unistd.h>
#include <errno.h>

#include <sys/socket.h>
#include <netinet/in.h>
#include <arpa/inet.h>

#define DEFAULT_PORT	5000
#define BUF_SIZE	1500

int main(int argc, char *argv[])
{
	int sockfd;
	int port = DEFAULT_PORT;
	const char *server_ip;
	const char *message = "hello from udp_client";
	struct sockaddr_in server_addr;
	struct sockaddr_in from_addr;
	socklen_t from_len;
	char buf[BUF_SIZE];
	ssize_t n;

	if (argc < 2) {
		fprintf(stderr, "usage: %s <server-ip> [port] [message]\n",
			argv[0]);
		return EXIT_FAILURE;
	}
	server_ip = argv[1];
	if (argc > 2)
		port = atoi(argv[2]);
	if (argc > 3)
		message = argv[3];

	sockfd = socket(AF_INET, SOCK_DGRAM, 0);
	if (sockfd < 0) {
		perror("socket");
		return EXIT_FAILURE;
	}

	memset(&server_addr, 0, sizeof(server_addr));
	server_addr.sin_family = AF_INET;
	server_addr.sin_port = htons(port);
	if (inet_pton(AF_INET, server_ip, &server_addr.sin_addr) != 1) {
		fprintf(stderr, "invalid IPv4 address: %s\n", server_ip);
		close(sockfd);
		return EXIT_FAILURE;
	}

	/*
	 * sendto() with no prior connect(): this is a single, complete UDP
	 * TX operation. The kernel resolves the route and (if needed) the
	 * destination's link-layer address entirely within this call before
	 * handing the resulting skb to the driver.
	 */
	n = sendto(sockfd, message, strlen(message), 0,
		   (struct sockaddr *)&server_addr, sizeof(server_addr));
	if (n < 0) {
		perror("sendto");
		close(sockfd);
		return EXIT_FAILURE;
	}
	printf("udp_client: sent %zd bytes to %s:%d: \"%s\"\n", n, server_ip,
	       port, message);

	from_len = sizeof(from_addr);
	n = recvfrom(sockfd, buf, sizeof(buf) - 1, 0,
		     (struct sockaddr *)&from_addr, &from_len);
	if (n < 0) {
		perror("recvfrom");
		close(sockfd);
		return EXIT_FAILURE;
	}
	buf[n] = '\0';
	printf("udp_client: received %zd bytes back: \"%s\"\n", n, buf);

	close(sockfd);
	return 0;
}
