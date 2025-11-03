#include "common.h"
#include <signal.h>
#include <stdarg.h>

#define NM_PORT 8080
#define METADATA_FILE "nm_metadata.dat"

// Global data structures
typedef struct FileNode {
    FileMetadata metadata;
    UserAccess* access_list;
    int access_count;
    struct FileNode* next;
} FileNode;

typedef struct {
    char key[MAX_FILENAME];
    FileNode* file;
} HashEntry;

typedef struct {
    char username[MAX_USERNAME];
    char ip[INET_ADDRSTRLEN];
    int sock;
    time_t connected_time;
} ClientInfo;

// LRU Cache Node
typedef struct CacheNode {
    char filename[MAX_FILENAME];
    FileNode* file;
    struct CacheNode* prev;
    struct CacheNode* next;
} CacheNode;

typedef struct {
    CacheNode* head;
    CacheNode* tail;
    int size;
    HashEntry* cache_map[LRU_CACHE_SIZE];
} LRUCache;

// Global variables
FileNode* file_list = NULL;
HashEntry* file_hash[MAX_FILES];
StorageServerInfo storage_servers[MAX_STORAGE_SERVERS];
ClientInfo clients[MAX_CLIENTS];
int num_storage_servers = 0;
int num_clients = 0;
pthread_mutex_t data_mutex = PTHREAD_MUTEX_INITIALIZER;
LRUCache cache;
FILE* log_file = NULL;

// Structure to pass to client handler thread
typedef struct {
    int sock;
    char ip[INET_ADDRSTRLEN];
    int port;
} ClientThreadArg;

// Bonus: Folder structure
typedef struct FolderNode {
    char foldername[MAX_FILENAME];
    char owner[MAX_USERNAME];
    time_t created;
    struct FolderNode* next;
} FolderNode;
FolderNode* folder_list = NULL;

// Bonus: Checkpoints (reduce size)
Checkpoint* checkpoints = NULL;
int num_checkpoints = 0;
int max_checkpoints = 100;

// Bonus: Access Requests (reduce size)
AccessRequest* access_requests = NULL;
int num_access_requests = 0;
int max_access_requests = 50;

// Bonus: System metrics
typedef struct {
    int total_reads;
    int total_writes;
    int total_creates;
    int total_deletes;
    int active_connections;
    time_t start_time;
} SystemMetrics;
SystemMetrics metrics = {0, 0, 0, 0, 0, 0};

// Function prototypes
void init_cache();
FileNode* cache_get(const char* filename);
void cache_put(const char* filename, FileNode* file);
unsigned int hash_function(const char* str);
FileNode* find_file(const char* filename);
void add_file(FileMetadata* metadata);
int get_user_access(FileNode* file, const char* username);
void* handle_client(void* arg);
void* handle_storage_server(void* arg);
void save_metadata();
void load_metadata();
void log_to_file(const char* format, ...);
// Bonus function prototypes
void handle_create_folder(int client_sock, Message* msg);
void handle_move_file(int client_sock, Message* msg);
void handle_view_folder(int client_sock, Message* msg);
void handle_checkpoint(int client_sock, Message* msg);
void handle_view_checkpoint(int client_sock, Message* msg);
void handle_revert_checkpoint(int client_sock, Message* msg);
void handle_list_checkpoints(int client_sock, Message* msg);
void handle_request_access(int client_sock, Message* msg);
void handle_view_requests(int client_sock, Message* msg);
void handle_approve_request(int client_sock, Message* msg);
void handle_deny_request(int client_sock, Message* msg);
void handle_search(int client_sock, Message* msg);
void handle_metrics(int client_sock, Message* msg);
int count_files();
int count_folders();
// Bonus: Fault tolerance
void* monitor_storage_servers(void* arg);
void handle_heartbeat(Message* msg);
void handle_replication_request(int client_sock, Message* msg);

// Initialize LRU cache
void init_cache() {
    cache.head = NULL;
    cache.tail = NULL;
    cache.size = 0;
    memset(cache.cache_map, 0, sizeof(cache.cache_map));
}

// Get client info by username
ClientInfo* get_client_info(const char* username) {
    for (int i = 0; i < num_clients; i++) {
        if (strcmp(clients[i].username, username) == 0) {
            return &clients[i];
        }
    }
    return NULL;
}

// Hash function for efficient file lookup
unsigned int hash_function(const char* str) {
    unsigned int hash = 5381;
    int c;
    while ((c = *str++)) {
        hash = ((hash << 5) + hash) + c;
    }
    return hash % MAX_FILES;
}

