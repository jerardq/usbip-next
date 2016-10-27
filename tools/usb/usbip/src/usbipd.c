/*
 * Copyright (C) 2015 Nobuo Iwata
 *               2011 matt mooney <mfm@muteddisk.com>
 *               2005-2007 Takahiro Hirofuchi
 * Copyright (C) 2015-2016 Samsung Electronics
 *               Igor Kotrasinski <i.kotrasinsk@samsung.com>
 *               Krzysztof Opasiak <k.opasiak@samsung.com>
 *
 * This program is free software: you can redistribute it and/or modify
 * it under the terms of the GNU General Public License as published by
 * the Free Software Foundation, either version 2 of the License, or
 * (at your option) any later version.
 *
 * This program is distributed in the hope that it will be useful,
 * but WITHOUT ANY WARRANTY; without even the implied warranty of
 * MERCHANTABILITY or FITNESS FOR A PARTICULAR PURPOSE. See the
 * GNU General Public License for more details.
 *
 * You should have received a copy of the GNU General Public License
 * along with this program. If not, see <http://www.gnu.org/licenses/>.
 */

#ifdef USBIP_WITH_LIBUSB
#include "usbip_sock.h"
#endif

#include "usbip_config.h"

#define _GNU_SOURCE

#include <errno.h>

#ifndef USBIP_OS_NO_SYS_SOCKET
#include <netdb.h>
#include <sys/types.h>
#include <sys/stat.h>
#include <arpa/inet.h>
#include <sys/socket.h>
#include <netinet/in.h>
#endif

#ifdef HAVE_LIBWRAP
#include <tcpd.h>
#endif

#include <getopt.h>
#include <signal.h>
#ifndef USBIP_OS_NO_POLL_H
#include <poll.h>
#endif

#include "usbip_common.h"
#include "usbip_network.h"
#include "usbipd.h"
#include "list.h"

#define MAXSOCKFD 20

#define MAIN_LOOP_TIMEOUT 10

static const char usbip_version_string[] = PACKAGE_STRING;

static const char usbipd_help_string[] =
	"usage: %s [options]\n"
	"\n"
	"	-4, --ipv4\n"
	"		Bind to IPv4. Default is both.\n"
	"\n"
	"	-6, --ipv6\n"
	"		Bind to IPv6. Default is both.\n"
	"\n"
#ifndef USBIP_DAEMON_APP
	"	-e, --device\n"
	"		Run in device mode.\n"
	"		Rather than drive an attached device, create\n"
	"		a virtual UDC to bind gadgets to.\n"
	"\n"
#endif
	"	-D, --daemon\n"
	"		Run as a daemon process.\n"
	"\n"
	"	-d, --debug\n"
	"		Print debugging information.\n"
	"\n"
#ifdef USBIP_WITH_LIBUSB
	"       -fHEX, --debug-flags HEX\n"
	"               Print flags for stub debugging.\n"
	"\n"
#endif
	"	-PFILE, --pid FILE\n"
	"		Write process id to FILE.\n"
	"		If no FILE specified, use %s.\n"
	"\n"
	"	-tPORT, --tcp-port PORT\n"
	"		Listen on TCP/IP port PORT.\n"
	"\n"
	"	-h, --help\n"
	"		Print this help.\n"
	"\n"
	"	-v, --version\n"
	"		Show version.\n";

static void usbipd_help(void)
{
	printf(usbipd_help_string, usbip_progname, usbip_default_pid_file);
}

#ifdef HAVE_LIBWRAP
static int tcpd_auth(int connfd)
{
	struct request_info request;
	int rc;

	request_init(&request, RQ_DAEMON, PROGNAME, RQ_FILE, connfd, 0);
	fromhost(&request);
	rc = hosts_access(&request);
	if (rc == 0)
		return -1;

	return 0;
}
#endif

