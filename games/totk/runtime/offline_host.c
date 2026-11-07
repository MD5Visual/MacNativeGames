/* Host side of the services a game reaches only when it has a network or a console identity: the host is an offline
   console. Network requests are never accepted, so a game keeps its offline paths (no sockets, friends or local wireless
   follow). From games/wonder (Super Mario Bros. Wonder imports them); here for the Switch 2 games (GAME_NS2: Donkey Kong
   Bananza initializes nifm at boot). TOTK and BotW import none of them and do not build this file. */
#include "runtime.h"
void hle_signal_system_event(void *event);
#include "game_version.h"
#include <arpa/inet.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <unistd.h>
#include <CommonCrypto/CommonDigest.h>

/* Failures in each service's own module; games test them for success only. */
#define R_NIFM_UNAVAILABLE (110u | (300u << 9))
#define R_FRIENDS_UNAVAILABLE (121u | (6u << 9))
#define R_LDN_UNAVAILABLE (203u | (32u << 9))
#define R_SSL_UNAVAILABLE (123u | (204u << 9))
#define R_NSD_UNAVAILABLE (141u | (2u << 9))

/* ---------- nn::oe ---------- */
/* The host never asks the game to exit, so a section that defers exit requests has nothing to defer. */
static void enter_exit_section(void) { }
static void leave_exit_section(void) { }
static void display_version(char *out) { memset(out, 0, 16); strncpy(out, TOTK_GAME_VERSION, 15); }
/* Stable for this Mac and this game, like the console's per-application pseudo device id. */
static void pseudo_device_id(uint8_t *uuid) {
    uuid_t host; struct timespec wait = {0, 0}; uint8_t digest[CC_SHA256_DIGEST_LENGTH]; uint64_t title = GAME_TITLE_ID;
    if (gethostuuid(host, &wait)) memset(host, 0, sizeof host);
    CC_SHA256_CTX c; CC_SHA256_Init(&c); CC_SHA256_Update(&c, host, sizeof host); CC_SHA256_Update(&c, &title, sizeof title); CC_SHA256_Final(digest, &c);
    memcpy(uuid, digest, 16);
}

/* ---------- nn::nifm ---------- */
static uint32_t nifm_initialize(void) { return 0; }
static unsigned char nifm_available(void) { return 0; }
static void nifm_local_mode(unsigned char on) { (void)on; }
static void nifm_submit_and_wait(void) { fprintf(stderr, "[totk] network request: refused (offline host)\n"); }
static void nifm_cancel(void) { }
static uint32_t nifm_unavailable(void) { return R_NIFM_UNAVAILABLE; }

/* ---------- nn::friends: no friends and no play history on an offline host ---------- */
static void friends_initialize(void) { }
static void friends_allocator(void *a, void *f) { (void)a; (void)f; }
static uint32_t friends_check(unsigned char *available, const void *uid) { (void)uid; if (available) *available = 0; return 0; }
static uint32_t friends_unavailable(void) { return R_FRIENDS_UNAVAILABLE; }
static uint32_t friends_list(int *count, void *out, const void *uid, int offset, int max, const void *filter) {
    (void)out; (void)uid; (void)offset; (void)max; (void)filter; if (count) *count = 0; return 0;
}
static uint32_t friends_add_history(void) { return 0; }

/* ---------- nn::ldn (local wireless): there is no radio, so no station or access point opens ---------- */
static uint32_t ldn_unavailable(void) { return R_LDN_UNAVAILABLE; }
static void ldn_finalize(void) { }
static int ldn_state(void) { return 0; }   /* State_None */
static void ldn_attach_event(void *event) { if (!event) runtime_fail("ldn state event"); hle_bind_system_event(event, 0); }

/* ---------- nn::ssl, nn::nsd, nn::ngc ---------- */
static uint32_t ssl_initialize(void) { return 0; }
static uint32_t ssl_unavailable(void) { return R_SSL_UNAVAILABLE; }
static uint32_t nsd_environment(char *out) { memset(out, 0, 8); memcpy(out, "lp1", 3); return 0; }
static uint32_t ngc_initialize(void *self, void *work, uint64_t size, unsigned char check) { (void)self; (void)work; (void)size; (void)check; return 0; }
/* Nothing is filtered: every word is reported clean. */
static uint32_t ngc_check(uint32_t *masks, uint32_t count, const void **words, uint64_t n) {
    (void)words; (void)n; if (masks) memset(masks, 0, count * sizeof *masks); return 0;
}

