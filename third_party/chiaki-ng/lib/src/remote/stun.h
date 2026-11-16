#include <stdint.h>
#include <stdlib.h>
#include <string.h>
#include <time.h>
#include <inttypes.h>

#ifdef _WIN32
#include <winsock2.h>
#include <ws2tcpip.h>
#include <windows.h>
#else
#include <unistd.h>
#include <errno.h>
#include <strings.h>
#include <pthread.h>
#include <sys/socket.h>
#include <sys/select.h>
#include <netdb.h>
#include <arpa/inet.h>
#include <netinet/in.h>
#endif

#include <chiaki/log.h>
#include <chiaki/seqnum.h>
#include <chiaki/sock.h>
#include <chiaki/random.h>
#include <chiaki/time.h>

/*
 * Oviora: STUN (finding our address as seen from the internet).
 *
 * The old code took its servers from a list on GitHub (10 s timeout when GitHub is slow) and then tried
 * hosts one by one, each name lookup blocking for as long as the DNS took (error 11001 after ~10 s).
 * Now:
 *  - a built-in list of well-known servers is used first, in a fixed order (the server that answered
 *    last time goes first); the GitHub list is only an extra, fetched in the background by holepunch.c
 *  - all names are looked up at the same time, with a short overall budget; a name that does not
 *    resolve in time (or at all) falls back to the address it had the last time, else it is skipped
 *  - servers are asked one after the other (the NAT test needs that order), each with a short reply
 *    timeout, and stray late replies of an earlier server no longer spoil the next one
 *  - the log says which server answered
 */

// Per server reply timeout (Yakamoz: 2 s instead of 5, dead servers each cost the full wait)
#define STUN_REPLY_TIMEOUT_SEC 2
// All STUN names are resolved in parallel within this time
#define STUN_RESOLVE_BUDGET_MS 2500
// ... or this one when every name has a known address from earlier
#define STUN_RESOLVE_BUDGET_CACHED_MS 800
// ... but once this many names have resolved (enough for the NAT test) after a short wait, go on
#define STUN_ENOUGH_SERVERS 5
#define STUN_RESOLVE_MIN_WAIT_MS 300
#define STUN_MAX_TARGETS 32
#define STUN_HOST_MAX 256

#define STUN_HEADER_SIZE 20
#define STUN_MSG_TYPE_BINDING_REQUEST 0x0001
#define STUN_MSG_TYPE_BINDING_RESPONSE 0x0101
#define STUN_MAGIC_COOKIE 0x2112A442UL
#define STUN_TRANSACTION_ID_LENGTH 12
#define STUN_ATTRIB_MAPPED_ADDRESS 0x0001
#define STUN_ATTRIB_XOR_MAPPED_ADDRESS 0x0020
#define STUN_MAPPED_ADDR_FAMILY_IPV4 0x01
#define STUN_MAPPED_ADDR_FAMILY_IPV6 0x02


typedef struct stun_server_t {
    char* host;
    uint16_t port;
} StunServer;

typedef struct stun_builtin_server_t {
    const char *host;
    uint16_t port;
} StunBuiltinServer;

// Reliable public servers, tried in this order. Different operators come first, so the NAT test
// (which needs answers from different destinations) gets distinct servers quickly.
static const StunBuiltinServer STUN_SERVERS_BUILTIN[] = {
    {"stun.l.google.com", 19302},
    {"stun.cloudflare.com", 3478},
    {"stun.nextcloud.com", 3478},
    {"stun.moonlight-stream.org", 3478},
    {"global.stun.twilio.com", 3478},
    {"stun1.l.google.com", 19302},
    {"stun2.l.google.com", 19302},
    {"stun3.l.google.com", 19302},
    {"stun4.l.google.com", 19302},
    {"stun.nextcloud.com", 443},
};

typedef struct stun_target_t {
    char host[STUN_HOST_MAX];
    uint16_t port;
    struct sockaddr_storage addr;
    socklen_t addr_len;
    bool from_cache;
} StunTarget;

// ------------------------------------------------------------------------------------------------
// Process wide memory: resolved addresses and the server that answered last
// ------------------------------------------------------------------------------------------------

#ifdef _WIN32
static SRWLOCK stun_cache_lock = SRWLOCK_INIT;
#define STUN_CACHE_LOCK() AcquireSRWLockExclusive(&stun_cache_lock)
#define STUN_CACHE_UNLOCK() ReleaseSRWLockExclusive(&stun_cache_lock)
#else
static pthread_mutex_t stun_cache_lock = PTHREAD_MUTEX_INITIALIZER;
#define STUN_CACHE_LOCK() pthread_mutex_lock(&stun_cache_lock)
#define STUN_CACHE_UNLOCK() pthread_mutex_unlock(&stun_cache_lock)
#endif

typedef struct stun_dns_cache_entry_t {
    bool used;
    char host[STUN_HOST_MAX];
    uint16_t port;
    int family;
    struct sockaddr_storage addr;
    socklen_t addr_len;
} StunDnsCacheEntry;

