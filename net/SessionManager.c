/* vim: set expandtab ts=4 sw=4: */
/*
 * You may redistribute this program and/or modify it under the terms of
 * the GNU General Public License as published by the Free Software Foundation,
 * either version 3 of the License, or (at your option) any later version.
 *
 * This program is distributed in the hope that it will be useful,
 * but WITHOUT ANY WARRANTY; without even the implied warranty of
 * MERCHANTABILITY or FITNESS FOR A PARTICULAR PURPOSE.  See the
 * GNU General Public License for more details.
 *
 * You should have received a copy of the GNU General Public License
 * along with this program.  If not, see <http://www.gnu.org/licenses/>.
 */
#include "memory/Allocator.h"
#include "wire/PFChan.h"
#include "net/SessionManager.h"
#include "crypto/AddressCalc.h"
#include "util/AddrTools.h"
#include "wire/Error.h"
#include "util/events/Time.h"
#include "util/Defined.h"
#include "wire/RouteHeader.h"
#include "util/events/Timeout.h"

/** Handle numbers 0-3 are reserved for CryptoAuth nonces. */
#define MIN_FIRST_HANDLE 4

#define MAX_FIRST_HANDLE 100000

struct BufferedMessage
{
    struct Message* msg;
    struct Allocator* alloc;
    uint32_t timeSent;
};

struct Ip6 {
    uint8_t bytes[16];
};
#define Map_KEY_TYPE struct Ip6
#define Map_VALUE_TYPE struct BufferedMessage*
#define Map_NAME BufferedMessages
#include "util/Map.h"

#define Map_KEY_TYPE struct Ip6
#define Map_VALUE_TYPE struct SessionManager_Session_pvt*
#define Map_NAME OfSessionsByIp6
#define Map_ENABLE_HANDLES
#include "util/Map.h"

struct SessionManager_pvt
{
    struct SessionManager pub;
    struct Iface eventIf;
    struct Allocator* alloc;
    struct Map_BufferedMessages bufMap;
    struct Log* log;
    struct CryptoAuth* ca;
    struct EventBase* eventBase;
    uint32_t firstHandle;
    Identity
};

struct SessionManager_Session_pvt
{
    struct SessionManager_Session pub;

    struct SessionManager_pvt* sessionManager;

    struct Allocator* alloc;

    Identity
};

#define debugHandlesAndLabel(logger, session, label, message, ...) \
    do {                                                                               \
        if (!Defined(Log_DEBUG)) { break; }                                            \
        uint8_t path[20];                                                              \
        AddrTools_printPath(path, label);                                              \
        uint8_t ip[40];                                                                \
        AddrTools_printIp(ip, session->ip6);                                           \
        Log_debug(logger, "ver[%u] send[%d] recv[%u] ip[%s] path[%s] " message,        \
                  session->version,                                                    \
                  session->sendHandle,                                                 \
                  session->receiveHandle,                                              \
                  ip,                                                                  \
                  path,                                                                \
                  __VA_ARGS__);                                                        \
    } while (0)
//CHECKFILES_IGNORE expecting a ;

#define debugHandlesAndLabel0(logger, session, label, message) \
    debugHandlesAndLabel(logger, session, label, "%s", message)

static void sendSession(struct SessionManager_Session_pvt* sess,
                        uint64_t path,
                        uint32_t destPf,
                        enum PFChan_Core ev)
{
    struct PFChan_Node session = {
        .path_be = Endian_hostToBigEndian64(path),
        .metric_be = 0xffffffff,
        .version_be = Endian_hostToBigEndian32(sess->pub.version)
    };
    Bits_memcpyConst(session.ip6, sess->pub.caSession->herIp6, 16);
    Bits_memcpyConst(session.publicKey, sess->pub.caSession->herPublicKey, 32);

