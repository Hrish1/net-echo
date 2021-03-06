/*
 * eutils.c
 *
 * Authors:
 *	Cody Doucette		Boston University
 *	Michel Machado		Boston University
 *
 * This file contains utility definitions for the echo clients and server.
 *
 */

#include <assert.h>
#include <limits.h>
#include <stdlib.h>
#include <string.h>
#include <errno.h>
#include <sys/types.h>
#include <sys/socket.h>
#include <sys/select.h>
#include <netinet/in.h>
#include <arpa/inet.h>
#include <unistd.h>
#include "eutils.h"

#define FILE_APPENDIX "_echo"

/**
 * check_cli_params(): Ensure that the correct number of arguments have been
 * given.
 */
int check_cli_params(int *pis_stream, int argc, char * const argv[])
{
	int is_xia;

	if (argc > 1) {
		if (!strcmp(argv[1], "datagram"))
			*pis_stream = 0;
		else if (!strcmp(argv[1], "stream"))
			*pis_stream = 1;
		else
			goto failure;
	}

	if (argc > 2) {
		if (!strcmp(argv[2], "xip"))
			is_xia = 1;
		else if (!strcmp(argv[2], "ip"))
			is_xia = 0;
		else
			goto failure;
	}

	/* Don't simplify this test, argc may change for each case. */
	if ((is_xia && argc == 5) || (!is_xia && argc == 5))
		return is_xia;

failure:
	printf("usage:\t%s <'datagram' | 'stream'> 'ip' srvip_addr port\n",
		argv[0]);
	printf(      "\t%s <'datagram' | 'stream'> 'xip' cli_addr_file srv_addr_file\n",
		argv[0]);
	exit(1);
}

static int ppal_map_loaded = 0;

static inline void load_ppal_map(void)
{
	if (ppal_map_loaded)
		return;
	assert(!init_ppal_map(NULL));
	ppal_map_loaded = 1;
}

static xid_type_t xidtype_xdp = XIDTYPE_NAT;
static xid_type_t xidtype_srvc = XIDTYPE_NAT;

xid_type_t get_xdp_type(void)
{
	if (xidtype_xdp != XIDTYPE_NAT)
		return xidtype_xdp;

	load_ppal_map();
	assert(!ppal_name_to_type("xdp", &xidtype_xdp));
	return xidtype_xdp;
}

xid_type_t get_srvc_type(void)
{
	if (xidtype_srvc != XIDTYPE_NAT)
		return xidtype_srvc;

	load_ppal_map();
	assert(!ppal_name_to_type("serval", &xidtype_srvc));
	return xidtype_srvc;
}

int any_socket(int is_xia, int is_stream)
{
	int domain = is_xia ? AF_XIA : AF_INET;
	int type = is_stream ? SOCK_STREAM : SOCK_DGRAM;
	int protocol = is_xia
			? (is_stream ? get_srvc_type()	: get_xdp_type())
			: (is_stream ? IPPROTO_TCP	: IPPROTO_UDP);
	int sock = socket(domain, type, protocol);

	if (!is_xia && sock >= 0) {
		/* Let the kernel reuse the socket address. This lets us run
		 * twice in a row, without waiting for the (ip, port) tuple
		 * to time out.
		 */
		int i = 1;
		setsockopt(sock, SOL_SOCKET, SO_REUSEADDR, &i, sizeof(i));
	}

	return sock;
}

static void set_sockaddr_in(struct sockaddr_in *in, char *str_addr, int port)
{
	in->sin_family = AF_INET;
	in->sin_port = htons(port);
	if (str_addr)
		assert(inet_aton(str_addr, &in->sin_addr));
	else
		in->sin_addr.s_addr = htonl(INADDR_ANY);
}

/* XXX This function was copied from xiaconf/xip/xiphid.c.
 * It should go to a library!
 */
static int parse_and_validate_addr(char *str, struct xia_addr *addr)
{
	int invalid_flag;
	int rc;

	rc = xia_pton(str, INT_MAX, addr, 0, &invalid_flag);
	if (rc < 0) {
		fprintf(stderr, "Syntax error: invalid address: [[%s]]\n", str);
		return rc;
	}
	rc = xia_test_addr(addr);
	if (rc < 0) {
		char buf[XIA_MAX_STRADDR_SIZE];
		assert(xia_ntop(addr, buf, XIA_MAX_STRADDR_SIZE, 1) >= 0);
		fprintf(stderr, "Invalid address (%i): [[%s]] "
			"as seen by xia_xidtop: [[%s]]\n", -rc, str, buf);
		return rc;
	}
	if (invalid_flag) {
		fprintf(stderr, "Although valid, address has invalid flag: "
			"[[%s]]\n", str);
		return -1;
	}
	return 0;
}

