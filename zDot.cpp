/* zDot - Echo Dot companion */
#if !defined(_POSIX_SOURCE)
#	define _POSIX_SOURCE
#endif
#if !defined(_BSD_SOURCE)
#	define _BSD_SOURCE
#endif

#include <sys/types.h>
#include <sys/socket.h>
#include <netinet/in.h>
#include <arpa/inet.h>
#include <netdb.h>
#include <poll.h>
#include <errno.h>
#include <stdio.h>
#include <string.h>
#include <string>
#include <stdlib.h>
#include <ctype.h>
#include <unistd.h>

#ifdef HAVE_ZLIB
#include "zlib.h"
#endif

#include "libtelnet.h"
#include "mongoose.h"

using namespace std;
static telnet_t *telnet;
static int do_echo;

static const telnet_telopt_t telopts[] = {
	{ TELNET_TELOPT_ECHO,		TELNET_WONT, TELNET_DO   },
	{ TELNET_TELOPT_TTYPE,		TELNET_WILL, TELNET_DONT },
	{ TELNET_TELOPT_COMPRESS2,	TELNET_WONT, TELNET_DO   },
	{ TELNET_TELOPT_MSSP,		TELNET_WONT, TELNET_DO   },
	{ -1, 0, 0 }
};

static const char *s_http_port = "8001";
// static struct mg_serve_http_opts s_http_server_opts;
char speakercat[20] = {"/avr/audio"};

static void _send(int sock, const char *buffer, size_t size) {
	int rs;

	/* send data */
	while (size > 0) {
		if ((rs = send(sock, buffer, size, 0)) == -1) {
			fprintf(stderr, "send() failed: %s\n", strerror(errno));
			exit(1);
		} else if (rs == 0) {
			fprintf(stderr, "send() unexpectedly returned 0\n");
			exit(1);
		}

		/* update pointer and size to see if we've got more to send */
		buffer += rs;
		size -= rs;
	}
}

static void _event_handler(telnet_t *telnet, telnet_event_t *ev,
		void *user_data) {
	int sock = *(int*)user_data;

	switch (ev->type) {
	/* data received */
	case TELNET_EV_DATA:
		printf("%.*s", (int)ev->data.size, ev->data.buffer);
		fflush(stdout);
		break;
	/* data must be sent */
	case TELNET_EV_SEND:
		_send(sock, ev->data.buffer, ev->data.size);
		break;
	/* request to enable remote feature (or receipt) */
	case TELNET_EV_WILL:
		/* we'll agree to turn off our echo if server wants us to stop */
		if (ev->neg.telopt == TELNET_TELOPT_ECHO)
			do_echo = 0;
		break;
	/* notification of disabling remote feature (or receipt) */
	case TELNET_EV_WONT:
		if (ev->neg.telopt == TELNET_TELOPT_ECHO)
			do_echo = 1;
		break;
	/* request to enable local feature (or receipt) */
	case TELNET_EV_DO:
		break;
	/* demand to disable local feature (or receipt) */
	case TELNET_EV_DONT:
		break;
	/* respond to TTYPE commands */
	case TELNET_EV_TTYPE:
		/* respond with our terminal type, if requested */
		if (ev->ttype.cmd == TELNET_TTYPE_SEND) {
			telnet_ttype_is(telnet, getenv("TERM"));
		}
		break;
	/* respond to particular subnegotiations */
	case TELNET_EV_SUBNEGOTIATION:
		break;
	/* error */
	case TELNET_EV_ERROR:
		fprintf(stderr, "ERROR: %s\n", ev->error.msg);
		exit(1);
	default:
		/* ignore */
		break;
	}
}

static void avrcmd(char *cmd)
{
	char avrbuf[8];
	int i;
	memset(&avrbuf, 0, sizeof(avrbuf));
	if(!strcmp(cmd, "on"))
	{
		strcpy(avrbuf, "PO\r");
		for (i = 0; i != 3; ++i) telnet_send(telnet, avrbuf + i, 1);
		strcpy(avrbuf, "01FN\r");
		for (i = 0; i != 5; ++i) telnet_send(telnet, avrbuf + i, 1);
		strcpy(avrbuf, "121VL\r");
		for (i = 0; i != 6; ++i) telnet_send(telnet, avrbuf + i, 1);
		system("/bin/echo -e 'connect 44:65:0D:EF:0E:8F\nquit' | /usr/bin/bluetoothctl");
	}
	if(!strcmp(cmd, "off"))
	{
		strcpy(avrbuf, "091VL\r");
		for(i = 0; i != 6; ++i) telnet_send(telnet, avrbuf + i, 1);
		strcpy(avrbuf, "PF\r\n");
		for (i = 0; i != 4; ++i) telnet_send(telnet, avrbuf + i, 1);
		system("/bin/echo -e 'disconnect 44:65:0D:EF:0E:8F\nquit' | /usr/bin/bluetoothctl");
	}
}

