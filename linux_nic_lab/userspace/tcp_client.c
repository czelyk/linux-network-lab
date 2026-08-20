/*
 * tcp_client.c - minimal TCP client used to drive traffic through the
 * edunic driver under a connection-oriented protocol.
 *
 * Usage: tcp_client <server-ip> [port] [message]
 *
 * connect() below is where the three-way handshake happens:
 *   1. kernel sends a SYN segment    (TX path: -> ndo_start_xmit())
 *   2. kernel receives a SYN-ACK     (RX path: driver IRQ -> NAPI -> skb)
 *   3. kernel sends the final ACK    (TX path again)
 * connect() does not return until step 2 has happened and step 3 has been
 * queued; the connection is ESTABLISHED by the time this program's code
 * resumes after the call.
 *
 * Every subsequent write()/read() below rides the same TX/RX driver paths
 * as UDP does -- TCP's contribution (sequence numbers, ACKs, retransmit
 * timers, congestion control) all lives in the kernel's TCP state machine,
 * invisible here, but every byte it sends or receives still physically
 * moves through edunic_start_xmit() / edunic_clean_rx_irq() exactly like a
 * UDP datagram does.
 */
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <unistd.h>
#include <errno.h>

#include <sys/socket.h>
#include <netinet/in.h>
#include <arpa/inet.h>

#define DEFAULT_PORT	5001
#define BUF_SIZE	4096

int main(int argc, char *argv[])
{
	int sockfd;
	int port = DEFAULT_PORT;
	const char *server_ip;
	const char *message = "hello from tcp_client";
	struct sockaddr_in server_addr;
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

	sockfd = socket(AF_INET, SOCK_STREAM, 0);
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

	printf("tcp_client: connecting to %s:%d ...\n", server_ip, port);

	/* See file header: this is where SYN / SYN-ACK / ACK happen. */
	if (connect(sockfd, (struct sockaddr *)&server_addr,
		    sizeof(server_addr)) < 0) {
		perror("connect");
		close(sockfd);
		return EXIT_FAILURE;
	}
	printf("tcp_client: connected (three-way handshake complete)\n");

	n = write(sockfd, message, strlen(message));
	if (n < 0) {
		perror("write");
		close(sockfd);
		return EXIT_FAILURE;
	}
	printf("tcp_client: sent %zd bytes: \"%s\"\n", n, message);

	n = read(sockfd, buf, sizeof(buf) - 1);
	if (n < 0) {
		perror("read");
		close(sockfd);
		return EXIT_FAILURE;
	}
	if (n == 0) {
		printf("tcp_client: server closed the connection\n");
	} else {
		buf[n] = '\0';
		printf("tcp_client: received %zd bytes back: \"%s\"\n", n, buf);
	}

	/* close(): kernel sends a FIN, driving the connection through
	 * FIN_WAIT/TIME_WAIT on this side -- one more trip through the same
	 * TX path for the FIN segment, and RX path for the peer's ACK/FIN.
	 */
	close(sockfd);
	return 0;
}