static StunDnsCacheEntry stun_dns_cache[STUN_MAX_TARGETS];
static size_t stun_dns_cache_next = 0;
static char stun_last_good_host[STUN_HOST_MAX] = {0};
static uint16_t stun_last_good_port = 0;

static bool stun_host_equal(const char *a, const char *b)
{
#ifdef _WIN32
    return _stricmp(a, b) == 0;
#else
    return strcasecmp(a, b) == 0;
#endif
}

static bool stun_dns_cache_get(const char *host, uint16_t port, int family, struct sockaddr_storage *addr, socklen_t *addr_len)
{
    bool found = false;
    STUN_CACHE_LOCK();
    for(size_t i = 0; i < STUN_MAX_TARGETS; i++)
    {
        StunDnsCacheEntry *e = &stun_dns_cache[i];
        if(e->used && e->port == port && e->family == family && stun_host_equal(e->host, host))
        {
            memcpy(addr, &e->addr, sizeof(e->addr));
            *addr_len = e->addr_len;
            found = true;
            break;
        }
    }
    STUN_CACHE_UNLOCK();
    return found;
}

static void stun_dns_cache_put(const char *host, uint16_t port, int family, const struct sockaddr_storage *addr, socklen_t addr_len)
{
    if(strlen(host) >= STUN_HOST_MAX)
        return;
    STUN_CACHE_LOCK();
    StunDnsCacheEntry *slot = NULL;
    for(size_t i = 0; i < STUN_MAX_TARGETS; i++)
    {
        StunDnsCacheEntry *e = &stun_dns_cache[i];
        if(e->used && e->port == port && e->family == family && stun_host_equal(e->host, host))
        {
            slot = e;
            break;
        }
    }
    if(!slot)
    {
        slot = &stun_dns_cache[stun_dns_cache_next];
        stun_dns_cache_next = (stun_dns_cache_next + 1) % STUN_MAX_TARGETS;
    }
    slot->used = true;
    strcpy(slot->host, host);
    slot->port = port;
    slot->family = family;
    memcpy(&slot->addr, addr, sizeof(slot->addr));
    slot->addr_len = addr_len;
    STUN_CACHE_UNLOCK();
}

static void stun_remember_good_server(const char *host, uint16_t port)
{
    if(strlen(host) >= STUN_HOST_MAX)
        return;
    STUN_CACHE_LOCK();
    strcpy(stun_last_good_host, host);
    stun_last_good_port = port;
    STUN_CACHE_UNLOCK();
}

// ------------------------------------------------------------------------------------------------
// Parallel name lookups. Every lookup runs on its own detached thread; the caller waits at most the
// budget and then goes on, a lookup still hanging in the system resolver just finishes on its own.
// ------------------------------------------------------------------------------------------------

typedef struct stun_resolve_job_t {
    volatile long refs;
    volatile long done;
    char host[STUN_HOST_MAX];
    char service[8];
    int family;
    bool ok;
    int error;
    struct sockaddr_storage addr;
    socklen_t addr_len;
} StunResolveJob;

static long stun_atomic_dec(volatile long *v)
{
#ifdef _WIN32
    return InterlockedDecrement((volatile LONG *)v);
#else
    return __atomic_sub_fetch(v, 1, __ATOMIC_ACQ_REL);
#endif
}

static long stun_atomic_load(volatile long *v)
{
#ifdef _WIN32
    return InterlockedCompareExchange((volatile LONG *)v, 0, 0);
#else
    return __atomic_load_n(v, __ATOMIC_ACQUIRE);
#endif
}

static void stun_atomic_store(volatile long *v, long value)
{
#ifdef _WIN32
    InterlockedExchange((volatile LONG *)v, value);
#else
    __atomic_store_n(v, value, __ATOMIC_RELEASE);
#endif
}

static void stun_sleep_ms(unsigned ms)
{
#ifdef _WIN32
    Sleep(ms);
#else
    usleep(ms * 1000);
#endif
}

static void stun_resolve_job_release(StunResolveJob *job)
{
    if(stun_atomic_dec(&job->refs) == 0)
        free(job);
}

static void stun_resolve_job_run(StunResolveJob *job)
{
    struct addrinfo hints;
    struct addrinfo *resolved = NULL;
    memset(&hints, 0, sizeof(hints));
    hints.ai_family = job->family;
    hints.ai_socktype = SOCK_DGRAM;
    if(job->family == AF_INET6)
        hints.ai_flags = AI_ADDRCONFIG;
    int r = getaddrinfo(job->host, job->service, &hints, &resolved);
    if(r == 0 && resolved && resolved->ai_addrlen <= sizeof(job->addr))
    {
        memcpy(&job->addr, resolved->ai_addr, resolved->ai_addrlen);
        job->addr_len = (socklen_t)resolved->ai_addrlen;
        job->ok = true;
    }
    else
    {
        job->ok = false;
        job->error = r;
    }
    if(resolved)
        freeaddrinfo(resolved);
    stun_atomic_store(&job->done, 1);
    stun_resolve_job_release(job);
}

#ifdef _WIN32
static DWORD WINAPI stun_resolve_thread(LPVOID arg)
{
    stun_resolve_job_run((StunResolveJob *)arg);
    return 0;
}
#else
static void *stun_resolve_thread(void *arg)
{
    stun_resolve_job_run((StunResolveJob *)arg);
    return NULL;
}
#endif

