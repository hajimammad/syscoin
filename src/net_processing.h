// Copyright (c) 2009-2010 Satoshi Nakamoto
// Copyright (c) 2009-2021 The Bitcoin Core developers
// Distributed under the MIT software license, see the accompanying
// file COPYING or http://www.opensource.org/licenses/mit-license.php.

#ifndef SYSCOIN_NET_PROCESSING_H
#define SYSCOIN_NET_PROCESSING_H

#include <net.h>
#include <validationinterface.h>

class AddrMan;
class CChainParams;
class CTxMemPool;
class ChainstateManager;
/** Default for -maxorphantx, maximum number of orphan transactions kept in memory */
static const unsigned int DEFAULT_MAX_ORPHAN_TRANSACTIONS = 100;
/** Default number of orphan+recently-replaced txn to keep around for block reconstruction */
static const unsigned int DEFAULT_BLOCK_RECONSTRUCTION_EXTRA_TXN = 100;
static const bool DEFAULT_PEERBLOOMFILTERS = false;
static const bool DEFAULT_PEERBLOCKFILTERS = false;
/** Threshold for marking a node to be discouraged, e.g. disconnected and added to the discouragement filter. */
static const int DISCOURAGEMENT_THRESHOLD{100};

struct CNodeStateStats {
    int nSyncHeight = -1;
    int nCommonHeight = -1;
    int m_starting_height = -1;
    std::chrono::microseconds m_ping_wait;
    std::vector<int> vHeightInFlight;
    bool m_relay_txs;
    CAmount m_fee_filter_received;
    uint64_t m_addr_processed = 0;
    uint64_t m_addr_rate_limited = 0;
    bool m_addr_relay_enabled{false};
};
// SYSCOIN
extern RecursiveMutex g_cs_orphans;
/**
 * Data structure for an individual peer. This struct is not protected by
 * cs_main since it does not contain validation-critical data.
 *
 * Memory is owned by shared pointers and this object is destructed when
 * the refcount drops to zero.
 *
 * Mutexes inside this struct must not be held when locking m_peer_mutex.
 *
 * TODO: move most members from CNodeState to this structure.
 * TODO: move remaining application-layer data members from CNode to this structure.
 */
struct Peer {
    /** Same id as the CNode object for this peer */
    const NodeId m_id{0};

    /** Protects misbehavior data members */
    Mutex m_misbehavior_mutex;
    /** Accumulated misbehavior score for this peer */
    int m_misbehavior_score GUARDED_BY(m_misbehavior_mutex){0};
    /** Whether this peer should be disconnected and marked as discouraged (unless it has NetPermissionFlags::NoBan permission). */
    bool m_should_discourage GUARDED_BY(m_misbehavior_mutex){false};

    /** Protects block inventory data members */
    Mutex m_block_inv_mutex;
    /** List of blocks that we'll announce via an `inv` message.
     * There is no final sorting before sending, as they are always sent
     * immediately and in the order requested. */
    std::vector<uint256> m_blocks_for_inv_relay GUARDED_BY(m_block_inv_mutex);
    /** Unfiltered list of blocks that we'd like to announce via a `headers`
     * message. If we can't announce via a `headers` message, we'll fall back to
     * announcing via `inv`. */
    std::vector<uint256> m_blocks_for_headers_relay GUARDED_BY(m_block_inv_mutex);
    /** The final block hash that we sent in an `inv` message to this peer.
     * When the peer requests this block, we send an `inv` message to trigger
     * the peer to request the next sequence of block hashes.
     * Most peers use headers-first syncing, which doesn't use this mechanism */
    uint256 m_continuation_block GUARDED_BY(m_block_inv_mutex) {};

    /** This peer's reported block height when we connected */
    std::atomic<int> m_starting_height{-1};

    /** The pong reply we're expecting, or 0 if no pong expected. */
    std::atomic<uint64_t> m_ping_nonce_sent{0};
    /** When the last ping was sent, or 0 if no ping was ever sent */
    std::atomic<std::chrono::microseconds> m_ping_start{0us};
    /** Whether a ping has been requested by the user */
    std::atomic<bool> m_ping_queued{false};

    /** Whether this peer relays txs via wtxid */
    std::atomic<bool> m_wtxid_relay{false};

