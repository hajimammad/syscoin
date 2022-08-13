// Copyright (c) 2018-2019 The Dash Core developers
// Distributed under the MIT software license, see the accompanying
// file COPYING or http://www.opensource.org/licenses/mit-license.php.
#include <test/util/setup_common.h>

#include <script/interpreter.h>
#include <script/standard.h>
#include <script/sign.h>
#include <validation.h>
#include <base58.h>
#include <netbase.h>
#include <messagesigner.h>
#include <policy/policy.h>
#include <script/signingprovider.h>
#include <spork.h>
#include <txmempool.h>

#include <evo/specialtx.h>
#include <evo/providertx.h>
#include <evo/deterministicmns.h>
#include <boost/test/unit_test.hpp>
using SimpleUTXOVec = std::vector<std::pair<COutPoint, std::pair<int, CAmount>> >;

static SimpleUTXOVec BuildSimpleUTXOVec(const std::vector<CTransactionRef>& txs)
{
    SimpleUTXOVec utxos;
    for (size_t i = 0; i < txs.size(); i++) {
        auto& tx = *txs[i];
        for (size_t j = 0; j < tx.vout.size(); j++) {
            if(tx.vout[j].nValue > 0)
                utxos.emplace_back(COutPoint(tx.GetHash(), j), std::make_pair((int)i + 1, tx.vout[j].nValue));
        }
    }
    return utxos;
}

static std::vector<COutPoint> SelectUTXOs(const NodeContext& node, SimpleUTXOVec& utxos, CAmount amount, CAmount& changeRet)
{
    changeRet = 0;
    std::vector<COutPoint> selectedUtxos;
    CAmount selectedAmount = 0;
    auto it = utxos.begin();
    bool bFound = false;
    while (it != utxos.end()) {
        if (*node.chain->getHeight() - it->second.first < 101) {
            it++;
            continue;
        }
        selectedAmount += it->second.second;
        selectedUtxos.emplace_back(it->first);
        it = utxos.erase(it);
        bFound = true;
        if (selectedAmount >= amount) {
            changeRet = selectedAmount - amount;
            break;
        }
    }
    BOOST_ASSERT(bFound);
    return selectedUtxos;
}

static void FundTransaction(const NodeContext& node, CMutableTransaction& tx, SimpleUTXOVec& utoxs, const CScript& scriptPayout, CAmount amount)
{
    CAmount change;
    auto inputs = SelectUTXOs(node, utoxs, amount, change);
    for (size_t i = 0; i < inputs.size(); i++) {
        tx.vin.emplace_back(CTxIn(inputs[i]));
    }
    tx.vout.emplace_back(CTxOut(amount, scriptPayout));
    if (change != 0) {
        tx.vout.emplace_back(CTxOut(change, scriptPayout));
    }
}

static void SignTransaction(const NodeContext& node, CMutableTransaction& tx, const CKey& coinbaseKey)
{
    LOCK(cs_main);
    FillableSigningProvider tempKeystore;
    tempKeystore.AddKeyPubKey(coinbaseKey, coinbaseKey.GetPubKey());
    std::map<COutPoint, Coin> coins;
    for (size_t i = 0; i < tx.vin.size(); i++) {
        coins[tx.vin[i].prevout]; 
        node.chain->findCoins(coins);
    }
    std::map<int, bilingual_str> input_errors;
    BOOST_CHECK(SignTransaction(tx, &tempKeystore, coins, SIGHASH_ALL, input_errors));
}

static CMutableTransaction CreateProRegTx(const NodeContext& node, SimpleUTXOVec& utxos, int port, const CScript& scriptPayout, const CKey& coinbaseKey, CKey& ownerKeyRet, CBLSSecretKey& operatorKeyRet)
{
    ownerKeyRet.MakeNewKey(true);
    operatorKeyRet.MakeNewKey();

    CProRegTx proTx;
    proTx.collateralOutpoint.n = 0;
    proTx.addr = LookupNumeric("1.1.1.1", port);
    proTx.keyIDOwner = ownerKeyRet.GetPubKey().GetID();
    proTx.pubKeyOperator = operatorKeyRet.GetPublicKey();
    proTx.keyIDVoting = ownerKeyRet.GetPubKey().GetID();
    proTx.scriptPayout = scriptPayout;
    proTx.nOperatorReward = 5000;

    CMutableTransaction tx;
    tx.nVersion = SYSCOIN_TX_VERSION_MN_REGISTER;
    FundTransaction(node, tx, utxos, scriptPayout, 100 * COIN);
    proTx.inputsHash = CalcTxInputsHash(CTransaction(tx));
    SetTxPayload(tx, proTx);
    SignTransaction(node, tx, coinbaseKey);

    return tx;
}