static bool stun_resolve_job_start(StunResolveJob *job)
{
#ifdef _WIN32
    HANDLE h = CreateThread(NULL, 256 * 1024, stun_resolve_thread, job, STACK_SIZE_PARAM_IS_A_RESERVATION, NULL);
    if(!h)
        return false;
    CloseHandle(h);
    return true;
#else
    pthread_attr_t attr;
    if(pthread_attr_init(&attr) != 0)
        return false;
    pthread_attr_setdetachstate(&attr, PTHREAD_CREATE_DETACHED);
    pthread_t thread;
    int r = pthread_create(&thread, &attr, stun_resolve_thread, job);
    pthread_attr_destroy(&attr);
    return r == 0;
#endif
}

static bool stun_sockaddr_equal(const struct sockaddr_storage *a, const struct sockaddr_storage *b)
{
    if(a->ss_family != b->ss_family)
        return false;
    if(a->ss_family == AF_INET)
    {
        const struct sockaddr_in *x = (const struct sockaddr_in *)a;
        const struct sockaddr_in *y = (const struct sockaddr_in *)b;
        return x->sin_port == y->sin_port && x->sin_addr.s_addr == y->sin_addr.s_addr;
    }
    if(a->ss_family == AF_INET6)
    {
        const struct sockaddr_in6 *x = (const struct sockaddr_in6 *)a;
        const struct sockaddr_in6 *y = (const struct sockaddr_in6 *)b;
        return x->sin6_port == y->sin6_port && memcmp(&x->sin6_addr, &y->sin6_addr, sizeof(x->sin6_addr)) == 0;
    }
    return false;
}

/**
 * Builds the list of servers to ask, in order: the server that answered last time, the built-in
 * servers, then the extra servers (the GitHub list, if it arrived). Names are resolved in parallel;
 * servers whose name can't be resolved in time use their last known address or are left out, and
 * servers that resolve to the same address and port are only asked once.
 */
static size_t stun_collect_targets(ChiakiLog *log, const StunServer *extra, size_t num_extra, bool ipv4, StunTarget *out, size_t max_out)
{
    typedef struct { const char *host; uint16_t port; } Name;
    Name names[STUN_MAX_TARGETS];
    size_t num_names = 0;
    char last_good_host[STUN_HOST_MAX];
    uint16_t last_good_port;

    STUN_CACHE_LOCK();
    strcpy(last_good_host, stun_last_good_host);
    last_good_port = stun_last_good_port;
    STUN_CACHE_UNLOCK();

    const size_t num_builtin = sizeof(STUN_SERVERS_BUILTIN) / sizeof(STUN_SERVERS_BUILTIN[0]);
    for(size_t pass = 0; pass < 3; pass++)
    {
        size_t count = pass == 0 ? (ipv4 && last_good_host[0] ? 1 : 0) : (pass == 1 ? num_builtin : num_extra);
        for(size_t i = 0; i < count && num_names < STUN_MAX_TARGETS; i++)
        {
            const char *host;
            uint16_t port;
            if(pass == 0) { host = last_good_host; port = last_good_port; }
            else if(pass == 1) { host = STUN_SERVERS_BUILTIN[i].host; port = STUN_SERVERS_BUILTIN[i].port; }
            else { host = extra[i].host; port = extra[i].port; }
            if(!host || !host[0] || port == 0 || strlen(host) >= STUN_HOST_MAX)
                continue;
            bool dup = false;
            for(size_t k = 0; k < num_names; k++)
            {
                if(names[k].port == port && stun_host_equal(names[k].host, host))
                {
                    dup = true;
                    break;
                }
            }
            if(dup)
                continue;
            names[num_names].host = host;
            names[num_names].port = port;
            num_names++;
        }
    }

    const int family = ipv4 ? AF_INET : AF_INET6;
    StunResolveJob *jobs[STUN_MAX_TARGETS];
    bool all_cached = true;
    for(size_t i = 0; i < num_names; i++)
    {
        struct sockaddr_storage tmp;
        socklen_t tmp_len;
        if(!stun_dns_cache_get(names[i].host, names[i].port, family, &tmp, &tmp_len))
            all_cached = false;
        StunResolveJob *job = calloc(1, sizeof(StunResolveJob));
        jobs[i] = job;
        if(!job)
            continue;
        job->refs = 2; // this function and the thread
        strcpy(job->host, names[i].host);
        snprintf(job->service, sizeof(job->service), "%u", (unsigned)names[i].port);
        job->family = family;
        if(!stun_resolve_job_start(job))
            stun_resolve_job_run(job); // no thread: resolve right here (drops the thread's reference)
    }

    const uint64_t budget_ms = all_cached ? STUN_RESOLVE_BUDGET_CACHED_MS : STUN_RESOLVE_BUDGET_MS;
    const uint64_t started = chiaki_time_now_monotonic_ms();
    while(true)
    {
        bool all_done = true;
        size_t usable = 0;
        for(size_t i = 0; i < num_names; i++)
        {
            if(!jobs[i])
                continue;
            if(!stun_atomic_load(&jobs[i]->done))
                all_done = false;
            else if(jobs[i]->ok)
                usable++;
        }
        uint64_t elapsed = chiaki_time_now_monotonic_ms() - started;
        // A slow DNS must not hold everything up: enough servers for the NAT test are enough
        if(all_done || elapsed >= budget_ms || (usable >= STUN_ENOUGH_SERVERS && elapsed >= STUN_RESOLVE_MIN_WAIT_MS))
            break;
        stun_sleep_ms(20);
    }

    const unsigned waited_ms = (unsigned)(chiaki_time_now_monotonic_ms() - started);
    size_t num_out = 0;
    size_t num_failed = 0;
    for(size_t i = 0; i < num_names; i++)
    {
        StunResolveJob *job = jobs[i];
        bool done = job && stun_atomic_load(&job->done);
        StunTarget target;
        memset(&target, 0, sizeof(target));
        strcpy(target.host, names[i].host);
        target.port = names[i].port;
        bool have = false;
        if(done && job->ok)
        {
            memcpy(&target.addr, &job->addr, sizeof(target.addr));
            target.addr_len = job->addr_len;
            stun_dns_cache_put(target.host, target.port, family, &target.addr, target.addr_len);
            have = true;
        }
        else
        {
            if(done)
                CHIAKI_LOGW(log, "STUN: could not resolve %s (error %d)", names[i].host, job->error);
            else
                CHIAKI_LOGW(log, "STUN: %s did not resolve within %u ms", names[i].host, waited_ms);
            if(stun_dns_cache_get(target.host, target.port, family, &target.addr, &target.addr_len))
            {
                CHIAKI_LOGI(log, "STUN: using the last known address of %s", names[i].host);
                target.from_cache = true;
                have = true;
            }
            else
                num_failed++;
        }
        if(job)
            stun_resolve_job_release(job);
        if(!have || num_out >= max_out)
            continue;
        bool dup = false;
        for(size_t k = 0; k < num_out; k++)
        {
            if(stun_sockaddr_equal(&out[k].addr, &target.addr))
            {
                dup = true;
                break;
            }
        }
        if(dup)
        {
            CHIAKI_LOGV(log, "STUN: %s:%u is the same server as one already listed, skipping", target.host, target.port);
            continue;
        }
        memcpy(&out[num_out++], &target, sizeof(target));
    }
    CHIAKI_LOGV(log, "STUN: %zu servers usable, %zu names unresolved", num_out, num_failed);
    return num_out;
}

