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
void handle_view(int client_sock, Message* msg) {
    pthread_mutex_lock(&data_mutex);
    
    int show_all = (msg->flags & 1);
    int show_details = (msg->flags & 2);
    
    Message response;
    memset(&response, 0, sizeof(response));
    response.type = MSG_RESPONSE;
    response.error_code = ERR_SUCCESS;
    
    char buffer[MAX_BUFFER_SIZE] = {0};
    int offset = 0;
    
    if (show_details) {
        offset += sprintf(buffer + offset, 
            "---------------------------------------------------------\n"
            "|  Filename  | Words | Chars | Last Access Time | Owner |\n"
            "|------------|-------|-------|------------------|-------|\n");
    }
    
    FileNode* current = file_list;
    while (current && offset < MAX_BUFFER_SIZE - 1024) {
        int access = get_user_access(current, msg->username);
        
        if (show_all || access != ACCESS_NONE) {
            // Update stats if showing details
            if (show_details) {
                update_file_stats(current);
            }
            
            if (show_details) {
                char time_str[32];
                format_time(current->metadata.last_accessed, time_str, sizeof(time_str));
                offset += sprintf(buffer + offset, "| %-10s | %5d | %5d | %16s | %5s |\n",
                    current->metadata.filename,
                    current->metadata.word_count,
                    current->metadata.char_count,
                    time_str,
                    current->metadata.owner);
            } else {
                offset += sprintf(buffer + offset, "--> %s\n", current->metadata.filename);
            }
        }
        current = current->next;
    }
    
    if (show_details) {
        offset += sprintf(buffer + offset, 
            "---------------------------------------------------------\n");
    }
    
    strcpy(response.data, buffer);
    response.data_len = strlen(buffer);
    
    pthread_mutex_unlock(&data_mutex);
    
    send_message(client_sock, &response);
    
    // Log with client IP and outcome
    ClientInfo* client = get_client_info(msg->username);
    if (client) {
        log_to_file("VIEW request from %s (IP: %s), flags=%d - SUCCESS", 
                   msg->username, client->ip, msg->flags);
    } else {
        log_to_file("VIEW request from %s, flags=%d - SUCCESS", 
                   msg->username, msg->flags);
    }
}

// Handle INFO command
void handle_info(int client_sock, Message* msg) {
    pthread_mutex_lock(&data_mutex);
    
    FileNode* file = find_file(msg->filename);
    
    Message response;
    memset(&response, 0, sizeof(response));
    response.type = MSG_RESPONSE;
    
    if (!file) {
        response.error_code = ERR_FILE_NOT_FOUND;
        strcpy(response.data, "ERROR: File not found");
    } else {
        int access = get_user_access(file, msg->username);
        if (access == ACCESS_NONE) {
            response.error_code = ERR_UNAUTHORIZED;
            strcpy(response.data, "ERROR: Unauthorized access");
        } else {
            // Update file stats first
            update_file_stats(file);
            
            response.error_code = ERR_SUCCESS;
            char buffer[MAX_BUFFER_SIZE];
            char created_str[32], modified_str[32], accessed_str[32];
            
            format_time(file->metadata.created, created_str, sizeof(created_str));
            format_time(file->metadata.last_modified, modified_str, sizeof(modified_str));
            format_time(file->metadata.last_accessed, accessed_str, sizeof(accessed_str));
            
            int offset = sprintf(buffer, 
                "--> File: %s\n"
                "--> Owner: %s\n"
                "--> Created: %s\n"
                "--> Last Modified: %s\n"
                "--> Size: %d bytes\n",
                file->metadata.filename,
                file->metadata.owner,
                created_str,
                modified_str,
                file->metadata.char_count);
            
            offset += sprintf(buffer + offset, "--> Access: ");
            for (int i = 0; i < file->access_count; i++) {
                offset += sprintf(buffer + offset, "%s (%s)%s",
                    file->access_list[i].username,
                    (file->access_list[i].access_rights & ACCESS_WRITE) ? "RW" : "R",
                    (i < file->access_count - 1) ? ", " : "");
            }
            offset += sprintf(buffer + offset, "\n--> Last Accessed: %s by %s\n",
                accessed_str, file->metadata.owner);
            
            strcpy(response.data, buffer);
            response.data_len = strlen(buffer);
        }
    }
    
    pthread_mutex_unlock(&data_mutex);
    
    send_message(client_sock, &response);
    
    // Log with client IP and outcome
    ClientInfo* client = get_client_info(msg->username);
    if (client) {
        log_to_file("INFO request from %s (IP: %s) for file %s - %s", 
                   msg->username, client->ip, msg->filename, 
                   response.error_code == ERR_SUCCESS ? "SUCCESS" : "FAILED");
    } else {
        log_to_file("INFO request from %s for file %s - %s", 
                   msg->username, msg->filename, 
                   response.error_code == ERR_SUCCESS ? "SUCCESS" : "FAILED");
    }
}