static CMutableTransaction CreateProUpServTx(const NodeContext& node, SimpleUTXOVec& utxos, const uint256& proTxHash, const CBLSSecretKey& operatorKey, int port, const CKey& coinbaseKey)
{
    CProUpServTx proTx;
    proTx.proTxHash = proTxHash;
    proTx.addr = LookupNumeric("1.1.1.1", port);
    proTx.scriptOperatorPayout = GetScriptForDestination(PKHash(coinbaseKey.GetPubKey()));
    CMutableTransaction tx;
    tx.nVersion = SYSCOIN_TX_VERSION_MN_UPDATE_SERVICE;
    FundTransaction(node, tx, utxos, GetScriptForDestination(PKHash(coinbaseKey.GetPubKey())), 1 * COIN);
    proTx.inputsHash = CalcTxInputsHash(CTransaction(tx));
    proTx.sig = operatorKey.Sign(::SerializeHash(proTx));
    SetTxPayload(tx, proTx);
    SignTransaction(node, tx, coinbaseKey);

    return tx;
}

static CMutableTransaction CreateProUpRegTx(const NodeContext& node, SimpleUTXOVec& utxos, const uint256& proTxHash, const CKey& mnKey, const CBLSPublicKey& pubKeyOperator, const CKeyID& keyIDVoting, const CScript& scriptPayout, const CKey& coinbaseKey)
{

    CProUpRegTx proTx;
    proTx.proTxHash = proTxHash;
    proTx.pubKeyOperator = pubKeyOperator;
    proTx.keyIDVoting = keyIDVoting;
    proTx.scriptPayout = scriptPayout;

    CMutableTransaction tx;
    tx.nVersion = SYSCOIN_TX_VERSION_MN_UPDATE_REGISTRAR;
    FundTransaction(node, tx, utxos, GetScriptForDestination(PKHash(coinbaseKey.GetPubKey())), 1 * COIN);
    proTx.inputsHash = CalcTxInputsHash(CTransaction(tx));
    CHashSigner::SignHash(::SerializeHash(proTx), mnKey, proTx.vchSig);
    SetTxPayload(tx, proTx);
    SignTransaction(node, tx, coinbaseKey);

    return tx;
}

static CMutableTransaction CreateProUpRevTx(const NodeContext& node, SimpleUTXOVec& utxos, const uint256& proTxHash, const CBLSSecretKey& operatorKey, const CKey& coinbaseKey)
{
    CProUpRevTx proTx;
    proTx.proTxHash = proTxHash;

    CMutableTransaction tx;
    tx.nVersion = SYSCOIN_TX_VERSION_MN_UPDATE_REVOKE;
    FundTransaction(node, tx, utxos, GetScriptForDestination(PKHash(coinbaseKey.GetPubKey())), 1 * COIN);
    proTx.inputsHash = CalcTxInputsHash(CTransaction(tx));
    proTx.sig = operatorKey.Sign(::SerializeHash(proTx));
    SetTxPayload(tx, proTx);
    SignTransaction(node, tx, coinbaseKey);

    return tx;
}

template<typename ProTx>
static CMutableTransaction MalleateProTxPayout(const CMutableTransaction& tx)
{
    ProTx proTx;
    GetTxPayload(tx, proTx);

    CKey key;
    key.MakeNewKey(true);
    proTx.scriptPayout = GetScriptForDestination(PKHash(key.GetPubKey()));

    CMutableTransaction tx2 = tx;
    SetTxPayload(tx2, proTx);

    return tx2;
}

static CScript GenerateRandomAddress()
{
    CKey key;
    key.MakeNewKey(true);
    return GetScriptForDestination(PKHash(key.GetPubKey()));
}

static CDeterministicMNCPtr FindPayoutDmn(const CBlock& block)
{
    auto mnList = deterministicMNManager->GetListAtChainTip();

    for (const auto& txout : block.vtx[0]->vout) {
        CDeterministicMNCPtr found;
        mnList.ForEachMN(true, [&](const CDeterministicMNCPtr& dmn) {
            if (found == nullptr && txout.scriptPubKey == dmn->pdmnState->scriptPayout) {
                found = dmn;
            }
        });
        if (found != nullptr) {
            return found;
        }
    }
    return nullptr;
}

