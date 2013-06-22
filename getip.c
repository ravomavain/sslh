#include <stdio.h>
#include <stdlib.h>
#include <errno.h>
#include <string.h>
#include <sys/types.h>
#include <sys/socket.h>
#include <sys/un.h>
#include <arpa/inet.h>
#include <unistd.h>

int main(int argc, char **argv)
{
	int s, t, len;
	struct sockaddr_un remote;

	if (argc < 3)
	{
		printf("Usage: %s /path/to/sslh.sock <port>\n", argv[0]);
		exit(1);
	}

	if ((s = socket(AF_UNIX, SOCK_STREAM, 0)) == -1) {
		perror("socket");
		exit(1);
	}

	remote.sun_family = AF_UNIX;
	strcpy(remote.sun_path, argv[1]);
	len = strlen(remote.sun_path) + sizeof(remote.sun_family);
	if (connect(s, (struct sockaddr *)&remote, len) == -1) {
		perror("connect");
		exit(1);
	}

	uint16_t port = htons(atoi(argv[2]));

	if (send(s, &port, sizeof port, 0) == -1) {
		perror("send");
		exit(1);
	}

	uint32_t ip;

	if ((t=recv(s, &ip, sizeof ip, 0)) > 0) {
		struct in_addr addr;
		addr.s_addr = ip;
		printf("%s\n", inet_ntoa(addr));
	} else {
		if (t < 0) perror("recv");
		else fprintf(stderr, "Server closed connection\n");
		exit(1);
	}

	close(s);

	return 0;
}
