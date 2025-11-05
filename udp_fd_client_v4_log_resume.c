// --------------------------------------------------------------
// Full-Duplex UDP File Transfer Client (Version 4 - Logging + Resume)
// Features:
//   ✅ Logging of all transfers to 'transfer_log.txt'
//   ✅ Resume interrupted file transfers using .meta files
//   ✅ Full-duplex parallel threads (send + receive)
// --------------------------------------------------------------

#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <unistd.h>
#include <arpa/inet.h>
#include <pthread.h>
#include <sys/stat.h>
#include <time.h>
#include <stdarg.h>

#define MAX 1024
#define PORT 8210
#define FILE_START "FILE_START"
#define FILE_END   "FILE_END"

int sockfd;
struct sockaddr_in servaddr;
socklen_t len = sizeof(servaddr);

FILE *log_fp; // Log file pointer

// --------------------------------------------------------------
// Function: log_event
// Purpose:  Write messages with timestamps into transfer_log.txt
// --------------------------------------------------------------
void log_event(const char *fmt, ...) {
    va_list args;
    va_start(args, fmt);
    time_t now = time(NULL);
    char *ts = strtok(ctime(&now), "\n"); // remove newline
    fprintf(log_fp, "[%s] ", ts);
    vfprintf(log_fp, fmt, args);
    fprintf(log_fp, "\n");
    fflush(log_fp);
    va_end(args);
}

// --------------------------------------------------------------
// Function: get_resume_point
// Purpose:  Read last saved chunk number from .meta file
// --------------------------------------------------------------
long get_resume_point(const char *filename) {
    char metafile[300];
    snprintf(metafile, sizeof(metafile), "%s.meta", filename);
    FILE *meta = fopen(metafile, "r");
    if (!meta) return 0;
    long chunk;
    fscanf(meta, "%ld", &chunk);
    fclose(meta);
    return chunk;
}

// --------------------------------------------------------------
// Function: save_progress
// Purpose:  Write the number of chunks processed so far to .meta
// --------------------------------------------------------------
void save_progress(const char *filename, long chunk_num) {
    char metafile[300];
    snprintf(metafile, sizeof(metafile), "%s.meta", filename);
    FILE *meta = fopen(metafile, "w");
    if (!meta) return;
    fprintf(meta, "%ld", chunk_num);
    fclose(meta);
}

// --------------------------------------------------------------
// Thread: receive_data
// Purpose: Receive files from server (handles resume + logging)
// --------------------------------------------------------------
void *receive_data(void *args) {
    char buff[MAX];
    FILE *fp = NULL;
    int receiving_file = 0;
    char filename[256];
    long chunk_count = 0;

    while (1) {
        bzero(buff, MAX);
        int n = recvfrom(sockfd, buff, sizeof(buff), 0, NULL, NULL);
        if (n <= 0)
            continue;

        buff[n] = '\0';

        // ---- When a file transfer starts ----
        if (strncmp(buff, FILE_START, strlen(FILE_START)) == 0) {
            sscanf(buff + strlen(FILE_START), "%s", filename);
            long resume_chunk = get_resume_point(filename);
            fp = fopen(filename, resume_chunk ? "ab" : "wb"); // append if resuming
            if (!fp) {
                perror("File open error");
                continue;
            }
            chunk_count = resume_chunk;
            receiving_file = 1;

            log_event("Receiving file '%s' (resume from chunk %ld)", filename, resume_chunk);
            printf("\n[CLIENT] Receiving file: %s (resume from %ld)\n", filename, resume_chunk);
            continue;
        }

        // ---- When file transfer ends ----
        if (strncmp(buff, FILE_END, strlen(FILE_END)) == 0) {
            if (fp) fclose(fp);
            receiving_file = 0;
            log_event("File '%s' received successfully (%ld chunks)", filename, chunk_count);
            printf("\n[CLIENT] File '%s' received successfully (%ld chunks)\n", filename, chunk_count);
            continue;
        }

        // ---- File data ----
        if (receiving_file) {
            fwrite(buff, 1, n, fp);
            chunk_count++;
            save_progress(filename, chunk_count);
            printf("[CLIENT] Receiving chunk #%ld\r", chunk_count);
            fflush(stdout);
        } else {
            // ---- Regular message ----
            printf("From Server: %s", buff);
            fflush(stdout);

            if (strncmp("exit", buff, 4) == 0) {
                printf("\nServer disconnected...\n");
                log_event("Server disconnected.");
                break;
            }
        }
    }
    return NULL;
}

