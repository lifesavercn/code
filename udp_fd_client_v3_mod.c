// --------------------------------------------------------------
// Full-duplex UDP File Transfer Client (Version 3 Modified)
// Implements Stop-and-Wait ARQ and saves incoming files as received_<filename>
// --------------------------------------------------------------

#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <unistd.h>
#include <arpa/inet.h>
#include <pthread.h>
#include <sys/time.h>
#include <errno.h>

#define MAX 1024
#define PORT 6200
#define FILE_START "FILE_START"
#define FILE_END "FILE_END"
#define ACK_PREFIX "ACK:"

int sockfd;
struct sockaddr_in servaddr;
socklen_t len = sizeof(servaddr);

#define TIMEOUT_USEC 500000 // 0.5s timeout

// ---------------------- Receiver Thread ----------------------
void *receive_data(void *args)
{
    char buff[MAX + 50];// Buffer for receiving data
    FILE *fp = NULL;// File pointer for writing received data
    int receiving_file = 0;// Flag to track if currently receiving a file
    int expected_seq = 0;// Expected sequence number (0 or 1)
    char filename[256], recv_filename[300]; // New filename for saving received file

    while (1)
    {
        bzero(buff, sizeof(buff));// New filename for saving received file
        int n = recvfrom(sockfd, buff, sizeof(buff), 0, NULL, NULL);
        if (n <= 0)
            continue;
        buff[n] = '\0';// Null-terminate

        // Start of file
        if (strncmp(buff, FILE_START, strlen(FILE_START)) == 0)
        {
            sscanf(buff + strlen(FILE_START), "%s", filename);// Extract filename from message
            snprintf(recv_filename, sizeof(recv_filename), "received_%s", filename);// Create a new name for saving received file
            fp = fopen(recv_filename, "wb");// Open file for writing (binary mode)
            if (!fp)
            {
                perror("File open");
                continue;
            }
            receiving_file = 1;// Enter file-receive mode
            expected_seq = 0;// Expect first sequence = 0
            printf("\n[CLIENT] Receiving file: %s -> saved as %s\n", filename, recv_filename);
            continue;
        }

        // End of file
        if (strncmp(buff, FILE_END, strlen(FILE_END)) == 0)
        {
            if (fp)
            {
                fclose(fp);
                fp = NULL;
            }
            receiving_file = 0;
            printf("[CLIENT] File received successfully!\n");
            continue;
        }

        // Data packet
        if (receiving_file)
        {
            int seq;// Data is formatted as: "SEQ:<num>|<data>"
            char *data_ptr = strstr(buff, "|");
            if (!data_ptr)
                continue;

            sscanf(buff, "SEQ:%d", &seq);// Extract sequence number
            data_ptr++;// Move pointer to start of data

            if (seq == expected_seq)// If sequence number matches expected, write data
            {
                fwrite(data_ptr, 1, strlen(data_ptr), fp);
                expected_seq = 1 - expected_seq;// Flip expected sequence
            }

            char ack_msg[16]; // Send acknowledgment back to server
            snprintf(ack_msg, sizeof(ack_msg), "ACK:%d", seq);
            sendto(sockfd, ack_msg, strlen(ack_msg), 0, (const struct sockaddr *)&servaddr, len);
        }
        else
        {
            // Handle regular server messages (not file data)
            printf("[CLIENT] From Server: %s\n", buff);
            if (strncmp("exit", buff, 4) == 0)
            {
                printf("[CLIENT] Server disconnected...\n");
                break;
            }
        }
    }
    return NULL;
}

// ---------------------- Sender Thread ----------------------
void *send_file(void *args)
{
    char filename[256];
    char buff[MAX];
    char packet[MAX + 50];
    fd_set readfds;// For select()
    struct timeval tv;// Timeout structure

    while (1)
    {
        printf("\nEnter filename to send (or 'exit' to quit): ");
        scanf("%s", filename);

        if (strncmp(filename, "exit", 4) == 0)
        {
            sendto(sockfd, "exit", 4, 0, (const struct sockaddr *)&servaddr, len);
            printf("Client exit...\n");
            break;
        }

        FILE *fp = fopen(filename, "rb");
        if (!fp)
        {
            perror("File open error");
            continue;
        }
        
        // Notify server about file start
        char header[512];
        snprintf(header, sizeof(header), "%s %s", FILE_START, filename);
        sendto(sockfd, header, strlen(header), 0, (const struct sockaddr *)&servaddr, len);
        usleep(100000);// Small delay to ensure server readiness

        int seq = 0;// Initial sequence number
        int chunk_no = 0;// Track number of chunks sent

        while (!feof(fp))
        {
            int bytes = fread(buff, 1, MAX, fp);//read next chunk
            if (bytes <= 0)
                break;
             // Create packet header: "SEQ:<num>|"
            snprintf(packet, sizeof(packet), "SEQ:%d|", seq);
            // Append raw file data to packet
            memcpy(packet + strlen(packet), buff, bytes);

            int ack_received = 0;// Acknowledgment flag
            while (!ack_received)
            {
                sendto(sockfd, packet, strlen(packet), 0, (const struct sockaddr *)&servaddr, len);
                printf("[CLIENT] Sent chunk #%d (seq %d)\n", ++chunk_no, seq);
		// Wait for ACK with timeout
                FD_ZERO(&readfds);
                FD_SET(sockfd, &readfds);
                tv.tv_sec = 0;
                tv.tv_usec = TIMEOUT_USEC;

                int rv = select(sockfd + 1, &readfds, NULL, NULL, &tv);
                if (rv > 0)
                {
                    // ACK received within timeout
                    char ack[32];
                    int n = recvfrom(sockfd, ack, sizeof(ack), 0, NULL, NULL);
                    ack[n] = '\0';
                    int ack_seq;
                    
                    // Validate ACK
                    if (sscanf(ack, "ACK:%d", &ack_seq) == 1 && ack_seq == seq)
                    {
                        ack_received = 1;// ACK matches
                        seq = 1 - seq;// Flip sequence for next packet
                    }
                }
                else
                    printf("[CLIENT] Timeout → retransmitting seq %d...\n", seq);// Timeout: resend the same packet
            }
        }

        fclose(fp);
        sendto(sockfd, FILE_END, strlen(FILE_END), 0, (const struct sockaddr *)&servaddr, len);
        printf("[CLIENT] File '%s' sent successfully!\n", filename);
    }
    return NULL;
}

// ---------------------- Main ----------------------
int main()
{
    pthread_t recv_thread, send_thread;

    sockfd = socket(AF_INET, SOCK_DGRAM, 0);
    if (sockfd < 0)
    {
        perror("Socket creation failed");
        exit(1);
    }

    printf("UDP Socket created successfully.\n");

    bzero(&servaddr, sizeof(servaddr));
    servaddr.sin_family = AF_INET;
    servaddr.sin_port = htons(PORT);
    servaddr.sin_addr.s_addr = inet_addr("127.0.0.1");

    char hello[] = "Hello from client\n";
    sendto(sockfd, hello, strlen(hello), 0, (const struct sockaddr *)&servaddr, len);
    printf("Connected to server (UDP)...\n");

    pthread_create(&recv_thread, NULL, receive_data, NULL);
    pthread_create(&send_thread, NULL, send_file, NULL);

    pthread_join(send_thread, NULL);
    pthread_join(recv_thread, NULL);

    close(sockfd);
    return 0;
}

