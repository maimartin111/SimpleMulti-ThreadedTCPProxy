/*
 * transport.c 
 *
 * CPSC4510: Project 3 (STCP)
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
#include "mysock.h"
#include "stcp_api.h"
#include "transport.h"

/*Added by TD------*/
#include <time.h>
#include <stdlib.h>
#include <sys/time.h>

#define MAX_WIN_SIZE 3072  			/*size of window in bytes*/
#define MAX_SEQ_NUM 4294967296
#define OFFSET 5
#define HDR_SIZE sizeof(STCPHeader)
#define OPTIONS_SIZE 40 		//padding
#define MSS 536
#define CONGESTION_WIN_SIZE 3072
//-------------

typedef enum {
	CSTATE_LISTEN, /*listen for connections*/
	CSTATE_SYN_RECEIVED, /*wait for connection confirmation acknowledgment*/
	CSTATE_SYN_SENT, /*wait for matching connection req after sending req*/

	CSTATE_ESTABLISHED, /*connected*/

	/*Active side*/
	CSTATE_FIN_WAIT_1, /*wait for FIN or ACK for FIN sent, from remote TCP*/
	CSTATE_FIN_WAIT_2, /*wait for FIN from remote TCP*/
	CSTATE_CLOSING, /*wait for FIN ACK from remote TCP*/

	/*Passive side*/
	CSTATE_CLOSE_WAIT, /*wait for CLOSE connection from local user (app layer?)*/
	CSTATE_LAST_ACK, /*wait for ACK of FIN from remote TCP*/

	CSTATE_TIME_WAIT, CSTATE_CLOSED

};
/* you should have more states */

/* this structure is global to a mysocket descriptor */
typedef struct {
	bool_t done; /* TRUE once connection is closed */

	int connection_state; /* state of the connection (established, etc.) */
	tcp_seq initial_sequence_num;	//start in sender side
//-----------------
	tcp_seq initial_ack_num;		//start in receiver side

	/* any other connection-wide global variables go here */

	/*sender side*/
	tcp_seq seq_num_send; 			//furthest seq sent in window*/
	tcp_seq last_byte_acked; 		//* last byte acked in sender */

	/*receiver side*/
	tcp_seq ack_num; 				//next expected byte*/
	tcp_seq last_byte_read;		//app read up to this byte

	uint16_t sender_window_size;	//*sender window size*/
	uint16_t recv_window_size; 		//*receiver window size*/

	bool sender_called_Fin;	//keep track of who called fin first
	bool receiver_called_Fin;
//-----------------
} context_t;

/*represents a stcp packet*/
//typedef struct stcp_packet {
//	STCPHeader header;
//	char segment[STCP_MSS];   //is segment correct?
//	size_t data_len;
//} stcp_packet_t;


/*Function declarations*/
static void generate_initial_seq_num(context_t *ctx);
static void control_loop(mysocket_t sd, context_t *ctx);
int send_stcp_header(mysocket_t sd, STCPHeader *header, context_t *ctx,
		uint32_t offset, uint8_t flag);
static void handleActiveSide(mysocket_t sd, context_t *ctx, STCPHeader *header);
static void handlePassiveSide(mysocket_t sd, context_t *ctx, STCPHeader *header);

void handleAppData(mysocket_t sd, context_t *ctx);
//unsigned int handleAppData(mysocket_t sd, context_t *ctx);
void handleNetworkData(mysocket_t sd, context_t *ctx);

void set_stcp_header(STCPHeader *header, tcp_seq seq, tcp_seq ack,
		uint32_t data_offset, uint8_t flag, uint16_t win);

void send_segment_to_app(mysocket_t sd, context_t *ctx, char *segment,
		size_t segment_size, STCPHeader *hdr);

void getSTCPheader(STCPHeader *hdr, char *buf);

//int sendTCPheader(mysocket_t sd, STCPHeader *hdr, context_t *ctxt,
//		uint32_t data_offset, uint8_t flag);

uint16_t getLocalWindowSize(context_t *ctx);
uint16_t getRemoteWindowSize(context_t *ctx);

/* initialise the transport layer, and start the main loop, handling
 * any data from the peer or the application.  this function should not
 * return until the connection is closed.
 */
