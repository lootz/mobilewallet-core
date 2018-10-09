//
//  BREthereumNode.c
//  Core
//
//  Created by Ed Gamble on 8/13/18.
//  Copyright (c) 2018 breadwallet LLC
//
//  Permission is hereby granted, free of charge, to any person obtaining a copy
//  of this software and associated documentation files (the "Software"), to deal
//  in the Software without restriction, including without limitation the rights
//  to use, copy, modify, merge, publish, distribute, sublicense, and/or sell
//  copies of the Software, and to permit persons to whom the Software is
//  furnished to do so, subject to the following conditions:
//
//  The above copyright notice and this permission notice shall be included in
//  all copies or substantial portions of the Software.
//
//  THE SOFTWARE IS PROVIDED "AS IS", WITHOUT WARRANTY OF ANY KIND, EXPRESS OR
//  IMPLIED, INCLUDING BUT NOT LIMITED TO THE WARRANTIES OF MERCHANTABILITY,
//  FITNESS FOR A PARTICULAR PURPOSE AND NONINFRINGEMENT. IN NO EVENT SHALL THE
//  AUTHORS OR COPYRIGHT HOLDERS BE LIABLE FOR ANY CLAIM, DAMAGES OR OTHER
//  LIABILITY, WHETHER IN AN ACTION OF CONTRACT, TORT OR OTHERWISE, ARISING FROM,
//  OUT OF OR IN CONNECTION WITH THE SOFTWARE OR THE USE OR OTHER DEALINGS IN
//  THE SOFTWARE.

#include <unistd.h>
#include <sys/socket.h>
#include <sys/select.h>
#include <pthread.h>
#include <arpa/inet.h>
#include <errno.h>
#include <limits.h>
#include "BRCrypto.h"
#include "BRKeyECIES.h"
#include "BREthereumNode.h"
#include "BREthereumLESFrameCoder.h"

///
/// MARK: - Forward Declarations
///

// #define NODE_SHOW_RLP_ITEMS
// #define NODE_SHOW_RECV_RLP_ITEMS
// #define NODE_SHOW_SEND_RLP_ITEMS

// #define NEED_TO_PRINT_SEND_RECV_DATA
// #define NEED_TO_AVOID_PROOFS_LOGGING

// Chose above; leave the following
#if defined (NODE_SHOW_RLP_ITEMS)
#define NODE_SHOW_RECV_RLP_ITEMS
#define NODE_SHOW_SEND_RLP_ITEMS
#endif

#undef NODE_DEBUG_SOCKETS

#if defined (NODE_DEBUG_SOCKETS)
static int socketOpenCount = 0;
#endif

static BREthereumNodeState
nodeStateAnnounce (BREthereumNode node,
                   BREthereumNodeEndpointRoute route,
                   BREthereumNodeState state);

static void
showEndpointStatusMessage (BREthereumNodeEndpoint *endpoint);

static void
updateLocalEndpointStatusMessage (BREthereumNodeEndpoint *endpoint,
                                  BREthereumNodeType type,
                                  uint64_t protocolVersion);

static BREthereumNodeType
nodeGetType (BREthereumNode node);

static uint64_t
nodeGetThenIncrementMessageIdentifier (BREthereumNode node,
                                       size_t byIncrement);

static BREthereumNodeMessageResult
nodeRecv (BREthereumNode node,
          BREthereumNodeEndpointRoute route);

static BREthereumNodeStatus
nodeSend (BREthereumNode node,
          BREthereumNodeEndpointRoute route,
          BREthereumMessage message);   // BRRlpData/BRRlpItem *optionalMessageData/Item

#define DEFAULT_SEND_DATA_BUFFER_SIZE    (16 * 1024)
#define DEFAULT_RECV_DATA_BUFFER_SIZE    (1 * 1024 * 1024)

#define DEFAULT_NODE_TIMEOUT_IN_SECONDS       (10)
#define DEFAULT_NODE_TIMEOUT_IN_SECONDS_RECV  (10 * 60)

//
// Frame Coder Stuff
//
#define SIG_SIZE_BYTES      65
#define PUBLIC_SIZE_BYTES   64
#define HEPUBLIC_BYTES      32
#define NONCE_BYTES         32

static const ssize_t authBufLen = SIG_SIZE_BYTES + HEPUBLIC_BYTES + PUBLIC_SIZE_BYTES + NONCE_BYTES + 1;
static const ssize_t authCipherBufLen =  authBufLen + 65 + 16 + 32;

static const ssize_t ackBufLen = PUBLIC_SIZE_BYTES + NONCE_BYTES + 1;
static const ssize_t ackCipherBufLen =  ackBufLen + 65 + 16 + 32;

//
static int _sendAuthInitiator(BREthereumNode node);
static int _readAuthAckFromRecipient(BREthereumNode node);

///
/// MARK: - Node Type
///

extern const char *
nodeTypeGetName (BREthereumNodeType type) {
    static const char *nodeTypeNames[] = {
        "Unknown",
        "Geth",
        "Parity"
    };
    return nodeTypeNames[type];
}

///
/// MARK: - Node State Create ...
///

static inline BREthereumNodeState
nodeStateCreate (BREthereumNodeStateType type) {
    return (BREthereumNodeState) { type };
}

static BREthereumNodeState
nodeStateCreateConnecting (BREthereumNodeConnectType type) {
    return (BREthereumNodeState) {
        NODE_CONNECTING,
        { .connecting = { type }}
    };
}

static BREthereumNodeState
nodeStateCreateConnected (void) {
    return nodeStateCreate (NODE_CONNECTED);
}

static BREthereumNodeState
nodeStateCreateErrorUnix (int error) {
    return (BREthereumNodeState) {
        NODE_ERROR,
        { .error = {
            NODE_ERROR_UNIX,
            { .unix = error }}}
    };
}

static BREthereumNodeState
nodeStateCreateErrorDisconnect (BREthereumP2PDisconnectReason reason) {
    return (BREthereumNodeState) {
        NODE_ERROR,
        { .error = {
            NODE_ERROR_DISCONNECT,
            { .disconnect = reason }}}
    };
}

static BREthereumNodeState
nodeStateCreateErrorProtocol (BREthereumNodeProtocolReason reason) {
    return (BREthereumNodeState) {
        NODE_ERROR,
        { .error = {
            NODE_ERROR_PROTOCOL,
            { .protocol = reason }}}
    };
}

const char *
nodeProtocolReasonDescription (BREthereumNodeProtocolReason reason) {
    static const char *
    protocolReasonDescriptions [] = {
        "Exhausted",
        "Non-Standard Port",
        "UDP Ping_Pong Missed",
        "UDP Excessive Byte Count",
        "TCP Authentication",
        "TCP Hello Missed",
        "TCP Status Missed",
        "Capabilities Mismatch",
        "Status Mismatch",
        "RLP Parse"
    };
    return protocolReasonDescriptions [reason];
}

static const char *
nodeConnectTypeDescription (BREthereumNodeConnectType type) {
    static const char *
    connectTypeDescriptions [] = {
        "Open",
        "Auth",
        "Auth Ack",
        "Hello",
        "Hello Ack",
        "PreStatus Ping Recv",
        "PreStatus Pong Send",
        "Status",
        "Status Ack",
        "Ping",
        "Ping Ack",
        "Ping Ack Discover",
        "Ping Ack Discover Ack",
        "Discover",
        "Discover Ack"
    };
    return connectTypeDescriptions [type];
}

extern const char *
nodeStateDescribe (const BREthereumNodeState *state,
                   char description[128]) {
    switch (state->type) {
        case NODE_AVAILABLE:  return strcpy (description, "Available");
        case NODE_CONNECTING: return strcat (strcpy (description, "Connecting: "),
                                             nodeConnectTypeDescription(state->u.connecting.type));
        case NODE_CONNECTED:  return strcpy (description, "Connected");
        case NODE_ERROR:
            switch (state->u.error.type) {
                case NODE_ERROR_UNIX:
                    return strcat (strcpy (description, "Unix: "),
                                   strerror (state->u.error.u.unix));
                case NODE_ERROR_DISCONNECT:
                    return strcat (strcpy (description, "Disconnect : "),
                                   messageP2PDisconnectDescription(state->u.error.u.disconnect));
                case NODE_ERROR_PROTOCOL:
                    return strcat (strcpy (description, "Protocol  : "),
                                   nodeProtocolReasonDescription(state->u.error.u.protocol));
                    break;
            }
    }
}

extern BRRlpItem
nodeStateEncode (const BREthereumNodeState *state,
                 BRRlpCoder coder) {
    BRRlpItem typeItem = rlpEncodeUInt64 (coder, state->type, 0);

    switch (state->type) {
        case NODE_AVAILABLE:
        case NODE_CONNECTING:
        case NODE_CONNECTED:
            return rlpEncodeList1 (coder, typeItem);
        case NODE_ERROR: {
            BRRlpItem reasonItem;
            switch (state->u.error.type) {
                case NODE_ERROR_UNIX:
                    reasonItem = rlpEncodeUInt64(coder, state->u.error.u.unix, 1);
                    break;
                case NODE_ERROR_DISCONNECT:
                    reasonItem = rlpEncodeUInt64(coder, state->u.error.u.disconnect, 1);
                    break;
                case NODE_ERROR_PROTOCOL:
                    reasonItem = rlpEncodeUInt64(coder, state->u.error.u.protocol, 1);
                    break;
            }

            return rlpEncodeList (coder, 3,
                                  typeItem,
                                  rlpEncodeUInt64(coder, state->u.error.type, 1),
                                  reasonItem);
            }
    }
}

extern BREthereumNodeState
nodeStateDecode (BRRlpItem item,
                 BRRlpCoder coder) {
    size_t itemsCount = 0;
    const BRRlpItem *items = rlpDecodeList (coder, item, &itemsCount);
    assert (1 == itemsCount || 3 == itemsCount);

    BREthereumNodeStateType type = (BREthereumNodeStateType) rlpDecodeUInt64(coder, items[0], 0);
    switch (type) {
        case NODE_AVAILABLE:
        case NODE_CONNECTING:
        case NODE_CONNECTED:
            return nodeStateCreate(type);
        case NODE_ERROR: {
            BREthereumNodeErrorType errorType = (BREthereumNodeErrorType) rlpDecodeUInt64 (coder, items[1], 0);
            switch (errorType) {
                case NODE_ERROR_UNIX:
                    return nodeStateCreateErrorUnix((int) rlpDecodeUInt64 (coder, items[1], 0));
                case NODE_ERROR_DISCONNECT:
                    return nodeStateCreateErrorDisconnect((BREthereumP2PDisconnectReason) rlpDecodeUInt64 (coder, items[1], 0));
                case NODE_ERROR_PROTOCOL:
                    return nodeStateCreateErrorProtocol((BREthereumNodeProtocolReason) rlpDecodeUInt64 (coder, items[1], 0));
            }
        }
    }
}

///
/// MARK: - Node Provisioner
///

/**
 * A Node Provisioner completes a Provision by dispatching messages, possibly multiple
 * messages, to fill the provision.  The number of messages dispatch depends on type of the message
 * and the content requests.  For example, if 192 block bodies are requested but a block bodies'
 * LES message only accepts at most 64 hashes, then 3 messages will be created, each with 64
 * hashes, to complete the provision of 192 headers.  Only when all 192 headers are received will
 * the provisioner be complete.
 */
typedef struct {
    /** The provision as a union of {reqeust, response} for each provision type. */
    BREthereumProvision provision;

    /** The node handling this provision.  How the provision is completed is determined by this
     * node; notably, different messages are sent based on if the node is for GETH or PARITY */
    BREthereumNode node;

    /** The base message identifier.  If the provision applies to multiple messages, then
     * the messages identifers will be sequential starting at this identifier */
    uint64_t messageIdentifier;

    /** The count of messages */
    size_t messagesCount;

    /** The limit for each message.  When constructing the 'response' from a set of messages
     * we'll expect eash message to have this many individual responses (except for the last
     * message which may have fewer). */
    size_t messageContentLimit;

    /** The count of messages remaining to be sent */
    size_t messagesRemainingCount;

    /** The count of messages received */
    size_t messagesReceivedCount;

    /** Time of creation */
    long timestamp;

    /** The messages needed to complete the provision.  These may be LES (for GETH) or PIP (for
     * Parity) messages. */
    BRArrayOf(BREthereumMessage) messages;

} BREthereumNodeProvisioner;

