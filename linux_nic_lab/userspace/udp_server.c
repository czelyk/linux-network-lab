/*
 * udp_server.c - minimal UDP echo server used to exercise the edunic
 * driver's full RX and TX paths end to end.
 *
 * Where this fits in the packet path (see README "Full UDP TX/RX path"
 * diagram):
 *
 *   Every recvfrom() below is the very last hop of:
 *     wire -> NIC DMA -> RX descriptor -> IRQ -> NAPI -> skb -> Ethernet ->
 *     IP -> UDP -> port/socket lookup -> this process's receive queue.
 *
 *   Every sendto() below is the very first hop of:
 *     sendto() -> UDP -> IP -> routing -> neighbour/ARP -> Ethernet -> skb
 *     -> qdisc -> ndo_start_xmit() -> TX descriptor -> DMA -> NIC -> wire.
 *
 * Run this on the machine hosting the edunic interface, then run
 * udp_client against that interface's IP address. Watch `tcpdump -i
 * <iface>` in another terminal while it runs to see both directions.
 */
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <unistd.h>
#include <signal.h>
#include <errno.h>

#include <sys/socket.h>
#include <netinet/in.h>
#include <arpa/inet.h>

#define DEFAULT_PORT	5000
#define BUF_SIZE	1500 /* one standard-MTU Ethernet payload's worth */

static volatile sig_atomic_t running = 1;

static void handle_sigint(int sig)
{
	(void)sig;
	running = 0;
}

int main(int argc, char *argv[])
{
	int sockfd;
	int port = DEFAULT_PORT;
	struct sockaddr_in server_addr, client_addr;
	char buf[BUF_SIZE];

	if (argc > 1)
		port = atoi(argv[1]);

	setvbuf(stdout, NULL, _IOLBF, 0); /* line-buffer even when redirected */
	signal(SIGINT, handle_sigint);

	/* AF_INET + SOCK_DGRAM: a connectionless UDP socket. No handshake --
	 * the kernel is ready to receive the moment bind() succeeds.
	 */
	sockfd = socket(AF_INET, SOCK_DGRAM, 0);
	if (sockfd < 0) {
		perror("socket");
		return EXIT_FAILURE;
	}

	memset(&server_addr, 0, sizeof(server_addr));
	server_addr.sin_family = AF_INET;
	server_addr.sin_addr.s_addr = htonl(INADDR_ANY);
	server_addr.sin_port = htons(port);

	/*
	 * bind(): registers (protocol=UDP, local port) in the kernel's UDP
	 * hash table. This is exactly the table UDP RX processing (ip_rcv ->
	 * udp_rcv) consults via a port lookup to decide which socket's
	 * receive queue an incoming datagram belongs on -- the final step of
	 * the UDP RX path mentioned in the file header comment above.
	 */
	if (bind(sockfd, (struct sockaddr *)&server_addr,
		 sizeof(server_addr)) < 0) {
		perror("bind");
		close(sockfd);
		return EXIT_FAILURE;
	}

	printf("udp_server: listening on 0.0.0.0:%d (Ctrl-C to stop)\n", port);

	while (running) {
		socklen_t client_len = sizeof(client_addr);
		char ip_str[INET_ADDRSTRLEN];
		ssize_t n;

		/*
		 * recvfrom(): blocks until a datagram is queued on this
		 * socket's receive buffer. Everything before that queueing
		 * happened entirely in kernel space, ending with the driver
		 * handing an skb to napi_gro_receive() -- see nic_rx.c.
		 */
		n = recvfrom(sockfd, buf, sizeof(buf) - 1, 0,
			     (struct sockaddr *)&client_addr, &client_len);
		if (n < 0) {
			if (errno == EINTR)
				continue;
			perror("recvfrom");
			break;
		}
		buf[n] = '\0';

		inet_ntop(AF_INET, &client_addr.sin_addr, ip_str, sizeof(ip_str));
		printf("udp_server: %zd bytes from %s:%u: \"%s\"\n", n, ip_str,
		       ntohs(client_addr.sin_port), buf);

		/*
		 * sendto(): re-enters the kernel and walks the entire TX
		 * path back out through the same NIC (ndo_start_xmit() in
		 * nic_tx.c) before this call returns.
		 */
		if (sendto(sockfd, buf, (size_t)n, 0,
			   (struct sockaddr *)&client_addr, client_len) < 0)
			perror("sendto");
	}

	close(sockfd);
	printf("udp_server: exiting\n");
	return 0;
}
