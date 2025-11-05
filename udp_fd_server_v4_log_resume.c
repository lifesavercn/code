// --------------------------------------------------------------
// Full-Duplex UDP File Transfer Server (Version 4 - Logging + Resume)
// Features:
//   ✅ Full-duplex using threads
//   ✅ Logging of all events (transfer_log.txt)
//   ✅ Resume on restart (reads .meta files)
// --------------------------------------------------------------

#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <unistd.h>
#include <arpa/inet.h>
#include <pthread.h>
#include <sys/stat.h>
#include <time.h>

#define MAX 1024
#define PORT 8210
#define FILE_START "FILE_START"
#define FILE_END   "FILE_END"

struct sockaddr_in cliaddr;
socklen_t len = sizeof(cliaddr);
int sockfd;

FILE *log_fp;

// ---- Utility: log events with timestamp ----
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

// ---- Utility: read last chunk index from .meta file ----
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

// ---- Utility: write progress to .meta ----
void save_progress(const char *filename, long chunk_num) {
    char metafile[300];
    snprintf(metafile, sizeof(metafile), "%s.meta", filename);
    FILE *meta = fopen(metafile, "w");
    if (!meta) return;
    fprintf(meta, "%ld", chunk_num);
    fclose(meta);
}

void *receive_data(void *args) {
    char buff[MAX];
    FILE *fp = NULL;
    int receiving_file = 0;
    char filename[256], new_filename[300];
    long chunk_count = 0;

    while (1) {
        bzero(buff, MAX);
        int n = recvfrom(sockfd, buff, sizeof(buff), 0, (struct sockaddr *)&cliaddr, &len);
        if (n <= 0) continue;
        buff[n] = '\0';

        // ---- New file transfer starting ----
        if (strncmp(buff, FILE_START, strlen(FILE_START)) == 0) {
            sscanf(buff + strlen(FILE_START), "%s", filename);
            snprintf(new_filename, sizeof(new_filename), "received_%s", filename);

            // check for resume
            chunk_count = get_resume_point(new_filename);
            fp = fopen(new_filename, chunk_count ? "ab" : "wb");
            if (!fp) {
                perror("File open error");
                continue;
            }
            receiving_file = 1;
            log_event("Receiving file '%s' (resume from chunk %ld)", filename, chunk_count);
            printf("[SERVER] Receiving file: %s (resume from %ld)\n", filename, chunk_count);
            continue;
        }

        // ---- End of file ----
        if (strncmp(buff, FILE_END, strlen(FILE_END)) == 0) {
            if (fp) fclose(fp);
            receiving_file = 0;
            log_event("File received successfully (%ld chunks)", chunk_count);
            printf("\n[SERVER] File received successfully (%ld chunks)\n", chunk_count);
            continue;
        }

        // ---- Actual file data ----
        if (receiving_file) {
            fwrite(buff, 1, n, fp);
            chunk_count++;
            save_progress(new_filename, chunk_count);
            printf("[SERVER] Receiving chunk #%ld\r", chunk_count);
            fflush(stdout);
        } else {
            printf("From Client: %s", buff);
            fflush(stdout);

            if (strncmp("exit", buff, 4) == 0) {
                printf("Client disconnected...\n");
                log_event("Client disconnected.");
                break;
            }
        }
    }
    return NULL;
}

void *send_file(void *args) {
    char filename[256];
    char buff[MAX];

    while (1) {
        printf("\nEnter filename to send (or 'exit'): ");
        scanf("%s", filename);

        if (strncmp(filename, "exit", 4) == 0) {
            sendto(sockfd, "exit", 4, 0, (struct sockaddr *)&cliaddr, len);
            log_event("Server exit command sent.");
            break;
        }

        FILE *fp = fopen(filename, "rb");
        if (!fp) {
            perror("File open error");
            log_event("Failed to open '%s'", filename);
            continue;
        }

        long resume_chunk = get_resume_point(filename);
        fseek(fp, resume_chunk * MAX, SEEK_SET);
        log_event("Sending '%s' (resume from chunk %ld)", filename, resume_chunk);

        char header[512];
        snprintf(header, sizeof(header), "%s %s", FILE_START, filename);
        sendto(sockfd, header, strlen(header), 0, (struct sockaddr *)&cliaddr, len);
        usleep(100000);

        long chunk_count = resume_chunk;
        while (!feof(fp)) {
            int bytes_read = fread(buff, 1, MAX, fp);
            if (bytes_read > 0) {
                sendto(sockfd, buff, bytes_read, 0, (struct sockaddr *)&cliaddr, len);
                chunk_count++;
                save_progress(filename, chunk_count);
                printf("[SERVER] Sent chunk #%ld\r", chunk_count);
                fflush(stdout);
                usleep(1000);
            }
        }
        fclose(fp);
        sendto(sockfd, FILE_END, strlen(FILE_END), 0, (struct sockaddr *)&cliaddr, len);
        log_event("File '%s' sent successfully (%ld chunks)", filename, chunk_count);
        printf("\n[SERVER] File '%s' sent successfully (%ld chunks)\n", filename, chunk_count);
    }
    return NULL;
}

int main() {
    struct sockaddr_in servaddr;
    pthread_t recv_thread, send_thread;

    sockfd = socket(AF_INET, SOCK_DGRAM, 0);
    if (sockfd < 0) { perror("Socket creation failed"); exit(1); }
    printf("UDP Socket created successfully.\n");

    bzero(&servaddr, sizeof(servaddr));
    servaddr.sin_family = AF_INET;
    servaddr.sin_addr.s_addr = INADDR_ANY;
    servaddr.sin_port = htons(PORT);

    if (bind(sockfd, (const struct sockaddr *)&servaddr, sizeof(servaddr)) < 0) {
        perror("Bind failed"); exit(1);
    }
    printf("Server listening on UDP port %d...\n", PORT);

    log_fp = fopen("transfer_log.txt", "a");
    log_event("Server started on port %d", PORT);

    char hello[MAX];
    int n = recvfrom(sockfd, hello, sizeof(hello), 0, (struct sockaddr *)&cliaddr, &len);
    hello[n] = '\0';
    printf("Client connected: %s\n", inet_ntoa(cliaddr.sin_addr));
    log_event("Client connected: %s", inet_ntoa(cliaddr.sin_addr));

    pthread_create(&recv_thread, NULL, receive_data, NULL);
    pthread_create(&send_thread, NULL, send_file, NULL);

    pthread_join(send_thread, NULL);
    pthread_join(recv_thread, NULL);

    fclose(log_fp);
    close(sockfd);
    return 0;
}

