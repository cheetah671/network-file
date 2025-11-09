#include "common.h"
// This file contains bonus feature handlers to be integrated into name_server.c

// Handle CREATEFOLDER command
void handle_create_folder(int client_sock, Message* msg) {
    pthread_mutex_lock(&data_mutex);
    
    Message response;
    memset(&response, 0, sizeof(response));
    response.type = MSG_RESPONSE;
    
    // Check if folder already exists
    FolderNode* current = folder_list;
    while (current) {
        if (strcmp(current->foldername, msg->folder_path) == 0) {
            response.error_code = ERR_FILE_EXISTS;
            sprintf(response.data, "ERROR: Folder '%s' already exists", msg->folder_path);
            pthread_mutex_unlock(&data_mutex);
            send_message(client_sock, &response);
            return;
        }
        current = current->next;
    }
    
    // Create new folder
    FolderNode* new_folder = (FolderNode*)malloc(sizeof(FolderNode));
    strcpy(new_folder->foldername, msg->folder_path);
    strcpy(new_folder->owner, msg->username);
    time(&new_folder->created);
    new_folder->next = folder_list;
    folder_list = new_folder;
    
    response.error_code = ERR_SUCCESS;
    sprintf(response.data, "✓ Folder '%s' created successfully!", msg->folder_path);
    
    pthread_mutex_unlock(&data_mutex);
    send_message(client_sock, &response);
    log_to_file("CREATEFOLDER: %s by %s", msg->folder_path, msg->username);
}

// Handle MOVE command
void handle_move_file(int client_sock, Message* msg) {
    pthread_mutex_lock(&data_mutex);
    
    FileNode* file = find_file(msg->filename);
    
    Message response;
    memset(&response, 0, sizeof(response));
    response.type = MSG_RESPONSE;
    
    if (!file) {
        response.error_code = ERR_FILE_NOT_FOUND;
        sprintf(response.data, "ERROR: File '%s' not found", msg->filename);
    } else if (strcmp(file->metadata.owner, msg->username) != 0) {
        response.error_code = ERR_UNAUTHORIZED;
        strcpy(response.data, "ERROR: Only owner can move files");
    } else {
        // Check if folder exists
        FolderNode* folder = folder_list;
        int folder_found = 0;
        while (folder) {
            if (strcmp(folder->foldername, msg->folder_path) == 0) {
                folder_found = 1;
                break;
            }
            folder = folder->next;
        }
        
        if (!folder_found && strcmp(msg->folder_path, "/") != 0) {
            response.error_code = ERR_FILE_NOT_FOUND;
            sprintf(response.data, "ERROR: Folder '%s' not found", msg->folder_path);
        } else {
            strcpy(file->metadata.folder_path, msg->folder_path);
            response.error_code = ERR_SUCCESS;
            sprintf(response.data, "✓ File '%s' moved to folder '%s'", msg->filename, msg->folder_path);
            save_metadata();
        }
    }
    
    pthread_mutex_unlock(&data_mutex);
    send_message(client_sock, &response);
    log_to_file("MOVE: %s to %s by %s", msg->filename, msg->folder_path, msg->username);
}

// Handle VIEWFOLDER command
void handle_view_folder(int client_sock, Message* msg) {
    pthread_mutex_lock(&data_mutex);
    
    Message response;
    memset(&response, 0, sizeof(response));
    response.type = MSG_RESPONSE;
    response.error_code = ERR_SUCCESS;
    
    char buffer[MAX_BUFFER_SIZE] = {0};
    int offset = 0;
    
    offset += sprintf(buffer + offset, "─── Files in folder '%s' ───\n", msg->folder_path);
    
    FileNode* current = file_list;
    int count = 0;
    while (current && offset < MAX_BUFFER_SIZE - 256) {
        if (strcmp(current->metadata.folder_path, msg->folder_path) == 0) {
            int access = get_user_access(current, msg->username);
            if (access != ACCESS_NONE) {
                offset += sprintf(buffer + offset, "  • %s (owner: %s)\n", 
                    current->metadata.filename, current->metadata.owner);
                count++;
            }
        }
        current = current->next;
    }
    
    if (count == 0) {
        offset += sprintf(buffer + offset, "  (empty)\n");
    }
    
    strcpy(response.data, buffer);
    response.data_len = strlen(buffer);
    
    pthread_mutex_unlock(&data_mutex);
    send_message(client_sock, &response);
    log_to_file("VIEWFOLDER: %s by %s", msg->folder_path, msg->username);
}

