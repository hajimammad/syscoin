﻿// Copyright (c) 2013-2019 The Syscoin Core developers
// Distributed under the MIT software license, see the accompanying
// file COPYING or http://www.opensource.org/licenses/mit-license.php.

#include <validation.h>
#include <services/asset.h>
#include <services/assetconsensus.h>
#include <consensus/validation.h>
#include <dbwrapper.h>

std::string stringFromSyscoinTx(const int &nVersion) {
    switch (nVersion) {
    case SYSCOIN_TX_VERSION_ASSET_ACTIVATE:
		return "assetactivate";
    case SYSCOIN_TX_VERSION_ASSET_UPDATE:
		return "assetupdate";     
	case SYSCOIN_TX_VERSION_ASSET_SEND:
		return "assetsend";
	case SYSCOIN_TX_VERSION_ALLOCATION_SEND:
		return "assetallocationsend";
	case SYSCOIN_TX_VERSION_ALLOCATION_BURN_TO_ETHEREUM:
		return "assetallocationburntoethereum"; 
	case SYSCOIN_TX_VERSION_ALLOCATION_BURN_TO_SYSCOIN:
		return "assetallocationburntosyscoin";
	case SYSCOIN_TX_VERSION_SYSCOIN_BURN_TO_ALLOCATION:
		return "syscoinburntoassetallocation";            
    case SYSCOIN_TX_VERSION_ALLOCATION_MINT:
        return "assetallocationmint";   
    default:
        return "<unknown assetallocation op>";
    }
}

std::vector<unsigned char> vchFromString(const std::string &str) {
	unsigned char *strbeg = (unsigned char*)str.c_str();
	return std::vector<unsigned char>(strbeg, strbeg + str.size());
}
std::string stringFromVch(const std::vector<unsigned char> &vch) {
	std::string res;
	std::vector<unsigned char>::const_iterator vi = vch.begin();
	while (vi != vch.end()) {
		res += (char)(*vi);
		vi++;
	}
	return res;
}




bool CAsset::UnserializeFromData(const std::vector<unsigned char> &vchData) {
    try {
		CDataStream dsAsset(vchData, SER_NETWORK, PROTOCOL_VERSION);
		Unserialize(dsAsset);
    } catch (std::exception &e) {
		SetNull();
        return false;
    }
	return true;
}

bool CAsset::UnserializeFromTx(const CTransaction &tx) {
	std::vector<unsigned char> vchData;
	int nOut;
	if (!GetSyscoinData(tx, vchData, nOut))
	{
		SetNull();
		return false;
	}
	if(!UnserializeFromData(vchData))
	{	
		SetNull();
		return false;
	}
    return true;
}


uint32_t GenerateSyscoinGuid(const COutPoint& outPoint) {
    const arith_uint256 &txidArith = UintToArith256(outPoint.hash);
    return txidArith.GetLow32();
}

void CAsset::SerializeData( std::vector<unsigned char> &vchData) {
    CDataStream dsAsset(SER_NETWORK, PROTOCOL_VERSION);
    Serialize(dsAsset);
	vchData = std::vector<unsigned char>(dsAsset.begin(), dsAsset.end());
}

bool GetAsset(const uint32_t &nAsset,
        CAsset& txPos) {
    if (passetdb == nullptr || !passetdb->ReadAsset(nAsset, txPos))
        return false;
    return true;
}

bool CheckTxInputsAssets(const CTransaction &tx, TxValidationState &state, const uint32_t &nAsset, std::unordered_map<uint32_t, std::pair<bool, uint64_t> > &mapAssetIn, const std::unordered_map<uint32_t, std::pair<bool, uint64_t> > &mapAssetOut) {
    if (mapAssetOut.empty()) {
        return state.Invalid(TxValidationResult::TX_CONSENSUS, "bad-txns-asset-outputs-empty");
    }
    std::unordered_map<uint32_t, std::pair<bool, uint64_t> >::const_iterator itOut;
    const bool &isNoInput = IsSyscoinWithNoInputTx(tx.nVersion);
    if(isNoInput) {
        itOut = mapAssetOut.find(nAsset);
        if (itOut == mapAssetOut.end()) {
            return state.Invalid(TxValidationResult::TX_CONSENSUS, "bad-txns-asset-output-first-asset-not-found");
        }
    }
    // case 1: asset send without zero val input, covered by this check
    // case 2: asset send with zero val of another asset, covered by mapAssetIn != mapAssetOut
    // case 3: asset send with multiple zero val input/output, covered by GetAssetValueOut() for output and CheckTxInputs() for input
    // case 4: asset send sending assets without inputs of those assets, covered by this check + mapAssetIn != mapAssetOut
    if (tx.nVersion == SYSCOIN_TX_VERSION_ASSET_SEND) {
        if (mapAssetIn.empty()) {
            return state.Invalid(TxValidationResult::TX_CONSENSUS, "bad-txns-assetsend-inputs-empty");
        }
        auto itIn = mapAssetIn.find(nAsset);
        if (itIn == mapAssetIn.end()) {
            return state.Invalid(TxValidationResult::TX_CONSENSUS, "bad-txns-assetsend-input-first-asset-not-found");
        }
        // check that the first input asset and first output asset match
        // note that we only care about first asset because below we will end up enforcing in == out for the rest
        if (itIn->first != itOut->first) {
            return state.Invalid(TxValidationResult::TX_CONSENSUS, "bad-txns-assetsend-guid-mismatch");
        }
        // check that the first input asset has zero val input spent
        if (!itIn->second.first) {
            return state.Invalid(TxValidationResult::TX_CONSENSUS, "bad-txns-assetsend-missing-zero-val-input");
        }
        // check that the first output asset has zero val output
        if (!itOut->second.first) {
            return state.Invalid(TxValidationResult::TX_CONSENSUS, "bad-txns-assetsend-missing-zero-val-output");
        }
    }
    // asset send also falls into this so for the first out we need to check for asset send seperately above
    if (isNoInput) {
        // add first asset out to in so it matches, the rest should be the same
        // the first one is verified by checksyscoininputs() later on (part of asset send is also)
        // emplace will add if it doesn't exist or update it below
        auto it = mapAssetIn.emplace(nAsset, std::make_pair(itOut->second.first, itOut->second.second));
        if (!it.second) {
            it.first->second = std::make_pair(itOut->second.first, itOut->second.second);
        }
    }
    // this will check that all assets with inputs match amounts being sent on outputs
    // it will also ensure that inputs and outputs per asset are equal with respects to zero-val inputs/outputs (asset ownership utxos) 
    if (mapAssetIn != mapAssetOut) {
        return state.Invalid(TxValidationResult::TX_CONSENSUS, "bad-txns-assetsend-io-mismatch");
    }
    return true;
}