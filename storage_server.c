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

// Save for undo
// Save for undo - with folder support
void save_for_undo_with_folder(const char* folder_path, const char* filename) {
    char src_path[512], dst_path[512];
    construct_file_path(src_path, sizeof(src_path), folder_path, filename);
    construct_file_path(dst_path, sizeof(dst_path), folder_path, filename);
    
    // Replace STORAGE_DIR with UNDO_DIR in dst_path
    snprintf(dst_path, sizeof(dst_path), "%s", src_path);
    char* storage_pos = strstr(dst_path, STORAGE_DIR);
    if (storage_pos == dst_path) {
        memmove(dst_path, dst_path + strlen(STORAGE_DIR), strlen(dst_path) - strlen(STORAGE_DIR) + 1);
        char temp[512];
        snprintf(temp, sizeof(temp), "%s%s", UNDO_DIR, dst_path);
        strcpy(dst_path, temp);
    }
    
    FILE* src = fopen(src_path, "r");
    if (!src) return;
    
    // Ensure parent directory exists for undo file
    char* last_slash = strrchr(dst_path, '/');
    if (last_slash) {
        char parent_dir[512];
        strncpy(parent_dir, dst_path, last_slash - dst_path);
        parent_dir[last_slash - dst_path] = '\0';
        create_folder_recursive(parent_dir);
    }
    
    FILE* dst = fopen(dst_path, "w");
    if (!dst) {
        fclose(src);
        return;
    }
    
    char buffer[4096];
    size_t n;
    while ((n = fread(buffer, 1, sizeof(buffer), src)) > 0) {
        fwrite(buffer, 1, n, dst);
    }
    
    fclose(src);
    fclose(dst);
}

void save_for_undo(const char* filename) {
    save_for_undo_with_folder("", filename);
}

// Asynchronously replicate file to replica storage server
void replicate_to_secondary(const char* filename, const char* replica_ip, int replica_port) {
    // This should be called in a separate thread for async replication
    char file_path[512];
    snprintf(file_path, sizeof(file_path), "%s/%s", STORAGE_DIR, filename);
    
    // Read file content
    char buffer[MAX_BUFFER_SIZE];
    FILE* f = fopen(file_path, "r");
    if (!f) {
        log_message("SS", "Failed to read file for replication: %s", filename);
        return;
    }
    
    size_t content_len = fread(buffer, 1, sizeof(buffer) - 1, f);
    buffer[content_len] = '\0';
    fclose(f);
    
    // Connect to replica server
    int replica_sock = connect_to_server(replica_ip, replica_port);
    if (replica_sock < 0) {
        log_message("SS", "Failed to connect to replica server for %s", filename);
        return;
    }
    
    // Send replication message
    Message msg;
    memset(&msg, 0, sizeof(msg));
    msg.type = MSG_SS_REPLICATE;
    strcpy(msg.filename, filename);
    memcpy(msg.data, buffer, content_len);
    msg.data_len = content_len;
    
    send_message(replica_sock, &msg);
    
    Message response;
    if (receive_message(replica_sock, &response) == 0 && response.error_code == ERR_SUCCESS) {
        log_message("SS", "Successfully replicated %s to secondary", filename);
    } else {
        log_message("SS", "Failed to replicate %s to secondary", filename);
    }
    
    close(replica_sock);
}

// Get sentence lock
SentenceLock* get_sentence_lock(const char* filename, int sentence_index) {
    pthread_mutex_lock(&locks_mutex);
    
    for (int i = 0; i < num_locks; i++) {
        if (strcmp(sentence_locks[i].filename, filename) == 0 &&
            sentence_locks[i].sentence_index == sentence_index) {
            pthread_mutex_unlock(&locks_mutex);
            return &sentence_locks[i];
        }
    }
    
    // Create new lock
    if (num_locks < MAX_FILES) {
        strcpy(sentence_locks[num_locks].filename, filename);
        sentence_locks[num_locks].sentence_index = sentence_index;
        pthread_mutex_init(&sentence_locks[num_locks].lock, NULL);
        sentence_locks[num_locks].locked_by[0] = '\0';
        num_locks++;
        
        pthread_mutex_unlock(&locks_mutex);
        return &sentence_locks[num_locks - 1];
    }
    
    pthread_mutex_unlock(&locks_mutex);
    return NULL;
}

// Trigger replication asynchronously after write
void trigger_replication(const char* filename) {
    printf("[DEBUG] trigger_replication() called for: %s\n", filename);
    fflush(stdout);
    
    char* filename_copy = strdup(filename);
    if (!filename_copy) {
        printf("[DEBUG] strdup failed!\n");
        fflush(stdout);
        return;
    }
    
    pthread_t repl_thread;
    if (pthread_create(&repl_thread, NULL, async_replicate_thread, filename_copy) == 0) {
        pthread_detach(repl_thread);
        printf("[DEBUG] ✅ Replication thread created for: %s\n", filename);
        fflush(stdout);
        log_message("SS", "🔄 Triggered async replication for %s", filename);
    } else {
        free(filename_copy);
        printf("[DEBUG] ❌ pthread_create failed for: %s\n", filename);
        fflush(stdout);
        log_message("SS", "⚠️ Failed to create replication thread for %s", filename);
    }
}

