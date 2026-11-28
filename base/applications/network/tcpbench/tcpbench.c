#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <winsock2.h>
#include <windows.h>

#define BUFSZ 65536

static double now_ms(void)
{
    LARGE_INTEGER f, c;
    QueryPerformanceFrequency(&f);
    QueryPerformanceCounter(&c);
    return (double)c.QuadPart * 1000.0 / (double)f.QuadPart;
}

static int run_server(unsigned short port)
{
    SOCKET ls = socket(AF_INET, SOCK_STREAM, 0);
    struct sockaddr_in a;
    memset(&a, 0, sizeof(a));
    a.sin_family = AF_INET;
    a.sin_port = htons(port);
    a.sin_addr.s_addr = INADDR_ANY;
    if (bind(ls, (struct sockaddr *)&a, sizeof(a)) || listen(ls, 4))
    {
        printf("bind/listen failed %d\n", WSAGetLastError());
        return 1;
    }
    printf("server ready\n");
    fflush(stdout);
    for (;;)
    {
        SOCKET s = accept(ls, NULL, NULL);
        if (s == INVALID_SOCKET) continue;
        char *buf = malloc(BUFSZ);
        for (;;)
        {
            int r = recv(s, buf, BUFSZ, 0);
            if (r <= 0) break;
            /* echo first byte back as a liveness ping-pong when asked */
            if (buf[0] == 'P')
                send(s, buf, r, 0);
        }
        free(buf);
        closesocket(s);
    }
    return 0;
}

static int run_client(const char *ip, unsigned short port, int megabytes)
{
    SOCKET s = socket(AF_INET, SOCK_STREAM, 0);
    struct sockaddr_in a;
    memset(&a, 0, sizeof(a));
    a.sin_family = AF_INET;
    a.sin_port = htons(port);
    a.sin_addr.s_addr = inet_addr(ip);
    if (connect(s, (struct sockaddr *)&a, sizeof(a)))
    {
        printf("connect failed %d\n", WSAGetLastError());
        return 1;
    }

    char *buf = malloc(BUFSZ);
    memset(buf, 'D', BUFSZ);

    /* throughput: push megabytes MB as fast as possible */
    double t0 = now_ms();
    long long total = (long long)megabytes * 1024 * 1024;
    long long sent = 0;
    while (sent < total)
    {
        int chunk = (int)((total - sent) < BUFSZ ? (total - sent) : BUFSZ);
        int w = send(s, buf, chunk, 0);
        if (w <= 0) { printf("send failed %d\n", WSAGetLastError()); return 1; }
        sent += w;
    }
    double t1 = now_ms();
    double secs = (t1 - t0) / 1000.0;
    printf("THROUGHPUT send %lld bytes in %.3f s = %.2f MB/s (%.2f Mbit/s)\n",
           sent, secs, sent / secs / 1048576.0, sent / secs * 8.0 / 1048576.0);
    fflush(stdout);

    /* latency on a fresh connection: keeps ping bytes unambiguous server-side */
    closesocket(s);
    s = socket(AF_INET, SOCK_STREAM, 0);
    if (connect(s, (struct sockaddr *)&a, sizeof(a)))
    {
        printf("reconnect failed %d\n", WSAGetLastError());
        return 1;
    }

    /* latency: ping-pong small messages */
    memset(buf, 'P', BUFSZ);
    int iter = 2000;
    double best = 1e9, sum = 0;
    for (int i = 0; i < iter; i++)
    {
        double p0 = now_ms();
        if (send(s, buf, 64, 0) <= 0) break;
        int r = recv(s, buf, 64, 0);
        if (r <= 0) break;
        double d = now_ms() - p0;
        if (d < best) best = d;
        sum += d;
    }
    printf("LATENCY roundtrip avg %.3f ms min %.3f ms over %d iters\n",
           sum / iter, best, iter);

    free(buf);
    closesocket(s);
    return 0;
}

int main(int argc, char **argv)
{
    WSADATA wd;
    if (WSAStartup(MAKEWORD(2, 2), &wd)) { printf("wsa failed\n"); return 1; }
    int rc = 1;
    if (argc >= 3 && !strcmp(argv[1], "server"))
        rc = run_server((unsigned short)atoi(argv[2]));
    else if (argc >= 5 && !strcmp(argv[1], "client"))
        rc = run_client(argv[2], (unsigned short)atoi(argv[3]), atoi(argv[4]));
    else
        printf("usage: tcpbench server PORT | client IP PORT MB\n");
    WSACleanup();
    return rc;
}