static int
provisionerSendMessagesPending (BREthereumNodeProvisioner *provisioner) {
    return provisioner->messagesRemainingCount > 0;
}

static int
provisionerRecvMessagesPending (BREthereumNodeProvisioner *provisioner) {
    return provisioner->messagesReceivedCount < provisioner->messagesCount;
}

static int
provisionerMessageOfInterest (BREthereumNodeProvisioner *provisioner,
                              uint64_t messageIdentifier) {
    return (provisioner->messageIdentifier <= messageIdentifier &&
            messageIdentifier < (provisioner->messageIdentifier + provisioner->messagesCount));
}

static BREthereumNodeStatus
provisionerMessageSend (BREthereumNodeProvisioner *provisioner) {
    BREthereumMessage message = provisioner->messages [provisioner->messagesCount -
                                                       provisioner->messagesRemainingCount];
    BREthereumNodeStatus status = nodeSend (provisioner->node, NODE_ROUTE_TCP, message);
    provisioner->messagesRemainingCount--;

    return status;
}

static uint64_t
provisionerGetCount (BREthereumNodeProvisioner *provisioner) {
    switch (provisioner->provision.type) {
        case PROVISION_BLOCK_HEADERS:
            return provisioner->provision.u.headers.limit;
        case PROVISION_BLOCK_BODIES:
            return array_count (provisioner->provision.u.bodies.hashes);
        case PROVISION_TRANSACTION_RECEIPTS:
            return array_count (provisioner->provision.u.receipts.hashes);
        case PROVISION_ACCOUNTS:
            return array_count (provisioner->provision.u.accounts.hashes);
        case PROVISION_TRANSACTION_STATUSES:
            return array_count (provisioner->provision.u.statuses.hashes);
        case PROVISION_SUBMIT_TRANSACTION:
            // We'll submit the transaction and then query it's status.  We'll only expect
            // one response.. which makes this different from all the other messages and thus
            // see how provisioner->messagesReceivedCount is handled in `provisionerEstablish()`.
            return 2;
    }
}

static size_t
provisionerGetMessageContentLimit (BREthereumNodeProvisioner *provisioner) {
    assert (NULL != provisioner->node);

    switch (nodeGetType(provisioner->node)) {
        case NODE_TYPE_UNKNOWN:
            assert (0);
        case NODE_TYPE_GETH: {
            BREthereumLESMessageIdentifier id = provisionGetMessageLESIdentifier(provisioner->provision.type);
            return messageLESSpecs[id].limit;
        }
        case NODE_TYPE_PARITY:
            // The Parity code seems to have this implicit limit.
            return 256;
    }
}

static void
provisionerEstablish (BREthereumNodeProvisioner *provisioner,
                          BREthereumNode node) {
    // The `node` will handle the `provisioner`
    provisioner->node = node;

    // A message of `type` is limited to this number 'requests'
    provisioner->messageContentLimit = provisionerGetMessageContentLimit (provisioner);
    assert (0 != provisioner->messageContentLimit);

    // We'll need this many messages to handle all the 'requests'
    provisioner->messagesCount = (provisionerGetCount (provisioner) + provisioner->messageContentLimit - 1) / provisioner->messageContentLimit;

    // Set the `messageIdentifier` and the `messagesRemainingCount` given the `messagesCount`
    provisioner->messageIdentifier = nodeGetThenIncrementMessageIdentifier (node, provisioner->messagesCount);
    provisioner->messagesRemainingCount = provisioner->messagesCount;

    // For SUBMIT_TRANSACTION we send two messages but only expect one back; fake receivedCount.
    provisioner->messagesReceivedCount  = (PROVISION_SUBMIT_TRANSACTION == provisioner->provision.type
                                           ? 1
                                           : 0);

    // Create the messages, or just one, needed to complete the provision
    array_new (provisioner->messages, provisioner->messagesCount);

    // Add each message, constructed from the provision
    for (size_t index = 0; index < provisioner->messagesCount; index++)
        array_add (provisioner->messages, provisionCreateMessage (&provisioner->provision,
                                                                  (NODE_TYPE_GETH == nodeGetType(node)
                                                                   ? MESSAGE_LES
                                                                   : MESSAGE_PIP),
                                                                  provisioner->messageContentLimit,
                                                                  provisioner->messageIdentifier,
                                                                  index));
}

static void
provisionerHandleMessage (BREthereumNodeProvisioner *provisioner,
                          BREthereumMessage message) {
    provisionHandleMessage (&provisioner->provision,
                            message,
                            provisioner->messageContentLimit,
                            provisioner->messageIdentifier);

    // We've processed another message;
    provisioner->messagesReceivedCount++;
}

///
/// MARK: - LES Node
///

struct BREthereumNodeRecord {
    // Must be first to support BRSet.
    /**
     * The identifer is the hash of the remote node endpoing.
     */
    BREthereumHash hash;

    /** The type as GETH or PARITY (only GETH supported) */
    BREthereumNodeType type;

    /** The states by route; one for UDP and one for TCP */
    BREthereumNodeState states[NUMBER_OF_NODE_ROUTES];

    // The endpoints connected by this node
    BREthereumNodeEndpoint local;
    BREthereumNodeEndpoint remote;

    // The DIS distance between local <==> remote.
    UInt256 distance;

    /** The message specs by identifier.  Includes credit params and message count limits */
    // TODO: This should not be LES specific; applies to PIP too.
    BREthereumLESMessageSpec specs [NUMBER_OF_LES_MESSAGE_IDENTIFIERS];

    /** Credit remaining (if not zero) */
    uint64_t credits;

    /** Callbacks */
    BREthereumNodeContext callbackContext;
    BREthereumNodeCallbackStatus callbackStatus;
    BREthereumNodeCallbackAnnounce callbackAnnounce;
    BREthereumNodeCallbackProvide callbackProvide;
    BREthereumNodeCallbackNeighbor callbackNeighbor;

    /** Send/Recv Buffer */
    BRRlpData sendDataBuffer;
    BRRlpData recvDataBuffer;

    /** Message Coder - remember 'not thread safe'! */
    BREthereumMessageCoder coder;

    /** TRUE if we've discovered the neighbors of this node */
    BREthereumBoolean discovered;

    /** When waiting to receive a message, timeout when time exceeds `timeout`. */
    time_t timeout;

    /** Frame Coder */
    BREthereumLESFrameCoder frameCoder;
    uint8_t authBuf[authBufLen];
    uint8_t authBufCipher[authCipherBufLen];
    uint8_t ackBuf[ackBufLen];
    uint8_t ackBufCipher[ackCipherBufLen];

    // Provision
    uint64_t messageIdentifier;

    BRArrayOf(BREthereumNodeProvisioner) provisioners;

    // A largely unneeded lock.
    pthread_mutex_t lock;
};

static BREthereumNodeType
nodeGetType (BREthereumNode node) {
    return node->type;
}

extern void
nodeShow (BREthereumNode node) {
    char descUDP[128], descTCP[128];

    BREthereumDISNeighborEnode enode = neighborDISAsEnode (node->remote.dis, 1);
    eth_log (LES_LOG_TOPIC, "Node: %15s", node->remote.hostname);
    eth_log (LES_LOG_TOPIC, "   NodeID    : %s", enode.chars);
    eth_log (LES_LOG_TOPIC, "   Type      : %s", nodeTypeGetName(node->type));
    eth_log (LES_LOG_TOPIC, "   UDP       : %s", nodeStateDescribe (&node->states[NODE_ROUTE_UDP], descUDP));
    eth_log (LES_LOG_TOPIC, "   TCP       : %s", nodeStateDescribe (&node->states[NODE_ROUTE_TCP], descTCP));
    eth_log (LES_LOG_TOPIC, "   Discovered: %s", (ETHEREUM_BOOLEAN_IS_TRUE(node->discovered) ? "Yes" : "No"));
    eth_log (LES_LOG_TOPIC, "   Credits   : %llu", node->credits);
}

extern BREthereumNodeEndpoint *
nodeGetRemoteEndpoint (BREthereumNode node) {
    return &node->remote;
}

extern BREthereumNodeEndpoint *
nodeGetLocalEndpoint (BREthereumNode node) {
    return &node->local;
}


extern BREthereumComparison
nodeNeighborCompare (BREthereumNode n1,
                     BREthereumNode n2) {
    switch (compareUInt256 (n1->distance, n2->distance)) {
        case -1: return ETHEREUM_COMPARISON_LT;
        case  0: return ETHEREUM_COMPARISON_EQ;
        case +1: return ETHEREUM_COMPARISON_GT;
        default: assert (0);
    }
}

static uint64_t
nodeGetThenIncrementMessageIdentifier (BREthereumNode node,
                                       size_t byIncrement) {
    uint64_t identifier;
    pthread_mutex_lock(&node->lock);
    identifier = node->messageIdentifier;
    node->messageIdentifier += byIncrement;
    pthread_mutex_unlock(&node->lock);
    return identifier;
}

static BREthereumNodeState
nodeStateAnnounce (BREthereumNode node,
                   BREthereumNodeEndpointRoute route,
                   BREthereumNodeState state) {
    node->states [route] = state;
    return state;
}

extern int
nodeHasState (BREthereumNode node,
              BREthereumNodeEndpointRoute route,
              BREthereumNodeStateType type) {
    return type == node->states[route].type;
}

static int
nodeHasErrorState (BREthereumNode node,
                   BREthereumNodeEndpointRoute route) {
    switch (node->states[route].type) {
        case NODE_AVAILABLE:
        case NODE_CONNECTING:
        case NODE_CONNECTED:
            return 0;
        case NODE_ERROR:
            return 1;
    }
}

extern BREthereumNodeState
nodeGetState (BREthereumNode node,
              BREthereumNodeEndpointRoute route) {
    return node->states[route];
}

static void
nodeSetStateErrorProtocol (BREthereumNode node,
                           BREthereumNodeEndpointRoute route,
                           BREthereumNodeProtocolReason reason) {
    node->states[route] = nodeStateCreateErrorProtocol(reason);
}

// Support BRSet
extern size_t
nodeHashValue (const void *h) {
    return hashSetValue(&((BREthereumNode) h)->hash);
}

// Support BRSet
extern int
nodeHashEqual (const void *h1, const void *h2) {
    return h1 == h2 || hashSetEqual (&((BREthereumNode) h1)->hash,
                                     &((BREthereumNode) h2)->hash);
}

//
// Create
//
extern BREthereumNode
nodeCreate (BREthereumNetwork network,
            BREthereumNodeEndpoint remote,  // remote, local ??
            BREthereumNodeEndpoint local,
            BREthereumNodeContext context,
            BREthereumNodeCallbackStatus callbackStatus,
            BREthereumNodeCallbackAnnounce callbackAnnounce,
            BREthereumNodeCallbackProvide callbackProvide,
            BREthereumNodeCallbackNeighbor callbackNeighbor) {
    BREthereumNode node = calloc (1, sizeof (struct BREthereumNodeRecord));

    // Identify this `node` with the remote hash.
    node->hash = remote.hash;

    // Fixed the type as GETH (for now, at least).
    node->type = NODE_TYPE_UNKNOWN;

    // Make all routes as 'available'
    for (int route = 0; route < NUMBER_OF_NODE_ROUTES; route++)
        node->states[route] = nodeStateCreate(NODE_AVAILABLE);

    // Save the local and remote nodes.
    node->local  = local;
    node->remote = remote;

    // Compute the 'DIS Distance' between the two endpoints.  We'll favor nodes with a
    // remote endpoint that is closer to our local endpoint.
    node->distance = neighborDISDistance(node->local.dis, node->remote.dis);

    // Fill in the specs with default values (for GETH)
    for (int i = 0; i < NUMBER_OF_LES_MESSAGE_IDENTIFIERS; i++)
        node->specs[i] = messageLESSpecs[i];

    // No credits, yet.
    node->credits = 0;

    node->sendDataBuffer = (BRRlpData) { DEFAULT_SEND_DATA_BUFFER_SIZE, malloc (DEFAULT_SEND_DATA_BUFFER_SIZE) };
    node->recvDataBuffer = (BRRlpData) { DEFAULT_RECV_DATA_BUFFER_SIZE, malloc (DEFAULT_RECV_DATA_BUFFER_SIZE) };

    // Define the message coder
    node->coder.network = network;
    node->coder.rlp = rlpCoderCreate();
    node->coder.messageIdOffset = 0x00;  // Changed with 'hello' message exchange.

    node->discovered = ETHEREUM_BOOLEAN_FALSE;

    node->frameCoder = frameCoderCreate();
    frameCoderInit(node->frameCoder,
                   &remote.ephemeralKey, &remote.nonce,
                   &local.ephemeralKey,  &local.nonce,
                   node->ackBufCipher, ackCipherBufLen,
                   node->authBufCipher, authCipherBufLen,
                   ETHEREUM_BOOLEAN_TRUE);

    node->callbackContext  = context;
    node->callbackStatus   = callbackStatus;
    node->callbackAnnounce = callbackAnnounce;
    node->callbackProvide  = callbackProvide;
    node->callbackNeighbor = callbackNeighbor;

    node->messageIdentifier = 0;
    array_new (node->provisioners, 10);

    // A remote port (TCP or UDP) of '0' marks this node in error.
    if (0 == remote.dis.node.portTCP)
        nodeSetStateErrorProtocol (node, NODE_ROUTE_TCP, NODE_PROTOCOL_NONSTANDARD_PORT);

    if (0 == remote.dis.node.portUDP)
        nodeSetStateErrorProtocol (node, NODE_ROUTE_UDP, NODE_PROTOCOL_NONSTANDARD_PORT);

    node->timeout = (time_t) -1;
    
    {
        // The cacheLock is a normal, non-recursive lock
        pthread_mutexattr_t attr;
        pthread_mutexattr_init(&attr);
        pthread_mutexattr_settype(&attr, PTHREAD_MUTEX_NORMAL);
        pthread_mutex_init(&node->lock, &attr);
        pthread_mutexattr_destroy(&attr);
    }

    return node;
}