// Handle LIST USERS command
void handle_list_users(int client_sock, Message* msg) {
    pthread_mutex_lock(&data_mutex);
    
    Message response;
    memset(&response, 0, sizeof(response));
    response.type = MSG_RESPONSE;
    response.error_code = ERR_SUCCESS;
    
    char buffer[MAX_BUFFER_SIZE] = {0};
    int offset = 0;
    
    // Collect unique usernames
    char usernames[MAX_CLIENTS][MAX_USERNAME];
    int user_count = 0;
    
    // First, add all currently connected clients
    for (int i = 0; i < num_clients; i++) {
        int found = 0;
        for (int j = 0; j < user_count; j++) {
            if (strcmp(usernames[j], clients[i].username) == 0) {
                found = 1;
                break;
            }
        }
        if (!found && user_count < MAX_CLIENTS) {
            strcpy(usernames[user_count++], clients[i].username);
        }
    }
    
    // Then add users from file metadata
    FileNode* current = file_list;
    while (current) {
        // Add owner
        int found = 0;
        for (int i = 0; i < user_count; i++) {
            if (strcmp(usernames[i], current->metadata.owner) == 0) {
                found = 1;
                break;
            }
        }
        if (!found && user_count < MAX_CLIENTS) {
            strcpy(usernames[user_count++], current->metadata.owner);
        }
        
        // Add users with access
        for (int i = 0; i < current->access_count; i++) {
            found = 0;
            for (int j = 0; j < user_count; j++) {
                if (strcmp(usernames[j], current->access_list[i].username) == 0) {
                    found = 1;
                    break;
                }
            }
            if (!found && user_count < MAX_CLIENTS) {
                strcpy(usernames[user_count++], current->access_list[i].username);
            }
        }
        current = current->next;
    }
    
    for (int i = 0; i < user_count && offset < MAX_BUFFER_SIZE - 128; i++) {
        offset += sprintf(buffer + offset, "--> %s\n", usernames[i]);
    }
    
    strcpy(response.data, buffer);
    response.data_len = strlen(buffer);
    
    pthread_mutex_unlock(&data_mutex);
    
    send_message(client_sock, &response);
    
    // Log with client IP
    ClientInfo* client = get_client_info(msg->username);
    if (client) {
        log_to_file("LIST USERS request from %s (IP: %s) - SUCCESS", 
                   msg->username, client->ip);
    } else {
        log_to_file("LIST USERS request from %s - SUCCESS", msg->username);
    }
}