    struct TxRelay {
        mutable RecursiveMutex m_bloom_filter_mutex;
        // We use m_relay_txs for two purposes -
        // a) it allows us to not relay tx invs before receiving the peer's version message
        // b) the peer may tell us in its version message that we should not relay tx invs
        //    unless it loads a bloom filter.
        bool m_relay_txs GUARDED_BY(m_bloom_filter_mutex){false};
        std::unique_ptr<CBloomFilter> m_bloom_filter PT_GUARDED_BY(m_bloom_filter_mutex) GUARDED_BY(m_bloom_filter_mutex){nullptr};

        mutable RecursiveMutex m_tx_inventory_mutex;
        CRollingBloomFilter m_tx_inventory_known_filter GUARDED_BY(m_tx_inventory_mutex){50000, 0.000001};
        // Set of transaction ids we still have to announce.
        // They are sorted by the mempool before relay, so the order is not important.
        std::set<uint256> m_tx_inventory_to_send;
        // Used for BIP35 mempool sending
        bool m_send_mempool GUARDED_BY(m_tx_inventory_mutex){false};
        // Last time a "MEMPOOL" request was serviced.
        std::atomic<std::chrono::seconds> m_last_mempool_req{0s};
        std::chrono::microseconds m_next_inv_send_time{0};

        /** Minimum fee rate with which to filter inv's to this node */
        std::atomic<CAmount> m_fee_filter_received{0};
        CAmount m_fee_filter_sent{0};
        std::chrono::microseconds m_next_send_feefilter{0};
        // SYSCOIN
        std::set<CInv> m_tx_inventory_to_send_other;
    };

    /** Transaction relay data. Will be a nullptr if we're not relaying
     *  transactions with this peer (e.g. if it's a block-relay-only peer) */
    std::unique_ptr<TxRelay> m_tx_relay;

    /** A vector of addresses to send to the peer, limited to MAX_ADDR_TO_SEND. */
    std::vector<CAddress> m_addrs_to_send;
    /** Probabilistic filter to track recent addr messages relayed with this
     *  peer. Used to avoid relaying redundant addresses to this peer.
     *
     *  We initialize this filter for outbound peers (other than
     *  block-relay-only connections) or when an inbound peer sends us an
     *  address related message (ADDR, ADDRV2, GETADDR).
     *
     *  Presence of this filter must correlate with m_addr_relay_enabled.
     **/
    std::unique_ptr<CRollingBloomFilter> m_addr_known;
    /** Whether we are participating in address relay with this connection.
     *
     *  We set this bool to true for outbound peers (other than
     *  block-relay-only connections), or when an inbound peer sends us an
     *  address related message (ADDR, ADDRV2, GETADDR).
     *
     *  We use this bool to decide whether a peer is eligible for gossiping
     *  addr messages. This avoids relaying to peers that are unlikely to
     *  forward them, effectively blackholing self announcements. Reasons
     *  peers might support addr relay on the link include that they connected
     *  to us as a block-relay-only peer or they are a light client.
     *
     *  This field must correlate with whether m_addr_known has been
     *  initialized.*/
    std::atomic_bool m_addr_relay_enabled{false};
    /** Whether a getaddr request to this peer is outstanding. */
    bool m_getaddr_sent{false};
    /** Guards address sending timers. */
    mutable Mutex m_addr_send_times_mutex;
    /** Time point to send the next ADDR message to this peer. */
    std::chrono::microseconds m_next_addr_send GUARDED_BY(m_addr_send_times_mutex){0};
    /** Time point to possibly re-announce our local address to this peer. */
    std::chrono::microseconds m_next_local_addr_send GUARDED_BY(m_addr_send_times_mutex){0};
    /** Whether the peer has signaled support for receiving ADDRv2 (BIP155)
     *  messages, indicating a preference to receive ADDRv2 instead of ADDR ones. */
    std::atomic_bool m_wants_addrv2{false};
    /** Whether this peer has already sent us a getaddr message. */
    bool m_getaddr_recvd{false};
    /** Number of addresses that can be processed from this peer. Start at 1 to
     *  permit self-announcement. */
    double m_addr_token_bucket{1.0};
    /** When m_addr_token_bucket was last updated */
    std::chrono::microseconds m_addr_token_timestamp{GetTime<std::chrono::microseconds>()};
    /** Total number of addresses that were dropped due to rate limiting. */
    std::atomic<uint64_t> m_addr_rate_limited{0};
    /** Total number of addresses that were processed (excludes rate-limited ones). */
    std::atomic<uint64_t> m_addr_processed{0};

    /** Set of txids to reconsider once their parent transactions have been accepted **/
    std::set<uint256> m_orphan_work_set GUARDED_BY(g_cs_orphans);

