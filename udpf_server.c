#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <unistd.h>
#include <arpa/inet.h>
#include <sys/stat.h>

#define SERVER_IP "127.0.0.1"     // Server IP address (localhost)
#define SERVER_PORT 7600          // Port number for UDP communication
#define MAX_DATA 1024             // Maximum data per packet

// Structure representing a data packet from the client
struct Packet {
    int seq_num;                  // Sequence number of the packet
    int size;                     // Actual size of 'data' in bytes
    char filename[100];           // Name of the file being transferred
    char data[MAX_DATA];          // Chunk of file data
};

// Structure for ACK packet sent from server to client
struct Ack {
    int seq_num;                  // Acknowledged sequence number
    char filename[100];           // Filename (for matching ACK)
};

int main() {
    int sock;
    struct sockaddr_in server_addr, client_addr;
    socklen_t addr_len = sizeof(client_addr);

    struct Packet packet;
    struct Ack ack;
    FILE *fp = NULL;
    int expected_seq = 0;          // Keeps track of expected packet sequence
    char current_file[100] = {0};

    // 1️⃣ Create UDP socket
    if ((sock = socket(AF_INET, SOCK_DGRAM, 0)) == -1) {
        perror("Socket");
        exit(1);
    }

    // 2️⃣ Define server address
    server_addr.sin_family = AF_INET;
    server_addr.sin_port = htons(SERVER_PORT);
    server_addr.sin_addr.s_addr = inet_addr(SERVER_IP);
    memset(&(server_addr.sin_zero), 0, 8);

    // 3️⃣ Bind the socket to IP and port
    if (bind(sock, (struct sockaddr *)&server_addr, sizeof(struct sockaddr)) == -1) {
        perror("Bind");
        exit(1);
    }

    printf("UDP Folder Server listening on %s:%d...\n", SERVER_IP, SERVER_PORT);

    // 4️⃣ Create folder where received files will be saved
    mkdir("received_folder", 0777);

    // 5️⃣ Infinite loop to receive packets
    while (1) {
        int bytes = recvfrom(sock, &packet, sizeof(packet), 0,
                             (struct sockaddr *)&client_addr, &addr_len);
        if (bytes <= 0) continue;

        // 📁 End of entire folder
        if (packet.seq_num == -999) {
            printf("\n[+] Folder transfer complete.\n");
            if (fp) fclose(fp);
            break;
        }

        // 📄 End of current file
        if (packet.seq_num == -1) {
            printf("\n[+] File %s transfer complete.\n", current_file);
            if (fp) fclose(fp);
            fp = NULL;
            expected_seq = 0;
            continue;
        }

        // 🆕 If a new file begins, create a new output file
        if (fp == NULL) {
            char new_filepath[200];
            snprintf(new_filepath, sizeof(new_filepath), "received_folder/%s", packet.filename);
            fp = fopen(new_filepath, "wb");
            if (!fp) {
                perror("File open");
                exit(1);
            }
            strcpy(current_file, packet.filename);
            printf("[+] Receiving file: %s\n", new_filepath);
        }

        // ✅ Correct packet received in order
        if (packet.seq_num == expected_seq) {
            fwrite(packet.data, 1, packet.size, fp);  // Write to file
            printf("Received packet %d (%d bytes) of %s\n", packet.seq_num, packet.size, packet.filename);

            // Send acknowledgment back to client
            ack.seq_num = expected_seq;
            strcpy(ack.filename, packet.filename);
            sendto(sock, &ack, sizeof(ack), 0, (struct sockaddr *)&client_addr, addr_len);

            expected_seq++;  // Expect next packet
        } 
        // 🚫 Duplicate or out-of-order packet
        else {
            printf("Duplicate packet %d ignored\n", packet.seq_num);
            ack.seq_num = expected_seq - 1;  // Resend last ACK
            strcpy(ack.filename, packet.filename);
            sendto(sock, &ack, sizeof(ack), 0, (struct sockaddr *)&client_addr, addr_len);
        }
    }

    close(sock);
    return 0;
}