    struct Allocator* alloc = Allocator_child(sess->alloc);
    struct Message* msg = Message_new(0, PFChan_Node_SIZE + 512, alloc);
    Message_push(msg, &session, PFChan_Node_SIZE, NULL);
    Message_push32(msg, destPf, NULL);
    Message_push32(msg, ev, NULL);
    Iface_send(&sess->sessionManager->eventIf, msg);
    Allocator_free(alloc);
}

static int sessionCleanup(struct Allocator_OnFreeJob* job)
{
    struct SessionManager_Session_pvt* sess =
        Identity_check((struct SessionManager_Session_pvt*) job->userData);
    sendSession(sess, sess->pub.sendSwitchLabel, 0xffffffff, PFChan_Core_SESSION_ENDED);
    return 0;
}

static inline struct SessionManager_Session_pvt* sessionForHandle(uint32_t handle,
                                                                  struct SessionManager_pvt* sm)
{
    int index = Map_OfSessionsByIp6_indexForHandle(handle - sm->firstHandle, &sm->ifaceMap);
    if (index < 0) { return NULL; }
    check(sm, index);
    return Identity_check(sm->ifaceMap.values[index]);
}

struct SessionManager_Session* SessionManager_sessionForHandle(uint32_t handle,
                                                               struct SessionManager* manager)
{
    struct SessionManager_pvt* sm = Identity_check((struct SessionManager_pvt*) manager);
    return sessionForHandle(handle, sm);
}

static inline struct SessionManager_Session* sessionForIp6(uint8_t ip6[16],
                                                           struct SessionManager_pvt* sm)
{
    int ifaceIndex = Map_OfSessionsByIp6_indexForKey((struct Ip6*)ip6, &sm->ifaceMap);
    if (ifaceIndex == -1) { return NULL; }
    check(st, ifaceIndex);
    return Identity_check(sm->ifaceMap.values[ifaceIndex]);
}

struct SessionManager_Session* SessionManager_sessionForIp6(uint8_t* ip6,
                                                            struct SessionManager* sm)
{
    struct SessionManager_pvt* sm = Identity_check((struct SessionManager_pvt*) manager);
    return sessionForIp6(ip6, sm);
}

struct SessionManager_HandleList* SessionManager_getHandleList(struct SessionManager* st,
                                                               struct Allocator* alloc)
{
    struct SessionManager_HandleList* out =
        Allocator_calloc(alloc, sizeof(struct SessionManager_HandleList), 1);
    uint32_t* buff = Allocator_calloc(alloc, 4, st->ifaceMap.count);
    Bits_memcpy(buff, st->ifaceMap.handles, 4 * st->ifaceMap.count);
    out->handles = buff;
    out->count = st->ifaceMap.count;
    for (int i = 0; i < out->length; i++) {
        buff[i] += st->first;
    }
    return out;
}


static struct SessionManager_Session_pvt* getSession(struct SessionManager_pvt* sm,
                                                     uint8_t ip6[16],
                                                     uint8_t pubKey[32],
                                                     uint32_t version,
                                                     uint64_t label)
{
    struct SessionManager_Session_pvt* sess = sessionForIp6(ip6, sm);
    if (sess) {
        sess->pub.version = (sess->pub.version) ? sess->pub.version : version;
        sess->pub.sendSwitchLabel = (sess->pub.sendSwitchLabel) ? sess->pub.sendSwitchLabel : label;
        return sess;
    }
    struct Allocator* alloc = Allocator_child(sm->alloc);
    sess = Allocator_calloc(alloc, sizeof(struct SessionManager_Session_pvt), 1);
    Identity_set(sess);

    sess->pub.caSession = CryptoAuth_newSession(pubKey, ip6, false, "inner", sm->cryptoAuth);
    int ifaceIndex = Map_OfSessionsByIp6_put((struct Ip6*)ip6, &ss, &sm->ifaceMap);
    sess->receiveHandle = sm->ifaceMap.handles[ifaceIndex] + sm->firstHandle;
    check(sm, ifaceIndex);

    sess->alloc = alloc;
    sess->sessionManager = sm;
    sess->pub.version = version;
    sess->pub.timeOfCreation = Time_currentTimeMilliseconds(sm->eventBase);
    sess->pub.sendSwitchLabel = label;
    Allocator_onFree(alloc, sessionCleanup, sess);
    sendSession(sess, label, 0xffffffff, PFChan_Core_SESSION);
    return sess;
}