extern void
nodeRelease (BREthereumNode node) {
    nodeDisconnect (node, NODE_ROUTE_TCP, nodeStateCreate (NODE_AVAILABLE), ETHEREUM_BOOLEAN_FALSE);
    nodeDisconnect (node, NODE_ROUTE_UDP, nodeStateCreate (NODE_AVAILABLE), ETHEREUM_BOOLEAN_FALSE);

    if (NULL != node->sendDataBuffer.bytes) free (node->sendDataBuffer.bytes);
    if (NULL != node->recvDataBuffer.bytes) free (node->recvDataBuffer.bytes);

    rlpCoderRelease(node->coder.rlp);
    frameCoderRelease(node->frameCoder);

    pthread_mutex_destroy(&node->lock);
    free (node);
}

static BREthereumNodeState
nodeProcessFailed (BREthereumNode node,
                   BREthereumNodeEndpointRoute route,
                   BREthereumNodeState state) {
    return nodeDisconnect (node, route, state, ETHEREUM_BOOLEAN_FALSE);
}

static BREthereumNodeState
nodeProcessSuccess (BREthereumNode node,
                    BREthereumNodeEndpointRoute route,
                    BREthereumNodeState state) {
    return nodeStateAnnounce(node, route, state);
}

extern BREthereumNodeState
nodeConnect (BREthereumNode node,
             BREthereumNodeEndpointRoute route) {
    int error;

    // Nothing if not AVAILABLE
    if (!nodeHasState (node, route, NODE_AVAILABLE))
        return node->states[route];

#if defined (NODE_DEBUG_SOCKETS)
    // Increment here; on failure we'll decrement.
    eth_log(LES_LOG_TOPIC, "Sockets: %d (Open)", ++socketOpenCount);
#endif


    // Actually open the endpoint connection/port
    error = nodeEndpointOpen (&node->remote, route);
    if (error)
        return nodeProcessFailed (node, route, nodeStateCreateErrorUnix(error));

    // Move to the next state.
    return nodeStateAnnounce(node, route, nodeStateCreateConnecting (NODE_ROUTE_TCP == route
                                                                     ? NODE_CONNECT_AUTH
                                                                     : NODE_CONNECT_PING));
}

extern BREthereumNodeState
nodeDisconnect (BREthereumNode node,
                BREthereumNodeEndpointRoute route,
                BREthereumNodeState stateToAnnounce,
                BREthereumBoolean returnToAvailable) {

    nodeStateAnnounce (node, route, stateToAnnounce);

#if defined (NODE_DEBUG_SOCKETS)
    // Close the appropriate endpoint route
    int failed = nodeEndpointClose (&node->remote, route, !nodeHasErrorState (node, route));
    if (!failed) eth_log(LES_LOG_TOPIC, "Sockets: %d (Closed)", --socketOpenCount);
#else
    nodeEndpointClose (&node->remote, route, !nodeHasErrorState (node, route));
#endif


    if (ETHEREUM_BOOLEAN_IS_TRUE(returnToAvailable))
        nodeStateAnnounce(node, route, nodeStateCreate (NODE_AVAILABLE));

    return node->states[route];
}

///
/// MARK: - Node Process
///
extern void
nodeHandleProvision (BREthereumNode node,
                     BREthereumProvision provision) {
    BREthereumNodeProvisioner provisioner = { provision };
    array_add (node->provisioners, provisioner);
    // Pass the proper provision reference - so we establish the actual provision
    provisionerEstablish (&node->provisioners[array_count(node->provisioners) - 1], node);
}

extern BRArrayOf(BREthereumProvision)
nodeUnhandleProvisions (BREthereumNode node) {
    BRArrayOf(BREthereumProvision) provisions;
    array_new (provisions, array_count(node->provisioners));
    for (size_t index = 0; index < array_count(node->provisioners); index++)
        array_add (provisions, node->provisioners[index].provision);
    array_clear(node->provisioners);
    return provisions;
}

static void
nodeHandleProvisionerMessage (BREthereumNode node,
                              BREthereumNodeProvisioner *provisioner,
                              BREthereumMessage message) {
    // Let the provisioner handle the message, gathering results as warranted.
    provisionerHandleMessage (provisioner, message);

    // If all messages have been received...
    if (!provisionerRecvMessagesPending(provisioner)) {
        // ... callback the result,
        node->callbackProvide (node->callbackContext,
                               node,
                               (BREthereumProvisionResult) {
                                   provisioner->provision.identifier,
                                   provisioner->provision.type,
                                   PROVISION_SUCCESS,
                                   { .success = { provisioner->provision }}
                               });
        // ... and remove the provisioner
        for (size_t index = 0; index < array_count (node->provisioners); index++)
            if (provisioner == &node->provisioners[index]) {
                // TODO: Memory clean
                array_rm (node->provisioners, index);
                break;
            }
    }
}

static void
nodeProcessRecvP2P (BREthereumNode node,
                    BREthereumNodeEndpointRoute route,
                    BREthereumP2PMessage message) {
    assert (NODE_ROUTE_TCP == route);
    switch (message.identifier) {
        case P2P_MESSAGE_DISCONNECT:
            eth_log (LES_LOG_TOPIC, "Recv: Disconnect: %s", messageP2PDisconnectDescription (message.u.disconnect.reason));
            nodeDisconnect(node, NODE_ROUTE_TCP, nodeStateCreateErrorDisconnect(message.u.disconnect.reason), ETHEREUM_BOOLEAN_FALSE);
            break;

        case P2P_MESSAGE_PING: {
            // Immediately send a poing message
            BREthereumMessage pong = {
                MESSAGE_P2P,
                { .p2p = {
                    P2P_MESSAGE_PONG,
                    {}}}
            };
            if (NODE_STATUS_ERROR == nodeSend (node, NODE_ROUTE_TCP, pong))
                nodeStateAnnounce(node, NODE_ROUTE_TCP, nodeStateCreateErrorProtocol(NODE_PROTOCOL_PING_PONG_MISSED));
            break;
        }

        case P2P_MESSAGE_PONG:
        case P2P_MESSAGE_HELLO:
            eth_log (LES_LOG_TOPIC, "Recv: [ P2P, %15s ] Unexpected",
                     messageP2PGetIdentifierName (message.identifier));
            break;
    }
}

static void
nodeProcessRecvDIS (BREthereumNode node,
                    BREthereumNodeEndpointRoute route,
                    BREthereumDISMessage message) {
    assert (NODE_ROUTE_UDP == route);
    switch (message.identifier) {
        case DIS_MESSAGE_PING: {
            // Immediately send a pong message
            BREthereumMessage pong = {
                MESSAGE_DIS,
                { .dis = {
                    DIS_MESSAGE_PONG,
                    { .pong =
                        messageDISPongCreate (message.u.ping.to,
                                              message.u.ping.hash,
                                              time(NULL) + 1000000) },
                    nodeGetLocalEndpoint(node)->dis.key }}
            };
            if (NODE_STATUS_ERROR == nodeSend (node, NODE_ROUTE_UDP, pong))
                nodeStateAnnounce(node, NODE_ROUTE_TCP, nodeStateCreateErrorProtocol(NODE_PROTOCOL_PING_PONG_MISSED));

            break;
        }

        case DIS_MESSAGE_NEIGHBORS: {
            node->callbackNeighbor (node->callbackContext,
                                    node,
                                    message.u.neighbors.neighbors);
            break;
        }

        case DIS_MESSAGE_PONG:
        case DIS_MESSAGE_FIND_NEIGHBORS:
            eth_log (LES_LOG_TOPIC, "Recv: [ DIS, %15s ] Unexpected",
                     messageDISGetIdentifierName (message.identifier));
            break;
    }
}

static void
nodeProcessRecvLES (BREthereumNode node,
                    BREthereumNodeEndpointRoute route,
                    BREthereumLESMessage message) {
    // eth_log (LES_LOG_TOPIC, "Recv: RequestID: %llu", messageLESGetRequestId (&message));
    assert (NODE_TYPE_GETH == node->type);
    switch (message.identifier) {
        case LES_MESSAGE_STATUS:
            node->callbackStatus (node->callbackContext,
                                  node,
                                  message.u.status.p2p.headHash,
                                  message.u.status.p2p.headNum);
            break;

        case LES_MESSAGE_ANNOUNCE:
            node->callbackAnnounce (node->callbackContext,
                                    node,
                                    message.u.announce.headHash,
                                    message.u.announce.headNumber,
                                    message.u.announce.headTotalDifficulty,
                                    message.u.announce.reorgDepth);
            break;

        case LES_MESSAGE_GET_BLOCK_HEADERS:
        case LES_MESSAGE_GET_BLOCK_BODIES:
        case LES_MESSAGE_GET_RECEIPTS:
        case LES_MESSAGE_GET_PROOFS:
        case LES_MESSAGE_GET_CONTRACT_CODES:
        case LES_MESSAGE_SEND_TX:
        case LES_MESSAGE_GET_HEADER_PROOFS:
        case LES_MESSAGE_GET_PROOFS_V2:
        case LES_MESSAGE_GET_HELPER_TRIE_PROOFS:
        case LES_MESSAGE_SEND_TX2:
        case LES_MESSAGE_GET_TX_STATUS:
            eth_log (LES_LOG_TOPIC, "Recv: [ LES, %15s ] Unexpected Request",
                     messageLESGetIdentifierName (message.identifier));
            break;

        case LES_MESSAGE_CONTRACT_CODES:
        case LES_MESSAGE_HEADER_PROOFS:
        case LES_MESSAGE_HELPER_TRIE_PROOFS:;
            eth_log (LES_LOG_TOPIC, "Recv: [ LES, %15s ] Unexpected Response",
                     messageLESGetIdentifierName (message.identifier));
            break;

        case LES_MESSAGE_BLOCK_HEADERS:
        case LES_MESSAGE_BLOCK_BODIES:
        case LES_MESSAGE_RECEIPTS:
        case LES_MESSAGE_PROOFS:
        case LES_MESSAGE_PROOFS_V2:
        case LES_MESSAGE_TX_STATUS:
            // Find the provisioner applicable to `message`...
            for (size_t index = 0; index < array_count (node->provisioners); index++) {
                BREthereumNodeProvisioner *provisioner = &node->provisioners[index];
                // ... using the message's requestId
                if (provisionerMessageOfInterest (provisioner, messageLESGetRequestId (&message))) {
                    // When found, handle it.
                    nodeHandleProvisionerMessage (node, provisioner,
                                                  (BREthereumMessage) {
                                                      MESSAGE_LES,
                                                      { .les = message }
                                                  });
                    break;
                }
            }
            break;
    }
}

