// SPDX-License-Identifier: BSD-3-Clause

#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <assert.h>
#include <sys/types.h>
#include <unistd.h>
#include <fcntl.h>
#include <sys/stat.h>
#include <sys/epoll.h>
#include <sys/socket.h>
#include <netinet/in.h>
#include <arpa/inet.h>
#include <sys/sendfile.h>
#include <sys/eventfd.h>
#include <libaio.h>
#include <errno.h>

#include "aws.h"
#include "utils/util.h"
#include "utils/debug.h"
#include "utils/sock_util.h"
#include "utils/w_epoll.h"

static int listenfd;
static int epollfd;

static io_context_t ctx;


static int  aws_on_path_cb(http_parser *p, const char *buf, size_t len);
static void connection_prepare_send_reply_header(struct connection *conn);
static void connection_prepare_send_404(struct connection *conn);
static enum resource_type connection_get_resource_type(struct connection *conn);
static struct connection* connection_create(int sockfd);
static void connection_start_async_io(struct connection *conn);
static void connection_remove(struct connection *conn);
static int  make_socket_non_blocking(int sockfd);
static void handle_new_connection(void);
static void receive_data(struct connection *conn);
static int  connection_open_file(struct connection *conn);
static void connection_complete_async_io(struct connection *conn);
static int  parse_header(struct connection *conn);
static enum connection_state connection_send_static(struct connection *conn);
static int  connection_send_data(struct connection *conn);
static int  connection_send_dynamic(struct connection *conn);
static void handle_input(struct connection *conn);
static void handle_output(struct connection *conn);
static void handle_client(uint32_t event, struct connection *conn);


static struct connection* get_connection_from_parser(http_parser *p)
{
	if (p == NULL) {
		fprintf(stderr, "Error: http_parser is NULL\n");
		return NULL;
	}

	return (struct connection *)p->data;
}

static int aws_on_path_cb(http_parser *p, const char *buf, size_t len)
{
	struct connection *conn = get_connection_from_parser(p);

	if (conn == NULL) {
		fprintf(stderr, "Error: connection is NULL in aws_on_path_cb\n");
		return -1;
	}

	if (len >= sizeof(conn->request_path)) {
		fprintf(stderr, "Error: request path length exceeds buffer size\n");
		return -1;
	}

	memcpy(conn->request_path, buf, len);
	conn->request_path[len] = '\0';
	conn->have_path = 1;

	dlog(LOG_INFO, "Parsed request path: %s\n", conn->request_path);

	return 0;
}

static enum resource_type connection_get_resource_type(struct connection *conn)
{
	const char *request_path = conn->request_path;

	if (strncmp(request_path + 1, AWS_REL_STATIC_FOLDER, strlen(AWS_REL_STATIC_FOLDER)) == 0) {
		return RESOURCE_TYPE_STATIC;
	}

	if (strncmp(request_path + 1, AWS_REL_DYNAMIC_FOLDER, strlen(AWS_REL_DYNAMIC_FOLDER)) == 0) {
		return RESOURCE_TYPE_DYNAMIC;
	}

	return RESOURCE_TYPE_NONE;
}


static struct connection* connection_create(int sockfd)
{
	struct connection *conn = calloc(1, sizeof(struct connection));
	DIE(!conn, "malloc");

	conn->sockfd   = sockfd;
	conn->fd       = -1;
	conn->state    = STATE_INITIAL;
	conn->res_type = RESOURCE_TYPE_NONE;

	return conn;
}

static void close_connection_socket(struct connection *conn)
{
	if (conn->sockfd != -1) {
		close(conn->sockfd);
		conn->sockfd = -1;
	}
}

static void close_connection_file(struct connection *conn)
{
	if (conn->fd != -1) {
		close(conn->fd);
		conn->fd = -1;
	}
}

static void free_connection_resources(struct connection *conn)
{
	free(conn);
}

static void connection_remove(struct connection *conn)
{
	close_connection_socket(conn);
	conn->state = STATE_CONNECTION_CLOSED;
	close_connection_file(conn);
	free_connection_resources(conn);
}