// Async thread to request replication from Name Server
void* async_replicate_thread(void* arg) {
    char* filename = (char*)arg;
    
    printf("[DEBUG] async_replicate_thread started for: %s\n", filename);
    printf("[DEBUG] Connecting to Name Server at %s:%d\n", nm_ip, nm_port);
    fflush(stdout);
    
    // Connect to Name Server
    int nm_sock = connect_to_server(nm_ip, nm_port);
    if (nm_sock < 0) {
        printf("[DEBUG] ❌ Failed to connect to NM for replication\n");
        fflush(stdout);
        log_message("SS", "Failed to connect to NM for replication of %s", filename);
        free(filename);
        return NULL;
    }
    
    printf("[DEBUG] ✅ Connected to Name Server, sending MSG_SS_REPLICATE\n");
    fflush(stdout);
    
    // Send replication request
    Message msg;
    memset(&msg, 0, sizeof(msg));
    msg.type = MSG_SS_REPLICATE;
    strcpy(msg.filename, filename);
    strcpy(msg.ss_ip, my_ip);
    msg.ss_port = nm_listen_port;
    
    send_message(nm_sock, &msg);
    
    // Wait for acknowledgment
    Message response;
    if (receive_message(nm_sock, &response) == 0) {
        if (response.error_code == ERR_SUCCESS) {
            log_message("SS", "✅ Replication request for '%s' acknowledged", filename);
        } else {
            log_message("SS", "⚠️ Replication request for '%s' failed: %s", filename, response.data);
        }
    }
    
    close(nm_sock);
    free(filename);
    return NULL;
}

// Handle replication request from Name Server (this is the SECONDARY receiving the request)
void handle_replicate_from_primary(int nm_sock, Message* msg) {
    log_message("SS", "🔄 Replication request for '%s' from primary at %s:%d", 
               msg->filename, msg->ss_ip, msg->ss_port);
    
    Message response;
    memset(&response, 0, sizeof(response));
    response.type = MSG_ACK;
    
    // Connect to primary storage server to get file content
    int primary_sock = connect_to_server(msg->ss_ip, msg->ss_port);
    if (primary_sock < 0) {
        response.error_code = ERR_CONNECTION_FAILED;
        strcpy(response.data, "Cannot connect to primary server");
        send_message(nm_sock, &response);
        log_message("SS", "❌ Failed to connect to primary %s:%d", msg->ss_ip, msg->ss_port);
        return;
    }
    
    // Request file content from primary
    Message read_msg;
    memset(&read_msg, 0, sizeof(read_msg));
    read_msg.type = MSG_READ_FILE;
    strcpy(read_msg.filename, msg->filename);
    strcpy(read_msg.username, "REPLICATION");
    
    send_message(primary_sock, &read_msg);
    
    // Receive file content
    Message file_response;
    if (receive_message(primary_sock, &file_response) != 0 || 
        file_response.error_code != ERR_SUCCESS) {
        response.error_code = ERR_FILE_NOT_FOUND;
        strcpy(response.data, "Failed to read from primary");
        send_message(nm_sock, &response);
        close(primary_sock);
        log_message("SS", "❌ Failed to read '%s' from primary", msg->filename);
        return;
    }
    
    close(primary_sock);
    
    // Write content to local file
    if (write_file_content(msg->filename, file_response.data) == 0) {
        response.error_code = ERR_SUCCESS;
        sprintf(response.data, "✓ Replicated %d bytes", file_response.data_len);
        log_message("SS", "✅ Successfully replicated '%s' (%d bytes)", 
                   msg->filename, file_response.data_len);
    } else {
        response.error_code = ERR_SERVER_ERROR;
        strcpy(response.data, "Failed to write replica");
        log_message("SS", "❌ Failed to write replica of '%s'", msg->filename);
    }
    
    send_message(nm_sock, &response);
}

// Handle READ request
void handle_read(int sock, Message* msg) {
    log_to_file("REQUEST: READ from %s for file %s", msg->username, msg->filename);
    
    char buffer[MAX_BUFFER_SIZE];
    // filename now includes path (e.g., "documents/test.txt")
    int n = read_file_content(msg->filename, buffer, sizeof(buffer));
    
    Message response;
    memset(&response, 0, sizeof(response));
    response.type = MSG_RESPONSE;
    
    if (n < 0) {
        response.error_code = ERR_FILE_NOT_FOUND;
        strcpy(response.data, "ERROR: Cannot read file");
        log_to_file("RESPONSE: READ for %s - FAILED (file not found)", msg->filename);
    } else {
        response.error_code = ERR_SUCCESS;
        strcpy(response.data, buffer);
        response.data_len = n;
        log_to_file("RESPONSE: READ for %s - SUCCESS (%d bytes)", msg->filename, n);
    }
    
    send_message(sock, &response);
}