void transport_init(mysocket_t sd, bool_t is_active) {
	context_t *ctx;
	ctx = (context_t *) calloc(1, sizeof(context_t));
	assert(ctx);
	STCPHeader *header = (STCPHeader *) calloc(1, sizeof(STCPHeader));
	generate_initial_seq_num(ctx);

	//---------------------
//	unsigned int event = 0;
//	ssize_t bytes_sent_succ = 0;
	ctx->done = FALSE;
	ctx->sender_window_size = MAX_WIN_SIZE;
	ctx->recv_window_size = MAX_WIN_SIZE;
	//------------------------
	/* XXX: you should send a SYN packet here if is_active, or wait for one
	 * to arrive if !is_active.  after the handshake completes, unblock the
	 * application with stcp_unblock_application(sd).  you may also use
	 * this to communicate an error condition back to the application, e.g.
	 * if connection fails; to do so, just set errno appropriately (e.g. to
	 * ECONNREFUSED, etc.) before calling the function.
	 */

	/*client initiates connection, client sends SYN*/
	if (is_active) {
		handleActiveSide(sd, ctx, header);
	} else {
		handlePassiveSide(sd, ctx, header);
	}

	stcp_unblock_application(sd);

	if (header) {
		free(header);
		header = NULL;
	}

	/* Going to Control Loop if connection is estblished*/
	if (ctx->connection_state == CSTATE_ESTABLISHED)
		control_loop(sd, ctx);

	/* do any cleanup here */
	free(ctx);

	/*		ctx->sender_seq_num = ctx->initial_sequence_num;
	 prep syn packet to send
	 stcp_packet_t *pkt = (stcp_packet_t *) malloc(sizeof(stcp_packet_t)); //make a new packet
	 pkt->header.th_seq = ctx->initial_sequence_num;
	 pkt->header.th_flags |= TH_SYN;

	 3 way handshake
	 int sent_size = stcp_network_send(sd, pkt, sizeof(stcp_packet_t), NULL);

	 cout << "data_len " << pkt->data_len << endl;
	 cout << "sent_size " << sent_size << endl;

	 assert(sent_size == pkt->data_len);
	 ctx->connection_state = CSTATE_SYN_SENT;

	 shake 2: wait for syn-ack from server
	 event = stcp_wait_for_event(sd, NETWORK_DATA, NULL);
	 if (event && NETWORK_DATA) {	//need to check its an ACK message?

	 }

	 server side waits for syn
	 } else if (!is_active) {

	 }*/
}

/* generate random initial sequence number for an STCP connection */
/* random int between 0 and 255 */
static void generate_initial_seq_num(context_t *ctx) {

	srand(time(NULL));   // Initialization, should only be called once.

	assert(ctx);

#ifdef FIXED_INITNUM
	/* please don't change this! */
	ctx->initial_sequence_num = 1;
#else
	/* you have to fill this up */
	ctx->initial_sequence_num = rand() % 256;
#endif
}

static void handleActiveSide(mysocket_t sd, context_t *ctx, STCPHeader *header) {


	if (send_stcp_header(sd, header, ctx, OFFSET, 0 | TH_SYN) == -1) { // send opening connection SYN
		errno = ECONNREFUSED;
//		ctx->connection_state = CLOSED;
	} else {
		ctx->seq_num_send++;
		ctx->connection_state = CSTATE_SYN_SENT;
		memset(header, 0, HDR_SIZE);
		ssize_t bytes_transferred = stcp_network_recv(sd, header,
				sizeof(STCPHeader)); // receive SYN-ACK

		if (bytes_transferred <= 0 && !(header->th_flags & (TH_SYN | TH_ACK))) { //did not received ack for syn sent
			errno = ECONNREFUSED;
		} else {
			ctx->connection_state = CSTATE_SYN_RECEIVED;
//			ctx->initial_ack_num = ntohl(header->th_seq);
			ctx->ack_num = ntohl(header->th_seq);
			ctx->last_byte_read = ctx->ack_num;
			ctx->last_byte_acked = ntohl(header->th_ack);
			ctx->sender_window_size = MIN(CONGESTION_WIN_SIZE,
					ntohs(header->th_win));
//			ctx->sender_window_size = ntohs(header->th_win);

			bzero(header, HDR_SIZE);
			if (send_stcp_header(sd, header, ctx, OFFSET, 0 | TH_ACK) == -1) // send ACK for syn-ack
				errno = ECONNREFUSED;
			else
				ctx->connection_state = CSTATE_ESTABLISHED;
		}
	}
}

