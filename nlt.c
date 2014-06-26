#define _GNU_SOURCE
#include <assert.h>
#include <unistd.h>
#include <string.h>
#include <time.h>
#include <stdio.h>
#include <stdlib.h>
#include <nanomsg/nn.h>
#include <nanomsg/reqrep.h>
#include <getopt.h>
#include <err.h>
#include <pthread.h>

int debug = 0;

struct sw {
	int s;
	char *name;
	int sleep;
};

static void *
server_worker(void *v)
{
	struct sw *sw = v;
	int nreq = 0;

	while (1) {
		char *rbuf;
		char *buf = NULL;
		int bytes;

		if ((bytes = nn_recv(sw->s, &buf, NN_MSG, 0)) < 0)
			err(1, "nn_recv");

		if (debug) printf("server %s received(%d): [%.*s]\n", sw->name, nreq++, bytes, buf);

		if (sw->sleep)
			sleep(sw->sleep);

		asprintf(&rbuf, "e%.*s", bytes, buf);

		if (debug) printf("server %s: sending response: [%s]\n", sw->name, rbuf);

		if ((bytes = nn_send(sw->s, rbuf, strlen(rbuf), 0)) != (int)strlen(rbuf))
			err(1, "nn_send");
		free(rbuf);
		nn_freemsg(buf);
	}

	nn_shutdown(sw->s, 0);

	return NULL;			
}

static int
server(int argc, char **argv)
{
	char *url = argv[0];
	int ext_sock;
	int int_sock;
	int i;

	if ((ext_sock = nn_socket(AF_SP_RAW, NN_REP)) < 0)
		err(1, "nn_socket");
	if (nn_bind(ext_sock, url) < 0)
		err(1, "nn_bind");

	if ((int_sock = nn_socket(AF_SP_RAW, NN_REQ)) < 0)
		err(1, "nn_socket");
	if (nn_bind(int_sock, "inproc://hej") < 0)
		err(1, "nn_bind");

	for (i = 0; i < 5; i++) {
		struct sw *sw = calloc(1, sizeof(*sw));
		pthread_t thr;

		sw->sleep = i == 0 ? 2 : 0;

		if ((sw->s = nn_socket(AF_SP, NN_REP)) < 0)
			err(1, "nn_socket");
#ifdef TERRIBLE_WORKAROUND
		int opt = 1;
		if ((nn_setsockopt(sw->s, NN_SOL_SOCKET, NN_RCVBUF, &opt, sizeof(opt))) < 0)
			err(1, "nn_setsockopt");
#endif

		if (nn_connect(sw->s, "inproc://hej") < 0)
			errx(1, "nn_connect: %d %s", nn_errno(), nn_strerror(nn_errno()));

		asprintf(&sw->name, "s%d", i);
		pthread_create(&thr, NULL, server_worker, sw);
	}

	while (1) {
		if (debug) printf("starting device\n");
		if (nn_device(ext_sock, int_sock) < 0)
			errx(1, "nn_device %d %s %d %d", nn_errno(), nn_strerror(nn_errno()), int_sock, ext_sock);
	}

	return 0;
}

struct cw {
	int s;
	char *req;
};

static void *
client_worker(void *v)
{
	struct cw *cw = v;
	struct timespec s, e;
	char *repbuf = NULL;
	int replen;

	if (debug) printf("client: sending req: [%s]\n", cw->req);

	clock_gettime(CLOCK_MONOTONIC, &s);

	if (nn_send(cw->s, cw->req, strlen(cw->req), 0) != (int)strlen(cw->req))
		err(1, "nn_send");
	if ((replen = nn_recv(cw->s, &repbuf, NN_MSG, 0)) < 0)
		err(1, "nn_recv");

	clock_gettime(CLOCK_MONOTONIC, &e);

	e.tv_sec -= s.tv_sec;
	if ((e.tv_nsec -= s.tv_nsec) < 0) {
		e.tv_nsec += 1000000000;
		e.tv_sec--;
	}

	if (e.tv_sec) {
		double t = ((double)e.tv_sec * 1000000000.0 + (double)e.tv_nsec) / 1000000000.0;
		printf("client req took %f\n", t);
	}

	if (debug) printf("client: received reply: [%.*s]\n", replen, repbuf);
	nn_freemsg(repbuf);

	nn_shutdown(cw->s, 0);

	return NULL;
}

static int
client(int argc, char **argv)
{
	int cnt = 100;
	pthread_t thr[cnt];
	int i;

	for (i = 0; i < cnt; i++) {
		struct cw *cw = calloc(1, sizeof(*cw));

		if ((cw->s = nn_socket(AF_SP, NN_REQ)) < 0)
			err(1, "nn_socket");
		if (nn_connect(cw->s, argv[0]) < 0)
			err(1, "nn_connect");

		asprintf(&cw->req, "xx%d", i);
		pthread_create(&thr[i], NULL, client_worker, cw);
	}

	for (i = 0; i < 100; i++) {
		void *r;
		pthread_join(thr[i], &r);
	}

	return 0;
}

int
main(int argc, char **argv)
{
	char opt;
	int s,c;

	s = c = 0;

	while ((opt = getopt(argc, argv, "sc")) != -1) {
		switch (opt) {
		case 's':
			s = 1;
			break;
		case 'c':
			c = 1;
			break;
		default:
			goto usage;
		}
	}
	if (argc - optind < 1 || ((c^s) != 1)) {
usage:
		fprintf(stderr, "Usage: %s <-s|-c> url\n", argv[0]);
		exit(1);
	}
	argc -= optind;
	argv += optind;
	if (s)
		return server(argc, argv);

	return client(argc, argv);		
}
