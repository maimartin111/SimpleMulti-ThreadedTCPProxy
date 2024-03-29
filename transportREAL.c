/*
 * transport.c
 *
 *	Project 3
 *
 * This file implements the STCP layer that sits between the
 * mysocket and network layers. You are required to fill in the STCP
 * functionality in this file.
 *
 */

#include <stdio.h>
#include <stdarg.h>
#include <string.h>
#include <stdlib.h>
#include <assert.h>
#include <arpa/inet.h>
#include <sys/time.h>
#include "mysock.h"
#include "stcp_api.h"
#include "transport.h"

enum
{
    LISTEN,
    SYN_SENT,
    SYN_RECEIVED,
    CSTATE_ESTABLISHED,
    FIN_WAIT_1,
    FIN_WAIT_2,
    CLOSE_WAIT,
    CLOSING,
    LAST_ACK,
    TIME_WAIT,
    CLOSED
}; /* you should have more states */

#define MAX_SEQ_NUM 4294967296
#define ISN_RANGE 256
#define WIN_SIZE 3072
#define CONGESTION_WIN_SIZE 3072
#define OFFSET 5
#define MSS 536
#define HDR_SIZE sizeof(STCPHeader)
#define OPTIONS_SIZE 40

/* this structure is global to a mysocket descriptor */
typedef struct
{
    bool_t done; /* TRUE once connection is closed */

    int connection_state; /* state of the connection (established, etc.) */
    tcp_seq initial_sequence_num;

    /*Used when send data from app to network and to determine remote_advertised_window*/
    tcp_seq sequence_num;
    tcp_seq last_byte_acked;

    /*Used when send data from network to app and to determine local_advertised_window*/
    tcp_seq ack_num;
    tcp_seq last_byte_read;

    /* Remote and local advertised window */
    uint16_t remote_advertised_window;
    uint16_t local_advertised_window;

    /* any other connection-wide global variables go here */
} context_t;

static void generate_initial_seq_num(context_t *ctx);
static void control_loop(mysocket_t sd, context_t *ctx);

/* My Functions */

uint16_t getRemoteWindowSize(context_t *ctx);
uint16_t getLocalWindowSize(context_t *ctx);
void setSTCPheader(STCPHeader *hdr, tcp_seq seq, tcp_seq ack, uint32_t data_offset, uint8_t flag, uint16_t win);
void getSTCPheader(STCPHeader *hdr, char *buf);
void printSTCPheader(STCPHeader *hdr);
void send_segment_to_app(mysocket_t sd, context_t *ctx, char *segment, size_t segment_size, STCPHeader *hdr);
void printContext(context_t *ctx);

/* Send STCP header with appropriate flags to peer  */
int sendTCPheader(mysocket_t sd, STCPHeader *hdr, context_t *ctxt, uint32_t data_offset, uint8_t flag)
{
    hdr->th_seq = htonl(ctxt->sequence_num);
    hdr->th_ack = htonl(ctxt->ack_num + 1);
    hdr->th_off = data_offset;
    hdr->th_flags = flag;
    hdr->th_win = htons(ctxt->local_advertised_window);
    return stcp_network_send(sd, hdr, sizeof(STCPHeader), NULL);
}