    /** Protects m_getdata_requests **/
    Mutex m_getdata_requests_mutex;
    /** Work queue of items requested by this peer **/
    std::deque<CInv> m_getdata_requests GUARDED_BY(m_getdata_requests_mutex);
    // SYSCOIN
    /** This peer's a masternode connection */
    std::atomic<bool> m_masternode_connection{false};
    explicit Peer(NodeId id, bool tx_relay)
        : m_id(id)
        , m_tx_relay(tx_relay ? std::make_unique<TxRelay>() : nullptr)
    {}
};
using PeerRef = std::shared_ptr<Peer>;

class PeerManager : public CValidationInterface, public NetEventsInterface
{
public:
    static std::unique_ptr<PeerManager> make(const CChainParams& chainparams, CConnman& connman, AddrMan& addrman,
                                             BanMan* banman, ChainstateManager& chainman,
                                             CTxMemPool& pool, bool ignore_incoming_txs);
    virtual ~PeerManager() { }

    /**
     * Attempt to manually fetch block from a given peer. We must already have the header.
     *
     * @param[in]  peer_id      The peer id
     * @param[in]  block_index  The blockindex
     * @returns std::nullopt if a request was successfully made, otherwise an error message
     */
    virtual std::optional<std::string> FetchBlock(NodeId peer_id, const CBlockIndex& block_index) = 0;

    /** Begin running background tasks, should only be called once */
    virtual void StartScheduledTasks(CScheduler& scheduler) = 0;

    /** Get statistics from node state */
    virtual bool GetNodeStateStats(NodeId nodeid, CNodeStateStats& stats) const = 0;

    /** Whether this node ignores txs received over p2p. */
    virtual bool IgnoresIncomingTxs() = 0;

    /** Relay transaction to all peers. */
    virtual void RelayTransaction(const uint256& txid, const uint256& wtxid) = 0;

    /** Send ping message to all peers */
    virtual void SendPings() = 0;

    /** Set the best height */
    virtual void SetBestHeight(int height) = 0;

    // SYSCOIN
    virtual size_t GetRequestedCount(NodeId nodeId) const EXCLUSIVE_LOCKS_REQUIRED(::cs_main) = 0;
    virtual void ReceivedResponse(NodeId nodeId, const uint256& hash) EXCLUSIVE_LOCKS_REQUIRED(::cs_main) = 0;
    virtual void ForgetTxHash(NodeId nodeId, const uint256& hash) EXCLUSIVE_LOCKS_REQUIRED(::cs_main) = 0;
    virtual void _RelayTransaction(const uint256& txid, const uint256& wtxid) EXCLUSIVE_LOCKS_REQUIRED(::cs_main) = 0;
    virtual void PushTxInventory(Peer& peer, const uint256& txid, const uint256& wtxid) EXCLUSIVE_LOCKS_REQUIRED(::cs_main) = 0;
    virtual void RelayTransactionOther(const CInv& inv) = 0;
    virtual void _RelayTransactionOther(const CInv& inv) EXCLUSIVE_LOCKS_REQUIRED(::cs_main) = 0;
    virtual void PushTxInventoryOther(Peer& peer, const CInv& inv) EXCLUSIVE_LOCKS_REQUIRED(::cs_main) = 0;
    virtual PeerRef GetPeerRef(NodeId id) const = 0;
    virtual void AddKnownTx(Peer& peer, const uint256& hash) = 0;
    /**
     * Increment peer's misbehavior score. If the new value >= DISCOURAGEMENT_THRESHOLD, mark the node
     * to be discouraged, meaning the peer might be disconnected and added to the discouragement filter.
     * Public for unit testing.
     */
    virtual void Misbehaving(const NodeId pnode, const int howmuch, const std::string& message) = 0;

    /**
     * Evict extra outbound peers. If we think our tip may be stale, connect to an extra outbound.
     * Public for unit testing.
     */
    virtual void CheckForStaleTipAndEvictPeers() = 0;

    /** Process a single message from a peer. Public for fuzz testing */
    virtual void ProcessMessage(CNode& pfrom, const std::string& msg_type, CDataStream& vRecv,
                                const std::chrono::microseconds time_received, const std::atomic<bool>& interruptMsgProc) = 0;
};

// SYSCOIN
// Upstream moved this into net_processing.cpp (13417), however since we use Misbehaving in a number of syscoin specific
// files such as mnauth.cpp and governance.cpp it makes sense to keep it in the header
/** Increase a node's misbehavior score. */
bool IsBanned(NodeId nodeid, BanMan& banman);
unsigned int GetMaxInv();
#endif // SYSCOIN_NET_PROCESSING_H