// Handle access control commands
void handle_access_control(int client_sock, Message* msg) {
    pthread_mutex_lock(&data_mutex);
    
    FileNode* file = find_file(msg->filename);
    
    Message response;
    memset(&response, 0, sizeof(response));
    response.type = MSG_RESPONSE;
    
    if (!file) {
        response.error_code = ERR_FILE_NOT_FOUND;
        strcpy(response.data, "ERROR: File not found");
    } else if (strcmp(file->metadata.owner, msg->username) != 0) {
        response.error_code = ERR_UNAUTHORIZED;
        strcpy(response.data, "ERROR: Only owner can modify access");
    } else {
        response.error_code = ERR_SUCCESS;
        
        if (msg->type == MSG_ADD_ACCESS) {
            // Find if user already has access
            int found = -1;
            for (int i = 0; i < file->access_count; i++) {
                if (strcmp(file->access_list[i].username, msg->target_user) == 0) {
                    found = i;
                    break;
                }
            }
            
            int new_rights = (msg->flags == 1) ? ACCESS_READ : (ACCESS_READ | ACCESS_WRITE);
            
            if (found >= 0) {
                file->access_list[found].access_rights = new_rights;
            } else {
                file->access_list = (UserAccess*)realloc(file->access_list, 
                    (file->access_count + 1) * sizeof(UserAccess));
                strcpy(file->access_list[file->access_count].username, msg->target_user);
                file->access_list[file->access_count].access_rights = new_rights;
                file->access_count++;
            }
            strcpy(response.data, "Access granted successfully!");
        } else if (msg->type == MSG_REM_ACCESS) {
            int found = -1;
            for (int i = 0; i < file->access_count; i++) {
                if (strcmp(file->access_list[i].username, msg->target_user) == 0) {
                    found = i;
                    break;
                }
            }
            
            if (found >= 0 && strcmp(msg->target_user, file->metadata.owner) != 0) {
                for (int i = found; i < file->access_count - 1; i++) {
                    file->access_list[i] = file->access_list[i + 1];
                }
                file->access_count--;
                strcpy(response.data, "Access removed successfully!");
            } else {
                response.error_code = ERR_INVALID_COMMAND;
                strcpy(response.data, "ERROR: Cannot remove owner access or user not found");
            }
        }
        
        save_metadata();
    }
    
    pthread_mutex_unlock(&data_mutex);
    
    send_message(client_sock, &response);
    
    // Log with client IP and outcome
    ClientInfo* client = get_client_info(msg->username);
    const char* op_name = (msg->type == MSG_ADD_ACCESS) ? "GRANT ACCESS" : "REMOVE ACCESS";
    if (client) {
        log_to_file("%s from %s (IP: %s) for file %s, target %s - %s", 
                   op_name, msg->username, client->ip, msg->filename, msg->target_user,
                   response.error_code == ERR_SUCCESS ? "SUCCESS" : "FAILED");
    } else {
        log_to_file("%s from %s for file %s, target %s - %s", 
                   op_name, msg->username, msg->filename, msg->target_user,
                   response.error_code == ERR_SUCCESS ? "SUCCESS" : "FAILED");
    }
}