static void
nodeProcessRecvPIP (BREthereumNode node,
                    BREthereumNodeEndpointRoute route,
                    BREthereumPIPMessage message) {
    assert (NODE_TYPE_PARITY == node->type);
    switch (message.type) {
        case PIP_MESSAGE_STATUS:
            node->callbackStatus (node->callbackContext,
                                  node,
                                  message.u.status.p2p.headHash,
                                  message.u.status.p2p.headNum);
            break;

        case PIP_MESSAGE_ANNOUNCE:
            node->callbackAnnounce (node->callbackContext,
                                    node,
                                    message.u.announce.headHash,
                                    message.u.announce.headNumber,
                                    message.u.announce.headTotalDifficulty,
                                    message.u.announce.reorgDepth);
            break;

        case PIP_MESSAGE_REQUEST: {
            BRArrayOf(BREthereumPIPRequestInput) inputs = message.u.request.inputs;
            if (array_count (inputs) > 0)
                eth_log (LES_LOG_TOPIC, "Recv: [ PIP, %15s ] Unexpected Request (%zu)",
                         messagePIPGetRequestName (inputs[0].identifier),
                         array_count(inputs));
            break;
        }

        case PIP_MESSAGE_RESPONSE:
            // Find the provisioner applicable to `message`... it might be (stress 'might be')
            // that node->provisioners is empty at this point.  The node has been 'deactivated' (by
            // LES), the provisions reassigned but still, somehow, we handle this message.
            for (size_t index = 0; index < array_count (node->provisioners); index++) {
                BREthereumNodeProvisioner *provisioner = &node->provisioners[index];
                // ... using the message's requestId
                if (provisionerMessageOfInterest (provisioner, messagePIPGetRequestId (&message))) {
                    // When found, handle it.
                    nodeHandleProvisionerMessage (node, provisioner,
                                                  (BREthereumMessage) {
                                                      MESSAGE_PIP,
                                                      { .pip = message }
                                                  });
                    break;
                }
            }
            break;

        case PIP_MESSAGE_UPDATE_CREDIT_PARAMETERS: {
            // TODO: Process the new credit parameters...

            // ... and then, immediately acknowledge the update.
            BREthereumMessage ack = {
                MESSAGE_PIP,
                { .pip = {
                    PIP_MESSAGE_ACKNOWLEDGE_UPDATE,
                    { .acknowledgeUpdate = {}}}}
            };
            nodeSend (node, NODE_ROUTE_TCP, ack);
            break;
        }

        case PIP_MESSAGE_ACKNOWLEDGE_UPDATE:
        case PIP_MESSAGE_RELAY_TRANSACTIONS:
            // Nobody sends these to us.
            eth_log (LES_LOG_TOPIC, "Recv: [ PIP, %15s ] Unexpected Response",
                     messagePIPGetIdentifierName (message));
            break;
    }
}

static inline void
nodeUpdateTimeout (BREthereumNode node,
                   time_t now) {
    node->timeout = now + DEFAULT_NODE_TIMEOUT_IN_SECONDS;
}

static inline void
nodeUpdateTimeoutRecv (BREthereumNode node,
                   time_t now) {
    node->timeout = now + DEFAULT_NODE_TIMEOUT_IN_SECONDS_RECV;
}

static int
nodeStatusIsSufficient (BREthereumNode node) {
    BREthereumP2PMessageStatusValue remValue;

    // Both LES or both PIP
    assert (node->remote.status.identifier == node->local.status.identifier);

    // Remote is STATUS if LES or PIP
    assert (MESSAGE_LES != node->remote.status.identifier ||
            LES_MESSAGE_STATUS == node->remote.status.u.les.identifier);
    assert (MESSAGE_PIP != node->remote.status.identifier ||
            PIP_MESSAGE_STATUS == node->remote.status.u.pip.type);

    // Local is STATUS if LES or PIP
    assert (MESSAGE_LES != node->local.status.identifier ||
            LES_MESSAGE_STATUS == node->local.status.u.les.identifier);
    assert (MESSAGE_PIP != node->local.status.identifier ||
            PIP_MESSAGE_STATUS == node->local.status.u.pip.type);

    BREthereumP2PMessageStatus *locStatus = (MESSAGE_LES == node->local.status.identifier
                                              ? &node->local.status.u.les.u.status.p2p
                                              : &node->local.status.u.pip.u.status.p2p);

    BREthereumP2PMessageStatus *remStatus = (MESSAGE_LES == node->remote.status.identifier
                                              ? &node->remote.status.u.les.u.status.p2p
                                              : &node->remote.status.u.pip.u.status.p2p);

    // Must be our network
    if (remStatus->chainId != locStatus->chainId)
        return 0;

    // Must be our protocol - doesn't this cause problems?  PIPv1 vs LESV2.  Didn't we check
    // the protocol version when matching capabilities?  Our local node has a 'status' but we
    // can be PIPv1 or LESv2 - do we need two status messages?
    if (remStatus->protocolVersion != locStatus->protocolVersion)
        return 0;

    // Must have blocks in the future
    if (remStatus->headNum <= locStatus->headNum)
        return 0;

    // Must serve headers
    if (!messageP2PStatusExtractValue(remStatus, P2P_MESSAGE_STATUS_SERVE_HEADERS, &remValue) ||
        ETHEREUM_BOOLEAN_IS_FALSE(remValue.u.boolean))
        return 0;

    // Must Relay Tranactions
    if (!messageP2PStatusExtractValue(remStatus, P2P_MESSAGE_STATUS_TX_RELAY, &remValue) ||
        ETHEREUM_BOOLEAN_IS_FALSE(remValue.u.boolean))
        return 0;

    // Must serve state - archival node is '0'
    if (!messageP2PStatusExtractValue(remStatus, P2P_MESSAGE_STATUS_SERVE_STATE_SINCE, &remValue) ||
        remValue.u.integer < locStatus->headNum)
        return 0;

    // Must serve chain - archival node is '1'
    if (!messageP2PStatusExtractValue(remStatus, P2P_MESSAGE_STATUS_SERVE_CHAIN_SINCE, &remValue) ||
        remValue.u.integer < locStatus->headNum + (node->type == NODE_TYPE_PARITY ? 1 : 0))
        return 0;

    return 1;
}