// --------------------------------------------------------------
// Thread: send_file
// Purpose: Send files to server (supports resume + logging)
// --------------------------------------------------------------
void *send_file(void *args) {
    char filename[256];
    char buff[MAX];

    while (1) {
        printf("\nEnter filename to send (or 'exit' to quit): ");
        scanf("%s", filename);

        if (strncmp(filename, "exit", 4) == 0) {
            sendto(sockfd, "exit", 4, 0, (const struct sockaddr *)&servaddr, len);
            log_event("Client exited.");
            break;
        }

        FILE *fp = fopen(filename, "rb");
        if (!fp) {
            perror("File open error");
            log_event("Failed to open '%s' for sending.", filename);
            continue;
        }

        // ---- Check if resuming ----
        long resume_chunk = get_resume_point(filename);
        fseek(fp, resume_chunk * MAX, SEEK_SET);
        log_event("Sending file '%s' (resume from chunk %ld)", filename, resume_chunk);

        // ---- Notify server of file start ----
        char header[512];
        snprintf(header, sizeof(header), "%s %s", FILE_START, filename);
        sendto(sockfd, header, strlen(header), 0, (const struct sockaddr *)&servaddr, len);
        usleep(100000); // slight delay before sending chunks

        long chunk_count = resume_chunk;
        while (!feof(fp)) {
            int bytes_read = fread(buff, 1, MAX, fp);
            if (bytes_read > 0) {
                sendto(sockfd, buff, bytes_read, 0, (const struct sockaddr *)&servaddr, len);
                chunk_count++;
                save_progress(filename, chunk_count);
                printf("[CLIENT] Sent chunk #%ld\r", chunk_count);
                fflush(stdout);
                usleep(1000);
            }
        }

        fclose(fp);

        // ---- Mark file end ----
        sendto(sockfd, FILE_END, strlen(FILE_END), 0, (const struct sockaddr *)&servaddr, len);
        printf("\n[CLIENT] File '%s' sent successfully (%ld chunks)\n", filename, chunk_count);
        log_event("File '%s' sent successfully (%ld chunks)", filename, chunk_count);
    }
    return NULL;
}

// --------------------------------------------------------------
// main()
// Purpose: Initialize socket, start threads, handle cleanup
// --------------------------------------------------------------
int main() {
    pthread_t recv_thread, send_thread;

    // ---- Create UDP socket ----
    sockfd = socket(AF_INET, SOCK_DGRAM, 0);
    if (sockfd < 0) {
        perror("Socket creation failed");
        exit(1);
    }
    printf("UDP Socket created successfully.\n");

    // ---- Configure server address ----
    bzero(&servaddr, sizeof(servaddr));
    servaddr.sin_family = AF_INET;
    servaddr.sin_port = htons(PORT);
    servaddr.sin_addr.s_addr = inet_addr("127.0.0.1"); // local testing

    // ---- Send initial hello to server ----
    char hello[] = "Hello from client\n";
    sendto(sockfd, hello, strlen(hello), 0, (const struct sockaddr *)&servaddr, len);
    printf("Connected to server (UDP)...\n");

    // ---- Open log file ----
    log_fp = fopen("transfer_log.txt", "a");
    if (!log_fp) {
        perror("Unable to open log file");
        exit(1);
    }
    log_event("Client started and connected to server.");

    // ---- Launch threads ----
    pthread_create(&recv_thread, NULL, receive_data, NULL);
    pthread_create(&send_thread, NULL, send_file, NULL);

    // ---- Wait for both threads ----
    pthread_join(send_thread, NULL);
    pthread_join(recv_thread, NULL);

    fclose(log_fp);
    close(sockfd);
    return 0;
}