static int do_accept(int listenfd, char *host, char *port)
{
	int connfd;
	struct sockaddr_storage ss;
	socklen_t len = sizeof(ss);
	int rc;

	memset(&ss, 0, sizeof(ss));

	connfd = accept(listenfd, (struct sockaddr *)&ss, &len);
	if (connfd < 0) {
		err("failed to accept connection");
		return -1;
	}

	rc = getnameinfo((struct sockaddr *)&ss, len, host, NI_MAXHOST,
			 port, NI_MAXSERV, NI_NUMERICHOST | NI_NUMERICSERV);
	if (rc)
		err("getnameinfo: %s", usbip_net_gai_strerror(rc));

#ifdef HAVE_LIBWRAP
	rc = tcpd_auth(connfd);
	if (rc < 0) {
		info("denied access from %s", host);
		socket_close(connfd);
		return -1;
	}
#endif
	info("connection from %s:%s", host, port);

	/* should set TCP_NODELAY for usbip */
	usbip_net_set_nodelay(connfd);

	return connfd;
}

#ifndef USBIP_OS_NO_FORK
int process_request(int listenfd)
{
	pid_t childpid;
	int connfd;
	struct usbip_sock sock;
	char host[NI_MAXHOST], port[NI_MAXSERV];

	connfd = do_accept(listenfd, host, port);
	if (connfd < 0)
		return -1;
	childpid = fork();
	if (childpid == 0) {
		socket_close(listenfd);

		usbip_sock_init(&sock, connfd, NULL, NULL, NULL, NULL);
		usbip_recv_pdu(&sock, host, port);
		socket_close(connfd);
		exit(0);
	}
	socket_close(connfd);
	return 0;
}
#else
struct request_data {
	int connfd;
	char host[NI_MAXHOST], port[NI_MAXSERV];
};

static void *__process_request(void *arg)
{
	struct request_data *data = (struct request_data *)arg;
	struct usbip_sock sock;

	usbip_sock_init(&sock, data->connfd, NULL, NULL, NULL, NULL);
	usbip_recv_pdu(&sock, data->host, data->port);
	socket_close(data->connfd);
	free(data);
	pthread_exit();
}

int process_request(int listenfd)
{
	pthread_t thread;
	struct request_data *data;

	data = (struct request_data *)calloc(1, sizeof(struct request_data));
	if (!data)
		return -1;

	data->connfd = do_accept(listenfd, data->host, data->port);
	if (data->connfd < 0) {
		free(data);
		return -1;
	}
	if (pthread_create(&thread, NULL, __process_request, data)) {
		free(data);
		return -1;
	}
	return 0;
}
#endif /* !USBIP_OS_NO_FORK */

static void addrinfo_to_text(struct addrinfo *ai, char buf[],
			     const size_t buf_size)
{
	char hbuf[NI_MAXHOST];
	char sbuf[NI_MAXSERV];
	int rc;

	buf[0] = '\0';

	rc = getnameinfo(ai->ai_addr, ai->ai_addrlen, hbuf, sizeof(hbuf),
			 sbuf, sizeof(sbuf), NI_NUMERICHOST | NI_NUMERICSERV);
	if (rc)
		err("getnameinfo: %s", usbip_net_gai_strerror(rc));

	snprintf(buf, buf_size, "%s:%s", hbuf, sbuf);
}

static int listen_all_addrinfo(struct addrinfo *ai_head, int sockfdlist[],
			     int maxsockfd)
{
	struct addrinfo *ai;
	int ret, nsockfd = 0;
	const size_t ai_buf_size = NI_MAXHOST + NI_MAXSERV + 2;
	char ai_buf[NI_MAXHOST + NI_MAXSERV + 2];

	for (ai = ai_head; ai && nsockfd < maxsockfd; ai = ai->ai_next) {
		int sock;

		addrinfo_to_text(ai, ai_buf, ai_buf_size);
		dbg("opening %s", ai_buf);
		sock = socket(ai->ai_family, ai->ai_socktype, ai->ai_protocol);
		if (sock < 0) {
			err("socket: %s: %d (%s)",
			    ai_buf, errno, strerror(errno));
			continue;
		}

		usbip_net_set_reuseaddr(sock);
		usbip_net_set_nodelay(sock);
		/* We use seperate sockets for IPv4 and IPv6
		 * (see do_standalone_mode()) */
		usbip_net_set_v6only(sock);

		if (sock >= FD_SETSIZE) {
			err("FD_SETSIZE: %s: sock=%d, max=%d",
			    ai_buf, sock, FD_SETSIZE);
			socket_close(sock);
			continue;
		}

		ret = bind(sock, ai->ai_addr, ai->ai_addrlen);
		if (ret < 0) {
			err("bind: %s: %d (%s)",
			    ai_buf, errno, strerror(errno));
			socket_close(sock);
			continue;
		}

		ret = listen(sock, SOMAXCONN);
		if (ret < 0) {
			err("listen: %s: %d (%s)",
			    ai_buf, errno, strerror(errno));
			socket_close(sock);
			continue;
		}

		info("listening on %s", ai_buf);
		sockfdlist[nsockfd++] = sock;
	}

	return nsockfd;
}

