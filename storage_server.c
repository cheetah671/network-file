#include "common.h"
#include <stdarg.h>
#include <dirent.h>
#include <sys/stat.h>

// Dynamic storage directories (set based on port number)
char STORAGE_DIR[256] = "./storage";
char UNDO_DIR[256] = "./undo";

typedef struct {
    char filename[MAX_FILENAME];
    int sentence_index;
    pthread_mutex_t lock;
    char locked_by[MAX_USERNAME];
} SentenceLock;

// Global variables
char nm_ip[INET_ADDRSTRLEN] = "127.0.0.1";
char my_ip[INET_ADDRSTRLEN] = "127.0.0.1";  // This server's IP address
int nm_port = 8080;
int client_port = 9000;
int nm_listen_port = 9001;
SentenceLock sentence_locks[MAX_FILES];
int num_locks = 0;
pthread_mutex_t locks_mutex = PTHREAD_MUTEX_INITIALIZER;
FILE* log_file = NULL;

// Bonus: Fault Tolerance - Persistent NM connection for heartbeat
int nm_heartbeat_sock = -1;
pthread_mutex_t nm_sock_mutex = PTHREAD_MUTEX_INITIALIZER;
int should_exit = 0; // Flag to stop threads on shutdown

// Function prototypes
void register_with_nm();
void get_local_ip(char* buffer, size_t size);
void* handle_nm_request(void* arg);
void* handle_client_request(void* arg);
void* heartbeat_thread(void* arg); // Bonus: Send periodic heartbeats
void* nm_listener(void* arg); // Existing NM listener thread
int read_file_content(const char* filename, char* buffer, size_t buffer_size);
int write_file_content(const char* filename, const char* content);
int parse_sentences(const char* content, char sentences[][MAX_SENTENCE_LENGTH], int* count);
int reconstruct_file(char sentences[][MAX_SENTENCE_LENGTH], int count, char* output);
void create_file(const char* filename, const char* owner);
void delete_file(const char* filename);
void save_for_undo(const char* filename);
void log_to_file(const char* format, ...);
void trigger_replication(const char* filename);
void* async_replicate_thread(void* arg);
void handle_replicate_from_primary(int nm_sock, Message* msg);

// Logging
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

// Get local IP address
void get_local_ip(char* buffer, size_t size) {
    int sock = socket(AF_INET, SOCK_DGRAM, 0);
    if (sock < 0) {
        strncpy(buffer, "127.0.0.1", size);
        return;
    }
    
    struct sockaddr_in server;
    memset(&server, 0, sizeof(server));
    server.sin_family = AF_INET;
    server.sin_addr.s_addr = inet_addr("8.8.8.8");  // Google DNS
    server.sin_port = htons(53);
    
    // Connect to determine which interface would be used
    if (connect(sock, (struct sockaddr*)&server, sizeof(server)) < 0) {
        close(sock);
        strncpy(buffer, "127.0.0.1", size);
        return;
    }
    
    struct sockaddr_in local;
    socklen_t len = sizeof(local);
    if (getsockname(sock, (struct sockaddr*)&local, &len) < 0) {
        close(sock);
        strncpy(buffer, "127.0.0.1", size);
        return;
    }
    
    inet_ntop(AF_INET, &local.sin_addr, buffer, size);
    close(sock);
}

// Parse content into sentences
int parse_sentences(const char* content, char sentences[][MAX_SENTENCE_LENGTH], int* count) {
    *count = 0;
    int len = strlen(content);
    int start = 0;
    
    for (int i = 0; i < len && *count < 1000; i++) {
        // Check if current character is a delimiter
        if (content[i] == '.' || content[i] == '!' || content[i] == '?') {
            int sentence_len = i - start + 1;
            if (sentence_len > 0 && sentence_len < MAX_SENTENCE_LENGTH) {
                strncpy(sentences[*count], content + start, sentence_len);
                sentences[*count][sentence_len] = '\0';
                (*count)++;
            }
            start = i + 1;
            
            // Skip whitespace after delimiter
            while (start < len && content[start] == ' ') {
                start++;
            }
            
            // IMPORTANT: If the next character after delimiter is NOT a space,
            // it means delimiter was attached to a word (like "bye.hello")
            // The sentence break still happens here - next content starts new sentence
        }
    }
    
    // Handle last sentence without delimiter (or remaining content after last delimiter)
    if (start < len) {
        int sentence_len = len - start;
        if (sentence_len > 0 && sentence_len < MAX_SENTENCE_LENGTH) {
            strncpy(sentences[*count], content + start, sentence_len);
            sentences[*count][sentence_len] = '\0';
            (*count)++;
        }
    }
    
    return *count;
}

