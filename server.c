#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <signal.h>
#include <time.h>
#include <winsock2.h>
#include <ws2tcpip.h>
#include <windows.h>
#ifdef _MSC_VER
    #pragma comment(lib, "ws2_32.lib")
    #pragma comment(lib, "wsock32.lib")
#endif

#define PORT            8888
#define MAX_CLIENTS     100
#define BUFFER_SIZE     1024
#define USERNAME_LEN    32
#define TIME_STR_LEN    20

typedef struct ClientNode {
    SOCKET  sock;
    char    username[USERNAME_LEN];
    char    ip[INET_ADDRSTRLEN];
    struct ClientNode *next;
} ClientNode;

static ClientNode *online_clients = NULL;
static int         online_count   = 0;
static CRITICAL_SECTION cs;
static CRITICAL_SECTION g_console_cs;
static volatile int g_running = 1;
static SOCKET g_server_sock = INVALID_SOCKET;

typedef struct {
    SOCKET sock;
    char   ip[INET_ADDRSTRLEN];
} ConnInfo;

static void strip_newline(char *buf) {
    size_t len = strlen(buf);
    while (len > 0 && (buf[len - 1] == '\r' || buf[len - 1] == '\n')) {
        buf[len - 1] = '\0';
        len--;
    }
}

static void safe_strcpy(char *dst, const char *src, size_t dst_size) {
    if (dst == NULL || src == NULL || dst_size == 0) return;
    size_t src_len = strlen(src);
    size_t copy_len = (src_len < dst_size - 1) ? src_len : dst_size - 1;
    memcpy(dst, src, copy_len);
    dst[copy_len] = '\0';
}

static void get_time_str(char *out, size_t out_size) {
    time_t now = time(NULL);
    struct tm tm_buf;
    localtime_s(&tm_buf, &now);
    strftime(out, out_size, "%Y-%m-%d %H:%M:%S", &tm_buf);
}

static void server_log(const char *msg) {
    char time_str[TIME_STR_LEN];
    get_time_str(time_str, sizeof(time_str));
    EnterCriticalSection(&g_console_cs);
    printf("[%s] %s\n", time_str, msg);
    fflush(stdout);
    LeaveCriticalSection(&g_console_cs);
}

/* 下面这些函数都要求调用者已经持有 cs。*/

static int is_username_online_locked(const char *username) {
    ClientNode *curr = online_clients;
    while (curr) {
        if (strcmp(curr->username, username) == 0) return 1;
        curr = curr->next;
    }
    return 0;
}

static void add_online_client_locked(SOCKET sock, const char *username, const char *ip) {
    ClientNode *node = (ClientNode *)malloc(sizeof(ClientNode));
    if (node == NULL) return;
    node->sock = sock;
    safe_strcpy(node->username, username, USERNAME_LEN);
    safe_strcpy(node->ip, ip, sizeof(node->ip));
    node->next = online_clients;
    online_clients = node;
    online_count++;
}

static void remove_online_client_locked(SOCKET sock, char *out_username, size_t out_size) {
    ClientNode **curr = &online_clients;
    while (*curr) {
        if ((*curr)->sock == sock) {
            ClientNode *temp = *curr;
            if (out_username != NULL && out_size > 0) {
                safe_strcpy(out_username, temp->username, out_size);
            }
            *curr = (*curr)->next;
            free(temp);
            online_count--;
            return;
        }
        curr = &(*curr)->next;
    }
    if (out_username != NULL && out_size > 0) out_username[0] = '\0';
}

/* 取在线客户端快照，用于锁外广播。调用者须在返回后 free(*out_socks) 和 free(*out_names)。*/
static int snapshot_online_locked(SOCKET **out_socks, char ***out_names, SOCKET sender) {
    int cap = MAX_CLIENTS;
    SOCKET *socks = (SOCKET *)malloc(sizeof(SOCKET) * cap);
    char  **names = (char **)malloc(sizeof(char *) * cap);
    if (socks == NULL || names == NULL) {
        free(socks); free(names);
        *out_socks = NULL;
        *out_names = NULL;
        return 0;
    }

    int count = 0;
    ClientNode *curr = online_clients;
    while (curr && count < cap) {
        if (curr->sock != sender) {
            socks[count] = curr->sock;
            names[count]  = curr->username;
            count++;
        }
        curr = curr->next;
    }
    *out_socks = socks;
    *out_names = names;
    return count;
}

static void free_snapshot(SOCKET *socks, char **names) {
    free(socks);
    free(names);
}

/* 广播在锁外调用，names 留着只是为了和 snapshot 接口对齐。*/
static void broadcast_via_snapshot(const char *msg, SOCKET *socks, char **names, int count) {
    (void)names;
    for (int i = 0; i < count; i++) {
        send(socks[i], msg, (int)strlen(msg), 0);
    }
}