static bool CheckTransactionSignature(const NodeContext& node, const CMutableTransaction& tx)
{
    for (unsigned int i = 0; i < tx.vin.size(); i++) {
        const auto& txin = tx.vin[i];
        std::map<COutPoint, Coin> coins;
        coins[txin.prevout]; 
        node.chain->findCoins(coins);
        const Coin& coin = coins.at(txin.prevout);
        if (!VerifyScript(txin.scriptSig, coin.out.scriptPubKey, nullptr, STANDARD_SCRIPT_VERIFY_FLAGS, MutableTransactionSignatureChecker(&tx, i, coin.out.nValue, MissingDataBehavior::ASSERT_FAIL))) {
            return false;
        }
    }
    return true;
}

BOOST_AUTO_TEST_SUITE(evo_dip3_activation_tests)

BOOST_FIXTURE_TEST_CASE(dip3_activation, TestChainDIP3BeforeActivationSetup)
{
    auto utxos = BuildSimpleUTXOVec(m_coinbase_txns);
    CKey ownerKey;
    CBLSSecretKey operatorKey;
    CScript addr = GenerateRandomAddress();
    auto tx = CreateProRegTx(m_node, utxos, 1, addr, coinbaseKey, ownerKey, operatorKey);
    std::vector<CMutableTransaction> txns = std::vector<CMutableTransaction>{tx};

    int nHeight = *m_node.chain->getHeight();

    // We start one block before DIP3 activation, so mining a block with a DIP3 transaction should be no-op
    auto block = std::make_shared<CBlock>(CreateAndProcessBlock(txns, GetScriptForRawPubKey(coinbaseKey.GetPubKey())));
 
    BOOST_CHECK_EQUAL(*m_node.chain->getHeight() , nHeight + 1);
    BOOST_CHECK_EQUAL(block->GetHash() , m_node.chain->getBlockHash(*m_node.chain->getHeight()));
    
    BOOST_ASSERT(!deterministicMNManager->GetListAtChainTip().HasMN(tx.GetHash()));

    // re-create reg tx prev one got mined as no-op
    tx = CreateProRegTx(m_node, utxos, 1, addr, coinbaseKey, ownerKey, operatorKey);
    txns = std::vector<CMutableTransaction>{tx};
    // Mining a block with a DIP3 transaction should succeed now
    block = std::make_shared<CBlock>(CreateAndProcessBlock(txns, GetScriptForRawPubKey(coinbaseKey.GetPubKey())));

    if(deterministicMNManager)
        deterministicMNManager->UpdatedBlockTip(m_node.chainman->ActiveChain().Tip());
    BOOST_CHECK_EQUAL(*m_node.chain->getHeight() , nHeight + 2);
    BOOST_CHECK_EQUAL(block->GetHash() , m_node.chain->getBlockHash(*m_node.chain->getHeight()));
    
    BOOST_ASSERT(deterministicMNManager->GetListAtChainTip().HasMN(tx.GetHash()));
}