// Handle CREATE command
void handle_create(int client_sock, Message* msg) {
    pthread_mutex_lock(&data_mutex);
    
    FileNode* existing = find_file(msg->filename);
    
    Message response;
    memset(&response, 0, sizeof(response));
    response.type = MSG_RESPONSE;
    
    if (existing) {
        response.error_code = ERR_FILE_EXISTS;
        strcpy(response.data, "ERROR: File already exists");
        pthread_mutex_unlock(&data_mutex);
        send_message(client_sock, &response);
        return;
    }
    
    // Find available storage server - RANDOM assignment for load balancing
    int ss_index = -1;
    int replica_ss_index = -1;
    
    if (num_storage_servers >= 2) {
        // Use filename as seed for randomness (consistent for same file)
        srand(time(NULL) + (unsigned int)strlen(msg->filename));
        
        // Randomly select primary
        int primary_idx = rand() % num_storage_servers;
        
        // Find first active server starting from random position
        for (int i = 0; i < num_storage_servers; i++) {
            int idx = (primary_idx + i) % num_storage_servers;
            if (storage_servers[idx].is_active) {
                ss_index = idx;
                break;
            }
        }
        
        // Randomly select secondary (different from primary)
        if (ss_index >= 0) {
            int secondary_idx = (ss_index + 1 + (rand() % (num_storage_servers - 1))) % num_storage_servers;
            
            for (int i = 0; i < num_storage_servers; i++) {
                int idx = (secondary_idx + i) % num_storage_servers;
                if (idx != ss_index && storage_servers[idx].is_active) {
                    replica_ss_index = idx;
                    break;
                }
            }
        }
        
        log_message("NM", "File '%s' assigned: Primary=SS%d, Secondary=SS%d", 
                   msg->filename, ss_index, replica_ss_index);
    } else if (num_storage_servers == 1) {
        // Only one server available
        if (storage_servers[0].is_active) {
            ss_index = 0;
            replica_ss_index = -1;
            log_message("NM", "File '%s' assigned: Primary=SS%d (No secondary available)", 
                       msg->filename, ss_index);
        }
    }
    
    if (ss_index < 0) {
        response.error_code = ERR_NO_STORAGE_SERVER;
        strcpy(response.data, "ERROR: No storage server available");
        pthread_mutex_unlock(&data_mutex);
        send_message(client_sock, &response);
        return;
    }
    
    pthread_mutex_unlock(&data_mutex);
    
    // Forward to storage server
    int ss_sock = connect_to_server(storage_servers[ss_index].ip, 
        storage_servers[ss_index].nm_port);
    
    if (ss_sock < 0) {
        response.error_code = ERR_CONNECTION_FAILED;
        strcpy(response.data, "ERROR: Cannot connect to storage server");
        send_message(client_sock, &response);
        return;
    }
    
    Message ss_msg;
    memset(&ss_msg, 0, sizeof(ss_msg));
    ss_msg.type = MSG_SS_CREATE;
    strcpy(ss_msg.filename, msg->filename);
    strcpy(ss_msg.username, msg->username);
    
    send_message(ss_sock, &ss_msg);
    
    Message ss_response;
    if (receive_message(ss_sock, &ss_response) == 0) {
        if (ss_response.error_code == ERR_SUCCESS) {
            // Add to metadata
            pthread_mutex_lock(&data_mutex);
            FileMetadata metadata;
            memset(&metadata, 0, sizeof(metadata)); // Initialize all fields
            strcpy(metadata.filename, msg->filename);
            strcpy(metadata.folder_path, ""); // Root folder by default
            strcpy(metadata.owner, msg->username);
            time(&metadata.created);
            metadata.last_modified = metadata.created;
            metadata.last_accessed = metadata.created;
            metadata.word_count = 0;
            metadata.char_count = 0;
            metadata.ss_index = ss_index;
            metadata.replica_ss_index = replica_ss_index; // Assign replica if available
            
            add_file(&metadata);
            
            // If replica server exists, create file there too
            if (replica_ss_index >= 0) {
                int replica_sock = connect_to_server(storage_servers[replica_ss_index].ip,
                                                     storage_servers[replica_ss_index].nm_port);
                if (replica_sock >= 0) {
                    Message replica_msg;
                    memset(&replica_msg, 0, sizeof(replica_msg));
                    replica_msg.type = MSG_SS_CREATE;
                    strcpy(replica_msg.filename, msg->filename);
                    strcpy(replica_msg.username, msg->username);
                    send_message(replica_sock, &replica_msg);
                    
                    Message replica_response;
                    receive_message(replica_sock, &replica_response);
                    close(replica_sock);
                    
                    if (replica_response.error_code == ERR_SUCCESS) {
                        log_message("NM", "Replica created for %s on SS %d", msg->filename, replica_ss_index);
                    }
                }
            }
            
            save_metadata();
            pthread_mutex_unlock(&data_mutex);
            
            response.error_code = ERR_SUCCESS;
            if (replica_ss_index >= 0) {
                sprintf(response.data, "File Created Successfully! (Primary: SS%d, Replica: SS%d)", 
                        ss_index, replica_ss_index);
            } else {
                strcpy(response.data, "File Created Successfully!");
            }
        } else {
            response.error_code = ss_response.error_code;
            strcpy(response.data, ss_response.data);
        }
    } else {
        response.error_code = ERR_SERVER_ERROR;
        strcpy(response.data, "ERROR: Storage server communication failed");
    }
    
    close(ss_sock);
    send_message(client_sock, &response);
    
    // Log with client IP and outcome
    ClientInfo* client = get_client_info(msg->username);
    if (client) {
        log_to_file("CREATE request from %s (IP: %s) for file %s - %s", 
                   msg->username, client->ip, msg->filename, 
                   response.error_code == ERR_SUCCESS ? "SUCCESS" : "FAILED");
        log_to_file("ACK SENT to %s (IP: %s): %s [Error Code: %d]", 
                   msg->username, client->ip, response.data, response.error_code);
    } else {
        log_to_file("CREATE request from %s for file %s - %s", 
                   msg->username, msg->filename, 
                   response.error_code == ERR_SUCCESS ? "SUCCESS" : "FAILED");
        log_to_file("ACK SENT to %s: %s [Error Code: %d]", 
                   msg->username, response.data, response.error_code);
    }
}

