#include <stdio.h>
#include <string.h>

#include "protocol.h"
#include "datalink.h"

#define DATA_TIMER 4500
#define ACK_TIMER 300
#define MAX_SEQ 63
#define NR_BUFS ((MAX_SEQ + 1) / 2)

struct FRAME { 
    unsigned char kind; //帧类型(数据帧(1)、ACK帧(2)、NAK帧(3))
    unsigned char ack; //ack序号
    unsigned char seq; //帧序号
    unsigned char data[PKT_LEN]; //网络层数据
    unsigned int  padding; //CRC32校验位
};

static unsigned char recv_buffer[NR_BUFS][PKT_LEN]; //接收方缓存
static unsigned char send_buffer[NR_BUFS][PKT_LEN]; //发送方缓存
static unsigned char arrived[NR_BUFS]; //标志数组，标志接收方缓存中帧的情况
static unsigned char nbuffered = 0; //窗口大小
static unsigned char frame_expected = 0; //接收方下沿
static unsigned char next_frame_to_send = 0; //发送方上沿
static unsigned char ack_expected = 0; //发送方下沿
static unsigned char too_far = NR_BUFS; //接收方上沿
static unsigned char no_nak = 1; //标志位，标志是否发送了NAK
static int phl_ready = 0; //标志物理层准备状态

static inline unsigned char inc(unsigned char nr)
{
    return (nr + 1) % (MAX_SEQ + 1);
}

static unsigned char between(unsigned char a, unsigned char b, unsigned char c)
{
    if (((a <= b) && (b < c)) || ((a <= b) && (c < a)) || ((c < a) && (b < c))) return 1;
    return 0;
}

static void put_frame(unsigned char *frame, int len)
{
    *(unsigned int *)(frame + len) = crc32(frame, len);
    send_frame(frame, len + 4);
    phl_ready = 0;
}

static void send_data_frame(unsigned char fk, unsigned char next_frame_to_send, unsigned char frame_expected)
{
    struct FRAME s;

    s.kind = fk;
    s.seq = next_frame_to_send;
    s.ack = (frame_expected + MAX_SEQ) % (MAX_SEQ + 1);
    
    switch (fk) {
    case FRAME_DATA:
        memcpy(s.data, send_buffer[next_frame_to_send % NR_BUFS], PKT_LEN);
        dbg_frame("Send DATA %d %d, ID %d\n", s.seq, s.ack, *(short*)s.data);
        start_timer(next_frame_to_send % NR_BUFS, DATA_TIMER);
        put_frame((unsigned char*)&s, 3 + PKT_LEN);
        break;
    case FRAME_ACK:
        dbg_frame("Send ACK %d\n", s.ack);
        put_frame((unsigned char*)&s, 2);
        break;
    case FRAME_NAK:
        no_nak = 0; //已发送NAK，置位NAK
        dbg_frame("Send NAK %d\n", s.ack);
        put_frame((unsigned char*)&s, 2);
        break;
    }

    stop_ack_timer();
}

int main(int argc, char **argv)
{
    int event, arg;
    struct FRAME f;
    int len = 0;
    for (unsigned i = 0; i < NR_BUFS; i++) arrived[i] = 0; //标记未收到帧

    protocol_init(argc, argv); 
    lprintf("Designed by Suo Zhengduo, build: " __DATE__"  "__TIME__"\n");

    enable_network_layer();

    for (;;) {
        event = wait_for_event(&arg);

        dbg_frame("Window : %d\n", nbuffered);

        switch (event) {
        case NETWORK_LAYER_READY:
            get_packet(send_buffer[next_frame_to_send % NR_BUFS]);
            nbuffered++;
            send_data_frame(FRAME_DATA, next_frame_to_send, frame_expected);
            next_frame_to_send = inc(next_frame_to_send);
            break;

        case PHYSICAL_LAYER_READY:
            phl_ready = 1;
            break;

        case FRAME_RECEIVED:
            len = recv_frame((unsigned char*)&f, sizeof f);
            if (len < 5 || crc32((unsigned char*)&f, len) != 0) {
                dbg_event("**** Receiver Error, Bad CRC Checksum\n");
                if (no_nak) send_data_frame(FRAME_NAK, 0, frame_expected);
                break;
            }

            switch (f.kind) {
            case FRAME_DATA:
                dbg_frame("Recv DATA %d %d, ID %d\n", f.seq, f.ack, *(short*)f.data);
                if ((f.seq != frame_expected) && no_nak) send_data_frame(FRAME_NAK, 0, frame_expected);
                else start_ack_timer(ACK_TIMER);
                if (between(frame_expected, f.seq, too_far) && (arrived[f.seq % NR_BUFS] == 0)) {
                    arrived[f.seq % NR_BUFS] = 1;
                    memcpy(recv_buffer[f.seq % NR_BUFS], f.data, PKT_LEN);
                    while (arrived[frame_expected % NR_BUFS]) { //按序提交缓存中的帧至网络层
                        put_packet(recv_buffer[frame_expected % NR_BUFS], len - 7);
                        no_nak = 1; //收到新帧，恢复NAK标志位
                        arrived[frame_expected % NR_BUFS] = 0;
                        frame_expected = inc(frame_expected);
                        too_far = inc(too_far);
                        start_ack_timer(ACK_TIMER);
                    }
                }
                break;

            case FRAME_NAK:
                dbg_frame("Recv NAK %d\n", f.ack);
                if (between(ack_expected, (f.ack + 1) % (MAX_SEQ + 1), next_frame_to_send))
                    send_data_frame(FRAME_DATA, (f.ack + 1) % (MAX_SEQ + 1), frame_expected);
                break;

            case FRAME_ACK:
                dbg_frame("Recv ACK %d\n", f.ack);
                break;
            }

            while (between(ack_expected, f.ack, next_frame_to_send)) { //累积确认
                nbuffered--;
                stop_timer(ack_expected % NR_BUFS);
                ack_expected = inc(ack_expected);
            }
            break;

        case DATA_TIMEOUT:
            dbg_event("---- DATA %d timeout\n", arg);
            if (between(ack_expected, arg, next_frame_to_send)) send_data_frame(FRAME_DATA, arg, frame_expected);
            else send_data_frame(FRAME_DATA, (arg + NR_BUFS) % (MAX_SEQ + 1), frame_expected);
            break;

        case ACK_TIMEOUT:
            dbg_event("---- ACK %d timeout\n", arg);
            send_data_frame(FRAME_ACK, 0, frame_expected);
            break;
        }

        if (nbuffered < NR_BUFS && phl_ready)
            enable_network_layer();
        else
            disable_network_layer();
   }
}