void handlePassiveSide(mysocket_t sd, context_t *ctx, STCPHeader *header) {
	ctx->connection_state = CSTATE_LISTEN;
	ssize_t bytes_transferred = stcp_network_recv(sd, (void *) header,
			sizeof(STCPHeader)); /*receive SYN from sender*/

	if (bytes_transferred <= 0)
		errno = ECONNREFUSED;
	else {
//		ctx->initial_ack_num = ntohl(header->th_seq);
		ctx->ack_num = ntohl(header->th_seq);
		ctx->last_byte_read = ctx->ack_num;
		ctx->recv_window_size = MIN(CONGESTION_WIN_SIZE, ntohs(header->th_win));
		ctx->connection_state = CSTATE_SYN_RECEIVED;
		memset(header, 0, HDR_SIZE);
		if (send_stcp_header(sd, header, ctx, OFFSET, 0 | TH_SYN | TH_ACK)
				== -1) /* Send SYN+ACK */
				{
			errno = ECONNREFUSED;

		} else {
			ctx->seq_num_send++;
			ctx->connection_state = CSTATE_SYN_SENT;

			memset(header, 0, HDR_SIZE);
			bytes_transferred = stcp_network_recv(sd, (void *) header,
					sizeof(STCPHeader)); /* Receive ACK */
			if (bytes_transferred <= 0) {
				errno = ECONNREFUSED;
			} else {
				ctx->last_byte_acked = ntohl(header->th_ack);
				ctx->recv_window_size = MIN(CONGESTION_WIN_SIZE,
						ntohs(header->th_win));

//				ctx->recv_window_size = ntohs(header->th_win);
				ctx->connection_state = CSTATE_ESTABLISHED;
			}
		}
	}
}

/* control_loop() is the main STCP loop; it repeatedly waits for one of the
 * following to happen:
 *   - incoming data from the peer
 *   - new data from the application (via mywrite())
 *   - the socket to be closed (via myclose())
 *   - a timeout
 */