// ------------------------------------------------------------------------------------------------
// One binding request
// ------------------------------------------------------------------------------------------------

/**
 * @return 1 if the response is valid and answers this request, 0 if it is for another request (a late
 *         reply of an earlier server), -1 if it is broken
 */
static int stun_parse_binding_response(ChiakiLog *log, const uint8_t *binding_resp, size_t received, const uint8_t *binding_req, char *address, uint16_t *port)
{
    if(received < STUN_HEADER_SIZE)
    {
        CHIAKI_LOGE(log, "remote/stun.h: Received message is too small");
        return -1;
    }
    if (*(uint16_t*)(&binding_resp[0]) != htons(STUN_MSG_TYPE_BINDING_RESPONSE)) {
        CHIAKI_LOGE(log, "remote/stun.h: Received STUN response with invalid message type");
        return -1;
    }
    // Verify length stored in binding_resp[2] is correct
    size_t expected_size = ntohs(*(uint16_t*)(&binding_resp[2])) + STUN_HEADER_SIZE;
    if (received != expected_size) {
        CHIAKI_LOGE(log, "remote/stun.h: Received STUN response with invalid length: %zu received, %zu expected", received, expected_size);
        return -1;
    }
    if (*(uint32_t*)(&binding_resp[4]) != htonl(STUN_MAGIC_COOKIE)) {
        CHIAKI_LOGE(log, "remote/stun.h: Received STUN response with invalid magic cookie");
        return -1;
    }
    if (memcmp(&binding_resp[8], &binding_req[8], STUN_TRANSACTION_ID_LENGTH) != 0)
        return 0;

    size_t response_pos = STUN_HEADER_SIZE;
    // Check we can read 4 bytes of attribute data
    while (response_pos + 4 <= received)
    {
        uint16_t attr_type = ntohs(*(uint16_t*)(&binding_resp[response_pos]));
        uint16_t attr_length = ntohs(*(uint16_t*)(&binding_resp[response_pos + 2]));
        // check that the whole advertised message has been received
        if(attr_length > received - response_pos - (sizeof(attr_type) + sizeof(attr_length)))
        {
            CHIAKI_LOGE(log, "remote/stun.h: Received STUN response with invalid data");
            return -1;
        }
        if (attr_type != STUN_ATTRIB_MAPPED_ADDRESS && attr_type != STUN_ATTRIB_XOR_MAPPED_ADDRESS)
        {
            // attribute values are padded to a multiple of 4 bytes
            response_pos += sizeof(attr_type) + sizeof(attr_length) + (((size_t)attr_length + 3) & ~(size_t)3);
            continue;
        }

        if (attr_length < 8)
        {
            CHIAKI_LOGE(log, "remote/stun.h: Received STUN mapped address with invalid length %u", attr_length);
            return -1;
        }
        bool xored = attr_type == STUN_ATTRIB_XOR_MAPPED_ADDRESS;
        uint8_t family = binding_resp[response_pos + 5];
        if (family == STUN_MAPPED_ADDR_FAMILY_IPV4) {
            if (attr_length != 8) {
                 CHIAKI_LOGE(log, "remote/stun.h: Received STUN_MAPPED_ADDR_FAMILY_IPV4 with invalid data!");
                 CHIAKI_LOGE(log, "remote/stun.h: Expected atribute length: 8. Received attribute length %u", attr_length);
                 return -1;
            }
            if (xored) {
                uint16_t xored_port = *(uint16_t*)(&binding_resp[response_pos + 6]) ^ (uint16_t)(htonl(STUN_MAGIC_COOKIE));
                *port = ntohs(xored_port);
                uint32_t xored_addr = *(uint32_t*)(&binding_resp[response_pos + 8]) ^ htonl(STUN_MAGIC_COOKIE);
                uint32_t addr = xored_addr;
                inet_ntop(AF_INET, &addr, address, INET_ADDRSTRLEN);
            } else {
                *port = ntohs(*(uint16_t*)(&binding_resp[response_pos + 6]));
                uint32_t addr = *(uint32_t*)(&binding_resp[response_pos + 8]);
                inet_ntop(AF_INET, &addr, address, INET_ADDRSTRLEN);
            }
        } else if (family == STUN_MAPPED_ADDR_FAMILY_IPV6) {
            if (attr_length != 20) {
                 CHIAKI_LOGE(log, "remote/stun.h: Received STUN_MAPPED_ADDR_FAMILY_IPV6 with invalid data!");
                 CHIAKI_LOGE(log, "remote/stun.h: Expected atribute length: 20. Received attribute length %u", attr_length);
                 return -1;
            }
            if (xored) {
                uint16_t xored_port = *(uint16_t*)(&binding_resp[response_pos + 6]) ^ (uint16_t)(htonl(STUN_MAGIC_COOKIE));
                *port = ntohs(xored_port);

                // XOR address with concat(STUN_MAGIC_COOKIE, transaction_id)
                // NOTE: RFC5389 says we need to convert from network to host
                // endianness here, but this seems to be misleading, see
                // https://stackoverflow.com/a/40325004
                uint8_t xored_addr[16];
                for (int i = 0; i < 16; i++) {
                    xored_addr[i] = binding_resp[response_pos + 8 + i] ^ binding_req[4 + i];
                }
                inet_ntop(AF_INET6, &xored_addr, address, INET6_ADDRSTRLEN);
            } else {
                *port = ntohs(*(uint16_t*)(&binding_resp[response_pos + 6]));
                inet_ntop(AF_INET6, &binding_resp[response_pos + 8], address, INET6_ADDRSTRLEN);
            }
        } else {
            CHIAKI_LOGE(log, "remote/stun.h: Received STUN response with invalid address family: %d", family);
            return -1;
        }
        return 1;
    }
    CHIAKI_LOGE(log, "remote/stun.h: STUN response without a mapped address");
    return -1;
}