static uint32_t nsd_unavailable(void) { return R_NSD_UNAVAILABLE; }

/* ---------- nn::socket: the library starts, but there is no network, so every socket call fails with ENETDOWN ---------- */
#define SOCKET_ENETDOWN 50   /* Horizon uses the BSD errno values */
#define SOCKET_EAI_FAIL 4
static _Thread_local int socket_errno;
static uint32_t socket_initialize(void) { return 0; }
static int socket_down(void) { socket_errno = SOCKET_ENETDOWN; return -1; }
static int socket_last_error(void) { return socket_errno; }
static int socket_addr_info(const char *node, const char *service, const void *hints, void **out) {
    (void)node; (void)service; (void)hints; if (out) *out = NULL; return SOCKET_EAI_FAIL;
}
static void socket_free_addr_info(void *info) { (void)info; }
static uint32_t socket_htonl(uint32_t v) { return htonl(v); }
static uint16_t socket_htons(uint16_t v) { return htons(v); }
static const char *socket_ntop(int family, const void *in, char *out, uint32_t size) { return family == 2 ? inet_ntop(AF_INET, in, out, size) : NULL; }

/* ---------- nn::socket event counters: local wakeups work even on the offline host.
   Horizon's socket errno/flags are BSD values; nnmusl's errno is Linux. EventFdRead/Write return status 0, not
   the byte count of a POSIX read/write. Poll keeps the existing offline-descriptor behavior (nothing ready).
   Blocking host waits must hand off the virtual core with no host mutex held. */
