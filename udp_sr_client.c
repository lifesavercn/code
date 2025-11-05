/*
 udp_sr_client.c
 Selective Repeat ARQ over UDP
 Client side - full-duplex, logging, resume

 Compile:
   gcc udp_sr_client.c -o udp_sr_client -pthread

 Run:
   ./udp_sr_client

 This client mirrors the server: it can both send (with SR) and receive (SR receiver buffer).
*/

#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <unistd.h>
#include <arpa/inet.h>
#include <pthread.h>
#include <sys/time.h>
#include <time.h>
#include <stdarg.h>

#define CHUNK_SIZE 1024
#define PORT 8210
#define SERVER_IP "127.0.0.1"
#define MAX_PKT (CHUNK_SIZE + 16)
#define WINDOW_SIZE 8
#define TIMEOUT_USEC 500000
#define FILE_START_MSG "FILE_START"
#define FILE_END_MSG "FILE_END"

int sockfd;
struct sockaddr_in servaddr;
socklen_t servlen = sizeof(servaddr);
FILE *log_fp = NULL;

// logging
void log_event(const char *fmt, ...) {
    if (!log_fp) return;
    va_list ap; va_start(ap, fmt);
    time_t now = time(NULL); char *ts = strtok(ctime(&now), "\n");
    fprintf(log_fp, "[%s] ", ts);
    vfprintf(log_fp, fmt, ap);
    fprintf(log_fp, "\n");
    fflush(log_fp);
    va_end(ap);
}

long read_meta(const char *saved_name) {
    char meta[512]; snprintf(meta, sizeof(meta), "%s.meta", saved_name);
    FILE *m = fopen(meta, "r"); if (!m) return 0;
    long v=0; fscanf(m, "%ld", &v); fclose(m); return v;
}
void write_meta(const char *saved_name, long v) {
    char meta[512]; snprintf(meta, sizeof(meta), "%s.meta", saved_name);
    FILE *m = fopen(meta, "w"); if (!m) return; fprintf(m, "%ld", v); fclose(m);
}

#define HDR_LEN 9

typedef struct { int present; int len; char data[CHUNK_SIZE]; } sr_slot_t;

// receiver thread similar to server's
void *receiver_thread(void *arg) {
    (void)arg;
    char buf[MAX_PKT];
    FILE *fp = NULL;
    char filename[512];
    char saved_name[600];
    long total_chunks = 0;
    long last_delivered = 0;
    long base = 0;
    sr_slot_t window[WINDOW_SIZE];

    for (;;) {
        int n = recvfrom(sockfd, buf, sizeof(buf), 0, NULL, NULL);
        if (n <= 0) continue;
        if (strncmp(buf, FILE_START_MSG, strlen(FILE_START_MSG)) == 0) {
            char orig[512];
            if (sscanf(buf + strlen(FILE_START_MSG), "%s %ld", orig, &total_chunks) >= 1) {
                snprintf(filename, sizeof(filename), "%s", orig);
                snprintf(saved_name, sizeof(saved_name), "received_%s", filename);
                last_delivered = read_meta(saved_name);
                base = last_delivered + 1;
                fp = fopen(saved_name, last_delivered ? "ab" : "wb");
                if (!fp) { perror("fopen recv"); log_event("ERROR fopen %s", saved_name); continue; }
                for (int i=0;i<WINDOW_SIZE;i++) window[i].present=0;
                log_event("CLIENT START receiving '%s' resume=%ld total=%ld", filename, last_delivered, total_chunks);
                printf("\n[CLIENT] Receiving '%s' (resume %ld)\n", filename, last_delivered);
            }
            continue;
        }
        if (strncmp(buf, FILE_END_MSG, strlen(FILE_END_MSG)) == 0) {
            if (fp) { fclose(fp); fp=NULL; }
            log_event("CLIENT END '%s' delivered=%ld", filename, last_delivered);
            printf("\n[CLIENT] Finished receiving '%s' (delivered=%ld)\n", filename, last_delivered);
            continue;
        }

        if (n < HDR_LEN) continue;
        uint32_t seq_net; memcpy(&seq_net, buf, 4);
        uint32_t len_net; memcpy(&len_net, buf+4,4);
        uint8_t flags = (uint8_t)buf[8];
        uint32_t seq = ntohl(seq_net);
        uint32_t len = ntohl(len_net);
        if (len > CHUNK_SIZE) continue;
        char *payload = buf + HDR_LEN;

        long window_start = base;
        long window_end = window_start + WINDOW_SIZE - 1;
        if (seq >= window_start && seq <= window_end) {
            int idx = seq - window_start;
            if (!window[idx].present) {
                memcpy(window[idx].data, payload, len);
                window[idx].len = len;
                window[idx].present = 1;
                log_event("CLIENT RECV seq=%u len=%u stored idx=%d", seq, len, idx);
            } else {
                log_event("CLIENT duplicate seq=%u", seq);
            }
            // send ACK
            char ack[64]; snprintf(ack, sizeof(ack), "ACK:%u", seq);
            sendto(sockfd, ack, strlen(ack), 0, (struct sockaddr *)&servaddr, servlen);

            // deliver contiguous
            int moved=0;
            while (window[0].present) {
                if (fp) {
                    fwrite(window[0].data, 1, window[0].len, fp);
                    last_delivered++;
                    write_meta(saved_name, last_delivered);
                }
                for (int k=0;k<WINDOW_SIZE-1;k++) window[k]=window[k+1];
                window[WINDOW_SIZE-1].present=0;
                window[WINDOW_SIZE-1].len=0;
                moved=1;
                base = window_start + (moved?1:0);
                window_start = base;
            }
            if (moved) log_event("CLIENT delivered upto %ld", last_delivered);
        } else {
            if (seq < window_start) {
                char ack[64]; snprintf(ack, sizeof(ack), "ACK:%u", seq);
                sendto(sockfd, ack, strlen(ack), 0, (struct sockaddr *)&servaddr, servlen);
                log_event("CLIENT received old seq=%u, resent ACK", seq);
            } else {
                log_event("CLIENT received seq=%u outside window [%ld..%ld]", seq, window_start, window_end);
            }
        }
    }
    return NULL;
}