/**
 * Sends one binding request to target and waits (at most STUN_REPLY_TIMEOUT_SEC) for its answer.
 * Replies to other requests (late answers of an earlier server) are skipped, not counted as failure.
 */
static bool stun_query_target(ChiakiLog *log, const StunTarget *target, char *address, uint16_t *port, chiaki_socket_t *sock)
{
    uint8_t binding_req[STUN_HEADER_SIZE];
    memset(binding_req, 0, sizeof(binding_req));
    *(uint16_t*)(&binding_req[0]) = htons(STUN_MSG_TYPE_BINDING_REQUEST);
    *(uint16_t*)(&binding_req[2]) = htons(0);  // Length
    *(uint32_t*)(&binding_req[4]) = htonl(STUN_MAGIC_COOKIE);
    chiaki_random_bytes_crypt(&binding_req[8], STUN_TRANSACTION_ID_LENGTH);

    CHIAKI_SSIZET_TYPE sent = sendto(*sock, (CHIAKI_SOCKET_BUF_TYPE)binding_req, sizeof(binding_req), 0, (const struct sockaddr *)&target->addr, target->addr_len);
    if (sent != sizeof(binding_req)) {
        CHIAKI_LOGW(log, "remote/stun.h: Failed to send STUN request to %s:%u, error was " CHIAKI_SOCKET_ERROR_FMT, target->host, target->port, CHIAKI_SOCKET_ERROR_VALUE);
        return false;
    }

    const uint64_t deadline = chiaki_time_now_monotonic_ms() + STUN_REPLY_TIMEOUT_SEC * 1000;
    while(true)
    {
        uint64_t now = chiaki_time_now_monotonic_ms();
        if(now >= deadline)
            return false;
        uint64_t left = deadline - now;
        fd_set fds;
        FD_ZERO(&fds);
        FD_SET(*sock, &fds);
        struct timeval tv;
        tv.tv_sec = (long)(left / 1000);
        tv.tv_usec = (long)((left % 1000) * 1000);
        int r = select((int)(*sock) + 1, &fds, NULL, NULL, &tv);
        if(r == 0)
            return false;
        if(r < 0)
        {
            CHIAKI_LOGE(log, "remote/stun.h: select failed while waiting for %s, error was " CHIAKI_SOCKET_ERROR_FMT, target->host, CHIAKI_SOCKET_ERROR_VALUE);
            return false;
        }
        uint8_t binding_resp[256];
        CHIAKI_SSIZET_TYPE received = recvfrom(*sock, (CHIAKI_SOCKET_BUF_TYPE)binding_resp, sizeof(binding_resp), 0, NULL, NULL);
        if (received < 0) {
#ifdef _WIN32
            // An ICMP "port unreachable" of an earlier request shows up as WSAECONNRESET on Windows
            if(WSAGetLastError() == WSAECONNRESET || WSAGetLastError() == WSAEWOULDBLOCK)
                continue;
#else
            if(errno == EAGAIN || errno == EWOULDBLOCK || errno == ECONNREFUSED)
                continue;
#endif
            CHIAKI_LOGE(log, "remote/stun.h: Failed to receive STUN response, error was " CHIAKI_SOCKET_ERROR_FMT, CHIAKI_SOCKET_ERROR_VALUE);
            return false;
        }
        int parsed = stun_parse_binding_response(log, binding_resp, (size_t)received, binding_req, address, port);
        if(parsed == 1)
            return true;
        if(parsed == 0)
        {
            CHIAKI_LOGV(log, "remote/stun.h: skipping a late reply to an earlier STUN request");
            continue;
        }
        return false;
    }
}