#include <pthread.h>
#include <time.h>
#include <errno.h>
#define EVFDS 64
#define EV_BASE 512
#define EV_SEMAPHORE 1u
#define EV_CLOEXEC 2u
#define EV_NONBLOCK 4u
#define EV_MAX UINT64_C(0xfffffffffffffffe)
static struct { uint64_t count, generation; unsigned flags; int used; } evfd[EVFDS];
static pthread_mutex_t evfd_lock = PTHREAD_MUTEX_INITIALIZER;
static int ev_error(int e) {
    /* gRPC tests the guest C library's errno as well as nnsocketGetLastErrno. */
    static _Thread_local int *(*location)(void);
    socket_errno = e;
    int libc_error = e == 35 ? 11 : e;
    if (!location) location = runtime_symbol("__errno_location");
    if (location) *location() = libc_error;
    errno = libc_error;
    return -1;
}
/* Caller holds evfd_lock. */
static int ev_fd(int fd) { return fd >= EV_BASE && fd < EV_BASE + EVFDS && evfd[fd - EV_BASE].used ? fd - EV_BASE : -1; }
static void ev_sleep_us(long us) {
    struct timespec ts = {us / 1000000, (us % 1000000) * 1000};
    int away = runtime_core_away();
    while (nanosleep(&ts, &ts) && errno == EINTR) { }
    runtime_core_back(&away);
}
static uint64_t ev_deadline(uint64_t duration) {
    uint64_t now = runtime_now_ns();
    return duration > UINT64_MAX - now ? UINT64_MAX : now + duration;
}
static void ev_pause(uint64_t deadline) {
    uint64_t now = runtime_now_ns();
    if (now >= deadline) return;
    uint64_t left = deadline - now;
    ev_sleep_us(left >= 1000000 ? 1000 : (long)((left + 999) / 1000));
}
static int ev_event_fd(uint64_t init, unsigned flags) {
    if (init > EV_MAX || (flags & ~(EV_SEMAPHORE | EV_CLOEXEC | EV_NONBLOCK))) return ev_error(22);
    pthread_mutex_lock(&evfd_lock);
    for (int i = 0; i < EVFDS; i++) if (!evfd[i].used) {
        evfd[i].count = init; evfd[i].flags = flags; evfd[i].used = 1; evfd[i].generation++;
        pthread_mutex_unlock(&evfd_lock); return EV_BASE + i;
    }
    pthread_mutex_unlock(&evfd_lock); return ev_error(24);
}
static int ev_read(int fd, uint64_t *out) {
    pthread_mutex_lock(&evfd_lock);
    int i = ev_fd(fd);
    if (i < 0) { pthread_mutex_unlock(&evfd_lock); return ev_error(9); }
    uint64_t generation = evfd[i].generation;
    for (;;) {
        if (!evfd[i].used || evfd[i].generation != generation) { pthread_mutex_unlock(&evfd_lock); return ev_error(9); }
        if (evfd[i].count) {
            uint64_t take = evfd[i].flags & EV_SEMAPHORE ? 1 : evfd[i].count;
            evfd[i].count -= take; if (out) *out = take;
            pthread_mutex_unlock(&evfd_lock); return 0;
        }
        int nonblock = evfd[i].flags & EV_NONBLOCK;
        pthread_mutex_unlock(&evfd_lock);
        if (nonblock) return ev_error(35);
        ev_sleep_us(1000);
        pthread_mutex_lock(&evfd_lock);
    }
}
static int ev_write(int fd, uint64_t v) {
    pthread_mutex_lock(&evfd_lock);
    int i = ev_fd(fd);
    if (i < 0) { pthread_mutex_unlock(&evfd_lock); return ev_error(9); }
    if (v > EV_MAX) { pthread_mutex_unlock(&evfd_lock); return ev_error(22); }
    uint64_t generation = evfd[i].generation;
    for (;;) {
        if (!evfd[i].used || evfd[i].generation != generation) { pthread_mutex_unlock(&evfd_lock); return ev_error(9); }
        if (evfd[i].count <= EV_MAX - v) { evfd[i].count += v; pthread_mutex_unlock(&evfd_lock); return 0; }
        int nonblock = evfd[i].flags & EV_NONBLOCK;
        pthread_mutex_unlock(&evfd_lock);
        if (nonblock) return ev_error(35);
        ev_sleep_us(1000);
        pthread_mutex_lock(&evfd_lock);
    }
}
static int ev_poll(void *fds_, uint64_t nfds, int timeout_ms) {
    struct { int fd; short events, revents; } *fds = fds_;
    if (nfds && !fds) return ev_error(14);
    static unsigned ep_count;
    if (getenv("TOTK_WAIT_LOG") && __atomic_fetch_add(&ep_count, 1, __ATOMIC_RELAXED) < 10) {
        fprintf(stderr, "[wait-probe] poll nfds=%llu timeout=%d fds:", (unsigned long long)nfds, timeout_ms);
        for (uint64_t k = 0; k < nfds && k < 8; k++) fprintf(stderr, " %d/%x", fds[k].fd, fds[k].events);
        fprintf(stderr, "\n");
    }
    uint64_t deadline = timeout_ms < 0 ? UINT64_MAX : ev_deadline((uint64_t)timeout_ms * 1000000);
    for (;;) {
        int ready = 0;
        pthread_mutex_lock(&evfd_lock);
        for (uint64_t k = 0; k < nfds; k++) {
            fds[k].revents = 0; int i = ev_fd(fds[k].fd);
            if (i < 0) continue;   /* no network sockets exist on this offline host */
            if (evfd[i].count) fds[k].revents |= fds[k].events & (1 | 0x40);   /* POLLIN | POLLRDNORM */
            if (evfd[i].count < EV_MAX) fds[k].revents |= fds[k].events & 4;   /* POLLOUT / POLLWRNORM */
            ready += fds[k].revents != 0;
        }
        pthread_mutex_unlock(&evfd_lock);
        if (ready || runtime_now_ns() >= deadline) return ready;
        ev_pause(deadline);
    }
}
static int ev_select(int nfds, void *rin_, void *win_, void *ein_, void *tv_) {
    unsigned *sets[3] = {rin_, win_, ein_}, want[3][32] = {{0}};
    if (nfds < 0 || nfds > 1024) return ev_error(22);
    uint64_t deadline = UINT64_MAX;
    if (tv_) {
        const int64_t *tv = tv_;
        if (tv[0] < 0 || tv[0] > INT64_MAX / 1000000000 || tv[1] < 0 || tv[1] >= 1000000) return ev_error(22);
        deadline = ev_deadline((uint64_t)tv[0] * 1000000000 + (uint64_t)tv[1] * 1000);
    }
    for (int k = 0; k < 3; k++) if (sets[k]) memcpy(want[k], sets[k], sizeof want[k]);
    for (;;) {
        unsigned result[3][32] = {{0}}; int ready = 0, bad = 0;
        pthread_mutex_lock(&evfd_lock);
        for (int fd = 0; fd < nfds; fd++) {
            unsigned b = 1u << (fd & 31); int w = fd >> 5;
            if (!((want[0][w] | want[1][w] | want[2][w]) & b)) continue;
            int i = ev_fd(fd); if (i < 0) { bad = 1; break; }
            if ((want[0][w] & b) && evfd[i].count) { result[0][w] |= b; ready++; }
            if ((want[1][w] & b) && evfd[i].count < EV_MAX) { result[1][w] |= b; ready++; }
            /* No exceptional condition on a local event counter. */
        }
        pthread_mutex_unlock(&evfd_lock);
        if (bad) return ev_error(9);
        if (ready || runtime_now_ns() >= deadline) {
            for (int k = 0; k < 3; k++) if (sets[k]) memcpy(sets[k], result[k], sizeof result[k]);
            return ready;
        }
        ev_pause(deadline);
    }
}
static int ev_close(int fd) {
    pthread_mutex_lock(&evfd_lock); int i = ev_fd(fd);
    if (i >= 0) evfd[i].used = 0;
    pthread_mutex_unlock(&evfd_lock); return i < 0 ? ev_error(9) : 0;
}
static int ev_fcntl(int fd, unsigned cmd, unsigned long arg) {
    pthread_mutex_lock(&evfd_lock); int i = ev_fd(fd), result = 0;
    if (i >= 0) {
        if (cmd == 3) result = evfd[i].flags & EV_NONBLOCK ? 4 : 0;   /* BSD O_NONBLOCK */
        else if (cmd == 4) evfd[i].flags = (evfd[i].flags & ~EV_NONBLOCK) | (arg & 4 ? EV_NONBLOCK : 0);
        else if (cmd == 1) result = !!(evfd[i].flags & EV_CLOEXEC);
        else if (cmd == 2) evfd[i].flags = (evfd[i].flags & ~EV_CLOEXEC) | (arg & 1 ? EV_CLOEXEC : 0);
        else result = -1;
    }
    pthread_mutex_unlock(&evfd_lock);
    return i < 0 ? ev_error(9) : result < 0 ? ev_error(22) : result;
}
static int ev_set_error(int e) { socket_errno = e; return 0; }

