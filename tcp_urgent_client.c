#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <unistd.h>
#include <arpa/inet.h>

#define PORT 9233

int main() {
    int sock;
    struct sockaddr_in serv_addr;

    sock = socket(AF_INET, SOCK_STREAM, 0);
    if (sock < 0) {
        perror("Socket creation failed");
        exit(1);
    }

    serv_addr.sin_family = AF_INET;
    serv_addr.sin_port = htons(PORT);
    inet_pton(AF_INET, "127.0.0.1", &serv_addr.sin_addr);

    if (connect(sock, (struct sockaddr*)&serv_addr, sizeof(serv_addr)) < 0) {
        perror("Connection failed");
        exit(1);
    }

    // Send normal data
    send(sock, "Hello Normal Data", strlen("Hello Normal Data"), 0);
    sleep(1);

    // Send urgent data using MSG_OOB flag
    send(sock, "!", 1, MSG_OOB);
    printf("[Client] Sent URGENT data (!)\n");
    sleep(1);

    // Send more normal data
    send(sock, "End Message", strlen("End Message"), 0);

    close(sock);
    return 0;
}