// Handle WRITE request - Word-level editing with sentence locking
void handle_write(int sock, Message* msg) {
    Message response;
    memset(&response, 0, sizeof(response));
    response.type = MSG_RESPONSE;
    
    // Save for undo before any modifications
    save_for_undo(msg->filename);
    
    // Read the current file content
    char buffer[MAX_BUFFER_SIZE];
    int n = read_file_content(msg->filename, buffer, sizeof(buffer));
    
    if (n < 0) {
        response.error_code = ERR_FILE_NOT_FOUND;
        strcpy(response.data, "ERROR: Cannot read file");
        send_message(sock, &response);
        return;
    }
    
    // Parse file content into sentences based on delimiters (. ! ?)
    char sentences[1000][MAX_SENTENCE_LENGTH];
    int sentence_count = 0;
    parse_sentences(buffer, sentences, &sentence_count);
    
    // Get the sentence index to edit from msg->flags
    int sentence_index = msg->flags;
    
    // Special case: Empty file - allow writing to sentence 0
    if (n == 0 && sentence_index == 0) {
        // Allow - will create first sentence
    } else {
        // Check if file ends with a delimiter (allows adding new sentence)
        int ends_with_delimiter = 0;
        if (n > 0) {
            char last_char = buffer[n - 1];
            if (last_char == '.' || last_char == '!' || last_char == '?') {
                ends_with_delimiter = 1;
            }
        }
        
        // Validate sentence index
        // Can only be equal to count if file ends with delimiter (allowing new sentence)
        // Otherwise, must be < count (can only edit existing sentences)
        int max_allowed_index = ends_with_delimiter ? sentence_count : (sentence_count - 1);
        if (sentence_index < 0 || sentence_index > max_allowed_index) {
            response.error_code = ERR_INVALID_INDEX;
            if (ends_with_delimiter) {
                sprintf(response.data, "ERROR: Sentence index out of range (0-%d)", sentence_count);
            } else {
                sprintf(response.data, "ERROR: Sentence index out of range (0-%d). Last sentence has no delimiter.", sentence_count - 1);
            }
            send_message(sock, &response);
            return;
        }
    }
    
    // Get or create lock for this specific sentence
    SentenceLock* lock = get_sentence_lock(msg->filename, sentence_index);
    if (!lock) {
        response.error_code = ERR_SERVER_ERROR;
        strcpy(response.data, "ERROR: Cannot create sentence lock");
        send_message(sock, &response);
        return;
    }
    
    // Try to acquire the lock (non-blocking)
    if (pthread_mutex_trylock(&lock->lock) != 0) {
        response.error_code = ERR_SENTENCE_LOCKED;
        sprintf(response.data, "ERROR: Sentence %d is locked by %s", sentence_index, lock->locked_by);
        send_message(sock, &response);
        return;
    }
    
    // Mark who locked this sentence
    strcpy(lock->locked_by, msg->username);
    
    // Send acknowledgment that lock is acquired
    response.error_code = ERR_SUCCESS;
    strcpy(response.data, "ACK: Sentence locked. Send word updates, end with ETIRW");
    send_message(sock, &response);
    
    // Get the sentence to edit (or create new if at end)
    char working_sentence[MAX_SENTENCE_LENGTH];
    if (sentence_index < sentence_count) {
        strcpy(working_sentence, sentences[sentence_index]);
    } else {
        working_sentence[0] = '\0';  // New empty sentence
        sentence_count = sentence_index + 1;  // Expand sentence array
    }
    
    // Receive word updates in a loop until ETIRW
    while (1) {
        Message update_msg;
        if (receive_message(sock, &update_msg) < 0) {
            // Connection lost, release lock and exit
            pthread_mutex_unlock(&lock->lock);
            lock->locked_by[0] = '\0';
            return;
        }
        
        // Check for ETIRW (end write marker)
        if (strcmp(update_msg.data, "ETIRW") == 0) {
            break;
        }
        
        // Extract word_index and content from update message
        int word_index = update_msg.word_index;
        char* new_content = update_msg.data;
        
        // Parse working_sentence into words, treating delimiters as separate tokens
        // "hi bye." becomes ["hi", "bye", "."]
        char words[1000][MAX_WORD_LENGTH];
        int word_count = 0;
        
        char temp_sentence[MAX_SENTENCE_LENGTH];
        strcpy(temp_sentence, working_sentence);
        
        char* saveptr;
        char* token = strtok_r(temp_sentence, " ", &saveptr);
        while (token != NULL && word_count < 1000) {
            int len = strlen(token);
            
            // Check if token ends with a delimiter
            if (len > 1 && (token[len-1] == '.' || token[len-1] == '!' || token[len-1] == '?')) {
                // Split into word and delimiter
                char word_part[MAX_WORD_LENGTH];
                strncpy(word_part, token, len - 1);
                word_part[len - 1] = '\0';
                
                // Add the word part
                strcpy(words[word_count], word_part);
                word_count++;
                
                // Add the delimiter as separate word
                if (word_count < 1000) {
                    words[word_count][0] = token[len - 1];
                    words[word_count][1] = '\0';
                    word_count++;
                }
            } else {
                // Normal word or standalone delimiter
                strcpy(words[word_count], token);
                word_count++;
            }
            
            token = strtok_r(NULL, " ", &saveptr);
        }
        
        // Validate word_index
        if (word_index < 0 || word_index > word_count) {
            Message error_resp;
            memset(&error_resp, 0, sizeof(error_resp));
            error_resp.type = MSG_RESPONSE;
            error_resp.error_code = ERR_INVALID_INDEX;
            sprintf(error_resp.data, "ERROR: Word index %d out of range (0-%d)", word_index, word_count);
            send_message(sock, &error_resp);
            continue;
        }
        
        // Insert new_content at word_index
        // Split new_content by spaces to get individual words to insert
        char content_words[1000][MAX_WORD_LENGTH];
        int content_word_count = 0;
        
        char content_copy[MAX_BUFFER_SIZE];
        strcpy(content_copy, new_content);
        
        char* saveptr2;
        char* ct = strtok_r(content_copy, " ", &saveptr2);
        while (ct != NULL && content_word_count < 1000) {
            strcpy(content_words[content_word_count], ct);
            content_word_count++;
            ct = strtok_r(NULL, " ", &saveptr2);
        }
        
        // Build new sentence: words[0..word_index-1] + content_words + words[word_index..]
        char new_sentence[MAX_SENTENCE_LENGTH * 2];
        new_sentence[0] = '\0';
        
        // Add words before insertion point
        for (int i = 0; i < word_index; i++) {
            // Check if this word is a delimiter
            int is_delimiter = (strlen(words[i]) == 1 && 
                              (words[i][0] == '.' || words[i][0] == '!' || words[i][0] == '?'));
            
            if (!is_delimiter && strlen(new_sentence) > 0) {
                strcat(new_sentence, " ");
            }
            strcat(new_sentence, words[i]);
        }
        
        // Add new content words
        for (int i = 0; i < content_word_count; i++) {
            if (strlen(new_sentence) > 0 && 
                new_sentence[strlen(new_sentence)-1] != '.' && 
                new_sentence[strlen(new_sentence)-1] != '!' && 
                new_sentence[strlen(new_sentence)-1] != '?') {
                strcat(new_sentence, " ");
            }
            strcat(new_sentence, content_words[i]);
        }
        
        // Add remaining words after insertion point
        for (int i = word_index; i < word_count; i++) {
            // Check if this word is a delimiter
            int is_delimiter = (strlen(words[i]) == 1 && 
                              (words[i][0] == '.' || words[i][0] == '!' || words[i][0] == '?'));
            
            if (!is_delimiter && strlen(new_sentence) > 0 && 
                new_sentence[strlen(new_sentence)-1] != '.' && 
                new_sentence[strlen(new_sentence)-1] != '!' && 
                new_sentence[strlen(new_sentence)-1] != '?') {
                strcat(new_sentence, " ");
            }
            strcat(new_sentence, words[i]);
        }
        
        // Update working_sentence
        strcpy(working_sentence, new_sentence);
        
skip_word_update:  // Label for skipping invalid word updates
        // Send ACK for this word update
        Message ack;
        memset(&ack, 0, sizeof(ack));
        ack.type = MSG_RESPONSE;
        ack.error_code = ERR_SUCCESS;
        strcpy(ack.data, "ACK");
        send_message(sock, &ack);
    }
    
    // After ETIRW, check for sentence delimiters and split if needed
    char split_sentences[100][MAX_SENTENCE_LENGTH];
    int split_count = 0;
    parse_sentences(working_sentence, split_sentences, &split_count);
    
    // CRITICAL: Re-read the file to get the latest content from other concurrent writers
    // This ensures we merge our changes with any updates made by other clients
    char fresh_buffer[MAX_BUFFER_SIZE];
    int fresh_n = read_file_content(msg->filename, fresh_buffer, sizeof(fresh_buffer));
    
    // Parse the latest file content into sentences
    // Use heap allocation to avoid stack overflow
    char (*fresh_sentences)[MAX_SENTENCE_LENGTH] = malloc(1000 * sizeof(char[MAX_SENTENCE_LENGTH]));
    if (!fresh_sentences) {
        pthread_mutex_unlock(&lock->lock);
        lock->locked_by[0] = '\0';
        
        response.error_code = ERR_SERVER_ERROR;
        strcpy(response.data, "ERROR: Memory allocation failed");
        send_message(sock, &response);
        return;
    }
    
    int fresh_sentence_count = 0;
    if (fresh_n >= 0) {
        parse_sentences(fresh_buffer, fresh_sentences, &fresh_sentence_count);
    }
    
    // Now merge: replace the sentence at our index with our edited version
    // Handle sentence splitting if delimiters were added
    if (split_count > 1) {
        // Our edit created multiple sentences, need to insert them
        
        // Shift sentences after sentence_index to make room
        int shift_amount = split_count - 1;
        for (int i = fresh_sentence_count - 1; i > sentence_index; i--) {
            if (i + shift_amount < 1000) {
                strcpy(fresh_sentences[i + shift_amount], fresh_sentences[i]);
            }
        }
        
        // Insert all our split sentences
        for (int i = 0; i < split_count && (sentence_index + i) < 1000; i++) {
            strcpy(fresh_sentences[sentence_index + i], split_sentences[i]);
        }
        
        fresh_sentence_count += shift_amount;
    } else if (split_count == 1) {
        // No split, just replace the sentence at our index
        if (sentence_index < fresh_sentence_count) {
            strcpy(fresh_sentences[sentence_index], split_sentences[0]);
        } else {
            // Sentence was added at the end
            strcpy(fresh_sentences[sentence_index], split_sentences[0]);
            fresh_sentence_count = sentence_index + 1;
        }
    } else {
        // Empty sentence or no delimiters, just update
        if (sentence_index < fresh_sentence_count) {
            strcpy(fresh_sentences[sentence_index], working_sentence);
        } else {
            strcpy(fresh_sentences[sentence_index], working_sentence);
            fresh_sentence_count = sentence_index + 1;
        }
    }
    
    // Use temporary swap file approach for concurrent write safety
    char temp_filepath[512];
    snprintf(temp_filepath, sizeof(temp_filepath), "%s/%s.tmp", STORAGE_DIR, msg->filename);
    
    // Reconstruct full file content from the merged sentences
    char final_content[MAX_BUFFER_SIZE];
    reconstruct_file(fresh_sentences, fresh_sentence_count, final_content);
    
    // Free the heap-allocated array
    free(fresh_sentences);
    
    // Write to temp file first
    FILE* temp_fp = fopen(temp_filepath, "w");
    if (!temp_fp) {
        pthread_mutex_unlock(&lock->lock);
        lock->locked_by[0] = '\0';
        
        response.error_code = ERR_SERVER_ERROR;
        strcpy(response.data, "ERROR: Cannot write to temp file");
        send_message(sock, &response);
        return;
    }
    
    fprintf(temp_fp, "%s", final_content);
    fclose(temp_fp);
    
    // Atomically move temp file to actual file
    char actual_filepath[512];
    snprintf(actual_filepath, sizeof(actual_filepath), "%s/%s", STORAGE_DIR, msg->filename);
    
    if (rename(temp_filepath, actual_filepath) != 0) {
        unlink(temp_filepath);  // Clean up temp file
        pthread_mutex_unlock(&lock->lock);
        lock->locked_by[0] = '\0';
        
        response.error_code = ERR_SERVER_ERROR;
        strcpy(response.data, "ERROR: Cannot save file");
        send_message(sock, &response);
        return;
    }
    
    // Release the sentence lock
    pthread_mutex_unlock(&lock->lock);
    lock->locked_by[0] = '\0';
    
    // Send success response
    memset(&response, 0, sizeof(response));
    response.type = MSG_RESPONSE;
    response.error_code = ERR_SUCCESS;
    sprintf(response.data, "Write Successful! Sentence %d updated.", sentence_index);
    send_message(sock, &response);
    
    log_to_file("REQUEST: WRITE from %s for file %s, sentence %d, word %d", 
                msg->username, msg->filename, sentence_index, msg->word_index);
    log_to_file("RESPONSE: WRITE for %s - SUCCESS (sentence %d, %d total sentences)", 
                msg->filename, sentence_index, fresh_sentence_count);
    
    // Trigger async replication to secondary storage server
    printf("[DEBUG] About to trigger replication for: %s\n", msg->filename);
    fflush(stdout);
    trigger_replication(msg->filename);
    printf("[DEBUG] trigger_replication() call completed for: %s\n", msg->filename);
    fflush(stdout);
}