static void prepare_async_read(struct connection *conn)
{
    struct iocb *iocb = &conn->iocb;
    io_prep_pread(iocb, conn->fd, conn->recv_buffer, BUFSIZ, conn->file_pos);
    conn->piocb[0] = iocb;
}

static void prepare_async_write(struct connection *conn)
{
    struct iocb *iocb = &conn->iocb;
    io_prep_pwrite(iocb, conn->sockfd, conn->recv_buffer, BUFSIZ, 0);
    conn->piocb[0] = iocb;
}

static void submit_async_io(io_context_t ctx, struct iocb **piocb)
{
	int rc = io_submit(ctx, 1, piocb);
	if (rc != 1) {
		perror("io_submit");
		exit(EXIT_FAILURE);
	}
}

static void connection_start_async_io(struct connection *conn)
{
	prepare_async_read(conn);
	submit_async_io(ctx, conn->piocb);

	prepare_async_write(conn);
	submit_async_io(ctx, conn->piocb);
}

static int complete_async_read(io_context_t ctx, struct connection *conn)
{
	struct io_event event;
	int rc = io_getevents(ctx, 1, 1, &event, NULL);
	if (rc != 1 || event.res < 0) {
		dlog(LOG_ERR, "Error completing async read\n");
		return -1;
	}
	conn->file_pos += event.res;
	return 0;
}

static int complete_async_write(io_context_t ctx, struct connection *conn)
{
	struct io_event event;
	int rc = io_getevents(ctx, 1, 1, &event, NULL);
	if (rc != 1 || event.res != BUFSIZ) {
		dlog(LOG_ERR, "Error completing async write\n");
		return -1;
	}
	conn->file_size -= BUFSIZ;
	return 0;
}


static void connection_complete_async_io(struct connection *conn)
{
	if (complete_async_read(ctx, conn) < 0) {
		return;
	}

	if (complete_async_write(ctx, conn) < 0) {
		return;
	}

	conn->state = (conn->file_size == 0 ? STATE_DATA_SENT : STATE_ASYNC_ONGOING);
}

static off_t get_file_size(int fd)
{
	struct stat st;
	if (fstat(fd, &st) == -1) {
		perror("fstat");
		return -1;
	}
	return st.st_size;
}

static void prepare_http_200_ok_header(char *buffer, size_t buffer_size, off_t file_size)
{
	snprintf(buffer, buffer_size,
			 "HTTP/1.1 200 OK\r\n"
			 "Content-Length: %ld\r\n"
			 "Connection: close\r\n\r\n",
			 file_size);
}

void set_state(struct connection *conn, enum connection_state state)
{
	conn->state = state;
}

static void connection_prepare_send_reply_header(struct connection *conn)
{
	conn->file_size = get_file_size(conn->fd);
	if (conn->file_size == -1) {
		dlog(LOG_ERR, "Failed to get file size\n");
		return;
	}

	prepare_http_200_ok_header(conn->send_buffer, sizeof(conn->send_buffer), conn->file_size);

	int len = strlen(conn->send_buffer);
	conn->send_len = len;
	set_state(conn, STATE_SENDING_DATA);

	dlog(LOG_INFO, "Sending header\n");
}

static void connection_prepare_send_404(struct connection *conn)
{
	struct stat st;

	if (conn->fd != -1) {
		fstat(conn->fd, &st);
		conn->file_size = st.st_size;
	} else {
		conn->file_size = 0;
	}

	sprintf(conn->send_buffer,
			"HTTP/1.1 404 Not Found\r\n"
			"Content-Length: %ld\r\n"
			"Connection: close\r\n\r\n",
			conn->file_size);

	conn->send_len = strlen(conn->send_buffer);
	conn->state    = STATE_404_SENT;

	dlog(LOG_INFO, "Sending 404\n");
}

static ssize_t send_file_chunk(int sockfd, int fd, off_t *offset, size_t count)
{
	ssize_t bytes_sent = sendfile(sockfd, fd, offset, count);
	if (bytes_sent < 0) {
		perror("sendfile");
	}
	return bytes_sent;
}

