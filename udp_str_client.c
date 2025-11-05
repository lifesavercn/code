#include<stdio.h>
#include<stdlib.h>
#include<string.h>
#include<unistd.h>
#include<netinet/in.h>
#include<arpa/inet.h>
#define PORT 5311
#define SIZE 50
int main(){
	int sockfd;
	struct sockaddr_in servaddr;
    char buffer[SIZE], str[SIZE];
    socklen_t len = sizeof(servaddr);

    // Create UDP socket
    if ((sockfd = socket(AF_INET, SOCK_DGRAM, 0)) < 0) {
        perror("Socket creation failed");
        exit(EXIT_FAILURE);
    }

    memset(&servaddr, 0, sizeof(servaddr));

    servaddr.sin_family = AF_INET;
    servaddr.sin_port = htons(PORT);
    servaddr.sin_addr.s_addr = inet_addr("127.0.0.1"); // Localhost

    printf("Enter a string (max 100 chars): ");
    fgets(str, SIZE, stdin);
    str[strcspn(str, "\n")] = '\0'; // remove newline

    // Send to server
    sendto(sockfd, (const char *)str, strlen(str), 0, (const struct sockaddr *)&servaddr, sizeof(servaddr));
    printf("[Client] Sent string: %s\n", str);

    // Receive modified string
    int n = recvfrom(sockfd, (char *)buffer, SIZE, 0, (struct sockaddr *)&servaddr, &len);
    buffer[n] = '\0';
    printf("[Client] Received modified string: %s\n", buffer);

    close(sockfd);
    return 0;
}