// Handle STREAM request
void handle_stream(int sock, Message* msg) {
    log_to_file("REQUEST: STREAM from %s for file %s", msg->username, msg->filename);
    
    // Allocate thread-local buffer to avoid race conditions
    char* buffer = (char*)malloc(MAX_BUFFER_SIZE);
    if (!buffer) {
        Message response;
        memset(&response, 0, sizeof(response));
        response.type = MSG_RESPONSE;
        response.error_code = ERR_SERVER_ERROR;
        strcpy(response.data, "ERROR: Memory allocation failed");
        send_message(sock, &response);
        log_to_file("RESPONSE: STREAM for %s - FAILED (memory allocation)", msg->filename);
        return;
    }
    
    // filename now includes path
    int n = read_file_content(msg->filename, buffer, MAX_BUFFER_SIZE);
    
    Message response;
    memset(&response, 0, sizeof(response));
    response.type = MSG_RESPONSE;
    
    if (n < 0) {
        response.error_code = ERR_FILE_NOT_FOUND;
        strcpy(response.data, "ERROR: Cannot read file");
        send_message(sock, &response);
        free(buffer);
        log_to_file("RESPONSE: STREAM for %s - FAILED (file not found)", msg->filename);
        return;
    }
    
    // Count words for logging
    int word_count = 0;
    
    // Send words one by one using thread-safe strtok_r
    char* saveptr;
    char* token = strtok_r(buffer, " \n\t", &saveptr);
    while (token) {
        memset(&response, 0, sizeof(response));
        response.type = MSG_RESPONSE;
        response.error_code = ERR_SUCCESS;
        
        // Copy token safely
        strncpy(response.data, token, MAX_BUFFER_SIZE - 1);
        response.data[MAX_BUFFER_SIZE - 1] = '\0';
        
        send_message(sock, &response);
        usleep(100000); // 0.1 second delay
        
        word_count++;
        token = strtok_r(NULL, " \n\t", &saveptr);
    }
    
    // Send STOP
    memset(&response, 0, sizeof(response));
    response.type = MSG_RESPONSE;
    response.error_code = ERR_SUCCESS;
    strcpy(response.data, "STOP");
    send_message(sock, &response);
    
    free(buffer);
    log_to_file("RESPONSE: STREAM for %s - SUCCESS (%d words streamed)", msg->filename, word_count);
}