static Iface_DEFUN incomingFromSwitchIf(struct Message* msg, struct Iface* iface)
{
    struct SessionManager_pvt* sm =
        Identity_containerOf(iface, struct SessionManager_pvt, pub.switchIf);

    // SwitchHeader, handle, small cryptoAuth header
    if (msg->length < SwitchHeader_SIZE + 4 + 20) {
        Log_debug(sm->log, "DROP runt");
        return NULL;
    }

    struct SwitchHeader* switchHeader = (struct SwitchHeader*) msg->bytes;
    Message_shift(msg, -SwitchHeader_SIZE, NULL);

    struct SessionManager_Session* session;
    uint32_t nonceOrHandle = Endian_bigEndianToHost32(((uint32_t*)msg->bytes)[0]);
    if (nonceOrHandle > 3) {
        // > 3 it's a handle.
        session = sessionForHandle(nonceOrHandle, sm);
        if (!session) {
            Log_debug(sm->log, "DROP message with unrecognized handle");
            return NULL;
        }
        Message_shift(msg, -4, NULL);
    } else {
        // handle + big cryptoauth header
        if (msg->length < CryptoHeader_SIZE + 4) {
            Log_debug(sm->log, "DROP runt");
            return NULL;
        }
        union CryptoHeader* caHeader = (union CryptoHeader*) msg->bytes;
        uint8_t* herKey = caHeader->handshake.publicKey;
        uint8_t ip6[16];
        // a packet which claims to be "from us" causes problems
        if (!AddressCalc_addressForPublicKey(ip6, herKey)) {
            Log_debug(sm->log, "DROP Handshake with non-fc key");
            return NULL;
        }

        if (!Bits_memcmp(herKey, sm->ca->publicKey, 32)) {
            Log_debug(sm->log, "DROP Handshake from 'ourselves'");
            return NULL;
        }

        uint64_t label = Endian_bigEndianToHost64(switchHeader->label_be);
        session = getSession(sm, ip6, herKey, 0, label);
        debugHandlesAndLabel(sm->log, session, label, "new session nonce[%d]", nonceOrHandle);
    }

    if (CryptoAuth_decrypt(session->caSession, msg)) {
        debugHandlesAndLabel(sm->log, session,
                             Endian_bigEndianToHost64(switchHeader->label_be),
                             "DROP Failed decrypting message NoH[%d] state[%s]",
                             nonceOrHandle,
                             CryptoAuth_stateString(CryptoAuth_getState(session->caSession)));
        return NULL;
    }

    bool currentMessageSetup = (nonceOrHandle <= 3);

    if (currentMessageSetup) {
        session->sendHandle = Message_pop32(msg, NULL);
    }

    Message_shift(msg, RouteHeader_SIZE, NULL);
    struct RouteHeader* header = (struct RouteHeader*) msg->bytes;

    if (currentMessageSetup) {
        Bits_memcpyConst(&header->sh, switchHeader, SwitchHeader_SIZE);
        debugHandlesAndLabel0(sm->log,
                              session,
                              Endian_bigEndianToHost64(switchHeader->label_be),
                              "received start message");
    } else {
        // RouteHeader is laid out such that no copy of switch header should be needed.
        Assert_true(&header->sh == switchHeader);
        debugHandlesAndLabel0(sm->log,
                              session,
                              Endian_bigEndianToHost64(switchHeader->label_be),
                              "received run message");
    }

    header->version_be = Endian_hostToBigEndian32(session->version);
    Bits_memcpyConst(header->ip6, session->caSession->herIp6, 16);
    Bits_memcpyConst(header->publicKey, session->caSession->herPublicKey, 32);

    uint64_t path = Endian_bigEndianToHost64(switchHeader->label_be);
    if (!session->sendSwitchLabel) {
        session->sendSwitchLabel = path;
    }
    if (path != session->recvSwitchLabel) {
        session->recvSwitchLabel = path;
        sendSession(sm, session, path, 0xffffffff, PFChan_Core_DISCOVERED_PATH);
    }

    return Iface_next(&sm->pub.insideIf, msg);
}