BOOST_FIXTURE_TEST_CASE(dip3_protx, TestChainDIP3Setup)
{
    CKey sporkKey;
    sporkKey.MakeNewKey(true);
    sporkManager.SetSporkAddress(EncodeDestination(PKHash(sporkKey.GetPubKey())));
    sporkManager.SetPrivKey(EncodeSecret(sporkKey));

    auto utxos = BuildSimpleUTXOVec(m_coinbase_txns);

    int nHeight = *m_node.chain->getHeight();
    int port = 1;

    std::vector<uint256> dmnHashes;
    std::map<uint256, CKey> ownerKeys;
    std::map<uint256, CBLSSecretKey> operatorKeys;

    // register one MN per block
    for (size_t i = 0; i < 6; i++) {
        CKey ownerKey;
        CBLSSecretKey operatorKey;
        auto tx = CreateProRegTx(m_node, utxos, port++, GenerateRandomAddress(), coinbaseKey, ownerKey, operatorKey);
        dmnHashes.emplace_back(tx.GetHash());
        ownerKeys.emplace(tx.GetHash(), ownerKey);
        operatorKeys.emplace(tx.GetHash(), operatorKey);
        {
            LOCK(cs_main);
            // also verify that payloads are not malleable after they have been signed
            // the form of ProRegTx we use here is one with a collateral included, so there is no signature inside the
            // payload itself. This means, we need to rely on script verification, which takes the hash of the extra payload
            // into account
            auto tx2 = MalleateProTxPayout<CProRegTx>(tx);
            TxValidationState dummyState;
            // Technically, the payload is still valid...
            BOOST_ASSERT(CheckProRegTx(CTransaction(tx), m_node.chainman->ActiveChain().Tip(), dummyState, m_node.chainman->ActiveChainstate().CoinsTip(), false));
            BOOST_ASSERT(CheckProRegTx(CTransaction(tx2), m_node.chainman->ActiveChain().Tip(), dummyState, m_node.chainman->ActiveChainstate().CoinsTip(), false));
            // But the signature should not verify anymore
            BOOST_ASSERT(CheckTransactionSignature(m_node, tx));
            BOOST_ASSERT(!CheckTransactionSignature(m_node, tx2));
        }

        CreateAndProcessBlock({tx}, GetScriptForRawPubKey(coinbaseKey.GetPubKey()));

        if(deterministicMNManager)
            deterministicMNManager->UpdatedBlockTip(m_node.chainman->ActiveChain().Tip());

        BOOST_CHECK_EQUAL(*m_node.chain->getHeight() , nHeight + 1);
        
        auto mnList = deterministicMNManager->GetListAtChainTip();
        BOOST_ASSERT(mnList.HasMN(tx.GetHash()));

        nHeight++;
    }
    int DIP0003EnforcementHeightBackup = Params().GetConsensus().DIP0003EnforcementHeight;
    const_cast<Consensus::Params&>(Params().GetConsensus()).DIP0003EnforcementHeight = *m_node.chain->getHeight() + 1;
    
    CreateAndProcessBlock({}, GetScriptForRawPubKey(coinbaseKey.GetPubKey()));
    {
        LOCK(cs_main);
        if(deterministicMNManager)
            deterministicMNManager->UpdatedBlockTip(m_node.chainman->ActiveChain().Tip());
    }
    nHeight++;

    // check MN reward payments
    for (size_t i = 0; i < 20; i++) {
        auto mnList = deterministicMNManager->GetListAtChainTip();
        auto dmnExpectedPayee = mnList.GetMNPayee();

        CBlock block = CreateAndProcessBlock({}, GetScriptForRawPubKey(coinbaseKey.GetPubKey()));
        {
            LOCK(cs_main);
            if(deterministicMNManager)
                deterministicMNManager->UpdatedBlockTip(m_node.chainman->ActiveChain().Tip());
        }
        BOOST_ASSERT(!block.vtx.empty());

        auto dmnPayout = FindPayoutDmn(block);
        BOOST_ASSERT(dmnPayout != nullptr);
        BOOST_CHECK_EQUAL(dmnPayout->proTxHash.ToString(), dmnExpectedPayee->proTxHash.ToString());

        nHeight++;
    }

    // register multiple MNs per block
    for (size_t i = 0; i < 3; i++) {
        std::vector<CMutableTransaction> txns;
        for (size_t j = 0; j < 3; j++) {
            CKey ownerKey;
            CBLSSecretKey operatorKey;
            auto tx = CreateProRegTx(m_node, utxos, port++, GenerateRandomAddress(), coinbaseKey, ownerKey, operatorKey);
            dmnHashes.emplace_back(tx.GetHash());
            ownerKeys.emplace(tx.GetHash(), ownerKey);
            operatorKeys.emplace(tx.GetHash(), operatorKey);
            txns.emplace_back(tx);
        }
        CreateAndProcessBlock(txns, GetScriptForRawPubKey(coinbaseKey.GetPubKey()));
        {
            LOCK(cs_main);
            if(deterministicMNManager)
                deterministicMNManager->UpdatedBlockTip(m_node.chainman->ActiveChain().Tip());
            BOOST_CHECK_EQUAL(*m_node.chain->getHeight() , nHeight + 1);
        }

        for (size_t j = 0; j < 3; j++) {
            BOOST_ASSERT(deterministicMNManager->GetListAtChainTip().HasMN(txns[j].GetHash()));
        }

        nHeight++;
    }

    // test ProUpServTx
    auto tx = CreateProUpServTx(m_node, utxos, dmnHashes[0], operatorKeys[dmnHashes[0]], 1000, coinbaseKey);
    CreateAndProcessBlock({tx}, GetScriptForRawPubKey(coinbaseKey.GetPubKey()));
    {
        LOCK(cs_main);
        deterministicMNManager->UpdatedBlockTip(m_node.chainman->ActiveChain().Tip());
    }
    BOOST_CHECK_EQUAL(*m_node.chain->getHeight() , nHeight + 1);
    nHeight++;
    auto dmn = deterministicMNManager->GetListAtChainTip().GetMN(dmnHashes[0]);
    BOOST_ASSERT(dmn != nullptr && dmn->pdmnState->addr.GetPort() == 1000);

    // test ProUpRevTx
    tx = CreateProUpRevTx(m_node, utxos, dmnHashes[0], operatorKeys[dmnHashes[0]], coinbaseKey);
    CreateAndProcessBlock({tx}, GetScriptForRawPubKey(coinbaseKey.GetPubKey()));
    {
        LOCK(cs_main);
        if(deterministicMNManager)
            deterministicMNManager->UpdatedBlockTip(m_node.chainman->ActiveChain().Tip());
    }
    BOOST_CHECK_EQUAL(*m_node.chain->getHeight() , nHeight + 1);
    
    nHeight++;
    dmn = deterministicMNManager->GetListAtChainTip().GetMN(dmnHashes[0]);
    BOOST_ASSERT(dmn != nullptr && dmn->pdmnState->GetBannedHeight() == nHeight);

    // test that the revoked MN does not get paid anymore
    for (size_t i = 0; i < 20; i++) {
        auto dmnExpectedPayee = deterministicMNManager->GetListAtChainTip().GetMNPayee();
        BOOST_ASSERT(dmnExpectedPayee->proTxHash != dmnHashes[0]);

        CBlock block = CreateAndProcessBlock({}, GetScriptForRawPubKey(coinbaseKey.GetPubKey()));
        {
            LOCK(cs_main);
            if(deterministicMNManager)
                deterministicMNManager->UpdatedBlockTip(m_node.chainman->ActiveChain().Tip());
        }
        BOOST_ASSERT(!block.vtx.empty());

        auto dmnPayout = FindPayoutDmn(block);
        BOOST_ASSERT(dmnPayout != nullptr);
        BOOST_CHECK_EQUAL(dmnPayout->proTxHash.ToString(), dmnExpectedPayee->proTxHash.ToString());

        nHeight++;
    }

    // test reviving the MN
    CBLSSecretKey newOperatorKey;
    newOperatorKey.MakeNewKey();
    dmn = deterministicMNManager->GetListAtChainTip().GetMN(dmnHashes[0]);
    tx = CreateProUpRegTx(m_node, utxos, dmnHashes[0], ownerKeys[dmnHashes[0]], newOperatorKey.GetPublicKey(), ownerKeys[dmnHashes[0]].GetPubKey().GetID(), dmn->pdmnState->scriptPayout, coinbaseKey);
    {
        LOCK(cs_main);
        // check malleability protection again, but this time by also relying on the signature inside the ProUpRegTx
        auto tx2 = MalleateProTxPayout<CProUpRegTx>(tx);
        TxValidationState dummyState;
        BOOST_ASSERT(CheckProUpRegTx(CTransaction(tx), m_node.chainman->ActiveChain().Tip(), dummyState, m_node.chainman->ActiveChainstate().CoinsTip(), false));
        BOOST_ASSERT(!CheckProUpRegTx(CTransaction(tx2), m_node.chainman->ActiveChain().Tip(), dummyState, m_node.chainman->ActiveChainstate().CoinsTip(), false));
        BOOST_ASSERT(CheckTransactionSignature(m_node, tx));
        BOOST_ASSERT(!CheckTransactionSignature(m_node, tx2));
    }
    // now process the block
    CreateAndProcessBlock({tx}, GetScriptForRawPubKey(coinbaseKey.GetPubKey()));
    {
        LOCK(cs_main);
        if(deterministicMNManager)
            deterministicMNManager->UpdatedBlockTip(m_node.chainman->ActiveChain().Tip());
    }
    BOOST_CHECK_EQUAL(*m_node.chain->getHeight() , nHeight + 1);
    nHeight++;

    tx = CreateProUpServTx(m_node, utxos, dmnHashes[0], newOperatorKey, 100, coinbaseKey);
    CreateAndProcessBlock({tx}, GetScriptForRawPubKey(coinbaseKey.GetPubKey()));
    {
        LOCK(cs_main);
        if(deterministicMNManager)
            deterministicMNManager->UpdatedBlockTip(m_node.chainman->ActiveChain().Tip());
    }
    BOOST_CHECK_EQUAL(*m_node.chain->getHeight() , nHeight + 1);
    nHeight++;
    dmn = deterministicMNManager->GetListAtChainTip().GetMN(dmnHashes[0]);
    BOOST_ASSERT(dmn != nullptr && dmn->pdmnState->addr.GetPort() == 100);
    BOOST_ASSERT(dmn != nullptr && !dmn->pdmnState->IsBanned());

    // test that the revived MN gets payments again
    bool foundRevived = false;
    for (size_t i = 0; i < 20; i++) {
        auto dmnExpectedPayee = deterministicMNManager->GetListAtChainTip().GetMNPayee();
        if (dmnExpectedPayee->proTxHash == dmnHashes[0]) {
            foundRevived = true;
        }

        CBlock block = CreateAndProcessBlock({}, GetScriptForRawPubKey(coinbaseKey.GetPubKey()));
        {
            LOCK(cs_main);
            if(deterministicMNManager)
                deterministicMNManager->UpdatedBlockTip(m_node.chainman->ActiveChain().Tip());
        }
        BOOST_ASSERT(!block.vtx.empty());

        auto dmnPayout = FindPayoutDmn(block);
        BOOST_ASSERT(dmnPayout != nullptr);
        BOOST_CHECK_EQUAL(dmnPayout->proTxHash.ToString(), dmnExpectedPayee->proTxHash.ToString());

        nHeight++;
    }
    BOOST_ASSERT(foundRevived);

    const_cast<Consensus::Params&>(Params().GetConsensus()).DIP0003EnforcementHeight = DIP0003EnforcementHeightBackup;
}