// sender thread SR similar to server version
typedef struct { long seq; int len; char data[CHUNK_SIZE]; int sent; struct timeval last_sent; int acked; } send_slot_t;
void timeval_now(struct timeval *tv) { gettimeofday(tv, NULL); }
long timeval_diff_usec(const struct timeval *a, const struct timeval *b) {
    return (a->tv_sec - b->tv_sec)*1000000L + (a->tv_usec - b->tv_usec);
}
long read_sender_meta(const char *filename) {
    char meta[512]; snprintf(meta,sizeof(meta),"%s.send.meta",filename);
    FILE *m=fopen(meta,"r"); if(!m) return 0; long v=0; fscanf(m,"%ld",&v); fclose(m); return v;
}
void write_sender_meta(const char *filename,long v) { char meta[512]; snprintf(meta,sizeof(meta),"%s.send.meta",filename); FILE *m=fopen(meta,"w"); if(!m) return; fprintf(m,"%ld",v); fclose(m); }

void *sender_thread(void *arg) {
    (void)arg;
    while (1) {
        printf("\nEnter filename to send (or 'exit'): ");
        char fname[512];
        if (scanf("%s", fname) != 1) continue;
        if (strncmp(fname,"exit",4)==0) {
            sendto(sockfd,"exit",4,0,(struct sockaddr *)&servaddr,servlen);
            log_event("CLIENT exit requested");
            break;
        }
        FILE *fp = fopen(fname,"rb");
        if (!fp) { perror("fopen send"); log_event("CLIENT cannot open %s", fname); continue; }
        fseek(fp,0,SEEK_END); long filesize = ftell(fp); fseek(fp,0,SEEK_SET);
        long total_chunks = (filesize + CHUNK_SIZE -1)/CHUNK_SIZE;
        char header[256]; snprintf(header,sizeof(header),"%s %s %ld", FILE_START_MSG, fname, total_chunks);
        sendto(sockfd, header, strlen(header), 0, (struct sockaddr *)&servaddr, servlen);
        log_event("CLIENT send FILE_START %s total=%ld", fname, total_chunks);

        long base = read_sender_meta(fname);
        long next_seq = base;
        send_slot_t window[WINDOW_SIZE];
        for (int i=0;i<WINDOW_SIZE;i++){ window[i].seq=-1; window[i].acked=0; window[i].sent=0; window[i].len=0; }

        fseek(fp, base*CHUNK_SIZE, SEEK_SET);
        for (int i=0;i<WINDOW_SIZE;i++) {
            if (next_seq < total_chunks) {
                int bytes = fread(window[i].data,1,CHUNK_SIZE,fp);
                window[i].seq = next_seq; window[i].len = bytes; window[i].acked=0; window[i].sent=0; next_seq++;
            } else window[i].seq=-1;
        }

        fd_set rfds; struct timeval tv;
        long base_seq = base;
        while (base_seq < total_chunks) {
            for (int i=0;i<WINDOW_SIZE;i++) {
                if (window[i].seq >= 0 && !window[i].acked) {
                    int do_send = 0;
                    if (!window[i].sent) do_send = 1;
                    else { struct timeval now; timeval_now(&now); if (timeval_diff_usec(&now,&window[i].last_sent) > TIMEOUT_USEC) do_send = 1; }
                    if (do_send) {
                        char pkt[HDR_LEN + CHUNK_SIZE];
                        uint32_t seq_net = htonl((uint32_t)window[i].seq);
                        uint32_t len_net = htonl((uint32_t)window[i].len);
                        memcpy(pkt, &seq_net, 4); memcpy(pkt+4, &len_net,4); uint8_t flags = (window[i].seq==total_chunks-1)?1:0; memcpy(pkt+8,&flags,1);
                        memcpy(pkt+HDR_LEN, window[i].data, window[i].len);
                        int sendlen = HDR_LEN + window[i].len;
                        sendto(sockfd, pkt, sendlen, 0, (struct sockaddr *)&servaddr, servlen);
                        timeval_now(&window[i].last_sent); window[i].sent=1;
                        log_event("CLIENT SENT seq=%ld len=%d", window[i].seq, window[i].len);
                        printf("[CLIENT] Sent seq=%ld len=%d\n", window[i].seq, window[i].len);
                    }
                }
            }
            FD_ZERO(&rfds); FD_SET(sockfd,&rfds); tv.tv_sec=0; tv.tv_usec = TIMEOUT_USEC/4;
            int rv = select(sockfd+1,&rfds,NULL,NULL,&tv);
            if (rv > 0 && FD_ISSET(sockfd,&rfds)) {
                char ackbuf[64]; int an = recvfrom(sockfd, ackbuf, sizeof(ackbuf)-1, 0, NULL, NULL);
                if (an <= 0) continue; ackbuf[an]='\0'; unsigned int ack_seq;
                if (sscanf(ackbuf,"ACK:%u",&ack_seq)==1) {
                    log_event("CLIENT RECV ACK:%u", ack_seq);
                    for (int i=0;i<WINDOW_SIZE;i++){ if (window[i].seq == (long)ack_seq) window[i].acked = 1; }
                    int slid = 1;
                    while (slid) {
                        slid = 0;
                        if (window[0].seq >= 0 && window[0].acked) {
                            base_seq = window[0].seq + 1;
                            write_sender_meta(fname, base_seq);
                            for (int k=0;k<WINDOW_SIZE-1;k++) window[k]=window[k+1];
                            window[WINDOW_SIZE-1].seq = -1; window[WINDOW_SIZE-1].acked=0; window[WINDOW_SIZE-1].sent=0; window[WINDOW_SIZE-1].len=0;
                            if (next_seq < total_chunks) { int bytes = fread(window[WINDOW_SIZE-1].data,1,CHUNK_SIZE,fp); window[WINDOW_SIZE-1].seq = next_seq; window[WINDOW_SIZE-1].len = bytes; window[WINDOW_SIZE-1].acked=0; window[WINDOW_SIZE-1].sent=0; next_seq++; }
                            slid = 1;
                        }
                    }
                }
            } else {
                // no ACKs within poll interval, will allow retransmit by timeout
            }
        }
        sendto(sockfd, FILE_END_MSG, strlen(FILE_END_MSG), 0, (struct sockaddr *)&servaddr, servlen);
        log_event("CLIENT completed send '%s' total=%ld", fname, (long) ( (filesize + CHUNK_SIZE -1)/CHUNK_SIZE) );
        printf("[CLIENT] Completed sending '%s'\n", fname);
        fclose(fp);
    }
    return NULL;
}

int main() {
    pthread_t t_recv, t_send;
    // open log
    log_fp = fopen("transfer_log.txt", "a");
    if (!log_fp) { perror("log open"); exit(1); }
    log_event("Client starting up, connecting to %s:%d", SERVER_IP, PORT);

    // create socket
    sockfd = socket(AF_INET, SOCK_DGRAM, 0);
    if (sockfd < 0) { perror("socket"); exit(1); }
    bzero(&servaddr, sizeof(servaddr));
    servaddr.sin_family = AF_INET;
    servaddr.sin_port = htons(PORT);
    servaddr.sin_addr.s_addr = inet_addr(SERVER_IP);

    // send hello to server (so server learns our address)
    char hello[] = "Hello from client";
    sendto(sockfd, hello, strlen(hello), 0, (struct sockaddr *)&servaddr, servlen);
    printf("Client sent hello to server\n");

    pthread_create(&t_recv, NULL, receiver_thread, NULL);
    pthread_create(&t_send, NULL, sender_thread, NULL);

    pthread_join(t_send, NULL);
    // optionally cancel receiver
    // pthread_cancel(t_recv);
    // pthread_join(t_recv, NULL);

    log_event("Client shutting down");
    fclose(log_fp);
    close(sockfd);
    return 0;
}