static void control_loop(mysocket_t sd, context_t *ctx) {
	assert(ctx);
	assert(!ctx->done);
	/* Header variables for network and application */
	STCPHeader *app_header = (STCPHeader *) calloc(1, sizeof(STCPHeader));
	STCPHeader *network_header = (STCPHeader *) calloc(1, sizeof(STCPHeader));

	if (app_header == NULL || network_header == NULL) {
		return;
	}

//	ssize_t bytes_transferred;		//to check if transfer worked
//	uint16_t recver_win_space;		//space left in the windows to transfer
//	uint16_t sender_win_space;

//	size_t app_segment_size;

	/*var to handle Network data */
//	size_t network_packet_size;
//	size_t network_segment_size;
//	size_t payload_size;
//	tcp_seq ack_val = 0;
//	uint8_t data_offset;
//-----------
//	bool sender_called_Fin = false;
//	bool receiver_called_Fin = false;
	/* Sequence Number at which FIN is sent or received */
//	tcp_seq FIN_seq_num;
	struct timespec *abs_time = NULL;
	struct timeval tv;

	unsigned int wait_flag = ANY_EVENT;

	while (!ctx->done) {
		unsigned int event;

		ctx->recv_window_size = getRemoteWindowSize(ctx); // receiver window space left
		ctx->sender_window_size = getLocalWindowSize(ctx); // sender window space left

		/*FIN*/
		/* If there is a FIN then prepare for closing, wait for network fin-ack, no data from app layer!*/
		//can receive fin from receiver or fin-ack from receiver
		if (ctx->connection_state == CSTATE_FIN_WAIT_1
				|| ctx->connection_state == CSTATE_FIN_WAIT_2) {
			wait_flag = 0 | NETWORK_DATA;
//		/* Wait for any event if there is sufficient remote window */
		} else if (ctx->recv_window_size > 0) { //receiver window has room for more pkts
			wait_flag = 0 | ANY_EVENT;
			/* No Remote Window size left then only wait for NETWORK_DATA or APP_CLOSE_REQUESTED*/
		} else {
			wait_flag = 0 | NETWORK_DATA | APP_CLOSE_REQUESTED;
		}

		/*APP DATA*/
		/* see stcp_api.h or stcp_api.c for details of this function */
		/* XXX: you will need to change some of these arguments! */
		event = stcp_wait_for_event(sd, wait_flag, abs_time);
		/* check whether it was the network, app, or a close request */
		/* Application Data */
		if (event & APP_DATA) {
			if (ctx->recv_window_size > 0) { //there's room in receiver for data
				handleAppData(sd, ctx);		//handle the app data
			} else {
				wait_flag = 0 | NETWORK_DATA | APP_CLOSE_REQUESTED;
			}
		}

		/* Network Data */
		if (event & NETWORK_DATA) {
			if (ctx->sender_window_size > 0) {
				handleNetworkData(sd, ctx);
			}
		}

		/* app close requested */
		if ((event & APP_CLOSE_REQUESTED)
				&& (ctx->connection_state == CSTATE_ESTABLISHED)) {
			ctx->sender_called_Fin = TRUE;

			/* data left in app Q or network Q.*/
			if (event
					& (APP_DATA | NETWORK_DATA)&& ctx->connection_state == CSTATE_ESTABLISHED
					&& ctx->receiver_called_Fin==FALSE) {
				gettimeofday(&tv, NULL);
				abs_time = (struct timespec *) (&tv);
				abs_time->tv_sec += 1; /* wait for a second */
			}
			/* No app data in the app Q left so send final FIN to remote */
			else {
				/* send FIN */
				set_stcp_header(app_header, ctx->seq_num_send, ctx->ack_num,
				OFFSET, 0 | TH_ACK | TH_FIN, ctx->sender_window_size);
				stcp_network_send(sd, app_header, HDR_SIZE, NULL);
				bzero(app_header, HDR_SIZE);
				ctx->seq_num_send++;
				ctx->connection_state = CSTATE_FIN_WAIT_1;
				ctx->sender_called_Fin = FALSE;
				abs_time = NULL;
			}
		}
	}

	/*	if (app_header)
	 free(app_header);
	 if (network_header)
	 free(network_header);

	 app_header = NULL;
	 network_header = NULL;*/
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
void our_dprintf(const char *format, ...) {
	va_list argptr;
	char buffer[1024];

	assert(format);
	va_start(argptr, format);
	vsnprintf(buffer, sizeof(buffer), format, argptr);
	va_end(argptr);
	fputs(buffer, stdout);
	fflush(stdout);
}

int send_stcp_header(mysocket_t sd, STCPHeader *header, context_t *ctx,
		uint32_t offset, uint8_t flag) {
	header->th_seq = htonl(ctx->seq_num_send);
	header->th_ack = htonl(ctx->ack_num + 1);
	header->th_off = offset;
	header->th_flags = flag;
	header->th_win = htons(ctx->sender_window_size);
	return stcp_network_send(sd, header, sizeof(STCPHeader), NULL);

}

void handleAppData(mysocket_t sd, context_t *ctx) {
	ssize_t bytes_transferred;
	STCPHeader *app_header = (STCPHeader *) calloc(1, sizeof(STCPHeader));

//	if (curr_remote_window > 0) {
	size_t segment_size = MSS; //max size of segment
	if (ctx->recv_window_size < MSS) {	//adjust size of segment to send
		segment_size = (size_t) ctx->recv_window_size;
	}

	/*buffer to hold data from app*/
	char *segment = (char *) calloc(1, segment_size);
	segment_size = stcp_app_recv(sd, segment, segment_size);

	/*got some data from app layer*/
	if (segment_size > 0) {
		/* No TCP options are set */
		set_stcp_header(app_header, ctx->seq_num_send, ctx->ack_num, OFFSET,
				0 | TH_ACK, ctx->sender_window_size);

		bytes_transferred = stcp_network_send(sd, app_header, HDR_SIZE, segment,
				segment_size, NULL);

		/*error*/
		if (bytes_transferred < 0) {
			errno = ECONNREFUSED;
			ctx->done = TRUE;
			if (segment) {
				free(segment);
				segment = NULL;
			}
			return;
		}

		//update seq num to next
		ctx->seq_num_send = (ctx->seq_num_send + segment_size);
	}

//	} else {
//		return (0 | NETWORK_DATA | APP_CLOSE_REQUESTED);
//	}

	if (segment) {
		free(segment);
		segment = NULL;
	}
	bzero(app_header, HDR_SIZE);
}

void handleNetworkData(mysocket_t sd, context_t *ctx) {
	STCPHeader *network_header = (STCPHeader *) calloc(1, sizeof(STCPHeader));
	ssize_t payload_size = MSS;
	size_t network_packet_size;
	size_t network_segment_size;
	tcp_seq ack_val = 0;
	uint8_t data_offset;

	if (ctx->sender_window_size < MSS) {
		payload_size = ctx->sender_window_size - OPTIONS_SIZE;
	}

	/* Full payload with header + options*/
	network_packet_size = HDR_SIZE + OPTIONS_SIZE + payload_size;
	char *network_packet = (char *) calloc(1, network_packet_size);	//buffer to read data into

	/* Receive network packet from peer*/
	network_packet_size = stcp_network_recv(sd, network_packet,
			network_packet_size);

	/* packet received should be at least a header size */
	if (network_packet_size >= HDR_SIZE) {
		/* copy 20 bytes (header) from received packet to local header*/
		getSTCPheader(network_header, network_packet);

		ctx->recv_window_size = MIN(CONGESTION_WIN_SIZE,
				ntohs(network_header->th_win));

		/* Received ACK flag from peer */
		if (network_header->th_flags & TH_ACK) {
			ack_val = ntohl(network_header->th_ack);

			ctx->last_byte_acked = ack_val;

			/* caase 1: received FIN-ACK for FIN sent before peers' FIN */
			if (ctx->connection_state == CSTATE_FIN_WAIT_1)
				ctx->connection_state = CSTATE_FIN_WAIT_2;

			/* case 2: received FIN-ACK for FIn sent after remote's FIN sent */
			if (ctx->receiver_called_Fin) {
				/* all FINs sent and acked so close connection */
				ctx->done = TRUE;
				ctx->receiver_called_Fin = FALSE;

				/*free memory*/
				if (network_packet) {
					free(network_packet);
					network_packet = NULL;
				}
				return;		//done
			}
		}

		/*received FIN flag from peer*/
		if (network_header->th_flags & TH_FIN) {
			ctx->ack_num = ntohl(network_header->th_seq);

			/* sent a FIN but now get another FIN from peer */
			if (ctx->connection_state == CSTATE_FIN_WAIT_2) {
//				/* Well, application must not be blocked at this point on myread call as */
//				/* app queue was  cleared before . But Still to remain safe. */
				stcp_fin_received(sd);

				/* send FIN-ACK for peer's FIN  */
				bzero(network_header, HDR_SIZE);
				set_stcp_header(network_header, ctx->seq_num_send, ctx->ack_num,
				OFFSET, 0 | TH_ACK, ctx->sender_window_size);

				stcp_network_send(sd, network_header, HDR_SIZE, NULL);

				/* NOW close the connection */
				ctx->done = TRUE;
				if (network_packet) {
					free(network_packet);
					network_packet = NULL;
				}
				return;
			}
			/* received FIN */
			if (ctx->connection_state == CSTATE_ESTABLISHED) {
				/*received FIN from peer so stop transmission from application*/
				stcp_fin_received(sd);
				ctx->receiver_called_Fin = TRUE;
			}
		}

		data_offset = (network_header->th_off);	//offset value

		network_segment_size = network_packet_size - (data_offset * 4);

		/* if get to here, must have some payload in network packet */
		if (network_segment_size > 0
				&& ctx->connection_state == CSTATE_ESTABLISHED) {
			if (ctx->sender_window_size < network_segment_size) {
				ctx->sender_window_size = 0;
			} else {
				ctx->sender_window_size -= network_segment_size;
			}

			assert(network_segment_size <= MSS);

			//buffer to hold data
			char *network_segment = (char *) calloc(1, network_segment_size);
			//copy from the packet to buffer,  excluding the header
			bcopy(network_packet + (data_offset * 4), network_segment,
					network_segment_size);

			//********************
			/*
			 tcp_seq seq_num = ntohl(network_hdr->th_seq);
			 ctx->ack_num = (seq_num + network_segment_size - 1);
			 ctx->last_byte_read = ctx->ack_num;


			 stcp_app_send(sd, network_segment, network_segment_size);

			 stcp_app_send(sd, ctx, network_segment, network_segment_size,
			 network_header);
			 */

			send_segment_to_app(sd, ctx, network_segment, network_segment_size,
					network_header);

			ctx->sender_window_size += network_segment_size;
			if (ctx->sender_window_size > MAX_WIN_SIZE) {
				ctx->sender_window_size = MAX_WIN_SIZE;
			}

			/*prep ack message header to send*/
			bzero(network_header, HDR_SIZE);
			set_stcp_header(network_header, ctx->seq_num_send, ctx->ack_num,
			OFFSET, 0 | TH_ACK, ctx->sender_window_size);
			/* send ack for pkt received */
			stcp_network_send(sd, network_header, HDR_SIZE, NULL);

			/* remote sent FIN first, so send FIN-ACK back*/
		} else if (ctx->receiver_called_Fin
				&& ctx->connection_state == CSTATE_ESTABLISHED) {
			ctx->last_byte_read = ctx->ack_num;

			bzero(network_header, HDR_SIZE);

			//*************
			set_stcp_header(network_header, ctx->seq_num_send, ctx->ack_num + 1,
			OFFSET, 0 | TH_ACK, ctx->sender_window_size);

			stcp_network_send(sd, network_header, HDR_SIZE, NULL);
		}
		//**************
		bzero(network_header, HDR_SIZE);
	} else if (network_packet_size == 0) { /* EOF or peer pressed ctrl+c */
		stcp_fin_received(sd);

		ctx->done = TRUE;

		if (network_packet) {
			free(network_packet);
			network_packet = NULL;
		}

		return;
	}

	if (network_packet) {
		free(network_packet);
		network_packet = NULL;
	}
}

uint16_t getLocalWindowSize(context_t *ctx) {
	assert(ctx);
	return (uint16_t)(
			ctx->sender_window_size - (ctx->ack_num - ctx->last_byte_read));
}

//******************************
void send_segment_to_app(mysocket_t sd, context_t *ctx, char *segment,
		size_t segment_size, STCPHeader *hdr) {
	assert(segment);
	assert(ctx);
	assert(hdr);

	tcp_seq seq_num = ntohl(hdr->th_seq);
	tcp_seq expected_seq_num = (ctx->ack_num + 1);

	/* sequence number is the same as we expected */
	if (seq_num == expected_seq_num) {
		ctx->ack_num = (seq_num + segment_size - 1);
		stcp_app_send(sd, segment, segment_size);
		ctx->last_byte_read = ctx->ack_num;
	}
	/* Sequence number less than expected */
	else if (seq_num < expected_seq_num) {
		/* Case I: If there is some new data */
		if ((seq_num + segment_size - 1) >= expected_seq_num) {
			ctx->ack_num = (seq_num + segment_size - 1);
			stcp_app_send(sd, segment + (expected_seq_num - seq_num),
					seq_num + segment_size - expected_seq_num);
			ctx->last_byte_read = ctx->ack_num;
		}
		/* Case II: No new data */
		else {
			; /* Duplicate Data */
		}
	}
}

//int sendTCPheader(mysocket_t sd, STCPHeader *hdr, context_t *ctxt,
//		uint32_t data_offset, uint8_t flag) {
//	hdr->th_seq = htonl(ctxt->seq_num_send);
//	hdr->th_ack = htonl(ctxt->ack_num + 1);
//	hdr->th_off = data_offset;
//	hdr->th_flags = flag;
//	hdr->th_win = htons(ctxt->sender_window_size);
//	return stcp_network_send(sd, hdr, sizeof(STCPHeader), NULL);
//}

void getSTCPheader(STCPHeader *hdr, char *buf) {
	assert(hdr);
	bcopy(buf, hdr, HDR_SIZE);
	return;
}

void set_stcp_header(STCPHeader *header, tcp_seq seq, tcp_seq ack,
		uint32_t data_offset, uint8_t flag, uint16_t win) {
	header->th_seq = htonl(seq);
	header->th_ack = htonl(ack);
	header->th_off = data_offset;
	header->th_flags = flag;
	header->th_win = htons(win);
	return;
}

uint16_t getRemoteWindowSize(context_t *ctx) {
	assert(ctx);
	return (uint16_t)(
			ctx->recv_window_size - (ctx->seq_num_send - ctx->last_byte_acked));
}