static void handleActiveSide(mysocket_t sd, context_t *ctx, STCPHeader *header)
{
    setSTCPheader(header, ctx->initial_sequence_num, -1, OFFSET, 0 | TH_SYN, ctx->local_advertised_window);

    if (sendTCPheader(sd, header, ctx, OFFSET, 0 | TH_SYN) == -1) // Send SYN
    {
        errno = ECONNREFUSED;
    }
    else
    {
        ctx->sequence_num++;
        ctx->connection_state = SYN_SENT;
        //bzero(header, HDR_SIZE);
        memset(header, 0, HDR_SIZE);
        int bytes_transfer = stcp_network_recv(sd, header, sizeof(STCPHeader)); // Receive SYN+ACK
        if (bytes_transfer <= 0 && !(header->th_flags & (TH_SYN | TH_ACK)))
        {
            errno = ECONNREFUSED;
        }
        else
        {
            ctx->connection_state = SYN_RECEIVED;
            ctx->ack_num = ntohl(header->th_seq);
            ctx->last_byte_read = ctx->ack_num;
            ctx->last_byte_acked = ntohl(header->th_ack);
            ctx->remote_advertised_window = MIN(CONGESTION_WIN_SIZE, ntohs(header->th_win));

            bzero(header, HDR_SIZE);
            if (sendTCPheader(sd, header, ctx, OFFSET, 0 | TH_ACK) == -1) // Send ACK
                errno = ECONNREFUSED;
            else
                ctx->connection_state = CSTATE_ESTABLISHED;
        }
    }
}

static void handlePassiveSide(mysocket_t sd, context_t *ctx, STCPHeader *header)
{
    ctx->connection_state = LISTEN;
    int bytes_transfer = stcp_network_recv(sd, (void *)header, sizeof(STCPHeader)); /*Receive SYN*/

    if (bytes_transfer <= 0 && !(header->th_flags & TH_SYN))
        errno = ECONNREFUSED;
    else
    {
        ctx->ack_num = ntohl(header->th_seq);
        ctx->last_byte_read = ctx->ack_num;
        ctx->remote_advertised_window = MIN(CONGESTION_WIN_SIZE, ntohs(header->th_win));
        ctx->connection_state = SYN_RECEIVED;
        memset(header, 0, HDR_SIZE);
        if (sendTCPheader(sd, header, ctx, OFFSET, 0 | TH_SYN | TH_ACK) == -1) /* Send SYN+ACK */
        {
            errno = ECONNREFUSED;
            //ctx->connection_state = CLOSED;
            printf("ERROR: Unable to send syn+ack to peer\nCLOSING CONNECTION\n");
        }
        else
        {
            ctx->sequence_num++;
            ctx->connection_state = SYN_SENT;

            memset(header, 0, HDR_SIZE);
            bytes_transfer = stcp_network_recv(sd, (void *)header, sizeof(STCPHeader)); /* Receive ACK */
            if (bytes_transfer <= 0)
            {
                errno = ECONNREFUSED;
            }
            else
            {
                ctx->last_byte_acked = ntohl(header->th_ack);
                ctx->remote_advertised_window = MIN(CONGESTION_WIN_SIZE, ntohs(header->th_win));
                ctx->connection_state = CSTATE_ESTABLISHED;
            }
        }
    }
}

/* initialise the transport layer, and start the main loop, handling
 * any data from the peer or the application.  this function should not
 * return until the connection is closed.
 */
void transport_init(mysocket_t sd, bool_t is_active)
{
    // check
    context_t *ctx;
    ssize_t bytes_transfer;
    STCPHeader *header = (STCPHeader *)calloc(1, sizeof(STCPHeader));
    ctx = (context_t *)calloc(1, sizeof(context_t));
    assert(ctx);
    generate_initial_seq_num(ctx);

    ctx->done = FALSE;
    ctx->local_advertised_window = WIN_SIZE;
    ctx->remote_advertised_window = WIN_SIZE; // Default window size

    /* XXX: you should send a SYN packet here if is_active, or wait for one
     * to arrive if !is_active.  after the handshake completes, unblock the
     * application with stcp_unblock_application(sd).  you may also use
     * this to communicate an error condition back to the application, e.g.
     * if connection fails; to do so, just set errno appropriately (e.g. to
     * ECONNREFUSED, etc.) before calling the function.
     */
    // Active connection
    if (is_active)
    {
        handleActiveSide(sd, ctx, header);
    }
    // Passive connection
    else
    {
        handlePassiveSide(sd, ctx, header);
    }

    stcp_unblock_application(sd);

    if (header)
    {
        free(header);
        header = NULL;
    }

    /* Going to Control Loop if connection is estblished*/
    if (ctx->connection_state == CSTATE_ESTABLISHED)
        control_loop(sd, ctx);

    /* do any cleanup here */
    free(ctx);
}