/* ---------- nn::crypto ---------- */
static void random_bytes(void *out, uint64_t n) { arc4random_buf(out, (size_t)n); }

/* ---------- play reports: nothing is reported from an offline host ---------- */
static uint32_t ok_result(void) { return 0; }
static void nothing(void) { }
static int socket_ready(void) { return 1; }
static uint32_t false_flag(void) { return 0; }
static void friends_event(void *self, void *event) { (void)self; if (event) { hle_bind_system_event(event, 0); hle_signal_system_event(event); } }
static uint8_t invitation_event[0x80] __attribute__((aligned(16)));   /* friend invitations: bound, never signalled */
static void *friends_invitation_event(void) { static int bound; if (!bound) { bound = 1; hle_bind_system_event(invitation_event, 0); } return invitation_event; }
static uint32_t friends_done(void *self, unsigned char *done) { (void)self; if (done) *done = 1; return 0; }

/* ---------- nn::settings ---------- */
/* The SDK builds the language's code through the settings service; the table is the system's fixed one. */
static unsigned char language_is(const int *language, const char *code) {
    static const char *const codes[] = {"ja", "en-US", "fr", "de", "it", "es", "zh-CN", "ko", "nl", "pt", "ru", "zh-TW",
                                        "en-GB", "fr-CA", "es-419", "zh-Hans", "zh-Hant", "pt-BR"};
    if (!language || !code || *language < 0 || *language >= (int)(sizeof codes / sizeof *codes)) return 0;
    return !strncmp(codes[*language], code, 8);
}

