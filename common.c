#include "common.h"
#include <stdarg.h>

// Logging utility
void log_message(const char* component, const char* format, ...) {
    time_t now;
    time(&now);
    char time_str[64];
    strftime(time_str, sizeof(time_str), "%Y-%m-%d %H:%M:%S", localtime(&now));
    
    printf("[%s] [%s] ", time_str, component);
    
    va_list args;
    va_start(args, format);
    vprintf(format, args);
    va_end(args);
    
    printf("\n");
    fflush(stdout);
}

// Send message over socket
void send_message(int sock, Message* msg) {
    int total_sent = 0;
    int bytes_to_send = sizeof(Message);
    char* ptr = (char*)msg;
    
    while (total_sent < bytes_to_send) {
        int sent = send(sock, ptr + total_sent, bytes_to_send - total_sent, 0);
        if (sent <= 0) {
            log_message("COMMON", "Error sending message: %s", strerror(errno));
            return;
        }
        total_sent += sent;
    }
}

// Receive message from socket
int receive_message(int sock, Message* msg) {
    int total_received = 0;
    int bytes_to_receive = sizeof(Message);
    char* ptr = (char*)msg;
    
    while (total_received < bytes_to_receive) {
        int received = recv(sock, ptr + total_received, bytes_to_receive - total_received, 0);
        if (received <= 0) {
            if (received == 0) {
                log_message("COMMON", "Connection closed");
            } else {
                log_message("COMMON", "Error receiving message: %s", strerror(errno));
            }
            return -1;
        }
        total_received += received;
    }
    return 0;
}

// Format time for display
void format_time(time_t time, char* buffer, size_t size) {
    strftime(buffer, size, "%Y-%m-%d %H:%M:%S", localtime(&time));
}

// Create a server socket
int create_socket(int port) {
    int sock = socket(AF_INET, SOCK_STREAM, 0);
    if (sock < 0) {
        log_message("COMMON", "Error creating socket: %s", strerror(errno));
        return -1;
    }
    
    int opt = 1;
    if (setsockopt(sock, SOL_SOCKET, SO_REUSEADDR, &opt, sizeof(opt)) < 0) {
        log_message("COMMON", "Error setting socket options: %s", strerror(errno));
        close(sock);
        return -1;
    }
    
    struct sockaddr_in addr;
    memset(&addr, 0, sizeof(addr));
    addr.sin_family = AF_INET;
    addr.sin_addr.s_addr = INADDR_ANY;
    addr.sin_port = htons(port);
    
    if (bind(sock, (struct sockaddr*)&addr, sizeof(addr)) < 0) {
        log_message("COMMON", "Error binding socket to port %d: %s", port, strerror(errno));
        close(sock);
        return -1;
    }
    
    if (listen(sock, 10) < 0) {
        log_message("COMMON", "Error listening on socket: %s", strerror(errno));
        close(sock);
        return -1;
    }
    
    return sock;
}

// Connect to a server
int connect_to_server(const char* ip, int port) {
    int sock = socket(AF_INET, SOCK_STREAM, 0);
    if (sock < 0) {
        log_message("COMMON", "Error creating socket: %s", strerror(errno));
        return -1;
    }
    
    struct sockaddr_in addr;
    memset(&addr, 0, sizeof(addr));
    addr.sin_family = AF_INET;
    addr.sin_port = htons(port);
    
    if (inet_pton(AF_INET, ip, &addr.sin_addr) <= 0) {
        log_message("COMMON", "Invalid IP address: %s", ip);
        close(sock);
        return -1;
    }
    
    if (connect(sock, (struct sockaddr*)&addr, sizeof(addr)) < 0) {
        log_message("COMMON", "Error connecting to %s:%d: %s", ip, port, strerror(errno));
        close(sock);
        return -1;
    }
    
    return sock;
}