// Handle UNDO request
void handle_undo(int sock, Message* msg) {
    log_to_file("REQUEST: UNDO from %s for file %s", msg->username, msg->filename);
    
    char src_path[512], dst_path[512];
    // filename now includes path (e.g., "documents/test.txt")
    snprintf(src_path, sizeof(src_path), "%s/%s", UNDO_DIR, msg->filename);
    snprintf(dst_path, sizeof(dst_path), "%s/%s", STORAGE_DIR, msg->filename);
    
    Message response;
    memset(&response, 0, sizeof(response));
    response.type = MSG_RESPONSE;
    
    FILE* src = fopen(src_path, "r");
    if (!src) {
        response.error_code = ERR_NO_UNDO_AVAILABLE;
        strcpy(response.data, "ERROR: No undo available");
        send_message(sock, &response);
        log_to_file("RESPONSE: UNDO for %s - FAILED (no backup available)", msg->filename);
        return;
    }
    
    FILE* dst = fopen(dst_path, "w");
    if (!dst) {
        fclose(src);
        response.error_code = ERR_SERVER_ERROR;
        strcpy(response.data, "ERROR: Cannot write file");
        send_message(sock, &response);
        log_to_file("RESPONSE: UNDO for %s - FAILED (cannot write)", msg->filename);
        return;
    }
    
    char buffer[4096];
    size_t n;
    size_t total_bytes = 0;
    while ((n = fread(buffer, 1, sizeof(buffer), src)) > 0) {
        fwrite(buffer, 1, n, dst);
        total_bytes += n;
    }
    
    fclose(src);
    fclose(dst);
    
    response.error_code = ERR_SUCCESS;
    strcpy(response.data, "Undo Successful!");
    send_message(sock, &response);
    
    log_to_file("RESPONSE: UNDO for %s - SUCCESS (%zu bytes restored)", msg->filename, total_bytes);
}

// Handle client request
void* handle_client_request(void* arg) {
    int client_sock = *(int*)arg;
    free(arg);
    
    Message msg;
    if (receive_message(client_sock, &msg) == 0) {
        log_message("SS", "Client request: type=%d, file=%s", msg.type, msg.filename);
        
        switch (msg.type) {
            case MSG_READ_FILE:
                handle_read(client_sock, &msg);
                break;
                
            case MSG_WRITE_FILE:
                handle_write(client_sock, &msg);
                break;
                
            case MSG_STREAM_FILE:
                handle_stream(client_sock, &msg);
                break;
                
            case MSG_UNDO_FILE:
                handle_undo(client_sock, &msg);
                break;
                
            default:
                log_message("SS", "Unknown client request: %d", msg.type);
        }
    }
    
    close(client_sock);
    return NULL;
}