static enum connection_state connection_send_static(struct connection *conn)
{
	ssize_t bytes_sent = 0, total_bytes_sent = 0;
	off_t offset = 0;

	for (total_bytes_sent = 0; total_bytes_sent < conn->file_size; total_bytes_sent += bytes_sent) {
		bytes_sent = send_file_chunk(conn->sockfd, conn->fd, &offset, conn->file_size - total_bytes_sent);
		if (bytes_sent < 0) {
			return STATE_DATA_SENT;
		}
	}

	conn->file_size -= total_bytes_sent;
	return (conn->file_size == 0 ? STATE_DATA_SENT : STATE_SENDING_DATA);
}


static ssize_t send_response_buffer(struct connection *conn)
{
	ssize_t bytes_sent = 0;
	ssize_t total_bytes_sent = 0;

	ssize_t remaining = conn->send_len;
	char *buf_ptr = conn->send_buffer;

	while (remaining > 0) {
		bytes_sent = send(conn->sockfd, buf_ptr, remaining, 0);
		if (bytes_sent < 0) {
			perror("send");
			return -1;
		}
		buf_ptr    += bytes_sent;
		remaining  -= bytes_sent;
		total_bytes_sent += bytes_sent;
	}

	return total_bytes_sent;
}

static void update_next_state(struct connection *conn)
{
    if (conn->fd == -1) {
        // Could not open the file or no file to serve
        conn->state = STATE_DATA_SENT;
        return;
    }

    if (conn->res_type == RESOURCE_TYPE_STATIC) {
        conn->state = connection_send_static(conn);
        return;
    }

    // If dynamic, use asynchronous I/O read+write
    conn->state = STATE_ASYNC_ONGOING;
}


static int connection_send_data(struct connection *conn)
{
	ssize_t total_bytes_sent = send_response_buffer(conn);
	if (total_bytes_sent < 0) {
		return -1;
	}

	conn->send_len -= total_bytes_sent;

	update_next_state(conn);

	return total_bytes_sent;
}

static int connection_send_dynamic(struct connection *conn)
{
	connection_start_async_io(conn);
	connection_complete_async_io(conn);
	return 0;
}

static void receive_data(struct connection *conn)
{
    while (1) {
        ssize_t bytes_recv = recv(conn->sockfd,
                                  conn->recv_buffer + conn->recv_len,
                                  BUFSIZ - conn->recv_len,
                                  0);

        if (bytes_recv < 0) {
            dlog(LOG_ERR, "Error receiving data\n");
            return;
        }

        if (bytes_recv == 0) {
            dlog(LOG_INFO, "Client closed connection or no more data\n");
            return;
        }

        conn->recv_len += bytes_recv;
        dlog(LOG_INFO, "Received %ld bytes\n", bytes_recv);

        if (strstr(conn->recv_buffer, "\r\n\r\n")) {
            dlog(LOG_INFO, "Received request:\n%s\n", conn->recv_buffer);
            dlog(LOG_INFO, "Request received\n");
            return;
        }

        if (conn->recv_len >= BUFSIZ) {
            dlog(LOG_INFO, "Buffer is full; stopping read\n");
            return;
        }
    }
}

static int parse_header(struct connection *conn)
{
	http_parser_settings settings_on_path = {
		.on_message_begin    = NULL,
		.on_header_field     = NULL,
		.on_header_value     = NULL,
		.on_path             = aws_on_path_cb,
		.on_url              = NULL,
		.on_fragment         = NULL,
		.on_query_string     = NULL,
		.on_body             = NULL,
		.on_headers_complete = NULL,
		.on_message_complete = NULL
	};

	size_t nparsed = http_parser_execute(&conn->request_parser,
										 &settings_on_path,
										 conn->recv_buffer,
										 conn->recv_len);

	return (nparsed == conn->recv_len) ? 0 : -1;
}

static void construct_file_path(char *filepath, size_t size, const char *document_root, const char *request_path)
{
    snprintf(filepath, size, "%s%s", document_root, request_path + 1);
}

static void log_file_opening(const char *filepath)
{
    dlog(LOG_INFO, "Opening file %s\n", filepath);
}

