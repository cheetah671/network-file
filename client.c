#include "common.h"

#define NM_PORT 8080

char username[MAX_USERNAME];
char nm_ip[50];  // Store the Name Server IP
int nm_sock = -1;

// Function prototypes
void connect_to_nm();
void print_menu();
void handle_command(const char* command);
void cmd_view(const char* args);
void cmd_read(const char* filename);
void cmd_create(const char* filename);
void cmd_write(const char* filename, int sentence_num);
void cmd_delete(const char* filename);
void cmd_info(const char* filename);
void cmd_stream(const char* filename);
void cmd_list();
void cmd_add_access(const char* flag, const char* filename, const char* target_user);
void cmd_rem_access(const char* filename, const char* target_user);
void cmd_exec(const char* filename);
void cmd_undo(const char* filename);
// Bonus: Folder operations
void cmd_create_folder(const char* foldername);
void cmd_move_file(const char* filename, const char* foldername);
void cmd_view_folder(const char* foldername);
// Bonus: Checkpoint operations
void cmd_checkpoint(const char* filename, const char* tag);
void cmd_view_checkpoint(const char* filename, const char* tag);
void cmd_revert_checkpoint(const char* filename, const char* tag);
void cmd_list_checkpoints(const char* filename);
// Bonus: Access request operations
void cmd_request_access(const char* filename, const char* flag);
void cmd_view_requests();
void cmd_approve_request(const char* requester, const char* filename);
void cmd_deny_request(const char* requester, const char* filename);

// Connect to Name Server
void connect_to_nm() {
    nm_sock = connect_to_server(nm_ip, NM_PORT);
    if (nm_sock < 0) {
        printf("ERROR: Cannot connect to Name Server at %s:%d\n", nm_ip, NM_PORT);
        exit(1);
    }
    
    // Register client
    Message msg;
    memset(&msg, 0, sizeof(msg));
    msg.type = MSG_REGISTER_CLIENT;
    strcpy(msg.username, username);
    
    send_message(nm_sock, &msg);
    
    Message response;
    if (receive_message(nm_sock, &response) == 0 && response.error_code == ERR_SUCCESS) {
        printf("✓ Connected to Name Server\n");
        printf("✓ Registered as user: %s\n\n", username);
    } else {
        printf("ERROR: Failed to register with Name Server\n");
        close(nm_sock);
        exit(1);
    }
}

// Print menu
void print_menu() {
    printf("\n═══════════════════════════════════════════════════════════\n");
    printf("                  DISTRIBUTED FILE SYSTEM                  \n");
    printf("═══════════════════════════════════════════════════════════\n");
    printf("Basic Commands:\n");
    printf("  VIEW [-a] [-l] [-al]      - List files\n");
    printf("  READ <filename>           - Read file content\n");
    printf("  CREATE <filename>         - Create new file\n");
    printf("  WRITE <filename> <sent#>  - Write to file\n");
    printf("  DELETE <filename>         - Delete file\n");
    printf("  INFO <filename>           - Get file information\n");
    printf("  STREAM <filename>         - Stream file content\n");
    printf("  LIST                      - List all users\n");
    printf("  ADDACCESS -R/-W <file> <user> - Grant access\n");
    printf("  REMACCESS <file> <user>   - Remove access\n");
    printf("  EXEC <filename>           - Execute file as commands\n");
    printf("  UNDO <filename>           - Undo last change\n");
    printf("───────────────────────────────────────────────────────────\n");
    printf("Bonus - Folders:\n");
    printf("  CREATEFOLDER <folder>     - Create new folder\n");
    printf("  MOVE <file> <folder>      - Move file to folder\n");
    printf("  VIEWFOLDER <folder>       - List files in folder\n");
    printf("───────────────────────────────────────────────────────────\n");
    printf("Bonus - Checkpoints:\n");
    printf("  CHECKPOINT <file> <tag>   - Create checkpoint\n");
    printf("  VIEWCHECKPOINT <file> <tag> - View checkpoint content\n");
    printf("  REVERT <file> <tag>       - Revert to checkpoint\n");
    printf("  LISTCHECKPOINTS <file>    - List all checkpoints\n");
    printf("───────────────────────────────────────────────────────────\n");
    printf("Bonus - Access Requests:\n");
    printf("  REQUESTACCESS -R/-W <file> - Request file access\n");
    printf("  VIEWREQUESTS              - View pending requests (owner)\n");
    printf("  APPROVEREQUEST <user> <file> - Approve request\n");
    printf("  DENYREQUEST <user> <file> - Deny request\n");
    printf("───────────────────────────────────────────────────────────\n");
    printf("  HELP                      - Show this menu\n");
    printf("  EXIT                      - Exit client\n");
    printf("═══════════════════════════════════════════════════════════\n\n");
}
