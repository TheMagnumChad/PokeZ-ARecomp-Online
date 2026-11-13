// UDP bridge for the explicitly enabled Lumiose integration.
#ifdef _WIN32
#include <winsock2.h>
#include <ws2tcpip.h>
#else
#include <arpa/inet.h>
#include <sys/socket.h>
#include <sys/ioctl.h>
#include <unistd.h>
typedef int SOCKET;
#define INVALID_SOCKET (-1)
#define closesocket close
#define ioctlsocket ioctl
#endif
#include <limits.h>
#include <pthread.h>
#include <stdatomic.h>
#include "../hle.h"
#include "../lumiose_native_profile.h"

enum { LUMIOSE_MAX_SOCKETS = 64 };
static SOCKET sockets[LUMIOSE_MAX_SOCKETS];
static pthread_mutex_t lock = PTHREAD_MUTEX_INITIALIZER;
static int started;
static _Atomic bool trainer_name_ready;

// Called after Lumiose reads the actual name from a loaded trainer save.
void lumiose_mark_trainer_ready(void) { atomic_store(&trainer_name_ready, true); }
HLE_FUNC("vmp_recomp_name_ready", lumiose_native_name_ready) {
    if (lumiose_native_caller(c->x[30])) lumiose_mark_trainer_ready();
    c->x[0] = 0;
}


static bool net_start(void) {
    if (!started) {
#ifdef _WIN32
        WSADATA data;
        if (WSAStartup(MAKEWORD(2, 2), &data)) return false;
#endif
        for (int i = 0; i < LUMIOSE_MAX_SOCKETS; i++) sockets[i] = INVALID_SOCKET;
        started = 1;
    }
    return true;
}

RT_ON_RESET(lumiose_net_reset) {
    atomic_store(&trainer_name_ready, false);
    pthread_mutex_lock(&lock);
    if (started) {
        for (int i = 0; i < LUMIOSE_MAX_SOCKETS; i++)
            if (sockets[i] != INVALID_SOCKET) closesocket(sockets[i]);
#ifdef _WIN32
        WSACleanup();
#endif
        started = 0;
    }
    pthread_mutex_unlock(&lock);
}

HLE_FUNC("_ZN2nn4nifm10InitializeEv", lumiose_nifm_init) { c->x[0] = 0; }
HLE_FUNC("_ZN2nn4nifm18IsNetworkAvailableEv", lumiose_nifm_available) { c->x[0] = 1; }
HLE_FUNC("_ZN2nn4nifm27SubmitNetworkRequestAndWaitEv", lumiose_nifm_request) { c->x[0] = 0; }

HLE_FUNC("_ZN2nn6socket10InitializeEPvmmi", lumiose_socket_init) {
    pthread_mutex_lock(&lock);
    bool ok = net_start();
    pthread_mutex_unlock(&lock);
    c->x[0] = ok ? 0 : 0xffffffffu;
    LOG("[lumiose net] socket initialization %s\n", ok ? "ok" : "failed");
}

HLE_FUNC("_ZN2nn6socket8FinalizeEv", lumiose_socket_finalize) { c->x[0] = 0; }

HLE_FUNC("_ZN2nn6socket6SocketENS0_6FamilyENS0_4TypeENS0_8ProtocolE", lumiose_socket_open) {
    int family = (int)c->x[0], type = (int)c->x[1], protocol = (int)c->x[2], fd = -1;
    pthread_mutex_lock(&lock);
    if (net_start() && family == 2 && type == 2 && (protocol == 0 || protocol == 17)) {
        SOCKET s = socket(AF_INET, SOCK_DGRAM, IPPROTO_UDP);
        if (s != INVALID_SOCKET) {
            u_long nonblocking = 1;
            if (ioctlsocket(s, FIONBIO, &nonblocking) == 0) {
                for (int i = 0; i < LUMIOSE_MAX_SOCKETS; i++) if (sockets[i] == INVALID_SOCKET) {
                    sockets[i] = s; fd = i; break;
                }
            }
            if (fd < 0) closesocket(s);
        }
    }
    pthread_mutex_unlock(&lock);
    LOG("[lumiose net] UDP socket %s\n", fd < 0 ? "failed" : "ready");
    c->x[0] = (uint32_t)fd;
}

HLE_FUNC("_ZN2nn6socket5CloseEi", lumiose_socket_close) {
    int fd = (int)c->x[0], result = -1;
    pthread_mutex_lock(&lock);
    if (started && fd >= 0 && fd < LUMIOSE_MAX_SOCKETS && sockets[fd] != INVALID_SOCKET) {
        result = closesocket(sockets[fd]);
        sockets[fd] = INVALID_SOCKET;
    }
    pthread_mutex_unlock(&lock);
    c->x[0] = (uint32_t)result;
}