static int open_file(const char *filepath)
{
    int fd = open(filepath, O_RDONLY);
    if (fd < 0) {
        perror("open");
    }
    return fd;
}

static void set_connection_state(struct connection *conn, int fd, const char *filepath)
{
    if (fd < 0) {
        conn->state = STATE_SENDING_404;
    } else {
        dlog(LOG_INFO, "Opened file %s\n", filepath);
        conn->fd = fd;
        conn->state = STATE_SENDING_HEADER;
    }
}

static int open_file_and_set_state(struct connection *conn, const char *filepath)
{
    int fd = open_file(filepath);
    set_connection_state(conn, fd, filepath);
    return (fd < 0) ? -1 : 0;
}

static int connection_open_file(struct connection *conn)
{
    char filepath[BUFSIZ];
    construct_file_path(filepath, sizeof(filepath), AWS_DOCUMENT_ROOT, conn->request_path);
    log_file_opening(filepath);
    return open_file_and_set_state(conn, filepath);
}

static int make_socket_non_blocking(int sockfd)
{
    int flags = fcntl(sockfd, F_GETFL);
    if (flags == -1) {
        perror("fcntl (F_GETFL)");
        return -1;
    }

    if (fcntl(sockfd, F_SETFL, flags | O_NONBLOCK) == -1) {
        perror("fcntl (F_SETFL)");
        return -1;
    }

    return 0;
}

static int accept_new_connection(int listenfd, struct sockaddr_in *addr, socklen_t *addrlen)
{
    int sockfd = accept(listenfd, (struct sockaddr *)addr, addrlen);
    if (sockfd < 0) {
        perror("accept");
        return -1;
    }
    return sockfd;
}

static void initialize_http_parser(struct connection *conn)
{
    http_parser *parser = &conn->request_parser;
    http_parser_init(parser, HTTP_REQUEST);
    parser->data = conn;
}

static void handle_new_connection(void)
{
    struct sockaddr_in addr;
    socklen_t addrlen = sizeof(addr);
    int sockfd = accept_new_connection(listenfd, &addr, &addrlen);
    if (sockfd < 0) {
        return;
    }

    if (make_socket_non_blocking(sockfd) < 0) {
        close(sockfd);
        return;
    }

    struct connection *conn = connection_create(sockfd);
    if (!conn) {
        close(sockfd);
        perror("connection_create");
        return;
    }

    dlog(LOG_INFO, "New connection from %s:%d on socket %d\n",
         inet_ntoa(addr.sin_addr), ntohs(addr.sin_port), sockfd);

    if (w_epoll_add_ptr_in(epollfd, sockfd, conn) < 0) {
        connection_remove(conn);
        perror("w_epoll_add_ptr_in");
        return;
    }

    initialize_http_parser(conn);
}

static void handle_input(struct connection *conn)
{
	int rc;

	if (conn->state == STATE_INITIAL) {
		dlog(LOG_INFO, "Initial state\n");
		conn->state = STATE_RECEIVING_DATA;
	} 
	else if (conn->state == STATE_RECEIVING_DATA) {
		dlog(LOG_INFO, "Receiving data\n");
		receive_data(conn);

		if (conn->recv_len <= 0) {
			perror("recv");
			connection_remove(conn);
			dlog(LOG_INFO, "Error receiving data\n");
			return;
		}
		conn->state = STATE_REQUEST_RECEIVED;
	} 
	else if (conn->state == STATE_CONNECTION_CLOSED) {
		dlog(LOG_INFO, "Connection closed\n");
	} 
	else {
	}

	rc = w_epoll_update_ptr_out(epollfd, conn->sockfd, conn);
	DIE(rc < 0, "w_epoll_update_ptr_inout");
}

