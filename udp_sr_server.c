/*
 udp_sr_server.c
 Selective Repeat ARQ over UDP
 Server side - full-duplex, logging, resume

 Compile:
   gcc udp_sr_server.c -o udp_sr_server -pthread

 Run:
   ./udp_sr_server

 This server:
  - waits for a client's hello to learn client's address
  - can receive files (Selective Repeat receive buffering + ACKs)
  - can send files using Selective Repeat sender with per-packet timers
  - logs events to transfer_log.txt
  - stores resume metadata in "<filename>.meta"
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
#include <errno.h>

#define CHUNK_SIZE 1024            // payload bytes per data packet
#define PORT 8210                  // server port
#define MAX_PKT (CHUNK_SIZE + 16)  // header + payload safety
#define WINDOW_SIZE 8              // selective repeat window size (tweak for performance)
#define TIMEOUT_USEC 500000        // retransmission timeout (microseconds)
#define FILE_START_MSG "FILE_START"// text header before file start: "FILE_START <filename> <total_chunks>"
#define FILE_END_MSG "FILE_END"

int sockfd;
struct sockaddr_in cliaddr;
socklen_t addrlen = sizeof(cliaddr);
FILE *log_fp = NULL;

// ---------- Logging utility ----------
void log_event(const char *fmt, ...) {
    if (!log_fp) return;
    va_list ap;
    va_start(ap, fmt);
    time_t now = time(NULL);
    char *ts = strtok(ctime(&now), "\n");
    fprintf(log_fp, "[%s] ", ts);
    vfprintf(log_fp, fmt, ap);
    fprintf(log_fp, "\n");
    fflush(log_fp);
    va_end(ap);
}

// ---------- Helpers for meta files (resume) ----------
// meta file stores one number: last_delivered (number of chunks written)
long read_meta(const char *saved_name) {
    char meta[512];
    snprintf(meta, sizeof(meta), "%s.meta", saved_name);
    FILE *m = fopen(meta, "r");
    if (!m) return 0;
    long v = 0;
    fscanf(m, "%ld", &v);
    fclose(m);
    return v;
}
void write_meta(const char *saved_name, long value) {
    char meta[512];
    snprintf(meta, sizeof(meta), "%s.meta", saved_name);
    FILE *m = fopen(meta, "w");
    if (!m) return;
    fprintf(m, "%ld", value);
    fclose(m);
}

// ---------- Packet header layout (we send header bytes then data) ----------
// Header (9 bytes): [seq (4 bytes network)] [len (4 bytes network)] [flags (1 byte)]
// Flags: bit0 = 1 -> last chunk (end)
#define HDR_LEN 9

// ---------- Receiver (Selective Repeat) ----------
// Behavior:
//   - expects to receive prior a "FILE_START <orig_filename> <total_chunks>"
//   - maintains window buffer of WINDOW_SIZE starting at base = last_delivered+1
//   - stores incoming chunks (within window) to in-memory buffer, sends ACK for each packet
//   - whenever contiguous chunks starting at base exist, write them to file and advance base
//   - on restart, uses <saved_filename>.meta to resume from last_delivered chunks already written

typedef struct {
    int present;                 // 0/1 whether data is stored
    int len;                     // bytes in data
    char data[CHUNK_SIZE];       // actual payload
} sr_slot_t;

void *receiver_thread(void *arg) {
    (void)arg;
    char buf[MAX_PKT];
    char textbuf[2048];

    FILE *fp = NULL;
    char filename[512];
    char saved_name[600];
    long total_chunks = 0;
    long last_delivered = 0; // number of chunks already written to file
    long base = 0;           // next expected chunk index to deliver (last_delivered + 1)
    // selective repeat buffer
    sr_slot_t window[WINDOW_SIZE];

    for (;;) {
        // receive packet or text
        int n = recvfrom(sockfd, buf, sizeof(buf), 0, (struct sockaddr *)&cliaddr, &addrlen);
        if (n <= 0) continue;

        // Attempt to parse text header messages first
        buf[n] = '\0';
        if (strncmp(buf, FILE_START_MSG, strlen(FILE_START_MSG)) == 0) {
            // format: FILE_START <orig_name> <total_chunks>
            char orig[512];
            if (sscanf(buf + strlen(FILE_START_MSG), "%s %ld", orig, &total_chunks) >= 1) {
                snprintf(filename, sizeof(filename), "%s", orig);
                snprintf(saved_name, sizeof(saved_name), "received_%s", filename);

                last_delivered = read_meta(saved_name); // how many chunks already written
                base = last_delivered + 1;

                // open file - append if resuming
                fp = fopen(saved_name, last_delivered ? "ab" : "wb");
                if (!fp) {
                    perror("fopen receive");
                    log_event("ERROR: cannot open '%s' for writing", saved_name);
                    continue;
                }
                // clear window buffer
                for (int i = 0; i < WINDOW_SIZE; ++i) window[i].present = 0;

                log_event("START receiving '%s' total_chunks=%ld resume_from=%ld", filename, total_chunks, last_delivered);
                printf("\n[SERVER] Receiving '%s' -> saved as '%s' (resume from chunk %ld)\n", filename, saved_name, last_delivered);
            }
            continue;
        }
        if (strncmp(buf, FILE_END_MSG, strlen(FILE_END_MSG)) == 0) {
            // close and finalize
            if (fp) {
                fclose(fp);
                fp = NULL;
            }
            log_event("END receiving '%s' (delivered=%ld)", filename, last_delivered);
            printf("\n[SERVER] Finished receiving '%s' (chunks delivered=%ld)\n", filename, last_delivered);
            continue;
        }

        // Otherwise process binary header + data: expect at least HDR_LEN bytes
        if (n < HDR_LEN) continue;
        // extract header
        uint32_t seq_net;
        memcpy(&seq_net, buf, 4);
        uint32_t len_net;
        memcpy(&len_net, buf+4, 4);
        uint8_t flags = (uint8_t)buf[8];
        uint32_t seq = ntohl(seq_net);
        uint32_t len = ntohl(len_net);
        // safety
        if (len > CHUNK_SIZE) continue;
        // pointer to payload
        char *payload = buf + HDR_LEN;

        // Compute window range
        long window_start = base;
        long window_end = window_start + WINDOW_SIZE - 1;

        // If seq is within current window, store and ACK
        if (seq >= window_start && seq <= window_end) {
            int idx = seq - window_start; // index within window
            // store data if not already stored
            if (!window[idx].present) {
                memcpy(window[idx].data, payload, len);
                window[idx].len = len;
                window[idx].present = 1;
                log_event("RECV pkt seq=%u len=%u (stored idx=%d window_start=%ld)", seq, len, idx, window_start);
            } else {
                // duplicate -- already present
                log_event("RECV duplicate pkt seq=%u (ignored store)", seq);
            }
            // send ACK (text form)
            char ackmsg[64];
            snprintf(ackmsg, sizeof(ackmsg), "ACK:%u", seq);
            sendto(sockfd, ackmsg, strlen(ackmsg), 0, (struct sockaddr *)&cliaddr, addrlen);

            // attempt to deliver contiguous chunks starting at base
            int moved = 0;
            while (window[0].present) {
                // write window[0] to file
                if (fp) {
                    fwrite(window[0].data, 1, window[0].len, fp);
                    last_delivered++;
                    write_meta(saved_name, last_delivered); // persist resume point
                }
                // shift window left by one
                for (int i = 0; i < WINDOW_SIZE-1; ++i) {
                    window[i] = window[i+1];
                }
                window[WINDOW_SIZE-1].present = 0;
                window[WINDOW_SIZE-1].len = 0;
                moved = 1;
                base = window_start + (moved ? 1 : 0); // update window_start
                window_start = base;
            }
            if (moved) {
                log_event("Delivered up to chunk %ld", last_delivered);
            }
        } else {
            // Out-of-window packet:
            // If it's less than base (already delivered), resend ACK for that seq (helpful if sender missed ack)
            if (seq < window_start) {
                char ackmsg[64];
                snprintf(ackmsg, sizeof(ackmsg), "ACK:%u", seq);
                sendto(sockfd, ackmsg, strlen(ackmsg), 0, (struct sockaddr *)&cliaddr, addrlen);
                log_event("RECV out-of-window seq=%u (< base=%ld), resent ACK", seq, window_start);
            } else {
                // seq > window_end: ignore or optionally send NACK/ACK for highest in-order
                log_event("RECV pkt seq=%u outside window [%ld..%ld], ignored", seq, window_start, window_end);
            }
        }
    }
    return NULL;
}

// ---------- Sender (Selective Repeat) ----------
// Sender maintains a buffer of WINDOW_SIZE slots representing seq = base .. base+W-1.
// It sends packets that are populated and not acked, and waits for ACKs using select().
// Metadata: we persist base (last_contiguous_acked) in "<filename>.meta" so sender can resume.

typedef struct {
    long seq;                 // absolute sequence number
    int len;                  // payload length
    char data[CHUNK_SIZE];    // payload
    int sent;                 // has been sent at least once
    struct timeval last_sent; // timestamp of last send
    int acked;                // 0/1
} send_slot_t;

void timeval_now(struct timeval *tv) {
    gettimeofday(tv, NULL);
}
long timeval_diff_usec(const struct timeval *a, const struct timeval *b) {
    // return a - b in usec
    return (a->tv_sec - b->tv_sec) * 1000000L + (a->tv_usec - b->tv_usec);
}

long read_sender_meta(const char *filename) {
    // sender meta stores base (last contiguous ACKed chunks)
    char meta[512];
    snprintf(meta, sizeof(meta), "%s.send.meta", filename);
    FILE *m = fopen(meta, "r");
    if (!m) return 0;
    long v=0;
    fscanf(m, "%ld", &v);
    fclose(m);
    return v;
}
void write_sender_meta(const char *filename, long v) {
    char meta[512];
    snprintf(meta, sizeof(meta), "%s.send.meta", filename);
    FILE *m = fopen(meta, "w");
    if (!m) return;
    fprintf(m, "%ld", v);
    fclose(m);
}

void *sender_thread(void *arg) {
    (void)arg;
    char control_buf[2048];

    while (1) {
        printf("\nEnter filename to send (or 'exit'): ");
        char fname[512];
        if (scanf("%s", fname) != 1) continue;
        if (strncmp(fname, "exit", 4) == 0) {
            sendto(sockfd, "exit", 4, 0, (struct sockaddr *)&cliaddr, addrlen);
            log_event("Server operator requested exit.");
            break;
        }
        // open file and compute total_chunks
        FILE *fp = fopen(fname, "rb");
        if (!fp) {
            perror("fopen send");
            log_event("ERROR: cannot open '%s' for sending", fname);
            continue;
        }
        // compute file size -> total_chunks
        fseek(fp, 0, SEEK_END);
        long filesize = ftell(fp);
        fseek(fp, 0, SEEK_SET);
        long total_chunks = (filesize + CHUNK_SIZE - 1) / CHUNK_SIZE;

        // send control header: "FILE_START <orig_name> <total_chunks>"
        snprintf(control_buf, sizeof(control_buf), "%s %s %ld", FILE_START_MSG, fname, total_chunks);
        sendto(sockfd, control_buf, strlen(control_buf), 0, (struct sockaddr *)&cliaddr, addrlen);
        log_event("Sent FILE_START for '%s' total_chunks=%ld", fname, total_chunks);

        // resume point from sender meta (last contiguous acked)
        long base = read_sender_meta(fname); // number of chunks already acked
        long next_seq = base;                // next sequence number to send (absolute)
        // Prepare window slots
        send_slot_t window[WINDOW_SIZE];
        for (int i=0;i<WINDOW_SIZE;i++){ window[i].seq = -1; window[i].acked=0; window[i].sent=0; window[i].len=0; }

        // Seek file to base*CHUNK_SIZE
        fseek(fp, base * CHUNK_SIZE, SEEK_SET);
        log_event("Starting send of '%s' from chunk %ld (total %ld)", fname, base, total_chunks);

        fd_set rfds;
        struct timeval tv;

        // Fill initial window
        for (int i=0;i<WINDOW_SIZE;i++) {
            if (next_seq < total_chunks) {
                int bytes = fread(window[i].data, 1, CHUNK_SIZE, fp);
                if (bytes <= 0) { window[i].seq = -1; break; }
                window[i].seq = next_seq;
                window[i].len = bytes;
                window[i].acked = 0;
                window[i].sent = 0;
                next_seq++;
            } else {
                window[i].seq = -1;
            }
        }

        // Main send loop: continues until all chunks acked (base == total_chunks)
        long base_seq = base; // track base sequence (last contiguous acked = base-1)
        while (base_seq < total_chunks) {
            // send any unsent packets in window
            for (int i=0;i<WINDOW_SIZE;i++) {
                if (window[i].seq >= 0 && !window[i].acked) {
                    // if never sent or timed out -> send
                    int do_send = 0;
                    if (!window[i].sent) do_send = 1;
                    else {
                        struct timeval now; timeval_now(&now);
                        long diff = timeval_diff_usec(&now, &window[i].last_sent);
                        if (diff > TIMEOUT_USEC) do_send = 1;
                    }
                    if (do_send) {
                        // build packet: 9 byte header + payload
                        char pkt[HDR_LEN + CHUNK_SIZE];
                        uint32_t seq_net = htonl((uint32_t)window[i].seq);
                        uint32_t len_net = htonl((uint32_t)window[i].len);
                        memcpy(pkt, &seq_net, 4);
                        memcpy(pkt+4, &len_net, 4);
                        uint8_t flags = (window[i].seq == total_chunks-1) ? 1 : 0; // last chunk flag
                        memcpy(pkt+8, &flags, 1);
                        memcpy(pkt+HDR_LEN, window[i].data, window[i].len);
                        int sendlen = HDR_LEN + window[i].len;
                        sendto(sockfd, pkt, sendlen, 0, (struct sockaddr *)&cliaddr, addrlen);
                        timeval_now(&window[i].last_sent);
                        window[i].sent = 1;
                        log_event("SENT seq=%ld len=%d (slot=%d)", window[i].seq, window[i].len, i);
                        printf("[SERVER] Sent seq=%ld (slot=%d len=%d)\n", window[i].seq, i, window[i].len);
                    }
                }
            }

            // wait for incoming ACKs with timeout
            FD_ZERO(&rfds);
            FD_SET(sockfd, &rfds);
            tv.tv_sec = 0;
            tv.tv_usec = TIMEOUT_USEC / 4; // poll interval shorter than timeout
            int rv = select(sockfd+1, &rfds, NULL, NULL, &tv);
            if (rv > 0 && FD_ISSET(sockfd, &rfds)) {
                // read ack
                char ackbuf[64];
                int an = recvfrom(sockfd, ackbuf, sizeof(ackbuf)-1, 0, NULL, NULL);
                if (an <= 0) continue;
                ackbuf[an] = '\0';
                unsigned int ack_seq;
                if (sscanf(ackbuf, "ACK:%u", &ack_seq) == 1) {
                    log_event("RECV ACK:%u", ack_seq);
                    // mark ack in window if present
                    for (int i=0;i<WINDOW_SIZE;i++) {
                        if (window[i].seq == (long)ack_seq) {
                            window[i].acked = 1;
                            break;
                        }
                    }
                    // slide window as far as possible (advance base_seq)
                    int slid = 1;
                    while (slid) {
                        slid = 0;
                        if (window[0].seq >= 0 && window[0].acked) {
                            // this slot is done; shift window left
                            base_seq = window[0].seq + 1;
                            write_sender_meta(fname, base_seq); // persist base
                            // shift left
                            for (int k=0;k<WINDOW_SIZE-1;k++) window[k] = window[k+1];
                            window[WINDOW_SIZE-1].seq = -1;
                            window[WINDOW_SIZE-1].acked = 0;
                            window[WINDOW_SIZE-1].sent = 0;
                            window[WINDOW_SIZE-1].len = 0;
                            // refill last slot with next_seq if available
                            if (next_seq < total_chunks) {
                                int bytes = fread(window[WINDOW_SIZE-1].data, 1, CHUNK_SIZE, fp);
                                window[WINDOW_SIZE-1].seq = next_seq;
                                window[WINDOW_SIZE-1].len = bytes;
                                window[WINDOW_SIZE-1].acked = 0;
                                window[WINDOW_SIZE-1].sent = 0;
                                next_seq++;
                            }
                            slid = 1;
                        }
                    }
                }
            } else {
                // no data within poll interval, will loop and retransmit timed packets
            }
            // loop until base_seq == total_chunks (all acked)
        }

        // All chunks acked; send FILE_END to inform receiver
        sendto(sockfd, FILE_END_MSG, strlen(FILE_END_MSG), 0, (struct sockaddr *)&cliaddr, addrlen);
        log_event("Completed sending '%s' total_chunks=%ld", fname, total_chunks);
        printf("[SERVER] Completed sending '%s'\n", fname);
        fclose(fp);
    }
    return NULL;
}

// ---------- Main ----------
int main() {
    struct sockaddr_in servaddr;
    pthread_t thr_recv, thr_send;

    // open log
    log_fp = fopen("transfer_log.txt", "a");
    if (!log_fp) { perror("log open"); exit(1); }
    log_event("Server starting up on port %d", PORT);

    // create UDP socket
    sockfd = socket(AF_INET, SOCK_DGRAM, 0);
    if (sockfd < 0) { perror("socket"); exit(1); }

    bzero(&servaddr, sizeof(servaddr));
    servaddr.sin_family = AF_INET;
    servaddr.sin_addr.s_addr = INADDR_ANY;
    servaddr.sin_port = htons(PORT);

    if (bind(sockfd, (const struct sockaddr *)&servaddr, sizeof(servaddr)) < 0) { perror("bind"); exit(1); }

    printf("Server listening on UDP port %d...\n", PORT);
    log_event("Server bound to port %d", PORT);

    // wait for client hello to capture client address
    char hello[256];
    int n = recvfrom(sockfd, hello, sizeof(hello)-1, 0, (struct sockaddr *)&cliaddr, &addrlen);
    if (n > 0) {
        hello[n] = '\0';
        printf("Client says: %s\n", hello);
        log_event("Client connected: %s", inet_ntoa(cliaddr.sin_addr));
    }

    pthread_create(&thr_recv, NULL, receiver_thread, NULL);
    pthread_create(&thr_send, NULL, sender_thread, NULL);

    pthread_join(thr_send, NULL);
    // optionally, could join receiver too; in this simple server we exit after send thread ends
    // pthread_cancel(thr_recv);
    // pthread_join(thr_recv, NULL);

    log_event("Server shutting down");
    fclose(log_fp);
    close(sockfd);
    return 0;
}

