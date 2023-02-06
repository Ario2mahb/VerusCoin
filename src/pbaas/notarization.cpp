/********************************************************************
 * (C) 2019 Michael Toutonghi
 *
 * Distributed under the MIT software license, see the accompanying
 * file COPYING or http://www.opensource.org/licenses/mit-license.php.
 *
 * This implements the public blockchains as a service (PBaaS) notarization protocol, VerusLink.
 * VerusLink is a new distributed consensus protocol that enables multiple public blockchains
 * to operate as a decentralized ecosystem of chains, which can interact and easily engage in cross
 * chain transactions.
 *
 */

#include <univalue.h>
#include "main.h"
#include "txdb.h"
#include "rpc/pbaasrpc.h"
#include "transaction_builder.h"
#include "cc/StakeGuard.h"

#include <assert.h>

using namespace std;

extern uint160 VERUS_CHAINID;
extern uint160 ASSETCHAINS_CHAINID;
extern string VERUS_CHAINNAME;
extern string PBAAS_HOST;
extern string PBAAS_USERPASS;
extern int32_t PBAAS_PORT;
extern int32_t VERUS_MIN_STAKEAGE;
extern std::string NOTARY_PUBKEY;

CNotaryEvidence::CNotaryEvidence(const UniValue &uni)
{
    version = uni_get_int(find_value(uni, "version"), VERSION_CURRENT);
    type = uni_get_int(find_value(uni, "type"));
    systemID = GetDestinationID(DecodeDestination(uni_get_str(find_value(uni, "systemid"))));
    output = CUTXORef(find_value(uni, "output"));
    state = uni_get_int(find_value(uni, "state"));
    evidence = CCrossChainProof(find_value(uni, "evidence"));
    if (!evidence.IsValid())
    {
        version = VERSION_INVALID;
    }
}

std::vector<CNotarySignature> CNotaryEvidence::GetConfirmedAndRejectedSignatureMaps(
                                                        const std::vector<CNotarySignature> &allSigVec,
                                                        std::map<uint32_t, std::map<CIdentityID, CIdentitySignature>> &confirmedByHeight,
                                                        std::map<uint32_t, std::map<CIdentityID, CIdentitySignature>> &rejectedByHeight)
{
    CUTXORef output;
    uint160 systemID;
    for (auto notarySig : allSigVec)
    {
        if (output.hash.IsNull())
        {
            output = notarySig.output;
            systemID = notarySig.systemID;
        }
        if (notarySig.IsConfirmed())
        {
            // get the signature proof and add to it, do not make a new one
            for (auto &oneSignature : notarySig.signatures)
            {
                if (confirmedByHeight[oneSignature.second.blockHeight].count(oneSignature.first))
                {
                    // all signatures are the same height, merge the CIdentitySignature
                    for (auto &oneKeySig : oneSignature.second.signatures)
                    {
                        confirmedByHeight[oneSignature.second.blockHeight][oneSignature.first].AddSignature(oneKeySig);
                    }
                }
                else
                {
                    confirmedByHeight[oneSignature.second.blockHeight].insert(oneSignature);
                }
            }
        }
        else
        {
            // get the signature proof and add to it, do not make a new one
            for (auto &oneSignature : notarySig.signatures)
            {
                if (rejectedByHeight[oneSignature.second.blockHeight].count(oneSignature.first))
                {
                    // all signatures are the same height, merge the CIdentitySignature
                    for (auto &oneKeySig : oneSignature.second.signatures)
                    {
                        rejectedByHeight[oneSignature.second.blockHeight][oneSignature.first].AddSignature(oneKeySig);
                    }
                }
                else
                {
                    rejectedByHeight[oneSignature.second.blockHeight].insert(oneSignature);
                }
            }
        }
    }

    std::vector<CNotarySignature> retVal;

    std::multimap<uint32_t, CNotarySignature> combinedSignatures;
    for (auto oneHeightEntry : rejectedByHeight)
    {
        combinedSignatures.insert(std::make_pair(oneHeightEntry.first, CNotarySignature(systemID, output, false, oneHeightEntry.second)));
    }
    for (auto oneHeightEntry : confirmedByHeight)
    {
        combinedSignatures.insert(std::make_pair(oneHeightEntry.first, CNotarySignature(systemID, output, true, oneHeightEntry.second)));
    }
    for (auto &oneNotarySig : combinedSignatures)
    {
        retVal.push_back(oneNotarySig.second);
    }
    return retVal;
}

CNotaryEvidence &CNotaryEvidence::MergeEvidence(const CNotaryEvidence &mergeWith, bool aggregateSignatures, bool onlySignatures)
{
    std::vector<CNotarySignature> notarySignatures;

    if (output.IsNull() && !mergeWith.output.IsNull())
    {
        output = mergeWith.output;
    }
    if (systemID.IsNull() && !mergeWith.systemID.IsNull())
    {
        systemID = mergeWith.systemID;
    }

    std::map<uint32_t, std::map<CIdentityID, CIdentitySignature>> confirmedByHeight;
    std::map<uint32_t, std::map<CIdentityID, CIdentitySignature>> rejectedByHeight;

    for (auto oneRef : mergeWith.evidence.chainObjects)
    {
        if (aggregateSignatures && oneRef->objectType == CHAINOBJ_NOTARYSIGNATURE)
        {
            notarySignatures.push_back(((CChainObject<CNotarySignature> *)oneRef)->object);
        }
        else if (!onlySignatures || oneRef->objectType == CHAINOBJ_NOTARYSIGNATURE)
        {
            evidence << oneRef;
        }
    }

    if (notarySignatures.size())
    {
        auto orderedNotarySigs = GetConfirmedAndRejectedSignatureMaps(notarySignatures, confirmedByHeight, rejectedByHeight);

        // place all signature blocks at the beginning of the evidence in order of height by inserting them at 0 in reverse order
        for (auto it = orderedNotarySigs.rbegin(); it != orderedNotarySigs.rend(); it++)
        {
            evidence.insert(0, *it);
        }
    }
    return *this;
}

CIdentitySignature::ESignatureVerification CNotaryEvidence::SignConfirmed(const std::set<uint160> &notarySet, int minConfirming, const CKeyStore &keyStore, const CTransaction &txToConfirm, const CIdentityID &signWithID, uint32_t height, CCurrencyDefinition::EHashTypes hashType)
{
    if (!notarySet.count(signWithID))
    {
        LogPrintf("%s: Attempting to sign as notary with unauthorized ID\n", __func__);
        return CIdentitySignature::SIGNATURE_INVALID;
    }

    COptCCParams p;

    if (txToConfirm.GetHash() != output.hash ||
        txToConfirm.vout.size() <= output.n ||
        !txToConfirm.vout[output.n].scriptPubKey.IsPayToCryptoCondition(p) ||
        !p.vData.size() ||
        !p.vData[0].size() ||
        p.evalCode == EVAL_NONE)
    {
        LogPrintf("%s: Attempting to sign an invalid or incompatible object\n", __func__);
        return CIdentitySignature::SIGNATURE_INVALID;
    }

    // write the object to the hash writer without a vector length prefix
    uint256 objHash;
    uint256 outputUTXOHash;
    {
        CNativeHashWriter hw(hashType);
        objHash = hw.write((const char *)&(p.vData[0][0]), p.vData[0].size()).GetHash();
    }
    {
        CNativeHashWriter hw(hashType);
        hw << output;
        outputUTXOHash = hw.GetHash();
    }

    uint32_t decisionHeight;
    std::map<CIdentityID, CIdentitySignature> confirmedAtHeight;
    std::map<CIdentityID, CIdentitySignature> rejectedAtHeight;

    CNotaryEvidence::EStates sigCheckResult = CheckSignatureConfirmation(objHash,
                                                                         notarySet,
                                                                         minConfirming,
                                                                         height,
                                                                         &decisionHeight,
                                                                         &confirmedAtHeight,
                                                                         &rejectedAtHeight);

    if (sigCheckResult == CNotaryEvidence::EStates::STATE_CONFIRMED || sigCheckResult == CNotaryEvidence::EStates::STATE_REJECTED)
    {
        return CIdentitySignature::ESignatureVerification::SIGNATURE_COMPLETE;
    }
    else if (sigCheckResult == CNotaryEvidence::EStates::STATE_INVALID)
    {
        return CIdentitySignature::ESignatureVerification::SIGNATURE_INVALID;
    }

    // sign for anything we can that is not already in the confirmedAtHeight signature block if decisionHeight is our height
    int currentNumSigs = (decisionHeight == height && confirmedAtHeight.count(signWithID)) ? confirmedAtHeight[signWithID].signatures.size() : 0;

    // all signatures present for this ID are correct, so we recover all pub keys and IDs,
    // then see if we have any of the keys in our wallet that are in the ID and have not already
    // signed
    CIdentity sigIdentity = CIdentity::LookupIdentity(signWithID, height);
    if (!sigIdentity.IsValid())
    {
        LogPrint("notarization", "%s: failed lookup of identity for rejection: %s\n", __func__, EncodeDestination(signWithID).c_str());
        return CIdentitySignature::ESignatureVerification::SIGNATURE_INVALID;
    }
    else
    {
        int sigsNeeded = std::max(sigIdentity.minSigs - currentNumSigs, 0);
        if (!sigsNeeded)
        {
            LogPrint("notarization", "%s: Already signed with ID\n", __func__);
            return CIdentitySignature::SIGNATURE_COMPLETE;
        }

        CIdentitySignature idSignature(height, std::set<std::vector<unsigned char>>(), hashType);

        uint256 signatureHash = idSignature.IdentitySignatureHash(std::vector<uint160>({NotaryConfirmedKey()}),
                                                                  std::vector<std::string>(),
                                                                  std::vector<uint256>({outputUTXOHash}),
                                                                  systemID,
                                                                  height,
                                                                  signWithID,
                                                                  "",
                                                                  objHash);

        std::set<CTxDestination> validKeys;

        for (auto &oneDest : sigIdentity.primaryAddresses)
        {
            if (oneDest.which() == COptCCParams::ADDRTYPE_PK)
            {
                validKeys.insert(CKeyID(GetDestinationID(oneDest)));
            }
            else
            {
                validKeys.insert(oneDest);
            }
        }

        if (currentNumSigs)
        {
            for (auto &oneSig : confirmedAtHeight[signWithID].signatures)
            {
                CPubKey checkKey;
                checkKey.RecoverCompact(signatureHash, oneSig);
                validKeys.erase(checkKey.GetID());
            }
        }

        // as long as we can continue to make new signatures, we do
        for (auto keyIT = validKeys.begin(); sigsNeeded > 0 && keyIT != validKeys.end(); keyIT++)
        {
            CKey signingKey;
            std::vector<unsigned char> newSig;
            if (keyStore.GetKey(GetDestinationID(*keyIT), signingKey) && signingKey.SignCompact(signatureHash, newSig))
            {
                idSignature.signatures.insert(newSig);
                sigsNeeded--;
            }
        }

        if (idSignature.signatures.size())
        {
            AddToSignatures(notarySet, signWithID, idSignature, STATE_CONFIRMING);
        }

        CIdentitySignature::ESignatureVerification sigResult = idSignature.CheckSignature(sigIdentity,
                                                                                        std::vector<uint160>({NotaryConfirmedKey()}),
                                                                                        std::vector<std::string>(),
                                                                                        std::vector<uint256>({outputUTXOHash}),
                                                                                        systemID,
                                                                                        "",
                                                                                        objHash);

        if (sigResult == CIdentitySignature::ESignatureVerification::SIGNATURE_EMPTY ||
            sigResult == CIdentitySignature::ESignatureVerification::SIGNATURE_INVALID)
        {
            LogPrintf("%s: Error signing confirmed with ID: %s\n", __func__, sigIdentity.ToUniValue().write(1,2).c_str());
            return sigResult;
        }
        // if we have enough signatures for the ID, make sure we return complete
        if (sigsNeeded == 0)
        {
            return CIdentitySignature::ESignatureVerification::SIGNATURE_COMPLETE;
        }
        return sigResult;
    }
}

CIdentitySignature::ESignatureVerification CNotaryEvidence::SignRejected(const std::set<uint160> &notarySet,
                                                                         int minConfirming,
                                                                         const CKeyStore &keyStore,
                                                                         const CTransaction &txToConfirm,
                                                                         const CIdentityID &signWithID,
                                                                         uint32_t height,
                                                                         CCurrencyDefinition::EHashTypes hashType)
{
    if (!notarySet.count(signWithID))
    {
        LogPrintf("%s: Attempting to sign as notary with unauthorized ID\n", __func__);
        return CIdentitySignature::SIGNATURE_INVALID;
    }

    COptCCParams p;

    if (txToConfirm.GetHash() != output.hash ||
        txToConfirm.vout.size() <= output.n ||
        !txToConfirm.vout[output.n].scriptPubKey.IsPayToCryptoCondition(p) ||
        !p.vData.size() ||
        !p.vData[0].size() ||
        p.evalCode == EVAL_NONE)
    {
        LogPrintf("%s: Attempting to sign an invalid or incompatible object\n", __func__);
        return CIdentitySignature::SIGNATURE_INVALID;
    }

    // write the object to the hash writer without a vector length prefix
    std::map<CIdentityID, CIdentitySignature> confirmedAtHeight;
    std::map<CIdentityID, CIdentitySignature> rejectedAtHeight;

    // write the object to the hash writer without a vector length prefix
    uint256 objHash;
    uint256 outputUTXOHash;
    {
        CNativeHashWriter hw(hashType);
        objHash = hw.write((const char *)&(p.vData[0][0]), p.vData[0].size()).GetHash();
    }
    {
        CNativeHashWriter hw(hashType);
        hw << output;
        outputUTXOHash = hw.GetHash();
    }

    uint32_t decisionHeight;

    CNotaryEvidence::EStates sigCheckResult = CheckSignatureConfirmation(objHash,
                                                                         notarySet,
                                                                         minConfirming,
                                                                         height,
                                                                         &decisionHeight,
                                                                         &confirmedAtHeight,
                                                                         &rejectedAtHeight);

    if ((sigCheckResult == CNotaryEvidence::EStates::STATE_CONFIRMED && decisionHeight != height) ||
         sigCheckResult == CNotaryEvidence::EStates::STATE_REJECTED)
    {
        return CIdentitySignature::ESignatureVerification::SIGNATURE_COMPLETE;
    }
    else if (sigCheckResult == CNotaryEvidence::EStates::STATE_INVALID)
    {
        return CIdentitySignature::ESignatureVerification::SIGNATURE_INVALID;
    }

    // sign for anything we can that is not already in the rejectedAtHeight signature block if decisionHeight is our height
    int currentNumSigs = (decisionHeight == height && rejectedAtHeight.count(signWithID)) ? rejectedAtHeight[signWithID].signatures.size() : 0;

    // all signatures present for this ID are correct, so we recover all pub keys and IDs,
    // then see if we have any of the keys in our wallet that are in the ID and have not already
    // signed
    CIdentity sigIdentity = CIdentity::LookupIdentity(signWithID, height);
    if (!sigIdentity.IsValid())
    {
        LogPrint("notarization", "%s: failed lookup of identity for rejection: %s\n", __func__, EncodeDestination(signWithID).c_str());
        return CIdentitySignature::ESignatureVerification::SIGNATURE_INVALID;
    }
    else
    {
        int sigsNeeded = std::max(sigIdentity.minSigs - currentNumSigs, 0);
        if (!sigsNeeded)
        {
            LogPrint("notarization", "%s: Already signed with ID\n", __func__);
            return CIdentitySignature::SIGNATURE_COMPLETE;
        }

        CIdentitySignature idSignature(height, std::set<std::vector<unsigned char>>(), hashType);

        uint256 signatureHash = idSignature.IdentitySignatureHash(std::vector<uint160>({NotaryRejectedKey()}),
                                                                  std::vector<std::string>(),
                                                                  std::vector<uint256>({outputUTXOHash}),
                                                                  systemID,
                                                                  height,
                                                                  signWithID,
                                                                  "",
                                                                  objHash);

        std::set<CTxDestination> validKeys;

        for (auto &oneDest : sigIdentity.primaryAddresses)
        {
            if (oneDest.which() == COptCCParams::ADDRTYPE_PK)
            {
                validKeys.insert(CKeyID(GetDestinationID(oneDest)));
            }
            else
            {
                validKeys.insert(oneDest);
            }
        }

        if (currentNumSigs)
        {
            for (auto &oneSig : rejectedAtHeight[signWithID].signatures)
            {
                CPubKey checkKey;
                checkKey.RecoverCompact(signatureHash, oneSig);
                validKeys.erase(checkKey.GetID());
            }
        }

        // as long as we can continue to make new signatures, we do
        for (auto keyIT = validKeys.begin(); sigsNeeded > 0 && keyIT != validKeys.end(); keyIT++)
        {
            CKey signingKey;
            std::vector<unsigned char> newSig;
            if (keyStore.GetKey(GetDestinationID(*keyIT), signingKey) && signingKey.SignCompact(signatureHash, newSig))
            {
                idSignature.signatures.insert(newSig);
                sigsNeeded--;
            }
        }

        if (idSignature.signatures.size())
        {
            AddToSignatures(notarySet, signWithID, idSignature, STATE_REJECTING);
        }

        CIdentitySignature::ESignatureVerification sigResult = idSignature.CheckSignature(sigIdentity,
                                                                                          std::vector<uint160>({NotaryRejectedKey()}),
                                                                                          std::vector<std::string>(),
                                                                                          std::vector<uint256>({outputUTXOHash}),
                                                                                          systemID,
                                                                                          "",
                                                                                          objHash);

        if (sigResult == CIdentitySignature::ESignatureVerification::SIGNATURE_EMPTY ||
            sigResult == CIdentitySignature::ESignatureVerification::SIGNATURE_INVALID)
        {
            LogPrintf("%s: Error signing rejected with ID: %s\n", __func__, sigIdentity.ToUniValue().write(1,2).c_str());
            return sigResult;
        }
        // if we have enough signatures for the ID, make sure we return complete
        if (sigsNeeded == 0)
        {
            return CIdentitySignature::ESignatureVerification::SIGNATURE_COMPLETE;
        }
        return sigResult;
    }
}

CNotaryEvidence::EStates CNotaryEvidence::CheckSignatureConfirmation(const uint256 &objHash,
                                                                     const std::set<uint160> &notarySet,
                                                                     int minConfirming,
                                                                     uint32_t checkHeight,
                                                                     uint32_t *pDecisionHeight,
                                                                     std::map<CIdentityID, CIdentitySignature> *pConfirmedAtHeight,
                                                                     std::map<CIdentityID, CIdentitySignature> *pRejectedAtHeight,
                                                                     std::map<CIdentityID, std::set<std::vector<unsigned char>>> *pNotarySetRejects,
                                                                     std::map<CIdentityID, std::set<std::vector<unsigned char>>> *pNotarySetConfirms) const
{
    std::map<uint32_t, std::map<CIdentityID, CIdentitySignature>> confirmedByHeight;
    std::map<uint32_t, std::map<CIdentityID, CIdentitySignature>> rejectedByHeight;
    std::vector<CNotarySignature> notarySignatures = GetNotarySignatures(&confirmedByHeight, &rejectedByHeight);

    std::map<CIdentityID, std::set<std::vector<unsigned char>>> _notarySetRejects;
    std::map<CIdentityID, std::set<std::vector<unsigned char>>> _notarySetConfirms;
    std::map<CIdentityID, std::set<std::vector<unsigned char>>> &notarySetRejects = pNotarySetRejects ? *pNotarySetRejects : _notarySetRejects;
    std::map<CIdentityID, std::set<std::vector<unsigned char>>> &notarySetConfirms = pNotarySetConfirms ? *pNotarySetConfirms : _notarySetConfirms;

    CNotaryEvidence::EStates retVal = CNotaryEvidence::EStates::STATE_CONFIRMING;

    if (!checkHeight)
    {
        checkHeight = UINT32_MAX;
    }

    // if we have full rejection from any IDs in any period, that is considered rejection and that ID is blacklisted from accepting
    // if we have full confirmation in the most recent period, that is considered confirmation

    // write the object to the hash writer without a vector length prefix
    uint256 outputUTXOHash;
    {
        CCurrencyDefinition::EHashTypes hashType = CCurrencyDefinition::EHashTypes::HASH_BLAKE2BMMR;
        for (auto &oneSig : notarySignatures)
        {
            if (oneSig.signatures.size())
            {
                hashType = (CCurrencyDefinition::EHashTypes)oneSig.signatures.begin()->second.hashType;
                break;
            }
        }

        CNativeHashWriter hw(hashType);
        hw << output;
        outputUTXOHash = hw.GetHash();
    }

    // for every height, we check and merge
    uint32_t lastHeight = 0;

    for (auto &oneSigBlock : notarySignatures)
    {
        // we only care up to signing height
        uint32_t height = oneSigBlock.signatures.size() ? oneSigBlock.signatures.begin()->second.blockHeight : 0;
        if (!height)
        {
            continue;
        }

        // if this was signed on this chain, it isn't valid if signed after the check height
        if (oneSigBlock.systemID == ASSETCHAINS_CHAINID && height > checkHeight)
        {
            break;
        }

        if (height != lastHeight)
        {
            notarySetRejects.clear();
            notarySetConfirms.clear();
        }

        if (oneSigBlock.IsConfirmed())
        {
            for (auto &oneIDSig : oneSigBlock.signatures)
            {
                if (!notarySet.count(oneIDSig.first))
                {
                    LogPrint("notarization", "%s: unauthorized notary identity: %s\n", __func__, EncodeDestination(oneIDSig.first).c_str());
                    return EStates::STATE_INVALID;
                }

                CIdentity sigIdentity;
                // TODO: POST HARDENING - height from alternate chain was used
                // initially on testnet, remove this if there is ever a testnet reset
                // be sure to initialize sigIdentity when removing
                if (IsVerusActive() && !IsVerusMainnetActive() && checkHeight > 2500 && checkHeight < 2800)
                {
                    sigIdentity = CIdentity::LookupIdentity(oneIDSig.first, oneIDSig.second.blockHeight);
                }
                else
                {
                    sigIdentity = CIdentity::LookupIdentity(oneIDSig.first, oneSigBlock.systemID == ASSETCHAINS_CHAINID ?
                                                                                        oneIDSig.second.blockHeight :
                                                                                        checkHeight);
                }

                if (!sigIdentity.IsValid())
                {
                    LogPrint("notarization", "%s: invalid notary identity: %s\n", __func__, EncodeDestination(oneIDSig.first).c_str());
                    return EStates::STATE_INVALID;
                }

                // we cannot confirm based on the signature of a revoked identity, but that state does not veto either
                if (sigIdentity.IsRevoked())
                {
                    confirmedByHeight[oneIDSig.second.blockHeight].erase(oneIDSig.first);
                    continue;
                }

                std::vector<std::vector<unsigned char>> dupSigs;
                CIdentitySignature::ESignatureVerification result = oneIDSig.second.CheckSignature(sigIdentity,
                                                                                                   std::vector<uint160>({NotaryConfirmedKey()}),
                                                                                                   std::vector<std::string>(),
                                                                                                   std::vector<uint256>({outputUTXOHash}),
                                                                                                   systemID,
                                                                                                   "",
                                                                                                   objHash,
                                                                                                   &dupSigs);

                for (auto &oneDup : dupSigs)
                {
                    confirmedByHeight[oneIDSig.second.blockHeight][oneIDSig.first].signatures.erase(oneDup);
                }

                if (result == oneIDSig.second.SIGNATURE_INVALID)
                {
                    LogPrint("notarization", "%s: invalid notary signature: %s\n", __func__, EncodeDestination(oneIDSig.first).c_str());
                }

                if (result != oneIDSig.second.SIGNATURE_COMPLETE)
                {
                    // could store and return partial signatures here
                    continue;
                }
                notarySetConfirms[oneIDSig.first] = oneIDSig.second.signatures;
            }
        }
        else
        {
            for (auto &oneIDSig : oneSigBlock.signatures)
            {
                if (!notarySet.count(oneIDSig.first))
                {
                    LogPrint("notarization", "%s: unauthorized notary identity for rejection: %s\n", __func__, EncodeDestination(oneIDSig.first).c_str());
                    return EStates::STATE_INVALID;
                }

                CIdentity sigIdentity = CIdentity::LookupIdentity(oneIDSig.first, oneIDSig.second.blockHeight);
                if (!sigIdentity.IsValid())
                {
                    LogPrint("notarization", "%s: invalid notary identity for rejection: %s\n", __func__, EncodeDestination(oneIDSig.first).c_str());
                    return EStates::STATE_INVALID;
                }

                // we cannot reject based on the signature of a revoked identity, but that state does not veto either
                if (sigIdentity.IsRevoked())
                {
                    rejectedByHeight[oneIDSig.second.blockHeight].erase(oneIDSig.first);
                    continue;
                }

                std::vector<std::vector<unsigned char>> dupSigs;
                CIdentitySignature::ESignatureVerification result = oneIDSig.second.CheckSignature(sigIdentity,
                                                                                                   std::vector<uint160>({NotaryRejectedKey()}),
                                                                                                   std::vector<std::string>(),
                                                                                                   std::vector<uint256>({outputUTXOHash}),
                                                                                                   systemID,
                                                                                                   "",
                                                                                                   objHash,
                                                                                                   &dupSigs);

                for (auto &oneDup : dupSigs)
                {
                    rejectedByHeight[oneIDSig.second.blockHeight][oneIDSig.first].signatures.erase(oneDup);
                }

                /* if (result == oneIDSig.second.SIGNATURE_INVALID)
                {
                    LogPrint("notarization", "%s: invalid notary signature for rejection: %s\n", __func__, EncodeDestination(oneIDSig.first).c_str());
                    return EStates::STATE_INVALID;
                }
                else */
                if (result != oneIDSig.second.SIGNATURE_COMPLETE)
                {
                    // could store and return partial signatures here
                    continue;
                }
                notarySetRejects.insert(std::make_pair(oneIDSig.first, oneIDSig.second.signatures));
            }
        }

        lastHeight = height;

        if (notarySetRejects.size() >= minConfirming)
        {
            retVal = EStates::STATE_REJECTED;
            break;
        }
        else if (notarySetConfirms.size() >= minConfirming)
        {
            retVal = EStates::STATE_CONFIRMED;
            break;
        }
    }

    // TODO: POST HARDENING - remove this if there is ever a testnet reset
    if (IsVerusActive() &&
        !IsVerusMainnetActive() &&
        checkHeight &&
        checkHeight < 5800 &&
        this->IsConfirmed() &&
        notarySetConfirms.size() &&
        retVal != EStates::STATE_CONFIRMED)
    {
        retVal = EStates::STATE_CONFIRMED;
    }

    if (pDecisionHeight)
    {
        *pDecisionHeight = lastHeight;
    }
    if (pConfirmedAtHeight && lastHeight && confirmedByHeight.count(lastHeight))
    {
        *pConfirmedAtHeight = confirmedByHeight[lastHeight];
    }
    if (pRejectedAtHeight && lastHeight && rejectedByHeight.count(lastHeight))
    {
        *pRejectedAtHeight = rejectedByHeight[lastHeight];
    }

    if (retVal != EStates::STATE_CONFIRMED && retVal != EStates::STATE_REJECTED)
    {
        if (lastHeight == checkHeight &&
            notarySetConfirms.size() >= notarySetRejects.size())
        {
            retVal = EStates::STATE_CONFIRMING;
        }
        else
        {
            retVal = EStates::STATE_REJECTING;
        }
    }
    return retVal;
}

CPBaaSNotarization::CPBaaSNotarization(const CScript &scriptPubKey) :
                    nVersion(VERSION_INVALID),
                    flags(0),
                    notarizationHeight(0),
                    prevHeight(0)
{
    COptCCParams p;
    if (scriptPubKey.IsPayToCryptoCondition(p) &&
        p.IsValid() &&
        (p.evalCode == EVAL_ACCEPTEDNOTARIZATION || p.evalCode == EVAL_EARNEDNOTARIZATION) &&
        p.vData.size())
    {
        ::FromVector(p.vData[0], *this);
    }
}

CPBaaSNotarization::CPBaaSNotarization(const CTransaction &tx, int32_t *pOutIdx) :
                    nVersion(VERSION_INVALID),
                    flags(0),
                    notarizationHeight(0),
                    prevHeight(0)
{
    // the PBaaS notarization itself is a combination of proper inputs, one output, and
    // a sequence of opret chain objects as proof of the output values on the chain to which the
    // notarization refers, the opret can be reconstructed from chain data in order to validate
    // the txid of a transaction that does not contain the opret itself

    int32_t _outIdx;
    int32_t &outIdx = pOutIdx ? *pOutIdx : _outIdx;

    // a notarization must have notarization output that spends to the address indicated by the
    // ChainID, an opret, that there is only one, and that it can be properly decoded to a notarization
    // output, whether or not validate is true
    bool found = false;
    for (int i = 0; i < tx.vout.size(); i++)
    {
        COptCCParams p;
        if (tx.vout[i].scriptPubKey.IsPayToCryptoCondition(p) &&
            p.IsValid() &&
            (p.evalCode == EVAL_ACCEPTEDNOTARIZATION || p.evalCode == EVAL_EARNEDNOTARIZATION) &&
            p.vData.size())
        {
            if (found)
            {
                nVersion = VERSION_INVALID;
                proofRoots.clear();
                break;
            }
            else
            {
                found = true;
                outIdx = i;
                ::FromVector(p.vData[0], *this);
            }
        }
    }
}

uint160 ValidateCurrencyName(std::string currencyStr, bool ensureCurrencyValid=false, CCurrencyDefinition *pCurrencyDef=NULL);

CPBaaSNotarization::CPBaaSNotarization(const UniValue &obj)
{
    nVersion = (uint32_t)uni_get_int64(find_value(obj, "version"));
    flags = FLAGS_NONE;
    SetDefinitionNotarization(uni_get_bool(find_value(obj, "isdefinition")));
    SetBlockOneNotarization(uni_get_bool(find_value(obj, "isblockonenotarization")));
    SetPreLaunch(uni_get_bool(find_value(obj, "prelaunch")));
    SetLaunchCleared(uni_get_bool(find_value(obj, "launchcleared")));
    SetRefunding(uni_get_bool(find_value(obj, "refunding")));
    SetLaunchConfirmed(uni_get_bool(find_value(obj, "launchconfirmed")));
    SetLaunchComplete(uni_get_bool(find_value(obj, "launchcomplete")));
    if (uni_get_bool(find_value(obj, "ismirror")))
    {
        flags |= FLAG_ACCEPTED_MIRROR;
    }
    SetSameChain(uni_get_bool(find_value(obj, "samechain")));

    currencyID = ValidateCurrencyName(uni_get_str(find_value(obj, "currencyid")));
    if (currencyID.IsNull())
    {
        nVersion = VERSION_INVALID;
        return;
    }

    UniValue transferID = find_value(obj, "proposer");
    if (transferID.isObject())
    {
        proposer = CTransferDestination(transferID);
    }

    notarizationHeight = (uint32_t)uni_get_int64(find_value(obj, "notarizationheight"));
    currencyState = CCoinbaseCurrencyState(find_value(obj, "currencystate"));
    prevNotarization = CUTXORef(uint256S(uni_get_str(find_value(obj, "prevnotarizationtxid"))), (uint32_t)uni_get_int(find_value(obj, "prevnotarizationout")));
    hashPrevCrossNotarization = uint256S(uni_get_str(find_value(obj, "hashprevcrossnotarization")));
    prevHeight = (uint32_t)uni_get_int64(find_value(obj, "prevheight"));

    auto curStateArr = find_value(obj, "currencystates");
    auto proofRootArr = find_value(obj, "proofroots");
    auto nodesUni = find_value(obj, "nodes");

    if (curStateArr.isArray())
    {
        for (int i = 0; i < curStateArr.size(); i++)
        {
            std::vector<std::string> keys = curStateArr[i].getKeys();
            std::vector<UniValue> values = curStateArr[i].getValues();
            if (keys.size() != 1 or values.size() != 1)
            {
                nVersion = VERSION_INVALID;
                return;
            }
            currencyStates.insert(std::make_pair(GetDestinationID(DecodeDestination(keys[0])), CCoinbaseCurrencyState(values[0])));
        }
    }

    if (proofRootArr.isArray())
    {
        for (int i = 0; i < proofRootArr.size(); i++)
        {
            CProofRoot oneRoot(proofRootArr[i]);
            if (!oneRoot.IsValid())
            {
                nVersion = VERSION_INVALID;
                return;
            }
            proofRoots.insert(std::make_pair(oneRoot.systemID, oneRoot));
        }
    }

    if (nodesUni.isArray())
    {
        vector<UniValue> nodeVec = nodesUni.getValues();
        for (auto node : nodeVec)
        {
            nodes.push_back(CNodeData(uni_get_str(find_value(node, "networkaddress")), uni_get_str(find_value(node, "nodeidentity"))));
        }
    }
}

bool operator==(const CProofRoot &op1, const CProofRoot &op2)
{
    return op1.version == op2.version &&
           op1.type == op2.type &&
           op1.rootHeight == op2.rootHeight &&
           op1.stateRoot == op2.stateRoot &&
           op1.systemID == op2.systemID &&
           op1.blockHash == op2.blockHash &&
           op1.compactPower == op2.compactPower &&
           (op1.type == CProofRoot::TYPE_ETHEREUM ? op1.gasPrice == op2.gasPrice : true);
}

bool operator!=(const CProofRoot &op1, const CProofRoot &op2)
{
    return !(op1 == op2);
}

bool operator<(const CProofRoot &op1, const CProofRoot &op2)
{
    return op1.systemID == op2.systemID ?
                (op1.rootHeight < op2.rootHeight ? true : (op1.rootHeight > op2.rootHeight ? false : ::AsVector(op1) < ::AsVector(op2))) :
                (op1.systemID < op2.systemID);
}

int CPBaaSNotarization::GetBlocksPerCheckpoint(int heightChange)
{
    int blocksPerCheckpoint = std::min(heightChange, (int)MIN_BLOCKS_PER_CHECKPOINT);
    if (heightChange > (MIN_BLOCKS_PER_CHECKPOINT * MAX_PROOF_CHECKPOINTS))
    {
        blocksPerCheckpoint = (heightChange / MAX_PROOF_CHECKPOINTS) +
                                ((heightChange % MAX_PROOF_CHECKPOINTS) ? 1 : 0);
    }
    return blocksPerCheckpoint;
}

int CPBaaSNotarization::GetNumCheckpoints(int heightChange)
{
    int blocksPerCheckpoint = GetBlocksPerCheckpoint(heightChange);

    // unless we have another proof range sized chunk at the end, don't add another checkpoint
    return !blocksPerCheckpoint ? blocksPerCheckpoint :
             (heightChange / blocksPerCheckpoint + ((heightChange % blocksPerCheckpoint) >= NUM_BLOCKS_PER_PROOF_RANGE ? 0 : -1));
}

CProofRoot CProofRoot::GetProofRoot(uint32_t blockHeight)
{
    if (blockHeight > chainActive.Height())
    {
        return CProofRoot(1, VERSION_INVALID);
    }
    auto mmv = chainActive.GetMMV();
    mmv.resize(blockHeight + 1);
    return CProofRoot(ASSETCHAINS_CHAINID,
                      blockHeight,
                      mmv.GetRoot(),
                      chainActive[blockHeight]->GetBlockHash(),
                      chainActive[blockHeight]->chainPower.CompactChainPower());
}

CNotaryEvidence::CNotaryEvidence(const CTransaction &tx, int outputNum, int &afterEvidence, uint8_t EvidenceType) :
    type(EvidenceType), version(CNotaryEvidence::VERSION_INVALID)
{
    afterEvidence = outputNum;

    COptCCParams p;

    if (afterEvidence >= 0 &&
        tx.vout.size() > afterEvidence &&
        tx.vout[afterEvidence++].scriptPubKey.IsPayToCryptoCondition(p) &&
        p.IsValid() &&
        p.evalCode == EVAL_NOTARY_EVIDENCE &&
        p.vData.size() &&
        (*this = CNotaryEvidence(p.vData[0])).IsValid())
    {
        if (IsMultipartProof())
        {
            COptCCParams eP;
            CNotaryEvidence supplementalEvidence;
            std::vector<CNotaryEvidence> partsVector({*this});
            while (tx.vout.size() > afterEvidence &&
                tx.vout[afterEvidence++].scriptPubKey.IsPayToCryptoCondition(eP) &&
                eP.IsValid() &&
                eP.evalCode == EVAL_NOTARY_EVIDENCE &&
                eP.vData.size() &&
                (supplementalEvidence = CNotaryEvidence(eP.vData[0])).IsValid() &&
                supplementalEvidence.IsMultipartProof() &&
                supplementalEvidence.evidence.chainObjects.size() == 1)
            {
                partsVector.push_back(supplementalEvidence);
            }
            *this = CNotaryEvidence(partsVector);
        }
    }
}

bool CPBaaSNotarization::GetLastNotarization(const uint160 &currencyID,
                                             int32_t startHeight,
                                             int32_t endHeight,
                                             CAddressIndexDbEntry *txOutIdx,
                                             CTransaction *txOut)
{
    CPBaaSNotarization notarization;
    std::vector<CAddressIndexDbEntry> notarizationIndex;
    // get the last notarization in the indicated height for this currency, which is valid by definition for a token
    if (GetAddressIndex(CCrossChainRPCData::GetConditionID(currencyID, CPBaaSNotarization::NotaryNotarizationKey()), CScript::P2IDX, notarizationIndex, startHeight, endHeight))
    {
        // filter out all transactions that do not spend from the notarization thread, or originate as the
        // chain definition
        for (auto it = notarizationIndex.rbegin(); it != notarizationIndex.rend(); it++)
        {
            // first unspent notarization that is valid is the one we want, skip spending
            if (it->first.spending)
            {
                continue;
            }
            LOCK(mempool.cs);
            CTransaction oneTx;
            uint256 blkHash;
            if (myGetTransaction(it->first.txhash, oneTx, blkHash))
            {
                if ((notarization = CPBaaSNotarization(oneTx.vout[it->first.index].scriptPubKey)).IsValid())
                {
                    *this = notarization;
                    if (txOutIdx)
                    {
                        *txOutIdx = *it;
                    }
                    if (txOut)
                    {
                        *txOut = oneTx;
                    }
                    break;
                }
            }
            else
            {
                LogPrintf("%s: error transaction %s not found, may need reindexing\n", __func__, it->first.txhash.GetHex().c_str());
                printf("%s: error transaction %s not found, may need reindexing\n", __func__, it->first.txhash.GetHex().c_str());
                continue;
            }
        }
    }
    return notarization.IsValid();
}

bool CPBaaSNotarization::GetLastUnspentNotarization(const uint160 &currencyID,
                                                    uint256 &txIDOut,
                                                    int32_t &txOutNum,
                                                    CTransaction *txOut)
{
    CPBaaSNotarization notarization;
    std::vector<CAddressUnspentDbEntry> notarizationIndex;
    // get the last notarization in the indicated height for this currency, which is valid by definition for a token
    if (GetAddressUnspent(CCrossChainRPCData::GetConditionID(currencyID, CPBaaSNotarization::NotaryNotarizationKey()), CScript::P2IDX, notarizationIndex))
    {
        // first valid, unspent notarization found is the one we return
        for (auto it = notarizationIndex.rbegin(); it != notarizationIndex.rend(); it++)
        {
            LOCK(mempool.cs);
            CTransaction oneTx;
            uint256 blkHash;
            if (myGetTransaction(it->first.txhash, oneTx, blkHash))
            {
                if ((notarization = CPBaaSNotarization(oneTx.vout[it->first.index].scriptPubKey)).IsValid())
                {
                    *this = notarization;
                    txIDOut = it->first.txhash;
                    txOutNum = it->first.index;
                    if (txOut)
                    {
                        *txOut = oneTx;
                    }
                    break;
                }
            }
            else
            {
                LogPrintf("%s: error transaction %s not found, may need reindexing\n", __func__, it->first.txhash.GetHex().c_str());
                printf("%s: error transaction %s not found, may need reindexing\n", __func__, it->first.txhash.GetHex().c_str());
                continue;
            }
        }
    }
    return notarization.IsValid();
}

bool CPBaaSNotarization::NextNotarizationInfo(const CCurrencyDefinition &sourceSystem,
                                              const CCurrencyDefinition &destCurrency,
                                              uint32_t lastExportHeight,
                                              uint32_t notaHeight,
                                              std::vector<CReserveTransfer> &exportTransfers,       // both in and out. this may refund conversions
                                              uint256 &transferHash,
                                              CPBaaSNotarization &newNotarization,
                                              std::vector<CTxOut> &importOutputs,
                                              CCurrencyValueMap &importedCurrency,
                                              CCurrencyValueMap &gatewayDepositsUsed,
                                              CCurrencyValueMap &spentCurrencyOut,
                                              CTransferDestination feeRecipient,
                                              bool forcedRefund) const
{
    uint160 sourceSystemID = sourceSystem.GetID();
    uint160 destSystemID = destCurrency.IsGateway() ? destCurrency.gatewayID : destCurrency.systemID;

    newNotarization = *this;
    newNotarization.SetDefinitionNotarization(false);
    newNotarization.SetBlockOneNotarization(false);
    newNotarization.prevNotarization = CUTXORef();
    newNotarization.prevHeight = newNotarization.notarizationHeight;
    newNotarization.notarizationHeight = notaHeight;

    uint32_t currentHeight = chainActive.Height() + 1;

    // if we are communicating with an external system that uses a different hash, use it for everything
    CCurrencyDefinition::EHashTypes hashType = CCurrencyDefinition::EHashTypes::HASH_BLAKE2BMMR;

    uint160 externalSystemID = sourceSystem.SystemOrGatewayID() == ASSETCHAINS_CHAINID ?
                                ((destSystemID == ASSETCHAINS_CHAINID) ? uint160() : destSystemID) :
                                sourceSystem.GetID();

    CCurrencyDefinition externalSystemDef;
    if (externalSystemID.IsNull() || externalSystemID == ASSETCHAINS_CHAINID)
    {
        externalSystemDef = ConnectedChains.ThisChain();
    }
    else if (externalSystemID == sourceSystemID)
    {
        externalSystemDef = sourceSystem;
    }
    else if (externalSystemID == destSystemID)
    {
        if (destCurrency.GetID() == destSystemID)
        {
            externalSystemDef = destCurrency;
        }
        else
        {
            externalSystemDef = ConnectedChains.GetCachedCurrency(externalSystemID);
            if (!externalSystemDef.IsValid())
            {
                LogPrintf("%s: cannot retrieve system definition for %s\n", __func__, EncodeDestination(CIdentityID(externalSystemID)));
                return false;
            }
        }
    }

    CTransferDestination notaryPayee;

    if (!externalSystemID.IsNull())
    {
        hashType = (CCurrencyDefinition::EHashTypes)externalSystemDef.proofProtocol;
    }

    CNativeHashWriter hwPrevNotarization;
    hwPrevNotarization << *this;
    newNotarization.hashPrevCrossNotarization = hwPrevNotarization.GetHash();

    CNativeHashWriter hw(hashType);

    CCurrencyValueMap newPreConversionReservesIn;

    if (!IsPreLaunch() && !IsLaunchComplete())
    {
        newPreConversionReservesIn = CCurrencyValueMap(currencyState.currencies, currencyState.primaryCurrencyIn);
    }

    for (int i = 0; i < exportTransfers.size(); i++)
    {
        CReserveTransfer reserveTransfer = exportTransfers[i];

        //auto transferVec = ::AsVector(reserveTransfer);
        //printf("%s: ReserveTransfer:\n%s\nSerialized:\n%s\n", __func__, reserveTransfer.ToUniValue().write(1,2).c_str(), HexBytes(&(transferVec[0]), transferVec.size()).c_str());

        // add the pre-mutation reserve transfer to the hash
        hw << reserveTransfer;

        if (currencyState.IsRefunding())
        {
            reserveTransfer = reserveTransfer.GetRefundTransfer();
        }
        // ensure that any pre-conversions or conversions are all valid, based on mined height and
        // maximum pre-conversions
        else if (reserveTransfer.IsPreConversion())
        {
            if (IsLaunchComplete())
            {
                //printf("%s: Invalid pre-conversion, mined on or after start block\n", __func__);
                LogPrintf("%s: Invalid pre-conversion, mined on or after start block\n", __func__);
                reserveTransfer = reserveTransfer.GetRefundTransfer();
            }
            else
            {
                // check if it exceeds pre-conversion maximums, and refund if so
                CCurrencyValueMap newReserveIn = CCurrencyValueMap(std::vector<uint160>({reserveTransfer.FirstCurrency()}),
                                                                   std::vector<int64_t>({reserveTransfer.FirstValue() - CReserveTransactionDescriptor::CalculateConversionFee(reserveTransfer.FirstValue())}));
                CCurrencyValueMap newTotalReserves;
                if (IsPreLaunch())
                {
                    newTotalReserves = CCurrencyValueMap(destCurrency.currencies, newNotarization.currencyState.reserves) + newReserveIn + newPreConversionReservesIn;
                }
                else
                {
                    newTotalReserves = CCurrencyValueMap(destCurrency.currencies, newNotarization.currencyState.primaryCurrencyIn) + newReserveIn + newPreConversionReservesIn;
                }

                if (destCurrency.maxPreconvert.size() && newTotalReserves > CCurrencyValueMap(destCurrency.currencies, destCurrency.maxPreconvert))
                {
                    LogPrintf("%s: refunding pre-conversion over maximum\n", __func__);
                    reserveTransfer = reserveTransfer.GetRefundTransfer();
                }
                else
                {
                    newPreConversionReservesIn += newReserveIn;
                }
            }
        }
        else if (reserveTransfer.IsConversion())
        {
            if (!IsLaunchComplete())
            {
                //printf("%s: Invalid conversion, mined before start block\n", __func__);
                LogPrintf("%s: Invalid conversion, mined before start block\n", __func__);
                reserveTransfer = reserveTransfer.GetRefundTransfer();
            }
        }
    }

    if (exportTransfers.size())
    {
        transferHash = hw.GetHash();
    }

    CReserveTransactionDescriptor rtxd;
    std::vector<CTxOut> dummyImportOutputs;
    bool thisIsLaunchSys = destCurrency.launchSystemID == ASSETCHAINS_CHAINID;

    // if this is the clear launch notarization after start, make the notarization and determine if we should launch or refund
    uint256 weakEntropy = EntropyHashFromHeight(CBlockIndex::BlockEntropyKey(), notaHeight, newNotarization.currencyID);

    if (destCurrency.launchSystemID == sourceSystemID &&
        destCurrency.startBlock &&
        ((thisIsLaunchSys && notaHeight <= (destCurrency.startBlock - 1)) ||
         (!thisIsLaunchSys &&
          destCurrency.systemID == ASSETCHAINS_CHAINID &&
          notaHeight == 1)))
    {
        // we get one pre-launch coming through here, initial supply is set and ready for pre-convert
        if (((thisIsLaunchSys && notaHeight == (destCurrency.startBlock - 1)) || sourceSystemID != ASSETCHAINS_CHAINID) &&
            newNotarization.IsPreLaunch())
        {
            // the first block executes the second time through
            if (newNotarization.IsLaunchCleared())
            {
                newNotarization.SetPreLaunch(false);
                newNotarization.currencyState.SetLaunchClear();
                newNotarization.currencyState.SetPrelaunch(false);
                newNotarization.currencyState.RevertReservesAndSupply();
            }
            else
            {
                newNotarization.SetLaunchCleared();
                newNotarization.currencyState.SetLaunchClear();

                // if we are connected to another currency, make sure it will also start before we confirm that we can
                CCurrencyDefinition coLaunchCurrency;
                CCoinbaseCurrencyState coLaunchState;
                bool coLaunching = false;
                if (destCurrency.IsGatewayConverter())
                {
                    // PBaaS or gateway converters have a parent which is the PBaaS chain or gateway
                    coLaunching = true;
                    coLaunchCurrency = ConnectedChains.GetCachedCurrency(destCurrency.parent);
                }
                else if (destCurrency.IsPBaaSChain() && !destCurrency.GatewayConverterID().IsNull())
                {
                    coLaunching = true;
                    coLaunchCurrency = ConnectedChains.GetCachedCurrency(destCurrency.GatewayConverterID());
                }

                if (coLaunching)
                {
                    if (!coLaunchCurrency.IsValid())
                    {
                        printf("%s: Invalid co-launch currency - likely corruption\n", __func__);
                        LogPrintf("%s: Invalid co-launch currency - likely corruption\n", __func__);
                        return false;
                    }
                    coLaunchState = ConnectedChains.GetCurrencyState(coLaunchCurrency, notaHeight, true);

                    if (!coLaunchState.IsValid())
                    {
                        printf("%s: Invalid co-launch currency state - likely corruption\n", __func__);
                        LogPrintf("%s: Invalid co-launch currency state - likely corruption\n", __func__);
                        return false;
                    }

                    // check our currency and any co-launch currency to determine our eligibility, as ALL
                    // co-launch currencies must launch for one to launch
                    if (coLaunchState.IsRefunding() ||
                        !coLaunchState.ValidateConversionLimits() ||
                        CCurrencyValueMap(coLaunchCurrency.currencies, coLaunchState.reserveIn) < CCurrencyValueMap(coLaunchCurrency.currencies, coLaunchCurrency.minPreconvert) ||
                        (coLaunchCurrency.IsFractional() &&
                         CCurrencyValueMap(coLaunchCurrency.currencies, coLaunchState.reserveIn).CanonicalMap().valueMap.size() != coLaunchCurrency.currencies.size()))
                    {
                        forcedRefund = true;
                    }
                }

                // first time through is export, second is import, then we finish clearing the launch
                // check if the chain is qualified to launch or should refund
                CCurrencyValueMap minPreMap, fees;
                CCurrencyValueMap preConvertedMap = (CCurrencyValueMap(destCurrency.currencies, newNotarization.currencyState.reserveIn) +
                                                     newPreConversionReservesIn).CanonicalMap();

                if (destCurrency.minPreconvert.size() && destCurrency.minPreconvert.size() == destCurrency.currencies.size())
                {
                    minPreMap = CCurrencyValueMap(destCurrency.currencies, destCurrency.minPreconvert).CanonicalMap();
                }

                if (forcedRefund ||
                    (minPreMap.valueMap.size() && preConvertedMap < minPreMap) ||
                    (destCurrency.IsFractional() &&
                     (CCurrencyValueMap(destCurrency.currencies, newNotarization.currencyState.reserveIn) +
                                        newPreConversionReservesIn).CanonicalMap().valueMap.size() != destCurrency.currencies.size()))
                {
                    // we force the reserves and supply to zero
                    // in any case where there was less than minimum participation,
                    newNotarization.currencyState.supply = 0;
                    newNotarization.currencyState.reserves = std::vector<int64_t>(newNotarization.currencyState.reserves.size(), 0);
                    newNotarization.currencyState.SetRefunding(true);
                    newNotarization.SetRefunding(true);
                }
                else
                {
                    newNotarization.SetLaunchConfirmed();
                    newNotarization.currencyState.SetLaunchConfirmed();

                    // if this is a launch notarization for a PBaaS chain, add a proofroot of the
                    // current chain as an anchor for the block one import
                    if (destCurrency.IsPBaaSChain() &&
                        destCurrency.launchSystemID == ASSETCHAINS_CHAINID)
                    {
                        CProofRoot curProofRoot = CProofRoot::GetProofRoot(destCurrency.startBlock - 1);
                        if (!curProofRoot.IsValid())
                        {
                            return false;
                        }
                        newNotarization.proofRoots[ASSETCHAINS_CHAINID] = curProofRoot;
                    }
                }
            }
        }
        else if (thisIsLaunchSys && notaHeight < (destCurrency.startBlock - 1))
        {
            newNotarization.currencyState.SetPrelaunch();
        }

        // NOTE: destcurrency systemID is correct here, since this is only prelaunch or block 1, which doesn't apply for gateway's
        CCurrencyDefinition destSystem = newNotarization.IsRefunding() ? ConnectedChains.GetCachedCurrency(destCurrency.launchSystemID) :
                                                                         ConnectedChains.GetCachedCurrency(destCurrency.systemID);

        CCoinbaseCurrencyState tempState = newNotarization.currencyState;
        if (destCurrency.IsFractional() &&
            !(newNotarization.IsLaunchCleared() && !newNotarization.currencyState.IsLaunchCompleteMarker()) &&
            exportTransfers.size())
        {
            // normalize prices on the way in to prevent overflows on first pass
            std::vector<int64_t> newReservesVector = newPreConversionReservesIn.AsCurrencyVector(tempState.currencies);
            tempState.reserves = tempState.AddVectors(tempState.reserves, newReservesVector);
            newNotarization.currencyState.conversionPrice = tempState.PricesInReserve();
        }

        std::vector<CTxOut> tempOutputs;
        bool retVal = rtxd.AddReserveTransferImportOutputs(newNotarization.IsRefunding() ? destSystem : sourceSystem,
                                                           newNotarization.IsRefunding() ? sourceSystem : destSystem,
                                                           destCurrency,
                                                           newNotarization.currencyState,
                                                           exportTransfers,
                                                           currentHeight,
                                                           tempOutputs,
                                                           importedCurrency,
                                                           gatewayDepositsUsed,
                                                           spentCurrencyOut,
                                                           &tempState,
                                                           feeRecipient,
                                                           proposer,
                                                           weakEntropy);

        if (retVal)
        {
            //printf("%s: importedCurrency: %s\ngatewaysDepositsUsed: %s\n", __func__, importedCurrency.ToUniValue().write(1,2).c_str(), gatewayDepositsUsed.ToUniValue().write(1,2).c_str());
            importedCurrency.valueMap.clear();
            gatewayDepositsUsed.valueMap.clear();
            spentCurrencyOut.valueMap.clear();
            newNotarization.currencyState.conversionPrice = tempState.conversionPrice;
            newNotarization.currencyState.viaConversionPrice = tempState.viaConversionPrice;
            rtxd = CReserveTransactionDescriptor();

            retVal = rtxd.AddReserveTransferImportOutputs(newNotarization.IsRefunding() ? destSystem : sourceSystem,
                                                          newNotarization.IsRefunding() ? sourceSystem : destSystem,
                                                          destCurrency,
                                                          newNotarization.currencyState,
                                                          exportTransfers,
                                                          currentHeight,
                                                          importOutputs,
                                                          importedCurrency,
                                                          gatewayDepositsUsed,
                                                          spentCurrencyOut,
                                                          &tempState,
                                                          feeRecipient,
                                                          proposer,
                                                          weakEntropy,
                                                          true);
        }
        else
        {
            return retVal;
        }

        //printf("%s: importedCurrency: %s\ngatewaysDepositsUsed: %s\n", __func__, importedCurrency.ToUniValue().write(1,2).c_str(), gatewayDepositsUsed.ToUniValue().write(1,2).c_str());

        // if we are in the pre-launch phase, all reserves in are cumulative and then calculated together at launch
        // reserves in represent all reserves in, and fees are taken out after launch or refund as well
        //
        // we add up until the end, then stop adding reserves at launch clear. this gives us the ability to reverse the
        // pre-launch state for validation. we continue adding fees up to pre-launch to easily get a total of unconverted
        // fees, which we need when creating a PBaaS chain, as all currency, both reserves and fees exported to the new
        // chain must be either output to specific addresses, taken as fees by miners, or stored in reserve deposits.
        if (tempState.IsPrelaunch())
        {
            tempState.reserveIn = tempState.AddVectors(tempState.reserveIn, this->currencyState.reserveIn);
            if (!destCurrency.IsFractional() && !this->IsDefinitionNotarization() && !tempState.IsLaunchClear())
            {
                tempState.supply += this->currencyState.supply - this->currencyState.emitted;
                tempState.preConvertedOut += this->currencyState.preConvertedOut;
                tempState.primaryCurrencyOut += this->currencyState.primaryCurrencyOut;
            }
        }
        else if (!newNotarization.currencyState.IsLaunchCompleteMarker() && !tempState.IsRefunding() && !this->currencyState.IsPrelaunch())
        {
            // accumulate reserves during pre-conversions import to enforce max pre-convert
            CCurrencyValueMap tempReserves;
            for (auto &oneCurrencyID : tempState.currencies)
            {
                if (rtxd.currencies.count(oneCurrencyID))
                {
                    int64_t reservesConverted = rtxd.currencies[oneCurrencyID].nativeOutConverted;
                    if (reservesConverted)
                    {
                        tempReserves.valueMap[oneCurrencyID] = reservesConverted;
                    }
                }
            }

            // use double entry to enable pass through of the accumulated reserve such that when
            // reverting supply and reserves, we end up with what we started, after prelaunch and
            // before all post launch functions are complete, we use primaryCurrencyIn to accumulate
            // reserves to enforce maxPreconvert
            tempState.primaryCurrencyIn = tempState.AddVectors(this->currencyState.primaryCurrencyIn, tempReserves.AsCurrencyVector(tempState.currencies));
            tempState.reserveOut =
                    tempState.AddVectors(tempState.reserveOut,
                                         (CCurrencyValueMap(tempState.currencies, tempState.reserveIn) * -1).AsCurrencyVector(tempState.currencies));
            tempState.reserveIn = tempReserves.AsCurrencyVector(tempState.currencies);
        }
        if (destCurrency.IsPBaaSChain() && tempState.IsLaunchClear() && this->currencyState.IsLaunchClear())
        {
            tempState.reserveIn = this->currencyState.reserveIn;
        }
        newNotarization.currencyState = tempState;
        return retVal;
    }
    else
    {
        if (sourceSystemID != destCurrency.launchSystemID || lastExportHeight >= destCurrency.startBlock || newNotarization.IsLaunchComplete())
        {
            newNotarization.currencyState.SetLaunchCompleteMarker();
        }
        newNotarization.currencyState.SetLaunchClear(false);

        CCurrencyDefinition destSystem = newNotarization.IsRefunding() ? ConnectedChains.GetCachedCurrency(destCurrency.launchSystemID) :
                                                                         ConnectedChains.GetCachedCurrency(destCurrency.SystemOrGatewayID());

        // calculate new state from processing all transfers
        // we are not refunding, and it is possible that we also have
        // normal conversions in addition to pre-conversions. add any conversions that may
        // be present into the new currency state
        CCoinbaseCurrencyState intermediateState = newNotarization.currencyState;
        bool isValidExport = rtxd.AddReserveTransferImportOutputs(sourceSystem,
                                                                  destSystem,
                                                                  destCurrency,
                                                                  intermediateState,
                                                                  exportTransfers,
                                                                  currentHeight,
                                                                  dummyImportOutputs,
                                                                  importedCurrency,
                                                                  gatewayDepositsUsed,
                                                                  spentCurrencyOut,
                                                                  &newNotarization.currencyState,
                                                                  feeRecipient,
                                                                  proposer,
                                                                  weakEntropy);
        if (!newNotarization.currencyState.IsPrelaunch() &&
            isValidExport &&
            destCurrency.IsFractional())
        {
            // we want the new price and the old state as a starting point to ensure no rounding error impact
            // on reserves
            importedCurrency = CCurrencyValueMap();
            gatewayDepositsUsed = CCurrencyValueMap();
            CCoinbaseCurrencyState tempCurState = intermediateState;
            tempCurState.conversionPrice = newNotarization.currencyState.conversionPrice;
            tempCurState.viaConversionPrice = newNotarization.currencyState.viaConversionPrice;
            rtxd = CReserveTransactionDescriptor();
            isValidExport = rtxd.AddReserveTransferImportOutputs(sourceSystem,
                                                                 destSystem,
                                                                 destCurrency,
                                                                 tempCurState,
                                                                 exportTransfers,
                                                                 currentHeight,
                                                                 importOutputs,
                                                                 importedCurrency,
                                                                 gatewayDepositsUsed,
                                                                 spentCurrencyOut,
                                                                 &newNotarization.currencyState,
                                                                 feeRecipient,
                                                                 proposer,
                                                                 weakEntropy,
                                                                 true);
            if (isValidExport)
            {
                newNotarization.currencyState.conversionPrice = tempCurState.conversionPrice;
                newNotarization.currencyState.viaConversionPrice = tempCurState.viaConversionPrice;
            }
        }
        else if (isValidExport)
        {
            importOutputs.insert(importOutputs.end(), dummyImportOutputs.begin(), dummyImportOutputs.end());
        }

        if (!isValidExport)
        {
            LogPrintf("%s: invalid export\n", __func__);
            return false;
        }
        return true;
    }

    // based on the last notarization and existing
    return false;
}

CObjectFinalization::CObjectFinalization(const CScript &script) : version(VERSION_INVALID)
{
    COptCCParams p;
    if (script.IsPayToCryptoCondition(p) && p.IsValid())
    {
        if ((p.evalCode == EVAL_FINALIZE_NOTARIZATION || p.evalCode == EVAL_FINALIZE_EXPORT) &&
            p.vData.size())
        {
            *this = CObjectFinalization(p.vData[0]);
        }
    }
}

CObjectFinalization::CObjectFinalization(const CTransaction &tx, uint32_t *pEcode, int32_t *pFinalizationOutNum, uint32_t minFinalHeight, uint8_t Version) :
    minFinalizationHeight(minFinalHeight),
    version(Version)
{
    uint32_t _ecode;
    uint32_t &ecode = pEcode ? *pEcode : _ecode;
    int32_t _finalizeOutNum;
    int32_t &finalizeOutNum = pFinalizationOutNum ? *pFinalizationOutNum : _finalizeOutNum;
    finalizeOutNum = -1;
    for (int i = 0; i < tx.vout.size(); i++)
    {
        COptCCParams p;
        if (tx.vout[i].scriptPubKey.IsPayToCryptoCondition(p) && p.IsValid())
        {
            if (p.evalCode == EVAL_FINALIZE_NOTARIZATION || p.evalCode == EVAL_FINALIZE_EXPORT)
            {
                if (finalizeOutNum != -1)
                {
                    version = VERSION_INVALID;
                    finalizeOutNum = -1;
                    break;
                }
                else
                {
                    finalizeOutNum = i;
                    ecode = p.evalCode;
                }
            }
        }
    }
}

CChainNotarizationData::CChainNotarizationData(UniValue &obj,
                                               bool loadNotarizations,
                                               std::vector<uint256> *pBlockHash,
                                               std::vector<std::vector<std::tuple<CNotaryEvidence, CProofRoot, CProofRoot>>> *pCounterEvidence)
{
    version = (uint32_t)uni_get_int(find_value(obj, "version"));
    UniValue vtxUni = find_value(obj, "notarizations");
    if (pBlockHash)
    {
        pBlockHash->clear();
    }
    if (pCounterEvidence)
    {
        pCounterEvidence->clear();
    }
    if (vtxUni.isArray())
    {
        for (int i = 0; i < vtxUni.size(); i++)
        {
            auto &o = vtxUni[i];

            UniValue notarizationUni = find_value(o, "notarization");
            if (loadNotarizations && notarizationUni.isNull())
            {
                uint256 txid;
                txid.SetHex(uni_get_str(find_value(o, "txid")));
                CUTXORef utxo(txid, uni_get_int(find_value(o, "vout"), -1));
                if (utxo.IsValid())
                {
                    uint256 blockHash;
                    CTransaction tx;
                    COptCCParams p;
                    CPBaaSNotarization pbn;
                    if (utxo.GetOutputTransaction(tx, blockHash) &&
                        tx.vout.size() > utxo.n &&
                        tx.vout[utxo.n].scriptPubKey.IsPayToCryptoCondition(p) &&
                        (p.evalCode == EVAL_EARNEDNOTARIZATION || p.evalCode == EVAL_ACCEPTEDNOTARIZATION) &&
                        p.vData.size() &&
                        (pbn = CPBaaSNotarization(p.vData[0])).IsValid())
                    {
                        notarizationUni = pbn.ToUniValue();
                    }
                    else
                    {
                        version = 0;
                        return;
                    }
                }
                else
                {
                    version = 0;
                    return;
                }
            }

            vtx.push_back(make_pair(CUTXORef(uint256S(uni_get_str(find_value(o, "txid"))),
                                             uni_get_int(find_value(o, "vout"))),
                                    CPBaaSNotarization(notarizationUni)));
            if (pBlockHash)
            {
                uint256 blockHash;
                blockHash.SetHex(uni_get_str(find_value(o, "blockhash")));
                pBlockHash->push_back(blockHash);
            }
            if (pCounterEvidence)
            {
                pCounterEvidence->resize(pCounterEvidence->size() + 1);
                UniValue counterEvidenceUni = find_value(o, "counterevidence");
                if (counterEvidenceUni.isArray() && counterEvidenceUni.size())
                {
                    for (int j = 0; j < counterEvidenceUni.size(); j++)
                    {
                        if (counterEvidenceUni[j].isObject())
                        {
                            std::tuple<CNotaryEvidence, CProofRoot, CProofRoot>
                                oneCounterEvidenceItem(
                                    {
                                        CNotaryEvidence(find_value(counterEvidenceUni[j], "counterproof")),
                                        CProofRoot(find_value(counterEvidenceUni[j], "counterroot")),
                                        CProofRoot(find_value(counterEvidenceUni[j], "challengestart"))
                                    }
                                );

                            if (!std::get<0>(oneCounterEvidenceItem).IsValid() ||
                                !std::get<1>(oneCounterEvidenceItem).IsValid() ||
                                !std::get<2>(oneCounterEvidenceItem).IsValid())
                            {
                                continue;
                            }
                            (*pCounterEvidence)[j].push_back(oneCounterEvidenceItem);
                        }
                    }
                }
            }
        }
    }

    lastConfirmed = (uint32_t)uni_get_int(find_value(obj, "lastconfirmed"));
    UniValue forksUni = find_value(obj, "forks");
    if (forksUni.isArray())
    {
        vector<UniValue> forksVec = forksUni.getValues();
        for (auto fv : forksVec)
        {
            if (fv.isArray() && fv.size())
            {
                forks.push_back(vector<int32_t>());
                for (auto fidx : fv.getValues())
                {
                    forks.back().push_back(uni_get_int(fidx));
                }
            }
        }
    }

    bestChain = (uint32_t)uni_get_int(find_value(obj, "bestchain"));
}

UniValue CChainNotarizationData::ToUniValue(const std::vector<std::pair<CTransaction, uint256>> &transactionsAndBlockHash,
                                            const std::vector<std::vector<std::tuple<CObjectFinalization, CNotaryEvidence, CProofRoot, CProofRoot>>> &counterEvidence) const
{
    UniValue obj(UniValue::VOBJ);
    obj.push_back(Pair("version", (int32_t)version));
    UniValue notarizations(UniValue::VARR);
    for (int64_t i = 0; i < vtx.size(); i++)
    {
        UniValue notarization(UniValue::VOBJ);
        notarization.push_back(Pair("index", i));
        notarization.push_back(Pair("txid", vtx[i].first.hash.GetHex()));
        if (i < transactionsAndBlockHash.size())
        {
            notarization.push_back(Pair("blockhash", transactionsAndBlockHash[i].second.GetHex()));
        }
        notarization.push_back(Pair("vout", (int32_t)vtx[i].first.n));
        notarization.push_back(Pair("notarization", vtx[i].second.ToUniValue()));
        if (i < counterEvidence.size())
        {
            UniValue counterEvidenceUni(UniValue::VARR);
            for (auto &oneCounterItem : counterEvidence[i])
            {
                UniValue counterEvidenceElement(UniValue::VOBJ);
                counterEvidenceElement.pushKV("finalization", std::get<0>(oneCounterItem).ToUniValue());
                counterEvidenceElement.pushKV("counterproof", std::get<1>(oneCounterItem).ToUniValue());
                counterEvidenceElement.pushKV("counterroot", std::get<2>(oneCounterItem).ToUniValue());
                counterEvidenceElement.pushKV("challengestart", std::get<3>(oneCounterItem).ToUniValue());
                counterEvidenceUni.push_back(counterEvidenceElement);
            }
            if (counterEvidenceUni.size())
            {
                notarization.pushKV("counterevidence", counterEvidenceUni);
            }
        }
        notarizations.push_back(notarization);
    }
    obj.push_back(Pair("notarizations", notarizations));
    UniValue Forks(UniValue::VARR);
    for (int32_t i = 0; i < forks.size(); i++)
    {
        UniValue Fork(UniValue::VARR);
        for (int32_t j = 0; j < forks[i].size(); j++)
        {
            Fork.push_back(forks[i][j]);
        }
        Forks.push_back(Fork);
    }
    obj.push_back(Pair("forks", Forks));
    if (IsConfirmed())
    {
        obj.push_back(Pair("lastconfirmedheight", (int32_t)vtx[lastConfirmed].second.notarizationHeight));
    }
    obj.push_back(Pair("lastconfirmed", lastConfirmed));
    obj.push_back(Pair("bestchain", bestChain));
    return obj;
}

bool CChainNotarizationData::CalculateConfirmation(int confirmingIdx, std::set<int> &confirmedOutputNums, std::set<int> &invalidatedOutputNums) const
{
    int forkIdx = -1, forkPos = -1;

    if (confirmingIdx != lastConfirmed)
    {
        // if the third notarization does not equal the last confirmed notarization, it
        // must equal a last pending notarization to confirm. that will always be the
        // first non-confirmed entry in a fork of the notarization data, if it is present
        for (int i = 0; i < forks.size(); i++)
        {
            if (forks[i].size() > 1)
            {
                for (int j = forks[i].size() - 1; j >= 0; j--)
                {
                    if (forks[i][j] == confirmingIdx)
                    {
                        forkIdx = i;
                        forkPos = j;
                        break;
                    }
                }
                if (forkIdx > -1)
                {
                    break;
                }
            }
        }
        // if it wasn't the confirmed entry and wasn't pending confirmation, it is referring to an
        // invalid predecessor and therefore, confirmingIdx is invalid
        if (forkIdx == -1)
        {
            LogPrint("notarization", "%s: invalid prior notarization - neither pending nor confirmed\n", __func__);
            return false;
        }
    }

    if (forkIdx > -1)
    {
        // if this finalization is either confirmed or invalidated by the new accepted notarization,
        // add a spend here
        for (int i = 0; i <= forkPos; i++)
        {
            confirmedOutputNums.insert(forks[forkIdx][i]);
        }

        for (int i = 0; i < forks.size(); i++)
        {
            // already did the fork that is our focus
            if (i == forkIdx)
            {
                continue;
            }
            bool failed = false;

            int j;
            for (j = 0; j < forks[i].size(); j++)
            {
                // if we reach the end, the rest after are ignored
                if (forks[i][j] == confirmingIdx)
                {
                    break;
                }
                if (confirmedOutputNums.count(forks[i][j]))
                {
                    continue;
                }
                // it did not derive from the newly confirmed index, so it is invalid and all after this
                failed = true;
                break;
            }
            // if failed, invalidate all until the end, otherwise, ignore all until the end
            if (failed)
            {
                for (int k = j; k < forks[i].size(); k++)
                {
                    invalidatedOutputNums.insert(forks[i][k]);
                }
            }
        }
    }
    return true;
}

bool CChainNotarizationData::CorrelatedFinalizationSpends(const std::vector<std::pair<CTransaction, uint256>> &txes,
                                                          std::vector<std::vector<CInputDescriptor>> &spendsToClose,
                                                          std::vector<CInputDescriptor> &extraSpends,
                                                          std::vector<std::vector<CNotaryEvidence>> *pEvidenceVec) const
{
    if (!(IsValid() && IsConfirmed()))
    {
        return false;
    }
    uint160 currencyID = vtx[0].second.currencyID;

    if (pEvidenceVec)
    {
        *pEvidenceVec = std::vector<std::vector<CNotaryEvidence>>(vtx.size());
    }

    std::map<CUTXORef, int> notarizationOutputMap;

    for (int i = 0; i < vtx.size(); i++)
    {
        notarizationOutputMap.insert(std::make_pair(vtx[i].first, i));
    }

    uint32_t confirmedBlockHeight = mapBlockIndex.count(txes[lastConfirmed].second) ? mapBlockIndex[txes[lastConfirmed].second]->GetHeight() : 0;
    if (!confirmedBlockHeight)
    {
        LogPrint("notarization", "%s: last confirmed notarization not found in block index\n", __func__);
        return false;
    }

    spendsToClose = std::vector<std::vector<CInputDescriptor>>(vtx.size());

    for (auto onePending : CObjectFinalization::GetUnspentPendingFinalizations(currencyID))
    {
        // determine the notarization output that this is referring to
        COptCCParams p;
        CObjectFinalization existingFinalization;
        CUTXORef pendingNotarizationOutput;
        std::set<CUTXORef> evidenceOutputSet;
        if (onePending.second.scriptPubKey.IsPayToCryptoCondition(p) &&
            p.IsValid() &&
            p.vData.size())
        {
            if (p.evalCode == EVAL_FINALIZE_NOTARIZATION &&
                (existingFinalization = CObjectFinalization(p.vData[0])).IsValid())
            {
                if (existingFinalization.output.IsOnSameTransaction())
                {
                    pendingNotarizationOutput = CUTXORef(onePending.second.txIn.prevout.hash, existingFinalization.output.n);
                }
                else
                {
                    pendingNotarizationOutput = existingFinalization.output;
                }
            }
            else if (p.evalCode == EVAL_EARNEDNOTARIZATION)
            {
                pendingNotarizationOutput = CUTXORef(onePending.second.txIn.prevout.hash, onePending.second.txIn.prevout.n);
            }
        }
        else
        {
            // this is likely some form of incorrect index
            LogPrintf("%s: LIKELY LOCAL STATE OR INDEX CORRUPTION. Bootstrap, reindex, or resync recommended\n", __func__);
            continue;
        }

        // if this is a finalization with some evidence, add the spends to close it that include the finalization and
        // its evidence outputs
        int associatedIdx = -1;
        if (pendingNotarizationOutput.IsValid() && !pendingNotarizationOutput.hash.IsNull())
        {
            std::vector<CInputDescriptor> associatedSpends;
            associatedSpends.push_back(onePending.second);      // this is a finalization or earned notarization, so it is first associated spend

            if (notarizationOutputMap.count(pendingNotarizationOutput))
            {
                associatedIdx = notarizationOutputMap[pendingNotarizationOutput];
            }

            // if we are asssociated with a known node,
            // get additional associated evidence as well
            if (associatedIdx != -1)
            {
                // if there is a finalization, we need to add it and its evidence,
                if (existingFinalization.IsValid() && existingFinalization.evidenceOutputs.size())
                {
                    CTransaction finalizationTx;
                    const CTransaction *pEvidenceOutputTx = nullptr;
                    if (existingFinalization.output.IsOnSameTransaction())
                    {
                        pEvidenceOutputTx = &(txes[associatedIdx].first);
                    }
                    else
                    {
                        LOCK(mempool.cs);
                        uint256 hashBlock;
                        if (myGetTransaction(onePending.second.txIn.prevout.hash, finalizationTx, hashBlock))
                        {
                            pEvidenceOutputTx = &finalizationTx;
                        }
                        else
                        {
                            LogPrint("notarization", "%s: cannot access transaction required as input for notarization\n", __func__);
                            return false;
                        }
                    }

                    // add evidence outs as spends to close this entry as well
                    int afterMultiPart = existingFinalization.evidenceOutputs.size() ? existingFinalization.evidenceOutputs[0] : 0;
                    for (auto oneEvidenceOutN : existingFinalization.evidenceOutputs)
                    {
                        if (pEvidenceOutputTx->vout.size() <= oneEvidenceOutN)
                        {
                            LogPrint("notarization", "%s: indexing error for notarization evidence\n", __func__);
                            return false;
                        }

                        if (pEvidenceVec &&
                            oneEvidenceOutN >= afterMultiPart &&
                            associatedIdx != -1)
                        {
                            CNotaryEvidence oneEvidenceObj(*pEvidenceOutputTx, oneEvidenceOutN, afterMultiPart);
                            if (oneEvidenceObj.IsValid())
                            {
                                (*pEvidenceVec)[associatedIdx].push_back(oneEvidenceObj);
                            }
                        }

                        associatedSpends.push_back(
                            CInputDescriptor(pEvidenceOutputTx->vout[oneEvidenceOutN].scriptPubKey,
                                                pEvidenceOutputTx->vout[oneEvidenceOutN].nValue,
                                                CTxIn(pEvidenceOutputTx->GetHash(), oneEvidenceOutN)));
                        evidenceOutputSet.insert(associatedSpends.back().txIn.prevout);
                    }
                }

                // unspent evidence is specific to the target notarization
                std::vector<std::pair<uint32_t, CInputDescriptor>> unspentEvidence =
                    CObjectFinalization::GetUnspentEvidence(currencyID, vtx[associatedIdx].first.hash, vtx[associatedIdx].first.n);
                for (auto &oneEvidenceSpend : unspentEvidence)
                {
                    // if not already added to our spends as evidence, add it
                    if (!evidenceOutputSet.count(oneEvidenceSpend.second.txIn.prevout))
                    {
                        COptCCParams tP;
                        CNotaryEvidence oneEvidenceObj;
                        if (pEvidenceVec &&
                            oneEvidenceSpend.second.scriptPubKey.IsPayToCryptoCondition(tP) &&
                            tP.IsValid() &&
                            tP.vData.size() &&
                            tP.evalCode == EVAL_NOTARY_EVIDENCE &&
                            (oneEvidenceObj = CNotaryEvidence(tP.vData[0])).IsValid())
                        {
                            (*pEvidenceVec)[associatedIdx].push_back(oneEvidenceObj);
                        }

                        associatedSpends.push_back(oneEvidenceSpend.second);
                    }
                }

                spendsToClose[associatedIdx].insert(spendsToClose[associatedIdx].end(), associatedSpends.begin(), associatedSpends.end());
            }
            else
            {
                // it may be a straggler that came in before confirmation. if so, just clean it up
                if (onePending.first)
                {
                    // add it to unknown closing spends
                    extraSpends.insert(extraSpends.end(), associatedSpends.begin(), associatedSpends.end());
                }
                else
                {
                    LogPrint("notarization", "%s: no associated notarization located for pending finalization in mempool:\n%s\n", __func__, pendingNotarizationOutput.ToString().c_str());
                }
            }
        }
    }

    for (auto oneConfirmed : CObjectFinalization::GetUnspentConfirmedFinalizations(currencyID))
    {
        // since we are creating a new, confirmed finalization, spend old one here
        // determine the notarization output that this is referring to
        COptCCParams p;
        CObjectFinalization confirmedFinalization;
        CUTXORef confirmedNotarizationOutput;
        std::set<CUTXORef> evidenceOutputSet;

        if (oneConfirmed.second.scriptPubKey.IsPayToCryptoCondition(p) &&
            p.IsValid())
        {
            CPBaaSNotarization confirmedNotarization;
            if (p.evalCode == EVAL_FINALIZE_NOTARIZATION &&
                p.vData.size() &&
                (confirmedFinalization = CObjectFinalization(p.vData[0])).IsValid())
            {
                if (confirmedFinalization.output.IsOnSameTransaction())
                {
                    confirmedNotarizationOutput = CUTXORef(oneConfirmed.second.txIn.prevout.hash, confirmedFinalization.output.n);
                }
                else
                {
                    confirmedNotarizationOutput = confirmedFinalization.output;
                }
            }
            else if ((p.evalCode == EVAL_EARNEDNOTARIZATION || p.evalCode == EVAL_ACCEPTEDNOTARIZATION) &&
                     p.vData.size() &&
                     (confirmedNotarization = CPBaaSNotarization(p.vData[0])).IsValid() &&
                     (confirmedNotarization.IsDefinitionNotarization() || confirmedNotarization.IsBlockOneNotarization()))
            {
                confirmedFinalization = CObjectFinalization(CObjectFinalization::FINALIZE_CONFIRMED + CObjectFinalization::FINALIZE_NOTARIZATION,
                                                            confirmedNotarization.currencyID,
                                                            oneConfirmed.second.txIn.prevout.hash,
                                                            oneConfirmed.second.txIn.prevout.n);
                confirmedNotarizationOutput = confirmedFinalization.output;
            }
        }
        else
        {
            // this is likely some form of incorrect index
            LogPrintf("%s: LIKELY LOCAL STATE OR INDEX CORRUPTION. Bootstrap, reindex, or resync recommended\n", __func__);
            continue;
        }

        // if this is a finalization, add the spends to close it that include the finalization and
        // its evidence outputs
        int associatedIdx = -1;
        if (confirmedFinalization.IsValid())
        {
            std::vector<CInputDescriptor> associatedSpends;
            associatedSpends.push_back(oneConfirmed.second);

            if (notarizationOutputMap.count(confirmedNotarizationOutput))
            {
                associatedIdx = notarizationOutputMap[confirmedNotarizationOutput];
            }

            if (associatedIdx != -1)
            {
                if (confirmedFinalization.evidenceOutputs.size() || confirmedFinalization.evidenceInputs.size())
                {
                    CTransaction finalizationTx;
                    const CTransaction *pEvidenceOutputTx = nullptr;
                    if (confirmedFinalization.output.IsOnSameTransaction())
                    {
                        pEvidenceOutputTx = &(txes[associatedIdx].first);
                    }
                    else
                    {
                        LOCK(mempool.cs);
                        uint256 hashBlock;
                        if (myGetTransaction(oneConfirmed.second.txIn.prevout.hash, finalizationTx, hashBlock))
                        {
                            pEvidenceOutputTx = &finalizationTx;
                        }
                        else
                        {
                            LogPrint("notarization", "%s: cannot access transaction required as input for notarization\n", __func__);
                            return false;
                        }
                    }

                    // add evidence outs as spends to close this entry as well
                    int afterMultiPart = confirmedFinalization.evidenceOutputs.size() ? confirmedFinalization.evidenceOutputs[0] : 0;
                    for (auto oneEvidenceOutN : confirmedFinalization.evidenceOutputs)
                    {
                        if (pEvidenceOutputTx->vout.size() <= oneEvidenceOutN)
                        {
                            LogPrint("notarization", "%s: indexing error for notarization evidence\n", __func__);
                            return false;
                        }

                        if (pEvidenceVec &&
                            oneEvidenceOutN >= afterMultiPart &&
                            associatedIdx != -1)
                        {
                            CNotaryEvidence oneEvidenceObj(*pEvidenceOutputTx, oneEvidenceOutN, afterMultiPart);
                            if (oneEvidenceObj.IsValid())
                            {
                                (*pEvidenceVec)[associatedIdx].push_back(oneEvidenceObj);
                            }
                        }

                        associatedSpends.push_back(
                            CInputDescriptor(pEvidenceOutputTx->vout[oneEvidenceOutN].scriptPubKey,
                                                pEvidenceOutputTx->vout[oneEvidenceOutN].nValue,
                                                CTxIn(pEvidenceOutputTx->GetHash(), oneEvidenceOutN)));
                        evidenceOutputSet.insert(associatedSpends.back().txIn.prevout);
                    }

                    if (pEvidenceVec &&
                        associatedIdx != -1)
                    {
                        // on a confirmed notarization, get input evidence as well, even though it doesn't need to be cleaned up
                        CTransaction priorOutputTx;
                        CNotaryEvidence oneEvidenceObj;
                        std::vector<CNotaryEvidence> evidenceInVec;
                        for (auto oneIn : confirmedFinalization.evidenceInputs)
                        {
                            uint256 hashBlock;
                            if (priorOutputTx.GetHash() != pEvidenceOutputTx->vin[oneIn].prevout.hash &&
                                !myGetTransaction(pEvidenceOutputTx->vin[oneIn].prevout.hash, priorOutputTx, hashBlock))
                            {
                                printf("%s: cannot access transaction for notarization evidence\n", __func__);
                                LogPrint("notarization", "%s: cannot access transaction for notarization evidence\n", __func__);
                                return false;
                            }

                            COptCCParams inP;
                            if (priorOutputTx.vout[pEvidenceOutputTx->vin[oneIn].prevout.n].scriptPubKey.IsPayToCryptoCondition(inP) &&
                                inP.IsValid() &&
                                inP.evalCode == EVAL_NOTARY_EVIDENCE &&
                                inP.vData.size() &&
                                (oneEvidenceObj = CNotaryEvidence(inP.vData[0])).IsValid() &&
                                oneEvidenceObj.evidence.chainObjects.size())
                            {
                                // there is no spend to store, as it has already been spent, but we
                                // still want to get its evidence

                                // if we are starting a new object, finish the old one
                                if (!oneEvidenceObj.IsMultipartProof() ||
                                    ((CChainObject<CEvidenceData> *)oneEvidenceObj.evidence.chainObjects[0])->object.md.index == 0)
                                {
                                    // if we have a composite evidence object, store it and clear the vector
                                    if (evidenceInVec.size())
                                    {
                                        CNotaryEvidence multiPartEvidence(evidenceInVec);
                                        if (multiPartEvidence.IsValid())
                                        {
                                            (*pEvidenceVec)[associatedIdx].push_back(multiPartEvidence);
                                        }
                                        evidenceInVec.clear();
                                    }
                                }

                                if (!oneEvidenceObj.IsMultipartProof())
                                {
                                    (*pEvidenceVec)[associatedIdx].push_back(oneEvidenceObj);
                                }
                                else
                                {
                                    evidenceInVec.push_back(oneEvidenceObj);
                                }
                            }
                        }

                        // if we have a composite evidence object, store it and clear the vector
                        if (evidenceInVec.size())
                        {
                            CNotaryEvidence multiPartEvidence(evidenceInVec);
                            if (multiPartEvidence.IsValid())
                            {
                                (*pEvidenceVec)[associatedIdx].push_back(multiPartEvidence);
                            }
                            else
                            {
                                printf("%s: invalid multipart evidence on input\n", __func__);
                                LogPrint("notarization", "%s: invalid multipart evidence on input\n", __func__);
                                return false;
                            }
                        }
                    }
                }

                // unspent evidence is specific to the target notarization
                std::vector<std::pair<uint32_t, CInputDescriptor>> unspentEvidence =
                    CObjectFinalization::GetUnspentEvidence(currencyID, vtx[associatedIdx].first.hash, vtx[associatedIdx].first.n);

                for (auto &oneEvidenceSpend : unspentEvidence)
                {
                    // if not already added to our spends as evidence, add it
                    if (!evidenceOutputSet.count(oneEvidenceSpend.second.txIn.prevout))
                    {
                        COptCCParams tP;
                        CNotaryEvidence oneEvidenceObj;
                        int afterEvidence;
                        if (pEvidenceVec &&
                            oneEvidenceSpend.second.scriptPubKey.IsPayToCryptoCondition(tP) &&
                            tP.IsValid() &&
                            tP.vData.size() &&
                            tP.evalCode == EVAL_NOTARY_EVIDENCE &&
                            (oneEvidenceObj = CNotaryEvidence(tP.vData[0])).IsValid())
                        {
                            (*pEvidenceVec)[associatedIdx].push_back(oneEvidenceObj);
                        }

                        associatedSpends.push_back(oneEvidenceSpend.second);
                    }
                }

                spendsToClose[associatedIdx].insert(spendsToClose[associatedIdx].end(), associatedSpends.begin(), associatedSpends.end());
            }
            else
            {
                extraSpends.insert(extraSpends.end(), associatedSpends.begin(), associatedSpends.end());
            }
        }

        // this output wasn't found associated with any valid confirmed or pending notarization
        if (associatedIdx == -1)
        {
            // for finalizations that have no found association, we warn
            // printf("%s: no associated notarization located for confirmed finalization:\n%s\n", __func__, confirmedNotarizationOutput.ToString().c_str());
            LogPrint("notarization", "%s: no associated notarization located for confirmed finalization:\n%s\n", __func__, confirmedNotarizationOutput.ToString().c_str());
        }
    }
    return true;
}

// returns a vector of unsigned 32 bit values as:
// 0 - nTime
// 1 - nBits
// 2 - nPoSBits
// 3 - (block height << 1) + IsPos bit
std::vector<uint32_t> UnpackBlockCommitment(__uint128_t oneBlockCommitment)
{
    std::vector<uint32_t> retVal;
    retVal.push_back(oneBlockCommitment & UINT32_MAX);
    oneBlockCommitment >>= 32;
    retVal.insert(retVal.begin(), oneBlockCommitment & UINT32_MAX);
    oneBlockCommitment >>= 32;
    retVal.insert(retVal.begin(), oneBlockCommitment & UINT32_MAX);
    oneBlockCommitment >>= 32;
    retVal.insert(retVal.begin(), oneBlockCommitment & UINT32_MAX);
    return retVal;
}

CProofRoot IsValidChallengeEvidence(const CCurrencyDefinition &externalSystem,
                                    const CProofRoot &defaultProofRoot,
                                    const CNotaryEvidence &e,
                                    const uint256 &entropyHash,
                                    bool &invalidates,
                                    CProofRoot &challengeStartRoot,
                                    uint32_t height)
{
    if (!e.IsRejected() || defaultProofRoot.systemID == ASSETCHAINS_CHAINID)
    {
        return CProofRoot();
    }
    bool validCounterEvidence = false;
    invalidates = false;

    // challenges to earned or accepted notarizations only differ in
    // the polarity of notarizations mirrored or not and each chain's
    // initial states.
    //
    // if the proof root system is a notary chain of this chain, we
    // can assume earned notarizations, except initial definition for
    // gateways. otherwise, we are working with mirrored, accepted
    // notarizations.
    //
    // currently, we support two types of challenges with the following properties:
    // 1) General challenge for proof, must have an alternate chain at least 3 blocks long. There
    //    are two subclasses of this kind of proof:
    //    a) Simply force proof about a chain that does not match the observed chain. In most cases,
    //       this is benign and will self-resolve, especially by sharing "good nodes" in notarizations.
    //    b) Proving a more powerful chain for an extended period of time and preventing finalization
    //       of alternate notarizations.
    //
    // 2) Fraud proof of failure to follow the notarization rule that requires a notarization to refer to
    //    the prior notarization on the blockchain with which it agrees, whether or not doing so would
    //    disqualify the miner or staker from being able to notarize at all.
    //    This type of proof simply proves or more recent notarization than the one claimed as being
    //    prior good one in the chain, using the proof root of the notarization to be invalidated.
    //    A notarization may be finalized as rejected, which invalidates all of its successors, if valid
    //    evidence of fraud is provided.
    //
    bool earnedNotarizations = false;
    if (ConnectedChains.NotarySystems().count(defaultProofRoot.systemID))
    {
        earnedNotarizations = true;
    }

    std::set<int> evidenceTypes;
    evidenceTypes.insert(CHAINOBJ_PROOF_ROOT);
    evidenceTypes.insert(CHAINOBJ_TRANSACTION_PROOF);
    evidenceTypes.insert(CHAINOBJ_COMMITMENTDATA);
    evidenceTypes.insert(CHAINOBJ_HEADER);
    evidenceTypes.insert(CHAINOBJ_HEADER_REF);
    CCrossChainProof autoProof(e.GetSelectEvidence(evidenceTypes));

    CPBaaSNotarization lastNotarization;
    CBlockHeader alternateHeader;

    // valid counter evidence includes:
    // commitment to more powerful chain since the last confirmed notarization:
    //    1) must include:
    //       a) proof root of alternate to the notarization proof root that is more powerful
    //       b) proof of header of the alternate root height
    //       b) block commitments behind header proof that lead to more powerful chain
    //       c) proof of prior random headers based on current system root as of evidence tx submission

    enum EProofState {
        EXPECT_NOTHING = 0,
        EXPECT_ALTERNATE_PROOF_ROOT = 1,            // alternate root to the notarization being challenged
        EXPECT_ALTERNATE_HEADER = 2,                // proof of alternate header at the challenge height
        EXPECT_ALT_LAST_NOTARIZATION_PROOF = 3,     // proof for the last notarization
        EXPECT_ALTERNATE_COMMITMENTS = 4,           // alternate commitments of prior blocks from this notarization/alternate header
        EXPECT_ALTERNATE_HEADER_PROOFS = 5,         // selected header proofs from the entropy root
        EXPECT_ALT_STAKE_TX = 6,                    // stake transaction in block for stake headers
        EXPECT_ALT_ID_PROOF_OR_STAKEGUARD = 7,      // necessary ID proof(s) to be able to confirm signature & end with stakeguard
        EXPECT_SKIPPED_NOTARIZATION_REF = 8,        // UTXO reference to the notarization that was skipped
        EXPECT_SKIPPED_HEADERREF_PROOF = 9,         // proof of a header ref from the skipped notarization using the challenged notarization
    };
    EProofState proofState = EXPECT_ALTERNATE_PROOF_ROOT;
    int numHeaderProofs = 0;
    int rangeLen = 0;
    int expectNumHeaderProofs = 0;

    CProofRoot counterEvidenceRoot;
    uint256 evolvingEntropyHash = entropyHash;
    uint256 blockTypes;
    std::map<uint32_t, __uint128_t> priorNotarizationCommitments;

    CBlockHeaderAndProof posBlockHeaderAndProof;
    CPartialTransactionProof posSourceProof;
    CStakeParams stakeParams;
    std::vector<CBlockHeaderProof> posEntropyHeaders;
    std::vector<CIdentity> stakeSpendingIDs;
    CCurrencyDefinition externalCurrency;

    // used for fraud proofs to prove the skipped notarization proof root and other
    // consistencies with the challenged notarization
    CPBaaSNotarization skippedNotarization, priorNotarization, challengedNotarization;

    for (auto &proofComponent : autoProof.chainObjects)
    {
        switch (proofState)
        {
            case EXPECT_ALTERNATE_PROOF_ROOT:
            {
                if (proofComponent->objectType == CHAINOBJ_PROOF_ROOT)
                {
                    counterEvidenceRoot = ((CChainObject<CProofRoot> *)proofComponent)->object;
                    if (counterEvidenceRoot.systemID == defaultProofRoot.systemID)
                    {
                        if (counterEvidenceRoot.stateRoot != defaultProofRoot.stateRoot ||
                            counterEvidenceRoot.blockHash != defaultProofRoot.blockHash)
                        {
                            proofState = EXPECT_ALTERNATE_HEADER;
                        }
                        else if (counterEvidenceRoot == defaultProofRoot)
                        {
                            // this is a fraud proof, which must use this proof root to
                            // prove something that should not be able to be proven
                            proofState = EXPECT_SKIPPED_NOTARIZATION_REF;
                        }
                        else
                        {
                            proofState = EXPECT_NOTHING;
                        }
                    }
                    else
                    {
                        proofState = EXPECT_NOTHING;
                    }
                }
                else
                {
                    proofState = EXPECT_NOTHING;
                }
                break;
            }

            case EXPECT_ALTERNATE_HEADER:
            {
                if (proofComponent->objectType == CHAINOBJ_HEADER)
                {
                    alternateHeader = ((CChainObject<CBlockHeaderAndProof> *)proofComponent)->object.blockHeader;

                    if (((CChainObject<CBlockHeaderAndProof> *)proofComponent)->object.BlockNum() == counterEvidenceRoot.rootHeight &&
                        ((CChainObject<CBlockHeaderAndProof> *)proofComponent)->object.ValidateBlockHash(alternateHeader.GetHash(), counterEvidenceRoot.rootHeight) ==
                            counterEvidenceRoot.stateRoot)
                    {
                        proofState = EXPECT_ALT_LAST_NOTARIZATION_PROOF;
                    }
                    else
                    {
                        proofState = EXPECT_NOTHING;
                    }
                }
                else
                {
                    proofState = EXPECT_NOTHING;
                }
                break;
            }
            // last notarization proof is based on the stateroot of the prior block from the alternate header
            // ensuring that both alternate proof root and alternate block header both have MMR-compatible roots
            // this must prove a confirmed notarization counterpart from the other chain before the one being challenged
            case EXPECT_ALT_LAST_NOTARIZATION_PROOF:
            {
                if (proofComponent->objectType == CHAINOBJ_TRANSACTION_PROOF)
                {
                    CTransaction outTx;
                    bool isPartial = false;
                    uint256 txMMRRoot = ((CChainObject<CPartialTransactionProof> *)proofComponent)->object.CheckPartialTransaction(outTx, &isPartial);
                    uint256 txHash = ((CChainObject<CPartialTransactionProof> *)proofComponent)->object.TransactionHash();

                    if (LogAcceptCategory("notarization"))
                    {
                        LogPrint("notarization", "Last notarization alternate proof expected\n");

                        UniValue jsonTx(UniValue::VOBJ);
                        uint256 blockHash;
                        TxToUniv(outTx, blockHash, jsonTx);
                        printf("%s: alternate proof for transaction %s\nwith txid: %s\nproof: %s\nat height: %u\n",
                                __func__,
                                jsonTx.write(1,2).c_str(),
                                txHash.GetHex().c_str(),
                                ((CChainObject<CPartialTransactionProof> *)proofComponent)->object.ToUniValue().write(1,2).c_str(),
                                ((CChainObject<CPartialTransactionProof> *)proofComponent)->object.GetBlockHeight());
                        LogPrint("verbose", "%s: alternate proof for transaction %s\nwith txid: %s\nproof: %s\nat height: %u\n",
                                __func__,
                                jsonTx.write(1,2).c_str(),
                                txHash.GetHex().c_str(),
                                ((CChainObject<CPartialTransactionProof> *)proofComponent)->object.ToUniValue().write(1,2).c_str(),
                                ((CChainObject<CPartialTransactionProof> *)proofComponent)->object.GetBlockHeight());
                    }

                    // find the last notarization on the transaction. if we need more proof to address a challenge, we will need to retain this
                    // either the notarization is:
                    // 1) the block 1 coinbase proof, or
                    // 2) the prelaunch notarization on the launch chain, or
                    // 2) a proof of a transaction on the other chain that corresponds to a confirmed or pending transaction on this chain.
                    if (txMMRRoot == alternateHeader.GetPrevMMRRoot())
                    {
                        for (auto &oneOut : outTx.vout)
                        {
                            COptCCParams lastNotaP;
                            if (oneOut.nValue >= 0 &&
                                oneOut.scriptPubKey.size() &&
                                oneOut.scriptPubKey.IsPayToCryptoCondition(lastNotaP) &&
                                lastNotaP.IsValid() &&
                                (lastNotaP.evalCode == EVAL_ACCEPTEDNOTARIZATION || lastNotaP.evalCode == EVAL_EARNEDNOTARIZATION) &&
                                lastNotaP.vData.size() &&
                                (lastNotarization = CPBaaSNotarization(lastNotaP.vData[0])).IsValid() &&
                                lastNotarization.SetMirror(!earnedNotarizations) &&
                                lastNotarization.currencyID == counterEvidenceRoot.systemID &&
                                lastNotarization.proofRoots.count(lastNotarization.currencyID) &&
                                lastNotarization.proofRoots.count(ASSETCHAINS_CHAINID))
                            {
                                break;
                            }
                            lastNotarization = CPBaaSNotarization();
                        }

                        if (lastNotarization.IsValid())
                        {
                            CObjectFinalization lastOf;
                            CAddressIndexDbEntry lastNotarizationAddressEntry;
                            // if we expect it to be on our chain, find it
                            if (!lastNotarization.IsBlockOneNotarization() && !lastNotarization.IsPreLaunch())
                            {
                                if (!lastNotarization.FindEarnedNotarization(lastOf, &lastNotarizationAddressEntry) ||
                                    !lastOf.IsConfirmed())
                                {
                                    lastNotarization = CPBaaSNotarization();
                                }
                            }
                        }
                    }
                    if (lastNotarization.IsValid())
                    {
                        challengeStartRoot = lastNotarization.proofRoots[defaultProofRoot.systemID];
                        rangeLen = counterEvidenceRoot.rootHeight - challengeStartRoot.rootHeight;
                        expectNumHeaderProofs = std::min(std::max(rangeLen, (int)CPBaaSNotarization::EXPECT_MIN_HEADER_PROOFS),
                                                std::min((int)CPBaaSNotarization::MAX_HEADER_PROOFS_PER_PROOF, rangeLen / 10));
                        // a challenge must have moved forward at least the number of block of minimum header proofs
                        proofState = rangeLen > CPBaaSNotarization::EXPECT_MIN_HEADER_PROOFS ? EXPECT_ALTERNATE_COMMITMENTS : EXPECT_NOTHING;
                    }
                    else
                    {
                        proofState = EXPECT_NOTHING;
                    }
                }
                else
                {
                    proofState = EXPECT_NOTHING;
                }
                break;
            }
            case EXPECT_ALTERNATE_COMMITMENTS:
            {
                if (proofComponent->objectType == CHAINOBJ_COMMITMENTDATA)
                {
                    std::vector<__uint128_t> checkCommitments;
                    blockTypes = ((CChainObject<CHashCommitments> *)proofComponent)->object.GetSmallCommitments(checkCommitments);

                    int startingHeight = challengeStartRoot.rootHeight;
                    int endHeight = lastNotarization.proofRoots[lastNotarization.currencyID].rootHeight;
                    rangeLen = endHeight - startingHeight++;

                    std::vector<std::pair<uint32_t, uint32_t>> commitmentRanges =
                        CPBaaSNotarization::GetBlockCommitmentRanges(startingHeight, endHeight, entropyHash);

                    int checkIndex = 0;
                    bool mismatchIndex = false;
                    for (auto &oneRange : commitmentRanges)
                    {
                        for (uint32_t loop = oneRange.first; loop < oneRange.second; loop++, checkIndex++)
                        {
                            if (loop != ((uint32_t)checkCommitments[checkIndex]) >> 1)
                            {
                                mismatchIndex = true;
                                break;
                            }
                            priorNotarizationCommitments.insert(std::make_pair(loop, checkCommitments[checkIndex]));
                        }
                        if (mismatchIndex)
                        {
                            break;
                        }
                    }

                    if (!mismatchIndex &&
                        priorNotarizationCommitments.size() &&
                        counterEvidenceRoot.IsValid())
                    {
                        expectNumHeaderProofs = std::min(std::max(rangeLen, (int)CPBaaSNotarization::EXPECT_MIN_HEADER_PROOFS),
                                                std::min((int)CPBaaSNotarization::MAX_HEADER_PROOFS_PER_PROOF, rangeLen / 10));


                        proofState = EXPECT_ALTERNATE_HEADER_PROOFS;
                    }
                    else
                    {
                        proofState = EXPECT_NOTHING;
                    }
                }
                else
                {
                    proofState = EXPECT_NOTHING;
                }
                break;
            }
            case EXPECT_ALTERNATE_HEADER_PROOFS:
            {
                if ((proofComponent->objectType == CHAINOBJ_HEADER) &&
                    numHeaderProofs < expectNumHeaderProofs)
                {
                    int rangeStart = lastNotarization.proofRoots[lastNotarization.currencyID].rootHeight;
                    int rangeLen = counterEvidenceRoot.rootHeight - rangeStart++;

                    int indexToProve = (UintToArith256(evolvingEntropyHash).GetLow64() % rangeLen);
                    evolvingEntropyHash = ::GetHash(evolvingEntropyHash);
                    uint32_t blockToProve = rangeStart + indexToProve;

                    CBlockHeader &blockHeader = ((CChainObject<CBlockHeaderAndProof> *)proofComponent)->object.blockHeader;
                    std::vector<uint32_t> oneBlockCommitment = priorNotarizationCommitments.count(blockToProve) ?
                        UnpackBlockCommitment(priorNotarizationCommitments[blockToProve]) :
                        std::vector<uint32_t>({blockHeader.nTime,
                                               blockHeader.nBits,
                                               blockHeader.GetVerusPOSTarget(),
                                               blockToProve << 1 | (uint32_t)blockHeader.IsVerusPOSBlock()});

                    // TODO: HARDENING - verify that we match commitments
                    // both here and in PoS, we need to calculate the new power from the old power
                    // and this header solve if two overlap

                    if (((CChainObject<CBlockHeaderAndProof> *)proofComponent)->object.BlockNum() == blockToProve &&
                        blockHeader.nTime == oneBlockCommitment[0] &&
                        blockHeader.IsVerusPOSBlock() == (oneBlockCommitment[3] & 1) &&
                        ((oneBlockCommitment[3] & 1) ? blockHeader.GetVerusPOSTarget() == oneBlockCommitment[2] : blockHeader.nBits == oneBlockCommitment[1]) &&
                        ((CChainObject<CBlockHeaderAndProof> *)proofComponent)->object.ValidateBlockHash(blockHeader.GetHash(), blockToProve) ==
                            counterEvidenceRoot.stateRoot)
                    {
                        if (blockHeader.IsVerusPOSBlock())
                        {
                            posBlockHeaderAndProof = ((CChainObject<CBlockHeaderAndProof> *)proofComponent)->object;
                            posEntropyHeaders.clear();
                            // make sure we have all the expected objects in the header

                            // get proofs of the following from the PoS header, all proven by the MMR of the
                            // block just behind the PoS block:
                            // 1) proof of the source UTXO for the stake
                            // 2) the two header ref proofs from those used PoS entropy
                            //
                            std::vector<unsigned char> streamVch;
                            posBlockHeaderAndProof.blockHeader.GetExtraData(streamVch);
                            CDataStream ds(streamVch, SER_DISK, PROTOCOL_VERSION);

                            try
                            {
                                ds >> posSourceProof;
                                if (posSourceProof.IsValid())
                                {
                                    // and get the two entropy sources
                                    posEntropyHeaders.push_back(CBlockHeaderProof());
                                    ds >> posEntropyHeaders.back();
                                    if (posEntropyHeaders.back().IsValid())
                                    {
                                        posEntropyHeaders.push_back(CBlockHeaderProof());
                                        ds >> posEntropyHeaders.back();
                                    }
                                }
                                proofState = posEntropyHeaders.size() == 2 && posEntropyHeaders.back().IsValid() ? EXPECT_ALT_STAKE_TX : EXPECT_NOTHING;
                            }
                            catch(const std::exception& e)
                            {
                                std::cerr << e.what() << '\n';
                                proofState = EXPECT_NOTHING;
                            }
                        }
                        else
                        {
                            numHeaderProofs++;
                            proofState = EXPECT_ALTERNATE_HEADER_PROOFS;
                        }
                    }
                    else
                    {
                        // invalid proofs fail
                        proofState = EXPECT_NOTHING;
                    }
                    validCounterEvidence = numHeaderProofs == expectNumHeaderProofs;
                }
                else
                {
                    proofState = EXPECT_NOTHING;
                }
                break;
            }
            case EXPECT_ALT_STAKE_TX:
            {
                if (proofComponent->objectType == CHAINOBJ_TRANSACTION_PROOF)
                {
                    // expect a full transaction proof up to the merkle tree root
                    // of the block header of the actual PoS transaction in the block. along with the source output
                    // entropy blocks, and proof contained in the last PoS header, we can confirm both the
                    // validity of the PoS win from that source transaction, as well as the input script signature
                    // ensuring that it is truly a potentially valid proof of stake, assuming we can trust
                    // the blockchain >100 blocks behind us.

                    CTransaction outTx;
                    bool isPartial = false;
                    uint256 txMerkleRoot = ((CChainObject<CPartialTransactionProof> *)proofComponent)->object.CheckPartialTransaction(outTx, &isPartial);
                    bool stakeTxPassed = txMerkleRoot == posBlockHeaderAndProof.blockHeader.hashMerkleRoot &&
                                         ValidateStakeTransaction(outTx, stakeParams, false) &&
                                         stakeParams.prevHash == posBlockHeaderAndProof.blockHeader.hashPrevBlock &&
                                         stakeParams.srcHeight == posSourceProof.GetBlockHeight();

                    // Below, after we have gotten the stakeguard output and when we have any ID keys we may need, we can
                    // verify the spend signature.

                    if (stakeTxPassed)
                    {
                        // now, the proof we just got should be the stake transaction proven against the merkle root of our
                        // header, followed by proofs of any ID outputs that are needed to verify signatures, followed by
                        // the coinbase output proven against the header MMR root.

                        // with the output, the two header refs from the header proof and this stake transaction,
                        // we can now verify the PoS win against the PoS difficulty.
                        if (!externalCurrency.IsValid())
                        {
                            externalCurrency = ConnectedChains.GetCachedCurrency(counterEvidenceRoot.systemID);
                        }

                        arith_uint256 posTarget;
                        posTarget.SetCompact(posBlockHeaderAndProof.blockHeader.GetVerusPOSTarget());

                        uint256 rawPosHash = CBlockHeader::GetRawVerusPOSHash(
                            posBlockHeaderAndProof.blockHeader.nVersion,
                            CConstVerusSolutionVector::Version(posBlockHeaderAndProof.blockHeader.nSolution),
                            externalCurrency.MagicNumber(),
                            posBlockHeaderAndProof.blockHeader.nNonce,
                            ((CChainObject<CPartialTransactionProof> *)proofComponent)->object.GetBlockHeight(),
                            (!PBAAS_TESTMODE && counterEvidenceRoot.systemID == VERUS_CHAINID));

                        if (externalCurrency.IsValid() && (UintToArith256(rawPosHash) / outTx.vout[0].nValue) > posTarget)
                        {
                            proofState = EXPECT_NOTHING;
                        }
                        else
                        {
                            proofState = EXPECT_ALT_ID_PROOF_OR_STAKEGUARD;
                        }
                    }
                    else
                    {
                        proofState = EXPECT_NOTHING;
                    }
                }
                else
                {
                    proofState = EXPECT_NOTHING;
                }
                break;
            }

            case EXPECT_ALT_ID_PROOF_OR_STAKEGUARD:
            {
                if (proofComponent->objectType == CHAINOBJ_TRANSACTION_PROOF)
                {
                    // this is either an ID proof, proven by the prior block's blockchain MMR root,
                    // or a proof of the coinbase stakeguard output, proven by the block MMR root of the current
                    // POS header.
                    CTransaction outTx;
                    bool isPartial = false;
                    bool foundOut = false;
                    CValidationState tmpState;
                    uint256 txMMRRoot = ((CChainObject<CPartialTransactionProof> *)proofComponent)->object.CheckPartialTransaction(outTx, &isPartial);
                    CProofRoot lastNotarizationRoot = lastNotarization.proofRoots[counterEvidenceRoot.systemID];

                    if (txMMRRoot == posBlockHeaderAndProof.blockHeader.GetBlockMMRRoot())
                    {
                        for (int j = 0; j < outTx.vout.size(); j++)
                        {
                            COptCCParams optP;
                            auto &oneOut = outTx.vout[j];
                            if (oneOut.nValue >= 0 &&
                                oneOut.scriptPubKey.IsPayToCryptoCondition(optP) &&
                                optP.IsValid() &&
                                optP.evalCode == EVAL_STAKEGUARD &&
                                RawPrecheckStakeGuardOutput(outTx, j, tmpState))
                            {
                                foundOut = true;
                                break;
                            }
                        }


                        // TODO: HARDENING - now verify the stake tx signature, even if it spends an ID


                        // after confirming stakeguard, we either have another header proof, or we're done
                        validCounterEvidence = (++numHeaderProofs >= expectNumHeaderProofs);
                        proofState = EXPECT_ALTERNATE_HEADER_PROOFS;
                    }
                    else if (posBlockHeaderAndProof.BlockNum() > 0 &&
                             (posBlockHeaderAndProof.BlockNum() - ((CChainObject<CPartialTransactionProof> *)proofComponent)->object.GetBlockHeight()) >= VERUS_MIN_STAKEAGE &&
                             ((((CChainObject<CPartialTransactionProof> *)proofComponent)->object.GetBlockHeight() > lastNotarizationRoot.rootHeight &&
                               txMMRRoot == posBlockHeaderAndProof.blockHeader.GetPrevMMRRoot()) ||
                              txMMRRoot == lastNotarization.proofRoots[counterEvidenceRoot.systemID].stateRoot))
                    {
                        for (auto &oneOut : outTx.vout)
                        {
                            COptCCParams optP;
                            CIdentity oneID;
                            if (oneOut.nValue >= 0 &&
                                oneOut.scriptPubKey.IsPayToCryptoCondition(optP) &&
                                optP.IsValid() &&
                                optP.evalCode == EVAL_IDENTITY_PRIMARY &&
                                optP.vData.size() &&
                                (oneID = CIdentity(optP.vData[0])).IsValid())
                            {
                                stakeSpendingIDs.push_back(oneID);
                            }
                            else if (oneOut.nValue >= 0)
                            {
                                // this should only be proving an ID output, nothing else
                                proofState = EXPECT_NOTHING;
                                break;
                            }
                        }
                        // after confirming one ID proof, next should be an ID or the stakeguard
                        // we will leave it to be that unless we have set an error
                    }
                    else
                    {
                        proofState = EXPECT_NOTHING;
                    }
                }
                break;
            }
            case EXPECT_SKIPPED_NOTARIZATION_REF:
            {
                // we should find an output reference to a notarization present on this chain
                // proven in the next step by the non-compliant notarization and which is not
                // referenced as a prior by this notarization, proving that the notarization
                // should have referenced the more recent one, rather than skipping it.
                //
                // proof of a specific header referred to by a notarization that should
                // have been referenced, but was skipped by the notarization this challenge refers to
                CUTXORef notarizationTxRef;
                CTransaction priorTx, skippedTx;
                uint256 priorBlkHash, skippedBlkHash;

                BlockMap::iterator priorIdx, skippedIdx, challengedIdx;

                if (proofComponent->objectType == CHAINOBJ_EVIDENCEDATA &&
                    ((CChainObject<CEvidenceData> *)proofComponent)->object.vdxfd == CVDXF_Data::UTXORefKey() &&
                    ((CChainObject<CEvidenceData> *)proofComponent)->object.dataVec.size() &&
                    (notarizationTxRef = CUTXORef(((CChainObject<CEvidenceData> *)proofComponent)->object.dataVec)).IsValid() &&
                    myGetTransaction(notarizationTxRef.hash, skippedTx, skippedBlkHash) &&
                    !skippedBlkHash.IsNull() &&
                    (skippedIdx = mapBlockIndex.find(skippedBlkHash)) != mapBlockIndex.end() &&
                    chainActive.Contains(skippedIdx->second) &&
                    skippedTx.vout.size() > notarizationTxRef.n &&
                    (skippedNotarization = CPBaaSNotarization(priorTx.vout[notarizationTxRef.n].scriptPubKey)).IsValid() &&
                    e.output.GetOutputTransaction(priorTx, priorBlkHash) &&
                    notarizationTxRef.n > 0 &&
                    priorTx.vout.size() > notarizationTxRef.n &&
                    (challengedNotarization = CPBaaSNotarization(priorTx.vout[e.output.n].scriptPubKey)).IsValid() &&
                    challengedNotarization.currencyID == defaultProofRoot.systemID &&
                    !priorBlkHash.IsNull() &&
                    (challengedIdx = mapBlockIndex.find(priorBlkHash)) != mapBlockIndex.end() &&
                    chainActive.Contains(challengedIdx->second) &&
                    challengedNotarization.prevNotarization.GetOutputTransaction(priorTx, priorBlkHash) &&
                    !priorBlkHash.IsNull() &&
                    (priorIdx = mapBlockIndex.find(priorBlkHash)) != mapBlockIndex.end() &&
                    chainActive.Contains(priorIdx->second) &&
                    priorTx.vout.size() > challengedNotarization.prevNotarization.n &&
                    (priorNotarization = CPBaaSNotarization(priorTx.vout[challengedNotarization.prevNotarization.n].scriptPubKey)).IsValid() &&
                    skippedIdx->second->GetHeight() <= challengedIdx->second->GetHeight() &&
                    priorIdx->second->GetHeight() <= skippedIdx->second->GetHeight() &&
                    priorNotarization.proofRoots.count(defaultProofRoot.systemID) &&
                    priorNotarization.proofRoots[defaultProofRoot.systemID].rootHeight < skippedNotarization.proofRoots[defaultProofRoot.systemID].rootHeight)
                {
                    proofState = EXPECT_SKIPPED_HEADERREF_PROOF;
                }
                else
                {
                    proofState = EXPECT_NOTHING;
                }
                break;
            }
            case EXPECT_SKIPPED_HEADERREF_PROOF:
            {
                // this should be a header ref proof of the proof root in the skipped notarization with the proof root in
                // the challenged notarization
                if (proofComponent->objectType == CHAINOBJ_HEADER_REF &&
                    challengedNotarization.proofRoots[defaultProofRoot.systemID].stateRoot ==
                     ((CChainObject<CBlockHeaderProof> *)proofComponent)->object.ValidateBlockHash(skippedNotarization.proofRoots[defaultProofRoot.systemID].blockHash,
                                                                                                  skippedNotarization.proofRoots[defaultProofRoot.systemID].rootHeight))
                {
                    validCounterEvidence = true;
                    invalidates = true;
                }
                break;
            }
            default:
            {
                proofState = EXPECT_NOTHING;
                break;
            }
        }
        if (proofState == EXPECT_NOTHING || validCounterEvidence)
        {
            break;
        }
    }
    return validCounterEvidence ? counterEvidenceRoot : CProofRoot(CProofRoot::TYPE_PBAAS, CProofRoot::VERSION_INVALID);
}

bool IsPreTestnetFork(const uint256 &blockHash)
{
    auto notaIndexIT = mapBlockIndex.find(blockHash);
    return notaIndexIT->second->nTime < PBAAS_TESTFORK_TIME;
}

CPBaaSNotarization IsValidPrimaryChainEvidence(const CCurrencyDefinition &externalSystem,
                                               const CNotaryEvidence &evidence,
                                               const CPBaaSNotarization &expectedNotarization,
                                               uint32_t lastConfirmedHeight,
                                               uint32_t height,
                                               uint256 *pEntropyHash,
                                               const CProofRoot &challengeProofRoot,
                                               const CProofRoot &challengStartPoint)
{
    std::set<int> evidenceTypes;

    // if this is the first proof, it will extend a prelaunch finalization,
    // which should always pass
    if (expectedNotarization.IsPreLaunch() &&
        expectedNotarization.proofRoots.count(ASSETCHAINS_CHAINID) &&
        expectedNotarization.currencyState.IsPrelaunch())
    {
        return expectedNotarization;
    }

    if (challengeProofRoot.IsValid() && !(expectedNotarization.proofRoots.count(challengeProofRoot.systemID)))
    {
        return CPBaaSNotarization();
    }

    uint256 _entropyHash;
    uint256 &entropyHash = (pEntropyHash ? *pEntropyHash : _entropyHash);

    // one for the proof of the newly witnessed, confirmed notarization
    // one proof root for the entropy from the other chain if this verifies prior commitments
    evidenceTypes.insert(CHAINOBJ_PROOF_ROOT);

    // proof of the last confirmed notarization matching the one on this chain or block 1 coinbase
    // by the newly confirmed notarization
    // proof of the current notarization by the first proof root mentioned above
    evidenceTypes.insert(CHAINOBJ_TRANSACTION_PROOF);

    // commitment of last block time, bits, pos diff, and pos/pow
    evidenceTypes.insert(CHAINOBJ_COMMITMENTDATA);

    // random header proof of one or more of last notarization's commitment, proven by the last notarization
    evidenceTypes.insert(CHAINOBJ_HEADER);

    // currently unused
    evidenceTypes.insert(CHAINOBJ_HEADER_REF);
    CCrossChainProof autoProof(evidence.GetSelectEvidence(evidenceTypes));

    if (autoProof.chainObjects.size() < 3)
    {
        if (IsVerusMainnetActive())
        {
            return CPBaaSNotarization();
        }

        auto notaRootIT = expectedNotarization.proofRoots.find(ASSETCHAINS_CHAINID);
        if (notaRootIT == expectedNotarization.proofRoots.end())
        {
            return expectedNotarization;
        }
        if (IsPreTestnetFork(notaRootIT->second.blockHash))
        {
            return expectedNotarization;
        }
        else
        {
            return CPBaaSNotarization();
        }
    }

    enum EProofState {
        EXPECT_NOTHING = 0,
        EXPECT_LASTNOTARIZATION_PROOF = 1,      // transaction proof of block 1 coinbase or last confirmed on this chain
        EXPECT_FUTURE_PROOF_ROOT = 2,           // future root to prove this notarization
        EXPECT_THISNOTARIZATION_PROOF = 3,      // proof of this notarization
        EXPECT_CHECKPOINT = 4,                  // checkpoints between large spans of notarizations to narrow challenges
        EXPECT_COMMITMENT_PROOF = 5,            // commitments of prior blocks from this notarization (only needed if there is a challenge proof root)
        EXPECT_HEADER_PROOFS = 6,               // header proofs randomly selected from prior commitments, based on entropy
        EXPECT_STAKE_TX = 7,                    // stake transaction in block for stake headers
        EXPECT_ID_PROOF_OR_STAKEGUARD = 8,      // necessary ID proof(s) to be able to confirm signature & end with coinbase stakeguard output
    };

    EProofState proofState = EXPECT_LASTNOTARIZATION_PROOF;

    CProofRoot futureProofRoot(CProofRoot::TYPE_PBAAS, CProofRoot::VERSION_INVALID);
    CPBaaSNotarization provenNotarization = expectedNotarization;
    if (!provenNotarization.proofRoots.count(provenNotarization.currencyID) ||
        !provenNotarization.proofRoots.count(ASSETCHAINS_CHAINID))
    {
        return CPBaaSNotarization();
    }

    CPBaaSNotarization lastNotarization;
    CPBaaSNotarization lastLocalNotarization;
    CAddressIndexDbEntry lastNotarizationAddressEntry;
    uint256 commitmentHash;
    uint256 priorCommitmentHash;
    std::map<uint32_t, __uint128_t> priorNotarizationCommitments;
    std::vector<std::pair<uint32_t, uint32_t>> commitmentRanges;
    CTransaction priorNotaryTx;
    CNotaryEvidence priorEvidence;

    int startingHeight = 0;
    int rangeLen = 0;
    int numHeaders = 0;
    int numExpectedCheckpoints = 0;
    int numCheckpointsFound = 0;
    int blocksPerCheckpoint = 0;
    std::vector<CProofRoot> checkpointRoots;

    int numHeaderProofs = 0;
    uint256 blockTypes;

    bool validChallengeEvidence = false;
    bool validBasicEvidence = false;

    CProofRoot &defaultProofRoot = provenNotarization.proofRoots[provenNotarization.currencyID];
    CProofRoot &currentChainRoot = provenNotarization.proofRoots[ASSETCHAINS_CHAINID];

    uint256 headerSelectionHash;

    CBlockHeaderAndProof posBlockHeaderAndProof;
    CPartialTransactionProof posSourceProof;
    CStakeParams stakeParams;
    std::vector<CBlockHeaderProof> posEntropyHeaders;
    std::vector<CIdentity> stakeSpendingIDs;
    CCurrencyDefinition externalCurrency;

    for (auto &proofComponent : autoProof.chainObjects)
    {
        switch (proofState)
        {
            case EXPECT_LASTNOTARIZATION_PROOF:
            {
                if (proofComponent->objectType == CHAINOBJ_TRANSACTION_PROOF)
                {
                    CTransaction outTx;
                    bool isPartial = false;
                    uint256 txMMRRoot = ((CChainObject<CPartialTransactionProof> *)proofComponent)->object.CheckPartialTransaction(outTx, &isPartial);
                    uint256 txHash = ((CChainObject<CPartialTransactionProof> *)proofComponent)->object.TransactionHash();

                    if (LogAcceptCategory("notarization"))
                    {
                        LogPrint("notarization", "Last notarization proof expected\n");

                        UniValue jsonTx(UniValue::VOBJ);
                        uint256 blockHash;
                        TxToUniv(outTx, blockHash, jsonTx);
                        printf("%s: proof for transaction %s\nwith txid: %s\nproof: %s\nat height: %u, proofheight: %u\n",
                                __func__,
                                jsonTx.write(1,2).c_str(),
                                txHash.GetHex().c_str(),
                                ((CChainObject<CPartialTransactionProof> *)proofComponent)->object.ToUniValue().write(1,2).c_str(),
                                ((CChainObject<CPartialTransactionProof> *)proofComponent)->object.GetBlockHeight(),
                                ((CChainObject<CPartialTransactionProof> *)proofComponent)->object.GetProofHeight());
                        LogPrintf("%s: proof for transaction %s\nwith txid: %s\nproof: %s\nat height: %u, proofheight: %u\n",
                                __func__,
                                jsonTx.write(1,2).c_str(),
                                txHash.GetHex().c_str(),
                                ((CChainObject<CPartialTransactionProof> *)proofComponent)->object.ToUniValue().write(1,2).c_str(),
                                ((CChainObject<CPartialTransactionProof> *)proofComponent)->object.GetBlockHeight(),
                                ((CChainObject<CPartialTransactionProof> *)proofComponent)->object.GetProofHeight());
                    }

                    // find the last notarization on the transaction. if we need more proof to address a challenge, we will need to retain this
                    // either the notarization is the block 1 coinbase proof, or it is a proof of a transaction on the other chain that corresponds
                    // to a confirmed or pending transaction on this chain.
                    if (txMMRRoot == provenNotarization.proofRoots[provenNotarization.currencyID].stateRoot)
                    {
                        for (auto &oneOut : outTx.vout)
                        {
                            COptCCParams lastNotaP;
                            if (oneOut.nValue >= 0 &&
                                oneOut.scriptPubKey.size() &&
                                oneOut.scriptPubKey.IsPayToCryptoCondition(lastNotaP) &&
                                lastNotaP.IsValid() &&
                                (lastNotaP.evalCode == EVAL_ACCEPTEDNOTARIZATION || lastNotaP.evalCode == EVAL_EARNEDNOTARIZATION) &&
                                lastNotaP.vData.size() &&
                                (lastNotarization = CPBaaSNotarization(lastNotaP.vData[0])).IsValid() &&
                                (lastNotarization.IsPreLaunch() ||
                                 lastNotarization.IsBlockOneNotarization() ||
                                 lastNotarization.SetMirror(expectedNotarization.IsMirror())) &&
                                lastNotarization.currencyID == expectedNotarization.currencyID)
                            {
                                uint32_t checkHeight = provenNotarization.proofRoots[ASSETCHAINS_CHAINID].rootHeight;
                                if (checkHeight <= chainActive.Height() &&
                                    (checkHeight - lastNotarization.proofRoots[ASSETCHAINS_CHAINID].rootHeight) >=
                                            (chainActive[checkHeight]->nBits <= PBAAS_TESTFORK_TIME ?
                                                1 :
                                                CPBaaSNotarization::GetAdjustedNotarizationModulo(ConnectedChains.ThisChain().blockNotarizationModulo,
                                                                                                  height - lastConfirmedHeight)))
                                {
                                    break;
                                }
                            }
                            lastNotarization = CPBaaSNotarization();
                        }

                        if (lastNotarization.IsValid())
                        {
                            // if any of this is not true, the notarization evidence is not valid
                            uint32_t checkHeight = provenNotarization.proofRoots[ASSETCHAINS_CHAINID].rootHeight;
                            if (lastNotarization.proofRoots.count(lastNotarization.currencyID) &&
                                lastNotarization.proofRoots.count(ASSETCHAINS_CHAINID) &&
                                checkHeight <= chainActive.Height() &&
                                (provenNotarization.proofRoots[ASSETCHAINS_CHAINID].rootHeight -
                                        lastNotarization.proofRoots[ASSETCHAINS_CHAINID].rootHeight) >=
                                            (chainActive[provenNotarization.proofRoots[ASSETCHAINS_CHAINID].rootHeight]->nBits >=
                                            PBAAS_TESTFORK_TIME ?
                                                1 :
                                                CPBaaSNotarization::GetAdjustedNotarizationModulo(ConnectedChains.ThisChain().blockNotarizationModulo,
                                                                                                  height - lastConfirmedHeight)))
                            {
                                // evidence from the last notarization, unless it is prelaunch, includes
                                // proof of a prior notarization that is also on this chain (after PBAAS_TESTFORK_TIME)
                                // it also either includes commitments to pull from when proving headers, or a hash of
                                // full prior commitments.
                                // if the proven notarization is block 1, confirm that it is the full coinbase proof.
                                // if it is not block one, and we find no match in confirmed or pending
                                // notarizations, we must fail
                                if (lastNotarization.IsBlockOneNotarization())
                                {
                                    // the prior confirmed notarization on our chain must be prelaunch when this was made,
                                    // and we should be able to find it with a search before the root height of the last
                                    // proof root for our chain and up to the height of the block 1 notarization proof root.
                                    if (!(lastLocalNotarization.GetLastNotarization(provenNotarization.currencyID,
                                                                                    0,
                                                                                    lastNotarization.proofRoots[ASSETCHAINS_CHAINID].rootHeight) &&
                                          lastLocalNotarization.IsPreLaunch()))
                                    {
                                        lastNotarization = CPBaaSNotarization();
                                    }
                                }
                                else if (chainActive[lastNotarization.proofRoots[ASSETCHAINS_CHAINID].rootHeight]->nBits >= PBAAS_TESTFORK_TIME)
                                {
                                    CObjectFinalization lastOf;
                                    lastLocalNotarization = lastNotarization;
                                    if (!lastLocalNotarization.FindEarnedNotarization(lastOf, &lastNotarizationAddressEntry))
                                    {
                                        lastLocalNotarization = lastNotarization = CPBaaSNotarization();
                                    }
                                    else
                                    {
                                        headerSelectionHash = entropyHash = chainActive[lastNotarizationAddressEntry.first.blockHeight]->GetBlockHash();
                                    }
                                }
                            }
                            else
                            {
                                lastNotarization = CPBaaSNotarization();
                            }
                        }
                    }
                    else if (LogAcceptCategory("notarization"))
                    {
                        printf("expected state root: %s, from proof at height %u\ntxid: %s\nproven notarization: %s\n",
                                txMMRRoot.GetHex().c_str(),
                                ((CChainObject<CPartialTransactionProof> *)proofComponent)->object.GetProofHeight(),
                                txHash.GetHex().c_str(),
                                provenNotarization.ToUniValue().write(1,2).c_str());
                        LogPrintf("expected state root: %s, from proof at height %u\ntxid: %s\nproven notarization: %s\n",
                                txMMRRoot.GetHex().c_str(),
                                ((CChainObject<CPartialTransactionProof> *)proofComponent)->object.GetProofHeight(),
                                txHash.GetHex().c_str(),
                                provenNotarization.ToUniValue().write(1,2).c_str());
                    }
                    proofState = lastNotarization.IsValid() ? EXPECT_FUTURE_PROOF_ROOT : EXPECT_NOTHING;
                }
                else
                {
                    proofState = EXPECT_NOTHING;
                }
                break;
            }

            case EXPECT_FUTURE_PROOF_ROOT:
            {
                if (proofComponent->objectType == CHAINOBJ_PROOF_ROOT)
                {
                    futureProofRoot = ((CChainObject<CProofRoot> *)proofComponent)->object;
                    if (futureProofRoot.systemID == expectedNotarization.currencyID &&
                        futureProofRoot.rootHeight > defaultProofRoot.rootHeight)
                    {
                        proofState = EXPECT_THISNOTARIZATION_PROOF;
                    }
                    else
                    {
                        proofState = EXPECT_NOTHING;
                    }
                }
                else
                {
                    proofState = EXPECT_NOTHING;
                }
                break;
            }

            case EXPECT_THISNOTARIZATION_PROOF:
            {
                if (proofComponent->objectType == CHAINOBJ_TRANSACTION_PROOF)
                {
                    CTransaction outTx;
                    bool isPartial = false;
                    uint256 txMMRRoot = ((CChainObject<CPartialTransactionProof> *)proofComponent)->object.CheckPartialTransaction(outTx, &isPartial);
                    uint256 txHash = ((CChainObject<CPartialTransactionProof> *)proofComponent)->object.TransactionHash();

                    // loop through and check all the valid outputs to find a match
                    if (!provenNotarization.SetMirror(false) ||
                        txMMRRoot != futureProofRoot.stateRoot)
                    {
                        if (LogAcceptCategory("notarization"))
                        {
                            printf("%s: invalid notarization transaction proof, from height %u, proven at height %u\n", __func__, ((CChainObject<CPartialTransactionProof> *)proofComponent)->object.GetBlockHeight(), ((CChainObject<CPartialTransactionProof> *)proofComponent)->object.GetProofHeight());
                            LogPrintf("%s: invalid notarization transaction proof, from height %u, proven at height %u\n", __func__, ((CChainObject<CPartialTransactionProof> *)proofComponent)->object.GetBlockHeight(), ((CChainObject<CPartialTransactionProof> *)proofComponent)->object.GetProofHeight());
                        }
                        proofState = EXPECT_NOTHING;
                    }
                    else
                    {
                        bool notarizationFound = false;
                        for (auto &oneOut : outTx.vout)
                        {
                            COptCCParams notaP;
                            CPBaaSNotarization checkNotarization;
                            if (oneOut.nValue >= 0 &&
                                oneOut.scriptPubKey.IsPayToCryptoCondition(notaP) &&
                                (notaP.evalCode == EVAL_EARNEDNOTARIZATION || notaP.evalCode == EVAL_ACCEPTEDNOTARIZATION) &&
                                notaP.vData.size() &&
                                ::AsVector(CPBaaSNotarization(notaP.vData[0])) == ::AsVector(CPBaaSNotarization(provenNotarization)))
                            {
                                // just record we found a match
                                notarizationFound = true;
                                break;
                            }
                        }

                        // if we are spanning more than the min blocks per checkpoint, expect checkpoint(s)
                        int heightChange = provenNotarization.proofRoots[lastNotarization.currencyID].rootHeight -
                                            lastNotarization.proofRoots[lastNotarization.currencyID].rootHeight;
                        numExpectedCheckpoints = CPBaaSNotarization::GetNumCheckpoints(heightChange);
                        if (notarizationFound &&
                            numExpectedCheckpoints &&
                            chainActive[lastNotarization.proofRoots[ASSETCHAINS_CHAINID].rootHeight]->nBits >= PBAAS_TESTFORK_TIME)
                        {
                            blocksPerCheckpoint = CPBaaSNotarization::GetBlocksPerCheckpoint(heightChange);
                            proofState = EXPECT_CHECKPOINT;
                        }
                        else
                        {
                            validBasicEvidence = (lastLocalNotarization.IsValid() && lastLocalNotarization.IsPreLaunch()) || (notarizationFound && !challengeProofRoot.IsValid());
                            proofState = !notarizationFound || validBasicEvidence ? EXPECT_NOTHING : EXPECT_COMMITMENT_PROOF;
                        }
                    }

                    if (LogAcceptCategory("notarization"))
                    {
                        LogPrint("notarization", "This notarization proof expected\n");

                        UniValue jsonTx(UniValue::VOBJ);
                        uint256 blockHash;
                        TxToUniv(outTx, blockHash, jsonTx);
                        printf("%s: proof for transaction %s\nwith txid: %s\nproof: %s\nat height: %u\n",
                                __func__,
                                jsonTx.write(1,2).c_str(),
                                txHash.GetHex().c_str(),
                                ((CChainObject<CPartialTransactionProof> *)proofComponent)->object.ToUniValue().write(1,2).c_str(),
                                ((CChainObject<CPartialTransactionProof> *)proofComponent)->object.GetBlockHeight());
                        LogPrint("verbose", "%s: proof for transaction %s\nwith txid: %s\nproof: %s\nat height: %u\n",
                                __func__,
                                jsonTx.write(1,2).c_str(),
                                txHash.GetHex().c_str(),
                                ((CChainObject<CPartialTransactionProof> *)proofComponent)->object.ToUniValue().write(1,2).c_str(),
                                ((CChainObject<CPartialTransactionProof> *)proofComponent)->object.GetBlockHeight());
                    }
                }
                else
                {
                    proofState = EXPECT_NOTHING;
                }
                break;
            }

            case EXPECT_CHECKPOINT:
            {
                if (proofComponent->objectType == CHAINOBJ_PROOF_ROOT)
                {
                    CProofRoot oneCheckpoint = ((CChainObject<CProofRoot> *)proofComponent)->object;
                    if (blocksPerCheckpoint &&
                        oneCheckpoint.systemID == expectedNotarization.currencyID &&
                        oneCheckpoint.rootHeight ==
                            (lastNotarization.proofRoots[lastNotarization.currencyID].rootHeight +
                             (++numCheckpointsFound * blocksPerCheckpoint)))
                    {
                        checkpointRoots.push_back(oneCheckpoint);
                        if (numCheckpointsFound == numExpectedCheckpoints)
                        {
                            validBasicEvidence = (lastLocalNotarization.IsValid() && lastLocalNotarization.IsPreLaunch()) || !challengeProofRoot.IsValid();
                            proofState = validBasicEvidence ? EXPECT_NOTHING : EXPECT_COMMITMENT_PROOF;
                            proofState = EXPECT_NOTHING;
                        }
                        else
                        {
                            proofState = EXPECT_CHECKPOINT;
                        }
                    }
                    else
                    {
                        proofState = EXPECT_CHECKPOINT;
                    }
                }
                else
                {
                    proofState = EXPECT_NOTHING;
                }
                break;
            }

            case EXPECT_COMMITMENT_PROOF:
            {
                if (proofComponent->objectType == CHAINOBJ_COMMITMENTDATA)
                {
                    std::vector<__uint128_t> checkCommitments;
                    blockTypes = ((CChainObject<CHashCommitments> *)proofComponent)->object.GetSmallCommitments(checkCommitments);

                    startingHeight = challengStartPoint.rootHeight;
                    int endHeight = lastNotarization.proofRoots[expectedNotarization.currencyID].rootHeight;
                    rangeLen = endHeight - startingHeight++;

                    commitmentRanges = CPBaaSNotarization::GetBlockCommitmentRanges(startingHeight, endHeight, entropyHash);

                    int checkIndex = 0;
                    bool mismatchIndex = false;
                    for (auto &oneRange : commitmentRanges)
                    {
                        for (uint32_t loop = oneRange.first; loop < oneRange.second; loop++, checkIndex++)
                        {
                            if (loop != ((uint32_t)checkCommitments[checkIndex]) >> 1)
                            {
                                mismatchIndex = true;
                                break;
                            }
                            priorNotarizationCommitments.insert(std::make_pair(loop, checkCommitments[checkIndex]));
                        }
                        if (mismatchIndex)
                        {
                            break;
                        }
                    }

                    if (!mismatchIndex &&
                        priorNotarizationCommitments.size() &&
                        challengeProofRoot.IsValid())
                    {
                        numHeaders = std::min(std::max(rangeLen, (int)CPBaaSNotarization::EXPECT_MIN_HEADER_PROOFS),
                                              std::min((int)CPBaaSNotarization::MAX_HEADER_PROOFS_PER_PROOF, rangeLen / 10));


                        proofState = EXPECT_HEADER_PROOFS;
                    }
                    else
                    {
                        proofState = EXPECT_NOTHING;
                    }
                }
                else
                {
                    proofState = EXPECT_NOTHING;
                }
                break;
            }
            case EXPECT_HEADER_PROOFS:
            {
                if ((proofComponent->objectType == CHAINOBJ_HEADER) && numHeaderProofs < numHeaders)
                {
                    int indexToProve = (UintToArith256(headerSelectionHash).GetLow64() % rangeLen);
                    headerSelectionHash = ::GetHash(headerSelectionHash);

                    // height is kept shifted up one from the PoS/PoW bit
                    uint32_t blockToProve = startingHeight + indexToProve;

                    CBlockHeader &blockHeader = ((CChainObject<CBlockHeaderAndProof> *)proofComponent)->object.blockHeader;
                    std::vector<uint32_t> oneBlockCommitment = priorNotarizationCommitments.count(blockToProve) ?
                        UnpackBlockCommitment(priorNotarizationCommitments[blockToProve]) :
                        std::vector<uint32_t>({blockHeader.nTime,
                                               blockHeader.nBits,
                                               blockHeader.GetVerusPOSTarget(),
                                               blockToProve << 1 | (uint32_t)blockHeader.IsVerusPOSBlock()});

                    if (((CChainObject<CBlockHeaderAndProof> *)proofComponent)->object.BlockNum() == blockToProve &&
                        blockHeader.nTime == oneBlockCommitment[0] &&
                        blockHeader.IsVerusPOSBlock() == (oneBlockCommitment[3] & 1) &&
                        ((oneBlockCommitment[3] & 1) ? blockHeader.GetVerusPOSTarget() == oneBlockCommitment[2] : blockHeader.nBits == oneBlockCommitment[1]) &&
                        ((CChainObject<CBlockHeaderAndProof> *)proofComponent)->object.ValidateBlockHash(blockHeader.GetHash(), blockToProve) ==
                            defaultProofRoot.stateRoot)
                    {
                        if (blockHeader.IsVerusPOSBlock())
                        {
                            posBlockHeaderAndProof = ((CChainObject<CBlockHeaderAndProof> *)proofComponent)->object;
                            posEntropyHeaders.clear();
                            // make sure we have all the expected objects in the header

                            // get proofs of the following from the PoS header, all proven by the MMR of the
                            // block just behind the PoS block:
                            // 1) proof of the source UTXO for the stake
                            // 2) the two header ref proofs from those used PoS entropy
                            //
                            std::vector<unsigned char> streamVch;
                            posBlockHeaderAndProof.blockHeader.GetExtraData(streamVch);
                            CDataStream ds(streamVch, SER_DISK, PROTOCOL_VERSION);

                            try
                            {
                                ds >> posSourceProof;
                                if (posSourceProof.IsValid())
                                {
                                    // and get the two entropy sources
                                    posEntropyHeaders.push_back(CBlockHeaderProof());
                                    ds >> posEntropyHeaders.back();
                                    if (posEntropyHeaders.back().IsValid())
                                    {
                                        posEntropyHeaders.push_back(CBlockHeaderProof());
                                        ds >> posEntropyHeaders.back();
                                    }
                                }
                                proofState = posEntropyHeaders.size() == 2 && posEntropyHeaders.back().IsValid() ? EXPECT_STAKE_TX : EXPECT_NOTHING;
                            }
                            catch(const std::exception& e)
                            {
                                std::cerr << e.what() << '\n';
                                proofState = EXPECT_NOTHING;
                            }
                        }
                        else
                        {
                            // TODO: HARDENING - verify that we match commitments
                            numHeaderProofs++;
                            proofState = EXPECT_HEADER_PROOFS;
                        }
                    }
                    else
                    {
                        // invalid proofs fail
                        proofState = EXPECT_NOTHING;
                    }
                    validChallengeEvidence = (numHeaderProofs >= numHeaders);
                }
                else
                {
                    proofState = EXPECT_NOTHING;
                }
                break;
            }
            case EXPECT_STAKE_TX:
            {
                if (proofComponent->objectType == CHAINOBJ_TRANSACTION_PROOF)
                {
                    // this should be a full transaction proof up to the merkle tree root
                    // of the block header of the actual PoS transaction in the block. along with the source output
                    // entropy blocks, and proof contained in the last PoS header, we can confirm both the
                    // validity of the PoS win from that source transaction, as well as the input script signature
                    // ensuring that it is truly a potentially valid proof of stake, assuming we can trust
                    // the blockchain >100 blocks behind us.

                    CTransaction outTx;
                    bool isPartial = false;
                    uint256 txMerkleRoot = ((CChainObject<CPartialTransactionProof> *)proofComponent)->object.CheckPartialTransaction(outTx, &isPartial);
                    bool stakeTxPassed = txMerkleRoot == posBlockHeaderAndProof.blockHeader.hashMerkleRoot &&
                                         ValidateStakeTransaction(outTx, stakeParams, false) &&
                                         stakeParams.prevHash == posBlockHeaderAndProof.blockHeader.hashPrevBlock &&
                                         stakeParams.srcHeight == posSourceProof.GetBlockHeight();

                    // Below, after we have gotten the stakeguard output and when we have any ID keys we may need, we can
                    // verify the spend signature.

                    if (stakeTxPassed)
                    {
                        // now, the proof we just got should be the stake transaction proven against the merkle root of our
                        // header, followed by proofs of any ID outputs that are needed to verify signatures, followed by
                        // the coinbase output proven against the header MMR root.

                        // with the output, the two header refs from the header proof and this stake transaction,
                        // we can now verify the PoS win against the PoS difficulty.
                        if (!externalCurrency.IsValid())
                        {
                            externalCurrency = ConnectedChains.GetCachedCurrency(expectedNotarization.currencyID);
                        }

                        arith_uint256 posTarget;
                        posTarget.SetCompact(posBlockHeaderAndProof.blockHeader.GetVerusPOSTarget());

                        uint256 rawPosHash = CBlockHeader::GetRawVerusPOSHash(
                            posBlockHeaderAndProof.blockHeader.nVersion,
                            CConstVerusSolutionVector::Version(posBlockHeaderAndProof.blockHeader.nSolution),
                            externalCurrency.MagicNumber(),
                            posBlockHeaderAndProof.blockHeader.nNonce,
                            ((CChainObject<CPartialTransactionProof> *)proofComponent)->object.GetBlockHeight(),
                            (!PBAAS_TESTMODE && evidence.systemID == VERUS_CHAINID));

                        if (externalCurrency.IsValid() && (UintToArith256(rawPosHash) / outTx.vout[0].nValue) > posTarget)
                        {
                            proofState = EXPECT_NOTHING;
                        }
                        else
                        {
                            proofState = EXPECT_ID_PROOF_OR_STAKEGUARD;
                        }
                    }
                    else
                    {
                        proofState = EXPECT_NOTHING;
                    }
                }
                else
                {
                    proofState = EXPECT_NOTHING;
                }
                break;
            }

            case EXPECT_ID_PROOF_OR_STAKEGUARD:
            {
                if (proofComponent->objectType == CHAINOBJ_TRANSACTION_PROOF)
                {
                    // this is either an ID proof, proven by the prior block's blockchain MMR root,
                    // or a proof of the coinbase stakeguard output, proven by the block MMR root of the current
                    // POS header.
                    CTransaction outTx;
                    bool isPartial = false;
                    bool foundOut = false;
                    CValidationState tmpState;
                    uint256 txMMRRoot = ((CChainObject<CPartialTransactionProof> *)proofComponent)->object.CheckPartialTransaction(outTx, &isPartial);
                    CProofRoot lastNotarizationRoot = lastNotarization.proofRoots[expectedNotarization.currencyID];

                    if (txMMRRoot == posBlockHeaderAndProof.blockHeader.GetBlockMMRRoot())
                    {
                        for (int j = 0; j < outTx.vout.size(); j++)
                        {
                            COptCCParams optP;
                            auto &oneOut = outTx.vout[j];
                            if (oneOut.nValue >= 0 &&
                                oneOut.scriptPubKey.IsPayToCryptoCondition(optP) &&
                                optP.IsValid() &&
                                optP.evalCode == EVAL_STAKEGUARD &&
                                RawPrecheckStakeGuardOutput(outTx, j, tmpState))
                            {
                                foundOut = true;
                                break;
                            }
                        }


                        // TODO: HARDENING - now verify the stake tx signature, even if it spends an ID


                        // after confirming stakeguard, we either have another header proof, or we're done
                        validChallengeEvidence = (++numHeaderProofs >= numHeaders);
                        proofState = EXPECT_HEADER_PROOFS;
                    }
                    else if (posBlockHeaderAndProof.BlockNum() > 0 &&
                             (posBlockHeaderAndProof.BlockNum() - ((CChainObject<CPartialTransactionProof> *)proofComponent)->object.GetBlockHeight()) >= VERUS_MIN_STAKEAGE &&
                             ((((CChainObject<CPartialTransactionProof> *)proofComponent)->object.GetBlockHeight() > lastNotarizationRoot.rootHeight &&
                               txMMRRoot == posBlockHeaderAndProof.blockHeader.GetPrevMMRRoot()) ||
                              txMMRRoot == lastNotarization.proofRoots[expectedNotarization.currencyID].stateRoot))
                    {
                        for (auto &oneOut : outTx.vout)
                        {
                            COptCCParams optP;
                            CIdentity oneID;
                            if (oneOut.nValue >= 0 &&
                                oneOut.scriptPubKey.IsPayToCryptoCondition(optP) &&
                                optP.IsValid() &&
                                optP.evalCode == EVAL_IDENTITY_PRIMARY &&
                                optP.vData.size() &&
                                (oneID = CIdentity(optP.vData[0])).IsValid())
                            {
                                stakeSpendingIDs.push_back(oneID);
                            }
                            else if (oneOut.nValue >= 0)
                            {
                                // this should only be proving an ID output, nothing else
                                proofState = EXPECT_NOTHING;
                                break;
                            }
                        }
                        // after confirming one ID proof, next should be an ID or the stakeguard
                        // we will leave it to be that unless we have set an error
                    }
                    else
                    {
                        proofState = EXPECT_NOTHING;
                    }
                }
                break;
            }

            default:
            {
                proofState = EXPECT_NOTHING;
                break;
            }
        }
        if (proofState == EXPECT_NOTHING || validChallengeEvidence)
        {
            break;
        }
    }

    // some incorrect evidence is on testnet, so let it pass if we are on VRSCTEST and before the
    // test fork time
    CPBaaSNotarization retVal((!validBasicEvidence || (challengeProofRoot.IsValid() && !validChallengeEvidence)) ?
                              CPBaaSNotarization() :
                              expectedNotarization);
    if (IsVerusActive() && !IsVerusMainnetActive() && !retVal.IsValid())
    {
        if (LogAcceptCategory("notarization"))
        {
            printf("Failed normal check with evidence: %s\nchallengeroot:%s\n", evidence.ToUniValue().write(1,2).c_str(), challengeProofRoot.ToUniValue().write(1,2).c_str());
            LogPrintf("Failed normal check with evidence: %s\nchallengeroot:%s\n", evidence.ToUniValue().write(1,2).c_str(), challengeProofRoot.ToUniValue().write(1,2).c_str());
        }
        auto notaRootIT = expectedNotarization.proofRoots.find(ASSETCHAINS_CHAINID);
        if (notaRootIT == expectedNotarization.proofRoots.end())
        {
            return expectedNotarization;
        }
        auto notaIndexIT = mapBlockIndex.find(notaRootIT->second.blockHash);
        if (notaIndexIT->second->nTime < PBAAS_TESTFORK_TIME)
        {
            return expectedNotarization;
        }
        else
        {
            return CPBaaSNotarization();
        }
    }
    return retVal;
}

// gets the last confirmed notarization for a particular currency confirmed on or before a particular height
// do not use for heights more than 10 blocks before the tip
std::tuple<uint32_t, CUTXORef, CPBaaSNotarization> GetLastConfirmedNotarization(uint160 curID, uint32_t height)
{
    std::vector<std::pair<uint32_t, CInputDescriptor>> unspentFinalizations;

    // prior confirmed must be on the chain before the notarization we are finalizing as confirmed
    // try the easy way first, which won't work if we are in an edge case, but otherwise will
    CTransaction lastTx, lastTx1;
    uint256 lastTxBlock;
    COptCCParams foundP;
    CObjectFinalization foundOf;
    CPBaaSNotarization foundNotarization;

    std::tuple<uint32_t, CUTXORef, CPBaaSNotarization> retVal({0, CUTXORef(), CPBaaSNotarization()});

    unspentFinalizations = CObjectFinalization::GetUnspentConfirmedFinalizations(curID);

    if (unspentFinalizations.size())
    {
        std::pair<uint32_t, CInputDescriptor> firstUnspentFinalization = unspentFinalizations[0];
        bool error = false;

        CObjectFinalization priorOf;

        while (!error && (firstUnspentFinalization.first == 0 || firstUnspentFinalization.first > height))
        {
            // get the transaction and go backwards to get the finalized notarization it spends
            CTransaction checkTx;
            COptCCParams checkP;
            CObjectFinalization checkOf;
            uint256 checkBlockHash;
            if (myGetTransaction(firstUnspentFinalization.second.txIn.prevout.hash, checkTx, checkBlockHash) &&
                checkTx.vout.size() > firstUnspentFinalization.second.txIn.prevout.n &&
                checkTx.vout[firstUnspentFinalization.second.txIn.prevout.n].scriptPubKey.IsPayToCryptoCondition(checkP) &&
                checkP.IsValid() &&
                ((checkP.evalCode == EVAL_FINALIZE_NOTARIZATION &&
                  (checkOf = CObjectFinalization(checkP.vData[0])).IsValid() &&
                  checkOf.IsConfirmed() &&
                  checkOf.currencyID == curID) ||
                 ((checkP.evalCode == EVAL_EARNEDNOTARIZATION || checkP.evalCode == EVAL_ACCEPTEDNOTARIZATION) &&
                  (foundNotarization = CPBaaSNotarization(checkP.vData[0])).IsValid() &&
                  foundNotarization.currencyID == curID)))
            {
                bool found = false;
                if (foundNotarization.IsValid())
                {
                    // if we found an actual notarization, it is inherently confirmed and after requested height, error
                    error = true;
                    foundNotarization = CPBaaSNotarization();
                    break;
                }
                // go backwards and look for the prior confirmed finalization
                for (auto &oneIn : checkTx.vin)
                {
                    CTransaction inTx;
                    uint256 inBlockHash;
                    if (myGetTransaction(oneIn.prevout.hash, inTx, inBlockHash) &&
                        inTx.vout.size() > oneIn.prevout.n &&
                        ((inTx.vout[oneIn.prevout.n].scriptPubKey.IsPayToCryptoCondition(checkP) &&
                          checkP.IsValid() &&
                          ((checkP.evalCode == EVAL_FINALIZE_NOTARIZATION &&
                            (priorOf = CObjectFinalization(checkP.vData[0])).IsValid() &&
                            priorOf.IsConfirmed() &&
                            priorOf.currencyID == curID) ||
                           ((checkP.evalCode == EVAL_EARNEDNOTARIZATION || checkP.evalCode == EVAL_ACCEPTEDNOTARIZATION) &&
                            (foundNotarization = CPBaaSNotarization(checkP.vData[0])).IsValid() &&
                            foundNotarization.currencyID == curID &&
                            (foundNotarization.IsBlockOneNotarization() || foundNotarization.IsPreLaunch()))))))
                    {
                        found = true;
                        firstUnspentFinalization.second = CInputDescriptor(inTx.vout[oneIn.prevout.n].scriptPubKey,
                                                                           inTx.vout[oneIn.prevout.n].nValue,
                                                                           oneIn);

                        if (inBlockHash.IsNull())
                        {
                            firstUnspentFinalization.first = 0;
                        }
                        else
                        {
                            auto blockIt = mapBlockIndex.find(inBlockHash);
                            if (blockIt == mapBlockIndex.end() ||
                                !chainActive.Contains(blockIt->second))
                            {
                                error = true;
                            }
                            firstUnspentFinalization.first = blockIt->second->GetHeight();
                            if (firstUnspentFinalization.first > height)
                            {
                                foundNotarization = CPBaaSNotarization();
                            }
                        }
                        break;
                    }
                }
                if (!found)
                {
                    error = true;
                    foundNotarization = CPBaaSNotarization();
                    break;
                }
            }
            else
            {
                foundNotarization = CPBaaSNotarization();
            }
        }
        if (!error &&
            firstUnspentFinalization.first > 0 &&
            firstUnspentFinalization.first <= height)
        {
            COptCCParams foundP;
            CTransaction finalTx;
            uint256 finalBlockHash;
            if (!priorOf.IsValid() && !foundNotarization.IsValid())
            {
                if (myGetTransaction(firstUnspentFinalization.second.txIn.prevout.hash, finalTx, finalBlockHash) &&
                    finalTx.vout[firstUnspentFinalization.second.txIn.prevout.n].scriptPubKey.IsPayToCryptoCondition(foundP) &&
                    foundP.IsValid() &&
                    foundP.evalCode == EVAL_FINALIZE_NOTARIZATION &&
                    (priorOf = CObjectFinalization(foundP.vData[0])).IsValid())
                {
                    if (priorOf.output.hash.IsNull())
                    {
                        priorOf.output.hash = finalTx.GetHash();
                    }
                }
            }
            if ((foundP.IsValid() || priorOf.IsValid()) && !foundNotarization.IsValid())
            {
                if (((priorOf.IsValid() &&
                      priorOf.output.GetOutputTransaction(finalTx, finalBlockHash) &&
                      finalTx.vout[priorOf.output.n].scriptPubKey.IsPayToCryptoCondition(foundP) &&
                      foundP.IsValid()) ||
                     foundP.IsValid()) &&
                    ((foundP.evalCode == EVAL_EARNEDNOTARIZATION || foundP.evalCode == EVAL_ACCEPTEDNOTARIZATION) &&
                     foundP.vData.size()))
                {
                    foundNotarization = CPBaaSNotarization(foundP.vData[0]);
                }
            }
            if (foundNotarization.IsValid())
            {
                retVal = {firstUnspentFinalization.first, firstUnspentFinalization.second.txIn.prevout, foundNotarization};
            }
        }
    }
    return retVal;
}

bool CPBaaSNotarization::CreateAcceptedNotarization(const CCurrencyDefinition &externalSystem,
                                                    const CPBaaSNotarization &earnedNotarization,
                                                    const CNotaryEvidence &notaryEvidence,
                                                    CValidationState &state,
                                                    TransactionBuilder &txBuilder)
{
    std::string errorPrefix(strprintf("%s: ", __func__));
    uint160 SystemID = externalSystem.GetID();

    const CCurrencyDefinition *pNotaryCurrency;
    if (IsVerusActive() || !ConnectedChains.notarySystems.count(SystemID))
    {
        pNotaryCurrency = &externalSystem;
    }
    else
    {
        pNotaryCurrency = &ConnectedChains.ThisChain();
    }

    std::set<uint160> notarySet = pNotaryCurrency->GetNotarySet();
    int minimumNotariesConfirm = pNotaryCurrency->MinimumNotariesConfirm();

    // now, verify the evidence. accepted notarizations for another system must have at least one
    // valid piece of evidence, which currently means at least one notary signature
    if (externalSystem.notarizationProtocol != externalSystem.NOTARIZATION_NOTARY_CHAINID && notaryEvidence.evidence.Empty())
    {
        return state.Error(errorPrefix + "Requires at least some evidence to accept notarization");
    }

    // create an accepted notarization based on the cross-chain notarization provided
    CPBaaSNotarization newNotarization = earnedNotarization;

    // this should be mirrored for us to continue, if it can't be, it is invalid
    if (earnedNotarization.IsMirror() || !newNotarization.SetMirror())
    {
        return state.Error(errorPrefix + "invalid earned notarization");
    }

    LOCK(cs_main);

    // now, we'll want to put our own proposer as the beneficiary of this notarization, if possible
    if (!VERUS_NOTARYID.IsNull())
    {
        newNotarization.proposer.SetAuxDest(DestinationToTransferDestination(VERUS_NOTARYID), 0);
    }
    else if (!GetArg("-mineraddress", "").empty())
    {
        newNotarization.proposer.SetAuxDest(DestinationToTransferDestination(DecodeDestination(GetArg("-mineraddress", ""))), 0);
    }
    else if (!NOTARY_PUBKEY.empty())
    {
        newNotarization.proposer.SetAuxDest(DestinationToTransferDestination(CPubKey(ParseHex(NOTARY_PUBKEY))), 0);
    }
    else if (!VERUS_DEFAULTID.IsNull())
    {
        newNotarization.proposer.SetAuxDest(DestinationToTransferDestination(VERUS_DEFAULTID), 0);
    }

    uint32_t height = chainActive.Height();
    CProofRoot ourRoot = newNotarization.proofRoots[ASSETCHAINS_CHAINID];

    CChainNotarizationData cnd;
    std::vector<std::pair<CTransaction, uint256>> txes;
    if (!GetNotarizationData(SystemID, cnd, &txes))
    {
        return state.Error(errorPrefix + "cannot locate notarization history");
    }

    // any notarization submitted must include a proof root of this chain that is later than the last confirmed
    // notarization
    uint32_t lastHeight;

    if (!cnd.IsConfirmed())
    {
        return state.Error(errorPrefix + "earned notarization proof root cannot be verified without at least one prior confirmed for this chain");
    }

    CPBaaSNotarization lastConfirmedNotarization = cnd.vtx[cnd.lastConfirmed].second;

    lastHeight = lastConfirmedNotarization.IsPreLaunch() ?
                            lastConfirmedNotarization.notarizationHeight :
                            lastConfirmedNotarization.proofRoots.count(ASSETCHAINS_CHAINID) ?
                                lastConfirmedNotarization.proofRoots.find(ASSETCHAINS_CHAINID)->second.rootHeight :
                                UINT32_MAX;

    // all notarization protocols in Verus do the heavy lifting on the Verus side
    // as a result, the hash is currently assumed to be the default
    CNativeHashWriter hw;
    std::vector<unsigned char> notarizationVec = ::AsVector(earnedNotarization);
    uint256 objHash = hw.write((const char *)&(notarizationVec[0]), notarizationVec.size()).GetHash();

    CNotaryEvidence::EStates confirmationState = notaryEvidence.CheckSignatureConfirmation(objHash, notarySet, minimumNotariesConfirm, height);
    if (confirmationState != CNotaryEvidence::EStates::STATE_CONFIRMED &&
        (pNotaryCurrency->notarizationProtocol != pNotaryCurrency->NOTARIZATION_AUTO ||
         pNotaryCurrency->proofProtocol != pNotaryCurrency->PROOF_PBAASMMR ||
         cnd.forks.size() <= cnd.bestChain ||
         cnd.forks[cnd.bestChain].size() <= ConnectedChains.ThisChain().MIN_EARNED_FOR_AUTO))
    {
        return state.Error(errorPrefix + "insufficient signature evidence to confirm notarization");
    }

    int forkIdx = -1, forkPos = -1;
    int priorNotarizationIdx = -1;

    CPBaaSNotarization notarizationToConfirm;

    // auto notarization PBaaS protocol operates more quickly when evidence is signed by
    // a quorum of notary witnesses, but can confirm with a preponderance of cryptographic
    // evidence and no signatures
    if (externalSystem.notarizationProtocol == externalSystem.NOTARIZATION_AUTO &&
        externalSystem.proofProtocol == externalSystem.PROOF_PBAASMMR)
    {
        std::set<int> evidenceTypes;
        evidenceTypes.insert(CHAINOBJ_PROOF_ROOT);
        evidenceTypes.insert(CHAINOBJ_TRANSACTION_PROOF);
        evidenceTypes.insert(CHAINOBJ_COMMITMENTDATA);
        evidenceTypes.insert(CHAINOBJ_HEADER);
        evidenceTypes.insert(CHAINOBJ_HEADER_REF);
        CCrossChainProof autoProof(notaryEvidence.GetSelectEvidence(evidenceTypes));

        // get last notarization evidence
        CTransaction outTx;
        if (!(autoProof.chainObjects.size() &&
              autoProof.chainObjects[0]->objectType == CHAINOBJ_TRANSACTION_PROOF &&
              newNotarization.proofRoots[SystemID].stateRoot ==
                ((CChainObject<CPartialTransactionProof> *)autoProof.chainObjects[0])->object.CheckPartialTransaction(outTx)))
        {
            return state.Error(errorPrefix + "insufficient or invalid cryptographic evidence to confirm notarization");
        }

        uint256 txHash = ((CChainObject<CPartialTransactionProof> *)autoProof.chainObjects[0])->object.TransactionHash();
        CPBaaSNotarization provenNotarization;
        CObjectFinalization foundOf;
        CAddressIndexDbEntry foundOutput;
        for (auto &oneOut : outTx.vout)
        {
            if (oneOut.nValue >= 0 &&
                (provenNotarization = CPBaaSNotarization(oneOut.scriptPubKey)).IsValid() &&
                provenNotarization.currencyID == ASSETCHAINS_CHAINID &&
                provenNotarization.FindEarnedNotarization(foundOf, &foundOutput))
            {
                break;
            }
            provenNotarization = CPBaaSNotarization();
        }

        if (!provenNotarization.IsValid())
        {
            return state.Error(errorPrefix + "insufficient or invalid cryptographic evidence to confirm notarization 2");
        }

        int forkIdx = -1;
        int forkPos = -1;
        int oneIdx = -1;
        for (int j = 0; j < cnd.vtx.size(); j++)
        {
            auto &oneNTx = cnd.vtx[j];
            if (oneNTx.first == CUTXORef(foundOutput.first.txhash, foundOutput.first.index))
            {
                for (int k = 0; k < cnd.forks.size(); k++)
                {
                    auto oneFork = cnd.forks[k];
                    for (int l = 0; l < oneFork.size(); l++)
                    {
                        oneIdx = oneFork[l];
                        if (oneIdx == j)
                        {
                            forkIdx = k;
                            forkPos = l;
                            break;
                        }
                    }
                    if (forkIdx != -1)
                    {
                        break;
                    }
                }
            }
            if (forkIdx != -1)
            {
                break;
            }
        }
        if (forkIdx == -1)
        {
            return state.Error(errorPrefix + "insufficient or invalid cryptographic evidence to confirm notarization 3");
        }

        // our new notarization establishes a proof of the prior that it agrees with, which is now found if forkIdx != -1
        // if so and forkPos > notary confirms required, we can finalize as confirmed the one at the necessary level of
        // confirmation depth. the number of confirms depends on whether or not this is signed.
        bool isAutoNotarized = confirmationState == CNotaryEvidence::EStates::STATE_CONFIRMED;
        int confirmsRequired = isAutoNotarized ? CPBaaSNotarization::MIN_EARNED_FOR_SIGNED : CPBaaSNotarization::MIN_EARNED_FOR_AUTO;
        int blockSpacing = isAutoNotarized ?
            pNotaryCurrency->GetMinBlocksToAutoNotarize() / confirmsRequired :
            pNotaryCurrency->GetMinBlocksToSignNotarize() / confirmsRequired;

        // we want to accept only enough notarizations to get us to a confirmed state
        // if we are signed for confirmation, allow the normal block notarization modulo as a frequency
        // otherwise, reduce the frequency by extending the length between
        //
        // first, determine if we will accept this at all,
        if (height - foundOutput.first.blockHeight < blockSpacing)
        {
            return state.Error(errorPrefix + "not enough blocks have passed to create accepted notarization yet");
        }

        // now determine if we can confirm any prior notarization
        // we will confirm with 1 less notarization than required for earned confirmation
        // that means a signed confirmation can confirm the notarization before it
        // an auto notarization confirmation can confirm the notarization 3 before it
        // any challenges to a notarization being confirmed must be addressed prior, and
        // any autonotarization that is unsigned must carry primary evidence
        // as if it was challenged.
        int indexToConfirm = -1;
        if (forkPos >= (confirmsRequired - 1))
        {
            // we need both the number of confirms and the number of blocks in order to confirm
            // we start knowing that we have enough confirms and go backwards until we run out of space or
            // have enough blocks between
        }



        // in auto-notarization, the first CPartialTransactionProof should be a proof
        // of the last notarization output that was confirmed on the other chain, proven by this new notarization

        // the proof must contain:
        // 1) A notarization merge mined or staked that references the last modulo period of blocks on this
        //    chain. If mined, it must have a merge mining entry with prior data matching this chain, and it must
        //    contain a proof of a prior staked notarization that it agrees with. If staked, it must
        //    contain a proof of a prior merge mined entry that it agrees with. Further, it must have a proof using
        //    the second notarization that further references either the last pending notarization, or the last
        //    confirmed notarization. If it references the last pending then the last pending will be
        //    be confirmed if this notarization is accepted.
        //
        // 2) The new pending notarization signed by all unrevoked notaries.
        //
        // Once verified, the prior pending notarization will be confirmed and all alternate notarizations
        // closed by being spent, and the new pending notarization that closed the last will be placed onto the
        // chain in a pending state.
        //

        // chain objects that should be present:
        //
        // proof root of the chain as of the most recent notarization
        // being mined in. That should contain a proof root of this chain
        // within the last period of BLOCK_NOTARIZATION_MODULO number
        // of blocks on this chain and be either an arguably legitimate
        // PoW or PoS header on the other chain.
        //
        // that first notarization should point to a second notarization mined or staked
        // in an alternate validation type to the first and a transaction output proof,
        // proven against the proof root of the first notarization.
        //
        // finally, that second notarization must then refer to and prove with its proof root,
        // a new notarization to enter as pending. the new notarization must include proof
        // of either the last confirmed or one of the last pending notarizations on this chain.
        //
        // each proof includes a power proof of the block before, committing to and enabling
        // determination of PoW or PoS for the block containing the earned notarization, based on
        // its change in work or stake from the block before. this enables a lighter weight
        // proof with a header commitment vs. requiring headers initially.

        //
        // An accepted notarization must be signed by an unrevoked majority of necessary notaries.
        //
        // Notarization evidence includes an asserted proof root
        //
        // A partial transaction proof of an earned notarization against the initial proof root.
        // A header reference proof of the block before the notarization block to enable determination
        // of validation method.
        //
        // A partial transaction proof of the second prior and agreed earned notarization
        // A header reference proof of the block before the second notarization block to enable determination
        // of validation method, which must be opposite to the validation method in the first block.
        //
        // A partial transaction proof of the third prior and agreed earned notarization, which is proposed
        // as the new pending notarization. In addition, it has a proof for a prior notarization, which
        // must be either the last confirmed on this chain or one of the last pending, which will then
        // become the last confirmed.
        //

        // get the confirmed notarization from the evidence








        if (cnd.forks.size() != 1)
        {
            return state.Error(errorPrefix + "witnesses cannot agree");
        }
        notarizationToConfirm = cnd.vtx[cnd.forks[0].back()].second;

        // notarizationToConfirm must match the last confirmed or one pending. if it matches one pending,
        // that is in our chain, but the last one is the one proven with evidence
        auto serializedVec = ::AsVector(notarizationToConfirm);
        if (serializedVec != ::AsVector(cnd.vtx[cnd.lastConfirmed].second))
        {
            // if the third notarization does not equal the last confirmed notarization, it
            // must equal a last pending notarization to confirm. that should always be a
            // non-confirmed entry in a fork of the notarization data, if it is present
            for (int i = 0; i < cnd.forks.size(); i++)
            {
                if (cnd.forks[i].size() > 1)
                {
                    for (int j = cnd.forks[i].size() - 1; j >= 0; j--)
                    {
                        if (serializedVec == ::AsVector(cnd.vtx[cnd.forks[i][j]].second))
                        {
                            forkIdx = i;
                            forkPos = j;
                            break;
                        }
                    }
                    if (forkIdx > -1)
                    {
                        break;
                    }
                }
            }

            // if it wasn't the confirmed entry and wasn't pending confirmation, it is referring to an
            // invalid predecessor and is therefore invalid
            if (forkIdx == -1)
            {
                return state.Error(errorPrefix + "invalid prior notarization - neither pending nor confirmed");
            }
        }

        // if we should confirm a notarization, it will be the first in forkIdx
        if (forkIdx > -1)
        {
            printf("ready to confirm notarization tx: %s, output %d\n", cnd.vtx[cnd.forks[forkIdx][forkPos]].first.hash.GetHex().c_str(), cnd.vtx[cnd.forks[forkIdx][forkPos]].first.n);
            priorNotarizationIdx = cnd.forks[forkIdx][forkPos];
        }
        else
        {
            priorNotarizationIdx = cnd.lastConfirmed;
        }
        // new notarization is entered as pending if we get here
    }
    else
    {
        if (cnd.forks.size() != 1)
        {
            LogPrint("notarization", "%s: insufficient cross chain proof for non-auto notarization\n");
            return state.Error(errorPrefix + "cannot resolve conflict without sufficient evidence");
        }
        priorNotarizationIdx = cnd.forks[0].back();
        notarizationToConfirm = cnd.vtx[priorNotarizationIdx].second;
        notarizationToConfirm = cnd.vtx[cnd.forks[0].size() > 1 ? cnd.forks[0][1] : cnd.lastConfirmed].second;
    }

    uint32_t priorNotarizationHeight = mapBlockIndex[txes[priorNotarizationIdx].second]->GetHeight();
    // only accept notarizations as seldom as we enforce between our earned notarizations
    if ((height - priorNotarizationHeight) <
         CPBaaSNotarization::GetAdjustedNotarizationModulo(ConnectedChains.ThisChain().blockNotarizationModulo,
                                                           mapBlockIndex[txes[cnd.lastConfirmed].second]->GetHeight()))
    {
        return state.Error(errorPrefix + "cannot create accepted notarization earlier than " +
                                            std::to_string(ConnectedChains.ThisChain().blockNotarizationModulo) +
                                            " blocks since the prior notarization it confirms");
    }

    // all core proof roots that are in a prior, agreed notarization, must be present in the new one,
    // and we must have moved far enough forward from any there that we agree with, or early out
    for (auto &oneProofRoot : notarizationToConfirm.proofRoots)
    {
        if (oneProofRoot.first == ASSETCHAINS_CHAINID)
        {
            if (!newNotarization.proofRoots.count(oneProofRoot.first) ||
                newNotarization.proofRoots[oneProofRoot.first].rootHeight <= oneProofRoot.second.rootHeight)
            {
                return state.Error(errorPrefix + "insufficient progress in this chain " + EncodeDestination(CIdentityID(oneProofRoot.first)) + " to accept notarization");
            }
        }
        else if (oneProofRoot.first == SystemID &&
                    (!newNotarization.proofRoots.count(oneProofRoot.first) ||
                     (newNotarization.proofRoots[oneProofRoot.first].rootHeight - oneProofRoot.second.rootHeight) <
                        (chainActive.LastTip()->nBits < PBAAS_TESTFORK_TIME ? 1 : externalSystem.blockNotarizationModulo)))
        {
            return state.Error(errorPrefix + "insufficient progress in chain " + EncodeDestination(CIdentityID(oneProofRoot.first)) + " to accept notarization");
        }
    }

    auto mmv = chainActive.GetMMV();
    mmv.resize(ourRoot.rootHeight + 1);

    // we only create accepted notarizations for notarizations that are earned for this chain on another system
    // currently, we support ethereum and PBaaS types.
    if (!newNotarization.proofRoots.count(SystemID) ||
        !newNotarization.proofRoots.count(ASSETCHAINS_CHAINID) ||
        !(ourRoot = newNotarization.proofRoots[ASSETCHAINS_CHAINID]).IsValid() ||
        ourRoot.rootHeight > height ||
        ourRoot.blockHash != chainActive[ourRoot.rootHeight]->GetBlockHash() ||
        ourRoot.stateRoot != mmv.GetRoot() ||
        (ourRoot.type != ourRoot.TYPE_PBAAS && ourRoot.type != ourRoot.TYPE_ETHEREUM))
    {
        return state.Error(errorPrefix + "can only create accepted notarization from notarization with valid proof root of this chain");
    }

    // ensure that the data present is valid, as of the height
    CCoinbaseCurrencyState oldCurState = ConnectedChains.GetCurrencyState(ASSETCHAINS_CHAINID, ourRoot.rootHeight);
    if (!oldCurState.IsValid() ||
        ::GetHash(oldCurState) != ::GetHash(earnedNotarization.currencyState))
    {
        return state.Error(errorPrefix + "currency state is invalid in accepted notarization. is:\n" +
                                            newNotarization.currencyState.ToUniValue().write(1,2) +
                                            "\nshould be:\n" +
                                            oldCurState.ToUniValue().write(1,2) + "\n");
    }

    // ensure that all locally provable info is valid as of our root height
    // and determine if the new notarization should be already finalized or not
    for (auto &oneCur : newNotarization.currencyStates)
    {
        if (oneCur.first == SystemID)
        {
            return state.Error(errorPrefix + "cannot accept redundant currency state in notarization for " + EncodeDestination(CIdentityID(SystemID)));
        }
        else if (oneCur.first != ASSETCHAINS_CHAINID)
        {
            // see if this currency is on our chain, and if so, it must be correct as of the proof root of this chain
            CCurrencyDefinition curDef = ConnectedChains.GetCachedCurrency(oneCur.first);
            // we must have all currencies
            if (!curDef.IsValid())
            {
                return state.Error(errorPrefix + "all currencies in accepted notarizatoin must be registered on this chain");
            }
            // if the currency is not from this chain, we cannot validate it
            if (curDef.systemID != ASSETCHAINS_CHAINID)
            {
                continue;
            }
            // ensure that the data present is valid, as of the height
            oldCurState = ConnectedChains.GetCurrencyState(oneCur.first, ourRoot.rootHeight);
            if (!oldCurState.IsValid() ||
                ::GetHash(oldCurState) != ::GetHash(oneCur.second))
            {
                return state.Error(errorPrefix + "currency state is invalid in accepted notarization. is:\n" +
                                                 oneCur.second.ToUniValue().write(1,2) +
                                                 "\nshould be:\n" +
                                                 oldCurState.ToUniValue().write(1,2) + "\n");
            }
        }
        else
        {
            // already checked above
            continue;
        }
    }

    for (auto &oneRoot : newNotarization.proofRoots)
    {
        if (oneRoot.first == SystemID)
        {
            continue;
        }
        else
        {
            // see if this currency is on our chain, and if so, it must be correct as of the proof root of this chain
            CCurrencyDefinition curDef = ConnectedChains.GetCachedCurrency(oneRoot.first);
            // we must have all currencies in this notarization registered
            if (!curDef.IsValid())
            {
                return state.Error(errorPrefix + "all currencies in accepted notarizatoin must be registered on this chain");
            }
            uint160 curDefID = curDef.GetID();

            // only check other currencies on this chain, not the main chain itself
            if (curDefID != ASSETCHAINS_CHAINID && curDef.systemID == ASSETCHAINS_CHAINID)
            {
                return state.Error(errorPrefix + "proof roots are not accepted for token currencies");
            }
        }
    }

    // now create the new notarization, add the proof, finalize if appropriate, and finish

    // add spend of prior notarization and then outputs
    CPBaaSNotarization lastUnspentNotarization;
    uint256 lastTxId;
    int32_t lastTxOutNum;
    CTransaction lastTx;
    if (!lastUnspentNotarization.GetLastUnspentNotarization(SystemID, lastTxId, lastTxOutNum, &lastTx) ||
        (lastUnspentNotarization.proofRoots[ASSETCHAINS_CHAINID] == newNotarization.proofRoots[ASSETCHAINS_CHAINID] ||
         lastUnspentNotarization.proofRoots[SystemID] == newNotarization.proofRoots[SystemID]))
    {
        return state.Error(errorPrefix + "invalid new notarization");
    }

    // add prior unspent accepted notarization as our input
    txBuilder.AddTransparentInput(CUTXORef(lastTxId, lastTxOutNum), lastTx.vout[lastTxOutNum].scriptPubKey, lastTx.vout[lastTxOutNum].nValue);

    {
        LOCK(mempool.cs);
        std::list<CTransaction> removed;
        // eliminate any conflict with ourselves or someone else doing the same thing
        mempool.removeConflicts(txBuilder.mtx, removed);
    }

    // if we are going to confirm a prior notarization, we also should spend its prior
    // pending finalizations and all conflicting as well
    CUTXORef newConfirmedOutput;

    std::map<CUTXORef, int> notarizationOutputMap;

    for (int i = 0; i < cnd.vtx.size(); i++)
    {
        notarizationOutputMap.insert(std::make_pair(cnd.vtx[i].first, i));
    }

    std::vector<std::vector<CInputDescriptor>> spendsToClose(cnd.vtx.size());
    std::vector<CInputDescriptor> extraSpends;

    // now, figure out which elements we spend, whether confirming or invalidating
    std::set<int> confirmedOutputNums;
    std::set<int> invalidatedOutputNums;
    bool setPriorConfirmed = true;

    // if we expect to confirm an index, check for additional proof required
    if (forkIdx > -1)
    {
        std::vector<std::vector<CNotaryEvidence>> evidenceVec;
        if (!cnd.CorrelatedFinalizationSpends(txes, spendsToClose, extraSpends, &evidenceVec))
        {
            return state.Error(errorPrefix + "error correlating spends");
        }

        // Check the evidence for the prior notarization that we indend to confirm,
        // if there is valid counter-evidence, do not confirm. Instead, reject and replace it with the new pending.
        std::vector<CProofRoot> validCounterRoots;
        CProofRoot challengeStartRoot(CProofRoot::TYPE_PBAAS, CProofRoot::VERSION_INVALID);
        for (auto &oneItem : evidenceVec[priorNotarizationIdx])
        {
            // valid counter evidence includes:
            // commitment to more powerful chain since the last confirmed notarization:
            //    1) must include:
            //       a) proof root of alternate to the notarization proof root that is more powerful
            //       b) proof of header of the alternate root height
            //       b) block commitments behind header proof that lead to more powerful chain
            //       c) proof of prior random headers based on current system root as of evidence tx submission
            if (oneItem.IsRejected())
            {
                if (cnd.vtx[priorNotarizationIdx].second.proofRoots.count(SystemID))
                {
                    CProofRoot challengeStart(CProofRoot::TYPE_PBAAS, CProofRoot::VERSION_INVALID);
                    bool invalidates = false;
                    CProofRoot counterRoot = IsValidChallengeEvidence(externalSystem,
                                                                      cnd.vtx[priorNotarizationIdx].second.proofRoots[SystemID],
                                                                      oneItem,
                                                                      txes[priorNotarizationIdx - 1].second,
                                                                      invalidates,
                                                                      challengeStart,
                                                                      height);
                    if (invalidates)
                    {
                        return state.Error(errorPrefix + "notarization has been proven invalid");
                    }
                    if (challengeStart.IsValid() && counterRoot.IsValid())
                    {
                        validCounterRoots.push_back(counterRoot);
                        if (!challengeStartRoot.IsValid() || challengeStart.rootHeight < challengeStartRoot.rootHeight)
                        {
                            challengeStartRoot = challengeStart;
                        }
                    }
                }
            }
        }


        // TODO: HARDENING - take evidence from first unanswered challengeroot across the range, up to the confirming proof
        // add height and whether or not challenge is answered to notarizationdata



        // we should always be getting proofs of block headers to verify prior commitments
        // ensure that we can refute any valid counter root claim
        // if we can't refute a counter-claim, we are done
        if (validCounterRoots.size())
        {
            for (auto &validCounterRoot : validCounterRoots)
            {
                uint256 entropyHash;
                if (!IsValidPrimaryChainEvidence(externalSystem,
                                                 notaryEvidence,
                                                 newNotarization,
                                                 mapBlockIndex[txes[cnd.lastConfirmed].second]->GetHeight(),
                                                 height,
                                                 &entropyHash,
                                                 validCounterRoot,
                                                 challengeStartRoot).IsValid() ||
                    (entropyHash != txes[priorNotarizationIdx - 1].second && !IsPreTestnetFork(txes[priorNotarizationIdx - 1].second)))
                {
                    return state.Error(errorPrefix + "insufficient proof to verify challenged notarization");
                }
            }
        }
        else
        {
            if (!IsValidPrimaryChainEvidence(externalSystem, notaryEvidence, newNotarization, mapBlockIndex[txes[cnd.lastConfirmed].second]->GetHeight(), height).IsValid())
            {
                return state.Error(errorPrefix + "insufficient proof to verify unchallenged notarization");
            }
        }

        if (!cnd.CalculateConfirmation(priorNotarizationIdx, confirmedOutputNums, invalidatedOutputNums))
        {
            return state.Error(errorPrefix + "error calculating confirmation");
        }

        // any fork that does not match the confirmed fork up to the same index will be
        // considered invalid from all of its entries to its end
        for (auto oneConfirmedIdx : confirmedOutputNums)
        {
            if (oneConfirmedIdx != priorNotarizationIdx)
            {
                for (auto &oneInput : spendsToClose[oneConfirmedIdx])
                {
                    txBuilder.AddTransparentInput(oneInput.txIn.prevout, oneInput.scriptPubKey, oneInput.nValue);
                }
            }
        }

        for (auto oneInvalidatedIdx : invalidatedOutputNums)
        {
            for (auto &oneInput : spendsToClose[oneInvalidatedIdx])
            {
                txBuilder.AddTransparentInput(oneInput.txIn.prevout, oneInput.scriptPubKey, oneInput.nValue);
            }
        }
    }

    CCcontract_info CC;
    CCcontract_info *cp;
    std::vector<CTxDestination> dests;

    // make the accepted notarization output
    cp = CCinit(&CC, EVAL_ACCEPTEDNOTARIZATION);

    if (externalSystem.notarizationProtocol == externalSystem.NOTARIZATION_NOTARY_CHAINID)
    {
        dests = std::vector<CTxDestination>({CIdentityID(externalSystem.GetID())});
    }
    else
    {
        dests = std::vector<CTxDestination>({CPubKey(ParseHex(CC.CChexstr))});
    }

    txBuilder.AddTransparentOutput(MakeMofNCCScript(CConditionObj<CPBaaSNotarization>(EVAL_ACCEPTEDNOTARIZATION, dests, 1, &newNotarization)), 0);

    // now add the notary evidence and finalization that uses it to assert validity
    // make the evidence notarization output
    cp = CCinit(&CC, EVAL_NOTARY_EVIDENCE);
    dests = std::vector<CTxDestination>({CPubKey(ParseHex(CC.CChexstr))});

    std::vector<int32_t> evidenceOuts;

    COptCCParams chkP;
    if (!MakeMofNCCScript(CConditionObj<CNotaryEvidence>(EVAL_NOTARY_EVIDENCE, dests, 1, &notaryEvidence)).IsPayToCryptoCondition(chkP))
    {
        LogPrintf("%s: failed to package evidence script from system %s\n", __func__, EncodeDestination(CIdentityID(SystemID)).c_str());
        return false;
    }

    // the value should be considered for reduction
    if (chkP.AsVector().size() >= CScript::MAX_SCRIPT_ELEMENT_SIZE)
    {
        CNotaryEvidence emptyEvidence;
        int baseOverhead = MakeMofNCCScript(CConditionObj<CNotaryEvidence>(EVAL_NOTARY_EVIDENCE, dests, 1, &emptyEvidence)).size() + 128;
        auto evidenceVec = notaryEvidence.BreakApart(CScript::MAX_SCRIPT_ELEMENT_SIZE - baseOverhead);
        if (!evidenceVec.size())
        {
            LogPrintf("%s: failed to package evidence from system %s\n", __func__, EncodeDestination(CIdentityID(SystemID)).c_str());
            return false;
        }
        for (auto &oneProof : evidenceVec)
        {
            dests = std::vector<CTxDestination>({CPubKey(ParseHex(CC.CChexstr))});
            evidenceOuts.push_back(txBuilder.mtx.vout.size());
            txBuilder.AddTransparentOutput(MakeMofNCCScript(CConditionObj<CNotaryEvidence>(EVAL_NOTARY_EVIDENCE, dests, 1, &oneProof)), 0);
        }
    }
    else
    {
        evidenceOuts.push_back(txBuilder.mtx.vout.size());
        txBuilder.AddTransparentOutput(MakeMofNCCScript(CConditionObj<CNotaryEvidence>(EVAL_NOTARY_EVIDENCE, dests, 1, &notaryEvidence)), 0);
    }

    // now make 1 or two finalization outputs
    cp = CCinit(&CC, EVAL_FINALIZE_NOTARIZATION);
    dests = std::vector<CTxDestination>({CPubKey(ParseHex(CC.CChexstr))});

    // create the pending finalization for our new notarization first
    CObjectFinalization of = CObjectFinalization(CObjectFinalization::FINALIZE_NOTARIZATION, newNotarization.currencyID, uint256(), txBuilder.mtx.vout.size() - (1 + evidenceOuts.size()), height + 1);
    of.evidenceOutputs = evidenceOuts;
    txBuilder.AddTransparentOutput(MakeMofNCCScript(CConditionObj<CObjectFinalization>(EVAL_FINALIZE_NOTARIZATION, dests, 1, &of)), 0);

    // if there are outputs to close for this entry, it will first be
    // a finalization, then evidence
    COptCCParams fP;
    of = CObjectFinalization();

    for (auto &oneInput : spendsToClose[priorNotarizationIdx])
    {
        CObjectFinalization tmpOf;
        if (oneInput.scriptPubKey.IsPayToCryptoCondition(fP) &&
            fP.IsValid() &&
            fP.evalCode == EVAL_FINALIZE_NOTARIZATION &&
            fP.vData.size() &&
            (tmpOf = CObjectFinalization(fP.vData[0])).IsValid())
        {
            txBuilder.AddTransparentInput(oneInput.txIn.prevout, oneInput.scriptPubKey, oneInput.nValue);
            if (!of.IsValid())
            {
                of = tmpOf;
                if (of.output.hash.IsNull())
                {
                    of.output.hash = oneInput.txIn.prevout.hash;
                }
                of.evidenceInputs.clear();
                of.evidenceOutputs.clear();
                of.minFinalizationHeight = height;
            }
            // inputs will be finalization with its evidence
            // outputs are open to carry it forward, as they are on inputs
            of.evidenceInputs.push_back((int)txBuilder.mtx.vin.size() - 1);
        }
    }
    // add extra spends
    if (of.IsValid())
    {
        for (auto &oneInput : spendsToClose[priorNotarizationIdx])
        {
            CObjectFinalization tmpOf;
            if (!(oneInput.scriptPubKey.IsPayToCryptoCondition(fP) &&
                fP.IsValid() &&
                fP.evalCode == EVAL_FINALIZE_NOTARIZATION &&
                fP.vData.size() &&
                (tmpOf = CObjectFinalization(fP.vData[0])).IsValid()))
            {
                txBuilder.AddTransparentInput(oneInput.txIn.prevout, oneInput.scriptPubKey, oneInput.nValue);
            }
        }
    }
    else
    {
        // we have no valid finalization to close for this list of spends
        // spend whatever we have and make an output finalization
        of = CObjectFinalization(CObjectFinalization::FINALIZE_NOTARIZATION,
                                    newNotarization.currencyID,
                                    cnd.vtx[priorNotarizationIdx].first.hash,
                                    cnd.vtx[priorNotarizationIdx].first.n,
                                    height);
    }
    if (setPriorConfirmed)
    {
        of.SetConfirmed();
    }
    else
    {
        of.SetRejected();
    }
    txBuilder.AddTransparentOutput(MakeMofNCCScript(CConditionObj<CObjectFinalization>(EVAL_FINALIZE_NOTARIZATION, dests, 1, &of)), 0);

    /* UniValue univTx(UniValue::VOBJ);
    TxToUniv(txBuilder.mtx, uint256(), univTx);
    printf("%s: txBuilder returning:\n%s\n", __func__, univTx.write(1,2).c_str());
    // */

    return true;
}

extern void CopyNodeStats(std::vector<CNodeStats>& vstats);

std::vector<CNodeData> GetGoodNodes(int maxNum)
{
    // good nodes ordered by time connected
    std::map<int64_t, CNode *> goodNodes;
    std::vector<CNodeData> retVal;

    LOCK(cs_vNodes);
    for (auto oneNode : vNodes)
    {
        if (!oneNode->fInbound)
        {
            if (oneNode->fWhitelisted)
            {
                goodNodes.insert(std::make_pair(oneNode->nTimeConnected, oneNode));
            }
            else if (oneNode->GetTotalBytesRecv() > (1024 * 1024))
            {
                goodNodes.insert(std::make_pair(oneNode->nTimeConnected, oneNode));
            }
        }
    }
    if (goodNodes.size() > 1)
    {
        seed_insecure_rand();
        for (auto &oneNode : goodNodes)
        {
            if (insecure_rand() & 1)
            {
                retVal.push_back(CNodeData(oneNode.second->addr.ToStringIPPort(), oneNode.second->hashPaymentAddress));
                if (retVal.size() >= maxNum)
                {
                    break;
                }
            }
        }
        if (!retVal.size())
        {
            retVal.push_back(CNodeData(goodNodes.begin()->second->addr.ToStringIPPort(), goodNodes.begin()->second->hashPaymentAddress));
        }
    }
    else if (goodNodes.size())
    {
        retVal.push_back(CNodeData(goodNodes.begin()->second->addr.ToStringIPPort(), goodNodes.begin()->second->hashPaymentAddress));
    }
    return retVal;
}

std::vector<std::pair<uint32_t, uint32_t>>
CPBaaSNotarization::GetBlockCommitmentRanges(uint32_t lastNotarizationHeight, uint32_t currentNotarizationHeight, uint256 entropy)
{
    std::vector<std::pair<uint32_t, uint32_t>> retVal = std::vector<std::pair<uint32_t, uint32_t>>({{std::make_pair(lastNotarizationHeight, currentNotarizationHeight)}});

    int priorHeight = std::max((int)lastNotarizationHeight - NUM_COMMITMENT_BLOCKS_START_OFFSET, 1);

    // if our range is larger than 256, break it into up to 5, 256 block ranges and
    // spread them randomly over the target proof space
    if ((currentNotarizationHeight - priorHeight) > MAX_BLOCKS_PER_COMMITMENT_RANGE)
    {
        int totalRange = currentNotarizationHeight - priorHeight;
        int rangeLeft = totalRange;
        int numBlockRanges = std::max(std::min((int)((rangeLeft >> 1) / MAX_BLOCKS_PER_COMMITMENT_RANGE), (int)MAX_BLOCK_RANGES_PER_PROOF), 1);
        int blocksPerSection = (currentNotarizationHeight - priorHeight) / numBlockRanges;

        CNativeHashWriter hw;
        hw << RangeSelectEntropyKey();
        hw << entropy;
        entropy = hw.GetHash();

        // take off blocks per section at a time until we have no more whole blocks per section left
        while (rangeLeft)
        {
            int blocksThisSection = blocksPerSection;
            rangeLeft -= blocksPerSection;
            if (rangeLeft < blocksPerSection)
            {
                blocksThisSection += rangeLeft;
                rangeLeft = 0;
            }
            // we need room for the range we will prove
            blocksThisSection -= std::min(totalRange, (int)MAX_BLOCKS_PER_COMMITMENT_RANGE);

            uint32_t rangeStart = (UintToArith256(entropy).GetLow64() % blocksThisSection);
            retVal.push_back(std::make_pair(rangeStart, rangeStart + MAX_BLOCKS_PER_COMMITMENT_RANGE));
        }
    }
    return retVal;
}

std::vector<__uint128_t> GetBlockCommitments(uint32_t lastNotarizationHeight, uint32_t currentNotarizationHeight, const uint256 &entropy)
{
    std::vector<__uint128_t> blockCommitmentsSmall;
    if (chainActive.Height() >= currentNotarizationHeight)
    {
        auto blockRanges = CPBaaSNotarization::GetBlockCommitmentRanges(lastNotarizationHeight, currentNotarizationHeight, entropy);
        // commit to prior blocks nTime, nBits, stakeBits, & work or stake power component for up to 256 blocks prior or back,
        // to the last notarization, whichever comes first, enabling later random verification of subset
        auto rangeOffset = blockCommitmentsSmall.size();
        for (auto &oneRange : blockRanges)
        {
            int currentOffset = oneRange.second - oneRange.first;
            blockCommitmentsSmall.resize(blockCommitmentsSmall.size() + currentOffset--);

            uint32_t blockNum = oneRange.second - 1;
            for (; blockNum >= oneRange.first; blockNum--, currentOffset--)
            {
                bool isPosBlock = chainActive[blockNum]->IsVerusPOSBlock();
                __uint128_t bigCommitmentNum((uint32_t)chainActive[blockNum]->nTime);
                bigCommitmentNum = (bigCommitmentNum << 32) | (uint32_t)chainActive[blockNum]->nBits;
                bigCommitmentNum = (bigCommitmentNum << 32) | (uint32_t)chainActive[blockNum]->GetVerusPOSTarget();
                bigCommitmentNum = (bigCommitmentNum << 32) | ((uint32_t)(blockNum << 1) | (uint32_t)isPosBlock);
                blockCommitmentsSmall[currentOffset] = bigCommitmentNum;
            }
        }
    }
    return blockCommitmentsSmall;
}

bool ProvePosBlock(uint32_t startingHeight, const CBlockIndex *pindex, CNotaryEvidence &evidence, CValidationState &state)
{
    // if this is a PoS block, add the entire spending transaction && a coinbase stakeguard output, which
    // will enable full PoS and signature verification, preventing any form of "fake stake" attack that
    // might exaggerate power of an attacking chain
    CBlock posBlock;

    if (!ReadBlockFromDisk(posBlock, pindex, Params().GetConsensus(), false))
    {
        LogPrintf("%s: ERROR: could not read PoS block from disk, LIKELY DUE TO CORRUPT LOCAL STATE, BOOTSTRAP OR RESYNC RECOMMENDED\n", __func__);
        return state.Error("invalid crosschain notarization data");
    }

    // now prove:
    //
    // 1) the entire last transaction in the block - proven by the block Merkle Tree, available in the proven header
    evidence.evidence << posBlock.GetPartialTransactionProof(posBlock.vtx.back(), posBlock.vtx.size() - 1);

    // 2) any ID outputs from the past needed to verify keys for a signature, and
    COptCCParams stakeOutP;
    std::map<uint160, std::tuple<CIdentity, CTxIn, uint32_t>> idsToProve;
    std::set<uint160> idsWithAddressInSig;
    if (posBlock.vtx.back().vout[0].scriptPubKey.IsPayToCryptoCondition(stakeOutP) && stakeOutP.IsValid())
    {
        for (auto &oneDest : stakeOutP.vKeys)
        {
            if (oneDest.which() == COptCCParams::ADDRTYPE_ID)
            {
                uint32_t idHeight = 0;
                CTxIn idTxIn;
                CIdentity lookupID = CIdentity::LookupIdentity(GetDestinationID(oneDest), pindex->GetHeight() - 1, &idHeight, &idTxIn);
                if (!lookupID.IsValid())
                {
                    return state.Error("invalid identity lookup, LIKELY DUE TO CORRUPT LOCAL STATE, BOOTSTRAP OR RESYNC RECOMMENDED");
                }
                idsToProve.insert(std::make_pair(GetDestinationID(oneDest), std::make_tuple(lookupID, idTxIn, idHeight)));
            }
        }
        CSmartTransactionSignatures smartSigs;
        std::vector<unsigned char> ffVec = GetFulfillmentVector(posBlock.vtx.back().vin[0].scriptSig);
        if (ffVec.size() && (smartSigs = CSmartTransactionSignatures(std::vector<unsigned char>(ffVec.begin(), ffVec.end()))).IsValid())
        {
            // determine if any ID lookups are needed to verify the signature, and if so, prove the necessary ID outputs
            for (auto &oneID : idsToProve)
            {
                for (auto &oneAddr : std::get<0>(oneID.second).primaryAddresses)
                {
                    uint160 addrId = GetDestinationID(oneAddr);
                    if (smartSigs.signatures.count(addrId))
                    {
                        idsWithAddressInSig.insert(oneID.first);
                    }
                }
            }
        }
        // prove IDs
        for (auto &oneIDToProve : idsWithAddressInSig)
        {
            std::tuple<CIdentity, CTxIn, uint32_t> &idTuple = idsToProve[oneIDToProve];
            CTransaction idTx;
            uint256 blockHash;
            if (!myGetTransaction(std::get<1>(idTuple).prevout.hash, idTx, blockHash) ||
                !mapBlockIndex.count(blockHash) ||
                !chainActive.Contains(mapBlockIndex[blockHash]))
            {
                return state.Error("invalid ID transaction for stake - cannot make proof");
            }
            CBlockIndex *pIDBlockIdx = mapBlockIndex[blockHash];

            // if the ID was modified before the starting height, prove it at the starting height
            // to connect the evidence to prior parts of the chain
            evidence.evidence << CPartialTransactionProof(idTx,
                                                          std::vector<int>(),
                                                          std::vector<int>({(int)std::get<1>(idTuple).prevout.n}),
                                                          pIDBlockIdx,
                                                          startingHeight != 1 && pIDBlockIdx->GetHeight() <= startingHeight ?
                                                            startingHeight :
                                                            pindex->GetHeight() - 1);
        }
    }
    else
    {
        return state.Error("invalid stake transaction - cannot make proof");
    }

    // 3) one stakeguard output from the coinbase
    int cbOutNum;
    for (cbOutNum = 0; cbOutNum < posBlock.vtx[0].vout.size(); cbOutNum++)
    {
        const CTxOut &cbOut = posBlock.vtx[0].vout[cbOutNum];
        COptCCParams cboP;
        if (cbOut.scriptPubKey.IsPayToCryptoCondition(cboP) &&
            cboP.IsValid() &&
            cboP.evalCode == EVAL_STAKEGUARD)
        {
            break;
        }
    }
    if (cbOutNum >= posBlock.vtx[0].vout.size())
    {
        return state.Error("invalid proof of stake coinbase at block height " + std::to_string(pindex->GetHeight()));
    }

    evidence.evidence <<
        posBlock.GetPartialTransactionProof(posBlock.vtx[0],
                                            0,
                                            std::vector<std::pair<int16_t, int16_t>>(
                                                {{(int16_t)CTransactionHeader::TX_HEADER, (int16_t)0},
                                                    {(int16_t)CTransactionHeader::TX_OUTPUT, (int16_t)cbOutNum}}));
    return true;
}

// create a notarization that is validated as part of the block, statistically benefiting the miner or staker if the
// cross notarization is valid
bool CPBaaSNotarization::CreateEarnedNotarization(const CRPCChainData &externalSystem,
                                                  const CTransferDestination &Proposer,
                                                  bool isStake,
                                                  CValidationState &state,
                                                  std::vector<CTxOut> &txOutputs,
                                                  CPBaaSNotarization &notarization)
{
    std::string errorPrefix(strprintf("%s: ", __func__));

    uint32_t height;
    uint160 SystemID;
    const CCurrencyDefinition &systemDef = externalSystem.chainDefinition;
    SystemID = systemDef.GetID();

    CChainNotarizationData cnd;
    std::vector<std::pair<CTransaction, uint256>> txes;
    std::vector<std::vector<std::tuple<CObjectFinalization, CNotaryEvidence, CProofRoot, CProofRoot>>> localCounterEvidence;

    {
        LOCK2(cs_main, mempool.cs);
        height = chainActive.Height();

        // we can only create an earned notarization for a notary chain, so there must be a notary chain and a network connection to it
        // we also need to ensure that our notarization would be the first notarization in this notary block period  with which we agree.
        if (!externalSystem.IsValid() || externalSystem.rpcHost.empty())
        {
            // technically not a real error
            return state.Error("no-notary");
        }

        if (!GetNotarizationData(SystemID, cnd, &txes, &localCounterEvidence) || !cnd.IsConfirmed())
        {
            return state.Error(errorPrefix + "no prior confirmed notarization found");
        }
    }

    bool isGatewayFirstContact = systemDef.IsGateway() && cnd.vtx[cnd.lastConfirmed].second.IsDefinitionNotarization();

    // we really want the system proof roots for each notarization to make the JSON for the API smaller
    UniValue proofRootsUni(UniValue::VARR);
    for (auto &oneNot : cnd.vtx)
    {
        auto rootIt = oneNot.second.proofRoots.find(SystemID);
        if (rootIt != oneNot.second.proofRoots.end())
        {
            proofRootsUni.push_back(rootIt->second.ToUniValue());
        }
    }
    if (!proofRootsUni.size() && !isGatewayFirstContact)
    {
        return state.Error(errorPrefix + "no valid prior state root found");
    }

    // call notary to determine the prior notarization that we agree with
    UniValue params(UniValue::VARR);
    UniValue oneParam(UniValue::VOBJ);
    oneParam.push_back(Pair("proofroots", proofRootsUni));
    if (!isGatewayFirstContact && proofRootsUni.size())
    {
        oneParam.push_back(Pair("lastconfirmed", cnd.lastConfirmed));
    }
    params.push_back(oneParam);

    bool logNotarization = LogAcceptCategory("earnednotarizations");

    UniValue bestProofRootResult;
    try
    {
        if (logNotarization)
        {
            LogPrintf("calling getbestproofroot with params: %s\n", params.write(1,2).c_str());
            printf("calling getbestproofroot with params: %s\n", params.write(1,2).c_str());
        }
        bestProofRootResult = find_value(RPCCallRoot("getbestproofroot", params), "result");
        if (logNotarization)
        {
            LogPrintf("result from getbestproofroot: %s\n", bestProofRootResult.write(1,2).c_str());
            printf("result from getbestproofroot: %s\n", bestProofRootResult.write(1,2).c_str());
        }
    } catch (exception e)
    {
        if (logNotarization)
        {
            LogPrintf("exception from getbestproofroot: %s\n", e.what());
        }
        bestProofRootResult = NullUniValue;
    }

    UniValue notarizationResult;
    params = UniValue(UniValue::VARR);
    params.push_back(EncodeDestination(CIdentityID(ASSETCHAINS_CHAINID)));
    try
    {
        notarizationResult = find_value(RPCCallRoot("getnotarizationdata", params), "result");
        if (logNotarization)
        {
            LogPrintf("result from getnotarizationdata: %s\n", notarizationResult.write(1,2).c_str());
        }
    } catch (exception e)
    {
        if (logNotarization)
        {
            LogPrintf("exception from getnotarizationdata: %s\n", e.what());
        }
        notarizationResult = NullUniValue;
    }

    CChainNotarizationData crosschainCND;
    std::vector<uint256> blockHashes;
    std::vector<std::vector<std::tuple<CNotaryEvidence, CProofRoot, CProofRoot>>> crossChainCounterEvidence;

    if (notarizationResult.isNull() ||
        !(crosschainCND = CChainNotarizationData(notarizationResult, true, &blockHashes, &crossChainCounterEvidence)).IsValid() ||
        (!externalSystem.chainDefinition.IsGateway() && !crosschainCND.IsConfirmed()))
    {
        LogPrint("notarization", "Unable to get notarization data from %s\n", EncodeDestination(CIdentityID(externalSystem.GetID())).c_str());
        return state.Error("invalid crosschain notarization data");
    }

    // look for the following things to challenge:
    // 1) pending notarizations on the notary chain that we disagree with
    // 2) pending notarizations that agree with a prior notarization in a different notarization fork
    // 3) local notarizations that disagree with the notary chain we see
    //
    // if we find any of the above, we first look to see if they have counter-evidence already.
    // if not, post counter evidence and spend the last pending finalization in doing so to prevent
    // duplicates.
    //
    // we could combine the posting of challenges and submitting an accepted notarization if we
    // determine we will submit. this would be an optimization, but is not critical for function.
    //
    // once we determine that we have something to do, take the following action for each case:
    //
    // 1) submit a challenge with supporting evidence from this chain. if the chain is longer than ours,
    //    attempt to connect to the published nodes
    // 2) create a fraud proof and finalize the earliest notarization of each fork as rejected, rejecting
    //    any dependents automatically
    // 3) get a challenge from the notary chain for the specified range and with the specified
    //    entropy hash, then submit it to this chain with our own notarization or independently
    //    if we don't qualify to notarize.
    //

    UniValue validIndexesUni = find_value(bestProofRootResult, "validindexes");

    // now, we have the index for the transaction and notarization we agree with, a list of those we consider invalid,
    // and the most recent notarization to use when creating the new one
    //
    // we need to potentially make transactions for the challenges or add them to notarizations
    //
    UniValue validIndexes = find_value(bestProofRootResult, "validindexes");
    if (!isGatewayFirstContact && (!validIndexesUni.isArray() || !validIndexesUni.size()))
    {
        return state.Error("no-valid-proofroots");
    }
    std::set<int> validIndexSet;
    for (int i = 0; i < validIndexesUni.size(); i++)
    {
        validIndexSet.insert(uni_get_int(validIndexesUni[0], INT32_MAX));
        if (!(cnd.vtx.size() > *validIndexSet.rbegin() && *validIndexSet.rbegin() >= 0))
        {
            return state.Error("invalid-validindex-returned");
        }
    }

    if (externalSystem.chainDefinition.IsPBaaSChain())
    {
        // all challenges we make will require requesting challenge data from the notary chain
        // so we batch them up for one call
        UniValue validityChallenges(UniValue::VARR);
        if (validIndexesUni.isArray() && validIndexesUni.size())
        {
            UniValue challengeRequests(UniValue::VARR);
            std::vector<std::vector<int>> unchallengedForks = cnd.forks;
            std::set<int> validIndexes;
            for (int i = 0; i < validIndexesUni.size(); i++)
            {
                validIndexes.insert(uni_get_int(validIndexesUni[i], -1));
            }
            if (validIndexes.count(-1))
            {
                return state.Error("invalid-validindexes-from-getbestproofroot");
            }
            for (int i = 1; i < cnd.vtx.size(); i++)
            {
                auto proofRootIT = cnd.vtx[i].second.proofRoots.find(SystemID);
                if (proofRootIT == cnd.vtx[i].second.proofRoots.end())
                {
                    continue;
                }

                // find this entry's first location, k, in fork j
                int j, k;
                for (j = 0; j < unchallengedForks.size(); j++)
                {
                    for (k = 0; k < unchallengedForks[j].size(); k++)
                    {
                        if (unchallengedForks[j][k] == i)
                        {
                            break;
                        }
                    }
                    if (k < unchallengedForks[j].size())
                    {
                        break;
                    }
                }

                // if we didn't find this entry in the pruned forks, nothing to do
                if (j >= unchallengedForks.size())
                {
                    continue;
                }

                // if valid, use it to eliminate remaining forks and collect remaining challenges
                if (validIndexes.count(i))
                {
                    // we found the first valid fork with this entry
                    // all forks that do not have the same value in the k'th entry
                    // should be challenged, either because we don't agree,
                    // or because they skipped this valid one and did not reference it
                    // if their entry is not the same, but valid, they are provably
                    // rejectable, otherwise just to be challenged
                    for (int forkToChallenge = 0; forkToChallenge < unchallengedForks.size(); forkToChallenge++)
                    {
                        if (forkToChallenge == j)
                        {
                            continue;
                        }
                        if (unchallengedForks[forkToChallenge].size() > k &&
                            unchallengedForks[forkToChallenge][k] != unchallengedForks[j][k])
                        {
                            for (int challengeNum = k; challengeNum < unchallengedForks[forkToChallenge].size(); challengeNum++)
                            {
                                // is there already counter evidence for this entry?
                                // 1) if there is a fraud proof, prune and move on
                                // 2) if there is a validity challenge, only add one if we intend to
                                //    expand its range
                                bool moveToNext = false;
                                for (auto &oneChallenge : localCounterEvidence[unchallengedForks[forkToChallenge][k]])
                                {
                                    if (!std::get<1>(oneChallenge).evidence.chainObjects.size() ||
                                        std::get<1>(oneChallenge).evidence.chainObjects[0]->objectType != CHAINOBJ_PROOF_ROOT)
                                    {
                                        continue;
                                    }
                                    // if the challenge root is the same as the root being challenged, it is a skip challenge
                                    // and would not be on-chain or in mempool if not valid. we can ignore the rest of this fork.
                                    if (((CChainObject<CProofRoot> *)std::get<1>(oneChallenge).evidence.chainObjects[0])->object ==
                                        cnd.vtx[unchallengedForks[forkToChallenge][k]].second.proofRoots[SystemID])
                                    {
                                        unchallengedForks[forkToChallenge].erase(unchallengedForks[forkToChallenge].begin() + challengeNum, unchallengedForks[forkToChallenge].end());
                                        continue;
                                    }
                                    // TODO: POST HARDENING - right now, we actually don't break down ranges to a lower level of
                                    // granularity, based on checkpoints, so until we do, just consider this one covered and move
                                    // on to the next
                                    moveToNext = true;
                                    break;
                                }
                                if (moveToNext)
                                {
                                    continue;
                                }

                                // if it's valid, then it skipped a valid notarization and can be proven false
                                if (validIndexes.count(unchallengedForks[forkToChallenge][k]))
                                {
                                    // prove the root on the j fork with this root and submit the proof along with a rejection
                                    // finalization
                                    CNotaryEvidence fraudProof;
                                    fraudProof.output = cnd.vtx[unchallengedForks[forkToChallenge][k]].first;
                                    fraudProof.state = CNotaryEvidence::STATE_REJECTED;

                                    // create evidence in this order
                                    // EXPECT_ALTERNATE_PROOF_ROOT  // give it the same root as it has, indicating fraud proof
                                    fraudProof.evidence << cnd.vtx[unchallengedForks[forkToChallenge][k]].second.proofRoots[SystemID];

                                    // EXPECT_SKIPPED_NOTARIZATION_REF - this is the notarization it should not have skipped
                                    fraudProof.evidence << CEvidenceData(CVDXF_Data::UTXORefKey(), ::AsVector(cnd.vtx[unchallengedForks[j][k]].first));

                                    // TODO: HARDENING - confirm that in precheck notarization, if a notarization has the same proofroot as
                                    // any prior notarization, whether it refers to it or not, it cannot pass precheck. that is easy to check,
                                    // may already be checked as of this note, and eliminates the need for an equality proof.

                                    int64_t proveHeight = std::min(cnd.vtx[unchallengedForks[j][k]].second.proofRoots[SystemID].rootHeight,
                                                                    cnd.vtx[unchallengedForks[forkToChallenge][k]].second.proofRoots[SystemID].rootHeight);

                                    int64_t atHeight = std::max(cnd.vtx[unchallengedForks[j][k]].second.proofRoots[SystemID].rootHeight,
                                                                    cnd.vtx[unchallengedForks[forkToChallenge][k]].second.proofRoots[SystemID].rootHeight);

                                    UniValue challengeRequest(UniValue::VOBJ);
                                    challengeRequest.pushKV("type", EncodeDestination(CIdentityID(CNotaryEvidence::SkipChallengeKey())));
                                    challengeRequest.pushKV("evidence", fraudProof.ToUniValue());
                                    challengeRequest.pushKV("proveheight", proveHeight);
                                    challengeRequest.pushKV("atheight", atHeight);
                                    challengeRequest.pushKV("entropyhash", txes[unchallengedForks[forkToChallenge][k]].second.GetHex());
                                    challengeRequests.push_back(challengeRequest);

                                    // once we've created an invalidation proof, which a skipped proof is,
                                    // the entire fork will be automatically invalidated
                                    unchallengedForks[forkToChallenge].erase(unchallengedForks[forkToChallenge].begin() + challengeNum, unchallengedForks[forkToChallenge].end());
                                }
                                else
                                {
                                    // we need to get the challenge from the notary chain, and it should only need
                                    // to challenge and prove from the last agreed entry forward, using entropy hash
                                    // of the block of the notarization that we are challenging

                                    CNotaryEvidence challengeEvidence;
                                    challengeEvidence.output = cnd.vtx[i].first;
                                    challengeEvidence.state = CNotaryEvidence::STATE_REJECTED;

                                    // create evidence to either use when notarizing, or for a challenge
                                    int64_t fromHeight = cnd.vtx[unchallengedForks[j][k - 1]].second.IsBlockOneNotarization() ?
                                                            1 :
                                                            cnd.vtx[unchallengedForks[j][k - 1]].second.proofRoots[SystemID].rootHeight;

                                    int64_t toHeight = cnd.vtx[unchallengedForks[forkToChallenge][k]].second.proofRoots[SystemID].rootHeight;

                                    UniValue challengeRequest(UniValue::VOBJ);
                                    challengeRequest.pushKV("type", EncodeDestination(CIdentityID(CNotaryEvidence::ValidityChallengeKey())));
                                    challengeRequest.pushKV("evidence", challengeEvidence.ToUniValue());
                                    challengeRequest.pushKV("fromheight", fromHeight);
                                    challengeRequest.pushKV("toheight", toHeight);
                                    challengeRequest.pushKV("challengedroot", cnd.vtx[unchallengedForks[forkToChallenge][k]].second.proofRoots[SystemID].ToUniValue());
                                    challengeRequest.pushKV("confirmroot", cnd.vtx[unchallengedForks[j][k - 1]].second.proofRoots[SystemID].ToUniValue());
                                    challengeRequest.pushKV("entropyhash", txes[unchallengedForks[forkToChallenge][k]].second.GetHex());
                                    challengeRequests.push_back(challengeRequest);
                                }
                            }
                            unchallengedForks[forkToChallenge].erase(unchallengedForks[forkToChallenge].begin() + k, unchallengedForks[forkToChallenge].end());
                        }
                    }
                }
                else
                {
                    // all descendents of this entry are invalid according to our perspective
                    // look for the first fork that agrees and create challenges until the end
                    // then look for the next
                    int invalidIndex = unchallengedForks[j][k];
                    for (int forkToChallenge = 0; forkToChallenge < unchallengedForks.size(); forkToChallenge++)
                    {
                        if (unchallengedForks[forkToChallenge].size() > k &&
                            unchallengedForks[forkToChallenge][k] == invalidIndex)
                        {
                            for (int challengeNum = k; challengeNum < unchallengedForks[forkToChallenge].size(); challengeNum++)
                            {
                                bool moveToNext = false;
                                for (auto &oneChallenge : localCounterEvidence[unchallengedForks[forkToChallenge][k]])
                                {
                                    if (!std::get<1>(oneChallenge).evidence.chainObjects.size() ||
                                        std::get<1>(oneChallenge).evidence.chainObjects[0]->objectType != CHAINOBJ_PROOF_ROOT)
                                    {
                                        continue;
                                    }
                                    // if the challenge root is the same as the root being challenged, it is a skip challenge
                                    // and would not be on-chain or in mempool if not valid. we can ignore the rest of this fork.
                                    if (((CChainObject<CProofRoot> *)std::get<1>(oneChallenge).evidence.chainObjects[0])->object ==
                                        cnd.vtx[unchallengedForks[forkToChallenge][k]].second.proofRoots[SystemID])
                                    {
                                        unchallengedForks[forkToChallenge].erase(unchallengedForks[forkToChallenge].begin() + challengeNum, unchallengedForks[forkToChallenge].end());
                                        continue;
                                    }
                                    // TODO: POST HARDENING - right now, we actually don't break down ranges to a lower level of
                                    // granularity, based on checkpoints, so until we do, just consider this one covered and move
                                    // on to the next
                                    moveToNext = true;
                                    break;
                                }
                                if (moveToNext)
                                {
                                    continue;
                                }

                                CNotaryEvidence challengeEvidence;
                                challengeEvidence.output = cnd.vtx[i].first;
                                challengeEvidence.state = CNotaryEvidence::STATE_REJECTED;

                                // create evidence to either use when notarizing, or for a challenge
                                int64_t fromHeight = cnd.vtx[cnd.lastConfirmed].second.IsBlockOneNotarization() ?
                                                        1 :
                                                        cnd.vtx[cnd.lastConfirmed].second.proofRoots[SystemID].rootHeight;

                                int64_t toHeight = cnd.vtx[unchallengedForks[forkToChallenge][k]].second.proofRoots[SystemID].rootHeight;

                                UniValue challengeRequest(UniValue::VOBJ);
                                challengeRequest.pushKV("type", EncodeDestination(CIdentityID(CNotaryEvidence::ValidityChallengeKey())));
                                challengeRequest.pushKV("evidence", challengeEvidence.ToUniValue());
                                challengeRequest.pushKV("fromheight", fromHeight);
                                challengeRequest.pushKV("toheight", toHeight);
                                challengeRequest.pushKV("challengedroot", cnd.vtx[unchallengedForks[forkToChallenge][k]].second.proofRoots[SystemID].ToUniValue());
                                challengeRequest.pushKV("confirmroot", cnd.vtx[cnd.lastConfirmed].second.proofRoots[SystemID].ToUniValue());
                                challengeRequest.pushKV("entropyhash", txes[unchallengedForks[forkToChallenge][k]].second.GetHex());
                                challengeRequests.push_back(challengeRequest);
                            }
                            unchallengedForks[forkToChallenge].erase(unchallengedForks[forkToChallenge].begin() + k, unchallengedForks[forkToChallenge].end());
                        }
                    }
                }
            }
            // if we have challenge requests, get them to either submit rejections or add to our notarization, if it challenges
            // a pre-existing notarization. we decide which later on
            if (challengeRequests.size())
            {
                LogPrint("notarization", "Getting challenge proofs %s", challengeRequests.write(1,2).c_str());
                params = UniValue(UniValue::VARR);
                params.push_back(challengeRequests);
                try
                {
                    challengeRequests = find_value(RPCCallRoot("getchallengeproofs", params), "result");
                } catch (exception e)
                {
                    challengeRequests = NullUniValue;
                }
                LogPrint("notarization", "Challenge proofs returned %s", challengeRequests.write(1,2).c_str());

                // separate out skip challenges and submit those now to ensure they just get removed from the equation
                UniValue skipChallenges(UniValue::VARR);
                for (int j = 0; j < challengeRequests.size(); j++)
                {
                    UniValue oneChallengeUni;
                    if (find_value(challengeRequests[j], "error").isNull() &&
                        (oneChallengeUni = find_value(challengeRequests[j], "result")).isObject())
                    {
                        if (ParseVDXFKey(uni_get_str(find_value(oneChallengeUni, "type"))) == CNotaryEvidence::SkipChallengeKey())
                        {
                            skipChallenges.push_back(oneChallengeUni);
                        }
                        else
                        {
                            // validity challenges will hang off of our notarization
                            validityChallenges.push_back(oneChallengeUni);
                        }
                    }
                }

                if (skipChallenges.size())
                {
                    LogPrint("notarization", "Submitting skip challenges to local chain %s", skipChallenges.write(1,2).c_str());
                    params = UniValue(UniValue::VARR);
                    params.push_back(skipChallenges);
                    try
                    {
                        UniValue submitchallenges(const UniValue& params, bool fHelp);
                        skipChallenges = submitchallenges(params, false);
                    } catch (exception e)
                    {
                        skipChallenges = NullUniValue;
                    }
                    LogPrint("notarization", "Results of submissions returned from local chain %s", skipChallenges.write(1,2).c_str());
                }
            }
        }

        // we challenge any notarizations about a chain that is not us
        // we also challenge and finalize as rejected notarizations that skip over a prior notarization
        // which they actually should agree with
        UniValue challenges(UniValue::VARR);
        {
            LOCK2(cs_main, mempool.cs);

            std::vector<std::vector<int>> unchallengedForks = crosschainCND.forks;
            std::set<int> validIndexes;
            for (int i = 0; i < crosschainCND.vtx.size(); i++)
            {
                auto proofRootIT = crosschainCND.vtx[i].second.proofRoots.find(ASSETCHAINS_CHAINID);
                if (proofRootIT == crosschainCND.vtx[i].second.proofRoots.end())
                {
                    continue;
                }

                if (proofRootIT->second == CProofRoot::GetProofRoot(proofRootIT->second.rootHeight))
                {
                    validIndexes.insert(i);
                }
            }
            for (int i = 1; i < crosschainCND.vtx.size(); i++)
            {
                auto proofRootIT = crosschainCND.vtx[i].second.proofRoots.find(SystemID);
                if (proofRootIT == crosschainCND.vtx[i].second.proofRoots.end())
                {
                    continue;
                }

                // find this entry's first location, k, in fork j
                int j, k;
                for (j = 0; j < unchallengedForks.size(); j++)
                {
                    for (k = 0; k < unchallengedForks[j].size(); k++)
                    {
                        if (unchallengedForks[j][k] == i)
                        {
                            break;
                        }
                    }
                    if (k < unchallengedForks[j].size())
                    {
                        break;
                    }
                }

                // if we didn't find this entry in the pruned forks, nothing to do
                if (j >= unchallengedForks.size())
                {
                    continue;
                }

                // if valid, use it to eliminate remaining forks and collect remaining challenges
                if (validIndexes.count(i))
                {
                    // we found the first valid fork with this entry
                    // all forks that do not have the same value in the k'th entry
                    // should be challenged, either because we don't agree,
                    // or because they skipped this valid one and did not reference it
                    // if their entry is not the same, but valid, they are provably
                    // rejectable, otherwise just to be challenged
                    for (int forkToChallenge = 0; forkToChallenge < unchallengedForks.size(); forkToChallenge++)
                    {
                        if (forkToChallenge == j)
                        {
                            continue;
                        }
                        if (unchallengedForks[forkToChallenge].size() > k &&
                            unchallengedForks[forkToChallenge][k] != unchallengedForks[j][k])
                        {
                            for (int challengeNum = k; challengeNum < unchallengedForks[forkToChallenge].size(); challengeNum++)
                            {
                                // is there already counter evidence for this entry?
                                // 1) if there is a fraud proof, prune and move on
                                // 2) if there is a validity challenge, only add one if we intend to
                                //    expand its range
                                bool moveToNext = false;
                                for (auto &oneChallenge : crossChainCounterEvidence[unchallengedForks[forkToChallenge][k]])
                                {
                                    if (!std::get<0>(oneChallenge).evidence.chainObjects.size() ||
                                        std::get<0>(oneChallenge).evidence.chainObjects[0]->objectType != CHAINOBJ_PROOF_ROOT)
                                    {
                                        continue;
                                    }
                                    // if the challenge root is the same as the root being challenged, it is a skip challenge
                                    // and would not be on-chain or in mempool if not valid. we can ignore the rest of this fork.
                                    if (((CChainObject<CProofRoot> *)std::get<0>(oneChallenge).evidence.chainObjects[0])->object ==
                                        crosschainCND.vtx[unchallengedForks[forkToChallenge][k]].second.proofRoots[SystemID])
                                    {
                                        unchallengedForks[forkToChallenge].erase(unchallengedForks[forkToChallenge].begin() + challengeNum, unchallengedForks[forkToChallenge].end());
                                        continue;
                                    }
                                    // TODO: POST HARDENING - right now, we actually don't break down ranges to a lower level of
                                    // granularity, based on checkpoints, so until we do, just consider this one covered and move
                                    // on to the next
                                    moveToNext = true;
                                    break;
                                }
                                if (moveToNext)
                                {
                                    continue;
                                }

                                // if it's valid, then it skipped a valid notarization and can be proven false
                                if (validIndexes.count(unchallengedForks[forkToChallenge][k]))
                                {
                                    // prove the root on the j fork with this root and submit the proof along with a rejection
                                    // finalization
                                    CNotaryEvidence fraudProof;
                                    fraudProof.output = crosschainCND.vtx[unchallengedForks[forkToChallenge][k]].first;
                                    fraudProof.state = CNotaryEvidence::STATE_REJECTED;

                                    // create evidence in this order
                                    // EXPECT_ALTERNATE_PROOF_ROOT  // give it the same root as it has, indicating fraud proof
                                    fraudProof.evidence << crosschainCND.vtx[unchallengedForks[forkToChallenge][k]].second.proofRoots[SystemID];

                                    // EXPECT_SKIPPED_NOTARIZATION_REF - this is the notarization it should not have skipped
                                    fraudProof.evidence << CEvidenceData(CVDXF_Data::UTXORefKey(), ::AsVector(crosschainCND.vtx[unchallengedForks[j][k]].first));

                                    // TODO: HARDENING - confirm that in precheck notarization, if a notarization has the same proofroot as
                                    // any prior notarization, whether it refers to it or not, it cannot pass precheck. that is easy to check,
                                    // may already be checked as of this note, and eliminates the need for an equality proof.

                                    int64_t proveHeight = std::min(crosschainCND.vtx[unchallengedForks[j][k]].second.proofRoots[SystemID].rootHeight,
                                                                    crosschainCND.vtx[unchallengedForks[forkToChallenge][k]].second.proofRoots[SystemID].rootHeight);

                                    int64_t atHeight = std::max(crosschainCND.vtx[unchallengedForks[j][k]].second.proofRoots[SystemID].rootHeight,
                                                                    crosschainCND.vtx[unchallengedForks[forkToChallenge][k]].second.proofRoots[SystemID].rootHeight);

                                    UniValue challengeRequest(UniValue::VOBJ);
                                    challengeRequest.pushKV("type", EncodeDestination(CIdentityID(CNotaryEvidence::SkipChallengeKey())));
                                    challengeRequest.pushKV("evidence", fraudProof.ToUniValue());
                                    challengeRequest.pushKV("proveheight", proveHeight);
                                    challengeRequest.pushKV("atheight", atHeight);
                                    challengeRequest.pushKV("entropyhash", blockHashes[unchallengedForks[forkToChallenge][k]].GetHex());
                                    challenges.push_back(challengeRequest);

                                    // once we've created an invalidation proof, which a skipped proof is,
                                    // the entire fork will be automatically invalidated
                                    unchallengedForks[forkToChallenge].erase(unchallengedForks[forkToChallenge].begin() + challengeNum, unchallengedForks[forkToChallenge].end());
                                }
                                else
                                {
                                    // we need to get the challenge from the notary chain, and it should only need
                                    // to challenge and prove from the last agreed entry forward, using entropy hash
                                    // of the block of the notarization that we are challenging

                                    CNotaryEvidence challengeEvidence;
                                    challengeEvidence.output = crosschainCND.vtx[i].first;
                                    challengeEvidence.state = CNotaryEvidence::STATE_REJECTED;

                                    // create evidence to either use when notarizing, or for a challenge
                                    int64_t fromHeight = crosschainCND.vtx[unchallengedForks[j][k - 1]].second.IsBlockOneNotarization() ?
                                                            1 :
                                                            crosschainCND.vtx[unchallengedForks[j][k - 1]].second.proofRoots[SystemID].rootHeight;

                                    int64_t toHeight = crosschainCND.vtx[unchallengedForks[forkToChallenge][k]].second.proofRoots[SystemID].rootHeight;

                                    UniValue challengeRequest(UniValue::VOBJ);
                                    challengeRequest.pushKV("type", EncodeDestination(CIdentityID(CNotaryEvidence::ValidityChallengeKey())));
                                    challengeRequest.pushKV("evidence", challengeEvidence.ToUniValue());
                                    challengeRequest.pushKV("fromheight", fromHeight);
                                    challengeRequest.pushKV("toheight", toHeight);
                                    challengeRequest.pushKV("challengedroot", crosschainCND.vtx[unchallengedForks[forkToChallenge][k]].second.proofRoots[SystemID].ToUniValue());
                                    challengeRequest.pushKV("confirmroot", crosschainCND.vtx[unchallengedForks[j][k - 1]].second.proofRoots[SystemID].ToUniValue());
                                    challengeRequest.pushKV("entropyhash", blockHashes[unchallengedForks[forkToChallenge][k]].GetHex());
                                    challenges.push_back(challengeRequest);
                                }
                            }
                            unchallengedForks[forkToChallenge].erase(unchallengedForks[forkToChallenge].begin() + k, unchallengedForks[forkToChallenge].end());
                        }
                    }
                }
                else
                {
                    // all descendents of this entry are invalid according to our perspective
                    // look for the first fork that agrees and create challenges until the end
                    // then look for the next
                    int invalidIndex = unchallengedForks[j][k];
                    for (int forkToChallenge = 0; forkToChallenge < unchallengedForks.size(); forkToChallenge++)
                    {
                        if (unchallengedForks[forkToChallenge].size() > k &&
                            unchallengedForks[forkToChallenge][k] == invalidIndex)
                        {
                            for (int challengeNum = k; challengeNum < unchallengedForks[forkToChallenge].size(); challengeNum++)
                            {
                                bool moveToNext = false;
                                for (auto &oneChallenge : crossChainCounterEvidence[unchallengedForks[forkToChallenge][k]])
                                {
                                    if (!std::get<0>(oneChallenge).evidence.chainObjects.size() ||
                                        std::get<0>(oneChallenge).evidence.chainObjects[0]->objectType != CHAINOBJ_PROOF_ROOT)
                                    {
                                        continue;
                                    }
                                    // if the challenge root is the same as the root being challenged, it is a skip challenge
                                    // and would not be on-chain or in mempool if not valid. we can ignore the rest of this fork.
                                    if (((CChainObject<CProofRoot> *)std::get<0>(oneChallenge).evidence.chainObjects[0])->object ==
                                        crosschainCND.vtx[unchallengedForks[forkToChallenge][k]].second.proofRoots[SystemID])
                                    {
                                        unchallengedForks[forkToChallenge].erase(unchallengedForks[forkToChallenge].begin() + challengeNum, unchallengedForks[forkToChallenge].end());
                                        continue;
                                    }
                                    // TODO: POST HARDENING - right now, we actually don't break down ranges to a lower level of
                                    // granularity, based on checkpoints, so until we do, just consider this one covered and move
                                    // on to the next
                                    moveToNext = true;
                                    break;
                                }
                                if (moveToNext)
                                {
                                    continue;
                                }

                                CNotaryEvidence challengeEvidence;
                                challengeEvidence.output = crosschainCND.vtx[i].first;
                                challengeEvidence.state = CNotaryEvidence::STATE_REJECTED;

                                // create evidence to either use when notarizing, or for a challenge
                                int64_t fromHeight = crosschainCND.vtx[cnd.lastConfirmed].second.IsPreLaunch() ?
                                                        1 :
                                                        crosschainCND.vtx[cnd.lastConfirmed].second.proofRoots[SystemID].rootHeight;

                                int64_t toHeight = crosschainCND.vtx[unchallengedForks[forkToChallenge][k]].second.proofRoots[SystemID].rootHeight;

                                UniValue challengeRequest(UniValue::VOBJ);
                                challengeRequest.pushKV("type", EncodeDestination(CIdentityID(CNotaryEvidence::ValidityChallengeKey())));
                                challengeRequest.pushKV("evidence", challengeEvidence.ToUniValue());
                                challengeRequest.pushKV("fromheight", fromHeight);
                                challengeRequest.pushKV("toheight", toHeight);
                                challengeRequest.pushKV("challengedroot", crosschainCND.vtx[unchallengedForks[forkToChallenge][k]].second.proofRoots[SystemID].ToUniValue());
                                challengeRequest.pushKV("confirmnotarization", crosschainCND.vtx[cnd.lastConfirmed].second.ToUniValue());
                                challengeRequest.pushKV("entropyhash", blockHashes[unchallengedForks[forkToChallenge][k]].GetHex());
                                challenges.push_back(challengeRequest);
                            }
                            unchallengedForks[forkToChallenge].erase(unchallengedForks[forkToChallenge].begin() + k, unchallengedForks[forkToChallenge].end());
                        }
                    }
                }
            }

            // if we have challenge requests, get them to either submit rejections or add to our notarization, if it challenges
            // a pre-existing notarization. we decide which later on
            if (challenges.size())
            {
                LogPrint("notarization", "Getting challenge proofs from local chain %s", challenges.write(1,2).c_str());
                params = UniValue(UniValue::VARR);
                params.push_back(challenges);
                try
                {
                    UniValue getchallengeproofs(const UniValue& params, bool fHelp);
                    challenges = getchallengeproofs(params, false);
                } catch (exception e)
                {
                    challenges = NullUniValue;
                }
                LogPrint("notarization", "Challenge proofs returned from local chain %s", challenges.write(1,2).c_str());
            }

            // if we have challenges, submit them
            if (challenges.size())
            {
                UniValue submitParams(UniValue::VARR);
                for (int i = 0; i < challenges.size(); i++)
                {
                    if (find_value(challenges[i], "error").isNull())
                    {
                        UniValue resultUni = find_value(challenges[i], "result");
                        if (resultUni.isObject())
                        {
                            submitParams.push_back(resultUni);
                        }
                    }
                }
                LogPrint("notarization", "Submitting challenges %s", submitParams.write(1,2).c_str());
                UniValue challengeResult;
                params = UniValue(UniValue::VARR);
                params.push_back(submitParams);
                try
                {
                    challengeResult = find_value(RPCCallRoot("submitchallenges", params), "result");
                } catch (exception e)
                {
                    challengeResult = NullUniValue;
                }
                LogPrint("notarization", "Response from challenges %s", challengeResult.write(1,2).c_str());
            }
        }
    }

    // if we have enough notarizations in a row with the ones we agree with and
    // have no more powerful alternatives, we can finalize the notarization if this is auto-notarization.
    //
    // CPBaaSNotarization::MinBlocksToAutoNotarization(ConnectedChains.ThisChain().blockNotarizationModulo)

    int32_t notaryIdx = isGatewayFirstContact ? 0 : uni_get_int(find_value(bestProofRootResult, "bestindex"), -1);

    UniValue lastConfirmedUni = find_value(bestProofRootResult, "lastconfirmedproofroot");
    CProofRoot lastConfirmedProofRoot;
    if (!lastConfirmedUni.isNull())
    {
        lastConfirmedProofRoot = CProofRoot(lastConfirmedUni);
    }

    if (bestProofRootResult.isNull() || notaryIdx == -1)
    {
        return state.Error(bestProofRootResult.isNull() ? "no-notary" : "no-matching-proof-roots-found");
    }

    CProofRoot lastStableProofRoot(find_value(bestProofRootResult, "laststableproofroot"));

    if (!lastConfirmedProofRoot.IsValid() && lastStableProofRoot.IsValid())
    {
        for (int i = cnd.vtx.size() - 1; i >= 0; i--)
        {
            auto it = cnd.vtx[i].second.proofRoots.find(SystemID);
            if (it != cnd.vtx[i].second.proofRoots.end())
            {
                if (it->second.rootHeight < lastStableProofRoot.rootHeight)
                {
                    notaryIdx = i;
                    break;
                }
                else if (it->second.rootHeight == lastStableProofRoot.rootHeight &&
                        it->second == lastStableProofRoot && notaryIdx != i)
                {
                    LogPrintf("%s: notarization was rejected with identical proof root to last stable: %s\n", __func__, cnd.vtx[i].second.ToUniValue().write(1,2).c_str());
                    notaryIdx = i;
                }
            }
        }
    }

    {
        // take the lock again, now that we're back from calling out
        LOCK2(cs_main, mempool.cs);

        // if height changed, we need to fail and possibly try again
        if (height != chainActive.Height())
        {
            return state.Error("stale-block");
        }

        const CTransaction &priorNotarizationTx = txes[notaryIdx].first;
        uint256 priorBlkHash = txes[notaryIdx].second;
        const CUTXORef &priorUTXO = cnd.vtx[notaryIdx].first;
        const CPBaaSNotarization &priorNotarization = cnd.vtx[notaryIdx].second;

        // get block index holding the last notarization we agree with
        auto mapBlockIt = mapBlockIndex.find(priorBlkHash);
        if (mapBlockIt == mapBlockIndex.end() || !chainActive.Contains(mapBlockIt->second))
        {
            return state.Error(errorPrefix + "prior notarization not in blockchain");
        }

        // first determine if the prior notarization we agree with would make this one moot, given the current
        // block notarization modulo
        uint32_t adjustedNotarizationModulo = CPBaaSNotarization::GetAdjustedNotarizationModulo(ConnectedChains.ThisChain().blockNotarizationModulo,
                                                                                                (height + 1) -
                                                                                                mapBlockIndex[txes[cnd.lastConfirmed].second]->GetHeight());

        int blockPeriodNumber = (height + 1) / adjustedNotarizationModulo;
        int priorBlockPeriod = mapBlockIt->second->GetHeight() / adjustedNotarizationModulo;

        // for decentralized notarization, we must alternate between proof of stake and proof of work blocks
        // to confirm a prior earned notarization
        if (blockPeriodNumber <= priorBlockPeriod ||
            (height > (VERUS_MIN_STAKEAGE << 1) &&
            (ConnectedChains.ThisChain().notarizationProtocol == CCurrencyDefinition::NOTARIZATION_AUTO &&
            ((isStake && mapBlockIt->second->IsVerusPOSBlock()) || (!isStake && !mapBlockIt->second->IsVerusPOSBlock())))))
        {
            if (LogAcceptCategory("notarization"))
            {
                LogPrintf("%s: block period #%d, prior block period #%d, height: %u, notarization protocol: %d, isPoS: %s, is last notarization PoS: %s\n",
                        __func__,
                        blockPeriodNumber,
                        priorBlockPeriod,
                        height,
                        ConnectedChains.ThisChain().notarizationProtocol,
                        isStake ? "true" : "false",
                        mapBlockIt->second->IsVerusPOSBlock() ? "true" : "false");
                if (LogAcceptCategory("verbose"))
                {
                    printf("%s: block period #%d, prior block period #%d, height: %u, notarization protocol: %d, isPoS: %s, is last notarization PoS: %s\n",
                            __func__,
                            blockPeriodNumber,
                            priorBlockPeriod,
                            height,
                            ConnectedChains.ThisChain().notarizationProtocol,
                            isStake ? "true" : "false",
                            mapBlockIt->second->IsVerusPOSBlock() ? "true" : "false");
                }
            }
            return state.Error("ineligible");
        }

        notarization = priorNotarization;
        notarization.SetBlockOneNotarization(false);
        notarization.SetDefinitionNotarization(false);
        notarization.SetSameChain(false);
        notarization.SetPreLaunch(false);
        notarization.SetLaunchCleared(true);
        notarization.SetLaunchConfirmed(true);
        notarization.proposer = Proposer;
        notarization.prevHeight = priorNotarization.notarizationHeight;

        // get the latest notarization information for the new, earned notarization
        // one system may provide one proof root and multiple currency states
        CProofRoot latestProofRoot = lastConfirmedProofRoot.IsValid() ? lastConfirmedProofRoot : CProofRoot(find_value(bestProofRootResult, "laststableproofroot"));

        if (!latestProofRoot.IsValid() || notarization.proofRoots[latestProofRoot.systemID].rootHeight >= latestProofRoot.rootHeight)
        {
            return state.Error("no-new-stable-proof-root");
        }

        // remove any proof roots from other cross-chain imports that may have been put into the proof root array
        std::set<uint160> proofRootsToDelete;
        for (auto &oneProofRoot : notarization.proofRoots)
        {
            if (oneProofRoot.first != ASSETCHAINS_CHAINID)
            {
                proofRootsToDelete.insert(oneProofRoot.first);
            }
        }
        for (auto &oneProofRootID : proofRootsToDelete)
        {
            notarization.proofRoots.erase(oneProofRootID);
        }

        notarization.proofRoots[latestProofRoot.systemID] = latestProofRoot;
        notarization.notarizationHeight = latestProofRoot.rootHeight;

        // this is cross-checked to ensure that the correct/consensus gas price
        // is passed through and used at all times
        if (systemDef.proofProtocol == systemDef.PROOF_ETHNOTARIZATION &&
            notarization.currencyState.conversionPrice.size())
        {
            notarization.currencyState.conversionPrice[0] = latestProofRoot.gasPrice;
        }

        UniValue currencyStatesUni = find_value(bestProofRootResult, "currencystates");

        if (!systemDef.IsGateway() && !(currencyStatesUni.isArray() && currencyStatesUni.size()))
        {
            return state.Error(errorPrefix + "invalid or missing currency state data from notary");
        }

        if (crosschainCND.vtx.size())
        {
            int prevNotarizationIdx;
            CPBaaSNotarization lastPBN;
            for (prevNotarizationIdx = crosschainCND.vtx.size() - 1; prevNotarizationIdx >= 0; prevNotarizationIdx--)
            {
                lastPBN = crosschainCND.vtx[prevNotarizationIdx].second;

                std::map<uint160, CProofRoot>::iterator pIT = lastPBN.proofRoots.find(ASSETCHAINS_CHAINID);
                if (pIT != lastPBN.proofRoots.end() &&
                    (!priorNotarization.proofRoots.count(ASSETCHAINS_CHAINID) ||
                    pIT->second.rootHeight <= priorNotarization.proofRoots.find(ASSETCHAINS_CHAINID)->second.rootHeight) &&
                    CProofRoot::GetProofRoot(pIT->second.rootHeight) == pIT->second)
                {
                    break;
                }
                else if (pIT == crosschainCND.vtx[prevNotarizationIdx].second.proofRoots.end() &&
                        !prevNotarizationIdx &&
                        (crosschainCND.vtx[prevNotarizationIdx].second.IsDefinitionNotarization() || crosschainCND.vtx[prevNotarizationIdx].second.IsLaunchCleared()))
                {
                    // use the 0th element if no proof root and it is definition or start, since it has no proof root to be wrong
                    break;
                }
            }
            if (prevNotarizationIdx >= 0 &&
                lastPBN.SetMirror(false) &&
                !lastPBN.IsMirror())
            {
                if (LogAcceptCategory("notarization") && LogAcceptCategory("verbose"))
                {
                    std::vector<unsigned char> checkHex = ::AsVector(lastPBN);
                    LogPrintf("%s: hex of notarization: %s\nnotarization: %s\n", __func__, HexBytes(&(checkHex[0]), checkHex.size()).c_str(), lastPBN.ToUniValue().write(1,2).c_str());
                    printf("%s: notarization: %s\n", __func__, lastPBN.ToUniValue().write(1,2).c_str());
                }

                CNativeHashWriter hw;
                hw << lastPBN;
                notarization.hashPrevCrossNotarization = hw.GetHash();
            }
            else
            {
                notarization.hashPrevCrossNotarization.SetNull();
            }
        }

        notarization.currencyStates.clear();
        for (int i = 0; i < currencyStatesUni.size(); i++)
        {
            CCoinbaseCurrencyState oneCurState(currencyStatesUni[i]);
            CCurrencyDefinition oneCurDef;
            if (!oneCurState.IsValid())
            {
                return state.Error(errorPrefix + "invalid or missing currency state data from notary");
            }
            if (!(oneCurDef = ConnectedChains.GetCachedCurrency(oneCurState.GetID())).IsValid())
            {
                // if we don't have the currency for the state specified, and it isn't critical, ignore
                if (oneCurDef.GetID() == SystemID)
                {
                    return state.Error(errorPrefix + "system currency invalid - possible corruption");
                }
                continue;
            }
            if (oneCurDef.systemID == SystemID)
            {
                uint160 oneCurDefID = oneCurDef.GetID();
                if (notarization.currencyID == oneCurDefID)
                {
                    notarization.currencyState = oneCurState;
                }
                else
                {
                    notarization.currencyStates[oneCurDefID] = oneCurState;
                }
            }
        }
        notarization.currencyStates[ASSETCHAINS_CHAINID] = ConnectedChains.GetCurrencyState(height);
        if (!systemDef.GatewayConverterID().IsNull())
        {
            notarization.currencyStates[systemDef.GatewayConverterID()] = ConnectedChains.GetCurrencyState(systemDef.GatewayConverterID(), height);
        }

        // add this blockchain's info, based on the requested height
        uint160 thisChainID = ConnectedChains.ThisChain().GetID();
        notarization.proofRoots[thisChainID] = CProofRoot::GetProofRoot(height);

        // add currency states that we should include and then we're done
        // currency states to include are either a gateway currency indicated by the
        // gateway or our gateway converter for our PBaaS chain
        uint160 gatewayConverterID = systemDef.GatewayConverterID();
        if (systemDef.IsGateway() && (!systemDef.GatewayConverterID().IsNull()))
        {
            gatewayConverterID = systemDef.GatewayConverterID();
        }
        else if (SystemID == ConnectedChains.FirstNotaryChain().chainDefinition.GetID() && !ConnectedChains.ThisChain().GatewayConverterID().IsNull())
        {
            gatewayConverterID = ConnectedChains.ThisChain().GatewayConverterID();
        }
        if (!gatewayConverterID.IsNull())
        {
            // get the gateway converter currency from the gateway definition
            CChainNotarizationData gatewayCND;
            if (GetNotarizationData(gatewayConverterID, gatewayCND) && gatewayCND.vtx.size())
            {
                notarization.currencyStates[gatewayConverterID] = gatewayCND.vtx[gatewayCND.lastConfirmed].second.currencyState;
            }
        }

        notarization.nodes = GetGoodNodes(CPBaaSNotarization::MAX_NODES);

        notarization.prevNotarization = cnd.vtx[notaryIdx].first;
        notarization.prevHeight = cnd.vtx[notaryIdx].second.notarizationHeight;

        CCcontract_info CC;
        CCcontract_info *cp;
        std::vector<CTxDestination> dests;

        // make the earned notarization output
        cp = CCinit(&CC, EVAL_EARNEDNOTARIZATION);

        if (systemDef.notarizationProtocol == systemDef.NOTARIZATION_NOTARY_CHAINID)
        {
            dests = std::vector<CTxDestination>({CIdentityID(systemDef.GetID())});
        }
        else
        {
            dests = std::vector<CTxDestination>({CPubKey(ParseHex(CC.CChexstr))});
        }

        txOutputs.push_back(CTxOut(0, MakeMofNCCScript(CConditionObj<CPBaaSNotarization>(EVAL_EARNEDNOTARIZATION, dests, 1, &notarization))));

        // make the finalization output(s)
        //
        // if the notarization we agree with has an unanswered challenge, provide that evidence with our notarization
        // to enable ours to pass
        cp = CCinit(&CC, EVAL_FINALIZE_NOTARIZATION);

        dests = std::vector<CTxDestination>({CPubKey(ParseHex(CC.CChexstr))});

        // we need to store the input that we confirmed if we spent finalization outputs
        CObjectFinalization of = CObjectFinalization(CObjectFinalization::FINALIZE_NOTARIZATION, notarization.currencyID, uint256(), txOutputs.size() - 1, height + 15);
        txOutputs.push_back(CTxOut(0, MakeMofNCCScript(CConditionObj<CObjectFinalization>(EVAL_FINALIZE_NOTARIZATION, dests, 1, &of))));

        // TODO: HARDENING if we have validity challenges to issue, issue them here with evidence on our transaction
        // to ensure an informed chain of actions/evidence, etc., all challenges must either be present on an earned notarization
        // coinbase or they must spend a prior pending finalization.

    }
    return true;
}

std::vector<std::pair<uint32_t, CInputDescriptor>> CObjectFinalization::GetUnspentConfirmedFinalizations(const uint160 &currencyID)
{
    LOCK(mempool.cs);
    std::vector<std::pair<uint32_t, CInputDescriptor>> retVal;
    std::vector<CAddressUnspentDbEntry> indexUnspent;
    std::vector<std::pair<CMempoolAddressDeltaKey, CMempoolAddressDelta>> mempoolUnspent;

    uint160 indexKey = CCrossChainRPCData::GetConditionID(
        CCrossChainRPCData::GetConditionID(currencyID, CObjectFinalization::ObjectFinalizationNotarizationKey()),
        ObjectFinalizationConfirmedKey());
    if ((GetAddressUnspent(indexKey, CScript::P2IDX, indexUnspent) &&
         mempool.getAddressIndex(std::vector<std::pair<uint160, int32_t>>({{indexKey, CScript::P2IDX}}), mempoolUnspent)) &&
        (indexUnspent.size() || mempoolUnspent.size()))
    {
        for (auto &oneConfirmed : mempool.FilterUnspent(mempoolUnspent))
        {
            auto txProxy = mempool.mapTx.find(oneConfirmed.first.txhash);
            if (txProxy != mempool.mapTx.end())
            {
                auto &mpEntry = *txProxy;
                auto &tx = mpEntry.GetTx();
                //printf("%s: txid: %s, vout: %u\n", __func__, oneConfirmed.first.txhash.GetHex().c_str(), oneConfirmed.first.index);
                retVal.push_back(std::make_pair(0,
                                 CInputDescriptor(tx.vout[oneConfirmed.first.index].scriptPubKey,
                                                  tx.vout[oneConfirmed.first.index].nValue,
                                                  CTxIn(oneConfirmed.first.txhash, oneConfirmed.first.index))));
            }
        }
        for (auto &oneConfirmed : indexUnspent)
        {
            //printf("%s: txid: %s, vout: %lu, blockheight: %d\n", __func__, oneConfirmed.first.txhash.GetHex().c_str(), oneConfirmed.first.index, oneConfirmed.second.blockHeight);
            retVal.push_back(std::make_pair(oneConfirmed.second.blockHeight,
                             CInputDescriptor(oneConfirmed.second.script, oneConfirmed.second.satoshis, CTxIn(oneConfirmed.first.txhash, oneConfirmed.first.index))));
        }
    }
    return retVal;
}

std::vector<std::pair<uint32_t, CInputDescriptor>> CObjectFinalization::GetUnspentPendingFinalizations(const uint160 &currencyID)
{
    LOCK(mempool.cs);
    std::vector<std::pair<uint32_t, CInputDescriptor>> retVal;
    std::vector<CAddressUnspentDbEntry> indexUnspent;
    std::vector<std::pair<CMempoolAddressDeltaKey, CMempoolAddressDelta>> mempoolUnspent;

    uint160 indexKey = CCrossChainRPCData::GetConditionID(
        CCrossChainRPCData::GetConditionID(currencyID, CObjectFinalization::ObjectFinalizationNotarizationKey()),
        ObjectFinalizationPendingKey());
    if ((GetAddressUnspent(indexKey, CScript::P2IDX, indexUnspent) &&
         mempool.getAddressIndex(std::vector<std::pair<uint160, int32_t>>({{indexKey, CScript::P2IDX}}), mempoolUnspent)) &&
        (indexUnspent.size() || mempoolUnspent.size()))
    {
        for (auto &oneUnconfirmed : mempool.FilterUnspent(mempoolUnspent))
        {
            auto txProxy = mempool.mapTx.find(oneUnconfirmed.first.txhash);
            if (txProxy != mempool.mapTx.end())
            {
                auto &mpEntry = *txProxy;
                auto &tx = mpEntry.GetTx();
                retVal.push_back(std::make_pair(0,
                                 CInputDescriptor(tx.vout[oneUnconfirmed.first.index].scriptPubKey,
                                                  tx.vout[oneUnconfirmed.first.index].nValue,
                                                  CTxIn(oneUnconfirmed.first.txhash, oneUnconfirmed.first.index))));
            }
        }
        for (auto &oneConfirmed : indexUnspent)
        {
            //printf("%s: txid: %s, vout: %lu, blockheight: %d\n", __func__, oneConfirmed.first.txhash.GetHex().c_str(), oneConfirmed.first.index, oneConfirmed.second.blockHeight);
            retVal.push_back(std::make_pair(oneConfirmed.second.blockHeight,
                             CInputDescriptor(oneConfirmed.second.script, oneConfirmed.second.satoshis, CTxIn(oneConfirmed.first.txhash, oneConfirmed.first.index))));
        }
    }
    return retVal;
}

std::vector<std::tuple<uint32_t, COutPoint, CTransaction, CObjectFinalization>>
CObjectFinalization::GetFinalizations(const uint160 &currencyID,
                                      const uint160 &finalizationTypeKey,
                                      uint32_t startHeight,
                                      uint32_t endHeight)
{
    // if we don't have an endHeight, we also check the mempool
    bool checkMempool = false;
    if (!endHeight)
    {
        checkMempool = true;
    }

    std::vector<std::tuple<uint32_t, COutPoint, CTransaction, CObjectFinalization>> retVal;
    std::vector<CAddressIndexDbEntry> finalizationIndex;
    std::vector<std::pair<CMempoolAddressDeltaKey, CMempoolAddressDelta>> mempoolFinalizations;

    uint160 indexKey = CCrossChainRPCData::GetConditionID(
        CCrossChainRPCData::GetConditionID(currencyID, CObjectFinalization::ObjectFinalizationNotarizationKey()),
        finalizationTypeKey);

    LOCK(mempool.cs);

    if (GetAddressIndex(indexKey, CScript::P2IDX, finalizationIndex, startHeight, endHeight) &&
        finalizationIndex.size() &&
        (!checkMempool || mempool.getAddressIndex(std::vector<std::pair<uint160, int32_t>>({{indexKey, CScript::P2IDX}}), mempoolFinalizations)) &&
        (finalizationIndex.size() || mempoolFinalizations.size()))
    {
        std::vector<int> toRemove;
        std::map<COutPoint, int> outputMap;
        for (int i = 0; i < finalizationIndex.size(); i++)
        {
            if (!finalizationIndex[i].first.spending)
            {
                CTransaction finalizationTx;
                CObjectFinalization of;
                uint256 blkHash;
                COptCCParams fP;
                if (!myGetTransaction(finalizationIndex[i].first.txhash, finalizationTx, blkHash) ||
                    finalizationIndex[i].first.index >= finalizationTx.vout.size() ||
                    !(finalizationTx.vout[finalizationIndex[i].first.index].scriptPubKey.IsPayToCryptoCondition(fP) &&
                        fP.IsValid() &&
                        fP.evalCode == EVAL_FINALIZE_NOTARIZATION &&
                        fP.vData.size() &&
                        (of = CObjectFinalization(fP.vData[0])).IsValid()))
                {
                    LogPrintf("Invalid finalization transaction %s:\n", finalizationIndex[i].first.txhash.GetHex().c_str());
                    printf("Invalid finalization transaction %s:\n", finalizationIndex[i].first.txhash.GetHex().c_str());
                    return retVal;
                }
                retVal.push_back(std::make_tuple((uint32_t)finalizationIndex[i].first.blockHeight,
                                                 COutPoint(finalizationIndex[i].first.txhash,
                                                 finalizationIndex[i].first.index),
                                                 finalizationTx, of));
            }
        }
        for (int i = 0; i < mempoolFinalizations.size(); i++)
        {
            if (mempoolFinalizations[i].first.spending)
            {
                continue;
            }
            CTransaction oneTx;
            if (mempool.lookup(mempoolFinalizations[i].first.txhash, oneTx))
            {
                retVal.push_back(std::make_tuple((uint32_t)0,
                                                  COutPoint(mempoolFinalizations[i].first.txhash, mempoolFinalizations[i].first.index),
                                                  oneTx,
                                                  CObjectFinalization(oneTx.vout[mempoolFinalizations[i].first.index].scriptPubKey)));
            }
        }
    }
    return retVal;
}

std::vector<std::tuple<uint32_t, COutPoint, CTransaction, CObjectFinalization>>
CObjectFinalization::GetFinalizations(const CUTXORef &outputRef,
                 const uint160 &finalizationTypeKey,
                 uint32_t startHeight,
                 uint32_t endHeight)
{
    // if we don't have an endHeight, we also check the mempool
    bool checkMempool = false;
    if (!endHeight)
    {
        checkMempool = true;
    }

    std::vector<std::tuple<uint32_t, COutPoint, CTransaction, CObjectFinalization>> retVal;
    std::vector<CAddressIndexDbEntry> finalizationIndex;
    std::vector<std::pair<CMempoolAddressDeltaKey, CMempoolAddressDelta>> mempoolFinalizations;

    uint160 indexKey = CCrossChainRPCData::GetConditionID(finalizationTypeKey, outputRef.hash, outputRef.n);

    LOCK(mempool.cs);

    if (GetAddressIndex(indexKey, CScript::P2IDX, finalizationIndex, startHeight, endHeight) &&
        finalizationIndex.size() &&
        (!checkMempool || mempool.getAddressIndex(std::vector<std::pair<uint160, int32_t>>({{indexKey, CScript::P2IDX}}), mempoolFinalizations)) &&
        (finalizationIndex.size() || mempoolFinalizations.size()))
    {
        std::vector<int> toRemove;
        std::map<COutPoint, int> outputMap;
        for (int i = 0; i < finalizationIndex.size(); i++)
        {
            if (!finalizationIndex[i].first.spending)
            {
                CTransaction finalizationTx;
                CObjectFinalization of;
                uint256 blkHash;
                COptCCParams fP;
                if (!myGetTransaction(finalizationIndex[i].first.txhash, finalizationTx, blkHash) ||
                    finalizationIndex[i].first.index >= finalizationTx.vout.size() ||
                    !(finalizationTx.vout[finalizationIndex[i].first.index].scriptPubKey.IsPayToCryptoCondition(fP) &&
                        fP.IsValid() &&
                        fP.evalCode == EVAL_FINALIZE_NOTARIZATION &&
                        fP.vData.size() &&
                        (of = CObjectFinalization(fP.vData[0])).IsValid()))
                {
                    LogPrintf("Invalid finalization transaction %s:\n", finalizationIndex[i].first.txhash.GetHex().c_str());
                    printf("Invalid finalization transaction %s:\n", finalizationIndex[i].first.txhash.GetHex().c_str());
                    return retVal;
                }
                retVal.push_back(std::make_tuple((uint32_t)finalizationIndex[i].first.blockHeight,
                                                 COutPoint(finalizationIndex[i].first.txhash,
                                                 finalizationIndex[i].first.index),
                                                 finalizationTx, of));
            }
        }
        for (int i = 0; i < mempoolFinalizations.size(); i++)
        {
            if (mempoolFinalizations[i].first.spending)
            {
                continue;
            }
            CTransaction oneTx;
            if (mempool.lookup(mempoolFinalizations[i].first.txhash, oneTx))
            {
                retVal.push_back(std::make_tuple((uint32_t)0,
                                                  COutPoint(mempoolFinalizations[i].first.txhash, mempoolFinalizations[i].first.index),
                                                  oneTx,
                                                  CObjectFinalization(oneTx.vout[mempoolFinalizations[i].first.index].scriptPubKey)));
            }
        }
    }
    return retVal;
}

std::vector<std::tuple<uint32_t, COutPoint, CTransaction, CPBaaSNotarization>>
GetOutputKeyedNotarizations(const CUTXORef &outputRef,
                            const uint160 &typeKey,
                            uint32_t startHeight,
                            uint32_t endHeight)
{
    // if we don't have an endHeight, we also check the mempool
    bool checkMempool = false;
    if (!endHeight)
    {
        checkMempool = true;
    }

    std::vector<std::tuple<uint32_t, COutPoint, CTransaction, CPBaaSNotarization>> retVal;
    std::vector<CAddressIndexDbEntry> finalizationIndex;
    std::vector<std::pair<CMempoolAddressDeltaKey, CMempoolAddressDelta>> mempoolFinalizations;

    uint160 indexKey = CCrossChainRPCData::GetConditionID(typeKey, outputRef.hash, outputRef.n);

    LOCK(mempool.cs);

    if (GetAddressIndex(indexKey, CScript::P2IDX, finalizationIndex, startHeight, endHeight) &&
        finalizationIndex.size() &&
        (!checkMempool || mempool.getAddressIndex(std::vector<std::pair<uint160, int32_t>>({{indexKey, CScript::P2IDX}}), mempoolFinalizations)) &&
        (finalizationIndex.size() || mempoolFinalizations.size()))
    {
        std::vector<int> toRemove;
        std::map<COutPoint, int> outputMap;
        for (int i = 0; i < finalizationIndex.size(); i++)
        {
            if (!finalizationIndex[i].first.spending)
            {
                CTransaction notarizationTx;
                CPBaaSNotarization notarization;
                uint256 blkHash;
                COptCCParams fP;
                if (!myGetTransaction(finalizationIndex[i].first.txhash, notarizationTx, blkHash) ||
                    finalizationIndex[i].first.index >= notarizationTx.vout.size() ||
                    !(notarizationTx.vout[finalizationIndex[i].first.index].scriptPubKey.IsPayToCryptoCondition(fP) &&
                        fP.IsValid() &&
                        (fP.evalCode == EVAL_EARNEDNOTARIZATION || fP.evalCode == EVAL_ACCEPTEDNOTARIZATION) &&
                        fP.vData.size() &&
                        (notarization = CPBaaSNotarization(fP.vData[0])).IsValid()))
                {
                    LogPrintf("Invalid notarization transaction %s:\n", finalizationIndex[i].first.txhash.GetHex().c_str());
                    printf("Invalid notarization transaction %s:\n", finalizationIndex[i].first.txhash.GetHex().c_str());
                    return retVal;
                }
                retVal.push_back(std::make_tuple((uint32_t)finalizationIndex[i].first.blockHeight,
                                                  COutPoint(finalizationIndex[i].first.txhash,
                                                  finalizationIndex[i].first.index),
                                                  notarizationTx,
                                                  notarization));
            }
        }
        for (int i = 0; i < mempoolFinalizations.size(); i++)
        {
            if (mempoolFinalizations[i].first.spending)
            {
                continue;
            }
            CTransaction oneTx;
            if (mempool.lookup(mempoolFinalizations[i].first.txhash, oneTx))
            {
                retVal.push_back(std::make_tuple((uint32_t)0,
                                                  COutPoint(mempoolFinalizations[i].first.txhash, mempoolFinalizations[i].first.index),
                                                  oneTx,
                                                  CPBaaSNotarization(oneTx.vout[mempoolFinalizations[i].first.index].scriptPubKey)));
            }
        }
    }
    return retVal;
}

std::vector<std::pair<uint32_t, CInputDescriptor>> CObjectFinalization::GetUnspentEvidence(const uint160 &currencyID,
                                                                                           const uint256 &notarizationTxId,
                                                                                           int32_t notarizationOutNum)
{
    LOCK(mempool.cs);
    std::vector<std::pair<uint32_t, CInputDescriptor>> retVal;
    std::vector<CAddressUnspentDbEntry> indexUnspent;
    std::vector<std::pair<CMempoolAddressDeltaKey, CMempoolAddressDelta>> mempoolUnspent;
    uint160 indexKey = CCrossChainRPCData::GetConditionID(CNotaryEvidence::NotarySignatureKey(), notarizationTxId, notarizationOutNum);

    if ((GetAddressUnspent(indexKey, CScript::P2IDX, indexUnspent) &&
         mempool.getAddressIndex(std::vector<std::pair<uint160, int32_t>>({{indexKey, CScript::P2IDX}}), mempoolUnspent)) &&
        (indexUnspent.size() || mempoolUnspent.size()))
    {
        for (auto &oneUnconfirmed : mempool.FilterUnspent(mempoolUnspent))
        {
            auto txProxy = mempool.mapTx.find(oneUnconfirmed.first.txhash);
            if (txProxy != mempool.mapTx.end())
            {
                auto &mpEntry = *txProxy;
                auto &tx = mpEntry.GetTx();
                retVal.push_back(std::make_pair(0,
                                 CInputDescriptor(tx.vout[oneUnconfirmed.first.index].scriptPubKey,
                                                  tx.vout[oneUnconfirmed.first.index].nValue,
                                                  CTxIn(oneUnconfirmed.first.txhash, oneUnconfirmed.first.index))));
            }
        }
        for (auto &oneConfirmed : indexUnspent)
        {
            //printf("%s: txid: %s, vout: %lu, blockheight: %d\n", __func__, oneConfirmed.first.txhash.GetHex().c_str(), oneConfirmed.first.index, oneConfirmed.second.blockHeight);
            retVal.push_back(std::make_pair(oneConfirmed.second.blockHeight,
                             CInputDescriptor(oneConfirmed.second.script, oneConfirmed.second.satoshis, CTxIn(oneConfirmed.first.txhash, oneConfirmed.first.index))));
        }
    }
    return retVal;
}

int CChainNotarizationData::BestConfirmedNotarization(const CCurrencyDefinition &notarizingSystem,
                                                      int minNotaryConfirms,
                                                      int minBlockConfirms,
                                                      uint32_t height,
                                                      const std::vector<std::pair<CTransaction, uint256>> &txAndBlockVec) const
{
    // last notarization cannot be stale
    int maxDepth = notarizingSystem.blockNotarizationModulo << 1;
    uint160 notarizingSystemID = notarizingSystem.GetID();

    BlockMap::iterator blockIt;
    if (!vtx.size() ||
        (!IsConfirmed() && vtx[0].second.IsDefinitionNotarization()) ||
        forks[bestChain].size() <= minNotaryConfirms ||
        !((blockIt = mapBlockIndex.find(txAndBlockVec[forks[bestChain].back()].second)) != mapBlockIndex.end() &&
          (height - blockIt->second->GetHeight()) > 0 &&
          (height - blockIt->second->GetHeight()) <= maxDepth))
    {
        return -1;
    }

    if (notarizingSystem.IsPBaaSChain())
    {
        auto targetIt = forks[bestChain].rbegin() + minNotaryConfirms;
        for (;
             targetIt != forks[bestChain].rend();
             targetIt++)
        {
            blockIt = mapBlockIndex.find(txAndBlockVec[*targetIt].second);
            if (blockIt == mapBlockIndex.end())
            {
                LogPrintf("%s: invalid block hash in notarization data for system: %s\n", __func__, ConnectedChains.GetFriendlyCurrencyName(notarizingSystemID).c_str());
                return -1;
            }
            if ((height - blockIt->second->GetHeight()) >= minBlockConfirms)
            {
                break;
            }
        }
        if (targetIt == forks[bestChain].rend())
        {
            return -1;
        }
        return *targetIt;
    }
    else
    {
        // if notarizing a gateway or non-PBaaS chain, don't assume
        // the concept of "more power" is enough
        uint160 currencyID = vtx[0].second.currencyID;

        std::vector<int> bestFork;
        if (forks.size() > 1)
        {
            // need to get most recent forks. if we have
            // no conflict for the past 3 earned notarizations,
            // we are good to confirm, otherwise after 10 blocks, the one
            // that ends up > 2 longer can be notarized/finalized.
            std::multimap<int, std::pair<int, int>> forkAndIndexByBlock;
            for (int i = 0; i < forks.size(); i++)
            {
                auto &oneFork = forks[i];
                for (auto &oneRoot : oneFork)
                {
                    if (vtx[oneRoot].second.proofRoots.count(currencyID))
                    {
                        forkAndIndexByBlock.insert(std::make_pair(vtx[oneRoot].second.proofRoots.find(currencyID)->second.rootHeight, std::make_pair(i, oneRoot)));
                    }
                }
            }

            int forkNum = -1;
            int elemCount = 0;
            for (auto firstIt = forkAndIndexByBlock.rbegin(); firstIt != forkAndIndexByBlock.rend(); firstIt++)
            {
                if (forkNum == -1 || forkNum == firstIt->second.first)
                {
                    elemCount++;
                    forkNum = firstIt->second.first;
                }
                else
                {
                    break;
                }
            }
            if (elemCount >= minNotaryConfirms)
            {
                bestFork = forks[forkNum];
            }
        }
        else if (forks.size() &&
                (forks[0].size() > std::max(minNotaryConfirms, 1) || ConnectedChains.ThisChain().notarizationProtocol != CCurrencyDefinition::NOTARIZATION_AUTO))
        {
            bestFork = forks[0];
        }

        if (bestFork.size() < minNotaryConfirms || !vtx[lastConfirmed].second.proofRoots.count(currencyID))
        {
            LogPrint("notarization", "insufficient validator confirmations\n");
            return -1;
        }

        // all we really want is the system proof roots for each notarization to make the JSON for the API smaller
        UniValue proofRootsUni(UniValue::VARR);

        proofRootsUni.push_back(vtx[lastConfirmed].second.proofRoots.find(currencyID)->second.ToUniValue());

        for (auto forkIdxIt = bestFork.begin(); forkIdxIt != bestFork.end(); forkIdxIt++)
        {
            // don't enter the confirmed notarization twice
            if (*forkIdxIt != lastConfirmed)
            {
                auto &oneNot = vtx[*forkIdxIt];
                auto rootIt = oneNot.second.proofRoots.find(currencyID);
                if (rootIt != oneNot.second.proofRoots.end())
                {
                    proofRootsUni.push_back(rootIt->second.ToUniValue());
                }
                else
                {
                    proofRootsUni.push_back(CProofRoot().ToUniValue());
                }
            }
        }

        if (!proofRootsUni.size())
        {
            LogPrint("notarization", "no valid prior state root found\n");
            return -1;
        }

        UniValue firstParam(UniValue::VOBJ);
        firstParam.push_back(Pair("proofroots", proofRootsUni));
        firstParam.push_back(Pair("lastconfirmed", 0));

        // call notary to determine the notarization that we should notarize
        UniValue params(UniValue::VARR);
        params.push_back(firstParam);

        //printf("%s: about to getbestproofroot with:\n%s\n", __func__, params.write(1,2).c_str());

        UniValue result;
        try
        {
            result = find_value(RPCCallRoot("getbestproofroot", params), "result");
        } catch (exception e)
        {
            result = NullUniValue;
        }

        CProofRoot lastConfirmedRoot(find_value(result, "lastconfirmedproofroot"));
        int32_t notaryIdx = lastConfirmedRoot.IsValid() ? uni_get_int(find_value(result, "lastconfirmedindex"), -1) :
                                                        uni_get_int(find_value(result, "bestindex"), -1);

        if (result.isNull() || notaryIdx == -1)
        {
            LogPrint("notarization", "no-matching-notarization-found\n");
            return -1;
        }

        // now, assuming that our notarization of the other chain is confirmed, sign the latest valid one on this chain
        UniValue proofRootArr = find_value(result, "validindexes");

        if (!proofRootArr.isArray() || !proofRootArr.size())
        {
            LogPrint("notarization", "no valid notarizations found\n");
            return -1;
        }
        return bestFork[notaryIdx];
    }
}

bool IsScriptTooLargeToSpend(const CScript &script)
{
    COptCCParams p;
    if ((script.IsPayToCryptoCondition(p) &&
         p.AsVector().size() >= CScript::MAX_SCRIPT_ELEMENT_SIZE) ||
        script.IsOpReturn())
    {
        return true;
    }
    return false;
}

// this is called to locate any notarizations of a specific system that can be finalized or of called by a notary witness,
// can be signed/witnessed, to determine if we agree with the notarization in question, and to confirm or reject the notarization.
bool CPBaaSNotarization::ConfirmOrRejectNotarizations(CWallet *pWallet,
                                                      const CRPCChainData &externalSystem,
                                                      CValidationState &state,
                                                      std::vector<TransactionBuilder> &txBuilders,
                                                      uint32_t nHeight,
                                                      bool &finalized)
{
    auto txBuilder = TransactionBuilder(Params().GetConsensus(), nHeight, pWallet);

    std::string errorPrefix(strprintf("%s: ", __func__));

    finalized = false;

    CCurrencyDefinition::EHashTypes hashType = (CCurrencyDefinition::EHashTypes)externalSystem.chainDefinition.proofProtocol;

    uint32_t height, signingHeight;
    uint160 SystemID = externalSystem.chainDefinition.GetID();

    const CCurrencyDefinition *pNotaryCurrency;
    if (IsVerusActive() || !ConnectedChains.notarySystems.count(SystemID))
    {
        pNotaryCurrency = &externalSystem.chainDefinition;
    }
    else
    {
        pNotaryCurrency = &ConnectedChains.ThisChain();
    }

    CChainNotarizationData cnd;
    std::vector<std::pair<CTransaction, uint256>> txes;

    std::set<uint160> notarySet = pNotaryCurrency->GetNotarySet();
    int minimumNotariesConfirm = pNotaryCurrency->MinimumNotariesConfirm();
    int confirmIfSigned;
    int confirmIfAuto;

    std::vector<std::pair<CIdentityMapKey, CIdentityMapValue>> mine;
    {
        std::vector<std::pair<CIdentityMapKey, CIdentityMapValue>> imsigner, watchonly;
        LOCK(pWallet->cs_wallet);
        pWallet->GetIdentities(pNotaryCurrency->notaries, mine, imsigner, watchonly);
    }

    {
        LOCK2(cs_main, mempool.cs);
        signingHeight = height = chainActive.Height();

        // we can only create an earned notarization for a notary chain, so there must be a notary chain and a network connection to it
        // we also need to ensure that our notarization would be the first notarization in this notary block period  with which we agree.
        if (!externalSystem.IsValid() || externalSystem.rpcHost.empty())
        {
            // technically not a real error
            return state.Error("no-notary-chain");
        }

        if (!GetNotarizationData(SystemID, cnd, &txes))
        {
            return state.Error(errorPrefix + "no prior notarization found");
        }

        confirmIfSigned =
            cnd.BestConfirmedNotarization(ConnectedChains.ThisChain(),
                                          CPBaaSNotarization::MIN_EARNED_FOR_SIGNED,
                                          CPBaaSNotarization::MinBlocksToSignedNotarization(ConnectedChains.ThisChain().blockNotarizationModulo),
                                          nHeight,
                                          txes);

        confirmIfAuto =
            cnd.BestConfirmedNotarization(ConnectedChains.ThisChain(),
                                          CPBaaSNotarization::MIN_EARNED_FOR_AUTO,
                                          CPBaaSNotarization::MinBlocksToAutoNotarization(ConnectedChains.ThisChain().blockNotarizationModulo),
                                          nHeight,
                                          txes);
    }

    if (cnd.IsConfirmed() && cnd.vtx.size() == 1)
    {
        return state.Error("no-unconfirmed");
    }

    if (height <= ConnectedChains.ThisChain().GetMinBlocksToStartNotarization())
    {
        return state.Error(errorPrefix + "too early");
    }

    // spend all outputs we need to, even if there gets to be too many for 1 tx
    // rules to confirm or reject an earned notarization
    // 1) Notaries may confirm an earned notarization if and only if the following is true:
    //   a) There is no previous, valid notarization in the same eligible period with which the notarization agrees and should confirm
    //   b) If auto-notarization and in a staked block, the last entry to confirm was an earned notarization from a mining block
    //   c) If auto-notarization and making a mined block, the last entry to confirm must be an earned notarization from a staked block
    //   d) The miner or staker has entered accurate information about the other system, which must be confirmed by the notary
    //
    // 2) Each earned notarization in a staked block contains a proof of the stake transaction in the header.
    //
    // 3) Each includes a proof of the prior notarization with which it agrees and either a PoW or PoS proof of its own.
    //
    // 4) Once a notarization gets 3 uncontested and agreed notarizations in a row, notaries of an auto-notarized chain
    //    may sign and confirm, also including the proof of each of the confirmation outputs.
    //
    // if we are auto-notarizing, the following conditions must be true to qualify for us to confirm a notarization:
    // 1) the notarization must be agreed to by 2 or greater valid earned notarizations since the next longest fork.
    //

    // TODO: HARDENING - post evidence to finalize a notarization if the following is true:
    //
    // 1) We agree with the last finalized or a pending fork of notarizations on the other chain
    //    Said differently, we we can only attempt to submit finalization evidence if there is
    //    no pending notarization on the alternate chain that we agree with, which is more recent
    //    than 10x the block notarization modulo (BNM) of both chains.
    //
    //    If we agree with a pending notarization, do not control a notary, and the notarization
    //    we agree with is less than 10x the (BNM) of both chains, we don't submit anything, as our
    //    notarization can never be finalized if someone proves the alternate notarization that
    //    disqualifies us.
    //
    // 2) the last confirmed notarization on this chain occurred more than 10x our period prior
    //    and the other chain has progressed 10x its BNM period in blocks on the proof root, and we
    //    have 10 or more valid notarizations behind this one that we agree with.
    //
    // If #1 and #2 are true, we package up enough proof to make a transaction that can stand alone with evidence
    // that miners and stakers of this chain agree on and there is no valid challenge as we settled on this finalization.
    // It is then submitted as a finalization on this chain and accepted as a finalization. It will later be submitted
    // as an accepted notarization to the other chain to confirm the last pending and become a pending notarization.
    //

    // if there's no hope even for signed confirmation, we're done
    if (confirmIfSigned == -1)
    {
        return state.Error("insufficient validator confirmations");
    }

    // assume the bestchain algorithm does better than we would here. if we don't agree,
    // we may challenge in other places, but we won't be confirming
    std::vector<int> bestFork = cnd.forks[cnd.bestChain];

    bool isFirstConfirmedCND = cnd.vtx[cnd.lastConfirmed].second.IsBlockOneNotarization() ||
                               cnd.vtx[cnd.lastConfirmed].second.IsDefinitionNotarization();

    // only the first actual notarization can have a prior without a proof root
    if (!isFirstConfirmedCND && !cnd.vtx[cnd.lastConfirmed].second.proofRoots.count(SystemID))
    {
        return state.Error("invalid prior notarization confirmation");
    }

    // any valid earned notarization that we choose to use for our proof, which will determine
    // the signing height, enabling all of them to match without additional coordination
    uint32_t eligibleHeight = height - ConnectedChains.ThisChain().GetMinBlocksToSignNotarize();

    // all we really want is the system proof roots for each notarization to make the JSON for the API smaller
    UniValue proofRootsUni(UniValue::VARR);

    // get just the indexes in the fork that we could confirm
    for (auto forkIdxIt = bestFork.begin(); forkIdxIt != bestFork.end(); forkIdxIt++)
    {
        auto rootIt = cnd.vtx[*forkIdxIt].second.proofRoots.find(SystemID);
        if (rootIt != cnd.vtx[*forkIdxIt].second.proofRoots.end())
        {
            proofRootsUni.push_back(rootIt->second.ToUniValue());
        }
        else
        {
            proofRootsUni.push_back(CProofRoot().ToUniValue());
        }
    }

    if (!proofRootsUni.size())
    {
        return state.Error(errorPrefix + "no valid prior state root found");
    }
    else if (proofRootsUni.size() == 1 && !uni_get_int64(proofRootsUni[0]))
    {
        return state.Error(errorPrefix + "no valid notarizations to confirm yet");
    }

    UniValue firstParam(UniValue::VOBJ);
    firstParam.push_back(Pair("proofroots", proofRootsUni));
    firstParam.push_back(Pair("lastconfirmed", 0));

    // call notary to determine the notarization that we should notarize
    UniValue params(UniValue::VARR);
    params.push_back(firstParam);

    //printf("%s: about to getbestproofroot with:\n%s\n", __func__, params.write(1,2).c_str());

    UniValue result;
    try
    {
        result = find_value(RPCCallRoot("getbestproofroot", params), "result");
    } catch (exception e)
    {
        result = NullUniValue;
    }

    CProofRoot lastConfirmedRoot(find_value(result, "lastconfirmedproofroot"));
    int32_t notaryIdx = lastConfirmedRoot.IsValid() ? uni_get_int(find_value(result, "lastconfirmedindex"), -1) :
                                                      uni_get_int(find_value(result, "bestindex"), -1);

    if (result.isNull() || notaryIdx == -1)
    {
        return state.Error(result.isNull() ? "no-notary" : "no-matching-notarization-found");
    }

    // get the index from the best fork to make it an index into cnd.vtx
    notaryIdx = bestFork[notaryIdx];

    // take the lock again, now that we're back from calling out
    LOCK(cs_main);

    // if height changed, we need to fail and possibly try again later
    if (height != chainActive.Height())
    {
        return state.Error("stale-block");
    }

    // if the most recent valid earned notarization is prior to the eligible height,
    // we cannot finalize until a more recent one is mined

    // now, assuming that our notarization of the other chain is confirmed, sign the latest valid one on this chain
    UniValue proofRootArr = find_value(result, "validindexes");

    if (!proofRootArr.isArray() || !proofRootArr.size())
    {
        return state.Error("no-valid-unconfirmed");
    }

    bool retVal = false;
    int firstNotarizationIndex = -1;

    LogPrint("notarization", "%s: proofRootArr: %s\n", __func__, proofRootArr.write().c_str());

    // we need to confirm that validated indexes are all in sync with the best chain from its beginning.
    // if we have a shorter best chain than expected because we don't agree with the end, we can move backwards
    // in best chain if there is room, but to be confirmed, no notarization along the stretch being confirmed may have
    // a verified challenge showing a more powerful chain than the range confirming.

    int verifiedSize;
    for (verifiedSize = 0; verifiedSize < proofRootArr.size(); verifiedSize++)
    {
        if (verifiedSize >= bestFork.size() || uni_get_int(proofRootArr[verifiedSize]) != bestFork[verifiedSize])
        {
            break;
        }
    }

    if (verifiedSize < bestFork.size())
    {
        CChainNotarizationData prunedData = cnd;
        prunedData.forks[prunedData.bestChain].resize(verifiedSize);

        confirmIfSigned =
        prunedData.BestConfirmedNotarization(ConnectedChains.ThisChain(),
                                             CPBaaSNotarization::MIN_EARNED_FOR_SIGNED,
                                             CPBaaSNotarization::MinBlocksToSignedNotarization(ConnectedChains.ThisChain().blockNotarizationModulo),
                                             nHeight,
                                             txes);

        confirmIfAuto =
        prunedData.BestConfirmedNotarization(ConnectedChains.ThisChain(),
                                             CPBaaSNotarization::MIN_EARNED_FOR_AUTO,
                                             CPBaaSNotarization::MinBlocksToAutoNotarization(ConnectedChains.ThisChain().blockNotarizationModulo),
                                             nHeight,
                                             txes);
        if (confirmIfSigned == -1)
        {
            return state.Error("insufficient validator confirmations after pruning disagreements");
        }
    }

    // we can now confirm either confirmIfSigned or confirmIfAuto
    // ensure that there is no loss of primacy during the confirmation range
    bool attemptAutoConfirm = false;
    int idx = confirmIfSigned;
    while (idx > 0)
    {
        // these are deleted, mutated below, and recreated each iteration
        std::set<CIdentityID> myIDSet;
        for (auto &oneID : mine)
        {
            myIDSet.insert(oneID.first.idID);
        }

        // before confirming the one we are about to, we want to ensure that it isn't already signed sufficiently
        // if there are enough signatures to confirm it without signature, make our signature, then create a finalization
        CObjectFinalization of = CObjectFinalization(CObjectFinalization::FINALIZE_NOTARIZATION,
                                                        SystemID,
                                                        cnd.vtx[idx].first.hash,
                                                        cnd.vtx[idx].first.n,
                                                        height);

        std::vector<std::pair<CInputDescriptor, CNotaryEvidence>> additionalEvidence;

        std::set<CIdentityID> sigSet;

        std::set<CIdentityID> notarySetRejects;
        std::map<CIdentityID, CInputDescriptor> notarySetConfirms;

        std::vector<std::vector<CNotaryEvidence>> evidenceVec;
        std::vector<std::vector<CInputDescriptor>> spendsToClose;
        std::vector<CInputDescriptor> extraSpends;

        std::set<int> confirmedOutputNums;
        std::set<int> invalidatedOutputNums;

        std::vector<std::pair<CMempoolAddressDeltaKey, CMempoolAddressDelta>> mempoolUnspent;
        if (mempool.getAddressIndex(
                std::vector<std::pair<uint160, int32_t>>({
                    {CCrossChainRPCData::GetConditionID(CObjectFinalization::ObjectFinalizationFinalizedKey(),
                                                        cnd.vtx[idx].first.hash,
                                                        cnd.vtx[idx].first.n),
                        CScript::P2IDX}}),
                mempoolUnspent) &&
            mempoolUnspent.size())
        {
            for (auto &oneUnspent : mempool.FilterUnspent(mempoolUnspent))
            {
                // if we have a confirmed, valid entry in the mempool already, we don't have something to do
                CTransaction oneTx;
                COptCCParams optP;
                CObjectFinalization checkOf;
                if (mempool.lookup(oneUnspent.first.txhash, oneTx) &&
                    oneTx.vout.size() > oneUnspent.first.index &&
                    oneTx.vout[oneUnspent.first.index].scriptPubKey.IsPayToCryptoCondition(optP) &&
                    optP.IsValid() &&
                    optP.evalCode == EVAL_FINALIZE_NOTARIZATION &&
                    optP.vData.size() &&
                    (checkOf = CObjectFinalization(optP.vData[0])).IsValid() &&
                    checkOf.currencyID == SystemID &&
                    checkOf.IsConfirmed())
                {
                    return state.Error("notarization confirmed in mempool");
                }
            }
        }

        if (!cnd.CorrelatedFinalizationSpends(txes, spendsToClose, extraSpends, &evidenceVec) ||
            !cnd.CalculateConfirmation(idx, confirmedOutputNums, invalidatedOutputNums))
        {
            return state.Error("invalid-correlated-spends");
        }

        if (LogAcceptCategory("notarization"))
        {
            for (int j = 0; j < spendsToClose.size(); j++)
            {
                auto &oneEntrySpends = spendsToClose[j];
                LogPrintf("%s: index #%d - %s\n", __func__, j, confirmedOutputNums.count(j) ? "confirmed" : invalidatedOutputNums.count(j) ? "rejected" : "pending");
                for (auto &oneSpend : oneEntrySpends)
                {
                    UniValue scriptUni(UniValue::VOBJ);
                    ScriptPubKeyToUniv(oneSpend.scriptPubKey, scriptUni, false, false);
                    LogPrintf("txid: %s:%d, nValue: %ld\n", oneSpend.txIn.prevout.hash.GetHex().c_str(), oneSpend.txIn.prevout.n, oneSpend.nValue);
                }
            }
            for (int j = 0; j < evidenceVec.size(); j++)
            {
                auto &oneEvidenceVec = evidenceVec[j];
                LogPrintf("evidence vector: index #%d\n", j);
                for (auto &oneEvidence : oneEvidenceVec)
                {
                    LogPrintf("%s\n", oneEvidence.ToUniValue().write(1,2).c_str());
                }
            }
        }

        CNotaryEvidence ne(ASSETCHAINS_CHAINID, cnd.vtx[idx].first, CNotaryEvidence::STATE_CONFIRMING);
        CCcontract_info CC;
        CCcontract_info *cp;
        std::vector<CTxDestination> dests;

        COptCCParams p;
        if (!(txes[idx].first.vout[ne.output.n].scriptPubKey.IsPayToCryptoCondition(p) &&
                p.IsValid() &&
                p.evalCode != EVAL_NONE))
        {
            return state.Error(errorPrefix + "invalid output for notarization");
        }
        CNativeHashWriter hw(hashType);
        uint256 objHash = hw.write((const char *)&(p.vData[0][0]), p.vData[0].size()).GetHash();

        // combine signatures and evidence
        // add additional evidence spends
        CNotaryEvidence mergedEvidence = ne;
        for (auto &oneEvidenceObj : evidenceVec[idx])
        {
            mergedEvidence.MergeEvidence(oneEvidenceObj, true, true);
        }

        uint32_t decisionHeight;
        std::map<CIdentityID, CIdentitySignature> confirmedAtHeight;
        std::map<CIdentityID, CIdentitySignature> rejectedAtHeight;
        CNotaryEvidence::EStates confirmationResult = mergedEvidence.CheckSignatureConfirmation(objHash,
                                                                                                notarySet,
                                                                                                minimumNotariesConfirm,
                                                                                                signingHeight,
                                                                                                &decisionHeight,
                                                                                                &confirmedAtHeight,
                                                                                                &rejectedAtHeight);

        // rejected is irreversible, so we are done
        if (confirmationResult == CNotaryEvidence::EStates::STATE_REJECTED)
        {
            // we should consider different spends to close when rejecting
            // a notarization, only pruning it and all dependent on it, confirming nothing
            // currently, rejected notarizations would get consumed by confirmed notarizations
        }
        else if (confirmationResult != CNotaryEvidence::EStates::STATE_CONFIRMED &&
                 !attemptAutoConfirm)
        {
            LOCK(cs_main);

            // if we can still sign, do so and check confirmation
            if (myIDSet.size())
            {
                cp = CCinit(&CC, EVAL_NOTARY_EVIDENCE);
                dests = std::vector<CTxDestination>({CPubKey(ParseHex(CC.CChexstr))});

                auto currentSignatures = mergedEvidence.GetNotarySignatures();
                for (auto &oneSignature : currentSignatures)
                {
                    if (oneSignature.IsConfirmed())
                    {
                        for (auto &oneIDSig : oneSignature.signatures)
                        {
                            std::set<CTxDestination> idDestinations;

                            for (auto &oneKeySig : oneIDSig.second.signatures)
                            {
                                // TODO: HARDENING - for efficiency, not security, remove destinations already signed
                            }
                        }
                    }
                }

                {
                    LOCK(pWallet->cs_wallet);
                    // sign with all IDs under our control that are eligible for this currency
                    for (auto &oneID : myIDSet)
                    {
                        printf("Signing notarization (%s:%u) to confirm for %s\n", ne.output.hash.GetHex().c_str(), ne.output.n, EncodeDestination(oneID).c_str());
                        LogPrint("notarization", "Signing notarization (%s:%u) to confirm for %s\n", ne.output.hash.GetHex().c_str(), ne.output.n, EncodeDestination(oneID).c_str());

                        auto signResult = ne.SignConfirmed(notarySet, minimumNotariesConfirm, *pWallet, txes[idx].first, oneID, signingHeight, hashType);

                        if (signResult == CIdentitySignature::SIGNATURE_PARTIAL || signResult == CIdentitySignature::SIGNATURE_COMPLETE)
                        {
                            sigSet.insert(oneID);

                            // if our signatures altogether have provided a complete validation, we can early out
                            // check to see if this notarization now qualifies with signatures
                            if (signResult == CIdentitySignature::SIGNATURE_COMPLETE &&
                                ne.CheckSignatureConfirmation(objHash, notarySet, minimumNotariesConfirm, signingHeight) == CNotaryEvidence::EStates::STATE_CONFIRMED)
                            {
                                break;
                            }
                        }
                        else
                        {
                            return state.Error(errorPrefix + "invalid identity signature");
                        }
                    }
                }

                if (!ne.evidence.Empty())
                {
                    mergedEvidence.MergeEvidence(ne, true, true);

                    retVal = true;
                    CScript evidenceScript = MakeMofNCCScript(CConditionObj<CNotaryEvidence>(EVAL_NOTARY_EVIDENCE, dests, 1, &mergedEvidence));
                    of.evidenceOutputs.push_back(txBuilder.mtx.vout.size());
                    txBuilder.AddTransparentOutput(evidenceScript, CNotaryEvidence::DEFAULT_OUTPUT_VALUE);
                }
            }

            confirmationResult = mergedEvidence.CheckSignatureConfirmation(objHash, notarySet, minimumNotariesConfirm, signingHeight);
        }
        else if (attemptAutoConfirm)
        {
            // we should already have eliminated any reason to attempt auto-confirm if
            // we have a valid index here
            confirmationResult = CNotaryEvidence::EStates::STATE_CONFIRMED;
        }
        if (confirmationResult != CNotaryEvidence::EStates::STATE_CONFIRMED &&
            confirmationResult != CNotaryEvidence::EStates::STATE_REJECTED &&
            pNotaryCurrency->notarizationProtocol == pNotaryCurrency->NOTARIZATION_AUTO &&
            pNotaryCurrency->proofProtocol == pNotaryCurrency->PROOF_PBAASMMR) // not confirmed or rejected - see if we can auto-confirm
        {
            idx = confirmIfAuto;
            attemptAutoConfirm = true;
            continue;
        }

        // if we have enough to finalize, do so as a combination of pre-existing evidence and this
        if (confirmationResult == CNotaryEvidence::EStates::STATE_CONFIRMED)
        {
            std::set<COutPoint> inputSet;
            std::vector<CInputDescriptor> nonEvidenceSpends;
            for (auto &oneEvidenceSpend : spendsToClose[idx])
            {
                COptCCParams tP;
                if (oneEvidenceSpend.scriptPubKey.IsPayToCryptoCondition(tP) &&
                    tP.IsValid() &&
                    tP.evalCode == EVAL_NOTARY_EVIDENCE || tP.evalCode == EVAL_FINALIZE_NOTARIZATION)
                {
                    // if our evidence include an already confirmed finalization for the same output,
                    // abort with nothing to do
                    CObjectFinalization tPOF;
                    if (tP.evalCode == EVAL_FINALIZE_NOTARIZATION &&
                        tP.vData.size() &&
                        (tPOF = CObjectFinalization(tP.vData[0])).IsValid() &&
                        tPOF.output == of.output &&
                        tPOF.IsConfirmed())
                    {
                        return state.Error(errorPrefix + "target notarization already confirmed");
                    }
                    of.evidenceInputs.push_back(txBuilder.mtx.vin.size());
                    if (!inputSet.count(oneEvidenceSpend.txIn.prevout))
                    {
                        inputSet.insert(oneEvidenceSpend.txIn.prevout);
                        if (!IsScriptTooLargeToSpend(oneEvidenceSpend.scriptPubKey))
                        {
                            txBuilder.AddTransparentInput(oneEvidenceSpend.txIn.prevout, oneEvidenceSpend.scriptPubKey, oneEvidenceSpend.nValue);
                        }
                    }
                    else if (LogAcceptCategory("notarization"))
                    {
                        printf("%s: duplicate input: %s\n", __func__, oneEvidenceSpend.txIn.prevout.ToString().c_str());
                        LogPrintf("%s: duplicate input: %s\n", __func__, oneEvidenceSpend.txIn.prevout.ToString().c_str());
                    }
                }
                else
                {
                    nonEvidenceSpends.push_back(oneEvidenceSpend);
                }
            }
            for (auto &oneSpend : nonEvidenceSpends)
            {
                if (!inputSet.count(oneSpend.txIn.prevout))
                {
                    inputSet.insert(oneSpend.txIn.prevout);
                    if (!IsScriptTooLargeToSpend(oneSpend.scriptPubKey))
                    {
                        txBuilder.AddTransparentInput(oneSpend.txIn.prevout, oneSpend.scriptPubKey, oneSpend.nValue);
                    }
                }
                else if (LogAcceptCategory("notarization"))
                {
                    printf("%s: duplicate input: %s\n", __func__, oneSpend.txIn.prevout.ToString().c_str());
                    LogPrintf("%s: duplicate input: %s\n", __func__, oneSpend.txIn.prevout.ToString().c_str());
                }
            }

            int sigCount = 0;

            of.SetConfirmed();

            // any fork that does not match the confirmed fork up to the same index will be
            // considered invalid from all of its entries to its end.
            //
            bool makeInputTxes = confirmedOutputNums.size() > 3;
            for (auto oneConfirmedIdx : confirmedOutputNums)
            {
                if (oneConfirmedIdx != idx)
                {
                    bool makeInputTx = (makeInputTxes && spendsToClose[oneConfirmedIdx].size() > 2);
                    // if we are confirming more
                    // than 2 additional pending notarizations (3 total), we break the spends up into one
                    // transaction to close out each pending transaction into a pending finalization, which
                    // then is spent into the main transaction
                    std::vector<int> evidenceInputs;
                    auto newTxBuilder = TransactionBuilder(Params().GetConsensus(), nHeight, pWallet);
                    TransactionBuilder &oneConfirmedBuilder = (oneConfirmedIdx != cnd.lastConfirmed && makeInputTx) ? newTxBuilder : txBuilder;

                    bool isConfirmed = false;
                    for (auto &oneInput : spendsToClose[oneConfirmedIdx])
                    {
                        COptCCParams tP;
                        if (oneInput.scriptPubKey.IsPayToCryptoCondition(tP) &&
                            tP.IsValid() &&
                            tP.evalCode == EVAL_NOTARY_EVIDENCE || tP.evalCode == EVAL_FINALIZE_NOTARIZATION)
                        {
                            // if our evidence include an already confirmed finalization for the same output,
                            // abort with nothing to do
                            CObjectFinalization tPOF;
                            if (tP.evalCode == EVAL_FINALIZE_NOTARIZATION &&
                                tP.vData.size() &&
                                (tPOF = CObjectFinalization(tP.vData[0])).IsValid() &&
                                tPOF.IsConfirmed() &&
                                ((tPOF.output.hash.IsNull() &&
                                    oneInput.txIn.prevout.hash == cnd.vtx[oneConfirmedIdx].first.hash &&
                                    tPOF.output.n == cnd.vtx[oneConfirmedIdx].first.n) ||
                                    tPOF.output == cnd.vtx[oneConfirmedIdx].first))
                            {
                                isConfirmed = true;
                                makeInputTx = false;
                            }
                        }

                        if (!inputSet.count(oneInput.txIn.prevout))
                        {
                            inputSet.insert(oneInput.txIn.prevout);
                            evidenceInputs.push_back(oneConfirmedBuilder.mtx.vin.size());
                            if (!IsScriptTooLargeToSpend(oneInput.scriptPubKey))
                            {
                                oneConfirmedBuilder.AddTransparentInput(oneInput.txIn.prevout, oneInput.scriptPubKey, oneInput.nValue);
                            }
                        }
                        else if (LogAcceptCategory("notarization"))
                        {
                            printf("%s: duplicate input: %s\n", __func__, oneInput.txIn.prevout.ToString().c_str());
                            LogPrintf("%s: duplicate input: %s\n", __func__, oneInput.txIn.prevout.ToString().c_str());
                        }
                    }

                    if (makeInputTx)
                    {
                        CObjectFinalization oneConfirmedFinalization =
                            CObjectFinalization(isConfirmed ?
                                                    CObjectFinalization::FINALIZE_NOTARIZATION + CObjectFinalization::FINALIZE_CONFIRMED :
                                                    CObjectFinalization::FINALIZE_NOTARIZATION,
                                                SystemID,
                                                cnd.vtx[oneConfirmedIdx].first.hash,
                                                cnd.vtx[oneConfirmedIdx].first.n,
                                                height);

                        cp = CCinit(&CC, EVAL_FINALIZE_NOTARIZATION);
                        dests = std::vector<CTxDestination>({CPubKey(ParseHex(CC.CChexstr))});

                        CScript finalizeScript = MakeMofNCCScript(CConditionObj<CObjectFinalization>(EVAL_FINALIZE_NOTARIZATION, dests, 1, &oneConfirmedFinalization));
                        oneConfirmedBuilder.AddTransparentOutput(finalizeScript, 0);
                        txBuilders.push_back(oneConfirmedBuilder);
                    }
                }
            }

            makeInputTxes = invalidatedOutputNums.size() > 3;
            for (auto oneInvalidatedIdx : invalidatedOutputNums)
            {
                bool makeInputTx = (makeInputTxes && spendsToClose[oneInvalidatedIdx].size() > 2);

                auto newTxBuilder = TransactionBuilder(Params().GetConsensus(), nHeight, pWallet);
                TransactionBuilder &oneInvalidatedBuilder = makeInputTx ? newTxBuilder : txBuilder;

                for (auto &oneInput : spendsToClose[oneInvalidatedIdx])
                {
                    if (!inputSet.count(oneInput.txIn.prevout))
                    {
                        inputSet.insert(oneInput.txIn.prevout);
                        if (!IsScriptTooLargeToSpend(oneInput.scriptPubKey))
                        {
                            oneInvalidatedBuilder.AddTransparentInput(oneInput.txIn.prevout, oneInput.scriptPubKey, oneInput.nValue);
                        }
                    }
                    else if (LogAcceptCategory("notarization"))
                    {
                        printf("%s: duplicate input: %s\n", __func__, oneInput.txIn.prevout.ToString().c_str());
                        LogPrintf("%s: duplicate input: %s\n", __func__, oneInput.txIn.prevout.ToString().c_str());
                    }
                }

                if (makeInputTx)
                {
                    CObjectFinalization oneConfirmedFinalization = CObjectFinalization(CObjectFinalization::FINALIZE_NOTARIZATION,
                                                                                        SystemID,
                                                                                        cnd.vtx[oneInvalidatedIdx].first.hash,
                                                                                        cnd.vtx[oneInvalidatedIdx].first.n,
                                                                                        height);

                    cp = CCinit(&CC, EVAL_FINALIZE_NOTARIZATION);
                    dests = std::vector<CTxDestination>({CPubKey(ParseHex(CC.CChexstr))});

                    CScript finalizeScript = MakeMofNCCScript(CConditionObj<CObjectFinalization>(EVAL_FINALIZE_NOTARIZATION, dests, 1, &oneConfirmedFinalization));
                    oneInvalidatedBuilder.AddTransparentOutput(finalizeScript, 0);
                    txBuilders.push_back(oneInvalidatedBuilder);
                }
            }

            retVal = true;
            finalized = true;
            cp = CCinit(&CC, EVAL_FINALIZE_NOTARIZATION);
            dests = std::vector<CTxDestination>({CPubKey(ParseHex(CC.CChexstr))});

            CScript finalizeScript = MakeMofNCCScript(CConditionObj<CObjectFinalization>(EVAL_FINALIZE_NOTARIZATION, dests, 1, &of));
            txBuilder.AddTransparentOutput(finalizeScript, 0);
            txBuilders.push_back(txBuilder);
        }
        else if (txBuilder.mtx.vout.size())
        {
            txBuilders.push_back(txBuilder);
        }
        break;
    }
    return retVal;
}

bool CallNotary(const CRPCChainData &notarySystem, std::string command, const UniValue &params, UniValue &result, UniValue &error);

bool CPBaaSNotarization::FindEarnedNotarization(CObjectFinalization &confirmedFinalization, CAddressIndexDbEntry *pEarnedNotarizationIndex) const
{
    CAddressIndexDbEntry __earnedNotarizationIndex;
    CAddressIndexDbEntry &earnedNotarizationIndex = pEarnedNotarizationIndex ? *pEarnedNotarizationIndex : __earnedNotarizationIndex;

    bool retVal = false;
    CPBaaSNotarization checkNotarization = *this;
    if (checkNotarization.IsMirror())
    {
        if (!checkNotarization.SetMirror(false))
        {
            LogPrintf("Unable to interpret notarization data for notarization:\n%s\n", checkNotarization.ToUniValue().write(1,2).c_str());
            printf("Unable to interpret notarization data for notarization:\n%s\n", checkNotarization.ToUniValue().write(1,2).c_str());
            return retVal;
        }
    }

    if (LogAcceptCategory("notarization") && LogAcceptCategory("verbose"))
    {
        std::vector<unsigned char> checkHex = ::AsVector(checkNotarization);
        LogPrintf("%s: hex of notarization: %s\n", __func__, HexBytes(&(checkHex[0]), checkHex.size()).c_str());
        printf("%s: hex of notarization: %s\n", __func__, HexBytes(&(checkHex[0]), checkHex.size()).c_str());
    }

    CNativeHashWriter hw;
    hw << checkNotarization;
    uint256 objHash = hw.GetHash();
    uint160 notarizationIdxKey = CCrossChainRPCData::GetConditionID(checkNotarization.currencyID, CPBaaSNotarization::EarnedNotarizationKey(), objHash);

    std::vector<CAddressIndexDbEntry> addressIndex;
    if (!GetAddressIndex(notarizationIdxKey, CScript::P2IDX, addressIndex) || !addressIndex.size())
    {
        LogPrintf("Unable to confirm notarization data for notarization:\n%s\n", checkNotarization.ToUniValue().write(1,2).c_str());
        printf("Unable to confirm notarization data for notarization:\n%s\n", checkNotarization.ToUniValue().write(1,2).c_str());
        return retVal;
    }

    for (auto &oneIndexEntry : addressIndex)
    {
        if (oneIndexEntry.first.spending)
        {
            continue;
        }
        earnedNotarizationIndex = oneIndexEntry;
        break;
    }
    if (!earnedNotarizationIndex.first.blockHeight)
    {
        LogPrintf("Unable to locate transaction for notarization:\n%s\n", checkNotarization.ToUniValue().write(1,2).c_str());
        printf("Unable to locate transaction for notarization:\n%s\n", checkNotarization.ToUniValue().write(1,2).c_str());
        return retVal;
    }

    uint160 finalizedNotarizationKey = CCrossChainRPCData::GetConditionID(CObjectFinalization::ObjectFinalizationFinalizedKey(),
                                                                          earnedNotarizationIndex.first.txhash,
                                                                          earnedNotarizationIndex.first.index);

    addressIndex.clear();
    if (!GetAddressIndex(finalizedNotarizationKey, CScript::P2IDX, addressIndex) ||
         !addressIndex.size())
    {
        confirmedFinalization = CObjectFinalization();
        return true;
    }

    CAddressIndexDbEntry finalizationIndex;
    for (auto &oneIndexEntry : addressIndex)
    {
        if (oneIndexEntry.first.spending)
        {
            continue;
        }
        finalizationIndex = oneIndexEntry;
        break;
    }
    if (!finalizationIndex.first.blockHeight)
    {
        LogPrintf("Unable to locate indexed finalization transaction for notarization:\n%s\n", checkNotarization.ToUniValue().write(1,2).c_str());
        printf("Unable to locate indexed finalization transaction for notarization:\n%s\n", checkNotarization.ToUniValue().write(1,2).c_str());
        return retVal;
    }

    CTransaction finalizationTx;
    uint256 blkHash;
    COptCCParams fP;
    if (!myGetTransaction(finalizationIndex.first.txhash, finalizationTx, blkHash) ||
        finalizationIndex.first.index >= finalizationTx.vout.size() ||
        !(finalizationTx.vout[finalizationIndex.first.index].scriptPubKey.IsPayToCryptoCondition(fP) &&
            fP.IsValid() &&
            fP.evalCode == EVAL_FINALIZE_NOTARIZATION &&
            fP.vData.size() &&
            (confirmedFinalization = CObjectFinalization(fP.vData[0])).IsValid()))
    {
        LogPrintf("Invalid finalization transaction %s for notarization:\n%s\n", finalizationIndex.first.txhash.GetHex().c_str(), checkNotarization.ToUniValue().write(1,2).c_str());
        printf("Invalid finalization transaction %s for notarization:\n%s\n", finalizationIndex.first.txhash.GetHex().c_str(), checkNotarization.ToUniValue().write(1,2).c_str());
        return retVal;
    }

    // if the entry is finalized and not confirmed
    if (!confirmedFinalization.IsConfirmed())
    {
        LogPrint("notarization", "Finalization unconfirmed on transaction %s for notarization:\n%s\n", finalizationIndex.first.txhash.GetHex().c_str(), checkNotarization.ToUniValue().write(1,2).c_str());
        return false;
    }
    return true;
}

// look for finalized notarizations either on chain or in the mempool, which are eligible for submission
// and submit them to the notary chain, referring to the last on the notary chain that we agree with.
std::vector<uint256> CPBaaSNotarization::SubmitFinalizedNotarizations(const CRPCChainData &externalSystem,
                                                                      CValidationState &state)
{
    std::vector<uint256> retVal;

    // look for finalized notarizations for our notary chains in recent blocks
    // if we find any and are connected, submit them
    uint160 systemID = externalSystem.chainDefinition.GetID();

    const CCurrencyDefinition *pNotaryCurrency;
    if (IsVerusActive() || !ConnectedChains.notarySystems.count(systemID))
    {
        pNotaryCurrency = &externalSystem.chainDefinition;
    }
    else
    {
        pNotaryCurrency = &ConnectedChains.ThisChain();
    }

    std::set<uint160> notarySet = pNotaryCurrency->GetNotarySet();

    CChainNotarizationData cnd;
    std::vector<std::pair<CTransaction, uint256>> notarizationTxes;

    // define here to construct in netsted scope and then access below
    CNotaryEvidence allEvidence(CNotaryEvidence::TYPE_NOTARY_EVIDENCE, CNotaryEvidence::VERSION_CURRENT);

    uint32_t nHeight;
    int startingNotarizationIdx;
    int autoNotarizationIdx;

    {
        LOCK2(cs_main, mempool.cs);

        nHeight = chainActive.Height();

        // we need to start with a confirmed notarization and one pending after that, which we agree with to move forward
        // this is to ensure that any transaction we intend to finalize has both a minimum number of mining/staking confirmations
        // and a minimum number of notary confirmation signatures
        if (!GetNotarizationData(systemID, cnd, &notarizationTxes))
        {
            return retVal;
        }
        startingNotarizationIdx =
            cnd.BestConfirmedNotarization(externalSystem.chainDefinition,
                                          CPBaaSNotarization::MIN_EARNED_FOR_SIGNED,
                                          externalSystem.chainDefinition.GetMinBlocksToSignNotarize(),
                                          nHeight,
                                          notarizationTxes);
        autoNotarizationIdx =
            cnd.BestConfirmedNotarization(externalSystem.chainDefinition,
                                          CPBaaSNotarization::MIN_EARNED_FOR_AUTO,
                                          externalSystem.chainDefinition.GetMinBlocksToAutoNotarize(),
                                          nHeight,
                                          notarizationTxes);
    }

    // to submit a confirmed notarization to the alternate chain, we need it to be a signed, confirmed notarization,
    // and 2 earned notarizations that agree to be mined into the chain, look from the end of the vtx
    // to find one we agree with the index returned is the one we agree with, and the one that we will
    // use to prove
    if (startingNotarizationIdx == -1 || cnd.vtx[cnd.lastConfirmed].second.IsBlockOneNotarization())
    {
        if (LogAcceptCategory("verbose"))
        {
            LogPrint("notarization", "Not enough confirmations to submit finalized notarizations for %s\n", EncodeDestination(CIdentityID(systemID)).c_str());
        }
        return retVal;
    }

    // if latest confirmed notarization on this chain is not present on notary chain, we need to submit it and reinforce
    // the last matching notarization from the other system, which should correspond to a prior confirmed notarization
    // on this chain
    UniValue params(UniValue::VARR);
    params.push_back(EncodeDestination(CIdentityID(ASSETCHAINS_CHAINID)));

    UniValue result;
    try
    {
        result = find_value(RPCCallRoot("getnotarizationdata", params), "result");
    } catch (exception e)
    {
        result = NullUniValue;
    }

    CChainNotarizationData crosschainCND;
    std::vector<uint256> blockHashes;
    std::vector<std::vector<std::tuple<CNotaryEvidence, CProofRoot, CProofRoot>>> counterEvidence;

    {
        LOCK(cs_main);
        if (result.isNull() ||
            !(crosschainCND = CChainNotarizationData(result, true, &blockHashes, &counterEvidence)).IsValid() ||
            (!externalSystem.chainDefinition.IsGateway() && !crosschainCND.IsConfirmed()))
        {
            LogPrintf("Unable to get notarization data from %s\n", EncodeDestination(CIdentityID(systemID)).c_str());
            printf("Unable to get notarization data from %s\n", EncodeDestination(CIdentityID(systemID)).c_str());
            return retVal;
        }

        // if the returned data is not confirmed, then it is a gateway that has not yet confirmed its first transaction
        // it may still have unconfirmed notarizations
        // if it is a gateway that is unconfirmed on the other side, we will insert its definition notarization as confirmed,
        // until it is confirmed on its side.
        if (!crosschainCND.IsConfirmed())
        {
            CInputDescriptor notarizationRef;
            CPBaaSNotarization definitionNotarization;
            if (!ConnectedChains.GetDefinitionNotarization(externalSystem.chainDefinition, notarizationRef, definitionNotarization))
            {
                LogPrintf("Unable to get definition notarization for %s\n", EncodeDestination(CIdentityID(systemID)).c_str());
                printf("Unable to get definition notarization for %s\n", EncodeDestination(CIdentityID(systemID)).c_str());
                return retVal;
            }
            crosschainCND.lastConfirmed = 0;
            crosschainCND.vtx.insert(crosschainCND.vtx.begin(), std::make_pair(CUTXORef(notarizationRef.txIn.prevout), definitionNotarization));

            if (crosschainCND.vtx.size() == 1)
            {
                crosschainCND.bestChain = 0;
                crosschainCND.forks = std::vector<std::vector<int>>({std::vector<int>({0})});
            }
            else
            {
                // loop through the forks, insert 0, and increment all following indices
                for (auto &oneFork : crosschainCND.forks)
                {
                    for (auto &oneIndex : oneFork)
                    {
                        oneIndex++;
                    }
                    oneFork.insert(oneFork.begin(), 0);
                }
            }
        }
    }

    // our latest confirmed is what we may submit.
    // if it is already on that chain, we have nothing to do
    if (cnd.forks[cnd.bestChain].size() <= 1 ||
        !cnd.vtx[cnd.forks[cnd.bestChain][1]].second.proofRoots.count(ASSETCHAINS_CHAINID))
    {
        LogPrint("notarization", "No confirming notarization with root for %s\n", EncodeDestination(CIdentityID(systemID)).c_str());
    }

    // if the alternate chain has knowledge of is prelaunch,
    // we will prove the block 1 coinbase with the new confirmed notarization
    // and the confirmed notarization with the next

    uint32_t firstProofHeight = cnd.vtx[cnd.forks[cnd.bestChain][1]].second.proofRoots[ASSETCHAINS_CHAINID].rootHeight;
    CPBaaSNotarization firstProofNotarization = cnd.vtx[cnd.forks[cnd.bestChain][1]].second;
    auto pfirstProofIdxIt = mapBlockIndex.find(notarizationTxes[cnd.lastConfirmed].second);
    if (pfirstProofIdxIt == mapBlockIndex.end())
    {
        LogPrintf("%s: ERROR: block for notarization invalid\n", __func__);
        return retVal;
    }

    CPBaaSNotarization newConfirmedNotarization = cnd.vtx[cnd.lastConfirmed].second;
    auto searchVec = ::AsVector(newConfirmedNotarization);

    // we can assume that all transactions we get back agree with us on the alternate chain, since we are
    // getting them from our merge mining/staking daemon and it would not accept notarizations with proof
    // roots that disagree with it.
    for (int i = 0; i < crosschainCND.vtx.size(); i++)
    {
        auto &oneNotarization = crosschainCND.vtx[i];
        if (oneNotarization.second.IsMirror() &&
            !oneNotarization.second.SetMirror(false))
        {
            LogPrintf("CROSS-CHAIN ERROR: Failure to unmirror notarization from %s\n", EncodeDestination(CIdentityID(systemID)).c_str());
            printf("CROSS-CHAIN ERROR: Failure to unmirror notarization from %s\n", EncodeDestination(CIdentityID(systemID)).c_str());
            return retVal;
        }
        if (searchVec == ::AsVector(oneNotarization.second))
        {
            LogPrint("notarization", "No new confirmed notarizations for %s\n", EncodeDestination(CIdentityID(systemID)).c_str());
            return retVal;
        }

        // all core proof roots that are in a prior, agreed notarization, must be present in the new one,
        // and we must have moved far enough forward from any there that we agree with, or early out
        for (auto &oneProofRoot : oneNotarization.second.proofRoots)
        {
            if (oneProofRoot.first == ASSETCHAINS_CHAINID)
            {
                if (oneProofRoot.second != CProofRoot::GetProofRoot(oneProofRoot.second.rootHeight))
                {
                    // if this has already been confirmed, the bridge to this chain cannot be easily repaired, so we fail
                    if (i == 0)
                    {
                        // TODO: HARDENING - Unless this is discrepancy from an attack, which is the only other option
                        // besides we are on a non-main fork, we should use the node information from the other chain
                        // to connect to new nodes, set the confirmed root from the other chain, and
                        // stop accepting blocks that disagree.
                        //
                        // If we get here on a chain that is not a local fork (figure out how to differentiate), there
                        // is a high chance it is due to a sophisticated chain attack in progress and should not be
                        // possible in any reasonably anticipated circumstance.
                        //
                        LogPrintf("CROSS-CHAIN ERROR: Confirmed notarization on %s does not match this chain\n", EncodeDestination(CIdentityID(systemID)).c_str());
                        printf("CROSS-CHAIN ERROR: Confirmed notarization on %s does not match this chain\n", EncodeDestination(CIdentityID(systemID)).c_str());
                        return retVal;
                    }
                    // it is a non-confirmed notarization on the other chain. we don't agree, but that won't deter us
                    continue;
                }

                if (!cnd.vtx[cnd.lastConfirmed].second.proofRoots.count(oneProofRoot.first) ||
                    cnd.vtx[cnd.lastConfirmed].second.proofRoots[oneProofRoot.first].rootHeight <= oneProofRoot.second.rootHeight)
                {
                    LogPrint("notarization", "Skip submission - no new confirmed notarizations for %s\n", EncodeDestination(CIdentityID(systemID)).c_str());
                    return retVal;
                }
            }
            else if (externalSystem.chainDefinition.IsPBaaSChain() &&
                     oneProofRoot.first == systemID &&
                     (!cnd.vtx[cnd.lastConfirmed].second.proofRoots.count(oneProofRoot.first) ||
                      (cnd.vtx[cnd.lastConfirmed].second.proofRoots[oneProofRoot.first].rootHeight - oneProofRoot.second.rootHeight) <
                           CPBaaSNotarization::GetAdjustedNotarizationModulo(externalSystem.chainDefinition.blockNotarizationModulo,
                                                                             cnd.vtx[cnd.lastConfirmed].second.proofRoots[oneProofRoot.first].rootHeight - oneProofRoot.second.rootHeight)))
            {
                LogPrint("notarization", "Skip submission - no new enough confirmed notarizations for %s\n", EncodeDestination(CIdentityID(systemID)).c_str());
                return retVal;
            }
        }
    }

    CPartialTransactionProof txProofEvidence(CPartialTransactionProof::VERSION_INVALID);
    CTransaction confirmedNTx;

    // check to see if any of the pending notarizations match either our last confirmed
    // or the one before it
    int confirmingIdx = 0;

    bool isFirstLaunchingNotarization = crosschainCND.vtx[confirmingIdx].second.IsDefinitionNotarization() ||
                                        crosschainCND.vtx[confirmingIdx].second.IsPreLaunch();

    CPBaaSNotarization lastConfirmedNotarization = cnd.vtx[cnd.lastConfirmed].second;
    CPBaaSNotarization confirmingLocalNotarization;
    CAddressIndexDbEntry crossConfirmedIndexEntry;
    CAddressIndexDbEntry earnedNotarizationIndexEntry;
    CPBaaSNotarization crossConfirmedNotarization;
    bool isBlock1Proof = false;

    std::vector<std::vector<CInputDescriptor>> spendsToClose;
    std::vector<CInputDescriptor> extraSpends;
    std::vector<std::vector<CNotaryEvidence>> evidenceVec;
    std::pair<CInputDescriptor, CPartialTransactionProof> notarizationTxInfo;

    {
        LOCK2(cs_main, mempool.cs);

        nHeight = chainActive.Height();

        if (crosschainCND.IsConfirmed() || crosschainCND.vtx.size() == 1)
        {
            // ensure that we at least agree with the last notarization being finalized, or we can't submit
            confirmingIdx = crosschainCND.IsConfirmed() ? crosschainCND.lastConfirmed : 0;
            CObjectFinalization finalizationObj;

            if (crosschainCND.vtx.size() != 1)
            {
                // go backwards until we find one to confirm or fail
                for (int i = crosschainCND.vtx.size() - 1; i > 0; i--)
                {
                    CAddressIndexDbEntry tmpIndexEntry;
                    if (crosschainCND.vtx[i].second.proofRoots.count(ASSETCHAINS_CHAINID) &&
                        crosschainCND.vtx[i].second.FindEarnedNotarization(finalizationObj, &tmpIndexEntry) &&
                        finalizationObj.IsConfirmed())
                    {
                        isFirstLaunchingNotarization = false;
                        confirmingIdx = i;
                        earnedNotarizationIndexEntry = crossConfirmedIndexEntry = tmpIndexEntry;
                        confirmingLocalNotarization = crosschainCND.vtx[i].second;
                        confirmingLocalNotarization.SetMirror(false);
                        break;
                    }
                    finalizationObj = CObjectFinalization();
                }
            }

            if (!isFirstLaunchingNotarization)
            {
                if (!confirmingLocalNotarization.IsValid() &&
                    !crosschainCND.vtx[confirmingIdx].second.FindEarnedNotarization(finalizationObj, &crossConfirmedIndexEntry))
                {
                    return retVal;
                }
                else if (!finalizationObj.IsValid() || !finalizationObj.IsConfirmed())
                {
                    // the notarization considered confirmed on the other
                    // chain must be in the same notarization chain as the one confirmed on this chain that would
                    // be true if it is on both chains, accurate on both, and behind this confirmed notarization
                    CTransaction notarizationTx;
                    CPBaaSNotarization checkNotarization1, checkNotarization2 = crosschainCND.vtx[confirmingIdx].second;
                    uint256 blkHash;
                    COptCCParams nP;
                    if (!myGetTransaction(crossConfirmedIndexEntry.first.txhash, notarizationTx, blkHash) ||
                        crossConfirmedIndexEntry.first.index >= notarizationTx.vout.size() ||
                        !(notarizationTx.vout[crossConfirmedIndexEntry.first.index].scriptPubKey.IsPayToCryptoCondition(nP) &&
                          nP.IsValid() &&
                          nP.evalCode == EVAL_EARNEDNOTARIZATION &&
                          nP.vData.size() &&
                          (checkNotarization1 = CPBaaSNotarization(nP.vData[0])).IsValid() &&
                          checkNotarization2.SetMirror(false) &&
                          ::AsVector(checkNotarization1) == ::AsVector(checkNotarization2) &&
                          checkNotarization1.proofRoots.count(ASSETCHAINS_CHAINID) &&
                          cnd.vtx[cnd.lastConfirmed].second.proofRoots.count(ASSETCHAINS_CHAINID) &&
                          checkNotarization1.proofRoots[ASSETCHAINS_CHAINID].rootHeight < cnd.vtx[cnd.lastConfirmed].second.proofRoots[ASSETCHAINS_CHAINID].rootHeight))
                    {
                        LogPrintf("Invalid notarization index entry for txid: %s\n", crossConfirmedIndexEntry.first.txhash.GetHex().c_str());
                        printf("Invalid notarization index entry for txid: %s\n", crossConfirmedIndexEntry.first.txhash.GetHex().c_str());
                        return retVal;
                    }
                    crossConfirmedNotarization = checkNotarization1;
                    crossConfirmedNotarization.SetMirror(true);
                }
            }
        }

        // if we have pending notarizations, see which of the possible ones we agree with
        if (!(crosschainCND.vtx.size() == 1 && isFirstLaunchingNotarization))
        {
            if (!crosschainCND.vtx[confirmingIdx].second.IsDefinitionNotarization())
            {
                // if we found one we agree with, make it the one we prove with our own notarization
                // CPartialTransactionProof(const CTransaction tx,
                //                          const std::vector<int> &inputNums,
                //                          const std::vector<int> &outputNums,
                //                          const CBlockIndex *pIndex,
                //                          uint32_t proofAtHeight)
                uint256 blockHash;
                CBlockIndex *pIndex;
                if (earnedNotarizationIndexEntry.first.txhash.IsNull() ||
                    !myGetTransaction(earnedNotarizationIndexEntry.first.txhash, confirmedNTx, blockHash) ||
                    !mapBlockIndex.count(blockHash) ||
                    !chainActive.Contains(pIndex = mapBlockIndex[blockHash]))
                {
                    LogPrintf("Unable to get confirmed notarization transaction for %s\n", EncodeDestination(CIdentityID(systemID)).c_str());
                    printf("Unable to get confirmed notarization transaction for %s\n", EncodeDestination(CIdentityID(systemID)).c_str());
                    return retVal;
                }
                CPartialTransactionProof firstProof(confirmedNTx,
                                                    std::vector<int>(),
                                                    std::vector<int>({(int)earnedNotarizationIndexEntry.first.index}),
                                                    pIndex,
                                                    newConfirmedNotarization.proofRoots[ASSETCHAINS_CHAINID].rootHeight);
                notarizationTxInfo = std::make_pair(CInputDescriptor(confirmedNTx.vout[earnedNotarizationIndexEntry.first.index].scriptPubKey,
                                                                     confirmedNTx.vout[earnedNotarizationIndexEntry.first.index].nValue,
                                                                     CTxIn(earnedNotarizationIndexEntry.first.txhash, earnedNotarizationIndexEntry.first.index)),
                                                    firstProof);
            }
        }
        if (isFirstLaunchingNotarization)
        {
            ConnectedChains.GetLaunchNotarization(externalSystem.chainDefinition,
                                                  notarizationTxInfo,
                                                  confirmingLocalNotarization,
                                                  newConfirmedNotarization);

            if (!((((PBAAS_TESTMODE && !IsVerusActive() && chainActive[std::min((uint32_t)chainActive.Height(), nHeight)]->nTime < PBAAS_TESTFORK_TIME) ||
                    (confirmingLocalNotarization.IsValid())) &&
                   ConnectedChains.ThisChain().launchSystemID == externalSystem.chainDefinition.GetID()) ||
                   ::AsVector(newConfirmedNotarization) == ::AsVector(cnd.vtx[cnd.lastConfirmed].second)))
            {
                LogPrintf("Unable to get notarization data from %s\n", EncodeDestination(CIdentityID(systemID)).c_str());
                printf("Unable to get notarization data from %s\n", EncodeDestination(CIdentityID(systemID)).c_str());
                return retVal;
            }
            else if (crosschainCND.vtx[confirmingIdx].second.IsPreLaunch() &&
                     confirmingLocalNotarization.IsValid() &&
                     confirmingLocalNotarization.IsBlockOneNotarization() &&
                     confirmingLocalNotarization.currencyID == externalSystem.chainDefinition.GetID() &&
                     ConnectedChains.ThisChain().launchSystemID == externalSystem.chainDefinition.GetID())
            {
                isBlock1Proof = true;
            }
        }

        // we prove our new notarization to submit with the unsubmitted one in front of it
        CBlock block;

        if (!ReadBlockFromDisk(block, pfirstProofIdxIt->second, Params().GetConsensus(), false))
        {
            LogPrintf("%s: ERROR: could not read block one from disk\n", __func__);
            return retVal;
        }
        std::vector<int> outputNums({(int)cnd.vtx[cnd.lastConfirmed].first.n});
        txProofEvidence = CPartialTransactionProof(notarizationTxes[cnd.lastConfirmed].first,
                                                   std::vector<int>({0}),
                                                   outputNums,
                                                   pfirstProofIdxIt->second,
                                                   firstProofHeight);

        if (!isFirstLaunchingNotarization && !(notarizationTxInfo.second.IsValid() && notarizationTxInfo.second.components.size()))
        {
            LogPrintf("Cannot construct notarization proof for %s\n", EncodeDestination(CIdentityID(systemID)).c_str());
            printf("Cannot construct notarization proof for %s\n", EncodeDestination(CIdentityID(systemID)).c_str());
            return retVal;
        }

        if (!cnd.CorrelatedFinalizationSpends(notarizationTxes, spendsToClose, extraSpends, &evidenceVec) ||
             (!evidenceVec[cnd.lastConfirmed].size()))
        {
            LogPrint("notarization", "Cannot correlate adequate proof evidence for %s\n", EncodeDestination(CIdentityID(systemID)).c_str());
            return retVal;
        }

        // now, we have our first proof of our prior, proven by our current, which is new to the notary chain
        // we need to prove the confirmed notarization with one after it and a proof root after that

        // we have:
        // 1) Access to the confirmed earned notarization on this chain, which has been determined
        //    not to have been submitted and mined into the notary chain as a confirmed or pending notarization yet.
        // 2) Proof using our confirmed notarization of a transaction that corresponds to either the last confirmed
        //    notarization on the notary chain or a pending notarization on the notary chain to confirm. In the case
        //    of a gateway, this proof may also apply to the definition transaction.
        //
        // we need to still get:
        // 1) proof root from tip
        // 2) partial transaction proof using the proof root in #1 of the startingNotarizationIdx notarization that we agree with
        // 3) proof using the notarization in #2 of the block behind it, enabling determination of PoW or PoS for notarization
        // 3) partial transaction proof using the notarization in #2 of our confirmed notarization on this chain
        // 4) proof using the confirmed notarization in #3 of the block behind, enabling determination of PoW or PoS for notarization
        // 5) proof using the last confirmed notarization of the notarization already known to the other chain in #2 of what we have above.
        // 6) proof using the last confirmed notarization of the block header ref prior to the previous notarization, enabling
        //    confirmation that the last notarization on the notary chain was earned from a PoS or PoW block

        // get all signature evidence for the confirmed notarization and combine it
        // we don't care about non-signature evidence that we have used to prove to this chain, since
        // we will be proving to the alternate chain things about this chain
        if (evidenceVec.size() > cnd.lastConfirmed)
        {
            for (auto &oneEvidenceObj : evidenceVec[cnd.lastConfirmed])
            {
                allEvidence.MergeEvidence(oneEvidenceObj, true, true);
            }
        }
        if (notarizationTxInfo.second.IsValid() && notarizationTxInfo.second.GetBlockHeight() > 0)
        {
            allEvidence.evidence << notarizationTxInfo.second;
        }
    }

    // allEvidence has all signatures that were used to confirm on this chain
    // now, if we are auto-notarizing, add the supporting evidence
    if (txProofEvidence.IsValid())
    {
        // make sure we have a more recent notarization, after the one we are posting, that is consistent with the other chain/system
        LOCK(cs_main);

        // now, allEvidence has all signatures that were used to confirm on this chain and chain commitments
        // add the supporting evidence
        auto curProofRoot = CProofRoot::GetProofRoot(firstProofHeight);
        allEvidence.evidence << curProofRoot;
        allEvidence.evidence << txProofEvidence;

        uint32_t startHeight = confirmingLocalNotarization.IsValid() ? confirmingLocalNotarization.proofRoots[ASSETCHAINS_CHAINID].rootHeight : 1;
        uint32_t heightChange = newConfirmedNotarization.proofRoots[ASSETCHAINS_CHAINID].rootHeight - startHeight;

        int blocksPerCheckpoint = GetBlocksPerCheckpoint(heightChange);

        // unless we have another proof range sized chunk at the end, don't add another checkpoint
        int numCheckPoints = GetNumCheckpoints(heightChange);

        // let any challengers know where we might diverge roots
        for (int i = 1; i <= numCheckPoints; i++)
        {
            CProofRoot checkpointRoot = CProofRoot::GetProofRoot(startHeight + (i * blocksPerCheckpoint));
            if (!checkpointRoot.IsValid())
            {
                LogPrint("notarization", "Invalid checkpoint, likely from local corruption when preparing proof for notarization to %s\n", EncodeDestination(CIdentityID(systemID)).c_str());
                return retVal;
            }
            allEvidence.evidence << checkpointRoot;
        }

        bool provideFullEvidence = confirmingIdx && counterEvidence.size() > confirmingIdx && counterEvidence[confirmingIdx].size();

        // handle multiple challenges by starting at the earliest
        CProofRoot challengeRoot(CProofRoot::TYPE_PBAAS, CProofRoot::VERSION_INVALID);
        if (provideFullEvidence)
        {
            for (int j = 0; j < counterEvidence[confirmingIdx].size(); j++)
            {
                CProofRoot challengeCandidate(std::get<1>(counterEvidence[confirmingIdx][j]));
                if (challengeCandidate.IsValid())
                {
                    if (!challengeRoot.IsValid() || challengeCandidate.rootHeight < challengeRoot.rootHeight)
                    {
                        challengeRoot = challengeCandidate;
                    }
                }
            }
        }
        if (!challengeRoot.IsValid())
        {
            provideFullEvidence = false;
        }

        // select random block from the last commitments to prove, based on entropy from the recent block, provided by the destination system
        // if we are only confirming an already confirmed notarization on the other chain, no reason to prove prior commitments
        if (provideFullEvidence && !isBlock1Proof && confirmingIdx)
        {
            if (!crosschainCND.vtx[confirmingIdx].second.IsPreLaunch())
            {
                CObjectFinalization finalizationObj;

                if (!crossConfirmedNotarization.IsValid() &&
                    (crosschainCND.vtx[0].second.FindEarnedNotarization(finalizationObj, &crossConfirmedIndexEntry) &&
                     finalizationObj.IsConfirmed()))
                {
                    CTransaction priorConfirmedNTx;
                    uint256 blkHash;
                    COptCCParams nP;
                    if (!crosschainCND.vtx[0].second.IsPreLaunch())
                    {
                       if (!myGetTransaction(crossConfirmedIndexEntry.first.txhash, priorConfirmedNTx, blkHash) ||
                             crossConfirmedIndexEntry.first.index >= priorConfirmedNTx.vout.size() ||
                             !(priorConfirmedNTx.vout[crossConfirmedIndexEntry.first.index].scriptPubKey.IsPayToCryptoCondition(nP) &&
                               nP.IsValid() &&
                               nP.evalCode == EVAL_EARNEDNOTARIZATION &&
                               nP.vData.size() &&
                               (crossConfirmedNotarization = CPBaaSNotarization(nP.vData[0])).IsValid() &&
                               crossConfirmedNotarization.SetMirror(false)))
                        {
                            LogPrint("notarization", "Cannot confirm prior confirmed notarization for %s\n", EncodeDestination(CIdentityID(systemID)).c_str());
                            return retVal;
                        }
                    }
                }

                uint32_t startingHeight = crossConfirmedNotarization.IsValid() ? crossConfirmedNotarization.proofRoots[ASSETCHAINS_CHAINID].rootHeight : 1;

                // last commitments will be in the evidence on the other chain
                // for us, it's more efficient to get the last notarization and recreate them, since they are not stored on this chain
                uint256 entropyHash = blockHashes.size() > confirmingIdx ? blockHashes[confirmingIdx] : uint256();
                std::vector<__uint128_t> smallCommitments =
                    GetBlockCommitments(startingHeight,
                                        crosschainCND.vtx[confirmingIdx].second.proofRoots[ASSETCHAINS_CHAINID].rootHeight, entropyHash);
                int32_t rangeEnd = crosschainCND.vtx[confirmingIdx].second.proofRoots[ASSETCHAINS_CHAINID].rootHeight;
                int32_t rangeLen = rangeEnd - startingHeight++;

                if (smallCommitments.size())
                {
                    // now add the commitments
                    allEvidence.evidence << CHashCommitments(smallCommitments);

                    // get a pseudo-random number from the returned proof root height + the confirming notarization.
                    // from it, select one or more block headers to prove in the range of provided commitments.
                    // if the proof does not match the commitment numbers, it is considered catastrophic.
                    // the bridge will be stopped pending software update and reactivation.
                    // this blocks function in the error case, but since such a discrepancy may
                    // be the result of a blockchain mining/staking/+witness colluding attack (ie. a rogue chain),
                    // it provides yet another proof check to prevent risk of funds loss, making failure
                    // and exposure the most likely attack outcome, even with collaboration of technologists,
                    // miners, stakers, and witnesses

                    auto curMMV = chainActive.GetMMV();
                    curMMV.resize(rangeEnd);

                    int loopNum = std::min(std::max(rangeLen, (int)CPBaaSNotarization::EXPECT_MIN_HEADER_PROOFS),
                                           std::min((int)MAX_HEADER_PROOFS_PER_PROOF, rangeLen / 10));

                    for (int headerLoop = 0; headerLoop < loopNum; headerLoop++)
                    {
                        int indexToProve = (UintToArith256(entropyHash).GetLow64() % rangeLen);
                        entropyHash = ::GetHash(entropyHash);

                        // height is kept shifted up one from the PoS/PoW bit
                        uint32_t blockToProve = startingHeight + indexToProve;;

                        CMMRProof headerProof;
                        if (!chainActive.GetBlockProof(curMMV, headerProof, blockToProve))
                        {
                            LogPrintf("Cannot prove evidence for %s\n", EncodeDestination(CIdentityID(systemID)).c_str());
                            return retVal;
                        }
                        CBlockHeaderAndProof blockHeaderProof(headerProof, chainActive[blockToProve]->GetBlockHeader());

                        allEvidence.evidence << blockHeaderProof;

                        if (blockHeaderProof.blockHeader.IsVerusPOSBlock())
                        {
                            // if this is a proof of stake header, we need to prove:
                            // 1) The PoS transaction at the end of the block and a stakeguard output from the coinbase
                            // 2) If the spender is an ID, prove the last ID to enable signature verification.
                            // 3) The last piece of the puzzle in calculating/verifying PoS hash is determining
                            //    the magic number of the chain, which can be done by the currencyDef.
                            CValidationState state;
                            if (!ProvePosBlock(startingHeight, chainActive[blockToProve], allEvidence, state))
                            {
                                LogPrintf("Cannot prove PoS evidence for %s\n", EncodeDestination(CIdentityID(systemID)).c_str());
                                return retVal;
                            }
                        }
                    }
                }
            }
        }
    }

    uint32_t blocksBeforeModuloExtension = CPBaaSNotarization::GetBlocksBeforeModuloExtension(externalSystem.chainDefinition.blockNotarizationModulo);
    bool submit = GetBoolArg("-alwayssubmitnotarizations", false) ||
                  !crosschainCND.IsConfirmed() ||
                  (!GetBoolArg("-allowdelayednotarizations", false) &&
                   (newConfirmedNotarization.proofRoots[systemID].rootHeight - lastConfirmedNotarization.proofRoots[systemID].rootHeight) >
                   (blocksBeforeModuloExtension - (blocksBeforeModuloExtension >> 2)));

    if (!submit)
    {
        CPBaaSNotarization unMirrored = crosschainCND.vtx[crosschainCND.lastConfirmed].second;
        if (!unMirrored.SetMirror(false))
        {
            submit = true;
        }
        else
        {
            for (auto &oneCurState : lastConfirmedNotarization.currencyStates)
            {
                if (!unMirrored.currencyStates.count(oneCurState.first) ||
                    (unMirrored.currencyStates[oneCurState.first].IsPrelaunch() &&
                     !oneCurState.second.IsPrelaunch()))
                {
                    submit = true;
                    break;
                }

                // if the relative price of the two sided fee currencies has changed more than 15% since last confirmed
                // notarization, submit
                if (oneCurState.second.IsFractional())
                {
                    auto curIdxMap = oneCurState.second.GetReserveMap();
                    uint160 externID = externalSystem.GetID();
                    if (!curIdxMap.count(ASSETCHAINS_CHAINID) || !curIdxMap.count(externID))
                    {
                        continue;
                    }

                    // if we are so far out of range we can't tell, submit
                    if (!(unMirrored.currencyStates[oneCurState.first].PriceInReserve(curIdxMap[externID])) ||
                        !(oneCurState.second.PriceInReserve(curIdxMap[externID])))
                    {
                        submit = true;
                        break;
                    }

                    int64_t firstRatioOfPrice = CCurrencyDefinition::CalculateRatioOfValue(
                                                    unMirrored.currencyStates[oneCurState.first].PriceInReserve(curIdxMap[ASSETCHAINS_CHAINID]),
                                                    unMirrored.currencyStates[oneCurState.first].PriceInReserve(curIdxMap[externID]));
                    int64_t secondRatioOfPrice = CCurrencyDefinition::CalculateRatioOfValue(
                                                    oneCurState.second.PriceInReserve(curIdxMap[ASSETCHAINS_CHAINID]),
                                                    oneCurState.second.PriceInReserve(curIdxMap[externID]));

                    // if second ratio is zero, can't tell, so submit
                    if (!secondRatioOfPrice)
                    {
                        submit = true;
                        break;
                    }

                    int64_t ratioOfPriceChange = CCurrencyDefinition::CalculateRatioOfValue(firstRatioOfPrice, secondRatioOfPrice);

                    // if we go up or down by 10% from the last confirmed notarization, notarize again
                    if (ratioOfPriceChange > (SATOSHIDEN + (SATOSHIDEN / 10)) || ratioOfPriceChange < (SATOSHIDEN - (SATOSHIDEN / 10)))
                    {
                        submit = true;
                        break;
                    }
                }
            }
        }
        if (!submit && lastConfirmedNotarization.proofRoots.count(ASSETCHAINS_CHAINID))
        {
            // check exports in our range
            // submit notarization only if there are exports pending on this chain and the last submitted notarization
            // is too old to be used to prove the exports
            uint32_t lastCrossHeight = (crosschainCND.IsConfirmed() &&
                                        crosschainCND.vtx[crosschainCND.lastConfirmed].second.proofRoots.count(ASSETCHAINS_CHAINID)) ?
                                            crosschainCND.vtx[crosschainCND.lastConfirmed].second.proofRoots[ASSETCHAINS_CHAINID].rootHeight :
                                            0;

            // get exports since last cross height, and if there are any that would be enabled by this notarization, submit
            UniValue params(UniValue::VARR);
            params = UniValue(UniValue::VARR);
            params.push_back(EncodeDestination(CIdentityID(systemID)));
            params.push_back((int64_t)lastCrossHeight);
            params.push_back((int64_t)lastConfirmedNotarization.proofRoots[ASSETCHAINS_CHAINID].rootHeight);

            UniValue result = NullUniValue;
            try
            {
                UniValue getexports(const UniValue& params, bool fHelp);
                result = getexports(params, false);
            } catch (exception e)
            {
                LogPrint("notarization", "Could not determine pending exports to external chain %s\n", uni_get_str(params[0]).c_str());
                return retVal;
            }
            if (result.isArray() && result.size())
            {
                submit = true;
            }
        }
    }

    LogPrint("notarization", "%s: ready to submit accepted notarization with evidence:\n%s\n", __func__, allEvidence.ToUniValue().write(1,2).c_str());
    if (!submit)
    {
        LogPrint("notarization", "skipping submission due to no pending exports or currency transitions\n");
        return retVal;
    }

    // now, we should have enough evidence to prove
    // the notarization. the API call will ensure that we do
    params = UniValue(UniValue::VARR);
    UniValue error;
    std::string strTxId;
    params.push_back(lastConfirmedNotarization.ToUniValue());
    params.push_back(allEvidence.ToUniValue());

    LogPrint("notarization", "submitting notarization with parameters:\n%s\n%s\n", params[0].write(1,2).c_str(), params[1].write(1,2).c_str());

    /*
    printf("%s: initial notarization:\n%s\n", __func__, oneNotarization.first.ToUniValue().write(1,2).c_str());
    std::vector<unsigned char> notVec1 = ::AsVector(oneNotarization.first);
    CPBaaSNotarization checkNotarization(oneNotarization.first.ToUniValue());
    std::vector<unsigned char> notVec2 = ::AsVector(checkNotarization);
    printf("%s: processed notarization:\n%s\n", __func__, checkNotarization.ToUniValue().write(1,2).c_str());
    std::vector<unsigned char> notVec3 = ::AsVector(CPBaaSNotarization(notVec1));
    printf("%s: hex before univalue:\n%s\n", __func__, HexBytes(&(notVec1[0]), notVec1.size()).c_str());
    printf("%s: hex after univalue:\n%s\n", __func__, HexBytes(&(notVec2[0]), notVec2.size()).c_str());
    printf("%s: hex after reserialization:\n%s\n", __func__, HexBytes(&(notVec3[0]), notVec3.size()).c_str()); */

    if (!CallNotary(externalSystem, "submitacceptednotarization", params, result, error) ||
        !error.isNull() ||
        (strTxId = uni_get_str(result)).empty() ||
        !IsHex(strTxId))
    {
        return retVal;
    }
    // store the transaction ID and accepted notariation to prevent redundant submits
    retVal.push_back(uint256S(strTxId));
    return retVal;
}

/*
 * Validates a notarization output spend by ensuring that the spending transaction fulfills all requirements.
 * to accept an earned notarization as valid on the Verus blockchain, it must prove a transaction on the alternate chain, which is
 * either the original chain definition transaction, which CAN and MUST be proven ONLY in block 1, or the latest notarization transaction
 * on the alternate chain that represents an accurate MMR for this chain.
 * In addition, any accepted notarization must fullfill the following requirements:
 * 1) Must prove either a PoS block from the alternate chain or a merge mined block that is owned by the submitter and in either case,
 *    the block must be exactly 8 blocks behind the submitted MMR used for proof.
 * 2) Must prove a chain definition tx and be block 1 or asserts a previous, valid MMR for the notarizing
 *    chain and properly prove objects using that MMR.
 * 3) Must spend the main notarization thread as well as any finalization outputs of either valid or invalid prior
 *    notarizations, and any unspent notarization contributions for this era. May also spend other inputs.
 * 4) Must output:
 *      a) finalization output of the expected reward amount, which will be sent when finalized
 *      b) normal output of reward from validated/finalized input if present, 50% to recipient / 50% to block miner less miner fee this tx
 *      c) main notarization thread output with remaining funds, no other output or fee deduction
 *
 */
bool ValidateAcceptedNotarization(struct CCcontract_info *cp, Eval* eval, const CTransaction &tx, uint32_t nIn, bool fulfilled)
{
    // the spending transaction must be a notarization in the same thread of notarizations
    // check the following things:
    // 1. It represents a valid PoS or merge mined block on the other chain, and contains the header in the opret
    // 2. The MMR and proof provided for the currently asserted block can prove the provided header. The provided
    //    header can prove the last block referenced.
    // 3. This notarization is not a superset of an earlier notarization posted before it that it does not
    //    reference. If that is the case, it is rejected.
    // 4. Has all relevant inputs, including finalizes all necessary transactions, both confirmed and orphaned
    //printf("ValidateAcceptedNotarization\n");

    // first, determine our notarization finalization protocol
    CUTXORef spendingOutput(tx.vin[nIn].prevout.hash, tx.vin[nIn].prevout.n);
    CTransaction sourceTx;
    uint256 blockHash;
    if (!spendingOutput.GetOutputTransaction(sourceTx, blockHash))
    {
        return eval->Error("Unable to retrieve spending transaction for accepted notarization");
    }

    CPBaaSNotarization pbn;
    COptCCParams pN;
    if (!(sourceTx.vout[spendingOutput.n].scriptPubKey.IsPayToCryptoCondition(pN) &&
          pN.IsValid() &&
          pN.evalCode == EVAL_ACCEPTEDNOTARIZATION &&
          pN.vData.size() &&
          (pbn = CPBaaSNotarization(pN.vData[0])).IsValid()))
    {
        return eval->Error("Invalid accepted notarization");
    }

    int i;
    for (i = 0; i < tx.vout.size(); i++)
    {
        COptCCParams p;
        CPBaaSNotarization nextPbn;
        CObjectFinalization of;
        if (tx.vout[i].scriptPubKey.IsPayToCryptoCondition(p) &&
            p.IsValid() &&
            p.evalCode == EVAL_ACCEPTEDNOTARIZATION &&
            p.vData.size() &&
            (nextPbn = CPBaaSNotarization(p.vData[0])).IsValid() &&
            nextPbn.currencyID == pbn.currencyID)
        {
            break;
        }
        if (p.IsValid() &&
            p.evalCode == EVAL_FINALIZE_NOTARIZATION &&
            p.vData.size() &&
            (of = CObjectFinalization(p.vData[0])).IsValid() &&
            ConnectedChains.NotarySystems().count(pbn.currencyID) &&
            ConnectedChains.NotarySystems().find(pbn.currencyID)->second.notaryChain.chainDefinition.IsGateway())
        {
            break;
        }
    }

    if (i < tx.vout.size())
    {
        return true;
    }

    return eval->Error("Accepted notarization can only be spent by a transaction containing another valid notarization");
}

bool PreCheckAcceptedOrEarnedNotarization(const CTransaction &tx, int32_t outNum, CValidationState &state, uint32_t height)
{
    if (IsVerusMainnetActive() && height < 1567999)
    {
        return true;
    }

    // though it would fail below, simplify failure messages pre-pbaas
    if (CConstVerusSolutionVector::GetVersionByHeight(height) < CActivationHeight::ACTIVATE_PBAAS)
    {
        return state.Error("Notarizations not supported before PBaaS activation");
    }
    // TODO: HARDENING - check that all is in place here, especially proof checking of accepted notarizations down below and conditions for
    // earned notarizations

    COptCCParams p;
    CPBaaSNotarization currentNotarization;
    if (!(tx.vout[outNum].scriptPubKey.IsPayToCryptoCondition(p) &&
          p.IsValid() &&
          (p.evalCode == EVAL_ACCEPTEDNOTARIZATION || p.evalCode == EVAL_EARNEDNOTARIZATION) &&
          p.vData.size() &&
          (currentNotarization = CPBaaSNotarization(p.vData[0])).IsValid() &&
          currentNotarization.currencyState.IsValid() &&
          currentNotarization.currencyState.GetID() == currentNotarization.currencyID &&
          p.IsEvalPKOut()))
    {
        return state.Error("Invalid notarization output");
    }

    // check aux destinations if this is an earned notarization, it cannot have auxdests
    // if it is an accepted notarization, it can have 1 auxdest
    if ((p.evalCode == EVAL_EARNEDNOTARIZATION && currentNotarization.proposer.AuxDestCount()) ||
        (p.evalCode == EVAL_ACCEPTEDNOTARIZATION && currentNotarization.proposer.AuxDestCount() > 1))
    {
        return state.Error("Too many auxilliary destinations in notarization proposer");
    }
    else if (p.evalCode == EVAL_ACCEPTEDNOTARIZATION && currentNotarization.proposer.AuxDestCount() > 0)
    {
        CTransferDestination auxDest = currentNotarization.proposer.GetAuxDest(0);
        // ensure that all proposer destinations and aux destinations are relevant for the purpose
        bool fail = false;
        switch (auxDest.TypeNoFlags())
        {
            case auxDest.DEST_PK:
            case auxDest.DEST_PKH:
            case auxDest.DEST_ID:
            {
                break;
            }
            default:
            {
                fail = true;
            }
        }
        if (fail)
        {
            return state.Error("Accepted notarization beneficiary must be public key, publick key hash, or ID destination");
        }
    }

    // ensure that we never accept an invalid proofroot for this chain in a notarization
    if (currentNotarization.proofRoots.count(ASSETCHAINS_CHAINID))
    {
        CProofRoot notarizationRoot = currentNotarization.proofRoots[ASSETCHAINS_CHAINID];
        if (notarizationRoot.rootHeight >= height)
        {
            return true;
        }
        CProofRoot correctRoot = CProofRoot::GetProofRoot(notarizationRoot.rootHeight);

        if (!correctRoot.IsValid())
        {
            return true;
        }

        if (correctRoot != notarizationRoot)
        {
            return state.Error("Incorrect proof root in notarization transaction");
        }

        // earned notarizations are only supported for
        if (p.evalCode == EVAL_EARNEDNOTARIZATION &&
            !currentNotarization.IsBlockOneNotarization() &&
            !currentNotarization.proofRoots.count(currentNotarization.currencyID))
        {
            return state.Error("Earned notarization besides PBaaS block one must contain proof root of system being notarized");
        }
    }
    else if (p.evalCode == EVAL_EARNEDNOTARIZATION &&
             !currentNotarization.IsBlockOneNotarization())
    {
        return state.Error("Earned notarization must contain proof root of current chain");
    }

    // if this is a notarization for this chain's notary chain or system, and this chain is running an auto-notarization protocol,
    // only earned notarizations may be entered, and those notarizations must alternate between being entered by a staker and
    // then a miner
    uint32_t nHeight = chainActive.Height();

    // do full checks if the chain is in sync behind us
    if (nHeight >= (height - 1))
    {
        CCurrencyDefinition curDef = ConnectedChains.GetCachedCurrency(currentNotarization.currencyID);
        if (!curDef.IsValid())
        {
            // the notarization must indicate that it is block one or definition if the currency definition is on
            // the same transaction
            if (currentNotarization.IsDefinitionNotarization() || currentNotarization.IsBlockOneNotarization())
            {
                COptCCParams defP;
                for (auto &oneOut : tx.vout)
                {
                    if (oneOut.scriptPubKey.IsPayToCryptoCondition(defP) &&
                        defP.IsValid() &&
                        defP.evalCode == EVAL_CURRENCY_DEFINITION &&
                        defP.vData.size() &&
                        (curDef = CCurrencyDefinition(defP.vData[0])).IsValid() &&
                        curDef.IsValid() &&
                        curDef.GetID() == currentNotarization.currencyID)
                    {
                        break;
                    }
                    else
                    {
                        curDef.nVersion = curDef.VERSION_INVALID;
                    }
                }
            }
            if (!curDef.IsValid())
            {
                if (LogAcceptCategory("notarization"))
                {
                    UniValue jsonNTx(UniValue::VOBJ);
                    TxToUniv(tx, uint256(), jsonNTx);
                    LogPrintf("%s: Unable to retrieve notarization currency on tx:\n%s\n", __func__, jsonNTx.write(1,2).c_str());
                }

                return state.Error("Unable to retrieve notarizing currency 1");
            }
        }

        // ensure that on a chain requiring earned notarizations, we do not enter new chain roots in the form of
        // accepted notarizations. accepted notarizations can be generated on cross-chain imports, and in that case,
        // may only include proof roots from a prior confirmed notarization.

        // if this is one of the recognized notary chains, the notarizations are "earned" by miners and stakers,
        // if not a notary chain, the currency or chain must have been launched by this chain and notarizations are "accepted"
        // either unquestioningly for notarization protocols other than auto notarization or with auto-notarization
        // requirements
        CPBaaSNotarization priorNotarization;
        if (ConnectedChains.notarySystems.count(currentNotarization.currencyID) ||
            p.evalCode == EVAL_EARNEDNOTARIZATION)
        {
            // chain roots may only come from earned notarizations that alternate between staked and mined
            // blocks. accepted notarizations may include chain roots from currently confirmed notarizations
            if (p.evalCode == EVAL_ACCEPTEDNOTARIZATION)
            {
                // this should only be present as an accepted notarization for the notary chain in any normal case
                // on an import transaction.
                // ensure that the proof root accurately reflects the last earned and confirmed notarization.
                if (currentNotarization.currencyID != ASSETCHAINS_CHAINID && currentNotarization.proofRoots.count(currentNotarization.currencyID))
                {
                    CTransaction priorNotTx;
                    uint256 hashBlock;
                    COptCCParams priorP;
                    // if there is a proof root in this accepted notarization, it must be as part of an import
                    // and the proof root should match the evidence notarization reference used to prove the
                    // import.
                    if (currentNotarization.prevNotarization.IsNull() ||
                        !myGetTransaction(currentNotarization.prevNotarization.hash, priorNotTx, hashBlock) ||
                        priorNotTx.vout.size() <= currentNotarization.prevNotarization.n ||
                        !priorNotTx.vout[currentNotarization.prevNotarization.n].scriptPubKey.IsPayToCryptoCondition(priorP) ||
                        !priorP.IsValid() ||
                        (priorP.evalCode != EVAL_ACCEPTEDNOTARIZATION && priorP.evalCode != EVAL_EARNEDNOTARIZATION) ||
                        priorP.vData.size() < 1 ||
                        !(priorNotarization = CPBaaSNotarization(priorP.vData[0])).IsValid() ||
                        priorNotarization.currencyID != currentNotarization.currencyID ||
                        !priorNotarization.proofRoots.count(currentNotarization.currencyID) ||
                        priorNotarization.proofRoots[currentNotarization.currencyID] != currentNotarization.proofRoots[currentNotarization.currencyID])
                    {
                        return state.Error("Invalid prior notarization");
                    }
                }
                if (currentNotarization.IsDefinitionNotarization())
                {
                    // let it go and if valid, it will be confirmed
                    // TODO: HARDENING - add something meaningful or delete this section
                }
            }
            else
            {
                // this is an earned notarization, ensure that the proof root of the current chain is correct as of the
                // specified block and that the notarization follows all other rules as well (alt stake/work, etc.)
                if (!tx.IsCoinBase())
                {
                    return state.Error("Earned notarization must be an output on coinbase transaction");
                }

                if ((curDef.IsPBaaSChain() || curDef.IsGateway()) &&
                     curDef.SystemOrGatewayID() != ASSETCHAINS_CHAINID)
                {
                    if (!currentNotarization.proofRoots.count(currentNotarization.currencyID) ||
                        (currentNotarization.IsBlockOneNotarization() &&
                         currentNotarization.proofRoots.count(ASSETCHAINS_CHAINID) &&
                         currentNotarization.proofRoots[ASSETCHAINS_CHAINID].rootHeight != 0) ||
                        (!currentNotarization.IsBlockOneNotarization() &&
                         (currentNotarization.proofRoots.size() < 2 ||
                         !currentNotarization.proofRoots.count(ASSETCHAINS_CHAINID) ||
                         currentNotarization.proofRoots[ASSETCHAINS_CHAINID] !=
                            CProofRoot::GetProofRoot(currentNotarization.proofRoots[ASSETCHAINS_CHAINID].rootHeight))))
                    {
                        LogPrint("notarization", "%s: Invalid earned notarization:\n%s\n", __func__, currentNotarization.ToUniValue().write(1,2).c_str());
                        return state.Error("Earned notarizations must contain valid, matching proof roots for current chain and notary chain");
                    }

                    // TODO: HARDENING - ensure the alternating PoS/PoW block state is checked when block is checked

                    if (!currentNotarization.IsBlockOneNotarization() && !currentNotarization.IsDefinitionNotarization())
                    {
                        // ensure that this notarization does not skip any valid notarization behind us
                        // whether or not this block alternates PoS and PoW is not checked here, as it is
                        // not natural for us to know that block information now
                        CTransaction priorNotTx;
                        uint256 hashBlock;
                        COptCCParams priorP;

                        // if there is a proof root in this accepted notarization, it must be as part of an import
                        // and the proof root should match the evidence notarization reference used to prove the
                        // import.
                        if (currentNotarization.prevNotarization.IsNull() ||
                            !myGetTransaction(currentNotarization.prevNotarization.hash, priorNotTx, hashBlock) ||
                            priorNotTx.vout.size() <= currentNotarization.prevNotarization.n ||
                            !priorNotTx.vout[currentNotarization.prevNotarization.n].scriptPubKey.IsPayToCryptoCondition(priorP) ||
                            !priorP.IsValid() ||
                            (priorP.evalCode != EVAL_ACCEPTEDNOTARIZATION && priorP.evalCode != EVAL_EARNEDNOTARIZATION) ||
                            priorP.vData.size() < 1 ||
                            !(priorNotarization = CPBaaSNotarization(priorP.vData[0])).IsValid() ||
                            (priorP.evalCode == EVAL_ACCEPTEDNOTARIZATION &&
                            !priorNotarization.IsBlockOneNotarization() &&
                            !priorNotarization.IsDefinitionNotarization()))
                        {
                            return state.Error("Invalid prior for earned notarization");
                        }
                    }
                }
            }
        }
        else
        {
            // either this is another system entering an accepted notarization or one coming from an import or pre-launch export
            // determine which, based on the tx and presence of an import
            if (currentNotarization.IsPreLaunch())
            {
                // export or launch notarization
                if (p.evalCode == EVAL_EARNEDNOTARIZATION)
                {
                    return state.Error("Earned notarizations cannot be for pre-launch currencies");
                }

                // ensure that this is part of an export transaction
                bool exportOutNum = -1;
                CCrossChainExport exportToCheck;
                for (int loop = 0; loop < tx.vout.size(); loop++)
                {
                    if ((exportToCheck = CCrossChainExport(tx.vout[loop].scriptPubKey)).IsValid() &&
                        !exportToCheck.IsSupplemental() &&
                        !exportToCheck.IsSystemThreadExport() &&
                        exportToCheck.destCurrencyID == currentNotarization.currencyID)
                    {
                        exportOutNum = loop;
                        break;
                    }
                    else
                    {
                        exportToCheck = CCrossChainExport();
                    }
                }

                // export or launch notarization
                if (exportOutNum < 0 || !exportToCheck.IsValid() || !exportToCheck.IsPrelaunch())
                {
                    return state.Error("Prelaunch notarization with no export on transaction");
                }
                // precheck on cross chain export will create a new export from transfers and the last export
                // and ensure that it matches
            }
            else
            {
                if (!currentNotarization.IsRefunding() &&
                    (curDef.IsPBaaSChain() || (curDef.IsGateway() && !currentNotarization.IsDefinitionNotarization())) &&
                     curDef.SystemOrGatewayID() != ASSETCHAINS_CHAINID)
                {
                    // we should only approve accepted notarizations that have the following qualifications
                    // for the following protocols:
                    // NOTARIZATION_AUTO        -   must have evidence to prove that one recent earned notarization
                    //                              from either a PoW or PoS block refers to and proves another alternated
                    //                              and proven PoW or PoS earned notarization, which then alternates and proves
                    //                              the notarization being confirmed. This allows us to post it, but it must mature
                    //                              N blocks without being rejected by the notaries to be considered confirmed.
                    // NOTARIZATION_NOTARY_CONFIRM - requires no evidence beyond notary signatures
                    // NOTARIZATION_NOTARY_CHAINID - requires no evidence beyond chain signature

                    // first get evidence and verify signatures

                    // TODO: HARDENING - also check blockchain for additional evidence to ensure it is all incorporated

                    int afterEvidence;
                    CNotaryEvidence evidence(tx, outNum + 1, afterEvidence);
                    if (!evidence.IsValid())
                    {
                        return state.Error("Invalid notary evidence for accepted notarization");
                    }
                    auto notarySignatures = evidence.GetNotarySignatures();
                    if (!notarySignatures.size() ||
                        !notarySignatures[0].signatures.size())
                    {
                        return state.Error("Notary signatures may not be empty for accepted notarization");
                    }
                    CNativeHashWriter hw((CCurrencyDefinition::EHashTypes)(notarySignatures[0].signatures.begin()->second.hashType));
                    CPBaaSNotarization normalizedNotarization = currentNotarization;
                    if (!currentNotarization.SetMirror(false))
                    {
                        return state.Error("Notarization cannot be unmirrored");
                    }
                    hw << currentNotarization;
                    if (evidence.CheckSignatureConfirmation(hw.GetHash(), curDef.GetNotarySet(), curDef.minNotariesConfirm, height - 1) !=
                        CNotaryEvidence::EStates::STATE_CONFIRMED)
                    {
                        if (LogAcceptCategory("notarization"))
                        {
                            LogPrintf("Cannot confirm notary signatures for evidence: %s\n", evidence.ToUniValue().write(1,2).c_str());
                        }
                        return state.Error("Cannot confirm notary signatures for notarization");
                    }

                    if (LogAcceptCategory("notarization") && LogAcceptCategory("verbose"))
                    {
                        LogPrintf("checkpoint for evidence: %s\n", evidence.ToUniValue().write(1,2).c_str());
                    }

                    // each PBaaS chain notarization will also include a proof of the notarization
                    // transaction in the case of block 1 of a PBaaS chain launch, it will include a proof of the
                    // entire block 1 coinbase transaction, which can be reconstructed and proven here
                    // and accepted only if all currency definition rules were properly followed
                    if (curDef.IsPBaaSChain())
                    {
                        // if this is the first notarization since block 1, it contains a full proof of the block 1 coinbase transaction
                        // ensure that it does and that a recreation of it results in the exact same outputs, except for miner rewards,
                        // which whould total to the same values, but may have different recipients

                        CTransaction priorNotTx;
                        uint256 hashBlock;
                        COptCCParams priorP;

                        if (!myGetTransaction(tx.vin[0].prevout.hash, priorNotTx, hashBlock) ||
                            priorNotTx.vout.size() <= tx.vin[0].prevout.n ||
                            !priorNotTx.vout[tx.vin[0].prevout.n].scriptPubKey.IsPayToCryptoCondition(priorP) ||
                            !priorP.IsValid() ||
                            (priorP.evalCode != EVAL_ACCEPTEDNOTARIZATION && priorP.evalCode != EVAL_EARNEDNOTARIZATION) ||
                            priorP.vData.size() < 1 ||
                            !(priorNotarization = CPBaaSNotarization(priorP.vData[0])).IsValid() ||
                            priorNotarization.currencyID != normalizedNotarization.currencyID)
                        {
                            return state.Error("Invalid prior notarization 2");
                        }

                        if (LogAcceptCategory("notarization"))
                        {
                            printf("%s: last notarization: %s\n", __func__, priorNotarization.ToUniValue().write(1,2).c_str());
                            LogPrintf("%s: last notarization: %s\n", __func__, priorNotarization.ToUniValue().write(1,2).c_str());
                        }

                        // get last confirmed notarization if there is one for last confirmed height param
                        auto lastConfirmedNotarizationInfo = GetLastConfirmedNotarization(normalizedNotarization.currencyID, height - 1);
                        if (!std::get<0>(lastConfirmedNotarizationInfo))
                        {
                            return state.Error("Unable to get prior accepted notarization");
                        }

                        CPBaaSNotarization verifiedNotarization = IsValidPrimaryChainEvidence(curDef,
                                                                                              evidence,
                                                                                              normalizedNotarization,
                                                                                              std::get<0>(lastConfirmedNotarizationInfo),
                                                                                              height - 1);
                        if (!verifiedNotarization.IsValid())
                        {
                            return state.Error("Unable to verify accepted notarization");
                        }
                    }
                }
            }
        }
    }
    return true;
}

LRUCache<CUTXORef, std::tuple<CTransaction, std::vector<std::pair<CObjectFinalization, CNotaryEvidence>>>> finalizationEvidenceCache(100, 0.3);

// get and aggregate all evidence from a finalization
std::vector<std::pair<CObjectFinalization, CNotaryEvidence>>
CObjectFinalization::GetFinalizationEvidence(const CTransaction &thisTx, int32_t outNum, CValidationState &state, CTransaction *pOutputTx) const
{
    std::tuple<CTransaction, std::vector<std::pair<CObjectFinalization, CNotaryEvidence>>> cachedReturn;
    CUTXORef cacheKey = CUTXORef(thisTx.GetHash(), outNum);
    if (finalizationEvidenceCache.Get(cacheKey, cachedReturn))
    {
        return std::get<1>(cachedReturn);
    }

    CTransaction _outputTx;
    CUTXORef fullOutput(output);
    uint256 outputTxBlockHash;
    CTransaction &finalizedTx = pOutputTx ? *pOutputTx : _outputTx;

    std::vector<std::pair<CObjectFinalization, CNotaryEvidence>> emptyVec;

    if (fullOutput.hash.IsNull())
    {
        fullOutput.hash = thisTx.GetHash();
    }

    if (!GetOutputTransaction(thisTx, finalizedTx, outputTxBlockHash))
    {
        state.Error(std::string(__func__) + ": cannot retrieve output transaction for finalization");
        LogPrint("notarization", "%s: cannot retrieve output transaction for finalization\n", __func__);
        return emptyVec;
    }

    CPBaaSNotarization notarization;
    std::vector<std::pair<CObjectFinalization, CNotaryEvidence>> evidenceVec;

    COptCCParams p;
    if (!(finalizedTx.vout[output.n].scriptPubKey.IsPayToCryptoCondition(p) &&
          p.IsValid() &&
          (p.evalCode == EVAL_ACCEPTEDNOTARIZATION || p.evalCode == EVAL_EARNEDNOTARIZATION) &&
          p.vData.size() &&
          (notarization = CPBaaSNotarization(p.vData[0])).IsValid()))
    {
        state.Error(std::string(__func__) + ": invalid notarization output for finalization");
        LogPrint("notarization", "%s: Invalid notarization output for finalization\n", __func__);
        return emptyVec;
    }

    // collect all evidence, outputs first
    if (evidenceOutputs.size() || evidenceInputs.size())
    {
        CTransaction evidenceTx;

        // add evidence outs as spends to close this entry as well
        int afterMultiPart = evidenceOutputs.size() ? evidenceOutputs[0] : 0;
        for (int i = 0; i < evidenceOutputs.size(); i++)
        {
            auto oneEvidenceOutN = evidenceOutputs[i];
            if (thisTx.vout.size() <= oneEvidenceOutN)
            {
                state.Error(std::string(__func__) + ": indexing error for finalization evidence");
                LogPrint("notarization", "%s: indexing error for finalization evidence\n", __func__);
                return emptyVec;
            }

            if (oneEvidenceOutN >= afterMultiPart)
            {
                CNotaryEvidence oneEvidenceObj(thisTx, oneEvidenceOutN, afterMultiPart);
                if (oneEvidenceObj.IsValid())
                {
                    if (evidenceVec.size())
                    {
                        evidenceVec[0].second.MergeEvidence(oneEvidenceObj);
                    }
                    else
                    {
                        evidenceVec.push_back(std::make_pair(*this, oneEvidenceObj));
                    }
                }
            }
        }

        CTransaction priorOutputTx;
        CNotaryEvidence oneEvidenceObj;
        std::vector<CNotaryEvidence> evidenceInVec;
        CObjectFinalization priorFinalization;
        for (int i = 0; i < evidenceInputs.size(); i++)
        {
            auto oneIn = evidenceInputs[i];
            uint256 hashBlock;
            if (priorOutputTx.GetHash() != thisTx.vin[oneIn].prevout.hash &&
                !myGetTransaction(thisTx.vin[oneIn].prevout.hash, priorOutputTx, hashBlock))
            {
                state.Error(std::string(__func__) + ": cannot access transaction " + thisTx.vin[oneIn].prevout.hash.GetHex() + " for notarization evidence");
                LogPrint("notarization", "%s: cannot access transaction %s for notarization evidence\n", __func__, thisTx.vin[oneIn].prevout.hash.GetHex().c_str());
                return emptyVec;
            }

            COptCCParams inP;
            if (priorOutputTx.vout[thisTx.vin[oneIn].prevout.n].scriptPubKey.IsPayToCryptoCondition(inP) &&
                inP.IsValid() &&
                inP.evalCode == EVAL_NOTARY_EVIDENCE &&
                inP.vData.size() &&
                (oneEvidenceObj = CNotaryEvidence(inP.vData[0])).IsValid() &&
                oneEvidenceObj.evidence.chainObjects.size())
            {
                // if we are starting a new object, finish the old one
                if (!oneEvidenceObj.IsMultipartProof() ||
                    ((CChainObject<CEvidenceData> *)oneEvidenceObj.evidence.chainObjects[0])->object.md.index == 0)
                {
                    // if we have a composite evidence object, store it and clear the vector
                    if (evidenceInVec.size())
                    {
                        CNotaryEvidence multiPartEvidence(evidenceInVec);
                        if (multiPartEvidence.IsValid())
                        {
                            if (evidenceVec.size())
                            {
                                evidenceVec[0].second.MergeEvidence(multiPartEvidence);
                            }
                            else
                            {
                                evidenceVec.push_back(std::make_pair(*this, multiPartEvidence));
                            }
                        }
                        evidenceInVec.clear();
                    }
                }

                if (!oneEvidenceObj.IsMultipartProof())
                {
                    evidenceVec.push_back(std::make_pair(*this, oneEvidenceObj));
                }
                else
                {
                    evidenceInVec.push_back(oneEvidenceObj);
                }
            }
            else if (inP.IsValid() &&
                     inP.evalCode == EVAL_FINALIZE_NOTARIZATION &&
                     inP.vData.size() &&
                     (priorFinalization = CObjectFinalization(inP.vData[0])).IsValid())
            {
                // cleanup any pending multipart
                if (evidenceInVec.size())
                {
                    CNotaryEvidence multiPartEvidence(evidenceInVec);
                    if (multiPartEvidence.IsValid())
                    {
                        if (evidenceVec.size())
                        {
                            evidenceVec[0].second.MergeEvidence(multiPartEvidence);
                        }
                        else
                        {
                            evidenceVec.push_back(std::make_pair(*this, multiPartEvidence));
                        }
                    }
                    evidenceInVec.clear();
                }

                CTransaction priorFinalizationTx;
                auto priorFinalizationVec = priorFinalization.GetFinalizationEvidence(priorOutputTx, thisTx.vin[oneIn].prevout.n, state, &priorFinalizationTx);
                evidenceVec.insert(evidenceVec.end(), priorFinalizationVec.begin(), priorFinalizationVec.end());
            }
        }

        // if we have a composite evidence object, store it and clear the vector
        if (evidenceInVec.size())
        {
            CNotaryEvidence multiPartEvidence(evidenceInVec);
            if (multiPartEvidence.IsValid())
            {
                if (evidenceVec.size())
                {
                    evidenceVec[0].second.MergeEvidence(multiPartEvidence);
                }
                else
                {
                    evidenceVec.push_back(std::make_pair(*this, multiPartEvidence));
                }
            }
            else
            {
                state.Error(std::string(__func__) + ": invalid multipart evidence on input");
                LogPrint("notarization", "%s: invalid multipart evidence on input\n", __func__);
                return emptyVec;
            }
        }
    }
    if (evidenceVec.size())
    {
        finalizationEvidenceCache.Put(cacheKey, {finalizedTx, evidenceVec});
    }
    return evidenceVec;
}

bool PreCheckNotaryEvidence(const CTransaction &tx, int32_t outNum, CValidationState &state, uint32_t height)
{
    COptCCParams p;
    CNotaryEvidence currentEvidence;
    if (!(tx.vout[outNum].scriptPubKey.IsPayToCryptoCondition(p) &&
          p.IsValid() &&
          p.evalCode == EVAL_NOTARY_EVIDENCE &&
          p.vData.size() &&
          (currentEvidence = CNotaryEvidence(p.vData[0])).IsValid()) &&
          p.IsEvalPKOut())
    {
        return state.Error("Invalid notary evidence output");
    }
    // if multipart data, get the full evidence, and if this is the first
    // output with it, continue verification, otherwise, consider it done
    int32_t afterEvidence = 0;
    if (currentEvidence.type == currentEvidence.TYPE_MULTIPART_DATA)
    {
        if (currentEvidence.evidence.chainObjects.size() != 1 ||
            currentEvidence.evidence.chainObjects[0]->objectType != CHAINOBJ_EVIDENCEDATA)
        {
            return state.Error("Invalid multipart evidence output");
        }
        if (((CChainObject<CEvidenceData> *)(currentEvidence.evidence.chainObjects[0]))->object.type == CEvidenceData::TYPE_MULTIPART_DATA)
        {
            COptCCParams multiP;
            CNotaryEvidence multiEvidence;
            int multiStart = (outNum - ((CChainObject<CEvidenceData> *)(currentEvidence.evidence.chainObjects[0]))->object.md.index);
            if (multiStart > 0 &&
                tx.vout[multiStart].scriptPubKey.IsPayToCryptoCondition(multiP) &&
                multiP.IsValid() &&
                multiP.evalCode == EVAL_NOTARY_EVIDENCE &&
                multiP.vData.size() &&
                (multiEvidence = CNotaryEvidence(multiP.vData[0])).IsValid() &&
                multiEvidence.type == CEvidenceData::TYPE_MULTIPART_DATA &&
                multiEvidence.evidence.chainObjects.size() == 1 &&
                multiEvidence.evidence.chainObjects[0]->objectType == CHAINOBJ_EVIDENCEDATA &&
                ((CChainObject<CEvidenceData> *)(multiEvidence.evidence.chainObjects[0]))->object.type == CEvidenceData::TYPE_MULTIPART_DATA &&
                ((CChainObject<CEvidenceData> *)(multiEvidence.evidence.chainObjects[0]))->object.md.index == 0 &&
                (multiEvidence = CNotaryEvidence(tx, multiStart, afterEvidence)).IsValid() &&
                afterEvidence > outNum)
            {
                if (multiStart != outNum)
                {
                    return true;
                }
                currentEvidence = multiEvidence;
            }
            else
            {
                return state.Error("Invalid multipart evidence");
            }
        }
    }
    // now verify that the evidence we have has the following properties:
    // 1) if it is import proof evidence, ensure it has one partial transaction proof
    // 2) if notary proof evidence, it should have the following properties:
    //  a) output that references a notarization
    //  b) either rejection based on more powerful chain or rejecting signatures
    //  c) or confirming signatures and if accepted notarization, additional proof evidence
    //
    if (currentEvidence.type != currentEvidence.TYPE_IMPORT_PROOF &&
        currentEvidence.type != currentEvidence.TYPE_NOTARY_EVIDENCE)
    {
        return state.Error("Invalid evidence type");
    }

    bool inSync = height <= (chainActive.Height() + 1);

    // if this is notary evidence, it must reference a notarization and be a confirmation signed by a notary, or a rejection that may either be signed
    // or provide proof of a more powerful chain
    if (currentEvidence.type == currentEvidence.TYPE_NOTARY_EVIDENCE)
    {
        // get notarization
        CTransaction outputTx;
        uint256 blockHash;
        if (currentEvidence.output.IsOnSameTransaction())
        {
            outputTx = tx;
        }
        else
        {
            if (currentEvidence.systemID != ASSETCHAINS_CHAINID || !currentEvidence.output.GetOutputTransaction(outputTx, blockHash))
            {
                if (!inSync || currentEvidence.systemID != ASSETCHAINS_CHAINID)
                {
                    return true;
                }
                else
                {
                    return state.Error("Cannot get underlying notarization");
                }
            }
        }

        COptCCParams notaP;
        CPBaaSNotarization notarizationForEvidence;
        if (!(currentEvidence.output.n < outputTx.vout.size() &&
              outputTx.vout[currentEvidence.output.n].scriptPubKey.IsPayToCryptoCondition(notaP) &&
              notaP.IsValid() &&
              (notaP.evalCode == EVAL_EARNEDNOTARIZATION || notaP.evalCode == EVAL_ACCEPTEDNOTARIZATION) &&
              notaP.vData.size() &&
              (notarizationForEvidence = CPBaaSNotarization(notaP.vData[0])).IsValid()))
        {
            return state.Error("Invalid notarization for evidence");
        }

        CCurrencyDefinition notaryCurrency = ConnectedChains.GetCachedCurrency(notarizationForEvidence.currencyID);
        if (!notaryCurrency.IsValid())
        {
            return state.Error("Invalid notary currency");
        }
        std::vector<uint160> *pActiveNotaries;
        if ((notaryCurrency.IsGateway() && notaryCurrency.launchSystemID == ASSETCHAINS_CHAINID) ||
            IsVerusActive())
        {
            pActiveNotaries = &notaryCurrency.notaries;
        }
        else
        {
            pActiveNotaries = &(ConnectedChains.ThisChain().notaries);
        }
        std::vector<CNotarySignature> notarySignatures = currentEvidence.GetNotarySignatures();
        bool notarySigned = false;
        for (auto &oneNotaryID : *pActiveNotaries)
        {
            for (auto &oneSig : notarySignatures)
            {
                if (oneSig.signatures.count(oneNotaryID))
                {
                    notarySigned = true;
                    break;
                }
            }
        }
    }
    return true;
}

bool CObjectFinalization::GetPendingEvidence(const CCurrencyDefinition &externalSystem,
                                             const CUTXORef &notarizationRef,
                                             uint32_t endHeight,
                                             std::vector<std::pair<bool, CProofRoot>> &validCounterRoots,
                                             CProofRoot &challengeStartRoot,
                                             std::vector<CInputDescriptor> &outputs,
                                             bool &invalidates,
                                             CValidationState &state)
{
    invalidates = false;
    uint160 currencyID = externalSystem.GetID();

    CPBaaSNotarization normalizedNotarization;
    CTransaction notarizationTx;
    uint256 blockHash;
    CBlockIndex *pIndex = nullptr;
    if (!(notarizationRef.GetOutputTransaction(notarizationTx, blockHash) &&
          notarizationTx.vout.size() > notarizationRef.n &&
          !blockHash.IsNull() &&
          mapBlockIndex.count(blockHash) &&
          chainActive.Contains(pIndex = mapBlockIndex[blockHash]) &&
          (normalizedNotarization = CPBaaSNotarization(notarizationTx.vout[notarizationRef.n].scriptPubKey)).IsValid() &&
          normalizedNotarization.currencyID == currencyID))
    {
        return state.Error("Cannot get valid notarization from output reference");
    }

    uint32_t startHeight = pIndex->GetHeight();

    std::vector<std::tuple<uint32_t, COutPoint, CTransaction, CObjectFinalization>>
        ofsToCheck =
            CObjectFinalization::GetFinalizations(notarizationRef,
                                                  CObjectFinalization::ObjectFinalizationPendingKey(),
                                                  startHeight,
                                                  endHeight);

    bool primaryValidated = false;
    for (auto &oneFinalization : ofsToCheck)
    {
        outputs.push_back(CInputDescriptor(std::get<2>(oneFinalization).vout[std::get<1>(oneFinalization).n].scriptPubKey,
                                           std::get<2>(oneFinalization).vout[std::get<1>(oneFinalization).n].nValue,
                                           CTxIn(std::get<1>(oneFinalization))));
        std::vector<std::pair<CObjectFinalization, CNotaryEvidence>> ofEvidence =
            std::get<3>(oneFinalization).GetFinalizationEvidence(std::get<2>(oneFinalization), std::get<1>(oneFinalization).n, state);
        for (auto &oneEItem : ofEvidence)
        {
            // if we find a rejection, check it
            if (oneEItem.second.IsRejected())
            {
                CProofRoot challengeStart(CProofRoot::TYPE_PBAAS, CProofRoot::VERSION_INVALID);
                CProofRoot counterRoot = IsValidChallengeEvidence(externalSystem,
                                                                  normalizedNotarization.proofRoots[currencyID],
                                                                  oneEItem.second,
                                                                  blockHash,
                                                                  invalidates,
                                                                  challengeStart,
                                                                  endHeight);
                if (invalidates)
                {
                    break;
                }
                if (challengeStart.IsValid() && counterRoot.IsValid())
                {
                    bool isMorePowerful = CChainPower::ExpandCompactPower(counterRoot.compactPower) >
                                                CChainPower::ExpandCompactPower(normalizedNotarization.proofRoots[currencyID].compactPower);
                    validCounterRoots.push_back(std::make_pair(isMorePowerful, counterRoot));
                    if (!challengeStartRoot.IsValid() || challengeStart.rootHeight < challengeStartRoot.rootHeight)
                    {
                        challengeStartRoot = challengeStart;
                    }
                }
            }
        }
    }
    return true;
}

bool PreCheckFinalizeNotarization(const CTransaction &tx, int32_t outNum, CValidationState &state, uint32_t height)
{
    // ensure that if we are finalizing a notarization, we have followed all rules to do so
    // after we precheck and confirm a notarization finalization for a notary chain of this chain, we record it
    // as the last notarized checkpoint and prevent any unwind of the blockchain from that point

    // Finalizing a notarization does not generally happen without a chain of notarizations, in addition
    // to some set of evidence.
    //
    // Generally, the evidence will include one or more of the following:
    // 1) Minimum number (2 for signed, 4 for auto) of more recent notarization(s) that build on the notarization being finalized
    // 2) Answer to any challenge that has occurred since the notarization was posted
    // 2) A majority of witness signatures
    //

    uint32_t chainHeight = chainActive.Height();
    bool haveFullChain = height <= chainHeight + 1;

    auto upgradeVersion = CConstVerusSolutionVector::GetVersionByHeight(height);
    if (upgradeVersion < CActivationHeight::ACTIVATE_VERUSVAULT)
    {
        return true;
    }
    else if (upgradeVersion < CActivationHeight::ACTIVATE_PBAAS)
    {
        return false;
    }

    // ensure that we never accept an invalid proofroot for this chain in a notarization
    COptCCParams p;
    CObjectFinalization currentFinalization;
    if (!(tx.vout[outNum].scriptPubKey.IsPayToCryptoCondition(p) &&
          p.IsValid() &&
          p.evalCode == EVAL_FINALIZE_NOTARIZATION &&
          p.vData.size() &&
          (currentFinalization = CObjectFinalization(p.vData[0])).IsValid()) &&
          p.IsEvalPKOut())
    {
        return state.Error("Invalid finalization for notarization output");
    }

    CTransaction finalizedTx;
    uint256 txBlockHash;
    CPBaaSNotarization notarization;

    if (!(currentFinalization.GetOutputTransaction(tx, finalizedTx, txBlockHash) &&
          finalizedTx.vout[currentFinalization.output.n].scriptPubKey.IsPayToCryptoCondition(p) &&
          p.IsValid() &&
          (p.evalCode == EVAL_ACCEPTEDNOTARIZATION || p.evalCode == EVAL_EARNEDNOTARIZATION) &&
          p.vData.size() &&
          (notarization = CPBaaSNotarization(p.vData[0])).IsValid()))
    {
        if (!haveFullChain)
        {
            return true;
        }
        return state.Error("Invalid notarization output for finalization");
    }

    if (p.evalCode == EVAL_EARNEDNOTARIZATION &&
        !ConnectedChains.notarySystems.count(notarization.currencyID))
    {
        if (!haveFullChain)
        {
            return true;
        }
        return state.Error("Earned notarizations are only valid for notary systems");
    }

    CPBaaSNotarization normalizedNotarization = notarization;
    uint160 curID = normalizedNotarization.currencyID;

    CCurrencyDefinition notaryCurrencyDef = ConnectedChains.GetCachedCurrency(curID);
    if (!notaryCurrencyDef.IsValid())
    {
        return state.Error("Cannot get currency definition for notarization");
    }

    const CCurrencyDefinition *pNotaryCurrency;
    if (IsVerusActive() || !ConnectedChains.notarySystems.count(curID))
    {
        pNotaryCurrency = &notaryCurrencyDef;
    }
    else
    {
        pNotaryCurrency = &ConnectedChains.thisChain;
    }

    CNativeHashWriter hw(
        (pNotaryCurrency->IsGateway() &&
        pNotaryCurrency->launchSystemID == ASSETCHAINS_CHAINID &&
        pNotaryCurrency->proofProtocol == pNotaryCurrency->PROOF_ETHNOTARIZATION) ?
            notaryCurrencyDef.PROOF_ETHNOTARIZATION :
            pNotaryCurrency->PROOF_PBAASMMR);

    if (!notarization.SetMirror(false))
    {
        return state.Error("Cannot prepare notarization to validate finalization");
    }

    hw << notarization;
    uint256 objHash = hw.GetHash();

    CTransaction notarizationTx;

    // combine and verify all evidence, primarily to get all signatures in one
    // other evidence is evaluated in its originally submitted bundle
    std::vector<std::pair<CObjectFinalization, CNotaryEvidence>> evidenceVec =
        currentFinalization.GetFinalizationEvidence(tx, outNum, state, &notarizationTx);

    if (state.IsError())
    {
        return !haveFullChain;
    }

    bool confirmNeedsProof = !(notarization.IsPreLaunch() || notarization.IsBlockOneNotarization() || notarization.IsDefinitionNotarization());

    CNotaryEvidence signatureEvidence;
    std::vector<std::pair<CObjectFinalization, CNotaryEvidence>> counterEvidenceVec;

    // combine and verify signatures, segregating by evidence source system, not by finalization system
    std::map<uint160, std::vector<std::pair<CObjectFinalization, CNotaryEvidence>>> evidenceMap;

    for (auto &oneEvidenceChunk : evidenceVec)
    {
        if (oneEvidenceChunk.second.IsRejected())
        {
            counterEvidenceVec.push_back(oneEvidenceChunk);
        }
        if (evidenceMap.count(oneEvidenceChunk.second.systemID))
        {
            // merge signatures into 0th, and add additional evidence after 0th element
            evidenceMap[oneEvidenceChunk.second.systemID][0].second.MergeEvidence(oneEvidenceChunk.second, true, true);
            evidenceMap[oneEvidenceChunk.second.systemID].push_back(oneEvidenceChunk);
        }
        else
        {
            evidenceMap[oneEvidenceChunk.second.systemID].push_back(oneEvidenceChunk);
        }
    }

    auto notarySet = pNotaryCurrency->GetNotarySet();
    int numSignedNeeded = CPBaaSNotarization::MIN_EARNED_FOR_SIGNED;
    int numAutoNeeded = CPBaaSNotarization::MIN_EARNED_FOR_AUTO;
    int numBlocksAutoNeeded = CPBaaSNotarization::MinBlocksToAutoNotarization(ConnectedChains.ThisChain().blockNotarizationModulo);

    if (currentFinalization.IsConfirmed() || currentFinalization.IsRejected())
    {
        auto idxIt = mapBlockIndex.find(txBlockHash);
        if (idxIt == mapBlockIndex.end())
        {
            if (!haveFullChain)
            {
                return true;
            }
            else
            {
                return state.Error("Notarization without block cannot be finalized");
            }
        }
        if (!chainActive.Contains(idxIt->second))
        {
            return state.Error("Block notarization not in active chain cannot be finalized");
        }

        CUTXORef finalizationOutput = currentFinalization.output;
        finalizationOutput.hash = finalizedTx.GetHash();

        // if this is already finalized, we can't do it again
        auto priorFinalizations = CObjectFinalization::GetFinalizations(finalizationOutput,
                                                                        CObjectFinalization::ObjectFinalizationFinalizedKey(),
                                                                        idxIt->second->GetHeight(),
                                                                        height - 1);
        if (priorFinalizations.size())
        {
            // as long as we're spending it, it's ok
            std::map<CUTXORef, bool> outputSet;
            for (auto &oneFinalization : priorFinalizations)
            {
                if (std::get<3>(oneFinalization).output.hash.IsNull())
                {
                    std::get<3>(oneFinalization).output.hash = std::get<2>(oneFinalization).GetHash();
                }
                outputSet.insert(std::make_pair(std::get<3>(oneFinalization).output, std::get<3>(oneFinalization).IsConfirmed()));
            }
            std::vector<int> spendIdxs;
            for (int j = 0; j < tx.vin.size(); j++)
            {
                auto outputIt = outputSet.find(tx.vin[j].prevout);
                if (outputIt->second == currentFinalization.IsConfirmed())
                {
                    outputSet.erase(tx.vin[j].prevout);
                }
            }
            return outputSet.size() ?
                        state.Error("already " + std::get<3>(priorFinalizations[0]).IsConfirmed() ? "confirmed" : "rejected") :
                        true;
        }

        std::vector<std::tuple<uint32_t, COutPoint, CTransaction, CObjectFinalization>> pendingFinalizations;

        // get one pending key that may be on the same block as the notarization, then we can use a more targeted indexing
        // for the rest
        for (auto &oneFinalization : CObjectFinalization::GetFinalizations(curID,
                                                                           CObjectFinalization::ObjectFinalizationPendingKey(),
                                                                           idxIt->second->GetHeight(),
                                                                           idxIt->second->GetHeight()))
        {
            if (std::get<1>(oneFinalization) == finalizationOutput)
            {
                pendingFinalizations.push_back(oneFinalization);
            }
        }
        auto insertVec = CObjectFinalization::GetFinalizations(curID,
                                                               CObjectFinalization::ObjectFinalizationPendingKey(),
                                                               idxIt->second->GetHeight(),
                                                               height - 1);
        pendingFinalizations.insert(pendingFinalizations.end(), insertVec.begin(), insertVec.end());
        std::vector<std::tuple<uint32_t, COutPoint, CTransaction, CObjectFinalization>> &ofsToCheck = pendingFinalizations;

        if (currentFinalization.IsConfirmed())
        {
            // confirming requires not being rejected by evidence (see below), not having reasonable
            // doubt evidence that is unanswered / not proven false, and one or both of the following:
            //
            // 1) More than maturity has passed, and the finalization is accompanied with full
            //    validation proof to close any missing validation proof to cover the chain since
            //    last confirmed, resulting in a full proof for the entire range.
            //
            // 2) 2 or more subsequent notarizations agree with the notarization, and a quorum of
            //    notaries/witnesses sign to finalize.
            //
            // To answer a challenge depends on the challenge. The following challenges may be
            // proven false / overridden by the indicated evidence:
            //
            // 1) Notary signatures rejecting the notarization
            //    override by: this can only be overridden After 10 consecutive notarizations of
            //    earned notarizations, and 3 notarizations uncontested by miners or stakers.
            //    until that occurs, this rejection will prevent finalization as confirmed and
            //    cannot be overridden the notarization challenged may be referred to, but only
            //    a later notarization in the referral chain with no valid challenge may be
            //    finalized.
            //
            // 2) An earned notarization may be finalized as rejected if it fails to refer to
            //    one that it agrees with and adds a notarization when it should not. If one
            //    is added in spite of a prior, agreeing notarization in the modulo period,
            //    it can be rejected with a proof of one root by the other, including if
            //    they are the same. If the new root is earlier than the older root, the
            //    rejection proof must include valid proof of the gap.
            //
            // 3) Proof of an alternate chain that does NOT have more power -
            //    override by: Simply provide proof proof of consensus on the committed chain.
            //
            // 4) Proof of a chain with more power -
            //    override by: this cannot be overridden and prevents finalization as confirmed
            //    if it is not answered, the notarization challenged may remain pending and be
            //    referred to, but only a later notarization in the chain with no valid challenge
            //    may be finalized.
            //
            // for earned notarizations, auto-approved will need at least 10 consecutive agreements with no
            // unanswered challenges and random validation proof from the other chain. notary approved will
            // need 2 or more consecutive agreements and quorum of notary signatures.
            //

            auto lastConfirmedNotarizationInfo = GetLastConfirmedNotarization(normalizedNotarization.currencyID, height - 1);
            if (!std::get<0>(lastConfirmedNotarizationInfo))
            {
                return haveFullChain ? state.Error("Cannot locate prior confirmed notarization") : true;
            }

            // Check the evidence for the prior notarization that we indend to confirm,
            // if there is valid counter-evidence, do not confirm. Instead, reject and replace it with the new pending.
            std::vector<CProofRoot> validCounterRoots;
            CProofRoot mostPowerfulRoot(CProofRoot::TYPE_PBAAS, CProofRoot::VERSION_INVALID);
            CProofRoot challengeStartRoot(CProofRoot::TYPE_PBAAS, CProofRoot::VERSION_INVALID);

            bool primaryValidated = false;
            if (!confirmNeedsProof || pNotaryCurrency->notarizationProtocol == pNotaryCurrency->NOTARIZATION_NOTARY_CHAINID)
            {
                primaryValidated = true;
            }
            else
            {
                uint160 evidenceSystem = (p.evalCode == EVAL_EARNEDNOTARIZATION) ? ASSETCHAINS_CHAINID : notaryCurrencyDef.SystemOrGatewayID();
                for (auto &oneFinalization : ofsToCheck)
                {
                    std::vector<std::pair<CObjectFinalization, CNotaryEvidence>> ofEvidence = std::get<3>(oneFinalization).GetFinalizationEvidence(std::get<2>(oneFinalization), std::get<1>(oneFinalization).n, state);
                    for (auto &oneEItem : ofEvidence)
                    {
                        // if we find a rejection, check it
                        //
                        // if a rejection was either more powerful at the time it was made or it has evolved into a chain that
                        // became more powerful and continues now, we cannot finalize the notarization
                        //
                        //
                        //









                        if (oneEItem.second.IsRejected())
                        {
                            bool invalidates;
                            CProofRoot challengeStart(CProofRoot::TYPE_PBAAS, CProofRoot::VERSION_INVALID);
                            CProofRoot counterRoot = IsValidChallengeEvidence(notaryCurrencyDef, normalizedNotarization.proofRoots[notaryCurrencyDef.GetID()], oneEItem.second, txBlockHash, invalidates, challengeStart, height - 1);
                            if (invalidates ||
                                (challengeStart.IsValid() &&
                                 counterRoot.IsValid() &&
                                 pNotaryCurrency->proofProtocol == pNotaryCurrency->PROOF_PBAASMMR &&
                                 CChainPower::ExpandCompactPower(counterRoot.compactPower) > CChainPower::ExpandCompactPower(notarization.proofRoots[curID].compactPower)))
                            {
                                return state.Error("Cannot confirm notarization against valid counter-evidence of a more powerful chain");
                            }


                            if (challengeStart.IsValid() && counterRoot.IsValid())
                            {
                                validCounterRoots.push_back(counterRoot);
                                if (!challengeStartRoot.IsValid() || challengeStart.rootHeight < challengeStartRoot.rootHeight)
                                {
                                    challengeStartRoot = challengeStart;
                                }
                                if (!mostPowerfulRoot.IsValid() ||
                                    CChainPower::ExpandCompactPower(mostPowerfulRoot.compactPower) < CChainPower::ExpandCompactPower(counterRoot.compactPower))
                                {
                                    mostPowerfulRoot = counterRoot;
                                }
                            }




                        }
                    }
                }

                // right now, an unchallenged earned notarization does not have to prove itself for signature finalization,
                // it's efficient and still arguably fine, but we should at least consider that again
                uint256 entropyHash;
                if (LogAcceptCategory("notarization"))
                {
                    LogPrintf("checking evidence from system %s in map: {\n", EncodeDestination(CIdentityID(notaryCurrencyDef.GetID())).c_str());
                    if (evidenceMap[evidenceSystem].size() == 0)
                    {
                        LogPrintf("Evidence map is empty\n");
                    }
                    for (auto &oneSystem : evidenceMap)
                    {
                        LogPrintf("mapsize: %ld, evidencesystem: %s, ", evidenceMap[evidenceSystem].size(), EncodeDestination(CIdentityID(evidenceSystem)).c_str());
                        LogPrintf("system %s: {", EncodeDestination(CIdentityID(oneSystem.first)).c_str());
                        for (auto &oneEChunk : oneSystem.second)
                        {
                            LogPrintf("\"finalization\":%s,\n\"evidence\":%s", oneEChunk.first.ToUniValue().write(1,2).c_str(), oneEChunk.second.ToUniValue().write(1,2).c_str());
                        }
                    }
                    LogPrintf("}\n");
                }

                if ((evidenceMap[evidenceSystem].size() == 0) ||
                     (!(p.evalCode == EVAL_EARNEDNOTARIZATION && !mostPowerfulRoot.IsValid()) &&
                      (!IsValidPrimaryChainEvidence(notaryCurrencyDef,
                                                    evidenceMap[evidenceSystem][0].second,
                                                    normalizedNotarization,
                                                    std::get<0>(lastConfirmedNotarizationInfo),
                                                    height - 1,
                                                    &entropyHash,
                                                    mostPowerfulRoot,
                                                    challengeStartRoot).IsValid() ||
                       (entropyHash != txBlockHash && !IsPreTestnetFork(txBlockHash)) ||
                       CChainPower::ExpandCompactPower(mostPowerfulRoot.compactPower) >
                           CChainPower::ExpandCompactPower(normalizedNotarization.proofRoots[notaryCurrencyDef.GetID()].compactPower))))
                {
                    return state.Error(std::string("Cannot finalize notarization as confirmed") + (mostPowerfulRoot.IsValid() ? " relative to valid counter-evidence" : ""));
                }
            }

            if (confirmNeedsProof)
            {
                if (p.evalCode == EVAL_EARNEDNOTARIZATION)
                {
                    if (!ConnectedChains.notarySystems.count(notarization.currencyID))
                    {
                        return state.Error("insufficient foundation for proof of notary chain");
                    }

                    if (!evidenceMap[ASSETCHAINS_CHAINID].size())
                    {
                        return state.Error("insufficient local evidence for finalization");
                    }

                    CNotaryEvidence::EStates signatureState =
                                                evidenceMap[ASSETCHAINS_CHAINID][0].second.CheckSignatureConfirmation(objHash,
                                                                                                                    notarySet,
                                                                                                                    pNotaryCurrency->minNotariesConfirm,
                                                                                                                    height - 1);

                    if (signatureState == CNotaryEvidence::STATE_REJECTED &&
                        (pNotaryCurrency->notarizationProtocol == pNotaryCurrency->NOTARIZATION_NOTARY_CHAINID ||
                        pNotaryCurrency->notarizationProtocol == pNotaryCurrency->NOTARIZATION_NOTARY_CONFIRM))
                    {
                        return state.Error("notary witnesses reject notarization, so it cannot be confirmed");
                    }

                    // if we need further notarization proof check further
                    // if it is a centralized chain, trust the notary without further proof
                    if (signatureState != CNotaryEvidence::STATE_CONFIRMED &&
                        (pNotaryCurrency->notarizationProtocol == pNotaryCurrency->NOTARIZATION_NOTARY_CHAINID ||
                        pNotaryCurrency->notarizationProtocol == pNotaryCurrency->NOTARIZATION_NOTARY_CONFIRM))
                    {
                        // we are now going to see if we have enough evidence to confirm the notarization, either with
                        // witness signatures, or without. the only difference with or without will be the following numbers.
                        // defaults are with signatures
                        int needNotaryConfirmations = signatureState == CNotaryEvidence::STATE_CONFIRMED ? numSignedNeeded : numAutoNeeded;

                        std::vector<std::pair<uint32_t, CInputDescriptor>> unspentFinalizations;

                        // get all notarizations since this one that points to this one
                        int numConfirms = 0;



                        if (signatureState == CNotaryEvidence::STATE_CONFIRMED)
                        {
                            //  a) Notarization being confirmed must be agreed to by 2 subsequent consecutive notarizations for auto-notarization and
                            //     1 for centralized notarization and in all cases, have no interceding disagreements
                            //  b) Block height must be at least 1 after last required agreed notarization
                            //  c) evidence up to this point on chain must must contain necessary signatures from valid, unrevoked notaries
                            //     to confirm, as well as none of the following counter evidence:
                            //      i) proof that the notarization skipped a prior that it should have agreed with (invalidates)
                            //      ii) other forms of proof that render the to-be-confirmed proof root onconfirmable, such as a quorum of rejecting
                            //          signatures or proof of a more powerful, provable chain since the last notarization.
                        }
                        else
                        {
                            bool provenWithEvidence = false;

                            // if we are not confirmed by signature, we must be auto-confirmed by sufficient proof and
                            // answers to all challenges.
                            //
                            // we must be the most powerful pending notarization on-chain and have enough confirmations
                            //



                            // check to see that we have a notarization recent enough that also has
                            // valid confirmations behind it and has not been challenged by a more powerful fork
                            // for that long. if so, we can confirm it.

                            if (!primaryValidated &&
                                !IsValidPrimaryChainEvidence(notaryCurrencyDef,
                                                            evidenceMap[notaryCurrencyDef.SystemOrGatewayID()][0].second,
                                                            normalizedNotarization,
                                                            std::get<0>(lastConfirmedNotarizationInfo),
                                                            height - 1).IsValid())
                            {
                                return state.Error("Invalid notarization proof without counter-evidence");
                            }



                            // get simulated notarization data assuming the notarization confirmed, starting from that notarization
                            // to now and ensure that we have sufficient earned notarization confirmation

                            // ensure that we meet all counter-evidence requirements


                            if (!provenWithEvidence)
                            {
                                return state.Error("insufficient evidence for finalization 2");
                            }
                        }
                    }

                    // if we get here, store the verified proof root of this chain as notarized
                    ConnectedChains.notarySystems[notarization.currencyID].lastConfirmedNotarization = notarization;
                    // we should persist this notarization off-chain as a checkpoint
                }
                else
                {
                    // accepted notarization
                    // 2) for accepted notarizations:
                    //  a) Confirmed finalizations must accompany a valid, earned notarization with sufficient proof, which
                    //     may or may not include notary agreement from the other chain.
                    //  b) All on-chain evidence will be retrieved to ensure there is none of the following counter evidence:
                    //      i) on-chain rejection signatures in blocks prior to this one
                    //      ii) IDs revoked that result in less than majority for the confirmation
                    //      iii) proof of a more powerful, provably mined/staked chain since the last notarization that is different than
                    //           the accepted notarization.

                    if (!evidenceMap[notaryCurrencyDef.SystemOrGatewayID()].size())
                    {
                        return state.Error("insufficient local evidence for finalization");
                    }

                    if (!primaryValidated &&
                        !IsValidPrimaryChainEvidence(notaryCurrencyDef,
                                                    evidenceMap[notaryCurrencyDef.SystemOrGatewayID()][0].second,
                                                    normalizedNotarization,
                                                    std::get<0>(lastConfirmedNotarizationInfo),
                                                    height - 1).IsValid())
                    {
                        return state.Error("Invalid notarization proof without counter-evidence");
                    }

                    if (notaryCurrencyDef.SystemOrGatewayID() != ASSETCHAINS_CHAINID &&
                        (notaryCurrencyDef.IsPBaaSChain() || notaryCurrencyDef.IsGateway()) &&
                        !normalizedNotarization.IsPreLaunch() &&
                        (notaryCurrencyDef.launchSystemID != ASSETCHAINS_CHAINID ||
                        !evidenceMap.count(normalizedNotarization.currencyID) ||
                        evidenceMap[normalizedNotarization.currencyID][0].second.CheckSignatureConfirmation(objHash,
                                                                                                            notarySet,
                                                                                                            pNotaryCurrency->minNotariesConfirm,
                                                                                                            height - 1) !=
                                                                                                                CNotaryEvidence::STATE_CONFIRMED))
                    {
                        if (LogAcceptCategory("notarization"))
                        {
                            LogPrintf("insufficient witness signatures to finalize checking evidence: %s\n",
                                        evidenceMap[normalizedNotarization.currencyID][0].second.ToUniValue().write(1,2).c_str());
                        }

                        // check for sufficient autonotarization evidence
                        if (pNotaryCurrency->notarizationProtocol != pNotaryCurrency->NOTARIZATION_NOTARY_CHAINID &&
                            pNotaryCurrency->notarizationProtocol != pNotaryCurrency->NOTARIZATION_NOTARY_CONFIRM)
                        {

                        }
                        return state.Error("insufficient evidence from notary system to accept finalization");
                    }
                }
            }
        }
        else
        {
            // get evidence to see if we can verify a final rejection of this notarization
            //
            // rejecting requires one of the following types of evidence:
            // 1) proof that there is a notarization in conflict with this notarization that
            //    can prove more power since the last confirmed notarization for 10x the block
            //    modulo period on both chains.
            // 2) proof that there is a notarization in conflict with this notarization that
            //    is finalized on our notary chain.
            // 3) notaries signing rejected on a thread of notarizations that cannot respond
            //    to challenges for 3 modulo periods.
            //

            CProofRoot counterRoot(CProofRoot::TYPE_PBAAS, CProofRoot::VERSION_INVALID);
            CProofRoot challengeStartRoot;
            bool invalidates;
            for (auto &oneItem : evidenceVec)
            {
                CProofRoot tmpCounterRoot(CProofRoot::TYPE_PBAAS, CProofRoot::VERSION_INVALID);
                CProofRoot tmpChallengeStartRoot;
                // valid pending finalization evidence for notarizations include:
                // signatures for or against confirmation
                // counter-evidence of a valid, alternate chain
                if (notarization.proofRoots.count(notaryCurrencyDef.SystemOrGatewayID()) && notaryCurrencyDef.SystemOrGatewayID() != ASSETCHAINS_CHAINID)
                {
                    tmpCounterRoot = IsValidChallengeEvidence(notaryCurrencyDef,
                                                              notarization.proofRoots[notaryCurrencyDef.SystemOrGatewayID()],
                                                              oneItem.second,
                                                              txBlockHash,
                                                              invalidates,
                                                              tmpChallengeStartRoot,
                                                              height - 1);
                    if (invalidates)
                    {
                        if (tmpCounterRoot.IsValid())
                        {
                            counterRoot = tmpCounterRoot;
                        }
                        if (tmpChallengeStartRoot.IsValid() &&
                            (!challengeStartRoot.IsValid() || challengeStartRoot.rootHeight < tmpChallengeStartRoot.rootHeight))
                        {
                            challengeStartRoot = tmpChallengeStartRoot;
                        }
                        break;
                    }
                    if (tmpCounterRoot.IsValid() &&
                        tmpChallengeStartRoot.IsValid() &&
                        tmpCounterRoot != notarization.proofRoots[notaryCurrencyDef.SystemOrGatewayID()])
                    {
                        // we may need to keep a vector of these, but we don't have the variability to need that yet
                        if (!counterRoot.IsValid())
                        {
                            counterRoot = tmpCounterRoot;
                        }
                        if (!challengeStartRoot.IsValid() || challengeStartRoot.rootHeight < tmpChallengeStartRoot.rootHeight)
                        {
                            challengeStartRoot = tmpChallengeStartRoot;
                        }
                        continue;
                    }
                }
            }

            if (!invalidates)
            {
                // this is asserting rejection, so we must confirm that it can be rejected and that it only spends
                // inputs that are now invalidated due to the rejection
                if (p.evalCode == EVAL_EARNEDNOTARIZATION)
                {
                    if (!ConnectedChains.notarySystems.count(notarization.currencyID))
                    {
                        return state.Error("insufficient foundation for rejection proof of notarization");
                    }

                    // 1) for earned notarizations:
                    //  a) disagreeing earned notarization between the notarization and the second agreed notarization
                    //  b) other forms of proof that render the to-be-confirmed proof root invalid, such as rejecting signatures
                    //     by the confirming IDs that cancel enough confirming, or proof of a more powerful, provably mined and
                    //     staked chain since the last notarization
                    if (!evidenceMap[ASSETCHAINS_CHAINID].size() ||
                         evidenceMap[ASSETCHAINS_CHAINID][0].second.CheckSignatureConfirmation(objHash,
                                                                                               notarySet,
                                                                                               pNotaryCurrency->minNotariesConfirm,
                                                                                               height - 1) != CNotaryEvidence::STATE_REJECTED)
                    {
                        return state.Error("insufficient evidence to reject finalization");
                    }
                }
                else
                {
                    // 2) for accepted notarizations:
                    //  a) Majority on-chain rejection signatures in blocks prior to this one
                    //  b) IDs revoked that result in less than majority for the confirmation
                    //  c) proof of a more powerful, provably mined/staked chain since the last notarization that is different than
                    //     the accepted notarization
                    if (counterRoot.IsValid() ||
                        (notaryCurrencyDef.IsPBaaSChain() || notaryCurrencyDef.IsGateway()) &&
                        !notarization.IsPreLaunch() &&
                        (notaryCurrencyDef.launchSystemID != ASSETCHAINS_CHAINID ||
                        !evidenceMap[notaryCurrencyDef.SystemOrGatewayID()].size() ||
                        evidenceMap[notaryCurrencyDef.SystemOrGatewayID()][0].second.CheckSignatureConfirmation(objHash,
                                                                                                                notarySet,
                                                                                                                pNotaryCurrency->minNotariesConfirm,
                                                                                                                height - 1) !=
                                                                                                                     CNotaryEvidence::STATE_REJECTED))
                    {
                        return state.Error("insufficient evidence from notary system to reject finalization");
                    }
                }
            }






        }
    }
    else if (notarization.currencyID != ASSETCHAINS_CHAINID)
    {
        CProofRoot counterRoot(CProofRoot::TYPE_PBAAS, CProofRoot::VERSION_INVALID);
        CProofRoot challengeStartRoot;
        bool hasSignature = false;
        bool invalidates;

        for (auto &oneItem : evidenceVec)
        {
            // valid pending finalization evidence for notarizations include:
            // signatures for or against confirmation
            // counter-evidence of a valid, alternate chain
            if (notarization.proofRoots.count(notaryCurrencyDef.SystemOrGatewayID()) &&
                notaryCurrencyDef.SystemOrGatewayID() != ASSETCHAINS_CHAINID)
            {
                counterRoot = IsValidChallengeEvidence(notaryCurrencyDef, notarization.proofRoots[curID], oneItem.second, txBlockHash, invalidates, challengeStartRoot, height - 1);
                if (counterRoot.IsValid())
                {
                    if (!oneItem.first.IsChallenge())
                    {
                        return state.Error("challenge notary evidence may only be included with a finalization marked as a challenges");
                    }
                    // if this challenge invalidates, it needs to be part of a rejection
                    if (invalidates)
                    {
                        return state.Error("invalidating evidence needs to be part of a rejecting finalization transaction");
                    }
                    // if this is more powerful, it needs to be set as such, or it is invalid
                    if (CChainPower::ExpandCompactPower(counterRoot.compactPower) >
                            CChainPower::ExpandCompactPower(notarization.proofRoots[curID].compactPower) != oneItem.first.IsMorePowerful())
                    {
                        return state.Error("evidence proving a more powerful chain needs to be part of a more powerful finalization transaction");
                    }
                    break;
                }
            }
            if (oneItem.second.GetNotarySignatures().size() &&
                oneItem.second.CheckSignatureConfirmation(objHash, notarySet, pNotaryCurrency->minNotariesConfirm, height - 1) != CNotaryEvidence::EStates::STATE_INVALID)
            {
                hasSignature = true;
                break;
            }
        }
        if (evidenceVec.size() && !counterRoot.IsValid() && !hasSignature)
        {
            return state.Error("insufficient evidence to create new pending finalization for notarization");
        }
    }
    return true;
}

bool IsAcceptedNotarizationInput(const CScript &scriptSig)
{
    uint32_t ecode;
    return scriptSig.IsPayToCryptoCondition(&ecode) && ecode == EVAL_ACCEPTEDNOTARIZATION;
}

// earned notarizations can be spent when finalized as either confirmed or invalidated due to a disagreeing,
// confirmed notarization
bool ValidateEarnedNotarization(struct CCcontract_info *cp, Eval* eval, const CTransaction &tx, uint32_t nIn, bool fulfilled)
{
    // first, determine our notarization finalization protocol
    CUTXORef spendingOutput(tx.vin[nIn].prevout.hash, tx.vin[nIn].prevout.n);
    CTransaction sourceTx;
    uint256 blockHash;
    if (!spendingOutput.GetOutputTransaction(sourceTx, blockHash))
    {
        return eval->Error("Unable to retrieve spending transaction");
    }

    CPBaaSNotarization pbn;
    COptCCParams pN;
    if (!(sourceTx.vout[spendingOutput.n].scriptPubKey.IsPayToCryptoCondition(pN) &&
          pN.IsValid() &&
          pN.evalCode == EVAL_EARNEDNOTARIZATION &&
          pN.vData.size() &&
          (pbn = CPBaaSNotarization(pN.vData[0])).IsValid()))
    {
        return eval->Error("Invalid earned notarization, unspendable");
    }

    int i;
    for (i = 0; i < tx.vout.size(); i++)
    {
        COptCCParams p;
        CObjectFinalization of;
        CPBaaSNotarization nextPbn;
        if (tx.vout[i].scriptPubKey.IsPayToCryptoCondition(p) &&
            p.IsValid() &&
            (p.evalCode == EVAL_FINALIZE_NOTARIZATION &&
            p.vData.size() &&
            (of = CObjectFinalization(p.vData[0])).IsValid() &&
            of.currencyID == pbn.currencyID) ||
            (p.evalCode == EVAL_ACCEPTEDNOTARIZATION &&
             p.vData.size() &&
             (nextPbn = CPBaaSNotarization(p.vData[0])).IsValid() &&
             nextPbn.currencyID == pbn.currencyID))
        {
            break;
        }
    }

    if (i < tx.vout.size())
    {
        return true;
    }
    return eval->Error("Earned notarization can only be spent to a valid finalization");
}

bool IsEarnedNotarizationInput(const CScript &scriptSig)
{
    // this is an output check, and is incorrect. need to change to input
    uint32_t ecode;
    return scriptSig.IsPayToCryptoCondition(&ecode) && ecode == EVAL_EARNEDNOTARIZATION;
}

CObjectFinalization GetOldFinalization(const CTransaction &spendingTx, uint32_t nIn, CTransaction *pSourceTx=nullptr, uint32_t *pHeight=nullptr);
CObjectFinalization GetOldFinalization(const CTransaction &spendingTx, uint32_t nIn, CTransaction *pSourceTx, uint32_t *pHeight)
{
    CTransaction _sourceTx;
    CTransaction &sourceTx(pSourceTx ? *pSourceTx : _sourceTx);

    CObjectFinalization oldFinalization;
    uint256 blkHash;
    if (myGetTransaction(spendingTx.vin[nIn].prevout.hash, sourceTx, blkHash))
    {
        if (pHeight)
        {
            auto bIt = mapBlockIndex.find(blkHash);
            if (bIt == mapBlockIndex.end() || !bIt->second)
            {
                *pHeight = chainActive.Height();
            }
            else
            {
                *pHeight = bIt->second->GetHeight();
            }
        }
        COptCCParams p;
        if (sourceTx.vout[spendingTx.vin[nIn].prevout.n].scriptPubKey.IsPayToCryptoCondition(p) &&
            p.IsValid() &&
            (p.evalCode == EVAL_FINALIZE_NOTARIZATION || p.evalCode == EVAL_FINALIZE_EXPORT) &&
            p.version >= COptCCParams::VERSION_V3 &&
            p.vData.size() > 1)
        {
            oldFinalization = CObjectFinalization(p.vData[0]);
        }
    }
    return oldFinalization;
}


/*
 * Ensures that the finalization, either asserted to be confirmed or rejected, has all signatures and evidence
 * necessary for confirmation.
 */
bool ValidateFinalizeNotarization(struct CCcontract_info *cp, Eval* eval, const CTransaction &tx, uint32_t nIn, bool fulfilled)
{
    // to validate a finalization spend, we need to validate the spender's assertion of confirmation or rejection as proven

    // first, determine our notarization finalization protocol
    CTransaction sourceTx;
    uint32_t oldHeight;
    CObjectFinalization oldFinalization = GetOldFinalization(tx, nIn, &sourceTx, &oldHeight);
    if (!oldFinalization.IsValid())
    {
        return eval->Error("Invalid finalization output");
    }

    // if we are spending a confirmed notarization, then as long as we are being spent by
    // an accepted notarization and have the same confirmation on an output just after a pending output,
    // it is ok
    if (oldFinalization.IsConfirmed())
    {
        int priorPendingFinalization = -1, notarizationOut = -1;
        for (int i = 0; i < tx.vout.size(); i++)
        {
            auto &oneOut = tx.vout[i];

            COptCCParams p;
            CObjectFinalization oneOF;

            if (oneOut.scriptPubKey.IsPayToCryptoCondition(p) &&
                p.IsValid() &&
                p.evalCode == EVAL_FINALIZE_NOTARIZATION &&
                p.vData.size() &&
                (oneOF = CObjectFinalization(p.vData[0])).IsValid() &&
                oneOF.IsNotarizationFinalization() &&
                oneOF.currencyID == oldFinalization.currencyID &&
                oneOF.IsConfirmed())
            {
                return true;
            }
        }
        if (LogAcceptCategory("notarization") && LogAcceptCategory("verbose"))
        {
            UniValue jsonNTx(UniValue::VOBJ);
            TxToUniv(tx, uint256(), jsonNTx);
            LogPrintf("%s: Invalid spend of confirmed finalization on transaction:\n%s\n", __func__, jsonNTx.write(1,2).c_str());
        }
        return eval->Error("Invalid spend of confirmed finalization to transaction with no confirmed output");
    }

    // get currency to determine system and notarization method
    CCurrencyDefinition curDef = ConnectedChains.GetCachedCurrency(oldFinalization.currencyID);
    if (!curDef.IsValid())
    {
        return eval->Error("Invalid currency ID in finalization output");
    }
    uint160 SystemID = curDef.GetID();

    // for now, auto notarization uses defined notaries. it can be updated later, while leaving those that
    // select notary confirm explicitly to remain on that protocol
    if (curDef.notarizationProtocol == curDef.NOTARIZATION_NOTARY_CONFIRM || curDef.notarizationProtocol == curDef.NOTARIZATION_AUTO)
    {
        // get the notarization this finalizes and its index output
        int32_t notaryOutNum;
        CTransaction notarizationTx;

        if (oldFinalization.output.IsOnSameTransaction())
        {
            notarizationTx = sourceTx;
            // output needs non-null hash below
            oldFinalization.output.hash = notarizationTx.GetHash();
        }
        else
        {
            uint256 blkHash;
            if (!oldFinalization.GetOutputTransaction(sourceTx, notarizationTx, blkHash))
            {
                return eval->Error("notarization-transaction-not-found");
            }
        }
        if (notarizationTx.vout.size() <= oldFinalization.output.n)
        {
            return eval->Error("invalid-finalization");
        }

        CPBaaSNotarization pbn(notarizationTx.vout[oldFinalization.output.n].scriptPubKey);
        if (!pbn.IsValid())
        {
            return eval->Error("invalid-notarization");
        }

        // now, we have an unconfirmed, non-rejected finalization being spent by a transaction
        // confirm that the spender contains one finalization output either correctly confirming or invalidating
        // the finalization. rejection may be implicit by confirming another, later notarization.

        // First. make sure the oldFinalization is not referring to an earlier notarization than the
        // one most recently confirmed. If so. then it can be spent by anyone.
        CChainNotarizationData cnd;
        if (!GetNotarizationData(SystemID, cnd) || !cnd.IsConfirmed())
        {
            return eval->Error("invalid-notarization");
        }

        CObjectFinalization newFinalization;
        int finalizationOutNum = -1;
        bool foundFinalization = false;
        for (int i = 0; i < tx.vout.size(); i++)
        {
            auto &oneOut = tx.vout[i];
            COptCCParams p;
            // we can accept only one finalization of this notarization as an output, find it and reject more than one

            // TODO: HARDENING - ensure that the output of the finalization we are spending is either a confirmed, earlier
            // output or invalidated alternate to the one we are finalizing
            if (oneOut.scriptPubKey.IsPayToCryptoCondition(p) &&
                p.IsValid() &&
                p.evalCode == EVAL_FINALIZE_NOTARIZATION &&
                p.vData.size() &&
                (newFinalization = CObjectFinalization(p.vData[0])).IsValid() &&
                newFinalization.currencyID == oldFinalization.currencyID &&
                (newFinalization.IsConfirmed() || newFinalization.output == oldFinalization.output))
            {
                if (oldFinalization.IsConfirmed() && !newFinalization.IsConfirmed())
                {
                    LogPrint("notarization", "WARNING: Unconfirmed finalization attempting to spend confirmed finalization.\n old: %s\nnew: %s\n", oldFinalization.ToUniValue().write(1,2).c_str(), newFinalization.ToUniValue().write(1,2).c_str());
                    return eval->Error("unconfirmed-spending-confirmed");
                }
                if (foundFinalization)
                {
                    return eval->Error("duplicate-finalization");
                }
                foundFinalization = true;
                finalizationOutNum = i;
            }
        }

        if (!foundFinalization)
        {
            return eval->Error("invalid-finalization-spend");
        }
    }
    return true;
}

bool IsFinalizeNotarizationInput(const CScript &scriptSig)
{
    // this is an output check, and is incorrect. need to change to input
    uint32_t ecode;
    return scriptSig.IsPayToCryptoCondition(&ecode) && ecode == EVAL_FINALIZE_NOTARIZATION;
}

bool CObjectFinalization::GetOutputTransaction(const CTransaction &initialTx, CTransaction &tx, uint256 &blockHash) const
{
    if (output.hash.IsNull())
    {
        tx = initialTx;
        return true;
    }
    else if (myGetTransaction(output.hash, tx, blockHash) && tx.vout.size() > output.n)
    {
        return true;
    }
    return false;
}

bool CUTXORef::GetOutputTransaction(CTransaction &tx, uint256 &blockHash) const
{
    if (hash.IsNull())
    {
        return false;
    }
    else if (myGetTransaction(hash, tx, blockHash) && tx.vout.size() > n)
    {
        return true;
    }
    return false;
}

// Sign the output object with an ID or signing authority of the ID from the wallet.
CNotaryEvidence CObjectFinalization::SignConfirmed(const std::set<uint160> &notarySet, int minConfirming, const CWallet *pWallet, const CTransaction &initialTx, const CIdentityID &signatureID, uint32_t signingHeight, CCurrencyDefinition::EHashTypes hashType) const
{
    CNotaryEvidence retVal = CNotaryEvidence(ASSETCHAINS_CHAINID, output, CNotaryEvidence::STATE_CONFIRMING);

    AssertLockHeld(cs_main);

    CTransaction tx;
    uint256 blockHash;
    if (GetOutputTransaction(initialTx, tx, blockHash))
    {
        retVal.SignConfirmed(notarySet, minConfirming, *pWallet, tx, signatureID, signingHeight, hashType);
    }
    return retVal;
}

CNotaryEvidence CObjectFinalization::SignRejected(const std::set<uint160> &notarySet, int minConfirming, const CWallet *pWallet, const CTransaction &initialTx, const CIdentityID &signatureID, uint32_t signingHeight, CCurrencyDefinition::EHashTypes hashType) const
{
    CNotaryEvidence retVal = CNotaryEvidence(ASSETCHAINS_CHAINID, output, CNotaryEvidence::STATE_REJECTING);

    AssertLockHeld(cs_main);

    CTransaction tx;
    uint256 blockHash;
    if (GetOutputTransaction(initialTx, tx, blockHash))
    {
        retVal.SignRejected(notarySet, minConfirming, *pWallet, tx, signatureID, signingHeight, hashType);
    }
    return retVal;
}

// this ensures that the signature is, in fact, both authorized to sign, and also a
// valid signature of the specified output object. if so, this is accepted and
// results in a valid index entry as a confirmation of the notary signature
// all signatures must be from a valid notary, or this returns false and should be
// considered invalid.
// returns the number of valid, unique notary signatures, enabling a single output
// to be sufficient to authorize.
bool ValidateNotarizationEvidence(const CTransaction &tx, int32_t outNum, CValidationState &state, uint32_t height, int &confirmedCount, bool &provenFalse)
{
    // we MUST know that the cs_main lock is held. since it can be held on the validation thread while smart transactions
    // execute, we cannot take it or assert here
    return true;
    /*
    CNotaryEvidence notarySig;
    COptCCParams p;
    CCurrencyDefinition curDef;

    confirmedCount = 0;         // if a unit of evidence, whether signature or otherwise, is validated as confirming
    provenFalse = false;        // if the notarization is proven false

    if (tx.vout[outNum].scriptPubKey.IsPayToCryptoCondition(p) &&
        p.IsValid() &&
        p.version >= p.VERSION_V3 &&
        p.vData.size() &&
        (notarySig = CNotaryEvidence(p.vData[0])).IsValid() &&
        (curDef = ConnectedChains.GetCachedCurrency(notarySig.systemID)).IsValid())
    {
        // now, get the output to check and ensure the signature is good
        CObjectFinalization of;
        CPBaaSNotarization notarization;
        uint256 notarizationTxId;
        CTransaction nTx;
        uint256 blkHash;
        if (notarySig.output.hash.IsNull() ? (nTx = tx), true : myGetTransaction(notarySig.output.hash, nTx, blkHash) &&
            nTx.vout.size() > notarySig.output.n &&
            nTx.vout[notarySig.output.n].scriptPubKey.IsPayToCryptoCondition(p) &&
            p.IsValid() &&
            (p.evalCode == EVAL_FINALIZE_NOTARIZATION) &&
            p.vData.size() &&
            (of = CObjectFinalization(p.vData[0])).IsValid() &&
            of.IsNotarizationFinalization() &&
            of.output.hash.IsNull() ? (nTx = tx), true : myGetTransaction(of.output.hash, nTx, blkHash) &&
            !(notarizationTxId = nTx.GetHash()).IsNull() &&
            nTx.vout.size() > of.output.n &&
            nTx.vout[of.output.n].scriptPubKey.IsPayToCryptoCondition(p) &&
            p.IsValid() &&
            (p.evalCode == EVAL_EARNEDNOTARIZATION || p.evalCode == EVAL_ACCEPTEDNOTARIZATION) &&
            p.vData.size() &&
            (notarization = CPBaaSNotarization(p.vData[0])).IsValid() &&
            notarization.proofRoots.count(notarySig.systemID))
        {
            // signature is relative only to the notarization, not the finalization
            // that way, the information we put into the vdxfCodes have some meaning beyond
            // the blockchain on which it was signed, and we do not have to carry the
            // finalization mechanism cross-chain.
            std::vector<uint160> vdxfCodes = {CCrossChainRPCData::GetConditionID(notarySig.systemID,
                                                                                 CNotaryEvidence::NotarySignatureKey(),
                                                                                 notarizationTxId,
                                                                                 of.output.n)};
            std::vector<uint256> statements;

            // check that signature is of the hashed vData[0] data
            CNativeHashWriter hw;
            hw.write((const char *)&(p.vData[0][0]), p.vData[0].size());
            uint256 msgHash = hw.GetHash();

            for (auto &authorizedNotary : curDef.notaries)
            {
                std::map<CIdentityID, CIdentitySignature>::iterator sigIt = notarySig.signatures.find(authorizedNotary);
                if (sigIt != notarySig.signatures.end())
                {
                    // get identity used to sign
                    CIdentity signer = CIdentity::LookupIdentity(authorizedNotary, height);
                    uint256 sigHash = sigIt->second.IdentitySignatureHash(vdxfCodes, vdxfCodeNames, statements, of.currencyID, height, authorizedNotary, "", msgHash);

                    if (signer.IsValid())
                    {
                        std::set<uint160> idAddresses;
                        std::set<uint160> verifiedSignatures;

                        for (const CTxDestination &oneAddress : signer.primaryAddresses)
                        {
                            if (oneAddress.which() != COptCCParams::ADDRTYPE_PK || oneAddress.which() != COptCCParams::ADDRTYPE_PKH)
                            {
                                // currently, can only check secp256k1 signatures
                                return state.Error("Unsupported signature type");
                            }
                            idAddresses.insert(GetDestinationID(oneAddress));
                        }

                        for (auto &oneSig : notarySig.signatures[authorizedNotary].signatures)
                        {
                            CPubKey pubKey;
                            pubKey.RecoverCompact(sigHash, oneSig);
                            uint160 pkID = pubKey.GetID();
                            if (!idAddresses.count(pkID))
                            {
                                return state.Error("Mismatched pubkey and ID in signature");
                            }
                            if (verifiedSignatures.count(pkID))
                            {
                                return state.Error("Duplicate key use in ID signature");
                            }
                            verifiedSignatures.insert(pkID);
                        }
                        if (verifiedSignatures.size() >= signer.minSigs)
                        {
                            confirmedCount++;
                        }
                        else
                        {
                            return state.Error("Insufficient signatures on behalf of ID: " + signer.name);
                        }
                    }
                    else
                    {
                        return state.Error("Invalid notary identity or corrupt local state");
                    }
                }
                else
                {
                    return state.Error("Unauthorized notary");
                }
            }
        }
        else
        {
            return state.Error("Invalid notarization reference");
        }
    }
    else
    {
        return state.Error("Invalid or non-evidence output");
    }

    if (!confirmedCount)
    {
        return state.Error("No evidence present");
    }
    else
    {
        return true;
    }
    */
}