extern BREthereumNodeState
nodeProcess (BREthereumNode node,
             BREthereumNodeEndpointRoute route,
             time_t now,
             fd_set *recv,    // read
             fd_set *send) {  // write
    BREthereumNodeMessageResult result;
    BREthereumMessage message;
    size_t ackCipherBufCount;
    int error;

    int socket = node->remote.sockets[route];

    // Do nothing if there is no socket.
    if (-1 == socket) return node->states[route];

    switch (node->states[route].type) {
            //
            // When CONNECTED:
            //   a) we'll send PIP and LES messages based on provisioned requests.
            //   b) we'll recv any message and dispatch to the appropriate handler.
            // Note: both FD SETS (send and recv) apply when CONNECTED.
            //
            // If we are just waiting to receive (typically an 'ANNOUNCE' message with a new
            // block header), then we are willing to wait for 10 minutes; otherwise if we sent
            // a request, then we are only willing to wait for 10 seconds.
            //
        case NODE_CONNECTED:
            if (FD_ISSET (socket, recv)) {
                nodeUpdateTimeoutRecv(node, now);  // wait w/ a longer timeout.

                // Recv if we can.  Get a result for the provided route; on success dispatch to
                // a protocol-specific (P2P, DIS, ETH, LES and PIP) handler.
                BREthereumNodeMessageResult result = nodeRecv (node, route);
                if (NODE_STATUS_ERROR == result.status)
                    return nodeProcessFailed (node, route, nodeStateCreateErrorProtocol (NODE_PROTOCOL_RLP_PARSE));

                BREthereumMessage message = result.u.success.message;

                switch (message.identifier) {
                    case MESSAGE_P2P:
                        nodeProcessRecvP2P (node, route, message.u.p2p);
                        break;
                    case MESSAGE_DIS:
                        nodeProcessRecvDIS (node, route, message.u.dis);
                        break;
                    case MESSAGE_ETH:
                        assert (0);
                        break;
                    case MESSAGE_LES:
                        nodeProcessRecvLES (node, route, message.u.les);
                        break;
                    case MESSAGE_PIP:
                        nodeProcessRecvPIP (node, route, message.u.pip);
                        break;
                }
            }

            if (FD_ISSET (socket, send)) {
                nodeUpdateTimeout(node, now);   // override prior timeout; expect a response.

                // Send if we can.  Really only applies to provision messages, for PIP and LES, using
                // the TCP route.  We might have multiple provisiones to deal with but we'll send them
                // one at a time to avoid potential blocking.
                switch (route) {
                    case NODE_ROUTE_UDP:
                        break;

                    case NODE_ROUTE_TCP:
                        // Look for the pending message in some provisioner
                        for (size_t index = 0; index < array_count (node->provisioners); index++)
                            if (provisionerSendMessagesPending (&node->provisioners[index])) {
                                BREthereumNodeStatus status = provisionerMessageSend(&node->provisioners[index]);
                                // Only send one at a time - socket might be blocked
                                break;
                            }
                        break;
                }
            }

            return node->states[route];

            //
            // When CONNECTING - we'll deal with sub-states as part of a handshake process.
            //
        case NODE_CONNECTING:
            switch (node->states[route].u.connecting.type) {
                case NODE_CONNECT_OPEN:
                    return node->states[route];

                case NODE_CONNECT_AUTH:
                    assert (NODE_ROUTE_TCP == route);
                    if (!FD_ISSET (socket, send)) return node->states[route];
                    nodeUpdateTimeout(node, now);

                    if (0 != _sendAuthInitiator(node))
                        return nodeProcessFailed (node, NODE_ROUTE_TCP, nodeStateCreateErrorProtocol(NODE_PROTOCOL_TCP_AUTHENTICATION));

                    eth_log (LES_LOG_TOPIC, "Send: [ WIP, %15s ] => %15s", "Auth",    node->remote.hostname);

                    error = nodeEndpointSendData (&node->remote, NODE_ROUTE_TCP, node->authBufCipher, authCipherBufLen); //  "auth initiator");
                    if (error)
                        return nodeProcessFailed (node, NODE_ROUTE_TCP, nodeStateCreateErrorUnix(error));

                    return nodeProcessSuccess (node, route, nodeStateCreateConnecting(NODE_CONNECT_AUTH_ACK));

                case NODE_CONNECT_AUTH_ACK:
                    assert (NODE_ROUTE_TCP == route);
                    if (!FD_ISSET (socket, recv)) return node->states[route];
                    nodeUpdateTimeout(node, now);

                    ackCipherBufCount = ackCipherBufLen;
                    error = nodeEndpointRecvData (&node->remote, NODE_ROUTE_TCP, node->ackBufCipher, &ackCipherBufCount, 1); // "auth ack from receivier"
                    if (error)
                        return nodeProcessFailed (node, NODE_ROUTE_TCP, nodeStateCreateErrorUnix(error));

                    eth_log (LES_LOG_TOPIC, "Recv: [ WIP, %15s ] <= %15s", "Auth Ack",    node->remote.hostname);
                    if (ackCipherBufCount != ackCipherBufLen)
                        return nodeProcessFailed (node, NODE_ROUTE_TCP, nodeStateCreateErrorProtocol(NODE_PROTOCOL_TCP_AUTHENTICATION));

                    if (0 != _readAuthAckFromRecipient (node)) {
                        eth_log (LES_LOG_TOPIC, "%s", "Something went wrong with AUK");
                        return nodeProcessFailed (node, NODE_ROUTE_TCP, nodeStateCreateErrorProtocol(NODE_PROTOCOL_TCP_AUTHENTICATION));
                    }

                    // Initilize the frameCoder with the information from the auth
                    frameCoderInit(node->frameCoder,
                                   &node->remote.ephemeralKey, &node->remote.nonce,
                                   &node->local.ephemeralKey, &node->local.nonce,
                                   node->ackBufCipher, ackCipherBufLen,
                                   node->authBufCipher, authCipherBufLen,
                                   ETHEREUM_BOOLEAN_TRUE);

                    return nodeProcessSuccess (node, route, nodeStateCreateConnecting(NODE_CONNECT_HELLO));

                case NODE_CONNECT_HELLO:
                    assert (NODE_ROUTE_TCP == route);
                    if (!FD_ISSET (socket, send)) return node->states[route];
                    nodeUpdateTimeout(node, now);

                    message = (BREthereumMessage) {
                            MESSAGE_P2P,
                            { .p2p = node->local.hello }
                        };
                        if (NODE_STATUS_ERROR == nodeSend (node, NODE_ROUTE_TCP, message))
                            return nodeProcessFailed (node, NODE_ROUTE_TCP, nodeStateCreateErrorProtocol(NODE_PROTOCOL_TCP_HELLO_MISSED));

                    return nodeProcessSuccess (node, route, nodeStateCreateConnecting(NODE_CONNECT_HELLO_ACK));

                case NODE_CONNECT_HELLO_ACK:
                    assert (NODE_ROUTE_TCP == route);
                    if (!FD_ISSET (socket, recv)) return node->states[route];
                    nodeUpdateTimeout(node, now);

                    result = nodeRecv (node, NODE_ROUTE_TCP);
                    if (NODE_STATUS_ERROR == result.status)
                        return nodeProcessFailed (node, NODE_ROUTE_TCP, nodeStateCreateErrorProtocol(NODE_PROTOCOL_TCP_HELLO_MISSED));

                    message = result.u.success.message;

                    // Handle a disconnect request
                    if (MESSAGE_P2P == message.identifier && P2P_MESSAGE_DISCONNECT == message.u.p2p.identifier)
                        return nodeProcessFailed(node, NODE_ROUTE_TCP, nodeStateCreateErrorDisconnect(message.u.p2p.u.disconnect.reason));

                    // Require a P2P Hello message.
                    if (MESSAGE_P2P != message.identifier || P2P_MESSAGE_HELLO != message.u.p2p.identifier)
                        return nodeProcessFailed (node, NODE_ROUTE_TCP, nodeStateCreateErrorProtocol(NODE_PROTOCOL_TCP_HELLO_MISSED));

                    // Save the 'hello' message received and then move on
                    messageP2PHelloShow (message.u.p2p.u.hello);
                    node->remote.hello = message.u.p2p;

                    // Assign the node type even before checking capabilities.
                    for (size_t ri = 0; ri < array_count(node->remote.hello.u.hello.capabilities); ri++) {
                        if (0 == strcmp ("pip", node->remote.hello.u.hello.capabilities[ri].name)) {
                            node->type = NODE_TYPE_PARITY;
                            break;
                        }
                        else if (0 == strcmp ("les", node->remote.hello.u.hello.capabilities[ri].name)) {
                            node->type = NODE_TYPE_GETH;
                            break;
                        }
                    }

                    // Confirm that the remote has one and only one of the local capabilities.  It is unlikely,
                    // but possible, that a remote offers both LESv2 and PIPv1 capabilities - we aren't interested.
                    BREthereumP2PCapability *capability = NULL;
                    {
                        int capabilitiesMatchCount = 0;

                        BREthereumP2PMessageHello *localHello  = &node->local.hello.u.hello;
                        BREthereumP2PMessageHello *remoteHello = &node->remote.hello.u.hello;
                        for (size_t li = 0; li < array_count(localHello->capabilities); li++)
                            capabilitiesMatchCount += ETHEREUM_BOOLEAN_IS_TRUE (messageP2PHelloHasCapability
                                                                                (remoteHello,
                                                                                 &localHello->capabilities[li]));

                        if (1 != capabilitiesMatchCount)
                            return nodeProcessFailed(node, NODE_ROUTE_TCP, nodeStateCreateErrorProtocol(NODE_PROTOCOL_CAPABILITIES_MISMATCH));

                        // Find the matching capability
                        for (size_t li = 0; li < array_count(localHello->capabilities); li++) {
                            capability = &localHello->capabilities[li];
                            if (ETHEREUM_BOOLEAN_IS_TRUE (messageP2PHelloHasCapability (remoteHello, capability)))
                                break;
                        }
                    }

                    // ... and the protocol version.
                    updateLocalEndpointStatusMessage(&node->local, node->type, capability->version);
                    showEndpointStatusMessage (&node->local);

                    // https://github.com/ethereum/wiki/wiki/ÐΞVp2p-Wire-Protocol
                    // ÐΞVp2p is designed to support arbitrary sub-protocols (aka capabilities) over the basic wire
                    // protocol. Each sub-protocol is given as much of the message-ID space as it needs (all such
                    // protocols must statically specify how many message IDs they require). On connection and
                    // reception of the Hello message, both peers have equivalent information about what
                    // subprotocols they share (including versions) and are able to form consensus over the
                    // composition of message ID space.
                    //
                    // Message IDs are assumed to be compact from ID 0x10 onwards (0x00-0x10 is reserved for
                    // ÐΞVp2p messages) and given to each shared (equal-version, equal name) sub-protocol in
                    // alphabetic order. Sub-protocols that are not shared are ignored. If multiple versions are
                    // shared of the same (equal name) sub-protocol, the numerically highest wins, others are
                    // ignored

                    // We'll trusted (but verified) above that we have one and only one (LES, PIP) subprotocol.
                    node->coder.messageIdOffset = 0x10;

                    // We handle a Parity Race Condition - We cannot send a STATUS message at this point...
                    // At this point in time Parity is constructing/sending a PING message and will be waiting for
                    // a PONG message.  If we send STATUS, Parity will see it but expected a PONG and then will
                    // instantly dump us.
                    //
                    // ... Except, apparently this is not struct as we get dumped no matter what.
                    //

                    // ETH: LES: Send: [ WIP,            Auth ] => 193.70.55.37
                    // ETH: LES: Open: UDP @ 30303 =>   139.99.51.203 Success
                    // ETH: LES: Send: [ DIS,            Ping ] => 139.99.51.203
                    // ETH: LES: Recv: [ WIP,        Auth Ack ] <= 193.70.55.37
                    // ETH: LES: Send: [ P2P,           Hello ] => 193.70.55.37
                    // ETH: LES: Recv: [ P2P,           Hello ] <= 193.70.55.37
                    // ETH: LES: Hello
                    // ETH: LES:     Version     : 5
                    // ETH: LES:     ClientId    : Parity/v1.11.8-stable-c754a02-20180725/x86_64-linux-gnu/rustc1.27.2
                    // ETH: LES:     ListenPort  : 30303
                    // ETH: LES:     NodeId      : 0x81863f47e9bd652585d3f78b4b2ee07b93dad603fd9bc3c293e1244250725998adc88da0cef48f1de89b15ab92b15db8f43dc2b6fb8fbd86a6f217a1dd886701
                    // ETH: LES:     Capabilities:
                    // ETH: LES:         eth = 62
                    // ETH: LES:         eth = 63
                    // ETH: LES:         par = 1
                    // ETH: LES:         par = 2
                    // ETH: LES:         par = 3
                    // ETH: LES:         pip = 1
                    // ETH: LES: StatusMessage:
                    // ETH: LES:     ProtocolVersion: 1
                    // ETH: LES:     NetworkId      : 1
                    // ETH: LES:     HeadNum        : 0
                    // ETH: LES:     HeadHash       : 0xd4e56740f876aef8c010b86a40d5f56745a118d0906a34e69aec8c0db1cb8fa3
                    // ETH: LES:     HeadTd         : 0
                    // ETH: LES:     GenesisHash    : 0xd4e56740f876aef8c010b86a40d5f56745a118d0906a34e69aec8c0db1cb8fa3
                    // ETH: LES: Send: [ PIP,          Status ] => 193.70.55.37
                    // ETH: LES: Recv: [ P2P,            Ping ] <= 193.70.55.37
                    // ETH: LES: Send: [ P2P,            Pong ] => 193.70.55.37
                    // ETH: LES: Recv: TCP @ 30303 =>    193.70.55.37 Error: Connection reset by peer

                    return nodeProcessSuccess (node, route, nodeStateCreateConnecting (NODE_TYPE_PARITY == node->type
                                                                                      ? NODE_CONNECT_PRE_STATUS_PING_RECV
                                                                                      : NODE_CONNECT_STATUS));

                case NODE_CONNECT_PRE_STATUS_PING_RECV:
                    assert (NODE_TYPE_PARITY == node->type);
                    assert (NODE_ROUTE_TCP == route);
                    if (!FD_ISSET (socket, recv))  return node->states[route];
                    nodeUpdateTimeout(node, now);

                    result = nodeRecv (node, NODE_ROUTE_TCP);
                    if (NODE_STATUS_ERROR == result.status)
                        return nodeProcessFailed (node, NODE_ROUTE_TCP, nodeStateCreateErrorProtocol(NODE_PROTOCOL_TCP_STATUS_MISSED));  // PARITY_PING

                    message = result.u.success.message;

                    if (MESSAGE_P2P != message.identifier || P2P_MESSAGE_PING != message.u.p2p.identifier)
                        return nodeProcessFailed (node, NODE_ROUTE_TCP, nodeStateCreateErrorProtocol(NODE_PROTOCOL_TCP_STATUS_MISSED));

                    return nodeProcessSuccess (node, route, nodeStateCreateConnecting (NODE_CONNECT_PRE_STATUS_PONG_SEND));

                case NODE_CONNECT_PRE_STATUS_PONG_SEND:
                    assert (NODE_TYPE_PARITY == node->type);
                    assert (NODE_ROUTE_TCP == route);
                    if (!FD_ISSET (socket, send)) return node->states[route];
                    nodeUpdateTimeout(node, now);

                    BREthereumMessage pong = {
                        MESSAGE_P2P,
                        { .p2p = {
                            P2P_MESSAGE_PONG,
                            {}}}
                    };
                    if (NODE_STATUS_ERROR == nodeSend (node, NODE_ROUTE_TCP, pong))
                        return nodeProcessFailed (node, NODE_ROUTE_TCP, node->states[NODE_ROUTE_TCP]); // nodeStateCreateErrorProtocol(NODE_PROTOCOL_TCP_STATUS_MISSED));  // PARITY_PING

                    return nodeProcessSuccess (node, route, nodeStateCreateConnecting (NODE_CONNECT_STATUS));

                case NODE_CONNECT_STATUS:
                    assert (NODE_ROUTE_TCP == route);
                    if (!FD_ISSET (socket, send)) return node->states[route];
                    nodeUpdateTimeout(node, now);

                    if (NODE_STATUS_ERROR == nodeSend (node, NODE_ROUTE_TCP, node->local.status))
                        return nodeProcessFailed (node, NODE_ROUTE_TCP, nodeStateCreateErrorProtocol(NODE_PROTOCOL_TCP_STATUS_MISSED));  // PARITY_PING

                    return nodeProcessSuccess (node, route, nodeStateCreateConnecting (NODE_CONNECT_STATUS_ACK));

                case NODE_CONNECT_STATUS_ACK:
                    assert (NODE_ROUTE_TCP == route);
                    if (!FD_ISSET (socket, recv)) return node->states[route];
                    nodeUpdateTimeout(node, now);

                    result = nodeRecv (node, NODE_ROUTE_TCP);
                    if (NODE_STATUS_ERROR == result.status)
                        return nodeProcessFailed (node, NODE_ROUTE_TCP, nodeStateCreateErrorProtocol(NODE_PROTOCOL_TCP_STATUS_MISSED));  // PARITY_PING

                    message = result.u.success.message;

                    // Handle a disconnect request
                    if (MESSAGE_P2P == message.identifier && P2P_MESSAGE_DISCONNECT == message.u.p2p.identifier)
                        return nodeProcessFailed(node, NODE_ROUTE_TCP, nodeStateCreateErrorDisconnect(message.u.p2p.u.disconnect.reason));

                    // Handle a ping - send a PONG and then wait again for a status
                    if (MESSAGE_P2P == message.identifier && P2P_MESSAGE_PING == message.u.p2p.identifier) {
                        BREthereumMessage pong = {
                            MESSAGE_P2P,
                            { .p2p = {
                                P2P_MESSAGE_PONG,
                                {}}}
                        };
                        if (NODE_STATUS_ERROR == nodeSend (node, NODE_ROUTE_TCP, pong))
                            return nodeProcessFailed (node, NODE_ROUTE_TCP, nodeStateCreateErrorProtocol(NODE_PROTOCOL_TCP_STATUS_MISSED));  // PARITY_PING

                        // Return an unchanged state - come right back expecting a STATUS ACK
                        return node->states[route];
                    }

                    if (MESSAGE_P2P == message.identifier && P2P_MESSAGE_DISCONNECT == message.u.p2p.identifier)
                        return nodeProcessFailed(node, NODE_ROUTE_TCP, nodeStateCreateErrorDisconnect(message.u.p2p.u.disconnect.reason));

                    // Require a Status message.
                    if ((MESSAGE_LES != message.identifier || LES_MESSAGE_STATUS != message.u.les.identifier) &&
                        (MESSAGE_PIP != message.identifier || PIP_MESSAGE_STATUS != message.u.pip.type))
                        return nodeProcessFailed (node, NODE_ROUTE_TCP, nodeStateCreateErrorProtocol(NODE_PROTOCOL_TCP_STATUS_MISSED));

                    // Save the 'status' message
                    node->remote.status = message;
                    showEndpointStatusMessage (&node->remote);

                    // Require a sufficient remote status.
                    if (!nodeStatusIsSufficient(node))
                        return nodeProcessFailed (node, NODE_ROUTE_TCP, nodeStateCreateErrorProtocol(NODE_PROTOCOL_STATUS_MISMATCH));

                    // Finally, CONNECTED
                    nodeProcessSuccess (node, NODE_ROUTE_TCP, nodeStateCreateConnected());

                    // 'Announce' the STATUS message.
                    switch (node->type) {
                        case NODE_TYPE_UNKNOWN:
                            assert (0);
                        case NODE_TYPE_GETH:
                            nodeProcessRecvLES (node, NODE_ROUTE_TCP, message.u.les);
                            break;
                        case NODE_TYPE_PARITY:
                            nodeProcessRecvPIP (node, NODE_ROUTE_TCP, message.u.pip);
                            break;
                    }

                    // Once connected, use a *much* longer timeout.
                    nodeUpdateTimeoutRecv(node, now);
                    return node->states[route];

                case NODE_CONNECT_PING:
                    assert (NODE_ROUTE_UDP == route);
                    if (!FD_ISSET (socket, send)) return node->states[route];
                    nodeUpdateTimeout(node, now);

                    message = (BREthereumMessage) {
                        MESSAGE_DIS,
                        { .dis = {
                            DIS_MESSAGE_PING,
                            { .ping = messageDISPingCreate (node->local.dis.node, // endpointDISCreate(&node->local),
                                                            node->remote.dis.node, // endpointDISCreate(&node->remote),
                                                            time(NULL) + 1000000) },
                            node->local.dis.key }}
                    };
                    if (NODE_STATUS_ERROR == nodeSend (node, NODE_ROUTE_UDP, message))
                        return nodeProcessFailed (node, NODE_ROUTE_UDP, nodeStateCreateErrorProtocol(NODE_PROTOCOL_PING_PONG_MISSED));

                    return nodeProcessSuccess (node, route, nodeStateCreateConnecting (NODE_CONNECT_PING_ACK));

                case NODE_CONNECT_PING_ACK:
                    assert (NODE_ROUTE_UDP == route);
                    if (!FD_ISSET (socket, recv)) return node->states[route];
                    nodeUpdateTimeout(node, now);

                    result = nodeRecv (node, NODE_ROUTE_UDP);
                    if (NODE_STATUS_ERROR == result.status)
                        return nodeProcessFailed (node, NODE_ROUTE_UDP, nodeStateCreateErrorProtocol(NODE_PROTOCOL_PING_PONG_MISSED));

                    // The PING_ACK must be a PONG message
                    message = result.u.success.message;
                    if (MESSAGE_DIS != message.identifier || DIS_MESSAGE_PONG != message.u.dis.identifier)
                        return nodeProcessFailed (node, NODE_ROUTE_UDP, nodeStateCreateErrorProtocol(NODE_PROTOCOL_PING_PONG_MISSED));

                    return nodeProcessSuccess (node, route, nodeStateCreateConnecting (NODE_CONNECT_PING_ACK_DISCOVER));

                case NODE_CONNECT_PING_ACK_DISCOVER:
                    // GETH and PARITY differ - at this point, we do not know which node type we have.  The GETH
                    // node will send a PING and require a PONG response before answering a FIND_NEIGHBORS.  By
                    // contrast, a PARITY node will not send a PING but will respond to a FIND_NEIGHBORS.
                    //
                    // Thus, if here we wait for a PING then, for a Parity node, we'll timeout as a PING is not
                    // coming.
                    //
                    // But if we send a FIND_NEIGHBORS message, a Geth node will ignore it and a Parity node will
                    // respond.  So, we'll send it and wait for a response.

                    assert (NODE_ROUTE_UDP == route);
                    if (!FD_ISSET (socket, send)) return node->states[route];
                    nodeUpdateTimeout(node, now);

                    // Send a FIND_NEIGHBORS.
                    if (NODE_STATUS_ERROR == nodeDiscover (node, &node->local))
                        return nodeProcessFailed (node, NODE_ROUTE_UDP, nodeStateCreateErrorProtocol(NODE_PROTOCOL_PING_PONG_MISSED)); // discover

                    return nodeProcessSuccess (node, route, nodeStateCreateConnecting (NODE_CONNECT_PING_ACK_DISCOVER_ACK));

                case NODE_CONNECT_PING_ACK_DISCOVER_ACK:
                        // We are waiting for a PING message or a NEIGHBORS message.
                    assert (NODE_ROUTE_UDP == route);
                    if (!FD_ISSET (socket, recv)) return node->states[route];
                    nodeUpdateTimeout(node, now);

                    result = nodeRecv (node, NODE_ROUTE_UDP);
                    if (NODE_STATUS_ERROR == result.status)
                        return nodeProcessFailed (node, NODE_ROUTE_UDP, nodeStateCreateErrorProtocol(NODE_PROTOCOL_PING_PONG_MISSED));

                    // Require a PING message or a NEIGHBORS message
                    message = result.u.success.message;
                    if (MESSAGE_DIS != message.identifier ||
                        (DIS_MESSAGE_PING != message.u.dis.identifier && DIS_MESSAGE_NEIGHBORS != message.u.dis.identifier))
                        return nodeProcessFailed (node, NODE_ROUTE_UDP, nodeStateCreateErrorProtocol(NODE_PROTOCOL_PING_PONG_MISSED));

                    // Connected?

                    // If we got a PING message, then respond with the required PONG
                    if (DIS_MESSAGE_PING == message.u.dis.identifier) {
                        message = (BREthereumMessage) {
                            MESSAGE_DIS,
                            { .dis = {
                                DIS_MESSAGE_PONG,
                                { .pong =
                                    messageDISPongCreate (message.u.dis.u.ping.to,
                                                          message.u.dis.u.ping.hash,
                                                          time(NULL) + 1000000) },
                                nodeGetLocalEndpoint(node)->dis.key }}
                        };
                        // TODO: We could block here - need another state...
                        if (NODE_STATUS_ERROR == nodeSend (node, NODE_ROUTE_UDP, message))
                            return nodeProcessFailed (node, NODE_ROUTE_UDP, nodeStateCreateErrorProtocol(NODE_PROTOCOL_PING_PONG_MISSED));
                    }

                    // Finally, CONNECTED
                    nodeProcessSuccess (node, NODE_ROUTE_UDP, nodeStateCreateConnected());

                    if (DIS_MESSAGE_NEIGHBORS == message.u.dis.identifier) {
                        // We got a NEIGHBORS response - this node is discovered and is Parity
                        nodeSetDiscovered (node, ETHEREUM_BOOLEAN_TRUE);
                        nodeProcessSuccess (node, NODE_ROUTE_UDP, nodeStateCreateConnected());
                        nodeProcessRecvDIS (node, NODE_ROUTE_UDP, message.u.dis);

                        // This is success...
                        return nodeDisconnect(node, NODE_ROUTE_UDP, nodeStateCreate(NODE_AVAILABLE), ETHEREUM_BOOLEAN_FALSE);
                    }

                    return nodeProcessSuccess (node, route, nodeStateCreateConnecting (NODE_CONNECT_DISCOVER));

                case NODE_CONNECT_DISCOVER:
                    assert (NODE_ROUTE_UDP == route);
                    if (!FD_ISSET (socket, send)) return node->states[route];
                    nodeUpdateTimeout(node, now);

                    // Send a FIND_NEIGHBORS.
                    if (NODE_STATUS_ERROR == nodeDiscover (node, &node->local))
                        return nodeProcessFailed (node, NODE_ROUTE_UDP, nodeStateCreateErrorProtocol(NODE_PROTOCOL_PING_PONG_MISSED)); // discover

                    return nodeProcessSuccess (node, route, nodeStateCreateConnecting (NODE_CONNECT_DISCOVER_ACK));

                case NODE_CONNECT_DISCOVER_ACK:
                    assert (NODE_ROUTE_UDP == route);
                    if (!FD_ISSET (socket, recv)) return node->states[route];
                    nodeUpdateTimeout(node, now);

                    result = nodeRecv (node, NODE_ROUTE_UDP);
                    if (NODE_STATUS_ERROR == result.status)
                        return nodeProcessFailed (node, NODE_ROUTE_UDP, nodeStateCreateErrorProtocol(NODE_PROTOCOL_PING_PONG_MISSED));

                    // Require a PING message or a NEIGHBORS message
                    message = result.u.success.message;
                    if (MESSAGE_DIS != message.identifier || DIS_MESSAGE_NEIGHBORS != message.u.dis.identifier)
                        return nodeProcessFailed (node, NODE_ROUTE_UDP, nodeStateCreateErrorProtocol(NODE_PROTOCOL_PING_PONG_MISSED));

                    nodeSetDiscovered (node, ETHEREUM_BOOLEAN_TRUE);
                    nodeProcessSuccess (node, NODE_ROUTE_UDP, nodeStateCreateConnected());
                    nodeProcessRecvDIS (node, NODE_ROUTE_UDP, message.u.dis);

                    // Tjhis is success...
                    return nodeDisconnect(node, NODE_ROUTE_UDP, nodeStateCreate(NODE_AVAILABLE), ETHEREUM_BOOLEAN_FALSE);

            }
            break;

        case NODE_AVAILABLE:
        case NODE_ERROR:
            node->timeout = (time_t) -1;
            return node->states[route];
    }
}