// Handle DELETE command
void handle_delete(int client_sock, Message* msg) {
    pthread_mutex_lock(&data_mutex);
    
    FileNode* file = find_file(msg->filename);
    
    Message response;
    memset(&response, 0, sizeof(response));
    response.type = MSG_RESPONSE;
    
    if (!file) {
        response.error_code = ERR_FILE_NOT_FOUND;
        strcpy(response.data, "ERROR: File not found");
        pthread_mutex_unlock(&data_mutex);
        send_message(client_sock, &response);
        return;
    }
    
    if (strcmp(file->metadata.owner, msg->username) != 0) {
        response.error_code = ERR_UNAUTHORIZED;
        strcpy(response.data, "ERROR: Only owner can delete file");
        pthread_mutex_unlock(&data_mutex);
        send_message(client_sock, &response);
        return;
    }
    
    int ss_index = file->metadata.ss_index;
    pthread_mutex_unlock(&data_mutex);
    
    // Forward to storage server
    int ss_sock = connect_to_server(storage_servers[ss_index].ip, 
        storage_servers[ss_index].nm_port);
    
    if (ss_sock < 0) {
        response.error_code = ERR_CONNECTION_FAILED;
        strcpy(response.data, "ERROR: Cannot connect to storage server");
        send_message(client_sock, &response);
        return;
    }
    
    Message ss_msg;
    memset(&ss_msg, 0, sizeof(ss_msg));
    ss_msg.type = MSG_SS_DELETE;
    strcpy(ss_msg.filename, msg->filename);
    
    send_message(ss_sock, &ss_msg);
    
    Message ss_response;
    if (receive_message(ss_sock, &ss_response) == 0) {
        if (ss_response.error_code == ERR_SUCCESS) {
            // Remove from metadata
            pthread_mutex_lock(&data_mutex);
            
            // Remove from linked list
            FileNode* prev = NULL;
            FileNode* current = file_list;
            while (current) {
                if (strcmp(current->metadata.filename, msg->filename) == 0) {
                    if (prev) {
                        prev->next = current->next;
                    } else {
                        file_list = current->next;
                    }
                    free(current->access_list);
                    free(current);
                    break;
                }
                prev = current;
                current = current->next;
            }
            
            // Remove from hash table (properly handle chaining)
            unsigned int index = hash_function(msg->filename);
            HashEntry* hash_prev = NULL;
            HashEntry* hash_current = file_hash[index];
            while (hash_current) {
                if (strcmp(hash_current->key, msg->filename) == 0) {
                    if (hash_prev) {
                        hash_prev->file->next = hash_current->file->next;
                    } else {
                        if (hash_current->file->next) {
                            file_hash[index] = (HashEntry*)hash_current->file->next;
                        } else {
                            file_hash[index] = NULL;
                        }
                    }
                    free(hash_current);
                    break;
                }
                hash_prev = hash_current;
                hash_current = (HashEntry*)hash_current->file->next;
            }
            
            // Clear from cache
            unsigned int cache_index = hash_function(msg->filename) % LRU_CACHE_SIZE;
            if (cache.cache_map[cache_index] && 
                strcmp(cache.cache_map[cache_index]->key, msg->filename) == 0) {
                free(cache.cache_map[cache_index]);
                cache.cache_map[cache_index] = NULL;
            }
            
            save_metadata();
            pthread_mutex_unlock(&data_mutex);
            
            response.error_code = ERR_SUCCESS;
            sprintf(response.data, "File '%s' deleted successfully!", msg->filename);
        } else {
            response.error_code = ss_response.error_code;
            strcpy(response.data, ss_response.data);
        }
    } else {
        response.error_code = ERR_SERVER_ERROR;
        strcpy(response.data, "ERROR: Storage server communication failed");
    }
    
    close(ss_sock);
    send_message(client_sock, &response);
    
    // Log with client IP and outcome
    ClientInfo* client = get_client_info(msg->username);
    if (client) {
        log_to_file("DELETE request from %s (IP: %s) for file %s - %s", 
                   msg->username, client->ip, msg->filename, 
                   response.error_code == ERR_SUCCESS ? "SUCCESS" : "FAILED");
        log_to_file("ACK SENT to %s (IP: %s): %s [Error Code: %d]", 
                   msg->username, client->ip, response.data, response.error_code);
    } else {
        log_to_file("DELETE request from %s for file %s - %s", 
                   msg->username, msg->filename, 
                   response.error_code == ERR_SUCCESS ? "SUCCESS" : "FAILED");
        log_to_file("ACK SENT to %s: %s [Error Code: %d]", 
                   msg->username, response.data, response.error_code);
    }
}