// Reconstruct file from sentences
int reconstruct_file(char sentences[][MAX_SENTENCE_LENGTH], int count, char* output) {
    int offset = 0;
    for (int i = 0; i < count; i++) {
        int len = strlen(sentences[i]);
        strcpy(output + offset, sentences[i]);
        offset += len;
        
        // Add space between sentences if needed
        if (i < count - 1 && offset > 0 && output[offset - 1] != ' ') {
            output[offset++] = ' ';
        }
    }
    output[offset] = '\0';
    return offset;
}

// Construct full file path including folder
void construct_file_path(char* filepath, size_t size, const char* folder_path, const char* filename) {
    if (folder_path != NULL && strlen(folder_path) > 0 && strcmp(folder_path, "/") != 0) {
        snprintf(filepath, size, "%s/%s/%s", STORAGE_DIR, folder_path, filename);
    } else {
        snprintf(filepath, size, "%s/%s", STORAGE_DIR, filename);
    }
}

// Read file content
int read_file_content(const char* filename, char* buffer, size_t buffer_size) {
    char filepath[512];
    snprintf(filepath, sizeof(filepath), "%s/%s", STORAGE_DIR, filename);
    
    FILE* fp = fopen(filepath, "r");
    if (!fp) {
        return -1;
    }
    
    size_t n = fread(buffer, 1, buffer_size - 1, fp);
    buffer[n] = '\0';
    fclose(fp);
    
    return n;
}

// Read file content with folder support
int read_file_content_with_folder(const char* folder_path, const char* filename, char* buffer, size_t buffer_size) {
    char filepath[512];
    construct_file_path(filepath, sizeof(filepath), folder_path, filename);
    
    FILE* fp = fopen(filepath, "r");
    if (!fp) {
        return -1;
    }
    
    size_t n = fread(buffer, 1, buffer_size - 1, fp);
    buffer[n] = '\0';
    fclose(fp);
    
    return n;
}

// Write file content
int write_file_content(const char* filename, const char* content) {
    char filepath[512];
    snprintf(filepath, sizeof(filepath), "%s/%s", STORAGE_DIR, filename);
    
    FILE* fp = fopen(filepath, "w");
    if (!fp) {
        return -1;
    }
    
    fprintf(fp, "%s", content);
    fclose(fp);
    
    return 0;
}

// Write file content with folder support
int write_file_content_with_folder(const char* folder_path, const char* filename, const char* content) {
    char filepath[512];
    construct_file_path(filepath, sizeof(filepath), folder_path, filename);
    
    FILE* fp = fopen(filepath, "w");
    if (!fp) {
        return -1;
    }
    
    fprintf(fp, "%s", content);
    fclose(fp);
    
    return 0;
}

// Helper function to create nested folders recursively
int create_folder_recursive(const char* path) {
    char tmp[512];
    char* p = NULL;
    size_t len;
    
    snprintf(tmp, sizeof(tmp), "%s", path);
    len = strlen(tmp);
    
    if (tmp[len - 1] == '/') {
        tmp[len - 1] = 0;
    }
    
    for (p = tmp + 1; *p; p++) {
        if (*p == '/') {
            *p = 0;
            if (mkdir(tmp, 0755) != 0 && errno != EEXIST) {
                return -1;
            }
            *p = '/';
        }
    }
    
    if (mkdir(tmp, 0755) != 0 && errno != EEXIST) {
        return -1;
    }
    
    return 0;
}

// Create file
void create_file(const char* filename, const char* owner) {
    char filepath[512];
    snprintf(filepath, sizeof(filepath), "%s/%s", STORAGE_DIR, filename);
    
    // Ensure parent folder exists (if file is in a folder)
    char* last_slash = strrchr(filepath, '/');
    if (last_slash != NULL && last_slash != filepath) {
        char parent_dir[512];
        strncpy(parent_dir, filepath, last_slash - filepath);
        parent_dir[last_slash - filepath] = '\0';
        create_folder_recursive(parent_dir);
    }
    
    FILE* fp = fopen(filepath, "w");
    if (fp) {
        fclose(fp);
        log_message("SS", "Created file: %s (owner: %s)", filename, owner);
        log_to_file("REQUEST: CREATE file %s by %s", filename, owner);
        log_to_file("RESPONSE: CREATE file %s - SUCCESS", filename);
    }
}

// Delete file
void delete_file(const char* filename) {
    char filepath[512];
    snprintf(filepath, sizeof(filepath), "%s/%s", STORAGE_DIR, filename);
    
    log_to_file("REQUEST: DELETE file %s", filename);
    
    if (unlink(filepath) == 0) {
        log_message("SS", "Deleted file: %s", filename);
        log_to_file("RESPONSE: DELETE file %s - SUCCESS", filename);
        
        // Also delete undo file
        snprintf(filepath, sizeof(filepath), "%s/%s", UNDO_DIR, filename);
        unlink(filepath);
    } else {
        log_to_file("RESPONSE: DELETE file %s - FAILED", filename);
    }
}