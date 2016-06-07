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
#include <mutex>          // std::mutex, std::try_lock

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
char speakercat[20] = {"/avr/audio"};
std::mutex strlock;
char telresp[255];
int volstate = -1; // Set to -1 to force us to get the real current value
int pwrstate = -1;

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
		// printf("Received: %.*s", (int)ev->data.size, ev->data.buffer);
		fflush(stdout);
		strlock.lock();
		snprintf(telresp,sizeof(telresp),"%.*s", (int)ev->data.size, ev->data.buffer);
		strlock.unlock();
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

static void avrcmd(char const *cmd, char const *lvl)
{
	char avrbuf[8];
	int laststate, volvl, i, diff;
	memset(&avrbuf, 0, sizeof(avrbuf));
	// It's possible that someone else changed the volume and power state, so get the current values
	volstate = -1;
	strcpy(avrbuf, "?V\r");
	for (i = 0; i != 3; ++i) telnet_send(telnet, avrbuf + i, 1);
	while(volstate == -1); // Wait for response from the receiver
	pwrstate = -1;
	strcpy(avrbuf, "?P\r");
	for (i = 0; i != 3; ++i) telnet_send(telnet, avrbuf + i, 1);
	while(pwrstate == -1); // Wait for response from the receiver
	if(!strcmp(cmd, "on"))
	{
		if(pwrstate != 0)
		{
			/*
			If the power isn't on, then we're expected to
			turn on power first, set the input (to CD), and make bluetooth connection with the Dot
			If the power was already on, then we're just expected to set the volume
			*/
			strcpy(avrbuf, "PO\r");
			for (i = 0; i != 3; ++i) telnet_send(telnet, avrbuf + i, 1);
			strcpy(avrbuf, "01FN\r");
			for (i = 0; i != 5; ++i) telnet_send(telnet, avrbuf + i, 1);
// TODO: take out the hardcoded device address
		    system("/bin/echo -e 'connect 44:65:0D:EF:0E:8F\nquit' | /usr/bin/bluetoothctl");
		}
		if(lvl[0] != 0)
		{ // see ev_handler for why lvl[0] could be zero
			volvl = atoi(lvl);
			if(volvl > 70) volvl = 70;
			volvl = volvl*2 + 1; // ?V command returns 2x+1 value of what's shown on the receiver's display (lvl)
			/* The right way to set the volume (to 60, for example) is this way
			strcpy(avrbuf, "121VL\r");
			for (i = 0; i != 6; ++i) telnet_send(telnet, avrbuf + i, 1);
			This isn't working for the Pioneer VSX-1022 receiver (which is mine),
			so we have to change the volume this way
			*/
			diff = (volstate-volvl);
			if(diff < 0) strcpy(avrbuf, "VU\r"); // increase the volume by 2
			else strcpy(avrbuf, "VD\r"); // decrease the volume by two
			diff = abs(diff);
			while (diff !=0)
			{
				laststate = volstate;
				for (i = 0; i != 3; ++i) telnet_send(telnet, avrbuf + i, 1);
				while (laststate == volstate);
				diff -= 2; // Every change to volstate is by 2
			}
		}
	}
	if(!strcmp(cmd, "off"))
	{
		/*
		We don't care if the power is already off. We're going to send the power off command
		and lower the volume so we don't accidently deafen someone the next time the receiver
		is turned on. Hopefully, the Dot won't care if we send a bluetooth disconnect command
		if we're not connected.
		*/
		strcpy(avrbuf, "PF\r");
		for (i = 0; i != 4; ++i) telnet_send(telnet, avrbuf + i, 1);
		volvl = atoi(lvl)*2 + 1; // ?V command returns 2x+1 value of what's shown on the receiver's display (lvl)
		diff = (volstate-volvl);
		if(diff < 0) strcpy(avrbuf, "VU\r"); // increase the volume by 2
		else strcpy(avrbuf, "VD\r"); // decrease the volume by two
		diff = abs(diff);
		while (diff !=0)
		{
			laststate = volstate;
			for (i = 0; i != 3; ++i) telnet_send(telnet, avrbuf + i, 1);
			while (laststate == volstate);
			diff -= 2; // Every change to volstate is by 2
		}
		system("/bin/echo -e 'disconnect 44:65:0D:EF:0E:8F\nquit' | /usr/bin/bluetoothctl");
	}
}

static int ev_handler(struct mg_connection *conn, enum mg_event ev)
{
/*
   rest URIs take the form of http://my.ip:8001/avr/audio?state=on&level=50 or
   http://my.ip:8001/avr/audio?state=off
*/
	switch (ev)
	{
		case MG_AUTH: return MG_TRUE;
		case MG_REQUEST:
			// printf("Request=%s, URI=%s, Query=%s\n", conn->request_method, conn->uri, conn->query_string);
			if(conn->content_len > 0)
			{
				std::string cbuff = std::string(conn->content, conn->content_len);
				// printf("content =%s\n", cbuff.c_str());
			}
			if(!strncmp(conn->uri, speakercat, strlen(speakercat)))
			{
				char spkrstate[4];
				char spkrlvl[4];
				if(mg_get_var(conn, "state", spkrstate, sizeof(spkrstate)) > -1)
				{ // normally, there will always be a "state" variable
					if(!strcmp(spkrstate, "on"))
					{
						mg_printf_data(conn, "Turning Speakers On");
						if(mg_get_var(conn, "level", spkrlvl, sizeof(spkrlvl)) > -1)
						{
							avrcmd("on", spkrlvl);
							mg_printf_data(conn, "To Volume Level %s\n",spkrlvl);
						}
						else
						{ // We didn't get a volume level with this command, so don't change the volume
							spkrlvl[0] = 0;
							avrcmd("on",spkrlvl);
							mg_printf_data(conn, "\n");
						}
					}
					if(!strcmp(spkrstate, "off"))
					{
						avrcmd("off", "037"); // 37 is a moderate level
						mg_printf_data(conn, "Turning Speakers Off\n");
					}
				}
				return MG_TRUE;
			}
			break;
		default: return MG_FALSE;
	}
	return 0;
}

static void* serve(void *server)
{
    // printf("Starting server poll\n");
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
	
	char avresp[255];
	memset(&telresp, 0, sizeof(telresp));
	
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
	mg_set_option(server, "listening_port", "8001");  // Open port 8001
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
		if (pfd[0].revents & POLLIN) {
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
		if(telresp[0] != 0) // Did the receiver send a message
		{
			if(strlock.try_lock())
			{
				strncpy(avresp,telresp,sizeof(telresp));
				telresp[0] = 0;
				strlock.unlock();
				if(!strncmp(avresp, "PWR0\r\n", 6)) pwrstate = 0; // Power is on
				// Documentation says that PWR1 is power off, but my receiver says PWR2
				if(!strncmp(avresp, "PWR2\r\n", 6)) pwrstate = 2; // Power is off (really standby)
				if(!strncmp(avresp, "VOL", 3)) volstate = atoi(avresp+3); // This number is 2x + 1 of what shows on display
			}
		}
	}
	/* clean up */
	telnet_free(telnet);
	close(sock);
	mg_destroy_server(&server);
	return(0);
}