static int set_sockaddr_xia(struct sockaddr_xia *xia, const char *filename)
{
#define BUFSIZE (4 * 1024)
	FILE *f;
	char buf[BUFSIZE];
	int len;

	load_ppal_map();

	/* Read address. */
	f = fopen(filename, "r");
	if (!f) {
		perror(__func__);
		return errno;
	}
	len = fread(buf, 1, BUFSIZE, f);
	assert(len < BUFSIZE);
	fclose(f);
	buf[len] = '\0';

	xia->sxia_family = AF_XIA;
	return parse_and_validate_addr(buf, &xia->sxia_addr);
}

struct sockaddr *__get_addr(int is_xia, char *str1, char *str2, int *plen)
{
	struct tmp_sockaddr_storage *skaddr;

	skaddr = malloc(sizeof(*skaddr));
	assert(skaddr);
	memset(skaddr, 0, sizeof(*skaddr));

	if (is_xia) {
		struct sockaddr_xia *xia = (struct sockaddr_xia *)skaddr;
		/* XXX It should be a BUILD_BUG_ON(). */
		assert(sizeof(*skaddr) >= sizeof(*xia));
		assert(!set_sockaddr_xia(xia, str1));
		*plen = sizeof(*xia);
	} else {
		struct sockaddr_in *in = (struct sockaddr_in *)skaddr;
		/* XXX It should be a BUILD_BUG_ON(). */
		assert(sizeof(*skaddr) >= sizeof(*in));
		set_sockaddr_in(in, str1, atoi(str2));
		*plen = sizeof(*in);
	}

	return (struct sockaddr *)skaddr;
}

struct sockaddr *get_cli_addr(int is_xia, int argc, char * const argv[],
	int *plen)
{
	UNUSED(argc);
	if (is_xia)
		return __get_addr(is_xia, argv[3], NULL, plen);
	else
		return __get_addr(is_xia, NULL, "0", plen);
}

struct sockaddr *get_srv_addr(int is_xia, int argc, char * const argv[],
	int *plen)
{
	UNUSED(argc);
	if (is_xia)
		return __get_addr(is_xia, argv[4], NULL, plen);
	else
		return __get_addr(is_xia, argv[3], argv[4], plen);
}

void any_bind(int is_xia, int force, int s, const struct sockaddr *addr,
	int addr_len)
{
	if (is_xia || force) {
		/* XIA requires explicit binding, whereas TCP/IP doesn't,
		 * so only bind if @force is true.
		 */
		assert(!bind(s, addr, addr_len));
	}
}

int read_command(char *buf, int len)
{
	size_t n_read;

	while (1) {
		char *s = fgets(buf, len, stdin);
		if (!s) {
			if (feof(stdin)) {
				/* User pressed Ctrl+D. */
				if (len >= 1)
					buf[0] = '\0';
				return 0;
			}
			fprintf(stderr, "%s: fgets errno=%i: %s\n",
				__func__, errno, strerror(errno));
			exit(1);
		}

		n_read = strlen(s);

		/* Skip empty commands. */
		if (n_read == 0)	/* EOF */
			continue;
		if (n_read == 1 && (s[0] == '\n'))
			continue;

		/* Done. */
		break;
	}

	/* Shave '\n' off. */
	if (buf[n_read - 1] == '\n') {
		n_read--;
		buf[n_read] = '\0';
	}

	assert(n_read > 0);
	return n_read;
}

/**
 * send_packet(): Send a packet via the given socket.
 */
void send_packet(int s, const char *buf, int n, const struct sockaddr *dst,
	socklen_t dst_len)
{
	ssize_t rc = sendto(s, buf, n, 0, dst, dst_len);
	if (rc < 0) {
		fprintf(stderr, "%s: sendto errno=%i: %s\n",
			__func__, errno, strerror(errno));
		exit(1);
	}
}

static int count_rows(const struct sockaddr_xia *xia)
{
	int i;
	for (i = 0; i < XIA_NODES_MAX; i++)
		if (xia_is_nat(xia->sxia_addr.s_row[i].s_xid.xid_type))
			break;
	return i;
}

static int address_match(const struct sockaddr *addr, socklen_t addr_len,
	const struct sockaddr *expected, socklen_t exp_len)
{
	assert(addr->sa_family == expected->sa_family);

	switch (addr->sa_family) {
	case AF_INET:
		return addr_len == exp_len && !memcmp(addr, expected, addr_len);

	case AF_XIA: {
		struct sockaddr_xia *addr_xia = (struct sockaddr_xia *)addr;
		struct sockaddr_xia *exp_xia = (struct sockaddr_xia *)expected;
		int addr_n = count_rows(addr_xia);
		int exp_n = count_rows(exp_xia);

		assert(addr_n > 0 && exp_n > 0);

		return !memcmp(&addr_xia->sxia_addr.s_row[addr_n - 1].s_xid,
			&exp_xia->sxia_addr.s_row[exp_n - 1].s_xid,
			sizeof(addr_xia->sxia_addr.s_row[0].s_xid));
		break;
	}

	default:
		assert(0);
	}
}

