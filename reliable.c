#include <stdio.h>
#include <string.h>
#include <unistd.h>
#include <stdlib.h>
#include <stddef.h>
#include <assert.h>
#include <poll.h>
#include <errno.h>
#include <time.h>
#include <sys/time.h>
#include <sys/socket.h>
#include <sys/uio.h>
#include <netinet/in.h>

#include "rlib.h"


#define EOF_RECV(flag)    (flag & 0x01)
#define EOF_SENT(flag)    (flag & 0x02)
#define ALL_SENT(flag)    (flag & 0x04)
#define ALL_WRITTEN(flag) (flag & 0x08)

#define SET_EOF_RECV(flag)    (flag | 0x01)
#define SET_EOF_SENT(flag)    (flag | 0x02)
#define SET_ALL_SENT(flag)    (flag | 0x04)
#define SET_ALL_WRITTEN(flag) (flag | 0x08)


typedef struct slice {
    char marked;
    char segment[500];
    uint16_t len;
} slice;

struct reliable_state {
    rel_t *next;			/* Linked list for traversing all connections */
    rel_t **prev;

    conn_t *c;			/* This is the connection object */

    slice* recv_buffer;
    slice* send_buffer;

    size_t recv_seqno;
    size_t send_seqno;
    size_t window_size;
    size_t already_written;

    char last_marked_already_sent;
    char flags;

};
rel_t *rel_list;


/* Creates a new reliable protocol session, returns NULL on failure.
* ss is always NULL */
rel_t * rel_create (conn_t *c, const struct sockaddr_storage *ss, const struct config_common *cc)
{
    rel_t *r;
    r = xmalloc (sizeof (*r));
    memset (r, 0, sizeof (*r));

    if (!c) {
        c = conn_create (r, ss);
        if (!c) {
            free (r);
            return NULL;
        }
    }

    r->c    = c;
    r->next = rel_list;
    r->prev = &rel_list;
    if (rel_list) rel_list->prev = &r->next;
    rel_list = r;

    r->window_size = cc->window;
    r->recv_buffer = malloc( sizeof(slice) * r->window_size);
    assert(r->recv_buffer != NULL && "Malloc failed!");

    r->send_buffer = malloc( sizeof(slice) * r->window_size);
    assert(r->send_buffer != NULL && "Malloc failed!");

    r->recv_seqno      = 1;
    r->send_seqno      = 1;
    r->flags           = 0;
    r->already_written = 0;
    r->last_marked_already_sent = 1;

    return r;
}

void rel_destroy (rel_t *r)
{
    if (r->next) r->next->prev = r->prev;
    *r->prev = r->next;
    conn_destroy (r->c);

    /* Free any other allocated memory here */
    free(r->recv_buffer);
    free(r->send_buffer);
    free(r);
}


void rel_recvpkt (rel_t *r, packet_t *pkt, size_t n)
{
    // network to host endianess
    uint16_t pkt_len   = ntohs(pkt->len);
    uint32_t pkt_ackno = ntohl(pkt->ackno);

    // check size of packet
    if(n < 8) return;
    if(pkt_len != n ) return;

    // verify checksum
    if(cksum(pkt, n) != 0 ) return;

    // mark acknowledged packets
    if (r->send_seqno < pkt_ackno) {
        for (uint16_t i = r->send_seqno; i < pkt_ackno; i++) {
            r->send_buffer[i % r->window_size].marked = 0;
        }
        r->send_seqno = pkt_ackno;
    }

    // in case of an ack-packet,the function is done
    if (n == 8) return;

    // handle data
    // check if seqno is in current window range
    uint32_t pkt_seqno = ntohl(pkt->seqno);
    size_t lower_bound = r->recv_seqno;
    size_t upper_bound = lower_bound + r-> window_size;
    if (pkt_seqno < lower_bound || pkt_seqno >= upper_bound ) return;

    // calculate index in window
    size_t index = pkt_seqno % r->window_size;

    // ignore duplicated incoming packets
    if (r->recv_buffer[index].marked) return;

    // store data in window
    // might want to check this out. Could be wrong in a horrible way.
    if( pkt_len == 12 ){
        SET_EOF_RECV(r->flags);
    }
    else{
        memcpy( &(r->recv_buffer[index].segment), &(pkt->data), n - 12);
        r->recv_buffer[index].len    = n - 12;
        r->recv_buffer[index].marked = 1;
    }

    // initiate data output
    if (pkt_seqno == r->recv_seqno) rel_output(r);
}


