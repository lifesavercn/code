#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <unistd.h>
#include <arpa/inet.h>
#include <signal.h>

#define PORT 9233

int client_socket;

// Signal handler to catch urgent data
void urgent_handler(int signo) {
    char ch;
    recv(client_socket, &ch, 1, MSG_OOB);
    printf("\n[Server] Received URGENT data: %c\n", ch);
}

int main() {
    int server_fd;
    struct sockaddr_in address;
    socklen_t addrlen = sizeof(address);
    char buffer[1024] = {0};

    // Create TCP socket
    server_fd = socket(AF_INET, SOCK_STREAM, 0);
    if (server_fd == -1) {
        perror("Socket failed");
        exit(1);
    }

    address.sin_family = AF_INET;
    address.sin_addr.s_addr = INADDR_ANY;
    address.sin_port = htons(PORT);

    // Bind and listen
    bind(server_fd, (struct sockaddr*)&address, sizeof(address));
    listen(server_fd, 1);

    printf("Server listening on port %d...\n", PORT);
    client_socket = accept(server_fd, (struct sockaddr*)&address, &addrlen);

    // Handle urgent data using signal SIGURG
    fcntl(client_socket, FIOSETOWN, getpid());
    signal(SIGURG, urgent_handler);

    while (1) {
        int n = recv(client_socket, buffer, sizeof(buffer)-1, 0);
        if (n <= 0) break;
        buffer[n] = '\0';
        printf("[Server] Normal data: %s\n", buffer);
    }

    close(client_socket);
    close(server_fd);
    return 0;
}