void *runtime_offline_override(const char *name) {
    static const struct { const char *name; void *fn; } map[] = {
        {"_ZN2nn2oe31EnterExitRequestHandlingSectionEv", enter_exit_section},
        {"_ZN2nn2oe31LeaveExitRequestHandlingSectionEv", leave_exit_section},
        {"_ZN2nn2oe17GetDisplayVersionEPNS0_14DisplayVersionE", display_version},
        {"_ZN2nn2oe17GetPseudoDeviceIdEPNS_4util4UuidE", pseudo_device_id}, {"_ZN2nn4nifm10InitializeEv", nifm_initialize},
        {"_ZN2nn4nifm18IsNetworkAvailableEv", nifm_available}, {"_ZN2nn4nifm19SetLocalNetworkModeEb", nifm_local_mode},
        {"_ZN2nn4nifm27SubmitNetworkRequestAndWaitEv", nifm_submit_and_wait},
        {"_ZN2nn4nifm20CancelNetworkRequestEv", nifm_cancel},
        {"_ZN2nn4nifm31HandleNetworkRequestErrorResultEv", nifm_unavailable},
        {"_ZN2nn4nifm22GetCurrentIpConfigInfoEP7in_addrS2_S2_S2_S2_", nifm_unavailable},
        {"_ZN2nn4nifm22GetCurrentProxySettingEPNS0_12ProxySettingE", nifm_unavailable},
        {"_ZN2nn3nsd9ResolveExEPNS0_4FqdnERKS1_", nsd_unavailable}, {"_ZN2nn6socket10InitializeEPvmmi", socket_initialize},
        {"_ZN2nn6socket12GetLastErrorEv", socket_last_error},
        /* SDK 20's socket forms (Pokemon Pokopia): initialization succeeds, every socket call finds the network down. */
        {"_ZN2nn6socket10InitializeERKNS0_6ConfigE", socket_initialize}, {"_ZN2nn6socket8FinalizeEv", socket_initialize},
        {"_ZN2nn6socket12GetLastErrnoEv", socket_last_error},
        /* More of SDK 20's offline surface (Pokemon Pokopia): sockets stay down; crash reports, play reports and the profanity
           filter's setup are accepted; SSL, friends, local wireless, data delivery and the network request report the
           network unavailable, so the game keeps to its offline paths. */
        {"_ZN2nn6socket13IsInitializedEv", socket_ready}, {"_ZN2nn6socket4ReadEiPvm", socket_down}, {"_ZN2nn6socket5WriteEiPKvm", socket_down},
        /* Event counters (Ocarina of Time): gRPC's poller and the movie TaskQueue wake through these; without them gRPC
           finds no poll engine and aborts at boot. The C forms are what the game's gRPC imports. */
        {"_ZN2nn6socket7EventFdEmNS0_12EventFdFlagsE", ev_event_fd}, {"_ZN2nn6socket11EventFdReadEiPm", ev_read}, {"_ZN2nn6socket12EventFdWriteEim", ev_write},
        {"_ZN2nn6socket6SelectEiPNS0_5FdSetES2_S2_PNS0_7TimeValE", ev_select}, {"_ZN2nn6socket5CloseEi", ev_close},
        {"nnsocketClose", ev_close}, {"nnsocketFcntl", ev_fcntl}, {"nnsocketPoll", ev_poll},
         /* The C socket forms the game's gRPC imports: answer them here (ENETDOWN) instead of letting the SDK's BSD
            client talk to a bsd: service that does not exist - gRPC then fails connects fast and stops retry-spinning
            (same cut as nxnative's hle_net.c). */
         {"nnsocketSocket", socket_down}, {"nnsocketConnect", socket_down}, {"nnsocketBind", socket_down}, {"nnsocketListen", socket_down},
         {"nnsocketAccept", socket_down}, {"nnsocketShutdown", socket_down}, {"nnsocketSend", socket_down}, {"nnsocketRecv", socket_down},
         {"nnsocketSendMsg", socket_down}, {"nnsocketRecvMsg", socket_down}, {"nnsocketGetPeerName", socket_down}, {"nnsocketGetSockName", socket_down},
         {"nnsocketGetSockOpt", socket_down}, {"nnsocketSetSockOpt", socket_down},
         {"nnsocketGetAddrInfo", socket_addr_info}, {"nnsocketGetAddrInfoWithOptions", socket_addr_info}, {"nnsocketFreeAddrInfo", socket_free_addr_info},
        {"nnsocketGetLastErrno", socket_last_error}, {"nnsocketSetLastErrno", ev_set_error},
        {"_ZN2nn6socket11GetPeerNameEiP8sockaddrPj", socket_down},
        {"_ZN2nn2oe17EnableCrashReportEv", nothing}, {"_ZN2nn2oe18DisableCrashReportEv", nothing},
        {"_ZN2nn2oe40SendServerMaintenanceOverlayNotificationENS_4time9PosixTimeES2_", nothing},
        {"_ZN2nn4nifm20SubmitNetworkRequestEv", nothing}, {"_ZN2nn4nifm22IsNetworkRequestOnHoldEv", false_flag},
        {"_ZN2nn4nifm26GetCurrentPrimaryIpAddressEPNS_6socket6InAddrE", nifm_unavailable},
        {"_ZN2nn3ssl7ContextC1Ev", nothing}, {"_ZN2nn3ssl7ContextD1Ev", nothing}, {"_ZN2nn3ssl7Context7DestroyEv", ok_result},
        {"_ZN2nn3ssl7Context15ImportClientPkiEPmPKcS4_jj", ssl_unavailable}, {"_ZN2nn3ssl7Context15ImportServerPkiEPmPKcjNS0_17CertificateFormatE", ssl_unavailable},
        {"_ZN2nn3ssl7Context9ImportCrlEPmPKcj", ssl_unavailable}, {"_ZN2nn3ssl8FinalizeEv", ok_result},
        {"_ZN2nn3ssl14BuiltInManager28GetBuiltInCertificateBufSizeEPjPNS0_15CaCertificateIdEj", ssl_unavailable},
        {"_ZN2nn3ssl14BuiltInManager22GetBuiltInCertificatesEPPNS0_26BuiltInCertificateInfoBaseEPhjPNS0_15CaCertificateIdEj", ssl_unavailable},
        {"_ZN2nn7friends12AsyncContextC1Ev", nothing}, {"_ZN2nn7friends12AsyncContextD1Ev", nothing},
        {"_ZN2nn7friends12AsyncContext14GetSystemEventEPNS_2os11SystemEventE", friends_event}, {"_ZN2nn7friends12AsyncContext7HasDoneEPb", friends_done},
        {"_ZN2nn7friends14GetProfileListEPNS0_12AsyncContextEPNS0_7ProfileEPKNS_7account23NetworkServiceAccountIdEi", friends_unavailable},
        {"_ZN2nn7friends14GetProfileListEPNS0_12AsyncContextEPNS0_7ProfileERKNS_7account3UidEPKNS5_23NetworkServiceAccountIdEi", friends_unavailable},
        {"_ZN2nn7friends21StartFriendInvitationERKNS_7account3UidEiRKNS0_35FriendInvitationGameModeDescriptionEPKvm", friends_unavailable},
        {"_ZN2nn7friends28StartSendingFriendInvitationERKNS_7account3UidEPKNS1_23NetworkServiceAccountIdEiRKNS0_35FriendInvitationGameModeDescriptionEPKvm", friends_unavailable},
        {"_ZN2nn7friends38TryPopFriendInvitationNotificationInfoEPNS_7account3UidEPmPvm", false_flag},
        {"_ZN2nn7friends7ProfileC1Ev", nothing},
        {"_ZN2nn7friends36GetFriendInvitationNotificationEventEv", friends_invitation_event},
        {"_ZN2nn3ldn10DisconnectEv", ldn_unavailable}, {"_ZN2nn3ldn12CloseStationEv", ldn_unavailable}, {"_ZN2nn3ldn16CloseAccessPointEv", ldn_unavailable},
        {"_ZN2nn3ldn13CreateNetworkERKNS0_13NetworkConfigERKNS0_14SecurityConfigERKNS0_10UserConfigE", ldn_unavailable},
        {"_ZN2nn3ldn16SetAdvertiseDataEPKvm", ldn_unavailable}, {"_ZN2nn3ldn6RejectENS0_11Ipv4AddressE", ldn_unavailable},
        {"_ZN2nn3ldn7ConnectERKNS0_11NetworkInfoERKNS0_14SecurityConfigERKNS0_10UserConfigEiNS0_13ConnectOptionE", ldn_unavailable},
        {"_ZN2nn3ngc15ProfanityFilterC1Ev", nothing}, {"_ZN2nn3ngc15ProfanityFilterD1Ev", nothing}, {"_ZN2nn3ngc15ProfanityFilter8FinalizeEv", ok_result},
        {"_ZN2nn3ngc15ProfanityFilter10InitializeEPvm", ok_result}, {"_ZN2nn3ngc15ProfanityFilter22SetNormalizationOptionEi", nothing},
        {"_ZN2nn5prepo10PlayReportC1EPKc", nothing}, {"_ZN2nn5prepo10PlayReportD1Ev", nothing},
        {"_ZN2nn5prepo10PlayReport3AddEPKcRKNS0_5ArrayE", ok_result}, {"_ZN2nn5prepo10PlayReport3AddEPKcb", ok_result}, {"_ZN2nn5prepo10PlayReport3AddEPKcf", ok_result},
        {"_ZN2nn5prepo5ArrayC1Ev", nothing}, {"_ZN2nn5prepo5Array9SetBufferEPvm", nothing}, {"_ZN2nn5prepo5Array3AddEf", ok_result}, {"_ZN2nn5prepo5Array3AddEl", ok_result},
        {"_ZN2nn4bcat17DeliveryCacheFileC1Ev", nothing}, {"_ZN2nn4bcat17DeliveryCacheFileD1Ev", nothing},
        {"_ZN2nn4bcat17DeliveryCacheFile4OpenEPKc", nifm_unavailable}, {"_ZN2nn4bcat17DeliveryCacheFile4OpenERKNS0_13DirectoryNameERKNS0_8FileNameE", nifm_unavailable},
        {"_ZN2nn4bcat22DeliveryCacheDirectoryC1Ev", nothing}, {"_ZN2nn4bcat22DeliveryCacheDirectoryD1Ev", nothing},
        {"_ZN2nn4bcat22DeliveryCacheDirectory4OpenERKNS0_13DirectoryNameE", nifm_unavailable},
        {"_ZN2nn4bcat21DeliveryCacheProgressC1Ev", nothing}, {"_ZN2nn4bcat21DeliveryCacheProgressD1Ev", nothing},
        {"_ZN2nn4bcat24RequestSyncDeliveryCacheEPNS0_21DeliveryCacheProgressE", nifm_unavailable},
        {"_ZN2nn6socket10GetSockOptEiiiPvPj", socket_down}, {"_ZN2nn6socket10SetSockOptEiiiPKvj", socket_down},
        {"_ZN2nn6socket11GetSockNameEiP8sockaddrPj", socket_down}, {"_ZN2nn6socket4PollEP6pollfdmi", ev_poll},
        {"_ZN2nn6socket4RecvEiPvmi", socket_down}, {"_ZN2nn6socket4SendEiPKvmi", socket_down}, {"_ZN2nn6socket5FcntlEiNS0_12FcntlCommandEz", ev_fcntl},
        {"_ZN2nn6socket6AcceptEiP8sockaddrPj", socket_down}, {"_ZN2nn6socket6ListenEii", socket_down},
        {"_ZN2nn6socket6SelectEiPNS0_5FdSetES2_S2_PNS0_7TimeValE", socket_down}, {"_ZN2nn6socket6SendToEiPKvmiPK8sockaddrj", socket_down},
        {"_ZN2nn6socket7ConnectEiPK8sockaddrj", socket_down}, {"_ZN2nn6socket8RecvFromEiPvmiP8sockaddrPj", socket_down},
        {"_ZN2nn6socket8ShutdownEii", socket_down},
        {"_ZN2nn6socket11GetAddrInfoEPKcS2_PKNS0_8AddrInfoEPPS3_", socket_addr_info},
        {"_ZN2nn6socket12FreeAddrInfoEPNS0_8AddrInfoE", socket_free_addr_info}, {"_ZN2nn6socket9InetHtonlEj", socket_htonl},
        {"_ZN2nn6socket9InetHtonsEt", socket_htons},
        {"_ZN2nn6socket6SocketENS0_6FamilyENS0_4TypeENS0_8ProtocolE", socket_down},
        {"_ZN2nn6socket4BindEiPKNS0_8SockAddrEj", socket_down},
        {"_ZN2nn6socket6SendToEiPKvmNS0_7MsgFlagEPKNS0_8SockAddrEj", socket_down},
        {"_ZN2nn6socket8RecvFromEiPvmNS0_7MsgFlagEPNS0_8SockAddrEPj", socket_down},
        {"_ZN2nn6socket10SetSockOptEiNS0_5LevelENS0_6OptionEPKvj", socket_down}, {"_ZN2nn6socket5CloseEi", socket_down},
        {"_ZN2nn6socket7EventFdEmNS0_12EventFdFlagsE", socket_down}, {"_ZN2nn6socket11EventFdReadEiPm", socket_down},
        {"_ZN2nn6socket12EventFdWriteEim", socket_down},
        {"_ZN2nn6crypto36GenerateCryptographicallyRandomBytesEPvm", random_bytes},
        {"_ZN2nn5prepo10PlayReportC1Ev", nothing}, {"_ZN2nn5prepo10PlayReport10SetEventIdEPKc", ok_result},
        {"_ZN2nn5prepo10PlayReport9SetBufferEPvm", ok_result}, {"_ZN2nn5prepo10PlayReport3AddEPKcl", ok_result},
        {"_ZN2nn5prepo10PlayReport3AddEPKcS3_", ok_result}, {"_ZN2nn6socket6SocketEiii", socket_down},
        {"_ZN2nn6socket4BindEiPK8sockaddrj", socket_down},
        {"_ZN2nn6socket11GetAddrInfoEPKcS2_PK8addrinfoPPS3_", socket_addr_info},
        {"_ZN2nn6socket12FreeAddrInfoEP8addrinfo", socket_free_addr_info}, {"_ZN2nn6socket8InetNtopEiPKvPcj", socket_ntop},
        {"_ZN2nn6socket9InetNtohsEt", socket_htons}, {"_ZN2nn7friends10InitializeEv", friends_initialize},
        {"_ZN2nn7friends12SetAllocatorEPFPvmEPFvS1_mE", friends_allocator},
        {"_ZN2nn7friends27CheckFriendListAvailabilityEPbRKNS_7account3UidE", friends_check},
        {"_ZN2nn7friends25EnsureFriendListAvailableEPNS0_12AsyncContextERKNS_7account3UidE", friends_unavailable},
        {"_ZN2nn7friends13GetFriendListEPiPNS0_6FriendERKNS_7account3UidEiiRKNS0_12FriendFilterE", friends_list},
        {"_ZN2nn7friends29GetPlayHistoryRegistrationKeyEPNS0_26PlayHistoryRegistrationKeyERKNS_7account3UidEb", friends_unavailable},
        {"_ZN2nn7friends14AddPlayHistoryERKNS_7account3UidERKNS0_26PlayHistoryRegistrationKeyERKNS0_15InAppScreenNameESA_", friends_add_history},
        {"_ZN2nn3ldn10InitializeEv", ldn_unavailable}, {"_ZN2nn3ldn8FinalizeEv", ldn_finalize},
        {"_ZN2nn3ldn8GetStateEv", ldn_state},
        {"_ZN2nn3ldn22AttachStateChangeEventEPNS_2os15SystemEventTypeE", ldn_attach_event},
        {"_ZN2nn3ldn15OpenAccessPointEv", ldn_unavailable}, {"_ZN2nn3ldn11OpenStationEv", ldn_unavailable},
        {"_ZN2nn3ldn14GetNetworkInfoEPNS0_11NetworkInfoE", ldn_unavailable},
        {"_ZN2nn3ldn14GetIpv4AddressEPNS0_11Ipv4AddressEPNS0_10SubnetMaskE", ldn_unavailable},
        {"_ZN2nn3ldn4ScanEPNS0_11NetworkInfoEPiiRKNS0_10ScanFilterEi", ldn_unavailable},
        {"_ZN2nn3ldn11ScanPrivateEPNS0_11NetworkInfoEPiiRKNS0_10ScanFilterEi", ldn_unavailable},
        {"_ZN2nn3ssl10InitializeEv", ssl_initialize}, {"_ZN2nn3ssl7Context6CreateENS1_10SslVersionE", ssl_unavailable},
        {"_ZN2nn3nsd24GetEnvironmentIdentifierEPNS0_21EnvironmentIdentifierE", nsd_environment},
        {"_ZN2nn8settingseqERKNS0_8LanguageERKNS0_12LanguageCodeE", language_is},
        {"_ZN2nn3ngc15ProfanityFilter10InitializeEPvmb", ngc_initialize},
        {"_ZN2nn3ngc15ProfanityFilter19CheckProfanityWordsEPjjPPKcm", ngc_check},
        {"_ZN2nn3ngc15ProfanityFilter19CheckProfanityWordsEPjjPPKDsm", ngc_check},
    };
    for (size_t i = 0; i < sizeof map / sizeof *map; i++) if (!strcmp(name, map[i].name)) return map[i].fn;
    return NULL;
}
