// udp_fd_server_v2_mod_fixed.c
// --------------------------------------------------------------
// Full-duplex UDP File Transfer Server (Version 2 - Fixed)
// Shows chunk progress and auto-renames received files
// to avoid overwriting sender’s originals.
// --------------------------------------------------------------

#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <unistd.h>
#include <arpa/inet.h>
#include <pthread.h>

#define MAX 1024
#define PORT 8210
#define FILE_START "FILE_START"
#define FILE_END "FILE_END"

struct sockaddr_in cliaddr;
socklen_t len = sizeof(cliaddr);
int sockfd;

void *receive_data(void *args)
{
    char buff[MAX];
    FILE *fp = NULL;
    int receiving_file = 0;
    char filename[256], new_filename[300];
    long chunk_count = 0;

    while (1)
    {
        bzero(buff, MAX);
        int n = recvfrom(sockfd, buff, sizeof(buff), 0, (struct sockaddr *)&cliaddr, &len);
        if (n <= 0)
            continue;

        buff[n] = '\0';

        if (strncmp(buff, FILE_START, strlen(FILE_START)) == 0)
        {
            sscanf(buff + strlen(FILE_START), "%s", filename);
            snprintf(new_filename, sizeof(new_filename), "received_%s", filename);
            fp = fopen(new_filename, "wb");
            if (!fp)
            {
                perror("File open error");
                continue;
            }
            receiving_file = 1;
            chunk_count = 0;
            printf("\n[SERVER] Receiving file: %s -> saved as %s\n", filename, new_filename);
            continue;
        }

        if (strncmp(buff, FILE_END, strlen(FILE_END)) == 0)
        {
            if (fp)
            {
                fclose(fp);
                fp = NULL;
            }
            receiving_file = 0;
            printf("[SERVER] File received successfully (%ld chunks)!\n", chunk_count);
            continue;
        }

        if (receiving_file)
        {
            fwrite(buff, 1, n, fp);
            chunk_count++;
            printf("[SERVER] Receiving chunk #%ld\r", chunk_count);
            fflush(stdout);
        }
        else
        {
            printf("From Client: %s", buff);
            fflush(stdout);

            if (strncmp("exit", buff, 4) == 0)
            {
                printf("Client disconnected...\n");
                break;
            }
        }
    }
    return NULL;
}

void *send_file(void *args)
{
    char filename[256];
    char buff[MAX];
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

        char header[512];
        snprintf(header, sizeof(header), "%s %s", FILE_START, filename);
        sendto(sockfd, header, strlen(header), 0, (struct sockaddr *)&cliaddr, len);
        usleep(100000);

        long chunk_count = 0;
        while (!feof(fp))
        {
            int bytes_read = fread(buff, 1, MAX, fp);
            if (bytes_read > 0)
            {
                sendto(sockfd, buff, bytes_read, 0, (struct sockaddr *)&cliaddr, len);
                chunk_count++;
                printf("[SERVER] Sent chunk #%ld\r", chunk_count);
                fflush(stdout);
                usleep(1000);
            }
        }
        fclose(fp);

        sendto(sockfd, FILE_END, strlen(FILE_END), 0, (struct sockaddr *)&cliaddr, len);
        printf("\n[SERVER] File '%s' sent successfully (%ld chunks)!\n", filename, chunk_count);
    }
    return NULL;
}

int main()
{
    struct sockaddr_in servaddr;
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
    servaddr.sin_addr.s_addr = INADDR_ANY;
    servaddr.sin_port = htons(PORT);

    if (bind(sockfd, (const struct sockaddr *)&servaddr, sizeof(servaddr)) < 0)
    {
        perror("Bind failed");
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