/* generate random initial sequence number for an STCP connection */
static void generate_initial_seq_num(context_t *ctx)
{
    assert(ctx);

#ifdef FIXED_INITNUM
    /* please don't change this! */
    ctx->initial_sequence_num = 1;
#else
    /* you have to fill this up */
    /*ctx->initial_sequence_num =;*/
    srand(time(NULL));
    ctx->initial_sequence_num = (tcp_seq)(rand() % ISN_RANGE);
    ctx->sequence_num = ctx->initial_sequence_num;
    ctx->ack_num = ctx->sequence_num; /* Can be Random, so initializing to its own sequence_num */
    ctx->last_byte_acked = ctx->sequence_num;
#endif
}

/* control_loop() is the main STCP loop; it repeatedly waits for one of the
 * following to happen:
 *   - incoming data from the peer
 *   - new data from the application (via mywrite())
 *   - the socket to be closed (via myclose())
 *   - a timeout
 */
static void control_loop(mysocket_t sd, context_t *ctx)
{
    assert(ctx);
    assert(!ctx->done);

    /* Header variables for network and application */
    STCPHeader *app_hdr = (STCPHeader *)calloc(1, sizeof(STCPHeader));
    STCPHeader *network_hdr = (STCPHeader *)calloc(1, sizeof(STCPHeader));

    if (app_hdr == NULL || network_hdr == NULL)
    {
        printf("Error: Unable to allocate space. Insufficient Memory!\n");
        ctx->connection_state = CLOSED;
        return;
    }

    ssize_t bytes_transfer;

    uint16_t curr_remote_window;
    uint16_t curr_local_window;

    /* Variables to handle Application Data */
    size_t app_segment_size;

    /* Variables to handle Network Data */
    size_t network_packet_size;
    size_t network_segment_size;
    size_t payload_size;

    tcp_seq ack_value = 0;
    uint8_t dataOffset;

    /* Boolean Variables  */
    /* LocalFINcalled: Set to 1 when I send FIN first*/
    int LocalFINcalled = 0;
    /* RemoteFINcalled: Set to 1 when peer sends FIN first*/
    int RemoteFINcalled = 0;

    /* Sequence Number at which FIN is sent or received */
    tcp_seq FIN_seq_num;

    struct timespec *abs_time = NULL;

    unsigned int waitFlags = ANY_EVENT;

    while (!ctx->done)
    {
        unsigned int event;

        curr_remote_window = getRemoteWindowSize(ctx); /* Remote window Size Left */
        curr_local_window = getLocalWindowSize(ctx);   /* Local window Size Left */

        /* If FIN has been sent or received then No APP_DATA would be entertained */
        if (ctx->connection_state == FIN_WAIT_1 || ctx->connection_state == FIN_WAIT_2)
            waitFlags = 0 | NETWORK_DATA;
        /* Wait for any event if there is sufficient remote window */
        else if (curr_remote_window > 0)
            waitFlags = 0 | ANY_EVENT;
        /* No Remote Window size left then only wait for NETWORK_DATA or APP_CLOSE_REQUESTED*/
        else
            waitFlags = 0 | NETWORK_DATA | APP_CLOSE_REQUESTED;

        /* see stcp_api.h or stcp_api.c for details of this function */
        /* XXX: you will need to change some of these arguments! */
        event = stcp_wait_for_event(sd, waitFlags, abs_time);
        /* check whether it was the network, app, or a close request */
        /* Application Data */
        if (event & APP_DATA)
        {
            /* the application has requested that data be sent */
            /* see stcp_app_recv() */

            app_segment_size = MSS; /* Payload Limit is a MSS */

            if (curr_remote_window > 0)
            {
                if (curr_remote_window < MSS)
                    app_segment_size = (size_t)curr_remote_window;

                char *app_segment = (char *)calloc(1, app_segment_size);
                app_segment_size = stcp_app_recv(sd, app_segment, app_segment_size);

                if (app_segment_size > 0)
                {
                    /* No TCP options are set */
                    setSTCPheader(app_hdr, ctx->sequence_num, ctx->ack_num, OFFSET, 0 | TH_ACK, ctx->local_advertised_window);

                    bytes_transfer = stcp_network_send(sd, app_hdr, HDR_SIZE, app_segment, app_segment_size, NULL);

                    if (bytes_transfer < 0)
                    {
                                                printf("--IF:340-- \n");

                        /* Peer may have closed connection abruptly*/
                        /* No other possibilities, as no packet loss is assumed*/
                        errno = ECONNREFUSED;
                        ctx->done = TRUE; /* I don't think I should try again */
                        if (app_segment)
                        {
                            free(app_segment);
                            app_segment = NULL;
                        }
                        continue;
                    }

                    ctx->sequence_num = (ctx->sequence_num + app_segment_size) % MAX_SEQ_NUM;
                }

                if (app_segment)
                {
                    free(app_segment);
                    app_segment = NULL;
                }
                bzero(app_hdr, HDR_SIZE);
            }
            else
            {
                waitFlags = 0 | NETWORK_DATA | APP_CLOSE_REQUESTED;
            }
        }
        /* Network Data */
        if (event & NETWORK_DATA)
        {   
            printf("--IF:368-- \n");
            payload_size = MSS;

            if (curr_local_window > 0)
            {   
                printf("--IF:373-- \n");
                if (curr_local_window < MSS)
                {
                    /* Removing OPTIONS_SIZE also from payload_size because if there are no options then more payload will be
                        read due to OPTIONS_SIZE*/
                    /* Such case won't occur as every time local window is advertised 3072 to peer*/
                    payload_size = curr_local_window - OPTIONS_SIZE;
                }
                /* Full Payload with header and options*/
                network_packet_size = HDR_SIZE + OPTIONS_SIZE + payload_size;
                char *network_packet = (char *)calloc(1, network_packet_size);

                /* Receive network packet from peer*/
                network_packet_size = stcp_network_recv(sd, network_packet, network_packet_size);

                /* Received packet should be of length greater than equal to STCP header Size */
                if (network_packet_size >= HDR_SIZE)
                {   
                    printf("--IF:391-- \n");
                    /* Copies first 20 bytes i.e. Header Size from network_packet to network_hdr */
                    getSTCPheader(network_hdr, network_packet);

                    ctx->remote_advertised_window = MIN(CONGESTION_WIN_SIZE, ntohs(network_hdr->th_win));

                    /* ACK Flag is set by peer */
                    if (network_hdr->th_flags & TH_ACK)
                    {   
                        printf("--IF:400--\n ");
                        ack_value = ntohl(network_hdr->th_ack);

                        /* ack_value lies between window */
                        if (ack_value >= ctx->last_byte_acked && ack_value <= ctx->sequence_num)
                        {   printf("--IF:407--\n ");
                            ctx->last_byte_acked = ack_value;

                            /* Case I: Got ACK of my FIN which was sent before the peer's FIN */
                            if (ctx->connection_state == FIN_WAIT_1 && (ack_value - 1) >= FIN_seq_num){
                                printf("--IF:412-- \n");
                                ctx->connection_state = FIN_WAIT_2;}

                            /* Case II: Got ACK of my FIN which was sent after the peer's FIN */
                            if (RemoteFINcalled == 1 && (ack_value - 1) >= FIN_seq_num)
                            {   
                                printf("--IF:416--\n ");
                                /* All FINs are sent and ACKed so Now closing connection */
                                //ctx->connection_state = CLOSED;
                                ctx->done = TRUE;
                                RemoteFINcalled = 0;

                                if (network_packet)
                                {   printf("--IF:424--\n ");
                                    free(network_packet);
                                    network_packet = NULL;
                                }
                                continue;
                            }
                        }
                        else
                        {   printf("--ELSE:433-- \n");
                            ; /* ACK flag is not set. Such case can occur only when */
                              /* ACK flag is not set deliberately by the peer */
                        }
                    }
                    if (network_hdr->th_flags & TH_FIN)
                    {   
                        printf("--IF:438-- \n");
                        ctx->ack_num = ntohl(network_hdr->th_seq);

                        /* Already sent the FIN and now got the FIN of peer */
                        if (ctx->connection_state == FIN_WAIT_2)
                        {   printf("--IF:444-- \n");
                            /* Well, application must not be blocked at this point on myread call as */
                            /* app queue was  cleared before . But Still to remain safe. */
                            stcp_fin_received(sd);

                            /* Send ACK of peer's FIN  */
                            bzero(network_hdr, HDR_SIZE);
                            setSTCPheader(network_hdr, ctx->sequence_num, ctx->ack_num, OFFSET, 0 | TH_ACK, ctx->local_advertised_window);

                            stcp_network_send(sd, network_hdr, HDR_SIZE, NULL);

                            /* NOW close the connection */
                            //ctx->connection_state = CLOSED;
                            ctx->done = TRUE;
                            if (network_packet)
                            {   printf("--IF:449--\n ");
                                free(network_packet);
                                network_packet = NULL;
                            }
                            continue;
                        }
                        /* Receive FIN from peer */
                        if (ctx->connection_state == CSTATE_ESTABLISHED)
                        {   printf("--IF:467-- \n");
                            /* Suspending all transmission to and fro from application */
                            /* As packets are assumed to be arrived in order so no more data packets will arrive */
                            stcp_fin_received(sd);
                            RemoteFINcalled = 1;
                        }
                    }

                    dataOffset = (network_hdr->th_off);
                    network_segment_size = network_packet_size - (dataOffset * 4);

                    /* There must be some payload in network packet */
                    /* If connection state is not established then no need to send data to application */
                    if (network_segment_size > 0 && ctx->connection_state == CSTATE_ESTABLISHED)
                    {
                         printf("--IF:485--\n ");
                        if (ctx->local_advertised_window < network_segment_size)
                            ctx->local_advertised_window = 0;
                        else
                            ctx->local_advertised_window -= network_segment_size;

                        assert(network_segment_size <= MSS);

                        char *network_segment = (char *)calloc(1, network_segment_size);
                        bcopy(network_packet + (dataOffset * 4), network_segment, network_segment_size);

                        // send_segment_to_app(sd, ctx, network_segment, network_segment_size, network_hdr);
                        tcp_seq seq_num = ntohl(network_hdr->th_seq);
                        ctx->ack_num = (seq_num + network_segment_size - 1) % MAX_SEQ_NUM;
                        ctx->last_byte_read = ctx->ack_num;

                        stcp_app_send(sd, network_segment, network_segment_size);

                        ctx->local_advertised_window += network_segment_size;
                        if (ctx->local_advertised_window > WIN_SIZE)
                            ctx->local_advertised_window = WIN_SIZE;

                        /* Send Ack */
                        bzero(network_hdr, HDR_SIZE);
                        setSTCPheader(network_hdr, ctx->sequence_num, ctx->ack_num, OFFSET, 0 | TH_ACK, ctx->local_advertised_window);

                        stcp_network_send(sd, network_hdr, HDR_SIZE, NULL);
                        /* ACK sent */
                    }
                    /* If remote sent FIN first and no payload is appended then sending ACK of FIN */
                    else if (RemoteFINcalled == 1 && ctx->connection_state == CSTATE_ESTABLISHED)
                    {
                         printf("--IF:517--\n ");
                        ctx->last_byte_read = ctx->ack_num;

                        bzero(network_hdr, HDR_SIZE);
                        setSTCPheader(network_hdr, ctx->sequence_num, ctx->ack_num, OFFSET, 0 | TH_ACK, ctx->local_advertised_window);

                        stcp_network_send(sd, network_hdr, HDR_SIZE, NULL);
                    }

                    bzero(network_hdr, HDR_SIZE);
                }
                /* Detected a EOF of socket or peer may have presses Ctrl+C */
                else if (network_packet_size == 0)
                {   printf("--ELIF:525- \n");
                    stcp_fin_received(sd);

                    //ctx->connection_state = CLOSED;
                    ctx->done = TRUE;

                    if (network_packet)
                    {
                        free(network_packet);
                        network_packet = NULL;
                    }

                    continue;
                }
                if (network_packet)
                {
                    free(network_packet);
                    network_packet = NULL;
                }
            }
        }
        /* APP_CLOSE_REQUESTED */
        if ((event & APP_CLOSE_REQUESTED || LocalFINcalled == 1 || RemoteFINcalled == 1) && (ctx->connection_state == CSTATE_ESTABLISHED))
        {
             printf("--IF:551--\n ");
            LocalFINcalled = 1;

            /* Send FIN */
            setSTCPheader(app_hdr, ctx->sequence_num, ctx->ack_num, OFFSET, 0 | TH_ACK | TH_FIN, ctx->local_advertised_window);
            stcp_network_send(sd, app_hdr, HDR_SIZE, NULL);
            bzero(app_hdr, HDR_SIZE);
            FIN_seq_num = ctx->sequence_num;
            ctx->sequence_num++;
            ctx->connection_state = FIN_WAIT_1;
            LocalFINcalled = 0;
            abs_time = NULL;
        }
        /* etc. */
    }

    /* Clean Up */
    if (app_hdr)
        free(app_hdr);
    if (network_hdr)
        free(network_hdr);

    app_hdr = NULL;
    network_hdr = NULL;
}

