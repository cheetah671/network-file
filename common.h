#ifndef COMMON_H
#define COMMON_H

#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <unistd.h>
#include <arpa/inet.h>
#include <sys/socket.h>
#include <pthread.h>
#include <time.h>
#include <errno.h>
#include <sys/stat.h>
#include <fcntl.h>
#include <dirent.h>

// Configuration
#define MAX_BUFFER_SIZE 65536
#define MAX_FILENAME 256
#define MAX_USERNAME 64
#define MAX_CLIENTS 100
#define MAX_STORAGE_SERVERS 50
#define MAX_FILES 10000
#define MAX_SENTENCE_LENGTH 4096
#define MAX_WORD_LENGTH 256
#define LRU_CACHE_SIZE 100

// Error Codes
#define ERR_SUCCESS 0
#define ERR_FILE_NOT_FOUND 1
#define ERR_UNAUTHORIZED 2
#define ERR_FILE_EXISTS 3
#define ERR_INVALID_INDEX 4
#define ERR_SENTENCE_LOCKED 5
#define ERR_NO_STORAGE_SERVER 6
#define ERR_CONNECTION_FAILED 7
#define ERR_INVALID_COMMAND 8
#define ERR_SERVER_ERROR 9
#define ERR_NO_UNDO_AVAILABLE 10

// Message Types
#define MSG_REGISTER_SS 100
#define MSG_REGISTER_CLIENT 101
#define MSG_VIEW_FILES 102
#define MSG_READ_FILE 103
#define MSG_CREATE_FILE 104
#define MSG_WRITE_FILE 105
#define MSG_DELETE_FILE 106
#define MSG_INFO_FILE 107
#define MSG_STREAM_FILE 108
#define MSG_LIST_USERS 109
#define MSG_ADD_ACCESS 110
#define MSG_REM_ACCESS 111
#define MSG_EXEC_FILE 112
#define MSG_UNDO_FILE 113
// Bonus: Folders
#define MSG_CREATE_FOLDER 114
#define MSG_MOVE_FILE 115
#define MSG_VIEW_FOLDER 116
// Bonus: Checkpoints
#define MSG_CHECKPOINT 117
#define MSG_VIEW_CHECKPOINT 118
#define MSG_REVERT_CHECKPOINT 119
#define MSG_LIST_CHECKPOINTS 120
// Bonus: Access Requests
#define MSG_REQUEST_ACCESS 121
#define MSG_VIEW_REQUESTS 122
#define MSG_APPROVE_REQUEST 123
#define MSG_DENY_REQUEST 124
// Bonus: Search and Metrics
#define MSG_SEARCH_FILE 125
#define MSG_GET_METRICS 126
#define MSG_RESPONSE 200
#define MSG_SS_CREATE 201
#define MSG_SS_DELETE 202
#define MSG_SS_READ 203
#define MSG_SS_WRITE 204
#define MSG_SS_STREAM 205
#define MSG_SS_UNDO 206
#define MSG_SS_STAT 207
#define MSG_SS_CHECKPOINT 208
#define MSG_SS_REPLICATE 209
#define MSG_HEARTBEAT 210
#define MSG_SS_CREATE_FOLDER 211
#define MSG_SS_MOVE_FILE 212
#define MSG_ACK 250
#define MSG_ERROR 255

// Access Rights
#define ACCESS_NONE 0
#define ACCESS_READ 1
#define ACCESS_WRITE 2

// Structure for messages
typedef struct {
    int type;
    int error_code;
    char username[MAX_USERNAME];
    char filename[MAX_FILENAME];
    char data[MAX_BUFFER_SIZE];
    int data_len;
    int flags; // For view flags, sentence numbers, etc.
    int word_index;
    char target_user[MAX_USERNAME];
    char ss_ip[INET_ADDRSTRLEN];
    int ss_port;
    // Bonus fields
    char folder_path[MAX_FILENAME];
    char checkpoint_tag[MAX_USERNAME];
} Message;

// File metadata structure
typedef struct {
    char filename[MAX_FILENAME];
    char folder_path[MAX_FILENAME]; // Bonus: hierarchical folders
    char owner[MAX_USERNAME];
    time_t created;
    time_t last_modified;
    time_t last_accessed;
    int word_count;
    int char_count;
    int ss_index; // Index of storage server
    int replica_ss_index; // Bonus: replication
} FileMetadata;

// User access structure
typedef struct {
    char username[MAX_USERNAME];
    int access_rights; // ACCESS_NONE, ACCESS_READ, ACCESS_WRITE
} UserAccess;

// Storage Server Info
typedef struct {
    char ip[INET_ADDRSTRLEN];
    int nm_port;
    int client_port;
    int is_active;
    time_t last_heartbeat;
    int replica_of; // Bonus: -1 if primary, else index of primary SS
} StorageServerInfo;

// Bonus: Access Request structure
typedef struct {
    char filename[MAX_FILENAME];
    char requester[MAX_USERNAME];
    int requested_rights; // ACCESS_READ or ACCESS_WRITE
    time_t request_time;
} AccessRequest;

// Bonus: Checkpoint structure
typedef struct {
    char filename[MAX_FILENAME];
    char tag[MAX_USERNAME];
    char content[MAX_BUFFER_SIZE];
    time_t created;
} Checkpoint;

// Utility functions
void log_message(const char* component, const char* format, ...);
void send_message(int sock, Message* msg);
int receive_message(int sock, Message* msg);
void format_time(time_t time, char* buffer, size_t size);
int create_socket(int port);
int connect_to_server(const char* ip, int port);

#endif