static void checkTimedOutBuffers(void* vSessionManager)
{
    struct SessionManager_pvt* sm = Identity_check((struct SessionManager_pvt*) vSessionManager);
    for (int i = 0; i < (int)sm->bufMap.count; i++) {
        struct BufferedMessage* buffered = sm->bufMap.values[i];
        uint64_t lag = Time_currentTimeSeconds(sm->eventBase) - buffered->timeSent;
        if (lag < 10) { continue; }
        Map_BufferedMessages_remove(i, &sm->bufMap);
        Allocator_free(buffered->alloc);
        i--;
    }
}

static void needsLookup(struct SessionManager_pvt* sm, struct Message* msg)
{
    struct RouteHeader* header = (struct RouteHeader*) msg->bytes;
    if (Defined(Log_DEBUG)) {
        uint8_t ipStr[40];
        AddrTools_printIp(ipStr, header->ip6);
        Log_debug(sm->log, "Buffering a packet to [%s] and beginning a search", ipStr);
    }
    int index = Map_BufferedMessages_indexForKey((struct Ip6*)header->ip6, &sm->bufMap);
    if (index > -1) {
        struct BufferedMessage* buffered = sm->bufMap.values[index];
        Map_BufferedMessages_remove(index, &sm->bufMap);
        Allocator_free(buffered->alloc);
        Log_debug(sm->log, "DROP message which needs lookup because new one received");
    }
    if ((int)sm->bufMap.count >= sm->pub.maxBufferedMessages) {
        checkTimedOutBuffers(sm);
        if ((int)sm->bufMap.count >= sm->pub.maxBufferedMessages) {
            Log_debug(sm->log, "DROP message needing lookup maxBufferedMessages ([%d]) is reached",
                      sm->pub.maxBufferedMessages);
            return;
        }
    }
    struct Allocator* lookupAlloc = Allocator_child(sm->alloc);
    struct BufferedMessage* buffered =
        Allocator_calloc(lookupAlloc, sizeof(struct BufferedMessage), 1);
    buffered->msg = msg;
    buffered->alloc = lookupAlloc;
    buffered->timeSent = Time_currentTimeSeconds(sm->eventBase);
    Allocator_adopt(lookupAlloc, msg->alloc);
    Assert_true(Map_BufferedMessages_put((struct Ip6*)header->ip6, &buffered, &sm->bufMap) > -1);

    struct Allocator* eventAlloc = Allocator_child(lookupAlloc);
    struct Message* eventMsg = Message_new(0, 512, eventAlloc);
    Message_push(eventMsg, header->ip6, 16, NULL);
    Message_push32(eventMsg, 0xffffffff, NULL);
    Message_push32(eventMsg, PFChan_Core_SEARCH_REQ, NULL);
    Iface_send(&sm->eventIf, eventMsg);
    Allocator_free(eventAlloc);
}