// Handle NM request
void* handle_nm_request(void* arg) {
    int nm_sock = *(int*)arg;
    free(arg);
    
    Message msg;
    while (receive_message(nm_sock, &msg) == 0) {
        log_message("SS", "NM request: type=%d, file=%s", msg.type, msg.filename);
        
        Message response;
        memset(&response, 0, sizeof(response));
        response.type = MSG_ACK;
        
        switch (msg.type) {
            case MSG_SS_CREATE:
                create_file(msg.filename, msg.username);
                response.error_code = ERR_SUCCESS;
                strcpy(response.data, "File created");
                break;
                
            case MSG_SS_DELETE:
                delete_file(msg.filename);
                response.error_code = ERR_SUCCESS;
                strcpy(response.data, "File deleted");
                break;
                
            case MSG_SS_READ: {
                char buffer[MAX_BUFFER_SIZE];
                int n = read_file_content(msg.filename, buffer, sizeof(buffer));
                if (n < 0) {
                    response.error_code = ERR_FILE_NOT_FOUND;
                    strcpy(response.data, "ERROR: Cannot read file");
                } else {
                    response.error_code = ERR_SUCCESS;
                    strcpy(response.data, buffer);
                }
                break;
            }
            
            case MSG_SS_STAT: {
                char buffer[MAX_BUFFER_SIZE];
                int n = read_file_content(msg.filename, buffer, sizeof(buffer));
                if (n < 0) {
                    response.error_code = ERR_FILE_NOT_FOUND;
                    strcpy(response.data, "0 0");
                } else {
                    // Count words and characters
                    int word_count = 0;
                    int char_count = n;
                    int in_word = 0;
                    
                    for (int i = 0; i < n; i++) {
                        if (buffer[i] == ' ' || buffer[i] == '\n' || buffer[i] == '\t') {
                            in_word = 0;
                        } else {
                            if (!in_word) {
                                word_count++;
                                in_word = 1;
                            }
                        }
                    }
                    
                    response.error_code = ERR_SUCCESS;
                    sprintf(response.data, "%d %d", word_count, char_count);
                }
                break;
            }
            
            case MSG_SS_CREATE_FOLDER: {
                // Create physical folder in storage directory
                char folder_path[512];
                snprintf(folder_path, sizeof(folder_path), "%s/%s", STORAGE_DIR, msg.folder_path);
                
                if (create_folder_recursive(folder_path) == 0) {
                    response.error_code = ERR_SUCCESS;
                    sprintf(response.data, "✓ Folder created: %s", msg.folder_path);
                    log_message("SS", "Created folder: %s", folder_path);
                } else {
                    response.error_code = ERR_INVALID_COMMAND;
                    sprintf(response.data, "ERROR: Cannot create folder: %s", strerror(errno));
                    log_message("SS", "Failed to create folder %s: %s", folder_path, strerror(errno));
                }
                break;
            }
            
            case MSG_SS_MOVE_FILE: {
                // Move file: msg.filename = old full path, msg.folder_path = new full path
                char old_path[512];
                char new_path[512];
                
                // Construct old path from old filename (which may include folder)
                snprintf(old_path, sizeof(old_path), "%s/%s", STORAGE_DIR, msg.filename);
                
                // Construct new path from new filename (which includes folder)
                snprintf(new_path, sizeof(new_path), "%s/%s", STORAGE_DIR, msg.folder_path);
                
                // Ensure parent directory exists for new path
                char* last_slash = strrchr(new_path, '/');
                if (last_slash) {
                    char parent_dir[512];
                    strncpy(parent_dir, new_path, last_slash - new_path);
                    parent_dir[last_slash - new_path] = '\0';
                    create_folder_recursive(parent_dir);
                }
                
                // Use rename() to move the file atomically
                if (rename(old_path, new_path) == 0) {
                    response.error_code = ERR_SUCCESS;
                    sprintf(response.data, "✓ File moved successfully");
                    log_message("SS", "Moved file: %s -> %s", old_path, new_path);
                    
                    // Also move undo file if it exists
                    char old_undo[512], new_undo[512];
                    snprintf(old_undo, sizeof(old_undo), "%s/%s", UNDO_DIR, msg.filename);
                    snprintf(new_undo, sizeof(new_undo), "%s/%s", UNDO_DIR, msg.folder_path);
                    
                    // Ensure parent directory exists for undo file
                    last_slash = strrchr(new_undo, '/');
                    if (last_slash) {
                        char undo_parent[512];
                        strncpy(undo_parent, new_undo, last_slash - new_undo);
                        undo_parent[last_slash - new_undo] = '\0';
                        create_folder_recursive(undo_parent);
                    }
                    
                    rename(old_undo, new_undo); // Ignore errors for undo file
                } else {
                    response.error_code = ERR_INVALID_COMMAND;
                    sprintf(response.data, "ERROR: Cannot move file: %s", strerror(errno));
                    log_message("SS", "Failed to move %s to %s: %s", old_path, new_path, strerror(errno));
                }
                break;
            }
            
            case MSG_SS_CHECKPOINT: {
                char checkpoint_dir[512];
                char checkpoint_path[512];
                char file_path[512];
                
                // Create checkpoints directory: checkpoints/<filename>/
                snprintf(checkpoint_dir, sizeof(checkpoint_dir), "checkpoints/%s", msg.filename);
                create_folder_recursive(checkpoint_dir);
                
                // Checkpoint file path: checkpoints/<filename>/<tag>
                snprintf(checkpoint_path, sizeof(checkpoint_path), "%s/%s", checkpoint_dir, msg.checkpoint_tag);
                snprintf(file_path, sizeof(file_path), "%s/%s", STORAGE_DIR, msg.filename);
                
                if (msg.flags == 0) {
                    // CREATE CHECKPOINT: Copy current file to checkpoint
                    FILE* src = fopen(file_path, "r");
                    if (!src) {
                        response.error_code = ERR_FILE_NOT_FOUND;
                        strcpy(response.data, "ERROR: File not found");
                        break;
                    }
                    
                    FILE* dst = fopen(checkpoint_path, "w");
                    if (!dst) {
                        fclose(src);
                        response.error_code = ERR_SERVER_ERROR;
                        strcpy(response.data, "ERROR: Cannot create checkpoint");
                        break;
                    }
                    
                    char buffer[4096];
                    size_t n;
                    while ((n = fread(buffer, 1, sizeof(buffer), src)) > 0) {
                        fwrite(buffer, 1, n, dst);
                    }
                    
                    fclose(src);
                    fclose(dst);
                    
                    response.error_code = ERR_SUCCESS;
                    sprintf(response.data, "✓ Checkpoint '%s' created for file '%s'", 
                            msg.checkpoint_tag, msg.filename);
                    log_message("SS", "Checkpoint created: %s for %s", msg.checkpoint_tag, msg.filename);
                    
                } else if (msg.flags == 1) {
                    // VIEW CHECKPOINT: Read and return checkpoint content
                    FILE* f = fopen(checkpoint_path, "r");
                    if (!f) {
                        response.error_code = ERR_FILE_NOT_FOUND;
                        sprintf(response.data, "ERROR: Checkpoint '%s' not found", msg.checkpoint_tag);
                        break;
                    }
                    
                    size_t n = fread(response.data, 1, sizeof(response.data) - 1, f);
                    response.data[n] = '\0';
                    fclose(f);
                    
                    response.error_code = ERR_SUCCESS;
                    response.data_len = n;
                    log_message("SS", "Checkpoint viewed: %s for %s", msg.checkpoint_tag, msg.filename);
                    
                } else if (msg.flags == 2) {
                    // REVERT CHECKPOINT: Copy checkpoint back to original file
                    FILE* src = fopen(checkpoint_path, "r");
                    if (!src) {
                        response.error_code = ERR_FILE_NOT_FOUND;
                        sprintf(response.data, "ERROR: Checkpoint '%s' not found", msg.checkpoint_tag);
                        break;
                    }
                    
                    // Save current version to undo first
                    save_for_undo(msg.filename);
                    
                    FILE* dst = fopen(file_path, "w");
                    if (!dst) {
                        fclose(src);
                        response.error_code = ERR_SERVER_ERROR;
                        strcpy(response.data, "ERROR: Cannot revert file");
                        break;
                    }
                    
                    char buffer[4096];
                    size_t n;
                    while ((n = fread(buffer, 1, sizeof(buffer), src)) > 0) {
                        fwrite(buffer, 1, n, dst);
                    }
                    
                    fclose(src);
                    fclose(dst);
                    
                    response.error_code = ERR_SUCCESS;
                    sprintf(response.data, "✓ File '%s' reverted to checkpoint '%s'", 
                            msg.filename, msg.checkpoint_tag);
                    log_message("SS", "File reverted: %s to checkpoint %s", msg.filename, msg.checkpoint_tag);
                    
                } else if (msg.flags == 3) {
                    // LIST CHECKPOINTS: List all checkpoint tags for file
                    DIR* dir = opendir(checkpoint_dir);
                    if (!dir) {
                        response.error_code = ERR_SUCCESS;
                        sprintf(response.data, "─── Checkpoints for '%s' ───\n(no checkpoints)\n", msg.filename);
                        break;
                    }
                    
                    char buffer[MAX_BUFFER_SIZE];
                    int offset = 0;
                    offset += sprintf(buffer + offset, "─── Checkpoints for '%s' ───\n", msg.filename);
                    
                    struct dirent* entry;
                    int count = 0;
                    while ((entry = readdir(dir)) != NULL && offset < MAX_BUFFER_SIZE - 100) {
                        if (strcmp(entry->d_name, ".") == 0 || strcmp(entry->d_name, "..") == 0) {
                            continue;
                        }
                        
                        offset += sprintf(buffer + offset, "  • %s\n", entry->d_name);
                        count++;
                    }
                    
                    if (count == 0) {
                        offset += sprintf(buffer + offset, "(no checkpoints)\n");
                    }
                    
                    closedir(dir);
                    strcpy(response.data, buffer);
                    response.error_code = ERR_SUCCESS;
                    log_message("SS", "Checkpoints listed for %s: %d found", msg.filename, count);
                }
                break;
            }
            
            case MSG_SS_REPLICATE: {
                // This secondary server receives replication request from Name Server
                // msg.ss_ip and msg.ss_port contain primary server info
                handle_replicate_from_primary(nm_sock, &msg);
                // Response is sent inside the handler
                continue; // Skip the send_message at the end
            }
                
            default:
                log_message("SS", "Unknown NM request: %d", msg.type);
                response.error_code = ERR_INVALID_COMMAND;
        }
        
        send_message(nm_sock, &response);
    }
    
    close(nm_sock);
    return NULL;
}