void rel_read (rel_t *r)
{
    slice*   fill_me_up;
    char     buffer[500];
    int16_t  recieved_bytes;
    uint16_t available_space;

    size_t upper_bound    = r->send_seqno + r->window_size;
    size_t first_unmarked = r->send_seqno;

    while ( first_unmarked < upper_bound ) {
        if ( r->send_buffer[first_unmarked % r->window_size].marked ) {
            first_unmarked++;
        }
        else {
            break;
        }
    }

    // no space available
    if ( r->send_buffer[first_unmarked % r->window_size].marked ) return;

    if ( r->last_marked_already_sent ) {
			fill_me_up = &(r->send_buffer[first_unmarked % r->window_size]);
			available_space = 500;
	}
	else {
			fill_me_up = &(r->send_buffer[(first_unmarked - 1 + r->window_size) % r->window_size]);
			available_space = 500 - fill_me_up->len;
	}

    recieved_bytes = conn_input(r->c, (void *)buffer, available_space);

    switch (recieved_bytes){
        case 0 :
            // nothing to read
            return;

        case -1:
            // EOF
            SET_EOF_RECV(r->flags);
            break;

        default:
            //packet and stuff ?
            break;
    }
}

void send_ack(rel_t *r) {
    struct ack_packet pkt;

    pkt.cksum = 0;
    pkt.len   = htons(8);
    pkt.ackno = htonl(r->recv_seqno);

    // compute checksum
    pkt.cksum = cksum(&pkt, 8);
    conn_sendpkt(r->c, (packet_t*) &pkt, 8);
}

void rel_output (rel_t *r)
{
    char flag = 0;
    while( r->recv_buffer[r->recv_seqno].marked ) {
        slice* s = &(r->recv_buffer[r->recv_seqno % r->window_size]);
        size_t written = conn_output(
                                        r->c,
                                        &(s->segment) + r->already_written ,
                                        s->len - r->already_written
                                    );

        if (written == s->len - r->already_written) {
            // full packet written
            r->recv_seqno++;
            r->already_written = 0;
            flag = 1;
        }
        else {
            // packet partially written
            r->already_written = written;
            break;
        }

    }

    if (flag) {
        send_ack(r);
    }
}

void rel_timer ()
{
    /* Retransmit any packets that need to be retransmitted */
    packet_t pkt;
    slice current_slice;

    int all_ackwoledged = 1;
    slice *send_buffer = rel_list->send_buffer;
    size_t window_size = rel_list->window_size;
    size_t upper_bound = rel_list->send_seqno + window_size;

    // go through window
    for(size_t slice_no = rel_list->recv_seqno; slice_no < upper_bound; slice_no++){
        current_slice = send_buffer[slice_no % window_size];

        // if packet is unackwnoledged
        if(current_slice.marked == 0){

            pkt.len   = htons(current_slice.len);
            pkt.seqno = htonl(slice_no);
            pkt.ackno = htonl(rel_list->recv_seqno);
            memcpy( &current_slice.segment, &pkt.data, current_slice.len);
            pkt.cksum = cksum(&pkt, pkt.len);

            conn_sendpkt(rel_list->c, &pkt, pkt.len);
            all_ackwoledged = 0;
        }
    }

    // Set correct flag if all packets where correctly recieved on the other side
    if( all_ackwoledged && EOF_SENT(rel_list->flags) ){
        SET_ALL_SENT(rel_list->flags);
    }
}