/**
 * Get external address and port using STUN.
 *
 * Tries the server that answered last time, then the built-in servers, then the passed (extra)
 * servers, one after the other.
 *
 * @param log Log context
 * @param[out] address Buffer to store address in
 * @param[out] port Buffer to store port in
 * @return true if successful, false otherwise
 */
static bool stun_get_external_address(ChiakiLog *log, char *address, uint16_t *port, StunServer *passed_servers, size_t num_passed_servers, chiaki_socket_t *sock, bool ipv4)
{
    StunTarget targets[STUN_MAX_TARGETS];
    size_t num_targets = stun_collect_targets(log, passed_servers, num_passed_servers, ipv4, targets, STUN_MAX_TARGETS);
    for (size_t i = 0; i < num_targets; i++)
    {
        if(CHIAKI_SOCKET_IS_INVALID(*sock))
            return false;
        uint64_t started = chiaki_time_now_monotonic_ms();
        if (stun_query_target(log, &targets[i], address, port, sock))
        {
            CHIAKI_LOGI(log, "STUN: %s:%u answered in %u ms (mapped port %u)", targets[i].host, targets[i].port,
                (unsigned)(chiaki_time_now_monotonic_ms() - started), (unsigned)*port);
            if(ipv4)
                stun_remember_good_server(targets[i].host, targets[i].port);
            return true;
        }
        CHIAKI_LOGW(log, "Failed to get external address from %s:%d, retrying with another STUN server...", targets[i].host, targets[i].port);
        // Only try 1 IPV6 server
        if(!ipv4)
            break;
    }
    CHIAKI_LOGE(log, "Failed to get external address from any STUN server.");
    return false;
}

/**
 * Get external address and port using STUN, and find out how the NAT hands out ports
 * (asks up to 4 different servers one after the other).
 *
 * @param log Log context
 * @param[out] address Buffer to store address in
 * @param[out] port Buffer to store port in
 * @return true if successful, false otherwise
 */