// Register with Name Server
void register_with_nm() {
    log_message("SS", "Registering with Name Server at %s:%d", nm_ip, nm_port);
    
    int sock = connect_to_server(nm_ip, nm_port);
    if (sock < 0) {
        log_message("SS", "Failed to connect to Name Server");
        exit(1);
    }
    
    // Get list of files
    DIR* dir = opendir(STORAGE_DIR);
    if (!dir) {
        mkdir(STORAGE_DIR, 0755);
        dir = opendir(STORAGE_DIR);
    }
    
    char file_list[MAX_BUFFER_SIZE] = {0};
    int offset = 0;
    
    if (dir) {
        struct dirent* entry;
        while ((entry = readdir(dir)) != NULL) {
            // Skip . and .. directories
            if (strcmp(entry->d_name, ".") == 0 || strcmp(entry->d_name, "..") == 0) {
                continue;
            }
            
            // Check if it's a regular file by trying to stat it
            char filepath[512];
            snprintf(filepath, sizeof(filepath), "%s/%s", STORAGE_DIR, entry->d_name);
            struct stat st;
            if (stat(filepath, &st) == 0 && S_ISREG(st.st_mode)) {
                offset += snprintf(file_list + offset, sizeof(file_list) - offset, 
                    "%s\n", entry->d_name);
            }
        }
        closedir(dir);
    }
    
    // Send registration message
    Message msg;
    memset(&msg, 0, sizeof(msg));
    msg.type = MSG_REGISTER_SS;
    strcpy(msg.ss_ip, my_ip);  // Use actual IP instead of 127.0.0.1
    msg.ss_port = nm_listen_port;
    msg.flags = client_port; // Store client port in flags
    strcpy(msg.data, file_list);
    msg.data_len = strlen(file_list);
    
    send_message(sock, &msg);
    
    Message response;
    if (receive_message(sock, &response) == 0 && response.error_code == ERR_SUCCESS) {
        log_message("SS", "Successfully registered with Name Server");
        log_message("SS", "%s", response.data);
    } else {
        log_message("SS", "Failed to register with Name Server");
    }
    
    close(sock);
}