/**
 * recv_write(): Receive a packet via the given socket and write to the
 * given file.
 */
void recv_write(int s, const struct sockaddr *expected_src,
	socklen_t exp_src_len, FILE *copy, int n_sent)
{
	struct timeval timeout = {.tv_sec = 2, .tv_usec = 0};
	fd_set readfds;
	struct tmp_sockaddr_storage src;
	char *out;
	unsigned int len;
	int rc, n_read;

	/* Is there anything to read? */
	FD_ZERO(&readfds);
	FD_SET(s, &readfds);
	rc = select(s + 1, &readfds, NULL, NULL, &timeout);
	assert(rc >= 0);
	if (!rc) {
		/* A packet was dropped. */
		fprintf(stderr, ".");
		return;
	}

	/* Read. */
	out = alloca(n_sent);
	len = sizeof(src);
	n_read = recvfrom(s, out, n_sent, 0, (struct sockaddr *)&src, &len);
	assert(n_read >= 0);

	/* Make sure that we're reading from the server. */
	assert(address_match((struct sockaddr *)&src, len,
		expected_src, exp_src_len));

	/* Write. */
	fwrite(out, sizeof(char), n_read, copy);
}

void read_write(int s, FILE *copy, int n_sent)
{
	char *out;
	int n_read;

	/* Read. */
	out = alloca(n_sent);
	n_read = 0;
	while (n_sent > n_read) {
		int len = read(s, out + n_read, n_sent - n_read);
		if (len <= 0) {
			/* The connection was closed or an error occorred. */
			fprintf(stderr, ".");
			return;
		}
		n_read += len;
	}
	assert(n_read == n_sent);

	/* Write. */
	fwrite(out, sizeof(char), n_read, copy);
}

/**
 * fopen_copy(): Create and open a file to for writing output.
 */
static FILE *fopen_copy(const char *orig_name, const char *mode)
{
	int name_len = strlen(orig_name) + strlen(FILE_APPENDIX) + 1;
	char *copy_name = alloca(name_len);

	assert(snprintf(copy_name, name_len, "%s%s", orig_name, FILE_APPENDIX)
		== name_len - 1);

	return fopen(copy_name, mode);
}

/**
 * process_file(): Set up the output file, read in the file into a char buffer,
 * and call __process_file() to do a sequence of sends and receives.
 */
void datagram_process_file(int s, const struct sockaddr *srv, socklen_t srv_len,
	const char *orig_name, int chunk_size, int times, pff_mark_t f)
{
	FILE *orig, *copy;
	char *buf;
	int count, bytes_sent;

	orig = fopen(orig_name, "rb");
	assert(orig);
	copy = fopen_copy(orig_name, "wb");
	assert(copy);

	buf = alloca(chunk_size);
	count = bytes_sent = 0;
	do {
		size_t bytes_read = fread(buf, 1, chunk_size, orig);
		assert(!ferror(orig));
		if (bytes_read > 0) {
			send_packet(s, buf, bytes_read, srv, srv_len);
			count++;
			bytes_sent += bytes_read;
		}
		if (count == times) {
			if (f)
				f(s);
			recv_write(s, srv, srv_len, copy, bytes_sent);
			count = bytes_sent = 0;
		}
	} while (!feof(orig));

	if (count) {
		if (f)
			f(s);
		recv_write(s, srv, srv_len, copy, bytes_sent);
	}

	assert(!fclose(copy));
	assert(!fclose(orig));
}

void stream_process_file(int s, const char *orig_name, int chunk_size,
	int times, pff_mark_t f)
{
	FILE *orig, *copy;
	char *buf;
	int count, bytes_sent;

	orig = fopen(orig_name, "rb");
	assert(orig);
	copy = fopen_copy(orig_name, "wb");
	assert(copy);

	buf = alloca(chunk_size);
	count = bytes_sent = 0;
	do {
		size_t bytes_read = fread(buf, 1, chunk_size, orig);
		assert(!ferror(orig));
		if (bytes_read > 0) {
			assert(write(s, buf, bytes_read) ==
				(ssize_t)bytes_read);
			count++;
			bytes_sent += bytes_read;
		}
		if (count == times) {
			if (f)
				f(s);
			read_write(s, copy, bytes_sent);
			count = bytes_sent = 0;
		}
	} while (!feof(orig));

	if (count) {
		if (f)
			f(s);
		read_write(s, copy, bytes_sent);
	}

	assert(!fclose(copy));
	assert(!fclose(orig));
}

/* Copies data from file descriptor @from to file descriptor
 * @to until nothing is left to be copied. Exits if an error
 * occurs. This assumes both @from and @to are set for blocking
 * reads and writes.
 */
void copy_data(int from, int to)
{
	char buf[2048];
	ssize_t amount;

	while ((amount = read(from, buf, sizeof(buf))) > 0)
		assert(write(to, buf, amount) == amount);
	assert(amount >= 0);
}