static struct addrinfo *do_getaddrinfo(const char *host, int ai_family)
{
	struct addrinfo hints, *ai_head;
	int rc;

	memset(&hints, 0, sizeof(hints));
	hints.ai_family   = ai_family;
	hints.ai_socktype = SOCK_STREAM;
	hints.ai_flags    = AI_PASSIVE;

	rc = getaddrinfo(host, usbip_port_string, &hints, &ai_head);
	if (rc) {
		err("failed to get a network address %s: %s", usbip_port_string,
		    usbip_net_gai_strerror(rc));
		return NULL;
	}

	return ai_head;
}

static void signal_handler(int i)
{
	dbg("received '%s' signal", strsignal(i));
	usbip_break_all_connections();
}

static void set_signal(void)
{
	struct sigaction act;

	memset(&act, 0, sizeof(act));
	act.sa_handler = signal_handler;
	sigemptyset(&act.sa_mask);
	sigaction(SIGTERM, &act, NULL);
	sigaction(SIGINT, &act, NULL);
	act.sa_handler = SIG_IGN;
	sigaction(SIGCLD, &act, NULL);
}

static const char *pid_file;

static void write_pid_file(void)
{
	FILE *fp;

	if (pid_file) {
		dbg("creating pid file %s", pid_file);

		fp = fopen(pid_file, "w");
		if (!fp) {
			err("pid_file: %s: %d (%s)",
			    pid_file, errno, strerror(errno));
			return;
		}
		fprintf(fp, "%d\n", getpid());
		fclose(fp);
	}
}

static void remove_pid_file(void)
{
	if (pid_file) {
		dbg("removing pid file %s", pid_file);
		unlink(pid_file);
	}
}

static int do_standalone_mode(int daemonize, int ipv4, int ipv6)
{
	struct addrinfo *ai_head;
	int sockfdlist[MAXSOCKFD];
	int nsockfd, family;
	int i, terminate;
	struct pollfd *fds;
	struct timespec timeout;
	sigset_t sigmask;

	if (usbip_open_driver())
		return -1;

	if (daemonize) {
#ifndef USBIP_OS_NO_DAEMON
		if (daemon(0, 0) < 0) {
			err("daemonizing failed: %s", strerror(errno));
			usbip_close_driver();
			return -1;
		}
		umask(0);
		usbip_use_syslog = 1;
#endif
	}
	set_signal();
	write_pid_file();

	info("starting " PROGNAME " (%s)", usbip_version_string);

	socket_start();

	/*
	 * To suppress warnings on systems with bindv6only disabled
	 * (default), we use seperate sockets for IPv6 and IPv4 and set
	 * IPV6_V6ONLY on the IPv6 sockets.
	 */
	if (ipv4 && ipv6)
		family = AF_UNSPEC;
	else if (ipv4)
		family = AF_INET;
	else
		family = AF_INET6;

	ai_head = do_getaddrinfo(NULL, family);
	if (!ai_head) {
		usbip_close_driver();
		socket_stop();
		return -1;
	}
	nsockfd = listen_all_addrinfo(ai_head, sockfdlist,
		sizeof(sockfdlist) / sizeof(*sockfdlist));
	freeaddrinfo(ai_head);
	if (nsockfd <= 0) {
		err("failed to open a listening socket");
		usbip_close_driver();
		socket_stop();
		return -1;
	}

	dbg("listening on %d address%s", nsockfd, (nsockfd == 1) ? "" : "es");

	fds = (struct pollfd *)calloc(nsockfd, sizeof(struct pollfd));
	for (i = 0; i < nsockfd; i++) {
		fds[i].fd = sockfdlist[i];
		fds[i].events = POLLIN;
	}
	timeout.tv_sec = MAIN_LOOP_TIMEOUT;
	timeout.tv_nsec = 0;

	sigfillset(&sigmask);
	sigdelset(&sigmask, SIGTERM);
	sigdelset(&sigmask, SIGINT);

	terminate = 0;
	while (!terminate) {
		int r;

		r = ppoll(fds, nsockfd, &timeout, &sigmask);
		if (r < 0) {
			dbg("%s", strerror(errno));
			terminate = 1;
		} else if (r) {
			for (i = 0; i < nsockfd; i++) {
				if (fds[i].revents & POLLIN) {
					dbg("read event on fd[%d]=%d",
					    i, sockfdlist[i]);
					process_request(sockfdlist[i]);
				}
			}
		} else {
			dbg("heartbeat timeout on ppoll()");
		}
	}

	info("shutting down " PROGNAME);
	free(fds);
	usbip_close_driver();
	socket_stop();

	return 0;
}

