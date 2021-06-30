#include <assert.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <unistd.h>
#include "uv.h"

#include "utils.h"

#define N_BACKLOG 64
typedef enum { INITIAL_ACK, WAIT_FOR_MSG, IN_MSG } ProcessingState;

#define SENDBUF_SIZE 1024

typedef struct {
    ProcessingState state;
    char sendbuf[SENDBUF_SIZE];
    int sendbuf_end;
    uv_tcp_t *client;
} peer_state_t;

void on_alloc_buffer(uv_handle_t *handle, size_t suggested_size, uv_buf_t * buf)
{
     buf->base = (char*)xmalloc(suggested_size);
     buf->len = suggested_size;   
}

void on_client_closed(uv_handle_t *handle)
{
    uv_tcp_t* client = (uv_tcp_t*)handle;
    if (client->data) {
        free(client->data);
    }
    free(client);
}

void on_wrote_buf(uv_write_t *req, int status)
 {
    if (status) {
        fprintf(stderr, "Write error: %s\n", uv_strerror(status));
        exit(1);
    }
    peer_state_t *peerstate = (peer_state_t*)req->data;

    if (peerstate->sendbuf_end >= 3 &&
        peerstate->sendbuf[peerstate->sendbuf_end -3] == 'X' &&
        peerstate->sendbuf[peerstate->sendbuf_end -2] == 'Y' &&
        peerstate->sendbuf[peerstate->sendbuf_end -1] == 'Z')
    {
            free(peerstate);
            free(req);
            uv_stop(uv_default_loop());
            return;
    }
    peerstate->sendbuf_end = 0;
    free(req);
 }

void on_peer_read(uv_stream_t *client, ssize_t nread, const uv_buf_t *buf)
{
    if (nread < 0) {
        if (nread != UV_EOF) {
            fprintf(stderr, "Read error: %s\n", uv_strerror(nread));
        }
        uv_close((uv_handle_t*)client, on_client_closed);
    } else if (nread == 0) {

    } else {
        assert(buf->len >= nread);

        peer_state_t *peerstate = (peer_state_t*)client->data;
        if (peerstate->state == INITIAL_ACK) {
            free(buf->base);
            return;
        }

        // Run the protocol state machine.
        for (int i=0; i<nread; ++i) {
            switch (peerstate->state) {
            case INITIAL_ACK:
                assert (0 && "can't reach here");
                break;
            case WAIT_FOR_MSG:
                if (buf->base[i] == '^') {
                    peerstate->state = IN_MSG;
                }
                break;
            case IN_MSG:
                if (buf->base[i] == '$') {
                    peerstate->state = WAIT_FOR_MSG;
                } else {
                    assert(peerstate->sendbuf_end < SENDBUF_SIZE);
                    peerstate->sendbuf[peerstate->sendbuf_end++] = buf->base[i] + 1;
                }
                break;
            }
        }

        if (peerstate->sendbuf_end > 0) {
            uv_buf_t writebuf = uv_buf_init(peerstate->sendbuf, peerstate->sendbuf_end);
            uv_write_t *writereq = (uv_write_t*)xmalloc(sizeof(*writereq));
            writereq->data = peerstate;
            int rc;
            if ((rc = uv_write(writereq, (uv_stream_t*)client, &writebuf, 1, on_wrote_buf)) < 0) {
                fprintf(stderr, "uv_write failed: %s", uv_strerror(rc));
                exit(1);
            }

        }
    }
    free(buf->base);
}
