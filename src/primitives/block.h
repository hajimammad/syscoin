// Copyright (c) 2009-2010 Satoshi Nakamoto
// Copyright (c) 2009-2020 The Bitcoin Core developers
// Distributed under the MIT software license, see the accompanying
// file COPYING or http://www.opensource.org/licenses/mit-license.php.

#ifndef SYSCOIN_PRIMITIVES_BLOCK_H
#define SYSCOIN_PRIMITIVES_BLOCK_H
#include <auxpow.h>
#include <primitives/transaction.h>
#include <primitives/pureheader.h>
#include <serialize.h>
#include <uint256.h>

#include <memory>

/** Nodes collect new transactions into a block, hash them into a hash tree,
 * and scan through nonce values to make the block's hash satisfy proof-of-work
 * requirements.  When they solve the proof-of-work, they broadcast the block
 * to everyone and the block is added to the block chain.  The first transaction
 * in the block is a special one that creates a new coin owned by the creator
 * of the block.
 */
class CBlockHeader : public CPureBlockHeader
{
public:
    // auxpow (if this is a merge-minded block)
    std::shared_ptr<CAuxPow> auxpow;
    CBlockHeader()
    {
        SetNull();
    }

    template<typename Stream>
    void Serialize(Stream& s) const
    {
        s << *(CPureBlockHeader*)this;
        if (this->IsAuxpow())
        {
            assert(auxpow != nullptr);
            s << *auxpow;
        }
    }
    template<typename Stream>
    void Unserialize(Stream& s)
    {
        s >> *(CPureBlockHeader*)this;
        if (this->IsAuxpow())
        {
            auxpow = std::make_shared<CAuxPow>();
            assert(auxpow != nullptr);
            s >> *auxpow;
        } else {
            auxpow.reset();
        }
    }

    
    void SetNull()
    {
        CPureBlockHeader::SetNull();
        auxpow.reset();
    }

    /**
     * Set the block's auxpow (or unset it).  This takes care of updating
     * the version accordingly.
     */
    void SetAuxpow (std::unique_ptr<CAuxPow> apow);
};


class CBlock : public CBlockHeader
{
public:
    // network and disk
    std::vector<CTransactionRef> vtx;
    // memory only
    mutable bool fChecked;
    // SYSCOIN
    std::vector<unsigned char> vchNEVMBlockData;
    CBlock()
    {
        SetNull();
    }

    CBlock(const CBlockHeader &header)
    {
        SetNull();
        *(static_cast<CBlockHeader*>(this)) = header;
    }

    SERIALIZE_METHODS(CBlock, obj)
    {
        READWRITEAS(CBlockHeader, obj);
        READWRITE(obj.vtx);
        // SYSCOIN
        if (obj.IsNEVM() && !(s.GetType() & SER_GETHASH) && !(s.GetType() & SER_SIZE))
            READWRITE(obj.vchNEVMBlockData);
    }

    void SetNull()
    {
        CBlockHeader::SetNull();
        vtx.clear();
        fChecked = false;
        // SYSCOIN
        vchNEVMBlockData.clear();
    }

    CBlockHeader GetBlockHeader() const
    {
        CBlockHeader block;
        block.nVersion       = nVersion;
        block.hashPrevBlock  = hashPrevBlock;
        block.hashMerkleRoot = hashMerkleRoot;
        block.nTime          = nTime;
        block.nBits          = nBits;
        block.nNonce         = nNonce;
        block.auxpow         = auxpow;
        return block;
    }

    std::string ToString() const;
};

/** Describes a place in the block chain to another node such that if the
 * other node doesn't have the same branch, it can find a recent common trunk.
 * The further back it is, the further before the fork it may be.
 */
struct CBlockLocator
{
    std::vector<uint256> vHave;

    CBlockLocator() {}

    explicit CBlockLocator(const std::vector<uint256>& vHaveIn) : vHave(vHaveIn) {}

    SERIALIZE_METHODS(CBlockLocator, obj)
    {
        int nVersion = s.GetVersion();
        if (!(s.GetType() & SER_GETHASH))
            READWRITE(nVersion);
        READWRITE(obj.vHave);
    }

    void SetNull()
    {
        vHave.clear();
    }

    bool IsNull() const
    {
        return vHave.empty();
    }
};

#endif // SYSCOIN_PRIMITIVES_BLOCK_H