// Handle READ/WRITE/STREAM commands - return SS info to client
void handle_direct_ss_operation(int client_sock, Message* msg) {
    pthread_mutex_lock(&data_mutex);
    
    FileNode* file = find_file(msg->filename);
    
    Message response;
    memset(&response, 0, sizeof(response));
    response.type = MSG_RESPONSE;
    
    if (!file) {
        response.error_code = ERR_FILE_NOT_FOUND;
        strcpy(response.data, "ERROR: File not found");
    } else {
        int access = get_user_access(file, msg->username);
        int required_access = (msg->type == MSG_WRITE_FILE) ? ACCESS_WRITE : ACCESS_READ;
        
        if ((access & required_access) == 0) {
            response.error_code = ERR_UNAUTHORIZED;
            strcpy(response.data, "ERROR: Unauthorized access");
        } else {
            // Update last accessed time
            time(&file->metadata.last_accessed);
            
            // Update last modified time for write operations
            if (msg->type == MSG_WRITE_FILE) {
                time(&file->metadata.last_modified);
            }
            
            response.error_code = ERR_SUCCESS;
            strcpy(response.ss_ip, storage_servers[file->metadata.ss_index].ip);
            response.ss_port = storage_servers[file->metadata.ss_index].client_port;
            strcpy(response.folder_path, file->metadata.folder_path); // Send folder path to client
            
            // Include replica information in the response
            if (msg->type == MSG_WRITE_FILE && file->metadata.replica_ss_index >= 0 && 
                file->metadata.replica_ss_index < num_storage_servers &&
                storage_servers[file->metadata.replica_ss_index].is_active) {
                // Add replica info to data: PRIMARY_SS_INDEX|REPLICA_SS_INDEX|REPLICA_IP|REPLICA_PORT
                sprintf(response.data, "Primary:SS%d|Replica:SS%d:%s:%d", 
                       file->metadata.ss_index,
                       file->metadata.replica_ss_index,
                       storage_servers[file->metadata.replica_ss_index].ip,
                       storage_servers[file->metadata.replica_ss_index].nm_port);
            } else {
                sprintf(response.data, "Connect to SS at %s:%d", response.ss_ip, response.ss_port);
            }
        }
    }
    
    // Save metadata to persist timestamp updates
    save_metadata();
    
    pthread_mutex_unlock(&data_mutex);
    
    send_message(client_sock, &response);
    
    // Log with client IP and outcome
    ClientInfo* client = get_client_info(msg->username);
    const char* op_name = (msg->type == MSG_READ_FILE) ? "READ" : 
                          (msg->type == MSG_WRITE_FILE) ? "WRITE" : "STREAM";
    if (client) {
        log_to_file("%s request from %s (IP: %s) for file %s - %s", 
                   op_name, msg->username, client->ip, msg->filename, 
                   response.error_code == ERR_SUCCESS ? "SUCCESS" : "FAILED");
        if (response.error_code == ERR_SUCCESS) {
            log_to_file("RESPONSE SENT to %s (IP: %s): Connect to SS at %s:%d [Error Code: %d]", 
                       msg->username, client->ip, response.ss_ip, response.ss_port, response.error_code);
        } else {
            log_to_file("RESPONSE SENT to %s (IP: %s): %s [Error Code: %d]", 
                       msg->username, client->ip, response.data, response.error_code);
        }
    } else {
        log_to_file("%s request from %s for file %s - %s", 
                   op_name, msg->username, msg->filename, 
                   response.error_code == ERR_SUCCESS ? "SUCCESS" : "FAILED");
        if (response.error_code == ERR_SUCCESS) {
            log_to_file("RESPONSE SENT to %s: Connect to SS at %s:%d [Error Code: %d]", 
                       msg->username, response.ss_ip, response.ss_port, response.error_code);
        } else {
            log_to_file("RESPONSE SENT to %s: %s [Error Code: %d]", 
                       msg->username, response.data, response.error_code);
        }
    }
}