static void handle_output(struct connection *conn)
{
	int rc;
	dlog(LOG_INFO, "Connection status: %d\n", conn->state);

	if (conn->state == STATE_REQUEST_RECEIVED) {
		dlog(LOG_INFO, "Request received\n");

		rc = parse_header(conn);
		if (rc < 0) {
			dlog(LOG_ERR, "Error parsing header\n");
			conn->state = STATE_SENDING_404;
		} else {
			conn->res_type = connection_get_resource_type(conn);
			connection_open_file(conn);
		}
		dlog(LOG_INFO, "Request received: EXIT\n");
	} 
	else if (conn->state == STATE_SENDING_404) {
		dlog(LOG_INFO, "Sending 404: ENTER\n");
		connection_prepare_send_404(conn);
		dlog(LOG_INFO, "Sending 404: EXIT\n");
	} 
	else if (conn->state == STATE_SENDING_HEADER) {
		dlog(LOG_INFO, "Sending header: ENTER\n");
		connection_prepare_send_reply_header(conn);
		dlog(LOG_INFO, "Sending header: EXIT\n");
	} 
	else if (conn->state == STATE_SENDING_DATA) {
		dlog(LOG_INFO, "Sending data\n");
		rc = connection_send_data(conn);
		if (rc <= 0) {
			dlog(LOG_ERR, "Error sending data\n");
			connection_remove(conn);
			return;
		}
	} 
	else if (conn->state == STATE_ASYNC_ONGOING) {
		dlog(LOG_INFO, "Async ongoing\n");
		connection_send_dynamic(conn);
	} 
	else if (conn->state == STATE_DATA_SENT) {
		dlog(LOG_INFO, "Data sent\n");
		w_epoll_update_fd_in(epollfd, conn->sockfd);
		connection_remove(conn);
	} 
	else if (conn->state == STATE_HEADER_SENT) {
		dlog(LOG_INFO, "Header sent\n");
		w_epoll_update_fd_in(epollfd, conn->sockfd);
		connection_remove(conn);
	} 
	else if (conn->state == STATE_404_SENT) {
		dlog(LOG_INFO, "404 sent\n");
		connection_remove(conn);
	} 
	else {
	}

	if (conn->res_type == RESOURCE_TYPE_NONE) {
		rc = w_epoll_update_ptr_in(epollfd, conn->sockfd, conn);
		DIE(rc < 0, "w_epoll_update_ptr_inout");
	}
}

static void handle_epollin_event(struct connection *conn)
{
    handle_input(conn);
}

static void handle_epollout_event(struct connection *conn)
{
    handle_output(conn);
}

static void handle_client(uint32_t event, struct connection *conn)
{
    if (event & EPOLLIN) {
        handle_epollin_event(conn);
    }
    if (event & EPOLLOUT) {
        handle_epollout_event(conn);
    }
}

static void initialize_async_operations(void)
{
    int rc = io_setup(128, &ctx);
    DIE(rc < 0, "io_setup");
}

static void create_and_bind_server_socket(void)
{
    listenfd = tcp_create_listener(AWS_LISTEN_PORT, DEFAULT_LISTEN_BACKLOG);
    DIE(listenfd < 0, "tcp_create_listener");
}

static void add_server_socket_to_epoll(void)
{
    int rc = w_epoll_add_fd_in(epollfd, listenfd);
    DIE(rc < 0, "w_epoll_add_fd_in");
}

static void handle_server_socket_event(struct epoll_event *event)
{
    if (event->events & EPOLLIN) {
        handle_new_connection();
    }
}

static void handle_client_connection_event(struct epoll_event *event)
{
    struct connection *conn = (struct connection *)event->data.ptr;
    dlog(LOG_INFO, "Handle client\n");
    handle_client(event->events, conn);
}

static void server_loop(void)
{
    while (1) {
        struct epoll_event event;

        int rc = w_epoll_wait_infinite(epollfd, &event);
        DIE(rc < 0, "w_epoll_wait_infinite");

        if (event.data.fd == listenfd) {
            handle_server_socket_event(&event);
        } else {
            handle_client_connection_event(&event);
        }
    }
}

static void cleanup(void)
{
    tcp_close_connection(listenfd);
}

int main(void)
{
    initialize_async_operations();

    epollfd = w_epoll_create();
    DIE(epollfd < 0, "w_epoll_create");

    create_and_bind_server_socket();

    add_server_socket_to_epoll();

    dlog(LOG_INFO, "Server waiting for connections on port %d\n", AWS_LISTEN_PORT);

    server_loop();

    cleanup();

    return 0;
}