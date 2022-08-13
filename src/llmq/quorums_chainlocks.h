// Copyright (c) 2019-2020 The Dash Core developers
// Distributed under the MIT/X11 software license, see the accompanying
// file COPYING or http://www.opensource.org/licenses/mit-license.php.

#ifndef SYSCOIN_LLMQ_QUORUMS_CHAINLOCKS_H
#define SYSCOIN_LLMQ_QUORUMS_CHAINLOCKS_H

#include <llmq/quorums_signing.h>

#include <chainparams.h>

#include <atomic>


class CBlockIndex;
class CConnman;
class PeerManager;
class CScheduler;
class ChainstateManager;
namespace llmq
{

class CChainLockSig
{
public:
    int32_t nHeight{-1};
    uint256 blockHash;
    CBLSSignature sig;
    std::vector<bool> signers;

public:
    SERIALIZE_METHODS(CChainLockSig, obj) {
        READWRITE(obj.nHeight, obj.blockHash, obj.sig);
        READWRITE(DYNBITSET(obj.signers));
    }

    bool IsNull() const;
    std::string ToString() const;
};

typedef std::shared_ptr<const CChainLockSig> CChainLockSigCPtr;

struct ReverseHeightComparator
{
    bool operator()(const int h1, const int h2) const {
        return h1 > h2;
    }
};

class CChainLocksHandler : public CRecoveredSigsListener
{
    static const int64_t CLEANUP_INTERVAL = 1000 * 30;
    static const int64_t CLEANUP_SEEN_TIMEOUT = 24 * 60 * 60 * 1000;


private:
    CScheduler* scheduler;
    std::thread* scheduler_thread;
    mutable RecursiveMutex cs;
    bool isEnabled GUARDED_BY(cs) {false};
    bool isEnforced GUARDED_BY(cs) {false};
    bool tryLockChainTipScheduled GUARDED_BY(cs) {false};

    CChainLockSig mostRecentChainLockShare GUARDED_BY(cs);
    CChainLockSig bestChainLockWithKnownBlock GUARDED_BY(cs);
    const CBlockIndex* bestChainLockBlockIndex GUARDED_BY(cs) {nullptr};
    // Keep best chainlock shares and candidates, sorted by height (highest heght first).
    std::map<int, std::map<CQuorumCPtr, CChainLockSigCPtr>, ReverseHeightComparator> bestChainLockShares GUARDED_BY(cs);
    std::map<int, CChainLockSigCPtr, ReverseHeightComparator> bestChainLockCandidates GUARDED_BY(cs);

    std::map<uint256, std::pair<int, uint256> > mapSignedRequestIds GUARDED_BY(cs);


    std::map<uint256, int64_t> seenChainLocks GUARDED_BY(cs);

    int64_t lastCleanupTime GUARDED_BY(cs) {0};

public:
    CConnman& connman;
    PeerManager& peerman;
    ChainstateManager& chainman;
    explicit CChainLocksHandler(CConnman &connman, PeerManager& peerman, ChainstateManager& chainman);
    ~CChainLocksHandler();

    void Start();
    void Stop();

    bool AlreadyHave(const uint256& hash) const;
    bool GetChainLockByHash(const uint256& hash, CChainLockSig& ret) const;
    const CChainLockSig GetMostRecentChainLock() const;
    const CChainLockSig GetBestChainLock() const ;
    const std::map<CQuorumCPtr, CChainLockSigCPtr> GetBestChainLockShares() const;

    void ProcessMessage(CNode* pfrom, const std::string& strCommand, CDataStream& vRecv);
    void ProcessNewChainLock(NodeId from, CChainLockSig& clsig, const uint256& hash, const uint256& idIn = uint256());
    void AcceptedBlockHeader(const CBlockIndex* pindexNew);
    void UpdatedBlockTip(const CBlockIndex* pindexNew, bool fInitialDownload);
    void CheckActiveState();
    void TrySignChainTip();
    void HandleNewRecoveredSig(const CRecoveredSig& recoveredSig) override;

    bool HasChainLock(int nHeight, const uint256& blockHash) const;
    bool HasConflictingChainLock(int nHeight, const uint256& blockHash) const;

private:
    // these require locks to be held already
    bool InternalHasChainLock(int nHeight, const uint256& blockHash) const EXCLUSIVE_LOCKS_REQUIRED(cs);
    bool InternalHasConflictingChainLock(int nHeight, const uint256& blockHash) const EXCLUSIVE_LOCKS_REQUIRED(cs);

    bool TryUpdateBestChainLock(const CBlockIndex* pindex) EXCLUSIVE_LOCKS_REQUIRED(cs);
    bool VerifyChainLockShare(const CChainLockSig& clsig, const CBlockIndex* pindexScan, const uint256& idIn, std::pair<int, CQuorumCPtr>& ret) LOCKS_EXCLUDED(cs);
    bool VerifyAggregatedChainLock(const CChainLockSig& clsig, const CBlockIndex* pindexScan) LOCKS_EXCLUDED(cs);
    void Cleanup();
};

extern CChainLocksHandler* chainLocksHandler;

bool AreChainLocksEnabled();

} // namespace llmq

#endif // SYSCOIN_LLMQ_QUORUMS_CHAINLOCKS_H