int main(int argc, char *argv[])
{
	static const struct option longopts[] = {
		{ "ipv4",     no_argument,       NULL, '4' },
		{ "ipv6",     no_argument,       NULL, '6' },
		{ "daemon",   no_argument,       NULL, 'D' },
		{ "debug",    no_argument,       NULL, 'd' },
#ifdef USBIP_WITH_LIBUSB
		{ "debug-flags", required_argument, NULL, 'f' },
#endif
#ifndef USBIP_DAEMON_APP
		{ "device",   no_argument,       NULL, 'e' },
#endif
		{ "pid",      optional_argument, NULL, 'P' },
		{ "tcp-port", required_argument, NULL, 't' },
		{ "help",     no_argument,       NULL, 'h' },
		{ "version",  no_argument,       NULL, 'v' },
		{ NULL,	      0,                 NULL,  0  }
	};

	enum {
		cmd_standalone_mode = 1,
		cmd_help,
		cmd_version
	} cmd;

	int daemonize = 0;
	int ipv4 = 0, ipv6 = 0;
	int opt, rc = -1;

	pid_file = NULL;

	usbip_use_stderr = 1;
	usbip_use_syslog = 0;

#ifndef USBIP_WITH_LIBUSB
	if (geteuid() != 0)
		err("not running as root?");
#endif

	cmd = cmd_standalone_mode;
	for (;;) {
		opt = getopt_long(argc, argv, "46Dd"
#ifdef USBIP_WITH_LIBUSB
				  "f:"
#endif
#ifndef USBIP_DAEMON_APP
				  "e"
#endif
				  "P::t:hv", longopts, NULL);

		if (opt == -1)
			break;

		switch (opt) {
		case '4':
			ipv4 = 1;
			break;
		case '6':
			ipv6 = 1;
			break;
		case 'D':
			daemonize = 1;
			break;
		case 'd':
			usbip_use_debug = 1;
			break;
#ifdef USBIP_WITH_LIBUSB
		case 'f':
			usbip_debug_flag = strtoul(optarg, NULL, 16);
			break;
#endif
		case 'h':
			cmd = cmd_help;
			break;
		case 'P':
			pid_file = optarg ? optarg : usbip_default_pid_file;
			break;
		case 't':
			usbip_setup_port_number(optarg);
			break;
		case 'v':
			cmd = cmd_version;
			break;
#ifndef USBIP_DAEMON_APP
		case 'e':
			usbip_update_driver();
			break;
#endif
		case '?':
			usbipd_help();
		default:
			goto err_out;
		}
	}

	if (!ipv4 && !ipv6)
		ipv4 = ipv6 = 1;

	switch (cmd) {
	case cmd_standalone_mode:
		rc = do_standalone_mode(daemonize, ipv4, ipv6);
		remove_pid_file();
		break;
	case cmd_version:
		printf(PROGNAME " (%s)\n", usbip_version_string);
		rc = 0;
		break;
	case cmd_help:
		usbipd_help();
		rc = 0;
		break;
	default:
		usbipd_help();
		goto err_out;
	}

err_out:
	return (rc > -1 ? EXIT_SUCCESS : EXIT_FAILURE);
}