// Find file using hash table
FileNode* find_file(const char* filename) {
    // Check cache first
    FileNode* cached = cache_get(filename);
    if (cached) {
        log_message("NM", "Cache hit for file: %s", filename);
        return cached;
    }
    
    unsigned int index = hash_function(filename);
    HashEntry* entry = file_hash[index];
    
    while (entry) {
        if (strcmp(entry->key, filename) == 0) {
            cache_put(filename, entry->file);
            return entry->file;
        }
        entry = (HashEntry*)entry->file->next;
    }
    
    return NULL;
}

// LRU Cache operations
FileNode* cache_get(const char* filename) {
    unsigned int index = hash_function(filename) % LRU_CACHE_SIZE;
    HashEntry* entry = cache.cache_map[index];
    
    if (entry && strcmp(entry->key, filename) == 0) {
        return entry->file;
    }
    return NULL;
}

void cache_put(const char* filename, FileNode* file) {
    unsigned int index = hash_function(filename) % LRU_CACHE_SIZE;
    
    HashEntry* entry = (HashEntry*)malloc(sizeof(HashEntry));
    strcpy(entry->key, filename);
    entry->file = file;
    cache.cache_map[index] = entry;
}

// Update hash table entry when filename changes (e.g., during MOVE)
void update_file_hash(const char* old_filename, const char* new_filename, FileNode* file_node) {
    // Remove from old hash bucket
    unsigned int old_index = hash_function(old_filename);
    
    // Find and remove from the old hash bucket
    if (file_hash[old_index] && strcmp(file_hash[old_index]->key, old_filename) == 0) {
        // It's the first entry in this bucket
        free(file_hash[old_index]);
        file_hash[old_index] = NULL;
    }
    
    // Add to new hash bucket (overwrites if exists, which is fine since we just removed it)
    unsigned int new_index = hash_function(new_filename);
    if (file_hash[new_index]) {
        free(file_hash[new_index]); // Free old entry if exists
    }
    file_hash[new_index] = (HashEntry*)malloc(sizeof(HashEntry));
    strcpy(file_hash[new_index]->key, new_filename);
    file_hash[new_index]->file = file_node;
    
    // Note: Cache entries will naturally expire - no explicit removal needed
}

// Add file to hash table
void add_file(FileMetadata* metadata) {
    unsigned int index = hash_function(metadata->filename);
    
    FileNode* node = (FileNode*)malloc(sizeof(FileNode));
    memcpy(&node->metadata, metadata, sizeof(FileMetadata));
    node->access_list = NULL;
    node->access_count = 0;
    node->next = NULL;
    
    // Add owner with full access
    node->access_list = (UserAccess*)malloc(sizeof(UserAccess));
    strcpy(node->access_list[0].username, metadata->owner);
    node->access_list[0].access_rights = ACCESS_READ | ACCESS_WRITE;
    node->access_count = 1;
    
    if (file_hash[index] == NULL) {
        HashEntry* entry = (HashEntry*)malloc(sizeof(HashEntry));
        strcpy(entry->key, metadata->filename);
        entry->file = node;
        file_hash[index] = entry;
    } else {
        FileNode* current = file_hash[index]->file;
        while (current->next) {
            current = current->next;
        }
        current->next = node;
    }
    
    // Add to linked list
    node->next = file_list;
    file_list = node;
}

// Get user access rights
int get_user_access(FileNode* file, const char* username) {
    for (int i = 0; i < file->access_count; i++) {
        if (strcmp(file->access_list[i].username, username) == 0) {
            return file->access_list[i].access_rights;
        }
    }
    return ACCESS_NONE;
}
// Update file statistics from Storage Server
void update_file_stats(FileNode* file) {
    if (!file || file->metadata.ss_index >= num_storage_servers) return;
    
    int ss_index = file->metadata.ss_index;
    int ss_sock = connect_to_server(storage_servers[ss_index].ip, 
        storage_servers[ss_index].nm_port);
    
    if (ss_sock < 0) return;
    
    Message msg;
    memset(&msg, 0, sizeof(msg));
    msg.type = MSG_SS_STAT;
    strcpy(msg.filename, file->metadata.filename);
    
    send_message(ss_sock, &msg);
    
    Message response;
    if (receive_message(ss_sock, &response) == 0 && response.error_code == ERR_SUCCESS) {
        // Response data contains: word_count char_count
        sscanf(response.data, "%d %d", &file->metadata.word_count, &file->metadata.char_count);
    }
    
    close(ss_sock);
}

// Log to file with timestamp
void log_to_file(const char* format, ...) {
    if (!log_file) return;
    
    time_t now;
    time(&now);
    char time_str[64];
    strftime(time_str, sizeof(time_str), "%Y-%m-%d %H:%M:%S", localtime(&now));
    
    fprintf(log_file, "[%s] ", time_str);
    
    va_list args;
    va_start(args, format);
    vfprintf(log_file, format, args);
    va_end(args);
    
    fprintf(log_file, "\n");
    fflush(log_file);
}