extern int
nodeUpdateDescriptors (BREthereumNode node,
                       BREthereumNodeEndpointRoute route,
                       fd_set *recv,   // read
                       fd_set *send) {  // write
    int socket = node->remote.sockets[route];

    // Do nothing - if there is no socket.
    if (-1 == socket) return -1;

    switch (node->states[route].type) {
        case NODE_AVAILABLE:
        case NODE_ERROR:
            break;

        case NODE_CONNECTED:
            if (NULL != recv)
                FD_SET (socket, recv);

            // If we have any provisioner with a pending message, we are willing to send
            for (size_t index = 0; index < array_count (node->provisioners); index++)
                if (provisionerSendMessagesPending (&node->provisioners[index])) {
                    if (NULL != send) FD_SET (socket, send);
                    break;
                }

            break;

        case NODE_CONNECTING:
            switch (node->states[route].u.connecting.type) {
                case NODE_CONNECT_OPEN:
                    assert (0);

                case NODE_CONNECT_AUTH:
                case NODE_CONNECT_HELLO:
                case NODE_CONNECT_PRE_STATUS_PONG_SEND:
                case NODE_CONNECT_STATUS:
                case NODE_CONNECT_PING:
                case NODE_CONNECT_PING_ACK_DISCOVER:
                case NODE_CONNECT_DISCOVER:
                    if (NULL != send) FD_SET (socket, send);
                    break;

                case NODE_CONNECT_AUTH_ACK:
                case NODE_CONNECT_HELLO_ACK:
                case NODE_CONNECT_PRE_STATUS_PING_RECV:
                case NODE_CONNECT_STATUS_ACK:
                case NODE_CONNECT_PING_ACK:
                case NODE_CONNECT_PING_ACK_DISCOVER_ACK:
                case NODE_CONNECT_DISCOVER_ACK:
                    if (NULL != recv) FD_SET (socket, recv);
                    break;
            }
            break;
    }
    // When connected, we are always willing to recv

    return socket;
}


///
/// MARK: - LES Node Support
///


/**
 * Extract the `type` and `subtype` of a message from the RLP-encoded `value`.  The `value` has
 * any applicable messagerIdOffset applied; thus we need to undo that offset.
 *
 * We've already assumed that we have one subprotocol (LES, PIP) and thus one and only one
 * offset to deal with.
 */