// ═══════════════════════════════════════════════════════════════════
// BONUS: CHECKPOINTS (15 marks)
// ═══════════════════════════════════════════════════════════════════

// Handle CHECKPOINT command
void handle_checkpoint(int client_sock, Message* msg) {
    pthread_mutex_lock(&data_mutex);
    
    FileNode* file = find_file(msg->filename);
    
    Message response;
    memset(&response, 0, sizeof(response));
    response.type = MSG_RESPONSE;
    
    if (!file) {
        response.error_code = ERR_FILE_NOT_FOUND;
        sprintf(response.data, "ERROR: File '%s' not found", msg->filename);
        pthread_mutex_unlock(&data_mutex);
        send_message(client_sock, &response);
        return;
    }
    
    int access = get_user_access(file, msg->username);
    if ((access & ACCESS_WRITE) == 0) {
        response.error_code = ERR_UNAUTHORIZED;
        strcpy(response.data, "ERROR: Write access required to create checkpoint");
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
    ss_msg.type = MSG_SS_CHECKPOINT;
    strcpy(ss_msg.filename, msg->filename);
    strcpy(ss_msg.checkpoint_tag, msg->checkpoint_tag);
    ss_msg.flags = 1; // Create checkpoint
    
    send_message(ss_sock, &ss_msg);
    
    Message ss_response;
    if (receive_message(ss_sock, &ss_response) == 0 && ss_response.error_code == ERR_SUCCESS) {
        // Store checkpoint metadata
        pthread_mutex_lock(&data_mutex);
        if (num_checkpoints < MAX_FILES * 10) {
            strcpy(checkpoints[num_checkpoints].filename, msg->filename);
            strcpy(checkpoints[num_checkpoints].tag, msg->checkpoint_tag);
            strcpy(checkpoints[num_checkpoints].content, ss_response.data);
            time(&checkpoints[num_checkpoints].created);
            num_checkpoints++;
        }
        pthread_mutex_unlock(&data_mutex);
        
        response.error_code = ERR_SUCCESS;
        sprintf(response.data, "✓ Checkpoint '%s' created for file '%s'", 
            msg->checkpoint_tag, msg->filename);
    } else {
        response.error_code = ERR_SERVER_ERROR;
        strcpy(response.data, "ERROR: Failed to create checkpoint");
    }
    
    close(ss_sock);
    send_message(client_sock, &response);
    log_to_file("CHECKPOINT: %s tag=%s by %s", msg->filename, msg->checkpoint_tag, msg->username);
}

// Handle VIEWCHECKPOINT command
void handle_view_checkpoint(int client_sock, Message* msg) {
    pthread_mutex_lock(&data_mutex);
    
    Message response;
    memset(&response, 0, sizeof(response));
    response.type = MSG_RESPONSE;
    
    // Find checkpoint
    int found = 0;
    for (int i = 0; i < num_checkpoints; i++) {
        if (strcmp(checkpoints[i].filename, msg->filename) == 0 &&
            strcmp(checkpoints[i].tag, msg->checkpoint_tag) == 0) {
            response.error_code = ERR_SUCCESS;
            strcpy(response.data, checkpoints[i].content);
            response.data_len = strlen(checkpoints[i].content);
            found = 1;
            break;
        }
    }
    
    if (!found) {
        response.error_code = ERR_FILE_NOT_FOUND;
        sprintf(response.data, "ERROR: Checkpoint '%s' not found for file '%s'", 
            msg->checkpoint_tag, msg->filename);
    }
    
    pthread_mutex_unlock(&data_mutex);
    send_message(client_sock, &response);
    log_to_file("VIEWCHECKPOINT: %s tag=%s by %s", msg->filename, msg->checkpoint_tag, msg->username);
}

// Handle REVERT command
void handle_revert_checkpoint(int client_sock, Message* msg) {
    pthread_mutex_lock(&data_mutex);
    
    FileNode* file = find_file(msg->filename);
    
    Message response;
    memset(&response, 0, sizeof(response));
    response.type = MSG_RESPONSE;
    
    if (!file) {
        response.error_code = ERR_FILE_NOT_FOUND;
        sprintf(response.data, "ERROR: File '%s' not found", msg->filename);
        pthread_mutex_unlock(&data_mutex);
        send_message(client_sock, &response);
        return;
    }
    
    int access = get_user_access(file, msg->username);
    if ((access & ACCESS_WRITE) == 0) {
        response.error_code = ERR_UNAUTHORIZED;
        strcpy(response.data, "ERROR: Write access required to revert");
        pthread_mutex_unlock(&data_mutex);
        send_message(client_sock, &response);
        return;
    }
    
    // Find checkpoint
    char checkpoint_content[MAX_BUFFER_SIZE] = {0};
    int found = 0;
    for (int i = 0; i < num_checkpoints; i++) {
        if (strcmp(checkpoints[i].filename, msg->filename) == 0 &&
            strcmp(checkpoints[i].tag, msg->checkpoint_tag) == 0) {
            strcpy(checkpoint_content, checkpoints[i].content);
            found = 1;
            break;
        }
    }
    
    if (!found) {
        response.error_code = ERR_FILE_NOT_FOUND;
        sprintf(response.data, "ERROR: Checkpoint '%s' not found", msg->checkpoint_tag);
        pthread_mutex_unlock(&data_mutex);
        send_message(client_sock, &response);
        return;
    }
    
    int ss_index = file->metadata.ss_index;
    pthread_mutex_unlock(&data_mutex);
    
    // Restore checkpoint content to file
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
    ss_msg.type = MSG_SS_CHECKPOINT;
    strcpy(ss_msg.filename, msg->filename);
    strcpy(ss_msg.checkpoint_tag, msg->checkpoint_tag);
    strcpy(ss_msg.data, checkpoint_content);
    ss_msg.flags = 2; // Revert to checkpoint
    
    send_message(ss_sock, &ss_msg);
    
    Message ss_response;
    if (receive_message(ss_sock, &ss_response) == 0 && ss_response.error_code == ERR_SUCCESS) {
        response.error_code = ERR_SUCCESS;
        sprintf(response.data, "✓ File '%s' reverted to checkpoint '%s'", 
            msg->filename, msg->checkpoint_tag);
    } else {
        response.error_code = ERR_SERVER_ERROR;
        strcpy(response.data, "ERROR: Failed to revert to checkpoint");
    }
    
    close(ss_sock);
    send_message(client_sock, &response);
    log_to_file("REVERT: %s to tag=%s by %s", msg->filename, msg->checkpoint_tag, msg->username);
}

// Handle LISTCHECKPOINTS command
void handle_list_checkpoints(int client_sock, Message* msg) {
    pthread_mutex_lock(&data_mutex);
    
    Message response;
    memset(&response, 0, sizeof(response));
    response.type = MSG_RESPONSE;
    response.error_code = ERR_SUCCESS;
    
    char buffer[MAX_BUFFER_SIZE] = {0};
    int offset = 0;
    
    offset += sprintf(buffer + offset, "─── Checkpoints for '%s' ───\n", msg->filename);
    
    int count = 0;
    for (int i = 0; i < num_checkpoints; i++) {
        if (strcmp(checkpoints[i].filename, msg->filename) == 0) {
            char time_str[64];
            format_time(checkpoints[i].created, time_str, sizeof(time_str));
            offset += sprintf(buffer + offset, "  • Tag: %s (Created: %s)\n", 
                checkpoints[i].tag, time_str);
            count++;
        }
    }
    
    if (count == 0) {
        offset += sprintf(buffer + offset, "  (no checkpoints)\n");
    }
    
    strcpy(response.data, buffer);
    response.data_len = strlen(buffer);
    
    pthread_mutex_unlock(&data_mutex);
    send_message(client_sock, &response);
    log_to_file("LISTCHECKPOINTS: %s by %s", msg->filename, msg->username);
}

// ═══════════════════════════════════════════════════════════════════
// BONUS: ACCESS REQUESTS (5 marks)
// ═══════════════════════════════════════════════════════════════════

// Handle REQUESTACCESS command
void handle_request_access(int client_sock, Message* msg) {
    pthread_mutex_lock(&data_mutex);
    
    FileNode* file = find_file(msg->filename);
    
    Message response;
    memset(&response, 0, sizeof(response));
    response.type = MSG_RESPONSE;
    
    if (!file) {
        response.error_code = ERR_FILE_NOT_FOUND;
        sprintf(response.data, "ERROR: File '%s' not found", msg->filename);
    } else if (strcmp(file->metadata.owner, msg->username) == 0) {
        response.error_code = ERR_INVALID_COMMAND;
        strcpy(response.data, "ERROR: You already own this file");
    } else {
        // Check if request already exists
        int exists = 0;
        for (int i = 0; i < num_access_requests; i++) {
            if (strcmp(access_requests[i].filename, msg->filename) == 0 &&
                strcmp(access_requests[i].requester, msg->username) == 0) {
                exists = 1;
                break;
            }
        }
        
        if (exists) {
            response.error_code = ERR_FILE_EXISTS;
            strcpy(response.data, "ERROR: Access request already pending");
        } else if (num_access_requests < MAX_CLIENTS * 10) {
            strcpy(access_requests[num_access_requests].filename, msg->filename);
            strcpy(access_requests[num_access_requests].requester, msg->username);
            access_requests[num_access_requests].requested_rights = msg->flags;
            time(&access_requests[num_access_requests].request_time);
            num_access_requests++;
            
            response.error_code = ERR_SUCCESS;
            sprintf(response.data, "✓ Access request sent to owner of '%s'", msg->filename);
        } else {
            response.error_code = ERR_SERVER_ERROR;
            strcpy(response.data, "ERROR: Too many pending requests");
        }
    }
    
    pthread_mutex_unlock(&data_mutex);
    send_message(client_sock, &response);
    log_to_file("REQUESTACCESS: %s for %s", msg->username, msg->filename);
}

// Handle VIEWREQUESTS command
void handle_view_requests(int client_sock, Message* msg) {
    pthread_mutex_lock(&data_mutex);
    
    Message response;
    memset(&response, 0, sizeof(response));
    response.type = MSG_RESPONSE;
    response.error_code = ERR_SUCCESS;
    
    char buffer[MAX_BUFFER_SIZE] = {0};
    int offset = 0;
    
    offset += sprintf(buffer + offset, "─── Pending Access Requests ───\n");
    
    int count = 0;
    for (int i = 0; i < num_access_requests; i++) {
        // Find file and check if user is owner
        FileNode* file = find_file(access_requests[i].filename);
        if (file && strcmp(file->metadata.owner, msg->username) == 0) {
            char time_str[64];
            format_time(access_requests[i].request_time, time_str, sizeof(time_str));
            offset += sprintf(buffer + offset, "  • User '%s' requests %s access to '%s' (at %s)\n", 
                access_requests[i].requester,
                (access_requests[i].requested_rights == ACCESS_READ) ? "READ" : "WRITE",
                access_requests[i].filename,
                time_str);
            count++;
        }
    }
    
    if (count == 0) {
        offset += sprintf(buffer + offset, "  (no pending requests)\n");
    }
    
    strcpy(response.data, buffer);
    response.data_len = strlen(buffer);
    
    pthread_mutex_unlock(&data_mutex);
    send_message(client_sock, &response);
    log_to_file("VIEWREQUESTS by %s", msg->username);
}

// Handle APPROVEREQUEST command
void handle_approve_request(int client_sock, Message* msg) {
    pthread_mutex_lock(&data_mutex);
    
    FileNode* file = find_file(msg->filename);
    
    Message response;
    memset(&response, 0, sizeof(response));
    response.type = MSG_RESPONSE;
    
    if (!file) {
        response.error_code = ERR_FILE_NOT_FOUND;
        sprintf(response.data, "ERROR: File '%s' not found", msg->filename);
    } else if (strcmp(file->metadata.owner, msg->username) != 0) {
        response.error_code = ERR_UNAUTHORIZED;
        strcpy(response.data, "ERROR: Only owner can approve requests");
    } else {
        // Find and remove request
        int found = -1;
        int requested_rights = ACCESS_NONE;
        for (int i = 0; i < num_access_requests; i++) {
            if (strcmp(access_requests[i].filename, msg->filename) == 0 &&
                strcmp(access_requests[i].requester, msg->target_user) == 0) {
                found = i;
                requested_rights = access_requests[i].requested_rights;
                break;
            }
        }
        
        if (found < 0) {
            response.error_code = ERR_FILE_NOT_FOUND;
            strcpy(response.data, "ERROR: No such access request found");
        } else {
            // Grant access
            int user_exists = 0;
            for (int i = 0; i < file->access_count; i++) {
                if (strcmp(file->access_list[i].username, msg->target_user) == 0) {
                    file->access_list[i].access_rights = requested_rights;
                    user_exists = 1;
                    break;
                }
            }
            
            if (!user_exists) {
                file->access_list = (UserAccess*)realloc(file->access_list, 
                    (file->access_count + 1) * sizeof(UserAccess));
                strcpy(file->access_list[file->access_count].username, msg->target_user);
                file->access_list[file->access_count].access_rights = requested_rights;
                file->access_count++;
            }
            
            // Remove request
            for (int i = found; i < num_access_requests - 1; i++) {
                access_requests[i] = access_requests[i + 1];
            }
            num_access_requests--;
            
            response.error_code = ERR_SUCCESS;
            sprintf(response.data, "✓ Access granted to '%s' for file '%s'", 
                msg->target_user, msg->filename);
            save_metadata();
        }
    }
    
    pthread_mutex_unlock(&data_mutex);
    send_message(client_sock, &response);
    log_to_file("APPROVEREQUEST: %s approved %s for %s", 
        msg->username, msg->target_user, msg->filename);
}

// Handle DENYREQUEST command
void handle_deny_request(int client_sock, Message* msg) {
    pthread_mutex_lock(&data_mutex);
    
    FileNode* file = find_file(msg->filename);
    
    Message response;
    memset(&response, 0, sizeof(response));
    response.type = MSG_RESPONSE;
    
    if (!file) {
        response.error_code = ERR_FILE_NOT_FOUND;
        sprintf(response.data, "ERROR: File '%s' not found", msg->filename);
    } else if (strcmp(file->metadata.owner, msg->username) != 0) {
        response.error_code = ERR_UNAUTHORIZED;
        strcpy(response.data, "ERROR: Only owner can deny requests");
    } else {
        // Find and remove request
        int found = -1;
        for (int i = 0; i < num_access_requests; i++) {
            if (strcmp(access_requests[i].filename, msg->filename) == 0 &&
                strcmp(access_requests[i].requester, msg->target_user) == 0) {
                found = i;
                break;
            }
        }
        
        if (found < 0) {
            response.error_code = ERR_FILE_NOT_FOUND;
            strcpy(response.data, "ERROR: No such access request found");
        } else {
            // Remove request
            for (int i = found; i < num_access_requests - 1; i++) {
                access_requests[i] = access_requests[i + 1];
            }
            num_access_requests--;
            
            response.error_code = ERR_SUCCESS;
            sprintf(response.data, "✓ Access request denied for '%s'", msg->target_user);
        }
    }
    
    pthread_mutex_unlock(&data_mutex);
    send_message(client_sock, &response);
    log_to_file("DENYREQUEST: %s denied %s for %s", 
        msg->username, msg->target_user, msg->filename);
}


// Handle SEARCH command
void handle_search(int client_sock, Message* msg) {
    pthread_mutex_lock(&data_mutex);
    
    Message response;
    memset(&response, 0, sizeof(response));
    response.type = MSG_RESPONSE;
    response.error_code = ERR_SUCCESS;
    
    char buffer[MAX_BUFFER_SIZE] = {0};
    int offset = 0;
    
    offset += sprintf(buffer + offset, "─── Search Results for '%s' ───\n", msg->data);
    
    FileNode* current = file_list;
    int count = 0;
    while (current && offset < MAX_BUFFER_SIZE - 256) {
        // Search in filename
        if (strstr(current->metadata.filename, msg->data) != NULL) {
            int access = get_user_access(current, msg->username);
            if (access != ACCESS_NONE) {
                offset += sprintf(buffer + offset, "  • %s (owner: %s, folder: %s)\n", 
                    current->metadata.filename, 
                    current->metadata.owner,
                    current->metadata.folder_path[0] ? current->metadata.folder_path : "/");
                count++;
            }
        }
        current = current->next;
    }
    
    if (count == 0) {
        offset += sprintf(buffer + offset, "  (no files found)\n");
    } else {
        offset += sprintf(buffer + offset, "Total: %d file(s) found\n", count);
    }
    
    strcpy(response.data, buffer);
    response.data_len = strlen(buffer);
    
    pthread_mutex_unlock(&data_mutex);
    send_message(client_sock, &response);
    log_to_file("SEARCH: '%s' by %s", msg->data, msg->username);
}

// Handle METRICS command
void handle_metrics(int client_sock, Message* msg) {
    pthread_mutex_lock(&data_mutex);
    
    Message response;
    memset(&response, 0, sizeof(response));
    response.type = MSG_RESPONSE;
    response.error_code = ERR_SUCCESS;
    
    char buffer[MAX_BUFFER_SIZE] = {0};
    int offset = 0;
    
    time_t now;
    time(&now);
    int uptime = (int)difftime(now, metrics.start_time);
    
    offset += sprintf(buffer + offset, 
        "╔═══════════════════════════════════════════════════════╗\n"
        "║            DISTRIBUTED FILE SYSTEM METRICS            ║\n"
        "╠═══════════════════════════════════════════════════════╣\n");
    
    offset += sprintf(buffer + offset, "║ System Uptime:           %d seconds                \n", uptime);
    offset += sprintf(buffer + offset, "║ Active Storage Servers:  %d                        \n", num_storage_servers);
    offset += sprintf(buffer + offset, "║ Connected Clients:       %d                        \n", num_clients);
    offset += sprintf(buffer + offset, "║ Total Files:             %d                        \n", 
        count_files());
    offset += sprintf(buffer + offset, "║ Total Folders:           %d                        \n", 
        count_folders());
    offset += sprintf(buffer + offset, "╠═══════════════════════════════════════════════════════╣\n");
    offset += sprintf(buffer + offset, "║ Operations Count:                                    ║\n");
    offset += sprintf(buffer + offset, "║   • Reads:               %d                        \n", metrics.total_reads);
    offset += sprintf(buffer + offset, "║   • Writes:              %d                        \n", metrics.total_writes);
    offset += sprintf(buffer + offset, "║   • Creates:             %d                        \n", metrics.total_creates);
    offset += sprintf(buffer + offset, "║   • Deletes:             %d                        \n", metrics.total_deletes);
    offset += sprintf(buffer + offset, "╠═══════════════════════════════════════════════════════╣\n");
    offset += sprintf(buffer + offset, "║ Checkpoints:             %d                        \n", num_checkpoints);
    offset += sprintf(buffer + offset, "║ Pending Access Requests: %d                        \n", num_access_requests);
    offset += sprintf(buffer + offset, 
        "╚═══════════════════════════════════════════════════════╝\n");
    
    strcpy(response.data, buffer);
    response.data_len = strlen(buffer);
    
    pthread_mutex_unlock(&data_mutex);
    send_message(client_sock, &response);
    log_to_file("METRICS viewed by %s", msg->username);
}

// Helper: Count files
int count_files() {
    int count = 0;
    FileNode* current = file_list;
    while (current) {
        count++;
        current = current->next;
    }
    return count;
}

// Helper: Count folders
int count_folders() {
    int count = 0;
    FolderNode* current = folder_list;
    while (current) {
        count++;
        current = current->next;
    }
    return count;
}