BOOST_FIXTURE_TEST_CASE(dip3_test_mempool_reorg, TestChainDIP3Setup)
{
    int nHeight = *m_node.chain->getHeight();
    auto utxos = BuildSimpleUTXOVec(m_coinbase_txns);

    CKey ownerKey;
    CKey payoutKey;
    CKey collateralKey;
    CBLSSecretKey operatorKey;

    ownerKey.MakeNewKey(true);
    payoutKey.MakeNewKey(true);
    collateralKey.MakeNewKey(true);
    operatorKey.MakeNewKey();

    auto scriptPayout = GetScriptForDestination(PKHash(payoutKey.GetPubKey()));
    auto scriptCollateral = GetScriptForDestination(PKHash(collateralKey.GetPubKey()));

    // Create a MN with an external collateral
    CMutableTransaction tx_collateral;
    FundTransaction(m_node, tx_collateral, utxos, scriptCollateral, 100 * COIN);
    SignTransaction(m_node, tx_collateral, coinbaseKey);

    auto block = CreateAndProcessBlock({tx_collateral}, GetScriptForRawPubKey(coinbaseKey.GetPubKey()));
    deterministicMNManager->UpdatedBlockTip(m_node.chainman->ActiveChain().Tip());
    BOOST_CHECK_EQUAL(*m_node.chain->getHeight() , nHeight + 1);
    BOOST_CHECK_EQUAL(block.GetHash() , m_node.chain->getBlockHash(*m_node.chain->getHeight()));

    CProRegTx payload;
    payload.addr = LookupNumeric("1.1.1.1", 1);
    payload.keyIDOwner = ownerKey.GetPubKey().GetID();
    payload.pubKeyOperator = operatorKey.GetPublicKey();
    payload.keyIDVoting = ownerKey.GetPubKey().GetID();
    payload.scriptPayout = scriptPayout;

    for (size_t i = 0; i < tx_collateral.vout.size(); ++i) {
        if (tx_collateral.vout[i].nValue == 100 * COIN) {
            payload.collateralOutpoint = COutPoint(tx_collateral.GetHash(), i);
            break;
        }
    }

    CMutableTransaction tx_reg;
    tx_reg.nVersion = SYSCOIN_TX_VERSION_MN_REGISTER;
    FundTransaction(m_node, tx_reg, utxos, scriptPayout, 100 * COIN);
    payload.inputsHash = CalcTxInputsHash(CTransaction(tx_reg));
    CMessageSigner::SignMessage(payload.MakeSignString(), payload.vchSig, collateralKey);
    SetTxPayload(tx_reg, payload);
    SignTransaction(m_node, tx_reg, coinbaseKey);

    CTxMemPool testPool;
    TestMemPoolEntryHelper entry;
    LOCK2(cs_main, testPool.cs);
    // Create ProUpServ and test block reorg which double-spend ProRegTx
    auto tx_up_serv = CreateProUpServTx(m_node, utxos, tx_reg.GetHash(), operatorKey, 2, coinbaseKey);
    testPool.addUnchecked(entry.FromTx(tx_up_serv));
    // A disconnected block would insert ProRegTx back into mempool
    testPool.addUnchecked(entry.FromTx(tx_reg));
    BOOST_CHECK_EQUAL(testPool.size(), 2U);

    // Create a tx that will double-spend ProRegTx
    CMutableTransaction tx_reg_ds;
    tx_reg_ds.vin = tx_reg.vin;
    tx_reg_ds.vout.emplace_back(0, CScript() << OP_RETURN);
    SignTransaction(m_node, tx_reg_ds, coinbaseKey);

    // Check mempool as if a new block with tx_reg_ds was connected instead of the old one with tx_reg
    std::vector<CTransactionRef> block_reorg;
    block_reorg.emplace_back(std::make_shared<CTransaction>(tx_reg_ds));
    testPool.removeForBlock(block_reorg, nHeight + 2);
    BOOST_CHECK_EQUAL(testPool.size(), 0U);
}