static void
extractIdentifier (BREthereumNode node,
                   uint8_t value,
                   BREthereumMessageIdentifier *type,
                   BREthereumANYMessageIdentifier *subtype) {
    if (value < node->coder.messageIdOffset || 0 == node->coder.messageIdOffset) {
        *type = MESSAGE_P2P;
        *subtype = value - 0x00;
    }
    else {
        switch (node->type) {
            case NODE_TYPE_UNKNOWN:
                assert (0);
            case NODE_TYPE_GETH:
                *type = MESSAGE_LES;
                break;
            case NODE_TYPE_PARITY:
                *type = MESSAGE_PIP;
                break;
        }
        *subtype = value - node->coder.messageIdOffset;
    }
}

/// MARK: LES Node State


extern void
nodeSetStateInitial (BREthereumNode node,
                     BREthereumNodeEndpointRoute route,
                     BREthereumNodeState state) {
    // Assume that the route is AVAILABLE.
    node->states[route] = nodeStateCreate (NODE_AVAILABLE);

    switch (state.type) {
        case NODE_AVAILABLE:
        case NODE_CONNECTING:
        case NODE_CONNECTED:
            break;

        case NODE_ERROR:
            switch (state.u.error.type) {
                case NODE_ERROR_UNIX:
                case NODE_ERROR_DISCONNECT:
                    break;
                case NODE_ERROR_PROTOCOL:
                    switch (state.u.error.u.protocol) {
                        case NODE_PROTOCOL_NONSTANDARD_PORT:
                        case NODE_PROTOCOL_CAPABILITIES_MISMATCH:
                        case NODE_PROTOCOL_STATUS_MISMATCH:
                        case NODE_PROTOCOL_UDP_EXCESSIVE_BYTE_COUNT:
                        case NODE_PROTOCOL_RLP_PARSE:
                            node->states[route] = state; // no recover; adopt the PROTOCOL error.
                            break;

                        case NODE_PROTOCOL_EXHAUSTED:
                        case NODE_PROTOCOL_PING_PONG_MISSED:
                        case NODE_PROTOCOL_TCP_AUTHENTICATION:
                        case NODE_PROTOCOL_TCP_HELLO_MISSED:
                        case NODE_PROTOCOL_TCP_STATUS_MISSED:
                            break;
                        }
                    break;
            }
            break;
    }
}

/// MARK: Move This
static void
updateLocalEndpointStatusMessage (BREthereumNodeEndpoint *endpoint,
                                  BREthereumNodeType type,
                                  uint64_t protocolVersion) {
    switch (type) {
        case NODE_TYPE_UNKNOWN:
            assert (0);
            
        case NODE_TYPE_GETH:
            assert (MESSAGE_LES == endpoint->status.identifier);
            endpoint->status.u.les.u.status.p2p.protocolVersion = protocolVersion;
            break;

        case NODE_TYPE_PARITY: {
            assert (MESSAGE_LES == endpoint->status.identifier);
            BREthereumLESMessageStatus *status = &endpoint->status.u.les.u.status;
            endpoint->status = (BREthereumMessage) {
                MESSAGE_PIP,
                { .pip = {
                    PIP_MESSAGE_STATUS,
                    { .status = {
                        protocolVersion,
                        status->p2p.chainId,
                        status->p2p.headNum,
                        status->p2p.headHash,
                        status->p2p.headTd,
                        status->p2p.genesisHash,
                        NULL }}}}
            };
            break;
        }
    }
}

static void
showEndpointStatusMessage (BREthereumNodeEndpoint *endpoint) {
    switch (endpoint->status.identifier) {
        case MESSAGE_P2P:
        case MESSAGE_DIS:
        case MESSAGE_ETH:
            assert (0);

        case MESSAGE_LES:
            messageLESStatusShow(&endpoint->status.u.les.u.status);
            break;

        case MESSAGE_PIP:
            messagePIPStatusShow(&endpoint->status.u.pip.u.status);
            break;
    }
}

/// MARK: - Send / Recv

static BREthereumNodeStatus
nodeSendFailed (BREthereumNode node,
                BREthereumNodeEndpointRoute route,
                BREthereumNodeState state) {
    nodeStateAnnounce (node, route, state);
    return NODE_STATUS_ERROR;
}

/**
 * Send `message` on `route` to `node`.  There is a consistency constraint whereby the message
 * identifier must be MESSAGE_DIS if and only if route is UDP.
 *
 * @param node
 * @param route
 * @param message
 */
static BREthereumNodeStatus
nodeSend (BREthereumNode node,
          BREthereumNodeEndpointRoute route,
          BREthereumMessage message) {

    int error = 0;
    size_t bytesCount = 0;

    assert ((NODE_ROUTE_UDP == route && MESSAGE_DIS == message.identifier) ||
            (NODE_ROUTE_UDP != route && MESSAGE_DIS != message.identifier));

    BRRlpItem item = messageEncode (message, node->coder);

#if defined (NEED_TO_AVOID_PROOFS_LOGGING)
    if (MESSAGE_LES != message.identifier || LES_MESSAGE_GET_PROOFS_V2 != message.u.les.identifier)
#endif
    eth_log (LES_LOG_TOPIC, "Send: [ %s, %15s ] => %15s",
             messageGetIdentifierName (&message),
             messageGetAnyIdentifierName (&message),
             node->remote.hostname);

    // Handle DIS messages specially.
    switch (message.identifier) {
        case MESSAGE_DIS: {
            // Extract the `item` bytes w/o the RLP length prefix.  This ends up being
            // simply the raw bytes.  We *know* the `item` is an RLP encoding of bytes; thus we
            // use `rlpDecodeBytes` (rather than `rlpDecodeList`.  Then simply send them.
            BRRlpData data = rlpDecodeBytesSharedDontRelease (node->coder.rlp, item);

            pthread_mutex_lock (&node->lock);
            error = nodeEndpointSendData (&node->remote, route, data.bytes, data.bytesCount);
            pthread_mutex_unlock (&node->lock);
            bytesCount = data.bytesCount;
            break;
        }

        default: {
#if defined (NODE_SHOW_SEND_RLP_ITEMS)
            if ((MESSAGE_PIP == message.identifier && PIP_MESSAGE_STATUS != message.u.pip.type) ||
                (MESSAGE_LES == message.identifier && LES_MESSAGE_STATUS != message.u.les.identifier))
                rlpShowItem (node->coder.rlp, item, "SEND");
#endif
            
            // Extract the `items` bytes w/o the RLP length prefix.  We *know* the `item` is an
            // RLP encoding of a list; thus we use `rlpDecodeList`.
            BRRlpData data = rlpDecodeListSharedDontRelease(node->coder.rlp, item);

            // Encrypt the length-less data
            BRRlpData encryptedData;
            pthread_mutex_lock (&node->lock);
            frameCoderEncrypt(node->frameCoder,
                              data.bytes, data.bytesCount,
                              &encryptedData.bytes, &encryptedData.bytesCount);

            error = nodeEndpointSendData (&node->remote, route, encryptedData.bytes, encryptedData.bytesCount);
            pthread_mutex_unlock (&node->lock);
            bytesCount = encryptedData.bytesCount;
            rlpDataRelease(encryptedData);
            break;
        }
    }
    rlpReleaseItem (node->coder.rlp, item);

#if defined (NEED_TO_PRINT_SEND_RECV_DATA)
    if (!error)
        eth_log (LES_LOG_TOPIC, "Size: Send: %s: PayLoad: %zu",
                 nodeEndpointRouteGetName(route),
                 data.bytesCount);
#endif

    return (0 == error
            ? NODE_STATUS_SUCCESS
            : nodeSendFailed (node, route, nodeStateCreateErrorUnix (error)));
}

static BREthereumNodeMessageResult
nodeRecvFailed (BREthereumNode node,
                BREthereumNodeEndpointRoute route,
                BREthereumNodeState state) {
    nodeStateAnnounce (node, route, state);
    return (BREthereumNodeMessageResult) { NODE_STATUS_ERROR };
}

static BREthereumNodeMessageResult
nodeRecv (BREthereumNode node,
          BREthereumNodeEndpointRoute route) {
    uint8_t *bytes = node->recvDataBuffer.bytes;
    size_t   bytesLimit = node->recvDataBuffer.bytesCount;
    size_t   bytesCount = 0;
    int error;

    BREthereumMessage message;

    rlpCoderClrFailed (node->coder.rlp);

    switch (route) {
        case NODE_ROUTE_UDP: {
            bytesCount = 1500;

            error = nodeEndpointRecvData (&node->remote, route, bytes, &bytesCount, 0);
            if (error) return nodeRecvFailed (node, NODE_ROUTE_UDP, nodeStateCreateErrorUnix (error));
            if (bytesCount > 1500)
                return nodeRecvFailed(node, NODE_ROUTE_UDP,
                                      nodeStateCreateErrorProtocol(NODE_PROTOCOL_UDP_EXCESSIVE_BYTE_COUNT));

            // Wrap at RLP Byte
            BRRlpItem item = rlpEncodeBytes (node->coder.rlp, bytes, bytesCount);

            message = messageDecode (item, node->coder,
                                     MESSAGE_DIS,
                                     MESSAGE_DIS_IDENTIFIER_ANY);
            rlpReleaseItem (node->coder.rlp, item);
            break;
        }

        case NODE_ROUTE_TCP: {
            size_t headerCount = 32;

            {
                // get header, decrypt it, validate it and then determine the bytesCount
                uint8_t header[32];
                memset(header, -1, 32);

                error = nodeEndpointRecvData (&node->remote, route, header, &headerCount, 1);
                if (error) return nodeRecvFailed (node, NODE_ROUTE_TCP, nodeStateCreateErrorUnix (error));

                pthread_mutex_lock (&node->lock);
                assert (ETHEREUM_BOOLEAN_IS_TRUE(frameCoderDecryptHeader(node->frameCoder, header, 32)));
                pthread_mutex_unlock (&node->lock);
                headerCount = ((uint32_t)(header[2]) <<  0 |
                               (uint32_t)(header[1]) <<  8 |
                               (uint32_t)(header[0]) << 16);

                // ??round to 16 ?? 32 ??
                bytesCount = headerCount + ((16 - (headerCount % 16)) % 16) + 16;
                // bytesCount = (headerCount + 15) & ~15;

                // ?? node->bodySize = headerCount; ??
            }

            // Given bytesCount, update recvDataBuffer if too small
            pthread_mutex_lock (&node->lock);
            if (bytesCount > bytesLimit) {
                node->recvDataBuffer.bytesCount = bytesCount;
                node->recvDataBuffer.bytes = realloc(node->recvDataBuffer.bytes, bytesCount);
                bytes = node->recvDataBuffer.bytes;
                bytesLimit = bytesCount;
            }
            pthread_mutex_unlock (&node->lock);

#if defined (NEED_TO_PRINT_SEND_RECV_DATA)
            eth_log (LES_LOG_TOPIC, "Size: Recv: TCP: PayLoad: %u, Frame: %zu", headerCount, bytesCount);
#endif
            
            // get body/frame
            error = nodeEndpointRecvData (&node->remote, route, bytes, &bytesCount, 1);
            if (error) return nodeRecvFailed (node, NODE_ROUTE_TCP, nodeStateCreateErrorUnix (error));

            pthread_mutex_lock (&node->lock);
            frameCoderDecryptFrame(node->frameCoder, bytes, bytesCount);
            pthread_mutex_unlock (&node->lock);

            // ?? node->bodySize = headerCount; ??

            // Identifier is at byte[0]
            BRRlpData identifierData = { 1, &bytes[0] };
            BRRlpItem identifierItem = rlpGetItem (node->coder.rlp, identifierData);
            uint8_t value = (uint8_t) rlpDecodeUInt64 (node->coder.rlp, identifierItem, 1);

            BREthereumMessageIdentifier type;
            BREthereumANYMessageIdentifier subtype;

            extractIdentifier(node, value, &type, &subtype);

            // Actual body
            BRRlpData data = { headerCount - 1, &bytes[1] };
            BRRlpItem item = rlpGetItem (node->coder.rlp, data);

#if defined (NEED_TO_PRINT_SEND_RECV_DATA)
            eth_log (LES_LOG_TOPIC, "Size: Recv: TCP: Type: %u, Subtype: %d", type, subtype);
#endif

            // Finally, decode the message
            message = messageDecode (item, node->coder, type, subtype);
#if defined (NODE_SHOW_RECV_RLP_ITEMS)
            if (!rlpCoderHasFailed(node->coder.rlp) &&
                ((MESSAGE_PIP == message.identifier && PIP_MESSAGE_STATUS != message.u.pip.type) ||
                 (MESSAGE_LES == message.identifier && LES_MESSAGE_STATUS != message.u.les.identifier)))
                rlpShowItem(node->coder.rlp, item, "RECV");
#endif

            // If this is a LES response message, then it has credit information.
            if (!rlpCoderHasFailed(node->coder.rlp) &&
                MESSAGE_LES == message.identifier &&
                messageLESHasUse (&message.u.les, LES_MESSAGE_USE_RESPONSE))
                node->credits = messageLESGetCredits (&message.u.les);
            
            rlpReleaseItem (node->coder.rlp, item);
            rlpReleaseItem (node->coder.rlp, identifierItem);

            break;
        }
    }

    if (!rlpCoderHasFailed(node->coder.rlp) ) {
        char disconnect[64] = { '\0' };

        if (MESSAGE_P2P == message.identifier && P2P_MESSAGE_DISCONNECT == message.u.p2p.identifier)
            sprintf (disconnect, " (%s)", messageP2PDisconnectDescription (message.u.p2p.u.disconnect.reason));

        eth_log (LES_LOG_TOPIC, "Recv: [ %s, %15s ] <= %15s%s",
                 messageGetIdentifierName (&message),
                 messageGetAnyIdentifierName (&message),
                 node->remote.hostname,
                 disconnect);
    }


    return (rlpCoderHasFailed(node->coder.rlp)
            ? (BREthereumNodeMessageResult) {
                NODE_STATUS_ERROR,
                { .error = {}}}
            : (BREthereumNodeMessageResult) {
                NODE_STATUS_SUCCESS,
                { .success = { message }}});
}


