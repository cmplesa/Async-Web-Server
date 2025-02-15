/* SPDX-License-Identifier: BSD-3-Clause */

#ifndef AWS_H_
#define AWS_H_  1

#ifdef __cplusplus
extern "C" {
#endif

/* -------------------------------------------------------------------------
 *                                Includes
 * ------------------------------------------------------------------------*/
#include "http-parser/http_parser.h"
#include <stddef.h> /* For size_t */
#include <sys/types.h>

/* -------------------------------------------------------------------------
 *                                Constants
 * ------------------------------------------------------------------------*/
#define AWS_LISTEN_PORT         8888
#define AWS_DOCUMENT_ROOT       "./"
#define AWS_REL_STATIC_FOLDER   "static/"
#define AWS_REL_DYNAMIC_FOLDER  "dynamic/"
#define AWS_ABS_STATIC_FOLDER   (AWS_DOCUMENT_ROOT AWS_REL_STATIC_FOLDER)
#define AWS_ABS_DYNAMIC_FOLDER  (AWS_DOCUMENT_ROOT AWS_REL_DYNAMIC_FOLDER)

/* -------------------------------------------------------------------------
 *                               Connection States
 * ------------------------------------------------------------------------*/
enum connection_state {
    STATE_INITIAL,
    STATE_RECEIVING_DATA,
    STATE_REQUEST_RECEIVED,
    STATE_SENDING_DATA,
    STATE_SENDING_HEADER,
    STATE_SENDING_404,
    STATE_ASYNC_ONGOING,
    STATE_DATA_SENT,
    STATE_HEADER_SENT,
    STATE_404_SENT,
    STATE_CONNECTION_CLOSED,
    STATE_NO_STATE /* Used for assignment skeleton */
};

/* Helper macro to check if a connection is in a sending state */
#define OUT_STATE(s) ( \
    ((s) == STATE_SENDING_DATA)   || \
    ((s) == STATE_SENDING_HEADER) || \
    ((s) == STATE_SENDING_404)    )

/* -------------------------------------------------------------------------
 *                               Resource Types
 * ------------------------------------------------------------------------*/
enum resource_type {
    RESOURCE_TYPE_NONE,
    RESOURCE_TYPE_STATIC,
    RESOURCE_TYPE_DYNAMIC
};

/* -------------------------------------------------------------------------
 *                         Connection Structure
 * ------------------------------------------------------------------------*/
/**
 * Holds all necessary information for handling an HTTP connection:
 * file descriptors, buffers, HTTP parser, etc.
 */
struct connection {
    /* File (to be sent) descriptor and name */
    int  fd;
    char filename[BUFSIZ];

    /* Event-based file descriptor (if used) and socket descriptor */
    int eventfd;
    int sockfd;

    /* libaio context and control blocks */
    io_context_t ctx;
    struct iocb iocb;
    struct iocb *piocb[1];
    size_t file_size;

    /* Buffers for receiving and sending data */
    char   recv_buffer[BUFSIZ];
    size_t recv_len;

    char   send_buffer[BUFSIZ];
    size_t send_len;
    size_t send_pos;
    size_t file_pos;
    size_t async_read_len;

    /* HTTP request information */
    int  have_path;
    char request_path[BUFSIZ];
    enum resource_type    res_type;
    enum connection_state state;
    http_parser           request_parser;
};


#ifdef __cplusplus
}
#endif

#endif /* AWS_H_ */
