#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <signal.h>
#include <winsock2.h>
#include <ws2tcpip.h>
#include <windows.h>
#ifdef _MSC_VER
    #pragma comment(lib, "ws2_32.lib")
    #pragma comment(lib, "wsock32.lib")
#endif

#define PORT            8888
#define BUFFER_SIZE     1024
#define USERNAME_LEN    32

static SOCKET      g_sock = INVALID_SOCKET;
static volatile int g_running = 1;
static char        g_username[USERNAME_LEN] = {0};
static CRITICAL_SECTION g_console_cs;

static void safe_strcpy(char *dst, const char *src, size_t dst_size) {
    if (dst == NULL || src == NULL || dst_size == 0) return;
    size_t src_len = strlen(src);
    size_t copy_len = (src_len < dst_size - 1) ? src_len : dst_size - 1;
    memcpy(dst, src, copy_len);
    dst[copy_len] = '\0';
}

static void strip_newline(char *s) {
    size_t len = strlen(s);
    while (len > 0 && (s[len - 1] == '\r' || s[len - 1] == '\n')) {
        s[--len] = '\0';
    }
}

static void draw_prompt_locked(void) {
    if (g_username[0]) {
        printf("[%s]> ", g_username);
    } else {
        printf("(未注册)> ");
    }
    fflush(stdout);
}

/* 打印一行 + 重绘 prompt。持锁调用。*/
static void print_line_redraw_prompt(const char *msg) {
    EnterCriticalSection(&g_console_cs);
    fputs(msg, stdout);
    size_t len = strlen(msg);
    if (len == 0 || msg[len - 1] != '\n') fputc('\n', stdout);
    fflush(stdout);
    draw_prompt_locked();
    LeaveCriticalSection(&g_console_cs);
}

static DWORD WINAPI receive_thread(LPVOID lpParam) {
    (void)lpParam;
    char buffer[BUFFER_SIZE];
    int  recv_len;

    while (g_running && (recv_len = recv(g_sock, buffer, BUFFER_SIZE - 1, 0)) > 0) {
        buffer[recv_len] = '\0';

        /* 清当前行 -> 打消息 -> 重绘 prompt，三步原子化，两个线程不会互相打断。*/
        EnterCriticalSection(&g_console_cs);
        printf("\r%-80s\r", "");
        fputs(buffer, stdout);
        if (buffer[recv_len - 1] != '\n') fputc('\n', stdout);
        fflush(stdout);

        if (strncmp(buffer, "REGISTER_FAIL", 13) == 0) {
            g_username[0] = '\0';
        }
        draw_prompt_locked();
        LeaveCriticalSection(&g_console_cs);
    }

    if (!g_running) return 0;

    print_line_redraw_prompt("与服务器断开连接。");
    g_running = 0;
    return 0;
}

int main(int argc, char *argv[]) {
    const char *server_ip = "127.0.0.1";
    if (argc >= 2) {
        server_ip = argv[1];
    }

    printf("正在连接 %s:%d ...\n", server_ip, PORT);

    WSADATA wsaData;
    if (WSAStartup(MAKEWORD(2, 2), &wsaData) != 0) {
        printf("WSAStartup 失败: %d\n", WSAGetLastError());
        return 1;
    }

    g_sock = socket(AF_INET, SOCK_STREAM, 0);
    if (g_sock == INVALID_SOCKET) {
        printf("socket 创建失败: %d\n", WSAGetLastError());
        WSACleanup();
        return 1;
    }

    struct sockaddr_in server_addr;
    memset(&server_addr, 0, sizeof(server_addr));
    server_addr.sin_family = AF_INET;
    server_addr.sin_port   = htons(PORT);

    if (inet_pton(AF_INET, server_ip, &server_addr.sin_addr) <= 0) {
        printf("IP 地址解析失败: %s\n", server_ip);
        closesocket(g_sock);
        WSACleanup();
        return 1;
    }

    if (connect(g_sock, (struct sockaddr *)&server_addr, sizeof(server_addr)) == SOCKET_ERROR) {
        printf("连接服务器失败: %d\n", WSAGetLastError());
        closesocket(g_sock);
        WSACleanup();
        return 1;
    }

    InitializeCriticalSection(&g_console_cs);

    printf("已连接。\n");
    printf("提示：发送 REGISTER:你的用户名 来注册；/quit 退出。\n");
    printf("========================================\n");

    HANDLE recv_thread = CreateThread(NULL, 0, receive_thread, NULL, 0, NULL);
    if (recv_thread == NULL) {
        printf("创建接收线程失败: %lu\n", (unsigned long)GetLastError());
        closesocket(g_sock);
        WSACleanup();
        return 1;
    }

    print_line_redraw_prompt("");

    char input[BUFFER_SIZE];
    while (g_running) {
        /* fgets 本身是阻塞的，没法放进锁里。
         * 所以主循环只在"输入了什么需要显示"的时候才去抢锁。*/
        if (fgets(input, BUFFER_SIZE, stdin) == NULL) {
            break;
        }
        strip_newline(input);
        if (input[0] == '\0') {
            print_line_redraw_prompt("");
            continue;
        }

        if (strcmp(input, "/quit") == 0 || strcmp(input, "/exit") == 0) {
            break;
        }

        /* 先假设注册成功，设 g_username。
         * 如果服务端回了 REGISTER_FAIL，接收线程会把它清空。*/
        if (strncmp(input, "REGISTER:", 9) == 0) {
            char *name = input + 9;
            strip_newline(name);
            if (strlen(name) > 0 && strlen(name) < USERNAME_LEN) {
                safe_strcpy(g_username, name, USERNAME_LEN);
            }
        }

        if (send(g_sock, input, (int)strlen(input), 0) == SOCKET_ERROR) {
            print_line_redraw_prompt("发送失败，连接可能已断开。");
            break;
        }

        print_line_redraw_prompt("");
    }

    EnterCriticalSection(&g_console_cs);
    printf("正在退出...\n");
    fflush(stdout);
    LeaveCriticalSection(&g_console_cs);

    g_running = 0;
    if (g_sock != INVALID_SOCKET) {
        shutdown(g_sock, SD_BOTH);
        closesocket(g_sock);
        g_sock = INVALID_SOCKET;
    }

    WaitForSingleObject(recv_thread, 2000);
    CloseHandle(recv_thread);
    DeleteCriticalSection(&g_console_cs);
    WSACleanup();
    printf("再见！\n");
    return 0;
}