// Bonus: Heartbeat thread - sends periodic heartbeats to Name Server
void* heartbeat_thread(void* arg) {
    (void)arg; // Unused
    
    log_message("SS", "Heartbeat thread started");
    
    while (!should_exit) {
        sleep(10); // Send heartbeat every 10 seconds
        
        if (should_exit) break;
        
        // Connect to NM for heartbeat
        pthread_mutex_lock(&nm_sock_mutex);
        int sock = connect_to_server(nm_ip, nm_port);
        pthread_mutex_unlock(&nm_sock_mutex);
        
        if (sock < 0) {
            log_message("SS", "Failed to send heartbeat - cannot connect to NM");
            continue;
        }
        
        Message msg;
        memset(&msg, 0, sizeof(msg));
        msg.type = MSG_HEARTBEAT;
        strcpy(msg.ss_ip, my_ip);  // Use actual IP instead of 127.0.0.1
        msg.ss_port = nm_listen_port;
        
        send_message(sock, &msg);
        log_message("SS", "Heartbeat sent to Name Server");
        
        close(sock);
    }
    
    log_message("SS", "Heartbeat thread stopped");
    return NULL;
}

// Listener for NM requests (runs in separate thread)
void* nm_listener(void* arg) {
    int nm_listen_port = *(int*)arg;
    
    int nm_listen_sock = create_socket(nm_listen_port);
    if (nm_listen_sock < 0) {
        log_message("SS", "Failed to create NM listener socket");
        return NULL;
    }
    
    log_message("SS", "Listening for NM requests on port %d", nm_listen_port);
    
    // Accept NM connections
    while (1) {
        struct sockaddr_in addr;
        socklen_t addr_len = sizeof(addr);
        
        int* nm_sock = (int*)malloc(sizeof(int));
        *nm_sock = accept(nm_listen_sock, (struct sockaddr*)&addr, &addr_len);
        
        if (*nm_sock >= 0) {
            pthread_t thread;
            pthread_create(&thread, NULL, handle_nm_request, nm_sock);
            pthread_detach(thread);
        } else {
            free(nm_sock);
        }
    }
    
    return NULL;
}

int main(int argc, char* argv[]) {
    if (argc < 4) {
        printf("Usage: %s <client_port> <nm_port> <nm_ip>\n", argv[0]);
        printf("Example: %s 9000 9001 10.42.0.238\n", argv[0]);
        return 1;
    }
    
    client_port = atoi(argv[1]);
    nm_listen_port = atoi(argv[2]);
    strncpy(nm_ip, argv[3], sizeof(nm_ip) - 1);
    nm_ip[sizeof(nm_ip) - 1] = '\0';
    
    // Set unique storage directories based on client port
    snprintf(STORAGE_DIR, sizeof(STORAGE_DIR), "./storage%d", client_port);
    snprintf(UNDO_DIR, sizeof(UNDO_DIR), "./undo%d", client_port);
    
    // Get local IP address
    get_local_ip(my_ip, sizeof(my_ip));
    
    log_message("SS", "Starting Storage Server");
    log_message("SS", "My IP: %s", my_ip);
    log_message("SS", "Client port: %d, NM listen port: %d", client_port, nm_listen_port);
    log_message("SS", "Name Server IP: %s:%d", nm_ip, nm_port);
    log_message("SS", "Storage directory: %s", STORAGE_DIR);
    log_message("SS", "Undo directory: %s", UNDO_DIR);
    
    // Create directories
    mkdir(STORAGE_DIR, 0755);
    mkdir(UNDO_DIR, 0755);
    
    // Open log file (unique per storage server)
    char log_filename[256];
    snprintf(log_filename, sizeof(log_filename), "ss_log_%d.txt", client_port);
    log_file = fopen(log_filename, "a");
    if (!log_file) {
        log_message("SS", "Warning: Cannot open log file");
    }
    
    // Start NM listener thread first
    pthread_t nm_listener_thread;
    int* port_arg = malloc(sizeof(int));
    *port_arg = nm_listen_port;
    pthread_create(&nm_listener_thread, NULL, nm_listener, port_arg);
    pthread_detach(nm_listener_thread);
    
    // Wait for NM listener to start
    sleep(1);
    
    // Now register with Name Server (synchronously)
    log_message("SS", "Registering with Name Server...");
    register_with_nm();
    
    log_message("SS", "Registration complete!");
    
    // Bonus: Start heartbeat thread for fault tolerance
    pthread_t heartbeat_tid;
    if (pthread_create(&heartbeat_tid, NULL, heartbeat_thread, NULL) == 0) {
        pthread_detach(heartbeat_tid);
        log_message("SS", "Heartbeat thread started for fault tolerance");
    } else {
        log_message("SS", "Warning: Failed to start heartbeat thread");
    }
    
    // Create client listener socket
    int client_sock = create_socket(client_port);
    if (client_sock < 0) {
        log_message("SS", "Failed to create client socket");
        return 1;
    }
    
    log_message("SS", "Storage Server started, listening for clients on port %d", client_port);
    
    // Accept client connections
    while (1) {
        struct sockaddr_in addr;
        socklen_t addr_len = sizeof(addr);
        
        int* conn_sock = (int*)malloc(sizeof(int));
        *conn_sock = accept(client_sock, (struct sockaddr*)&addr, &addr_len);
        
        if (*conn_sock < 0) {
            free(conn_sock);
            continue;
        }
        
        pthread_t thread;
        pthread_create(&thread, NULL, handle_client_request, conn_sock);
        pthread_detach(thread);
    }
    
    if (log_file) {
        fclose(log_file);
    }
    
    close(client_sock);
    return 0;
}
