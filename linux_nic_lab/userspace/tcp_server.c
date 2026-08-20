/*
 * tcp_server.c - minimal TCP echo server used to exercise the edunic
 * driver's RX/TX paths under a connection-oriented protocol.
 *
 * Where this fits in the packet path (see README "Full TCP TX/RX path"
 * diagram): everything below still bottoms out through the exact same
 * ndo_start_xmit() / NAPI-poll driver entry points as the UDP tools do.
 * What TCP adds on top is state: a three-way handshake before any
 * application data can flow, sequence numbers and ACKs on every segment,
 * and retransmission if an ACK doesn't arrive in time -- all of it
 * implemented in the kernel's TCP state machine (net/ipv4/tcp*.c), never
 * visible to this program. From this process's point of view, accept()
 * simply doesn't return until that handshake has already completed.
 *
 *   listen()  -> socket enters LISTEN, SYN queue + accept queue created
 *   (peer's SYN arrives via the driver's RX path, kernel replies SYN-ACK
 *    via the driver's TX path, peer's ACK arrives via RX path)
 *   accept()  -> returns a new connected socket once that handshake
 *                machinery has moved the connection to the accept queue
 *   read()/write() -> ordinary segment exchange, each segment still a
 *                normal TX or RX trip through the driver
 */
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <unistd.h>
#include <signal.h>
#include <errno.h>

#include <sys/socket.h>
#include <netinet/in.h>
#include <netinet/tcp.h>
#include <arpa/inet.h>

#define DEFAULT_PORT	5001
#define BUF_SIZE	4096
#define BACKLOG		16

static volatile sig_atomic_t running = 1;

static void handle_sigint(int sig)
{
	(void)sig;
	running = 0;
}

int main(int argc, char *argv[])
{
	int listen_fd;
	int port = DEFAULT_PORT;
	int optval = 1;
	struct sockaddr_in server_addr;

	if (argc > 1)
		port = atoi(argv[1]);

	setvbuf(stdout, NULL, _IOLBF, 0); /* line-buffer even when redirected */
	signal(SIGINT, handle_sigint);
	signal(SIGPIPE, SIG_IGN); /* a client closing mid-write should not kill us */

	listen_fd = socket(AF_INET, SOCK_STREAM, 0);
	if (listen_fd < 0) {
		perror("socket");
		return EXIT_FAILURE;
	}

	/* Let the port be reused immediately after this process exits,
	 * instead of sitting in TIME_WAIT -- convenience for repeated test
	 * runs, not part of the protocol path.
	 */
	setsockopt(listen_fd, SOL_SOCKET, SO_REUSEADDR, &optval, sizeof(optval));

	memset(&server_addr, 0, sizeof(server_addr));
	server_addr.sin_family = AF_INET;
	server_addr.sin_addr.s_addr = htonl(INADDR_ANY);
	server_addr.sin_port = htons(port);

	if (bind(listen_fd, (struct sockaddr *)&server_addr,
		 sizeof(server_addr)) < 0) {
		perror("bind");
		close(listen_fd);
		return EXIT_FAILURE;
	}

	/*
	 * listen(): transitions the socket to LISTEN state and tells the
	 * kernel how many fully-handshaked-but-not-yet-accept()ed
	 * connections (the accept queue) it may hold. From here on, any
	 * inbound SYN the driver's RX path delivers for this port is
	 * handled by the kernel's TCP state machine without this process
	 * being scheduled at all.
	 */
	if (listen(listen_fd, BACKLOG) < 0) {
		perror("listen");
		close(listen_fd);
		return EXIT_FAILURE;
	}

	printf("tcp_server: listening on 0.0.0.0:%d (Ctrl-C to stop)\n", port);

	while (running) {
		struct sockaddr_in client_addr;
		socklen_t client_len = sizeof(client_addr);
		char ip_str[INET_ADDRSTRLEN];
		char buf[BUF_SIZE];
		int conn_fd;

		/*
		 * accept(): dequeues one already-established connection.
		 * The SYN / SYN-ACK / ACK handshake for it happened entirely
		 * before this call returned -- see the file header comment.
		 */
		conn_fd = accept(listen_fd, (struct sockaddr *)&client_addr,
				  &client_len);
		if (conn_fd < 0) {
			if (errno == EINTR)
				continue;
			perror("accept");
			break;
		}

		inet_ntop(AF_INET, &client_addr.sin_addr, ip_str, sizeof(ip_str));
		printf("tcp_server: connection from %s:%u\n", ip_str,
		       ntohs(client_addr.sin_port));

		for (;;) {
			ssize_t n = read(conn_fd, buf, sizeof(buf));

			if (n < 0) {
				perror("read");
				break;
			}
			if (n == 0) {
				/* Peer sent FIN: orderly close. */
				printf("tcp_server: %s:%u closed the connection\n",
				       ip_str, ntohs(client_addr.sin_port));
				break;
			}

			printf("tcp_server: %zd bytes from %s:%u\n", n,
			       ip_str, ntohs(client_addr.sin_port));

			/* Echo it straight back. Each write() here may be
			 * split by the kernel into one or more TCP segments,
			 * each an independent trip through ndo_start_xmit().
			 */
			if (write(conn_fd, buf, (size_t)n) < 0) {
				perror("write");
				break;
			}
		}

		close(conn_fd);
	}

	close(listen_fd);
	printf("tcp_server: exiting\n");
	return 0;
}