CHIAKI_EXPORT bool stun_port_allocation_test(ChiakiLog *log, char *address, uint16_t *port, int32_t *allocation_increment, bool *random_allocation, StunServer *passed_servers, size_t num_passed_servers, chiaki_socket_t *sock)
{
    // skip testing if outgoing port changes with same internal ip and port if send to same ip and different port bc that doesn't apply in our case (we will be using a different address anyway)
    uint16_t port1 = 0;
    uint16_t port2 = 0;
    uint16_t port3 = 0;
    uint16_t port4 = 0;
    char addr1[INET6_ADDRSTRLEN];
    char addr2[INET6_ADDRSTRLEN];
    char addr3[INET6_ADDRSTRLEN];
    char addr4[INET6_ADDRSTRLEN];

    StunTarget targets[STUN_MAX_TARGETS];
    size_t num_targets = stun_collect_targets(log, passed_servers, num_passed_servers, true, targets, STUN_MAX_TARGETS);
    const char *first_host = NULL;
    uint16_t first_port = 0;
    int answered = 0;
    int asked = 0;
    const uint64_t test_started = chiaki_time_now_monotonic_ms();
    for (size_t i = 0; i < num_targets && port4 == 0; i++)
    {
        if(CHIAKI_SOCKET_IS_INVALID(*sock))
            break;
        char *addr_out = port1 == 0 ? addr1 : (port2 == 0 ? addr2 : (port3 == 0 ? addr3 : addr4));
        uint16_t *port_out = port1 == 0 ? &port1 : (port2 == 0 ? &port2 : (port3 == 0 ? &port3 : &port4));
        asked++;
        if (!stun_query_target(log, &targets[i], addr_out, port_out, sock))
        {
            *port_out = 0;
            CHIAKI_LOGW(log, "Failed to get external address from %s:%d, retrying with another STUN server...", targets[i].host, targets[i].port);
            continue;
        }
        answered++;
        CHIAKI_LOGV(log, "Got response from STUN server %s:%d", targets[i].host, targets[i].port);
        if(!first_host)
        {
            first_host = targets[i].host;
            first_port = targets[i].port;
        }
    }
    if(first_host)
    {
        CHIAKI_LOGI(log, "STUN: %d of %d servers answered in %u ms (first: %s:%u)", answered, asked,
            (unsigned)(chiaki_time_now_monotonic_ms() - test_started), first_host, (unsigned)first_port);
        stun_remember_good_server(first_host, first_port);
    }
    // No servers returned
    if(port1 == 0)
    {
        CHIAKI_LOGE(log, "Failed to get external address from any STUN server.");
        return false;
    }
    // 1 server returned
    else if(port2 == 0)
    {
        CHIAKI_LOGW(log, "Only 1 STUN server responded for packet allocation calculation.");
        CHIAKI_LOGW(log, "Couldn't determine packet allocation because not enough STUN servers responded.");
        memcpy(address, addr1, sizeof(addr1));
        *port = port1;
        *allocation_increment = 0;
    }
    // 2 servers returned
    else if(port3 == 0)
    {
        memcpy(address, addr2, sizeof(addr2));
        CHIAKI_LOGW(log, "Only 2 STUN servers responded for packet allocation calculation.");
        // address changed between requests use most recent one
        if(strcmp(addr1, addr2) != 0)
        {
            CHIAKI_LOGW(log, "Got different addresses between 2 responses, using 2nd one...");
            CHIAKI_LOGW(log, "Couldn't determine packet allocation because not enough STUN servers responded with the same address defaulting to 0.");
            *allocation_increment = 0;
        }
        else
        {
            *allocation_increment = port2 - port1;
        }
        *port = port2 + 2 * (*allocation_increment);
    }
    // 3 servers returned
    else if(port4 == 0)
    {
        CHIAKI_LOGW(log, "Only 3 STUN servers responded for packet allocation calculation.");
        memcpy(address, addr3, sizeof(addr3));
        if(strcmp(addr1, addr2) != 0)
        {
            if(strcmp(addr1, addr3) == 0)
            {
                CHIAKI_LOGW(log, "Calculating packet allocation based on 2 responses with the same address");
                *allocation_increment = (port3 - port1) / 2;
            }
            else if(strcmp(addr2, addr3) == 0)
            {
                CHIAKI_LOGW(log, "Calculating packet allocation based on 2 responses with the same address");
                *allocation_increment = port3 - port2;
            }
            else
            {
                CHIAKI_LOGW(log, "Got 3 different addresses between 3 responses, using 3rd one...");
                CHIAKI_LOGW(log, "Couldn't determine packet allocation because not enough STUN servers responded with the same address.");
                *allocation_increment = 0;
            }
        }
        else
        {
            if(strcmp(addr1, addr3) != 0)
            {
                CHIAKI_LOGW(log, "Calculating packet allocation based on 2 responses with the same address");
                *allocation_increment = port2 - port1;
            }
            else
            {
                CHIAKI_LOGW(log, "Calculating packet allocation based on 3 responses with the same address");
                *allocation_increment = port2 - port1;
                int32_t allocation_increment1 = port3 - port2;
                if(allocation_increment1 != (*allocation_increment))
                {
                    *random_allocation = true;
                    if(*allocation_increment == 0)
                        *allocation_increment = allocation_increment1;
                    CHIAKI_LOGW(log, "Got different allocation increment calculations from different ports.\nIncrement0: %d, Increment1: %d", *allocation_increment, allocation_increment1);
                }
            }
        }
        *port = port3 + (*allocation_increment);
    }
    // all 4 servers returned
    else
    {
        memcpy(address, addr4, sizeof(addr4));
        *port = port4;
        if(strcmp(addr1, addr2) != 0)
        {
            if((strcmp(addr1, addr3) == 0) || (strcmp(addr1, addr4) == 0))
            {
                if((strcmp(addr1, addr4) == 0))
                {
                    if(strcmp(addr1, addr3) == 0)
                    {
                        CHIAKI_LOGW(log, "Calculating packet allocation based on 3 responses with the same address");
                        *allocation_increment = port4 - port3;
                        int32_t allocation_increment1 = (port3 - port1) / 2;
                        if(allocation_increment1 != (*allocation_increment))
                        {
                            *random_allocation = true;
                            if(*allocation_increment == 0)
                                *allocation_increment = allocation_increment1;
                            CHIAKI_LOGW(log, "Got different allocation increment calculations from different ports.\nIncrement0: %d, Increment1: %d", *allocation_increment, allocation_increment1);
                        }
                    }
                    else
                    {
                        CHIAKI_LOGW(log, "Calculating packet allocation based on 2 responses with the same address");
                        *allocation_increment = (port4 - port1) / 4;
                    }
                }
                else
                {
                    CHIAKI_LOGW(log, "Calculating packet allocation based on 2 responses with the same address");
                    *allocation_increment = (port3 - port1) / 2;
                }
            }
            else
            {
                if((strcmp(addr2, addr3) == 0) || (strcmp(addr2, addr4) == 0))
                {
                    if((strcmp(addr2, addr4) == 0))
                    {
                        if(strcmp(addr2, addr3) == 0)
                        {
                            CHIAKI_LOGW(log, "Calculating packet allocation based on 3 responses with the same address");
                            *allocation_increment = port4 - port3;
                            int32_t allocation_increment1 = port3 - port2;
                            if(allocation_increment1 != (*allocation_increment))
                            {
                                *random_allocation = true;
                                if(*allocation_increment == 0)
                                    *allocation_increment = allocation_increment1;
                                CHIAKI_LOGW(log, "Got different allocation increment calculations from different ports.\nIncrement0: %d, Increment1: %d", *allocation_increment, allocation_increment1);
                            }
                        }
                        else
                        {
                            CHIAKI_LOGW(log, "Calculating packet allocation based on 2 responses with the same address");
                            *allocation_increment = (port4 - port1) / 4;
                        }
                    }
                    else
                    {
                        CHIAKI_LOGW(log, "Calculating packet allocation based on 2 responses with the same address");
                        *allocation_increment = (port3 - port1) / 2;
                    }

                }
                else if (strcmp(addr3, addr4) == 0)
                {
                    CHIAKI_LOGW(log, "Calculating packet allocation based on 2 responses with the same address");
                    *allocation_increment = port3 - port4;
                }
                else
                {
                    CHIAKI_LOGW(log, "Got 4 different addresses between 4 responses, using 4th one...");
                    CHIAKI_LOGW(log, "Couldn't determine packet allocation because not enough STUN servers responded with the same address.");
                    *allocation_increment = 0;
                }
            }
        }
        else if(strcmp(addr2, addr3) != 0)
        {
            *allocation_increment = port2 - port1;
            if(strcmp(addr2, addr4) == 0)
            {
                CHIAKI_LOGW(log, "Calculating packet allocation based on 3 responses with the same address");
                int32_t allocation_increment1 = (port4 - port2) / 2;
                if(allocation_increment1 != (*allocation_increment))
                {
                    *random_allocation = true;
                    if(*allocation_increment == 0)
                        *allocation_increment = allocation_increment1;
                    CHIAKI_LOGW(log, "Got different allocation increment calculations from different ports.\nIncrement0: %d, Increment1: %d", *allocation_increment, allocation_increment1);
                }
            }
            else
            {
                CHIAKI_LOGW(log, "Calculating packet allocation based on 2 responses with the same address");
            }
        }
        else if(strcmp(addr3, addr4) != 0)
        {
            CHIAKI_LOGW(log, "Calculating packet allocation based on 3 responses with the same address");
            *allocation_increment = port2 - port1;
            int32_t allocation_increment1 = port3 - port2;
            if(allocation_increment1 != (*allocation_increment))
            {
                *random_allocation = true;
                if(*allocation_increment == 0)
                    *allocation_increment = allocation_increment1;
                CHIAKI_LOGW(log, "Got different allocation increment calculations from different ports.\nIncrement0: %d, Increment1: %d", *allocation_increment, allocation_increment1);
            }
        }
        else
        {
            CHIAKI_LOGI(log, "Calculating packet allocation based on 4 responses with the same address");
            uint16_t increment0 = port2 - port1;
            uint16_t increment1 = port3 - port2;
            uint16_t increment2 = port4 - port3;
            if((increment0 == increment1) && (increment1 == increment2))
            {
                *allocation_increment = increment0;
                CHIAKI_LOGV(log, "Got 3 idential allocation increment calculations out of 3,\nIncrement %d", increment0);
            }
            else if (increment0 == increment1 || increment0 == increment2)
            {
                CHIAKI_LOGV(log, "Got 2 idential allocation increment calculations out of 3 from different ports.\nIncrement 0: %d, Increment 1: %d, Increment 2: %d", increment0, increment1, increment2);
                *allocation_increment = increment0;
            }
            else if(increment1 == increment2)
            {
                CHIAKI_LOGV(log, "Got 2 idential allocation increment calculations out of 3 from different ports.\nIncrement 0: %d, Increment 1: %d, Increment 2: %d", increment0, increment1, increment2);
                *allocation_increment = increment1;
            }
            else
            {
                *random_allocation = true;
                CHIAKI_LOGW(log, "Got different allocation increment calculations from different ports.\nIncrement 0: %d, Increment 1: %d, Increment 2: %d", increment0, increment1, increment2);
                if(increment0 != 0)
                    *allocation_increment = increment0;
                else
                    *allocation_increment = increment1;
            }
        }
    }

    return true;
}
