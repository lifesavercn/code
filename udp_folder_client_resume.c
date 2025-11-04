#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <unistd.h>
#include <arpa/inet.h>
#include <dirent.h>
#include <sys/time.h>
#include <sys/stat.h>

#define SERVER_IP   "127.0.0.1"
#define SERVER_PORT 7610
#define MAX_DATA    1024
#define TIMEOUT_SEC 2

struct Packet {
    int seq_num;
    int size;
    char filename[100];
    char data[MAX_DATA];
};

struct Ack {
    int seq_num;
    char filename[100];
};

// Read last acknowledged sequence number for a file from log
int get_last_ack(const char *filename) {
    FILE *log = fopen("transfer_log.txt", "r");
    if (!log) return -1;
    char name[100];
    int seq;
    while (fscanf(log, "%s %d", name, &seq) == 2) {
        if (strcmp(name, filename) == 0) {
            fclose(log);
            return seq;
        }
    }
    fclose(log);
    return -1;
}

// Update log file after each ACK
void update_log(const char *filename, int seq_num) {
    FILE *temp = fopen("temp_log.txt", "w");
    FILE *log = fopen("transfer_log.txt", "r");
    int updated = 0;
    char name[100];
    int seq;

    if (log) {
        while (fscanf(log, "%s %d", name, &seq) == 2) {
            if (strcmp(name, filename) == 0) {
                fprintf(temp, "%s %d\n", filename, seq_num);
                updated = 1;
            } else {
                fprintf(temp, "%s %d\n", name, seq);
            }
        }
        fclose(log);
    }

    if (!updated)
        fprintf(temp, "%s %d\n", filename, seq_num);

    fclose(temp);
    rename("temp_log.txt", "transfer_log.txt");
}

// Send one file with resume support
void send_file(int sock, struct sockaddr_in server_addr, socklen_t addr_len, const char *filepath, const char *filename) {
    struct Packet packet;
    struct Ack ack;
    FILE *fp = fopen(filepath, "rb");
    if (!fp) {
        perror("File open");
        return;
    }

    int last_ack = get_last_ack(filename);
    int seq_num = 0;
    printf("[+] Resuming file %s from packet %d\n", filename, last_ack + 1);

    // Skip already sent bytes
    if (last_ack >= 0)
        fseek(fp, (last_ack + 1) * MAX_DATA, SEEK_SET);
    else
        last_ack = -1;

    while (1) {
        memset(&packet, 0, sizeof(packet));
        packet.seq_num = ++last_ack;
        strcpy(packet.filename, filename);
        packet.size = fread(packet.data, 1, MAX_DATA, fp);
        if (packet.size <= 0) break;

        int acked = 0;
        while (!acked) {
            sendto(sock, &packet, sizeof(packet), 0, (struct sockaddr *)&server_addr, addr_len);
            printf("Sent packet %d (%d bytes) of %s\n", packet.seq_num, packet.size, filename);

            int n = recvfrom(sock, &ack, sizeof(ack), 0, (struct sockaddr *)&server_addr, &addr_len);
            if (n > 0 && ack.seq_num == packet.seq_num) {
                printf("ACK %d received for %s\n", ack.seq_num, filename);
                update_log(filename, ack.seq_num);
                acked = 1;
            } else {
                printf("Timeout — resending packet %d of %s\n", packet.seq_num, filename);
            }
        }
    }

    // EOF signal
    packet.seq_num = -1;
    packet.size = 0;
    strcpy(packet.filename, filename);
    sendto(sock, &packet, sizeof(packet), 0, (struct sockaddr *)&server_addr, addr_len);
    printf("[+] EOF for file %s sent.\n", filename);

    fclose(fp);
}

int main() {
    int sock;
    struct sockaddr_in server_addr;
    socklen_t addr_len = sizeof(server_addr);

    if ((sock = socket(AF_INET, SOCK_DGRAM, 0)) == -1) {
        perror("Socket");
        exit(1);
    }

    struct timeval tv = {TIMEOUT_SEC, 0};
    setsockopt(sock, SOL_SOCKET, SO_RCVTIMEO, &tv, sizeof(tv));

    server_addr.sin_family = AF_INET;
    server_addr.sin_port = htons(SERVER_PORT);
    server_addr.sin_addr.s_addr = inet_addr(SERVER_IP);
    memset(&(server_addr.sin_zero), 0, 8);

    char foldername[100];
    printf("Enter folder path to send: ");
    scanf("%s", foldername);

    DIR *dir = opendir(foldername);
    if (!dir) {
        perror("Open directory");
        exit(1);
    }

    struct dirent *entry;
    while ((entry = readdir(dir)) != NULL) {
        if (entry->d_type == DT_REG) {
            char filepath[200];
            snprintf(filepath, sizeof(filepath), "%s/%s", foldername, entry->d_name);
            send_file(sock, server_addr, addr_len, filepath, entry->d_name);
        }
    }
    closedir(dir);

    struct Packet packet;
    packet.seq_num = -999;
    packet.size = 0;
    strcpy(packet.filename, "END_FOLDER");
    sendto(sock, &packet, sizeof(packet), 0, (struct sockaddr *)&server_addr, addr_len);
    printf("[+] End of folder transfer.\n");

    close(sock);
    return 0;
}