static DWORD WINAPI client_handler(LPVOID lpParam) {
    ConnInfo *ci = (ConnInfo *)lpParam;
    SOCKET client_sock = ci->sock;
    char client_ip[INET_ADDRSTRLEN];
    safe_strcpy(client_ip, ci->ip, sizeof(client_ip));
    free(ci);

    char buffer[BUFFER_SIZE];
    int  recv_len;
    char username[USERNAME_LEN] = {0};
    int  logged_in = 0;

    const char *welcome = "[系统] 欢迎连接到聊天室，请使用 REGISTER:你的用户名 进行注册。\n";
    send(client_sock, welcome, (int)strlen(welcome), 0);

    while (g_running && (recv_len = recv(client_sock, buffer, BUFFER_SIZE - 1, 0)) > 0) {
        buffer[recv_len] = '\0';
        strip_newline(buffer);

        if (strncmp(buffer, "REGISTER:", 9) == 0) {
            char *name = buffer + 9;
            strip_newline(name);

            if (strlen(name) == 0 || strlen(name) >= USERNAME_LEN) {
                const char *reply = "REGISTER_FAIL:用户名长度需在 1 ~ 31 之间\n";
                send(client_sock, reply, (int)strlen(reply), 0);
                char log[128];
                snprintf(log, sizeof(log), "注册被拒绝（长度不合法）: %s（来自 %s）", name, client_ip);
                server_log(log);
                continue;
            }

            EnterCriticalSection(&cs);
            if (online_count >= MAX_CLIENTS) {
                LeaveCriticalSection(&cs);
                const char *reply = "REGISTER_FAIL:聊天室已满员\n";
                send(client_sock, reply, (int)strlen(reply), 0);
                char log[128];
                snprintf(log, sizeof(log), "注册被拒绝（聊天室满）: %s（来自 %s）", name, client_ip);
                server_log(log);
            } else if (is_username_online_locked(name)) {
                const char *reply = "REGISTER_FAIL:该用户名已被在线用户使用\n";
                LeaveCriticalSection(&cs);
                send(client_sock, reply, (int)strlen(reply), 0);
                char log[128];
                snprintf(log, sizeof(log), "注册被拒绝（用户名已存在）: %s（来自 %s）", name, client_ip);
                server_log(log);
            } else {
                safe_strcpy(username, name, USERNAME_LEN);
                add_online_client_locked(client_sock, username, client_ip);
                logged_in = 1;

                SOCKET *socks; char **names; int n;
                n = snapshot_online_locked(&socks, &names, client_sock);
                LeaveCriticalSection(&cs);

                const char *ok = "REGISTER_OK\n";
                send(client_sock, ok, (int)strlen(ok), 0);

                char join_msg[BUFFER_SIZE + USERNAME_LEN];
                snprintf(join_msg, sizeof(join_msg), "[系统]: %s 加入了聊天室（当前 %d 人在线）\n", username, online_count);
                broadcast_via_snapshot(join_msg, socks, names, n);
                free_snapshot(socks, names);

                char log[256];
                snprintf(log, sizeof(log), "[OK] %s(%s) 注册并上线 - 当前 %d 人在线", username, client_ip, online_count);
                server_log(log);
            }
        }
        else if (logged_in) {
            SOCKET *socks; char **names; int n;
            EnterCriticalSection(&cs);
            n = snapshot_online_locked(&socks, &names, client_sock);
            LeaveCriticalSection(&cs);

            char msg[BUFFER_SIZE + USERNAME_LEN + 16];
            snprintf(msg, sizeof(msg), "[%s]: %s\n", username, buffer);
            broadcast_via_snapshot(msg, socks, names, n);
            free_snapshot(socks, names);

            char log[BUFFER_SIZE + USERNAME_LEN + 32];
            snprintf(log, sizeof(log), "[CHAT] [%s] 说: %s", username, buffer);
            server_log(log);
        }
        else {
            const char *hint = "请先发送 REGISTER:你的用户名 进行注册。\n";
            send(client_sock, hint, (int)strlen(hint), 0);

            char log[128];
            snprintf(log, sizeof(log), "未注册客户端消息被忽略（%s）", client_ip);
            server_log(log);
        }
    }

    char offline_name[USERNAME_LEN] = {0};
    if (logged_in) {
        SOCKET *socks; char **names; int n;
        EnterCriticalSection(&cs);
        remove_online_client_locked(client_sock, offline_name, sizeof(offline_name));
        int count_after = online_count;
        n = snapshot_online_locked(&socks, &names, INVALID_SOCKET);
        LeaveCriticalSection(&cs);

        char leave_msg[BUFFER_SIZE + USERNAME_LEN];
        snprintf(leave_msg, sizeof(leave_msg), "[系统]: %s 离开了聊天室（剩余 %d 人在线）\n",
                 offline_name, count_after);
        broadcast_via_snapshot(leave_msg, socks, names, n);
        free_snapshot(socks, names);

        char log[256];
        snprintf(log, sizeof(log), "[LEAVE] %s(%s) 下线 - 剩余 %d 人在线", offline_name, client_ip, count_after);
        server_log(log);
    } else {
        char log[128];
        snprintf(log, sizeof(log), "未注册客户端断开（%s）", client_ip);
        server_log(log);
    }

    closesocket(client_sock);
    return 0;
}