static uint64_t
nodeEstimateCredits (BREthereumNode node,
                     BREthereumMessage message) {
    switch (message.identifier) {
        case MESSAGE_P2P: return 0;
        case MESSAGE_DIS: return 0;
        case MESSAGE_ETH: return 0;
        case MESSAGE_LES:
            return (node->specs[message.u.les.identifier].baseCost +
                    messageLESGetCreditsCount (&message.u.les) * node->specs[message.u.les.identifier].reqCost);
        case MESSAGE_PIP: return 0;
    }
}

static uint64_t
nodeGetCredits (BREthereumNode node) {
    return node->credits;
}

/// MARK: Discovered

extern BREthereumBoolean
nodeGetDiscovered (BREthereumNode node) {
    return node->discovered;
}

extern void
nodeSetDiscovered (BREthereumNode node,
                   BREthereumBoolean discovered) {
    node->discovered = discovered;
}

extern BREthereumNodeStatus
nodeDiscover (BREthereumNode node,
              BREthereumNodeEndpoint *endpoint) {
    BREthereumMessage findNodes = {
        MESSAGE_DIS,
        { .dis = {
            DIS_MESSAGE_FIND_NEIGHBORS,
            { .findNeighbors =
                messageDISFindNeighborsCreate (endpoint->dis.key,
                                               time(NULL) + 1000000) },
            nodeGetLocalEndpoint(node)->dis.key }}
    };
    return nodeSend (node, NODE_ROUTE_UDP, findNodes);
}

extern BREthereumBoolean
nodeHandleTime (BREthereumNode node,
                BREthereumNodeEndpointRoute route,
                time_t now) {
    if (now != (time_t) -1 &&
        node->timeout != (time_t) -1 &&
        now >= node->timeout) {

        nodeDisconnect (node, route, nodeStateCreateErrorDisconnect (P2P_MESSAGE_DISCONNECT_TIMEOUT), ETHEREUM_BOOLEAN_FALSE);
        return ETHEREUM_BOOLEAN_TRUE;
    }
    return ETHEREUM_BOOLEAN_FALSE;
}

///
/// MARK: - Auth Support
///

static void
bytesXOR(uint8_t * op1, uint8_t* op2, uint8_t* result, size_t len) {
    for (unsigned int i = 0; i < len;  ++i) {
        result[i] = op1[i] ^ op2[i];
    }
}

static void
_BRECDH(void *out32, const BRKey *privKey, BRKey *pubKey)
{
    uint8_t p[65];
    size_t pLen = BRKeyPubKey(pubKey, p, sizeof(p));

    if (pLen == 65) p[0] = (p[64] % 2) ? 0x03 : 0x02; // convert to compressed pubkey format
    BRSecp256k1PointMul((BRECPoint *)p, &privKey->secret); // calculate shared secret ec-point
    memcpy(out32, &p[1], 32); // unpack the x coordinate

    mem_clean(p, sizeof(p));
}


static int // 0 on success
_sendAuthInitiator(BREthereumNode node) {

    // eth_log(LES_LOG_TOPIC, "%s", "generating auth initiator");

    // authInitiator -> E(remote-pubk, S(ephemeral-privk, static-shared-secret ^ nonce) || H(ephemeral-pubk) || pubk || nonce || 0x0)
    uint8_t * authBuf = node->authBuf;
    uint8_t * authBufCipher = node->authBufCipher;

    uint8_t* signature = &authBuf[0];
    uint8_t* hPubKey = &authBuf[SIG_SIZE_BYTES];
    uint8_t* pubKey = &authBuf[SIG_SIZE_BYTES + HEPUBLIC_BYTES];
    uint8_t* nonce =  &authBuf[SIG_SIZE_BYTES + HEPUBLIC_BYTES + PUBLIC_SIZE_BYTES];
    BRKey* nodeKey   = &node->local.dis.key;   //nodeGetKey(node);
    BRKey* remoteKey = &node->remote.dis.key;  // nodeGetPeerKey(node);

    //static-shared-secret = ecdh.agree(privkey, remote-pubk)
    UInt256 staticSharedSecret;
    _BRECDH(staticSharedSecret.u8, nodeKey, remoteKey);

    //static-shared-secret ^ nonce
    UInt256 xorStaticNonce;
    UInt256* localNonce = &node->local.nonce;       // nodeGetNonce(node);
    BRKey* localEphemeral = &node->local.ephemeralKey; //  nodeGetEphemeral(node);
    memset(xorStaticNonce.u8, 0, 32);
    bytesXOR(staticSharedSecret.u8, localNonce->u8, xorStaticNonce.u8, sizeof(localNonce->u8));


    // S(ephemeral-privk, static-shared-secret ^ nonce)
    // Determine the signature length
    size_t signatureLen = 65; BRKeyCompactSignEthereum(localEphemeral,
                                                       NULL, 0,
                                                       xorStaticNonce);

    // Fill the signature
    signatureLen = BRKeyCompactSignEthereum(localEphemeral,
                                            signature, signatureLen,
                                            xorStaticNonce);

    // || H(ephemeral-pubk)||
    memset(&hPubKey[32], 0, 32);
    uint8_t ephPublicKey[65];
    BRKeyPubKey(localEphemeral, ephPublicKey, 65);
    BRKeccak256(hPubKey, &ephPublicKey[1], PUBLIC_SIZE_BYTES);
    // || pubK ||
    uint8_t nodePublicKey[65] = {0};
    BRKeyPubKey(nodeKey, nodePublicKey, 65);
    memcpy(pubKey, &nodePublicKey[1], PUBLIC_SIZE_BYTES);
    // || nonce ||
    memcpy(nonce, localNonce->u8, sizeof(localNonce->u8));
    // || 0x0   ||
    authBuf[authBufLen - 1] = 0x0;

    // E(remote-pubk, S(ephemeral-privk, static-shared-secret ^ nonce) || H(ephemeral-pubk) || pubk || nonce || 0x0)
    BRKeyECIESAES128SHA256Encrypt(remoteKey, authBufCipher, authCipherBufLen, localEphemeral, authBuf, authBufLen);
    return 0;
}

//static void
//_readAuthFromInitiator(BREthereumNode node) {
//    BRKey* nodeKey = &node->local.key; // nodeGetKey(node);
//    eth_log (LES_LOG_TOPIC, "%s", "received auth from initiator");
//
//    size_t len = BRKeyECIESAES128SHA256Decrypt(nodeKey, node->authBuf, authBufLen, node->authBufCipher, authCipherBufLen);
//
//    if (len != authBufLen) {
//        //TODO: call _readAuthFromInitiatorEIP8...
//    }
//    else {
//        //copy remote nonce
//        UInt256* remoteNonce = &node->remote.nonce; // nodeGetPeerNonce(node);
//        memcpy(remoteNonce->u8, &node->authBuf[SIG_SIZE_BYTES + HEPUBLIC_BYTES + PUBLIC_SIZE_BYTES], sizeof(remoteNonce->u8));
//
//        //copy remote public key
//        uint8_t remotePubKey[65];
//        remotePubKey[0] = 0x04;
//        BRKey* remoteKey = &node->remote.key; // nodeGetPeerKey(node);
//        remoteKey->compressed = 0;
//        memcpy(&remotePubKey[1], &node->authBuf[SIG_SIZE_BYTES + HEPUBLIC_BYTES], PUBLIC_SIZE_BYTES);
//        BRKeySetPubKey(remoteKey, remotePubKey, 65);
//
//        UInt256 sharedSecret;
//        _BRECDH(sharedSecret.u8, nodeKey, remoteKey);
//
//        UInt256 xOrSharedSecret;
//        bytesXOR(sharedSecret.u8, remoteNonce->u8, xOrSharedSecret.u8, sizeof(xOrSharedSecret.u8));
//
//        // The ephemeral public key of the remote peer
//        BRKey* remoteEphemeral = &node->remote.ephemeralKey; // nodeGetPeerEphemeral(node);
//        BRKeyRecoverPubKeyEthereum(remoteEphemeral, xOrSharedSecret, node->authBuf, SIG_SIZE_BYTES);
//    }
//}
//
//static void
//_sendAuthAckToInitiator(BREthereumNode node) {
//    eth_log (LES_LOG_TOPIC, "%s", "generating auth ack for initiator");
//
//    // authRecipient -> E(remote-pubk, epubK|| nonce || 0x0)
//    uint8_t* ackBuf = node->ackBuf;
//    uint8_t* ackBufCipher = node->ackBufCipher;
//    BRKey* remoteKey = &node->remote.key; // nodeGetPeerKey(node);
//
//    uint8_t* pubKey = &ackBuf[0];
//    uint8_t* nonce =  &ackBuf[PUBLIC_SIZE_BYTES];
//
//    // || epubK ||
//    uint8_t localEphPublicKey[65];
//    BRKey* localEphemeral = &node->local.ephemeralKey; // nodeGetEphemeral(node);
//    size_t ephPubKeyLength = BRKeyPubKey(localEphemeral, localEphPublicKey, 65);
//    assert(ephPubKeyLength == 65);
//    memcpy(pubKey, &localEphPublicKey[1], 64);
//
//    // || nonce ||
//    UInt256* localNonce = &node->local.nonce; // nodeGetNonce(node);
//    memcpy(nonce, localNonce->u8, sizeof(localNonce->u8));
//    // || 0x0   ||
//    ackBuf[ackBufLen- 1] = 0x0;
//
//    //E(remote-pubk, epubK || nonce || 0x0)
//    BRKeyECIESAES128SHA256Encrypt(remoteKey, ackBufCipher, ackCipherBufLen, localEphemeral, ackBuf, ackBufLen);
//
//}

static int // 0 on success
_readAuthAckFromRecipient(BREthereumNode node) {

    BRKey* nodeKey = &node->local.dis.key; // nodeGetKey(node);

    // eth_log (LES_LOG_TOPIC,"%s", "received auth ack from recipient");

    size_t len = BRKeyECIESAES128SHA256Decrypt(nodeKey, node->ackBuf, ackBufLen, node->ackBufCipher, ackCipherBufLen);

    if (len != ackBufLen) {
        //TODO: call _readAckAuthFromRecipientEIP8...
        return 1;
    }
    else {
        //copy remote nonce key
        UInt256* nonce = &node->remote.nonce; // nodeGetPeerNonce(node);
        memcpy(nonce->u8, &node->ackBuf[PUBLIC_SIZE_BYTES], sizeof(nonce->u8));

        //copy ephemeral public key of the remote peer
        uint8_t remoteEPubKey[65];
        remoteEPubKey[0] = 0x04;
        BRKey* remoteEphemeral = &node->remote.ephemeralKey; // nodeGetPeerEphemeral(node);
        memcpy(&remoteEPubKey[1], node->ackBuf, PUBLIC_SIZE_BYTES);
        BRKeySetPubKey(remoteEphemeral, remoteEPubKey, 65);
        return 0;
    }
}