static Iface_DEFUN readyToSend(struct Message* msg,
                               struct SessionManager_pvt* sm,
                               struct SessionManager_Session* sess)
{
    Message_shift(msg, -RouteHeader_SIZE, NULL);
    struct SwitchHeader* sh;
    CryptoAuth_resetIfTimeout(sess->internal);
    if (CryptoAuth_getState(sess->internal) < CryptoAuth_HANDSHAKE3) {
        // Put the handle into the message so that it's authenticated.
        Message_push32(msg, sess->receiveHandle, NULL);

        // Copy back the SwitchHeader so it is not clobbered.
        Message_shift(msg, (CryptoHeader_SIZE + SwitchHeader_SIZE), NULL);
        Bits_memcpyConst(msg->bytes, &header->sh, SwitchHeader_SIZE);
        sh = (struct SwitchHeader*) msg->bytes;
        Message_shift(msg, -(CryptoHeader_SIZE + SwitchHeader_SIZE), NULL);
    } else {
        struct RouteHeader* header = (struct RouteHeader*) &msg->bytes[-RouteHeader_SIZE];
        sh = &header->sh;
    }

    Assert_true(!CryptoAuth_encrypt(sess->caSession, msg));

    if (CryptoAuth_getState(sess->internal) >= CryptoAuth_HANDSHAKE3) {
        //if (0) { // Noisy
            debugHandlesAndLabel0(sm->log,
                                  sess,
                                  Endian_bigEndianToHost64(sh->label_be),
                                  "sending run message");
        //}
        Message_push32(msg, sess->sendHandle, NULL);
    } else {
        debugHandlesAndLabel0(sm->log,
                              sess,
                              Endian_bigEndianToHost64(sh->label_be),
                              "sending start message");
    }

    // The SwitchHeader should have been moved to the correct location.
    Message_shift(msg, SwitchHeader_SIZE, NULL);
    Assert_true((uint8_t*)sh == msg->bytes);

    return Iface_next(&sm->pub.switchIf, msg);
}

static Iface_DEFUN incomingFromInsideIf(struct Message* msg, struct Iface* iface)
{
    struct SessionManager_pvt* sm =
        Identity_containerOf(iface, struct SessionManager_pvt, pub.insideIf);
    Assert_true(msg->length >= RouteHeader_SIZE);
    struct RouteHeader* header = (struct RouteHeader*) msg->bytes;

    struct SessionManager_Session* sess = sessionForIp6(header->ip6, sm);
    if (!sess) {
        if (!Bits_isZero(header->publicKey, 32)) {
            sess = getSession(sm,
                              header->ip6,
                              header->publicKey,
                              Endian_bigEndianToHost32(header->version_be),
                              Endian_bigEndianToHost64(header->sh.label_be));
        } else {
            needsLookup(sm, msg);
            return NULL;
        }
    }

    if (header->version_be) { sess->version = Endian_bigEndianToHost32(header->version_be); }

    if (header->sh.label_be) {
        // fallthrough
    } else if (sess->sendSwitchLabel) {
        header->sh.label_be = Endian_hostToBigEndian64(sess->sendSwitchLabel);
    } else {
        needsLookup(sm, msg);
        return NULL;
    }

    return readyToSend(msg, sm, sess);
}

/* too good to toss!
static uint32_t getEffectiveMetric(uint64_t nowMilliseconds,
                                   uint32_t metricHalflifeMilliseconds,
                                   uint32_t metric,
                                   uint32_t time)
{
    if (time - nowMilliseconds < 1000 || !metricHalflifeMilliseconds) {
        // Clock wander (reverse) || halflife == 0
        return metric;
    }

    // Please do not store an entry for more than 21 days.
    Assert_true(nowMilliseconds > time);

    uint64_t halflives = nowMilliseconds - time;

    // support fractional halflives...
    halflives <<= 16;

    // now we have numHalflives**16
    halflives /= metricHalflifeMilliseconds;

    uint64_t out = (UINT32_MAX - metric) << 16;

    out /= halflives;

    return UINT32_MAX - out;
}
*/

