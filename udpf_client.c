#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <unistd.h>
#include <arpa/inet.h>
#include <dirent.h>
#include <sys/time.h>
#include <sys/stat.h>

#define SERVER_IP   "127.0.0.1"
#define SERVER_PORT 7600
#define MAX_DATA    1024
#define TIMEOUT_SEC 2             // Timeout for ACK reception (seconds)

// Packet and ACK structures (same as server)
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

// Function to send one file reliably (with retransmission)
void send_file(int sock, struct sockaddr_in server_addr, socklen_t addr_len, const char *filepath, const char *filename) {
    struct Packet packet;
    struct Ack ack;
    FILE *fp = fopen(filepath, "rb");
    if (!fp) {
        perror("File open");
        return;
    }

    int seq_num = 0;
    int bytes_read;

    // Loop through file chunks
    while (1) {
        memset(&packet, 0, sizeof(packet));
        packet.seq_num = seq_num;
        strcpy(packet.filename, filename);

        // Read a chunk from file
        bytes_read = fread(packet.data, 1, MAX_DATA, fp);
        packet.size = bytes_read;

        if (bytes_read <= 0) break;  // End of file

        int acked = 0;
        while (!acked) {
            // Send packet
            sendto(sock, &packet, sizeof(packet), 0, (struct sockaddr *)&server_addr, addr_len);
            printf("Sent packet %d (%d bytes) of file %s\n", seq_num, packet.size, filename);

            // Wait for ACK
            int n = recvfrom(sock, &ack, sizeof(ack), 0, (struct sockaddr *)&server_addr, &addr_len);

            // Check ACK validity
            if (n > 0 && ack.seq_num == seq_num) {
                printf("Received ACK %d for file %s\n", ack.seq_num, filename);
                acked = 1;
            } else {
                printf("Timeout! Resending packet %d of file %s\n", seq_num, filename);
            }
        }
        seq_num++;  // Move to next packet
    }

    // Send special packet indicating end of this file
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

    // 1️⃣ Create UDP socket
    if ((sock = socket(AF_INET, SOCK_DGRAM, 0)) == -1) {
        perror("Socket");
        exit(1);
    }

    // 2️⃣ Set socket receive timeout for retransmission handling
    struct timeval tv;
    tv.tv_sec = TIMEOUT_SEC;
    tv.tv_usec = 0;
    setsockopt(sock, SOL_SOCKET, SO_RCVTIMEO, &tv, sizeof(tv));

    // 3️⃣ Define server details
    server_addr.sin_family = AF_INET;
    server_addr.sin_port = htons(SERVER_PORT);
    server_addr.sin_addr.s_addr = inet_addr(SERVER_IP);
    memset(&(server_addr.sin_zero), 0, 8);

    // 4️⃣ Ask user for folder name
    char foldername[100];
    printf("Enter folder path to send: ");
    scanf("%s", foldername);

    // 5️⃣ Open folder
    DIR *dir = opendir(foldername);
    if (!dir) {
        perror("Open directory");
        exit(1);
    }

    // 6️⃣ For each regular file, call send_file()
    struct dirent *entry;
    while ((entry = readdir(dir)) != NULL) {
        if (entry->d_type == DT_REG) {  // Only normal files
            char filepath[200];
            snprintf(filepath, sizeof(filepath), "%s/%s", foldername, entry->d_name);
            send_file(sock, server_addr, addr_len, filepath, entry->d_name);
        }
    }
    closedir(dir);

    // 7️⃣ Send END-OF-FOLDER control packet
    struct Packet packet;
    packet.seq_num = -999;
    packet.size = 0;
    strcpy(packet.filename, "END_FOLDER");
    sendto(sock, &packet, sizeof(packet), 0, (struct sockaddr *)&server_addr, addr_len);
    printf("[+] End of folder transfer.\n");

    close(sock);
    return 0;
}