/**********************************************************************/
/* our_dprintf
 *
 * Send a formatted message to stdout.
 *
 * format               A printf-style format string.
 *
 * This function is equivalent to a printf, but may be
 * changed to log errors to a file if desired.
 *
 * Calls to this function are generated by the dprintf amd
 * dperror macros in transport.h
 */
void our_dprintf(const char *format, ...)
{
    va_list argptr;
    char buffer[1024];

    assert(format);
    va_start(argptr, format);
    vsnprintf(buffer, sizeof(buffer), format, argptr);
    va_end(argptr);
    fputs(buffer, stdout);
    fflush(stdout);
}

// void send_segment_to_app(mysocket_t sd, context_t *ctx, char *segment, size_t segment_size, STCPHeader *hdr)
// {
//     assert(segment);
//     assert(ctx);
//     assert(hdr);

//     tcp_seq seq_num = ntohl(hdr->th_seq);
//     ctx->ack_num = (seq_num + segment_size - 1) % MAX_SEQ_NUM;
//     ctx->last_byte_read = ctx->ack_num;

//     stcp_app_send(sd, segment, segment_size);
// }

void getSTCPheader(STCPHeader *hdr, char *buf)
{
    assert(hdr);
    bcopy(buf, hdr, HDR_SIZE);
    return;
}

void setSTCPheader(STCPHeader *hdr, tcp_seq seq, tcp_seq ack, uint32_t data_offset, uint8_t flag, uint16_t win)
{
    hdr->th_seq = htonl(seq);
    hdr->th_ack = htonl((ack + 1) % MAX_SEQ_NUM);
    hdr->th_off = data_offset;
    hdr->th_flags = flag;
    hdr->th_win = htons(win);
    return;
}

uint16_t getLocalWindowSize(context_t *ctx)
{
    assert(ctx);
    return (uint16_t)(ctx->local_advertised_window -
                      (ctx->ack_num - ctx->last_byte_read));
}

uint16_t getRemoteWindowSize(context_t *ctx)
{
    assert(ctx);
    return (uint16_t)(ctx->remote_advertised_window -
                      (ctx->sequence_num - ctx->last_byte_acked));
}