static Iface_DEFUN sessions(struct SessionManager_pvt* sm,
                            uint32_t sourcePf,
                            struct Allocator* tempAlloc)
{
    struct SessionTable_HandleList* handles =
        SessionTable_getHandleList(sm->pub.sessionTable, tempAlloc);
    for (int i = 0; i < (int)handles->count; i++) {
        struct SessionManager_Session* sess = sessionForHandle(handles->handles[i], sm);
        struct Allocator* alloc = Allocator_child(tempAlloc);
        sendSession(sm, sess, sess->sendSwitchLabel, sourcePf, PFChan_Core_SESSION, alloc);
        Allocator_free(alloc);
    }
    return NULL;
}

static Iface_DEFUN incomingFromEventIf(struct Message* msg, struct Iface* iface)
{
    struct SessionManager_pvt* sm = Identity_containerOf(iface, struct SessionManager_pvt, eventIf);
    enum PFChan_Pathfinder ev = Message_pop32(msg, NULL);
    uint32_t sourcePf = Message_pop32(msg, NULL);
    if (ev == PFChan_Pathfinder_SESSIONS) {
        Assert_true(!msg->length);
        return sessions(sm, sourcePf, msg->alloc);
    }
    Assert_true(ev == PFChan_Pathfinder_NODE);

    struct PFChan_Node node;
    Message_pop(msg, &node, PFChan_Node_SIZE, NULL);
    Assert_true(!msg->length);
    int index = Map_BufferedMessages_indexForKey((struct Ip6*)node.ip6, &sm->bufMap);
    struct SessionManager_Session* sess;
    if (index == -1) {
        sess = sessionForIp6(node.ip6, sm);
        // If we discovered a node we're not interested in ...
        if (!sess) { return NULL; }
        sess->sendSwitchLabel = Endian_bigEndianToHost64(node.path_be);
        sess->version = Endian_bigEndianToHost32(node.version_be);
    } else {
        sess = getSession(sm,
                          node.ip6,
                          node.publicKey,
                          Endian_bigEndianToHost32(node.version_be),
                          Endian_bigEndianToHost64(node.path_be));
    }

    // Send what's on the buffer...
    if (index > -1) {
        struct BufferedMessage* bm = sm->bufMap.values[index];
        Iface_CALL(readyToSend, bm->msg, sm, sess);
        Map_BufferedMessages_remove(index, &sm->bufMap);
        Allocator_free(bm->alloc);
    }
    return NULL;
}

struct SessionManager* SessionManager_new(struct Allocator* alloc,
                                          struct EventBase* eventBase,
                                          struct CryptoAuth* cryptoAuth,
                                          struct Random* rand,
                                          struct Log* log,
                                          struct EventEmitter* ee)
{
    struct SessionManager_pvt* sm = Allocator_calloc(alloc, sizeof(struct SessionManager_pvt), 1);
    sm->alloc = alloc;
    sm->pub.switchIf.send = incomingFromSwitchIf;
    sm->pub.insideIf.send = incomingFromInsideIf;
    sm->bufMap.allocator = alloc;
    sm->log = log;
    sm->ca = cryptoAuth;
    sm->eventBase = eventBase;

    sm->pub.metricHalflifeMilliseconds = SessionManager_METRIC_HALFLIFE_MILLISECONDS_DEFAULT;
    sm->pub.maxBufferedMessages = SessionManager_MAX_BUFFERED_MESSAGES_DEFAULT;

    sm->eventIf.send = incomingFromEventIf;
    EventEmitter_regCore(ee, &sm->eventIf, PFChan_Pathfinder_NODE);
    EventEmitter_regCore(ee, &sm->eventIf, PFChan_Pathfinder_SESSIONS);

    sm->firstHandle =
        (Random_uint32(rand) % (MAX_FIRST_HANDLE - MIN_FIRST_HANDLE)) + MIN_FIRST_HANDLE;

    sm->pub.sessionTable = SessionTable_new(cryptoAuth, rand, alloc);

    Timeout_setInterval(checkTimedOutBuffers, sm, 10000, eventBase, alloc);

    Identity_set(sm);

    return &sm->pub;
}