BOOST_FIXTURE_TEST_CASE(dip3_test_mempool_dual_proregtx, TestChainDIP3Setup)
{
    auto utxos = BuildSimpleUTXOVec(m_coinbase_txns);

    // Create a MN
    CKey ownerKey1;
    CBLSSecretKey operatorKey1;
    auto tx_reg1 = CreateProRegTx(m_node, utxos, 1, GenerateRandomAddress(), coinbaseKey, ownerKey1, operatorKey1);

    // Create a MN with an external collateral that references tx_reg1
    CKey ownerKey;
    CKey payoutKey;
    CKey collateralKey;
    CBLSSecretKey operatorKey;

    ownerKey.MakeNewKey(true);
    payoutKey.MakeNewKey(true);
    collateralKey.MakeNewKey(true);
    operatorKey.MakeNewKey();

    auto scriptPayout = GetScriptForDestination(PKHash(payoutKey.GetPubKey()));
    auto scriptCollateral = GetScriptForDestination(PKHash(collateralKey.GetPubKey()));

    CProRegTx payload;
    payload.addr = LookupNumeric("1.1.1.1", 2);
    payload.keyIDOwner = ownerKey.GetPubKey().GetID();
    payload.pubKeyOperator = operatorKey.GetPublicKey();
    payload.keyIDVoting = ownerKey.GetPubKey().GetID();
    payload.scriptPayout = scriptPayout;

    for (size_t i = 0; i < tx_reg1.vout.size(); ++i) {
        if (tx_reg1.vout[i].nValue == 100 * COIN) {
            payload.collateralOutpoint = COutPoint(tx_reg1.GetHash(), i);
            break;
        }
    }

    CMutableTransaction tx_reg2;
    tx_reg2.nVersion = SYSCOIN_TX_VERSION_MN_REGISTER;
    FundTransaction(m_node, tx_reg2, utxos, scriptPayout, 100 * COIN);
    payload.inputsHash = CalcTxInputsHash(CTransaction(tx_reg2));
    CMessageSigner::SignMessage(payload.MakeSignString(), payload.vchSig, collateralKey);
    SetTxPayload(tx_reg2, payload);
    SignTransaction(m_node, tx_reg2, coinbaseKey);

    CTxMemPool testPool;
    TestMemPoolEntryHelper entry;
    LOCK2(cs_main, testPool.cs);

    testPool.addUnchecked(entry.FromTx(tx_reg1));
    BOOST_CHECK_EQUAL(testPool.size(), 1U);
    BOOST_CHECK(testPool.existsProviderTxConflict(CTransaction(tx_reg2)));
}