// Handle EXEC command
void handle_exec(int client_sock, Message* msg) {
    pthread_mutex_lock(&data_mutex);
    
    FileNode* file = find_file(msg->filename);
    
    Message response;
    memset(&response, 0, sizeof(response));
    response.type = MSG_RESPONSE;
    
    if (!file) {
        response.error_code = ERR_FILE_NOT_FOUND;
        strcpy(response.data, "ERROR: File not found");
        pthread_mutex_unlock(&data_mutex);
        send_message(client_sock, &response);
        return;
    }
    
    int access = get_user_access(file, msg->username);
    if ((access & ACCESS_READ) == 0) {
        response.error_code = ERR_UNAUTHORIZED;
        strcpy(response.data, "ERROR: Unauthorized access");
        pthread_mutex_unlock(&data_mutex);
        send_message(client_sock, &response);
        return;
    }
    
    int ss_index = file->metadata.ss_index;
    pthread_mutex_unlock(&data_mutex);
    
    // Get file content from SS
    int ss_sock = connect_to_server(storage_servers[ss_index].ip, 
        storage_servers[ss_index].nm_port);
    
    if (ss_sock < 0) {
        response.error_code = ERR_CONNECTION_FAILED;
        strcpy(response.data, "ERROR: Cannot connect to storage server");
        send_message(client_sock, &response);
        return;
    }
    
    Message ss_msg;
    memset(&ss_msg, 0, sizeof(ss_msg));
    ss_msg.type = MSG_SS_READ;
    strcpy(ss_msg.filename, msg->filename);
    
    send_message(ss_sock, &ss_msg);
    
    Message ss_response;
    if (receive_message(ss_sock, &ss_response) == 0 && ss_response.error_code == ERR_SUCCESS) {
        // Execute commands
        char output[MAX_BUFFER_SIZE] = {0};
        FILE* fp = popen(ss_response.data, "r");
        if (fp) {
            size_t n = fread(output, 1, sizeof(output) - 1, fp);
            output[n] = '\0';
            pclose(fp);
            
            response.error_code = ERR_SUCCESS;
            strcpy(response.data, output);
        } else {
            response.error_code = ERR_SERVER_ERROR;
            strcpy(response.data, "ERROR: Command execution failed");
        }
    } else {
        response.error_code = ERR_SERVER_ERROR;
        strcpy(response.data, "ERROR: Cannot read file from storage server");
    }
    
    close(ss_sock);
    send_message(client_sock, &response);
    
    // Log with client IP and outcome
    ClientInfo* client = get_client_info(msg->username);
    if (client) {
        log_to_file("EXEC request from %s (IP: %s) for file %s - %s", 
                   msg->username, client->ip, msg->filename, 
                   response.error_code == ERR_SUCCESS ? "SUCCESS" : "FAILED");
        log_to_file("RESPONSE SENT to %s (IP: %s): %s [Error Code: %d]", 
                   msg->username, client->ip, 
                   response.error_code == ERR_SUCCESS ? "Execution completed" : response.data, 
                   response.error_code);
    } else {
        log_to_file("EXEC request from %s for file %s - %s", 
                   msg->username, msg->filename, 
                   response.error_code == ERR_SUCCESS ? "SUCCESS" : "FAILED");
        log_to_file("RESPONSE SENT to %s: %s [Error Code: %d]", 
                   msg->username, 
                   response.error_code == ERR_SUCCESS ? "Execution completed" : response.data, 
                   response.error_code);
    }
}