static BOOL WINAPI console_ctrl_handler(DWORD ctrl_type) {
    if (ctrl_type == CTRL_C_EVENT || ctrl_type == CTRL_BREAK_EVENT) {
        printf("\n收到退出信号，正在关闭服务端...\n");
        g_running = 0;
        if (g_server_sock != INVALID_SOCKET) {
            closesocket(g_server_sock);
            g_server_sock = INVALID_SOCKET;
        }
        return TRUE;
    }
    return FALSE;
}

int main(void) {
    WSADATA wsaData;

    if (WSAStartup(MAKEWORD(2, 2), &wsaData) != 0) {
        printf("WSAStartup 失败: %d\n", WSAGetLastError());
        return 1;
    }

    g_server_sock = socket(AF_INET, SOCK_STREAM, 0);
    if (g_server_sock == INVALID_SOCKET) {
        printf("socket 创建失败: %d\n", WSAGetLastError());
        WSACleanup();
        return 1;
    }

    BOOL reuse = TRUE;
    setsockopt(g_server_sock, SOL_SOCKET, SO_REUSEADDR, (const char *)&reuse, sizeof(reuse));

    struct sockaddr_in server_addr;
    memset(&server_addr, 0, sizeof(server_addr));
    server_addr.sin_family      = AF_INET;
    server_addr.sin_addr.s_addr = INADDR_ANY;
    server_addr.sin_port        = htons(PORT);

    if (bind(g_server_sock, (struct sockaddr *)&server_addr, sizeof(server_addr)) == SOCKET_ERROR) {
        printf("bind 失败: %d\n", WSAGetLastError());
        closesocket(g_server_sock);
        WSACleanup();
        return 1;
    }

    if (listen(g_server_sock, SOMAXCONN) == SOCKET_ERROR) {
        printf("listen 失败: %d\n", WSAGetLastError());
        closesocket(g_server_sock);
        WSACleanup();
        return 1;
    }

    SetConsoleCtrlHandler(console_ctrl_handler, TRUE);

    InitializeCriticalSection(&cs);
    InitializeCriticalSection(&g_console_cs);

    printf("========================================\n");
    printf("  简易聊天室 - 服务端启动\n");
    printf("  监听: 0.0.0.0:%d\n", PORT);
    printf("  Ctrl+C 退出\n");
    printf("========================================\n");

    while (g_running) {
        struct sockaddr_in client_addr;
        int client_addr_len = sizeof(client_addr);
        SOCKET client_sock = accept(g_server_sock,
                                   (struct sockaddr *)&client_addr,
                                   &client_addr_len);
        if (client_sock == INVALID_SOCKET) {
            if (!g_running) break;
            printf("accept 失败: %d\n", WSAGetLastError());
            continue;
        }

        char ip_str[INET_ADDRSTRLEN];
        strncpy(ip_str, inet_ntoa(client_addr.sin_addr), sizeof(ip_str) - 1);
        ip_str[sizeof(ip_str) - 1] = '\0';

        ConnInfo *ci = (ConnInfo *)malloc(sizeof(ConnInfo));
        if (ci == NULL) {
            printf("malloc 失败，拒绝连接\n");
            closesocket(client_sock);
            continue;
        }
        ci->sock = client_sock;
        safe_strcpy(ci->ip, ip_str, sizeof(ci->ip));

        char log[128];
        snprintf(log, sizeof(log), "[CONN] 新连接: %s:%d", ip_str, ntohs(client_addr.sin_port));
        server_log(log);

        HANDLE thread = CreateThread(NULL, 0, client_handler, (LPVOID)ci, 0, NULL);
        if (thread == NULL) {
            printf("创建线程失败，拒绝连接\n");
            closesocket(client_sock);
            free(ci);
        } else {
            CloseHandle(thread);
        }
    }

    printf("正在断开所有客户端...\n");
    EnterCriticalSection(&cs);
    ClientNode *curr = online_clients;
    while (curr) {
        closesocket(curr->sock);
        curr = curr->next;
    }
    curr = online_clients;
    while (curr) {
        ClientNode *next = curr->next;
        free(curr);
        curr = next;
    }
    online_clients = NULL;
    online_count   = 0;
    LeaveCriticalSection(&cs);

    DeleteCriticalSection(&cs);
    DeleteCriticalSection(&g_console_cs);
    if (g_server_sock != INVALID_SOCKET) {
        closesocket(g_server_sock);
        g_server_sock = INVALID_SOCKET;
    }
    WSACleanup();
    printf("服务端已退出。\n");
    return 0;
}