// Switch's IPv4 sockaddr begins with one-byte length and family; the port and
// address are already in network byte order.
static bool read_address(uint64_t guest, uint64_t size, struct sockaddr_in *out) {
    if (!guest || size < 16) return false;
    const uint8_t *p = (const uint8_t *)(uintptr_t)guest;
    if (p[1] != 2) return false;
    memset(out, 0, sizeof *out);
    out->sin_family = AF_INET;
    memcpy(&out->sin_port, p + 2, 2);
    memcpy(&out->sin_addr, p + 4, 4);
    return true;
}

HLE_FUNC("_ZN2nn6socket6SendToEiPKvmNS0_7MsgFlagEPKNS0_8SockAddrEj", lumiose_socket_send) {
    int fd = (int)c->x[0], result = -1;
    static int logged;
    struct sockaddr_in address;
    if (c->x[1] && c->x[2] >= 4 && !atomic_load(&trainer_name_ready)) {
        const uint8_t *b = (const uint8_t *)(uintptr_t)c->x[1];
        if (b[0] == 'Z' && b[1] == 'A' && b[2] == 1 && b[3] == 1) {
            static int deferred;
            if (!deferred++) LOG("[lumiose net] waiting for the trainer name before joining\n");
            c->x[0] = c->x[2];  // The mod retries its HELLO until connected.
            return;
        }
    }
    if (c->x[2] <= INT_MAX && read_address(c->x[4], c->x[5], &address)) {
        const char *allow = getenv("LUMIOSE_TEST_SERVER");
        if (allow && *allow) {
            const unsigned char *ip = (const unsigned char *)&address.sin_addr;
            char endpoint[64];
            snprintf(endpoint, sizeof endpoint, "%u.%u.%u.%u:%u", ip[0], ip[1], ip[2], ip[3],
                     (unsigned)ntohs(address.sin_port));
            if (strcmp(endpoint, allow)) {
                LOG("[lumiose net] test blocked an unexpected destination\n");
                c->x[0] = (uint32_t)-1;
                return;
            }
        }
        const char *drop_file = getenv("LUMIOSE_TEST_DROP_FILE");
        FILE *drop = drop_file && *drop_file ? fopen(drop_file, "rb") : NULL;
        if (drop) fclose(drop);
        if (getenv("LUMIOSE_TEST_OFFLINE_CAPTURE") || drop) {
            const unsigned char *ip = (const unsigned char *)&address.sin_addr;
            LOG("[lumiose net] offline capture to %u.%u.%u.%u:%u, %llu bytes (not sent)\n",
                ip[0], ip[1], ip[2], ip[3], (unsigned)ntohs(address.sin_port), (unsigned long long)c->x[2]);
            c->x[0] = (uint32_t)c->x[2];
            return;
        }
        pthread_mutex_lock(&lock);
        if (started && fd >= 0 && fd < LUMIOSE_MAX_SOCKETS && sockets[fd] != INVALID_SOCKET)
            result = sendto(sockets[fd], (const char *)(uintptr_t)c->x[1], (int)c->x[2], 0,
                            (struct sockaddr *)&address, sizeof address);
        pthread_mutex_unlock(&lock);
    }
    if (logged++ < 10) LOG("[lumiose net] send %s, %llu bytes%s\n", result < 0 ? "failed" : "ok",
                           (unsigned long long)c->x[2], result < 0 ? " (invalid address or socket error)" : "");
    c->x[0] = (uint32_t)result;
}

HLE_FUNC("_ZN2nn6socket8RecvFromEiPvmNS0_7MsgFlagEPNS0_8SockAddrEPj", lumiose_socket_recv) {
    int fd = (int)c->x[0], result = -1;
    static int received;
    if (getenv("LUMIOSE_TEST_OFFLINE_CAPTURE")) { c->x[0] = (uint32_t)-1; return; }
    struct sockaddr_in address;
#ifdef _WIN32
    int size = sizeof address;
#else
    socklen_t size = sizeof address;
#endif
    if (c->x[2] <= INT_MAX) {
        pthread_mutex_lock(&lock);
        if (started && fd >= 0 && fd < LUMIOSE_MAX_SOCKETS && sockets[fd] != INVALID_SOCKET)
            result = recvfrom(sockets[fd], (char *)(uintptr_t)c->x[1], (int)c->x[2], 0,
                              (struct sockaddr *)&address, &size);
        pthread_mutex_unlock(&lock);
    }
    if (result >= 0 && c->x[4] && c->x[5] && *(uint32_t *)(uintptr_t)c->x[5] >= 16) {
        uint8_t *p = (uint8_t *)(uintptr_t)c->x[4];
        memset(p, 0, 16);
        p[0] = 16; p[1] = 2;
        memcpy(p + 2, &address.sin_port, 2);
        memcpy(p + 4, &address.sin_addr, 4);
        *(uint32_t *)(uintptr_t)c->x[5] = 16;
    }
    if (result >= 0 && received++ < 10) LOG("[lumiose net] received %d bytes\n", result);
    c->x[0] = (uint32_t)result;
}
