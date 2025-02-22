#include <stdio.h>
#include <string.h>

#include "protocol.h"
#include "datalink.h"

#define DATA_TIMER  2000
#define MAX_SEQ 31

struct FRAME { 
    unsigned char kind; /* FRAME_DATA */
    unsigned char ack;
    unsigned char seq;
    unsigned char data[PKT_LEN]; 
    unsigned int  padding;
};

static unsigned char send_buffer[MAX_SEQ + 1][PKT_LEN];
static unsigned char recv_buffer[PKT_LEN];
static unsigned char frame_nr = 0;
static unsigned char nbuffered = 0;
static unsigned char frame_expected = 0;
static unsigned char next_frame_to_send = 0;
static unsigned char ack_expected = 0;
static int phl_ready = 0;

static void put_frame(unsigned char *frame, int len)
{
    *(unsigned int *)(frame + len) = crc32(frame, len);
    send_frame(frame, len + 4);
    phl_ready = 0;
}

static unsigned char inc(unsigned char nr)
{
    return (nr + 1) % (MAX_SEQ + 1);
}

static unsigned char between(unsigned char a, unsigned char b, unsigned char c)
{
    if (((a <= b) && (b < c)) || ((a <= b) && (c < a)) || ((c < a) && (b < c))) return 1;
    return 0;
}

static void send_data_frame(unsigned char frame_nr, unsigned char frame_expected)
{
    struct FRAME s;

    s.kind = FRAME_DATA;
    s.seq = frame_nr;
    s.ack = (frame_expected + MAX_SEQ) % (MAX_SEQ + 1);
    memcpy(s.data, send_buffer[frame_nr], PKT_LEN);

    dbg_frame("Send DATA %d %d, ID %d\n", s.seq, s.ack, *(short *)s.data);

    put_frame((unsigned char *)&s, 3 + PKT_LEN);
    start_timer(frame_nr, DATA_TIMER);
}

int main(int argc, char **argv)
{
    int event, arg;
    struct FRAME f;
    int len = 0;

    protocol_init(argc, argv); 
    lprintf("Designed by Suo Zhengduo, build: " __DATE__"  "__TIME__"\n");

    enable_network_layer();

    for (;;) {
        event = wait_for_event(&arg);

        switch (event) {
        case NETWORK_LAYER_READY:
            get_packet(send_buffer[next_frame_to_send]);
            nbuffered++;
            send_data_frame(next_frame_to_send,frame_expected);
            next_frame_to_send = inc(next_frame_to_send);
            break;

        case PHYSICAL_LAYER_READY:
            phl_ready = 1;
            break;

        case FRAME_RECEIVED: 
            len = recv_frame((unsigned char *)&f, sizeof f);
            if (len < 5 || crc32((unsigned char *)&f, len) != 0) {
                dbg_event("**** Receiver Error, Bad CRC Checksum\n");
                break;
            }

            dbg_frame("Recv DATA %d %d, ID %d\n", f.seq, f.ack, *(short *)f.data);
            if (f.seq == frame_expected) {
                put_packet(f.data, len - 7);
                frame_expected = inc(frame_expected);
            }

            while (between(ack_expected, f.ack, next_frame_to_send)) {
                stop_timer(ack_expected);
                nbuffered--;
                ack_expected = inc(ack_expected);
            }
            break; 

        case DATA_TIMEOUT:
            dbg_event("---- DATA %d timeout\n", arg); 
            next_frame_to_send = ack_expected;
            for (unsigned char i = 1; i <= nbuffered; i++) {
                send_data_frame(next_frame_to_send, frame_expected);
                next_frame_to_send = inc(next_frame_to_send);
            }
            break;
        }

        if (nbuffered < MAX_SEQ && phl_ready)
            enable_network_layer();
        else
            disable_network_layer();
   }
}