static int ev_handler(struct mg_connection *conn, enum mg_event ev)
{
	switch (ev)
	{
		case MG_AUTH: return MG_TRUE;
		case MG_REQUEST:
		printf("Request=%s, URI=%s, Query=%s\n", conn->request_method, conn->uri, conn->query_string);
			if(conn->content_len > 0)
			{
				std::string cbuff = std::string(conn->content, conn->content_len);
				printf("content =%s\n", cbuff.c_str());
			}
			if(!strncmp(conn->uri, speakercat, strlen(speakercat)))
			{
				char spkrstate[4];
				mg_get_var(conn, "state", spkrstate, sizeof(spkrstate));
				if(!strcmp(spkrstate, "on")) avrcmd("on");
				if(!strcmp(spkrstate, "off")) avrcmd("off");
				mg_printf_data(conn, "Turning Speakers %s\n", spkrstate);
				return MG_TRUE;
			}
			break;
		default: return MG_FALSE;
	}
	return 0;
}

static void* serve(void *server)
{
    printf("Starting server poll\n");
    for(;;) mg_poll_server((struct mg_server *) server, 1000);
    return NULL;
}

int main(int argc, char* argv[])
{
	char buffer[512];
	int rs;
	int sock;
	struct sockaddr_in addr;
	struct pollfd pfd[1];
	struct addrinfo *ai;
	struct addrinfo hints;

	struct mg_server *server = mg_create_server(NULL, ev_handler);
	
	/* check usage */
	if (argc != 3) {
		fprintf(stderr, "Usage:\n ./telnet-client <host> <port>\n");
		return 1;
	}

	/* look up server host */
	memset(&hints, 0, sizeof(hints));
	hints.ai_family = AF_UNSPEC;
	hints.ai_socktype = SOCK_STREAM;
	if ((rs = getaddrinfo(argv[1], argv[2], &hints, &ai)) != 0) {
		fprintf(stderr, "getaddrinfo() failed for %s: %s\n", argv[1],
				gai_strerror(rs));
		return 1;
	}
	
	/* create server socket */
	if ((sock = socket(AF_INET, SOCK_STREAM, 0)) == -1) {
		fprintf(stderr, "socket() failed: %s\n", strerror(errno));
		return 1;
	}

	/* bind server socket */
	memset(&addr, 0, sizeof(addr));
	addr.sin_family = AF_INET;
	if (bind(sock, (struct sockaddr *)&addr, sizeof(addr)) == -1) {
		fprintf(stderr, "bind() failed: %s\n", strerror(errno));
		return 1;
	}

	/* connect */
	if (connect(sock, ai->ai_addr, ai->ai_addrlen) == -1) {
		fprintf(stderr, "connect() failed: %s\n", strerror(errno));
		return 1;
	}

	/* free address lookup info */
	freeaddrinfo(ai);


	mg_set_option(server, "document_root", ".");      // Serve current directory
	mg_set_option(server, "listening_port", "8001");  // Open port 8000
	mg_start_thread(serve, server);
	/* initialize telnet box */
	telnet = telnet_init(telopts, _event_handler, 0, &sock);

	/* initialize poll descriptors */
	memset(pfd, 0, sizeof(pfd));
	pfd[0].fd = sock;
	pfd[0].events = POLLIN;

	/* loop while connection is open */
	while (poll(pfd, 1, -1) != -1) {
		/* read from client */
		if (pfd[1].revents & POLLIN) {
			if ((rs = recv(sock, buffer, sizeof(buffer), 0)) > 0) {
				telnet_recv(telnet, buffer, rs);
			} else if (rs == 0) {
				break;
			} else {
				fprintf(stderr, "recv(client) failed: %s\n",
						strerror(errno));
				exit(1);
			}
		}
	}
	/* clean up */
	telnet_free(telnet);
	close(sock);
	mg_destroy_server(&server);
	return(0);
}
