// --------------------------------------------------------------
// Full-duplex UDP File Transfer Server (Version 3 Modified)
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
#define PORT 8970
#define FILE_START "FILE_START"
#define FILE_END "FILE_END"
#define ACK_PREFIX "ACK:"

struct sockaddr_in cliaddr;
socklen_t len = sizeof(cliaddr);
int sockfd;

#define TIMEOUT_USEC 500000 // 0.5s timeout

// ---------------------- Receiver Thread ----------------------
void *receive_data(void *args)
{
    char buff[MAX + 50];// Buffer to store incoming data
    FILE *fp = NULL;// File pointer for writing received data
    int receiving_file = 0;// Flag to indicate file receive mode
    int expected_seq = 0;// Sequence number expected next (0/1)
    char filename[256], recv_filename[300]; // Destination filename: received_<filename>

    while (1)
    {
        bzero(buff, sizeof(buff));// Clear buffer before receiving new data
        // Receive UDP packet from client
        int n = recvfrom(sockfd, buff, sizeof(buff), 0, (struct sockaddr *)&cliaddr, &len);
        if (n <= 0)
            continue;
        buff[n] = '\0';// Null-terminate string data

        // Start of file
        if (strncmp(buff, FILE_START, strlen(FILE_START)) == 0)
        {
            sscanf(buff + strlen(FILE_START), "%s", filename);// Extract original filename sent by client
            snprintf(recv_filename, sizeof(recv_filename), "received_%s", filename);// Construct output filename with prefix
            fp = fopen(recv_filename, "wb");// Open file for writing (binary mode)
            if (!fp)
            {
                perror("File open");
                continue;
            }
            receiving_file = 1;// Enter file receive mode
            expected_seq = 0;// Expect first sequence number = 0
            printf("\n[SERVER] Receiving file: %s -> saved as %s\n", filename, recv_filename);
            continue;
        }

        // End of file
        if (strncmp(buff, FILE_END, strlen(FILE_END)) == 0)
        {
            if (fp)
            {
                fclose(fp);// Close file
                fp = NULL;
            }
            receiving_file = 0;// Exit file receive mode
            printf("[SERVER] File received successfully!\n");
            continue;
        }

        // Data packet: SEQ:<n>|<data>
        if (receiving_file)
        {
            int seq;
            char *data_ptr = strstr(buff, "|");// Locate '|' separator
            if (!data_ptr)
                continue;

            sscanf(buff, "SEQ:%d", &seq);// Extract sequence number
            data_ptr++;

            // Write data only if it's the expected sequence
            if (seq == expected_seq)
            {
                fwrite(data_ptr, 1, strlen(data_ptr), fp);
                expected_seq = 1 - expected_seq;
            }

            // Send ACK for last sequence received
            char ack_msg[16];
            snprintf(ack_msg, sizeof(ack_msg), "ACK:%d", seq);
            sendto(sockfd, ack_msg, strlen(ack_msg), 0, (struct sockaddr *)&cliaddr, len);
        }
        else
        {
            printf("[SERVER] From Client: %s\n", buff);// Handle text or exit messages (non-file data)
            if (strncmp("exit", buff, 4) == 0)
            {
                printf("[SERVER] Client disconnected...\n");
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
    fd_set readfds;
    struct timeval tv;// Timeout struct

    while (1)
    {
        printf("\nEnter filename to send (or 'exit' to quit): ");
        scanf("%s", filename);

        if (strncmp(filename, "exit", 4) == 0)
        {
            sendto(sockfd, "exit", 4, 0, (struct sockaddr *)&cliaddr, len);
            printf("Server exit...\n");
            break;
        }

        FILE *fp = fopen(filename, "rb");
        if (!fp)
        {
            perror("File open error");
            continue;
        }

        // Notify client of file start
        char header[512];
        snprintf(header, sizeof(header), "%s %s", FILE_START, filename);
        sendto(sockfd, header, strlen(header), 0, (struct sockaddr *)&cliaddr, len);
        usleep(100000);// Delay for synchronization

        int seq = 0;// Sequence number for Stop-and-Wait
        int chunk_no = 0;// Chunk counter
        while (!feof(fp))
        {
            int bytes = fread(buff, 1, MAX, fp);// Read a file chunk
            if (bytes <= 0)
                break;

            snprintf(packet, sizeof(packet), "SEQ:%d|", seq);
            memcpy(packet + strlen(packet), buff, bytes);// Append raw bytes

            int ack_received = 0;// Flag for acknowledgment
            while (!ack_received)
            {
            	// Send current packet
                sendto(sockfd, packet, strlen(packet), 0, (struct sockaddr *)&cliaddr, len);
                printf("[SERVER] Sent chunk #%d (seq %d)\n", ++chunk_no, seq);
                
                // Wait for ACK using select()
                FD_ZERO(&readfds);//Clears (initializes) the file-descriptor set readfds
                FD_SET(sockfd, &readfds);//Adds our UDP socket sockfd into the readfds set.
                tv.tv_sec = 0;
                tv.tv_usec = TIMEOUT_USEC;//Sets the timeout for select().

                int rv = select(sockfd + 1, &readfds, NULL, NULL, &tv);
                if (rv > 0)
                {
                    // ACK received before timeout
                    char ack[32];
                    int n = recvfrom(sockfd, ack, sizeof(ack), 0, NULL, NULL);
                    ack[n] = '\0';
                    int ack_seq;
                    
                    // Validate ACK sequence number
                    if (sscanf(ack, "ACK:%d", &ack_seq) == 1 && ack_seq == seq)
                    {
                        ack_received = 1;
                        seq = 1 - seq;
                    }
                }
                else
                    printf("[SERVER] Timeout → retransmitting seq %d...\n", seq);// Timeout occurred → retransmit
            }
        }
        fclose(fp);
        sendto(sockfd, FILE_END, strlen(FILE_END), 0, (struct sockaddr *)&cliaddr, len);
        printf("[SERVER] File '%s' sent successfully!\n", filename);
    }
    return NULL;
}

// ---------------------- Main ----------------------
int main()
{
    struct sockaddr_in servaddr;
    pthread_t recv_thread, send_thread;

    sockfd = socket(AF_INET, SOCK_DGRAM, 0);
    if (sockfd < 0)
    {
        perror("Socket");
        exit(1);
    }

    bzero(&servaddr, sizeof(servaddr));
    servaddr.sin_family = AF_INET;
    servaddr.sin_addr.s_addr = INADDR_ANY;
    servaddr.sin_port = htons(PORT);

    if (bind(sockfd, (const struct sockaddr *)&servaddr, sizeof(servaddr)) < 0)
    {
        perror("Bind");
        exit(1);
    }

    printf("Server listening on UDP port %d...\n", PORT);

    char hello[MAX];
    int n = recvfrom(sockfd, hello, sizeof(hello), 0, (struct sockaddr *)&cliaddr, &len);
    hello[n] = '\0';
    printf("Client connected: %s\n", inet_ntoa(cliaddr.sin_addr));

    pthread_create(&recv_thread, NULL, receive_data, NULL);
    pthread_create(&send_thread, NULL, send_file, NULL);

    pthread_join(send_thread, NULL);
    pthread_join(recv_thread, NULL);

    close(sockfd);
    return 0;
}