BOOST_FIXTURE_TEST_CASE(dip3_verify_db, TestChainDIP3Setup)
{
    int nHeight = *m_node.chain->getHeight();
    auto utxos = BuildSimpleUTXOVec(m_coinbase_txns);

    CKey ownerKey;
    CKey payoutKey;
    CKey collateralKey;
    CBLSSecretKey operatorKey;

    ownerKey.MakeNewKey(true);
    payoutKey.MakeNewKey(true);
    collateralKey.MakeNewKey(true);
    operatorKey.MakeNewKey();

    auto scriptPayout = GetScriptForDestination(PKHash(payoutKey.GetPubKey()));
    auto scriptCollateral = GetScriptForDestination(PKHash(collateralKey.GetPubKey()));

    // Create a MN with an external collateral
    CMutableTransaction tx_collateral;
    FundTransaction(m_node, tx_collateral, utxos, scriptCollateral, 100 * COIN);
    SignTransaction(m_node, tx_collateral, coinbaseKey);


    auto block = CreateAndProcessBlock({tx_collateral}, GetScriptForRawPubKey(coinbaseKey.GetPubKey()));
    deterministicMNManager->UpdatedBlockTip(m_node.chainman->ActiveChain().Tip());
    BOOST_ASSERT(*m_node.chain->getHeight() == nHeight + 1);
    BOOST_ASSERT(block.GetHash() == m_node.chain->getBlockHash(*m_node.chain->getHeight()));

    CProRegTx payload;
    payload.addr = LookupNumeric("1.1.1.1", 1);
    payload.keyIDOwner = ownerKey.GetPubKey().GetID();
    payload.pubKeyOperator = operatorKey.GetPublicKey();
    payload.keyIDVoting = ownerKey.GetPubKey().GetID();
    payload.scriptPayout = scriptPayout;

    for (size_t i = 0; i < tx_collateral.vout.size(); ++i) {
        if (tx_collateral.vout[i].nValue == 100 * COIN) {
            payload.collateralOutpoint = COutPoint(tx_collateral.GetHash(), i);
            break;
        }
    }

    CMutableTransaction tx_reg;
    tx_reg.nVersion = SYSCOIN_TX_VERSION_MN_REGISTER;
    FundTransaction(m_node, tx_reg, utxos, scriptPayout, 100 * COIN);
    payload.inputsHash = CalcTxInputsHash(CTransaction(tx_reg));
    CMessageSigner::SignMessage(payload.MakeSignString(), payload.vchSig, collateralKey);
    SetTxPayload(tx_reg, payload);
    SignTransaction(m_node, tx_reg, coinbaseKey);

    auto tx_reg_hash = tx_reg.GetHash();

    block = CreateAndProcessBlock({tx_reg}, GetScriptForRawPubKey(coinbaseKey.GetPubKey()));
    deterministicMNManager->UpdatedBlockTip(m_node.chainman->ActiveChain().Tip());
    BOOST_CHECK_EQUAL(*m_node.chain->getHeight() , nHeight + 2);
    BOOST_CHECK_EQUAL(block.GetHash() , m_node.chain->getBlockHash(*m_node.chain->getHeight()));
    BOOST_ASSERT(deterministicMNManager->GetListAtChainTip().HasMN(tx_reg_hash));

    // Now spend the collateral while updating the same MN
    SimpleUTXOVec collateral_utxos;
    collateral_utxos.emplace_back(payload.collateralOutpoint, std::make_pair(1, 100 * COIN));
    auto proUpRevTx = CreateProUpRevTx(m_node, collateral_utxos, tx_reg_hash, operatorKey, collateralKey);

    block = CreateAndProcessBlock({proUpRevTx}, GetScriptForRawPubKey(coinbaseKey.GetPubKey()));
    deterministicMNManager->UpdatedBlockTip(m_node.chainman->ActiveChain().Tip());
    BOOST_CHECK_EQUAL(*m_node.chain->getHeight() , nHeight + 3);
    BOOST_CHECK_EQUAL(block.GetHash() , m_node.chain->getBlockHash(*m_node.chain->getHeight()));
    BOOST_ASSERT(!deterministicMNManager->GetListAtChainTip().HasMN(tx_reg_hash));
    LOCK(cs_main);
    CChainState& active_chainstate = m_node.chainman->ActiveChainstate();
    // Verify db consistency
    BOOST_ASSERT(CVerifyDB().VerifyDB(active_chainstate, Params(), active_chainstate.CoinsTip(), 4, 2));
}

BOOST_AUTO_TEST_SUITE_END()
