// Copyright (c) 2019 yshurik
// Distributed under the MIT/X11 software license, see the accompanying
// file COPYING or http://www.opensource.org/licenses/mit-license.php.

#include "base58.h"
#include "rpcserver.h"
#include "txdb.h"
#include "init.h"
#include "main.h"
#include "net.h"
#include "keystore.h"
#include "wallet.h"

#include "pegops.h"
#include "pegdata.h"

#include <boost/algorithm/string.hpp>
#include <boost/algorithm/string/split.hpp>

using namespace std;
using namespace boost;
using namespace json_spirit;

void unpackpegdata(CFractions & fractions,
                   const string & pegdata64,
                   string tag);

void unpackbalance(CFractions & fractions,
                   CPegLevel & peglevel,
                   const string & pegdata64,
                   string tag);

void printpegshift(const CFractions & frPegShift,
                   const CPegLevel & peglevel,
                   Object & result,
                   bool print_pegdata);

void printpegbalance(const CFractions & frBalance,
                     const CPegLevel & peglevel,
                     Object & result,
                     string prefix,
                     bool print_pegdata);

void printpegbalance(const CFractions & frBalance,
                     int64_t nReserve,
                     int64_t nLiquid,
                     const CPegLevel & peglevel,
                     Object & result,
                     string prefix,
                     bool print_pegdata);

string scripttoaddress(const CScript& scriptPubKey,
                       bool* ptrIsNotary,
                       string* ptrNotary);

static void consumepegshift(CFractions & frBalance, 
                            CFractions & frExchange, 
                            CFractions & frPegShift,
                            const CFractions & frPegShiftInput) {
    int64_t nPegShiftPositive = 0;
    int64_t nPegShiftNegative = 0;
    CFractions frPegShiftPositive = frPegShiftInput.Positive(&nPegShiftPositive);
    CFractions frPegShiftNegative = frPegShiftInput.Negative(&nPegShiftNegative);
    CFractions frPegShiftNegativeConsume = frPegShiftNegative & (-frBalance);
    int64_t nPegShiftNegativeConsume = frPegShiftNegativeConsume.Total();
    int64_t nPegShiftPositiveConsume = frPegShiftPositive.Total();
    if ((-nPegShiftNegativeConsume) > nPegShiftPositiveConsume) {
        CFractions frToPositive = -frPegShiftNegativeConsume; 
        frToPositive = frToPositive.RatioPart(nPegShiftPositiveConsume);
        frPegShiftNegativeConsume = -frToPositive;
        nPegShiftNegativeConsume = frPegShiftNegativeConsume.Total();
    }
    nPegShiftPositiveConsume = -nPegShiftNegativeConsume;
    CFractions frPegShiftPositiveConsume = frPegShiftPositive.RatioPart(nPegShiftPositiveConsume);
    CFractions frPegShiftConsume = frPegShiftNegativeConsume + frPegShiftPositiveConsume;
    
    frBalance += frPegShiftConsume;
    frExchange += frPegShiftConsume;
    frPegShift -= frPegShiftConsume;
}

static void consumereservepegshift(CFractions & frBalance, 
                                   CFractions & frExchange, 
                                   CFractions & frPegShift,
                                   const CPegLevel & peglevel) 
{
    CFractions frPegShiftReserve = frPegShift.LowPart(peglevel, nullptr);
    consumepegshift(frBalance, frExchange, frPegShift, frPegShiftReserve);

    if (frPegShift.Positive(nullptr).Total() != -frPegShift.Negative(nullptr).Total()) {
        throw JSONRPCError(RPC_MISC_ERROR,
                           strprintf("Mismatch pegshift parts (%d - %d)",
                                     frPegShift.Positive(nullptr).Total(),
                                     frPegShift.Negative(nullptr).Total()));
    }
}

static void consumeliquidpegshift(CFractions & frBalance, 
                                  CFractions & frExchange, 
                                  CFractions & frPegShift,
                                  const CPegLevel & peglevel) 
{
    CFractions frPegShiftLiquid = frPegShift.HighPart(peglevel, nullptr);
    consumepegshift(frBalance, frExchange, frPegShift, frPegShiftLiquid);

    if (frPegShift.Positive(nullptr).Total() != -frPegShift.Negative(nullptr).Total()) {
        throw JSONRPCError(RPC_MISC_ERROR,
                           strprintf("Mismatch pegshift parts (%d - %d)",
                                     frPegShift.Positive(nullptr).Total(),
                                     frPegShift.Negative(nullptr).Total()));
    }
}

class CCoinToUse
{
public:
    uint256     txhash;
    uint64_t    i;
    int64_t     nValue;
    int64_t     nAvailableValue;
    CScript     scriptPubKey;
    int         nCycle;

    CCoinToUse() : i(0),nValue(0),nAvailableValue(0),nCycle(0) {}
    
    friend bool operator<(const CCoinToUse &a, const CCoinToUse &b) { 
        if (a.txhash < b.txhash) return true;
        if (a.txhash == b.txhash && a.i < b.i) return true;
        if (a.txhash == b.txhash && a.i == b.i && a.nValue < b.nValue) return true;
        if (a.txhash == b.txhash && a.i == b.i && a.nValue == b.nValue && a.nAvailableValue < b.nAvailableValue) return true;
        if (a.txhash == b.txhash && a.i == b.i && a.nValue == b.nValue && a.nAvailableValue == b.nAvailableValue && a.scriptPubKey < b.scriptPubKey) return true;
        if (a.txhash == b.txhash && a.i == b.i && a.nValue == b.nValue && a.nAvailableValue == b.nAvailableValue && a.scriptPubKey == b.scriptPubKey && a.nCycle < b.nCycle) return true;
        return false;
    }
    
    IMPLEMENT_SERIALIZE
    (
        READWRITE(txhash);
        READWRITE(i);
        READWRITE(nValue);
        READWRITE(scriptPubKey);
        READWRITE(nCycle);
    )
};

static bool sortByAddress(const CCoinToUse &lhs, const CCoinToUse &rhs) { 
    CScript lhs_script = lhs.scriptPubKey;
    CScript rhs_script = rhs.scriptPubKey;
    
    CTxDestination lhs_dst;
    CTxDestination rhs_dst;
    bool lhs_ok1 = ExtractDestination(lhs_script, lhs_dst);
    bool rhs_ok1 = ExtractDestination(rhs_script, rhs_dst);
    
    if (!lhs_ok1 || !rhs_ok1) {
        if (lhs_ok1 == rhs_ok1) 
            return lhs_script < rhs_script;
        return lhs_ok1 < rhs_ok1;
    }
    
    string lhs_addr = CBitcoinAddress(lhs_dst).ToString();
    string rhs_addr = CBitcoinAddress(rhs_dst).ToString();
    
    return lhs_addr < rhs_addr;
}

static bool sortByDestination(const CTxDestination &lhs, const CTxDestination &rhs) { 
    string lhs_addr = CBitcoinAddress(lhs).ToString();
    string rhs_addr = CBitcoinAddress(rhs).ToString();
    return lhs_addr < rhs_addr;
}

static void cleanupConsumed(const set<uint320> & setConsumedInputs,
                            const set<uint320> & setAllOutputs,
                            string & sConsumedInputs)
{
    set<uint320> setConsumedInputsNew;
    std::set_intersection(setConsumedInputs.begin(), setConsumedInputs.end(),
                          setAllOutputs.begin(), setAllOutputs.end(),
                          std::inserter(setConsumedInputsNew,setConsumedInputsNew.begin()));
    sConsumedInputs.clear();
    for(const uint320& fkey : setConsumedInputsNew) {
        if (!sConsumedInputs.empty()) sConsumedInputs += ",";
        sConsumedInputs += fkey.GetHex();
    }
}

static void cleanupProvided(const set<uint320> & setWalletOutputs,
                            map<uint320,CCoinToUse> & mapProvidedOutputs)
{
    map<uint320,CCoinToUse> mapProvidedOutputsNew;
    for(const pair<uint320,CCoinToUse> & item : mapProvidedOutputs) {
        if (setWalletOutputs.count(item.first)) continue;
        mapProvidedOutputsNew.insert(item);
    }
    mapProvidedOutputs = mapProvidedOutputsNew;
}

static void getAvailableCoins(const set<uint320> & setConsumedInputs,
                              int nCycleNow,
                              set<uint320> & setAllOutputs,
                              set<uint320> & setWalletOutputs,
                              map<uint320,CCoinToUse> & mapAllOutputs)
{
    vector<COutput> vecCoins;
    pwalletMain->AvailableCoins(vecCoins, false, true, NULL);
    for(const COutput& coin : vecCoins)
    {
        auto txhash = coin.tx->GetHash();
        auto fkey = uint320(txhash, coin.i);
        setAllOutputs.insert(fkey);
        setWalletOutputs.insert(fkey);
        if (setConsumedInputs.count(fkey)) continue; // already used
        CCoinToUse & out = mapAllOutputs[fkey];
        out.i = coin.i;
        out.txhash = txhash;
        out.nValue = coin.tx->vout[coin.i].nValue;
        out.scriptPubKey = coin.tx->vout[coin.i].scriptPubKey;
        out.nCycle = nCycleNow;
    }
}

static void parseConsumedAndProvided(const string & sConsumedInputs,
                                     const string & sProvidedOutputs,
                                     int nCycleNow,
                                     set<uint320> & setAllOutputs,
                                     set<uint320> & setConsumedInputs,
                                     map<uint320,CCoinToUse> & mapProvidedOutputs)
{
    vector<string> vConsumedInputsArgs;
    vector<string> vProvidedOutputsArgs;
    boost::split(vConsumedInputsArgs, sConsumedInputs, boost::is_any_of(","));
    boost::split(vProvidedOutputsArgs, sProvidedOutputs, boost::is_any_of(","));
    for(string sConsumedInput : vConsumedInputsArgs) {
        setConsumedInputs.insert(uint320(sConsumedInput));
    }
    for(string sProvidedOutput : vProvidedOutputsArgs) {
        vector<unsigned char> outData(ParseHex(sProvidedOutput));
        CDataStream ssData(outData, SER_NETWORK, PROTOCOL_VERSION);
        CCoinToUse out;
        try { ssData >> out; }
        catch (std::exception &) { continue; }
        if (out.nCycle != nCycleNow) { continue; }
        auto fkey = uint320(out.txhash, out.i);
        if (setConsumedInputs.count(fkey)) { continue; }
        mapProvidedOutputs[fkey] = out;
        setAllOutputs.insert(fkey);
    }
}

static void computeTxPegForNextCycle(const CTransaction & rawTx,
                                     const CPegLevel & peglevel_net,
                                     CTxDB & txdb,
                                     CPegDB & pegdb,
                                     map<uint320,CCoinToUse> & mapAllOutputs,
                                     map<int, CFractions> & mapTxOutputFractions,
                                     CFractions & feesFractions)
{
    MapPrevOut mapInputs;
    MapPrevTx mapTxInputs;
    MapFractions mapInputsFractions;
    MapFractions mapOutputFractions;
    string sPegFailCause;

    size_t n_vin = rawTx.vin.size();

    for (unsigned int i = 0; i < n_vin; i++)
    {
        const COutPoint & prevout = rawTx.vin[i].prevout;
        auto fkey = uint320(prevout.hash, prevout.n);

        if (mapAllOutputs.count(fkey)) {
            const CCoinToUse& coin = mapAllOutputs[fkey];
            CTxOut out(coin.nValue, coin.scriptPubKey);
            mapInputs[fkey] = out;
        }
        else {
            // Read txindex
            CTxIndex& txindex = mapTxInputs[prevout.hash].first;
            if (!txdb.ReadTxIndex(prevout.hash, txindex)) {
                continue;
            }
            // Read txPrev
            CTransaction& txPrev = mapTxInputs[prevout.hash].second;
            if (!txPrev.ReadFromDisk(txindex.pos)) {
                continue;
            }

            if (prevout.n >= txPrev.vout.size()) {
                continue;
            }

            mapInputs[fkey] = txPrev.vout[prevout.n];
        }

        CFractions& fractions = mapInputsFractions[fkey];
        fractions = CFractions(mapInputs[fkey].nValue, CFractions::VALUE);
        pegdb.ReadFractions(fkey, fractions);
    }

    bool peg_ok = CalculateStandardFractions(rawTx,
                                             peglevel_net.nSupplyNext,
                                             pindexBest->nTime,
                                             mapInputs,
                                             mapInputsFractions,
                                             mapOutputFractions,
                                             feesFractions,
                                             sPegFailCause);
    if (!peg_ok) {
        throw JSONRPCError(RPC_MISC_ERROR,
                           strprintf("Fail on calculations of tx fractions (cause=%s)",
                                     sPegFailCause.c_str()));
    }

    size_t n_out = rawTx.vout.size();
    for(size_t i=0; i< n_out; i++) {
        auto fkey = uint320(rawTx.GetHash(), i);
        mapTxOutputFractions[i] = mapOutputFractions[fkey];
    }
}

static void collectProvided(const CTransaction & rawTx,
                            string sAddress,
                            int nCycleNow,
                            string & sProvidedOutputs)
{
    size_t n_out = rawTx.vout.size();
    for (size_t i=0; i< n_out; i++) {
        string sNotary;
        bool fNotary = false;
        string sToAddress = scripttoaddress(rawTx.vout[i].scriptPubKey, &fNotary, &sNotary);
        if (fNotary) continue;
        if (sToAddress == sAddress) continue;

        CCoinToUse out;
        out.i = i;
        out.txhash = rawTx.GetHash();
        out.nValue = rawTx.vout[i].nValue;
        out.scriptPubKey = rawTx.vout[i].scriptPubKey;
        out.nCycle = nCycleNow;
        CDataStream ss(SER_NETWORK, PROTOCOL_VERSION);
        ss << out;
        if (!sProvidedOutputs.empty()) sProvidedOutputs += ",";
        sProvidedOutputs += HexStr(ss.begin(), ss.end());
    }
}

Value prepareliquidwithdraw(const Array& params, bool fHelp)
{
    if (fHelp || params.size() != 8)
        throw runtime_error(
            "prepareliquidwithdraw "
                "<balance_pegdata_base64> "
                "<exchange_pegdata_base64> "
                "<pegshift_pegdata_base64> "
                "<amount_with_fee> "
                "<address> "
                "<peglevel_hex> "
                "<consumed_inputs> "
                "<provided_outputs>\n"
            );
    
    string balance_pegdata64 = params[0].get_str();
    string exchange_pegdata64 = params[1].get_str();
    string pegshift_pegdata64 = params[2].get_str();
    int64_t nAmountWithFee = params[3].get_int64();
    string sAddress = params[4].get_str();

    CBitcoinAddress address(sAddress);
    if (!address.IsValid())
        throw JSONRPCError(RPC_INVALID_ADDRESS_OR_KEY, "Invalid BitBay address");
    
    string peglevel_hex = params[5].get_str();
    
    // exchange peglevel
    CPegLevel peglevel_exchange(peglevel_hex);
    if (!peglevel_exchange.IsValid()) {
        throw JSONRPCError(RPC_MISC_ERROR, "Can not unpack peglevel");
    }

    int nSupplyNow = pindexBest ? pindexBest->nPegSupplyIndex : 0;
    int nSupplyNext = pindexBest ? pindexBest->GetNextIntervalPegSupplyIndex() : 0;
    int nSupplyNextNext = pindexBest ? pindexBest->GetNextNextIntervalPegSupplyIndex() : 0;
    
    int nPegInterval = Params().PegInterval(nBestHeight);
    int nCycleNow = nBestHeight / nPegInterval;
    
    // network peglevel
    CPegLevel peglevel_net(nCycleNow,
                           nCycleNow-1,
                           nSupplyNow,
                           nSupplyNext,
                           nSupplyNextNext);
    
    CFractions frBalance(0, CFractions::VALUE);
    CFractions frExchange(0, CFractions::VALUE);
    CFractions frPegShift(0, CFractions::VALUE);
    
    CPegLevel peglevel_balance("");
    CPegLevel peglevel_exchange_skip("");

    unpackbalance(frBalance, peglevel_balance, balance_pegdata64, "balance");
    unpackbalance(frExchange, peglevel_exchange_skip, exchange_pegdata64, "exchange");
    unpackpegdata(frPegShift, pegshift_pegdata64, "pegshift");

    if (!balance_pegdata64.empty() && peglevel_balance.nCycle != peglevel_exchange.nCycle) {
        throw JSONRPCError(RPC_MISC_ERROR, "Balance has other cycle than peglevel");
    }

    frBalance = frBalance.Std();
    frExchange = frExchange.Std();
    frPegShift = frPegShift.Std();
    
    int64_t nBalanceLiquid = 0;
    CFractions frBalanceLiquid = frBalance.HighPart(peglevel_exchange, &nBalanceLiquid);
    if (nAmountWithFee > nBalanceLiquid) {
        throw JSONRPCError(RPC_MISC_ERROR, 
                           strprintf("Not enough liquid %d on 'balance' to withdraw %d",
                                     nBalanceLiquid,
                                     nAmountWithFee));
    }
    CFractions frAmount = frBalanceLiquid.RatioPart(nAmountWithFee);

    // inputs, outputs
    string sConsumedInputs = params[6].get_str();
    string sProvidedOutputs = params[7].get_str();

    set<uint320> setAllOutputs;
    set<uint320> setConsumedInputs;
    map<uint320,CCoinToUse> mapProvidedOutputs;
    parseConsumedAndProvided(sConsumedInputs, sProvidedOutputs, nCycleNow,
                             setAllOutputs, setConsumedInputs, mapProvidedOutputs);
    
    if (!pindexBest) {
        throw JSONRPCError(RPC_MISC_ERROR, "Blockchain is not in sync");
    }
    
    assert(pwalletMain != NULL);
   
    CTxDB txdb("r");
    CPegDB pegdb("r");
    
    // make list of 'rated' outputs, multimap with key 'distortion'
    // they are rated to be less distorted towards coins to withdraw
    
    map<uint320,CCoinToUse> mapAllOutputs = mapProvidedOutputs;
    set<uint320> setWalletOutputs;
    
    map<uint320,int64_t> mapAvailableLiquid;
    
    // get available coins
    getAvailableCoins(setConsumedInputs, nCycleNow,
                      setAllOutputs, setWalletOutputs, mapAllOutputs);
    // clean-up consumed, intersect with (wallet+provided)
    cleanupConsumed(setConsumedInputs, setAllOutputs, sConsumedInputs);
    // clean-up provided, remove what is already in wallet
    cleanupProvided(setWalletOutputs, mapProvidedOutputs);
    
    // read available coin fractions to rate
    // also consider only coins with are not less than 5% (20 inputs max)
    multimap<double,CCoinToUse> ratedOutputs;
    for(const pair<uint320,CCoinToUse>& item : mapAllOutputs) {
        uint320 fkey = item.first;
        CFractions frOut(0, CFractions::VALUE);
        if (!pegdb.ReadFractions(fkey, frOut, true)) {
            if (!mempool.lookup(fkey.b1(), fkey.b2(), frOut)) {
                continue;
            }
        }
        
        int64_t nAvailableLiquid = 0;
        frOut = frOut.HighPart(peglevel_exchange.nSupplyNext, &nAvailableLiquid);
        
        if (nAvailableLiquid < (nAmountWithFee / 20)) {
            continue;
        }
        
        double distortion = frOut.Distortion(frAmount);
        ratedOutputs.insert(pair<double,CCoinToUse>(distortion, item.second));
        mapAvailableLiquid[fkey] = nAvailableLiquid;
    }

    // get available value for selected coins
    set<CCoinToUse> setCoins;
    int64_t nLeftAmount = nAmountWithFee;
    auto it = ratedOutputs.begin();
    for (; it != ratedOutputs.end(); ++it) {
        CCoinToUse out = (*it).second;
        auto txhash = out.txhash;
        auto fkey = uint320(txhash, out.i);
        
        nLeftAmount -= mapAvailableLiquid[fkey];
        out.nAvailableValue = mapAvailableLiquid[fkey];
        setCoins.insert(out);
        
        if (nLeftAmount <= 0) {
            break;
        }
    }
    
    if (nLeftAmount > 0) {
        throw JSONRPCError(RPC_MISC_ERROR, 
                           strprintf("Not enough liquid or coins are too fragmented  on 'exchange' to withdraw %d",
                                     nAmountWithFee));
    }
    
    int64_t nFeeRet = 1000000 /*temp fee*/;
    int64_t nAmount = nAmountWithFee - nFeeRet;
    
    vector<pair<CScript, int64_t> > vecSend;
    CScript scriptPubKey;
    scriptPubKey.SetDestination(address.Get());
    vecSend.push_back(make_pair(scriptPubKey, nAmount));
    
    int64_t nValue = 0;
    for(const pair<CScript, int64_t>& s : vecSend) {
        if (nValue < 0)
            return false;
        nValue += s.second;
    }
    
    size_t nNumInputs = 1;

    CTransaction rawTx;
    
    nNumInputs = setCoins.size();
    if (!nNumInputs) return false;
    
    // Inputs to be sorted by address
    vector<CCoinToUse> vCoins;
    for(const CCoinToUse& coin : setCoins) {
        vCoins.push_back(coin);
    }
    sort(vCoins.begin(), vCoins.end(), sortByAddress);
    
    // Collect input addresses
    // Prepare maps for input,available,take
    set<CTxDestination> setInputAddresses;
    vector<CTxDestination> vInputAddresses;
    map<CTxDestination, int64_t> mapAvailableValuesAt;
    map<CTxDestination, int64_t> mapInputValuesAt;
    map<CTxDestination, int64_t> mapTakeValuesAt;
    int64_t nValueToTakeFromChange = 0;
    for(const CCoinToUse& coin : vCoins) {
        CTxDestination address;
        if(!ExtractDestination(coin.scriptPubKey, address))
            continue;
        setInputAddresses.insert(address); // sorted due to vCoins
        mapAvailableValuesAt[address] = 0;
        mapInputValuesAt[address] = 0;
        mapTakeValuesAt[address] = 0;
    }
    // Get sorted list of input addresses
    for(const CTxDestination& address : setInputAddresses) {
        vInputAddresses.push_back(address);
    }
    sort(vInputAddresses.begin(), vInputAddresses.end(), sortByDestination);
    // Input and available values can be filled in
    for(const CCoinToUse& coin : vCoins) {
        CTxDestination address;
        if(!ExtractDestination(coin.scriptPubKey, address))
            continue;
        int64_t& nValueAvailableAt = mapAvailableValuesAt[address];
        nValueAvailableAt += coin.nAvailableValue;
        int64_t& nValueInputAt = mapInputValuesAt[address];
        nValueInputAt += coin.nValue;
    }
            
    // vouts to the payees
    for(const pair<CScript, int64_t>& s : vecSend) {
        rawTx.vout.push_back(CTxOut(s.second, s.first));
    }
    
    CReserveKey reservekey(pwalletMain);
    reservekey.ReturnKey();

    // Available values - liquidity
    // Compute values to take from each address (liquidity is common)
    int64_t nValueLeft = nValue;
    for(const CCoinToUse& coin : vCoins) {
        CTxDestination address;
        if(!ExtractDestination(coin.scriptPubKey, address))
            continue;
        int64_t nValueAvailable = coin.nAvailableValue;
        int64_t nValueTake = nValueAvailable;
        if (nValueTake > nValueLeft) {
            nValueTake = nValueLeft;
        }
        int64_t& nValueTakeAt = mapTakeValuesAt[address];
        nValueTakeAt += nValueTake;
        nValueLeft -= nValueTake;
    }
    
    // Calculate change (minus fee and part taken from change)
    int64_t nTakeFromChangeLeft = nValueToTakeFromChange + nFeeRet;
    for (const CTxDestination& address : vInputAddresses) {
        CScript scriptPubKey;
        scriptPubKey.SetDestination(address);
        int64_t nValueTake = mapTakeValuesAt[address];
        int64_t nValueInput = mapInputValuesAt[address];
        int64_t nValueChange = nValueInput - nValueTake;
        if (nValueChange > nTakeFromChangeLeft) {
            nValueChange -= nTakeFromChangeLeft;
            nTakeFromChangeLeft = 0;
        }
        if (nValueChange < nTakeFromChangeLeft) {
            nTakeFromChangeLeft -= nValueChange;
            nValueChange = 0;
        }
        if (nValueChange == 0) continue;
        rawTx.vout.push_back(CTxOut(nValueChange, scriptPubKey));
    }
    
    // Fill vin
    for(const CCoinToUse& coin : vCoins) {
        rawTx.vin.push_back(CTxIn(coin.txhash,coin.i));
    }
    
    // Calculate peg
    CFractions feesFractions(0, CFractions::STD);
    map<int, CFractions> mapTxOutputFractions;
    computeTxPegForNextCycle(rawTx, peglevel_net, txdb, pegdb, mapAllOutputs,
                             mapTxOutputFractions, feesFractions);

    // for liquid just first output
    if (!mapTxOutputFractions.count(0)) {
        throw JSONRPCError(RPC_MISC_ERROR, "No withdraw fractions");
    }

    // signing the transaction to get it ready for broadcast
    int nIn = 0;
    for(const CCoinToUse& coin : vCoins) {
        if (!SignSignature(*pwalletMain, coin.scriptPubKey, rawTx, nIn++)) {
            throw JSONRPCError(RPC_MISC_ERROR, 
                               strprintf("Fail on signing input (%d)", nIn-1));
        }
    }
    // for liquid just first output
    CFractions frProcessed = mapTxOutputFractions[0] + feesFractions;
    CFractions frRequested = frAmount;

    if (frRequested.Total() != nAmountWithFee) {
        throw JSONRPCError(RPC_MISC_ERROR, 
                           strprintf("Mismatch requested and amount_with_fee (%d - %d)",
                                     frRequested.Total(), nAmountWithFee));
    }
    if (frProcessed.Total() != nAmountWithFee) {
        throw JSONRPCError(RPC_MISC_ERROR, 
                           strprintf("Mismatch processed and amount_with_fee (%d - %d)",
                                     frProcessed.Total(), nAmountWithFee));
    }
    
    // get list of consumed inputs
    for (size_t i=0; i< rawTx.vin.size(); i++) {
        const COutPoint & prevout = rawTx.vin[i].prevout;
        auto fkey = uint320(prevout.hash, prevout.n);
        if (mapProvidedOutputs.count(fkey)) mapProvidedOutputs.erase(fkey);
        if (!sConsumedInputs.empty()) sConsumedInputs += ",";
        sConsumedInputs += fkey.GetHex();
    }
    
    // get list of provided outputs and save fractions
    sProvidedOutputs.clear();
    for(const pair<uint320,CCoinToUse> & item : mapProvidedOutputs) {
        CDataStream ss(SER_NETWORK, PROTOCOL_VERSION);
        ss << item.second;
        if (!sProvidedOutputs.empty()) sProvidedOutputs += ",";
        sProvidedOutputs += HexStr(ss.begin(), ss.end());
    }

    string sTxhash = rawTx.GetHash().GetHex();

    // get list of changes and add to current provided outputs
    map<string, int64_t> mapTxChanges;
    {
        CPegDB pegdbrw;
        for (size_t i=1; i< rawTx.vout.size(); i++) { // skip 0 (withdraw)
            // make map of change outputs
            string txout = sTxhash+":"+itostr(i);
            mapTxChanges[txout] = rawTx.vout[i].nValue;
            // save these outputs in pegdb, so they can be used in next withdraws
            auto fkey = uint320(rawTx.GetHash(), i);
            pegdbrw.WriteFractions(fkey, mapTxOutputFractions[i]);
        }
    }
    collectProvided(rawTx, sAddress, nCycleNow, sProvidedOutputs);
    
    frBalance -= frRequested;
    frExchange -= frRequested;
    frPegShift += (frRequested - frProcessed);
    
    // consume liquid part of pegshift by balance
    // as computation were completed by pegnext it may use fractions
    // of current reserves - at current supply not to consume these fractions
    consumeliquidpegshift(frBalance, frExchange, frPegShift, peglevel_exchange);

    CDataStream ss(SER_NETWORK, PROTOCOL_VERSION);
    ss << rawTx;
    
    string txstr = HexStr(ss.begin(), ss.end());
    
    Object result;
    result.push_back(Pair("completed", true));
    result.push_back(Pair("txhash", sTxhash));
    result.push_back(Pair("rawtx", txstr));

    result.push_back(Pair("consumed_inputs", sConsumedInputs));
    result.push_back(Pair("provided_outputs", sProvidedOutputs));

    result.push_back(Pair("created_on_peg", peglevel_net.nSupply));
    result.push_back(Pair("broadcast_on_peg", peglevel_net.nSupplyNext));
    
    printpegbalance(frBalance, peglevel_exchange, result, "balance_", true);
    printpegbalance(frProcessed, peglevel_exchange, result, "processed_", true);
    printpegbalance(frExchange, peglevel_exchange, result, "exchange_", true);
    
    printpegshift(frPegShift, peglevel_net, result, true);
    
    Array changes;
    for(const pair<string, int64_t> & item : mapTxChanges) {
        Object obj;
        obj.push_back(Pair("txout", item.first));
        obj.push_back(Pair("amount", item.second));
        changes.push_back(obj);
    }
    result.push_back(Pair("changes", changes));
    
    return result;
}

Value preparereservewithdraw(const Array& params, bool fHelp)
{
    if (fHelp || params.size() != 8)
        throw runtime_error(
            "preparereservewithdraw "
                "<balance_pegdata_base64> "
                "<exchange_pegdata_base64> "
                "<pegshift_pegdata_base64> "
                "<amount_with_fee> "
                "<address> "
                "<peglevel_hex> "
                "<consumed_inputs> "
                "<provided_outputs>\n"
            );
    
    string balance_pegdata64 = params[0].get_str();
    string exchange_pegdata64 = params[1].get_str();
    string pegshift_pegdata64 = params[2].get_str();
    int64_t nAmountWithFee = params[3].get_int64();
    string sAddress = params[4].get_str();

    CBitcoinAddress address(sAddress);
    if (!address.IsValid())
        throw JSONRPCError(RPC_INVALID_ADDRESS_OR_KEY, "Invalid BitBay address");
    
    string peglevel_hex = params[5].get_str();

    // exchange peglevel
    CPegLevel peglevel_exchange(peglevel_hex);
    if (!peglevel_exchange.IsValid()) {
        throw JSONRPCError(RPC_MISC_ERROR, "Can not unpack peglevel");
    }

    int nSupplyNow = pindexBest ? pindexBest->nPegSupplyIndex : 0;
    int nSupplyNext = pindexBest ? pindexBest->GetNextIntervalPegSupplyIndex() : 0;
    int nSupplyNextNext = pindexBest ? pindexBest->GetNextNextIntervalPegSupplyIndex() : 0;
    
    int nPegInterval = Params().PegInterval(nBestHeight);
    int nCycleNow = nBestHeight / nPegInterval;
    
    // network peglevel
    CPegLevel peglevel_net(nCycleNow,
                           nCycleNow-1,
                           nSupplyNow,
                           nSupplyNext,
                           nSupplyNextNext);
    
    CFractions frBalance(0, CFractions::VALUE);
    CFractions frExchange(0, CFractions::VALUE);
    CFractions frPegShift(0, CFractions::VALUE);
    
    CPegLevel peglevel_balance("");
    CPegLevel peglevel_exchange_skip("");

    unpackbalance(frBalance, peglevel_balance, balance_pegdata64, "balance");
    unpackbalance(frExchange, peglevel_exchange_skip, exchange_pegdata64, "exchange");
    unpackpegdata(frPegShift, pegshift_pegdata64, "pegshift");

    if (!balance_pegdata64.empty() && peglevel_balance.nCycle != peglevel_exchange.nCycle) {
        throw JSONRPCError(RPC_MISC_ERROR, "Balance has other cycle than peglevel");
    }

    frBalance = frBalance.Std();
    frExchange = frExchange.Std();
    frPegShift = frPegShift.Std();
    
    int64_t nBalanceReserve = 0;
    CFractions frBalanceReserve = frBalance.LowPart(peglevel_exchange.nSupplyNext, &nBalanceReserve);
    if (nAmountWithFee > nBalanceReserve) {
        throw JSONRPCError(RPC_MISC_ERROR, 
                           strprintf("Not enough reserve %d on 'balance' to withdraw %d",
                                     nBalanceReserve,
                                     nAmountWithFee));
    }
    CFractions frAmount = frBalanceReserve.RatioPart(nAmountWithFee);

    // inputs, outputs
    string sConsumedInputs = params[6].get_str();
    string sProvidedOutputs = params[7].get_str();

    set<uint320> setAllOutputs;
    set<uint320> setConsumedInputs;
    map<uint320,CCoinToUse> mapProvidedOutputs;
    parseConsumedAndProvided(sConsumedInputs, sProvidedOutputs, nCycleNow,
                             setAllOutputs, setConsumedInputs, mapProvidedOutputs);
    
    if (!pindexBest) {
        throw JSONRPCError(RPC_MISC_ERROR, "Blockchain is not in sync");
    }
    
    assert(pwalletMain != NULL);
   
    CTxDB txdb("r");
    CPegDB pegdb("r");
    
    // make list of 'rated' outputs, multimap with key 'distortion'
    // they are rated to be less distorted towards coins to withdraw
    
    map<uint320,CCoinToUse> mapAllOutputs = mapProvidedOutputs;
    set<uint320> setWalletOutputs;
        
    // get available coins
    getAvailableCoins(setConsumedInputs, nCycleNow,
                      setAllOutputs, setWalletOutputs, mapAllOutputs);
    // clean-up consumed, intersect with (wallet+provided)
    cleanupConsumed(setConsumedInputs, setAllOutputs, sConsumedInputs);
    // clean-up provided, remove what is already in wallet
    cleanupProvided(setWalletOutputs, mapProvidedOutputs);
    
    // read avaialable coin fractions to rate
    // also consider only coins with are not less than 10% (10 inputs max)
    map<uint320,int64_t> mapAvailableReserve;
    multimap<double,CCoinToUse> ratedOutputs;
    for(const pair<uint320,CCoinToUse>& item : mapAllOutputs) {
        uint320 fkey = item.first;
        CFractions frOut(0, CFractions::VALUE);
        if (!pegdb.ReadFractions(fkey, frOut, true)) {
            if (!mempool.lookup(fkey.b1(), fkey.b2(), frOut)) {
                continue;
            }
        }
        
        int64_t nAvailableReserve = 0;
        frOut = frOut.LowPart(peglevel_exchange.nSupplyNext, &nAvailableReserve);
        
        if (nAvailableReserve < (nAmountWithFee / 20)) {
            continue;
        }
        
        double distortion = frOut.Distortion(frAmount);
        ratedOutputs.insert(pair<double,CCoinToUse>(distortion, item.second));
        mapAvailableReserve[fkey] = nAvailableReserve;
    }

    // get available value for selected coins
    set<CCoinToUse> setCoins;
    int64_t nLeftAmount = nAmountWithFee;
    auto it = ratedOutputs.begin();
    for (; it != ratedOutputs.end(); ++it) {
        CCoinToUse out = (*it).second;
        auto txhash = out.txhash;
        auto fkey = uint320(txhash, out.i);
        
        nLeftAmount -= mapAvailableReserve[fkey];
        out.nAvailableValue = mapAvailableReserve[fkey];
        setCoins.insert(out);
        
        if (nLeftAmount <= 0) {
            break;
        }
    }
    
    if (nLeftAmount > 0) {
        throw JSONRPCError(RPC_MISC_ERROR, 
                           strprintf("Not enough reserve or coins are too fragmented on 'exchange' to withdraw %d",
                                     nAmountWithFee));
    }
    
    int64_t nFeeRet = 1000000 /*temp fee*/;
    int64_t nAmount = nAmountWithFee - nFeeRet;
    
    vector<pair<CScript, int64_t> > vecSend;
    CScript scriptPubKey;
    scriptPubKey.SetDestination(address.Get());
    vecSend.push_back(make_pair(scriptPubKey, nAmount));
    
    int64_t nValue = 0;
    for(const pair<CScript, int64_t>& s : vecSend) {
        if (nValue < 0)
            return false;
        nValue += s.second;
    }
    
    size_t nNumInputs = 1;

    CTransaction rawTx;
    
    nNumInputs = setCoins.size();
    if (!nNumInputs) return false;
    
    // Inputs to be sorted by address
    vector<CCoinToUse> vCoins;
    for(const CCoinToUse& coin : setCoins) {
        vCoins.push_back(coin);
    }
    sort(vCoins.begin(), vCoins.end(), sortByAddress);
    
    // Collect input addresses
    // Prepare maps for input,available,take
    set<CTxDestination> setInputAddresses;
    vector<CTxDestination> vInputAddresses;
    map<CTxDestination, int64_t> mapAvailableValuesAt;
    map<CTxDestination, int64_t> mapInputValuesAt;
    map<CTxDestination, int64_t> mapTakeValuesAt;
    int64_t nValueToTakeFromChange = 0;
    for(const CCoinToUse& coin : vCoins) {
        CTxDestination address;
        if(!ExtractDestination(coin.scriptPubKey, address))
            continue;
        setInputAddresses.insert(address); // sorted due to vCoins
        mapAvailableValuesAt[address] = 0;
        mapInputValuesAt[address] = 0;
        mapTakeValuesAt[address] = 0;
    }
    // Get sorted list of input addresses
    for(const CTxDestination& address : setInputAddresses) {
        vInputAddresses.push_back(address);
    }
    sort(vInputAddresses.begin(), vInputAddresses.end(), sortByDestination);
    // Input and available values can be filled in
    for(const CCoinToUse& coin : vCoins) {
        CTxDestination address;
        if(!ExtractDestination(coin.scriptPubKey, address))
            continue;
        int64_t& nValueAvailableAt = mapAvailableValuesAt[address];
        nValueAvailableAt += coin.nAvailableValue;
        int64_t& nValueInputAt = mapInputValuesAt[address];
        nValueInputAt += coin.nValue;
    }
    
    // Notations for frozen **F**
    {
        // prepare indexes to freeze
        size_t nCoins = vCoins.size();
        size_t nPayees = vecSend.size();
        string out_indexes;
        if (nPayees == 1) { // trick to have triple to use sort
            auto out_index = std::to_string(0+nCoins);
            out_indexes = out_index+":"+out_index+":"+out_index;
        }
        else if (nPayees == 2) { // trick to have triple to use sort
            auto out_index1 = std::to_string(0+nCoins);
            auto out_index2 = std::to_string(1+nCoins);
            out_indexes = out_index1+":"+out_index1+":"+out_index2+":"+out_index2;
        }
        else {
            for(size_t i=0; i<nPayees; i++) {
                if (!out_indexes.empty())
                    out_indexes += ":";
                out_indexes += std::to_string(i+nCoins);
            }
        }
        // Fill vout with freezing instructions
        for(size_t i=0; i<nCoins; i++) {
            CScript scriptPubKey;
            scriptPubKey.push_back(OP_RETURN);
            unsigned char len_bytes = out_indexes.size();
            scriptPubKey.push_back(len_bytes+5);
            scriptPubKey.push_back('*');
            scriptPubKey.push_back('*');
            scriptPubKey.push_back('F');
            scriptPubKey.push_back('*');
            scriptPubKey.push_back('*');
            for (size_t j=0; j< out_indexes.size(); j++) {
                scriptPubKey.push_back(out_indexes[j]);
            }
            rawTx.vout.push_back(CTxOut(PEG_MAKETX_FREEZE_VALUE, scriptPubKey));
        }
        // Value for notary is first taken from reserves sorted by address
        int64_t nValueLeft = nCoins*PEG_MAKETX_FREEZE_VALUE;
        // take reserves in defined order
        for(const CTxDestination& address : vInputAddresses) {
            int64_t nValueAvailableAt = mapAvailableValuesAt[address];
            int64_t& nValueTakeAt = mapTakeValuesAt[address];
            int64_t nValueLeftAt = nValueAvailableAt-nValueTakeAt;
            if (nValueAvailableAt ==0) continue;
            int64_t nValueToTake = nValueLeft;
            if (nValueToTake > nValueLeftAt)
                nValueToTake = nValueLeftAt;
            
            nValueTakeAt += nValueToTake;
            nValueLeft -= nValueToTake;
            
            if (nValueLeft == 0) break;
        }
        // if nValueLeft is left - need to be taken from change (liquidity)
        nValueToTakeFromChange += nValueLeft;
    }
    
    // vouts to the payees
    for(const pair<CScript, int64_t>& s : vecSend) {
        rawTx.vout.push_back(CTxOut(s.second, s.first));
    }
    
    CReserveKey reservekey(pwalletMain);
    reservekey.ReturnKey();
    
    // Available values - reserves per address
    // vecSend - outputs to be frozen reserve parts
    
    // Prepare order of inputs
    // For **F** the first is referenced (last input) then others are sorted
    vector<CTxDestination> vAddressesForFrozen;
    CTxDestination addressFrozenRef = vInputAddresses.back();
    vAddressesForFrozen.push_back(addressFrozenRef);
    for(const CTxDestination & address : vInputAddresses) {
        if (address == addressFrozenRef) continue;
        vAddressesForFrozen.push_back(address);
    }
    
    // Follow outputs and compute taken values
    for(const pair<CScript, int64_t>& s : vecSend) {
        int64_t nValueLeft = s.second;
        // take reserves in defined order
        for(const CTxDestination& address : vAddressesForFrozen) {
            int64_t nValueAvailableAt = mapAvailableValuesAt[address];
            int64_t& nValueTakeAt = mapTakeValuesAt[address];
            int64_t nValueLeftAt = nValueAvailableAt-nValueTakeAt;
            if (nValueAvailableAt ==0) continue;
            int64_t nValueToTake = nValueLeft;
            if (nValueToTake > nValueLeftAt)
                nValueToTake = nValueLeftAt;

            nValueTakeAt += nValueToTake;
            nValueLeft -= nValueToTake;
            
            if (nValueLeft == 0) break;
        }
        // if nValueLeft is left then is taken from change (liquidity)
        nValueToTakeFromChange += nValueLeft;
    }
    
    // Calculate change (minus fee and part taken from change)
    int64_t nTakeFromChangeLeft = nValueToTakeFromChange + nFeeRet;
    for (const CTxDestination& address : vInputAddresses) {
        CScript scriptPubKey;
        scriptPubKey.SetDestination(address);
        int64_t nValueTake = mapTakeValuesAt[address];
        int64_t nValueInput = mapInputValuesAt[address];
        int64_t nValueChange = nValueInput - nValueTake;
        if (nValueChange > nTakeFromChangeLeft) {
            nValueChange -= nTakeFromChangeLeft;
            nTakeFromChangeLeft = 0;
        }
        if (nValueChange < nTakeFromChangeLeft) {
            nTakeFromChangeLeft -= nValueChange;
            nValueChange = 0;
        }
        if (nValueChange == 0) continue;
        rawTx.vout.push_back(CTxOut(nValueChange, scriptPubKey));
    }
    
    // Fill vin
    for(const CCoinToUse& coin : vCoins) {
        rawTx.vin.push_back(CTxIn(coin.txhash,coin.i));
    }
    
    // Calculate peg
    CFractions feesFractions(0, CFractions::STD);
    map<int, CFractions> mapTxOutputFractions;
    computeTxPegForNextCycle(rawTx, peglevel_net, txdb, pegdb, mapAllOutputs,
                             mapTxOutputFractions, feesFractions);

    // first out after F notations (same num as size inputs)
    if (!mapTxOutputFractions.count(rawTx.vin.size())) {
        throw JSONRPCError(RPC_MISC_ERROR, "No withdraw fractions");
    }

    // signing the transaction to get it ready for broadcast
    int nIn = 0;
    for(const CCoinToUse& coin : vCoins) {
        if (!SignSignature(*pwalletMain, coin.scriptPubKey, rawTx, nIn++)) {
            throw JSONRPCError(RPC_MISC_ERROR, 
                               strprintf("Fail on signing input (%d)", nIn-1));
        }
    }
        
    // first out after F notations (same num as size inputs)
    CFractions frProcessed = mapTxOutputFractions[rawTx.vin.size()] + feesFractions;
    CFractions frRequested = frAmount;

    if (frRequested.Total() != nAmountWithFee) {
        throw JSONRPCError(RPC_MISC_ERROR, 
                           strprintf("Mismatch requested and amount_with_fee (%d - %d)",
                                     frRequested.Total(), nAmountWithFee));
    }
    if (frProcessed.Total() != nAmountWithFee) {
        throw JSONRPCError(RPC_MISC_ERROR, 
                           strprintf("Mismatch processed and amount_with_fee (%d - %d)",
                                     frProcessed.Total(), nAmountWithFee));
    }
    
    // get list of consumed inputs
    for (size_t i=0; i< rawTx.vin.size(); i++) {
        const COutPoint & prevout = rawTx.vin[i].prevout;
        auto fkey = uint320(prevout.hash, prevout.n);
        if (mapProvidedOutputs.count(fkey)) mapProvidedOutputs.erase(fkey);
        if (!sConsumedInputs.empty()) sConsumedInputs += ",";
        sConsumedInputs += fkey.GetHex();
    }
    
    // get list of provided outputs and save fractions
    sProvidedOutputs.clear();
    for(const pair<uint320,CCoinToUse> & item : mapProvidedOutputs) {
        CDataStream ss(SER_NETWORK, PROTOCOL_VERSION);
        ss << item.second;
        if (!sProvidedOutputs.empty()) sProvidedOutputs += ",";
        sProvidedOutputs += HexStr(ss.begin(), ss.end());
    }

    string sTxhash = rawTx.GetHash().GetHex();

    // get list of changes and add to current provided outputs
    map<string, int64_t> mapTxChanges;
    {
        CPegDB pegdbrw;
        for (size_t i=rawTx.vin.size()+1; i< rawTx.vout.size(); i++) { // skip notations and withdraw
            // make map of change outputs
            string txout = rawTx.GetHash().GetHex()+":"+itostr(i);
            mapTxChanges[txout] = rawTx.vout[i].nValue;
            // save these outputs in pegdb, so they can be used in next withdraws
            auto fkey = uint320(rawTx.GetHash(), i);
            pegdbrw.WriteFractions(fkey, mapTxOutputFractions[i]);
        }
    }
    collectProvided(rawTx, sAddress, nCycleNow, sProvidedOutputs);
    
    frBalance -= frRequested;
    frExchange -= frRequested;
    frPegShift += (frRequested - frProcessed);
    
    // consume reserve part of pegshift by balance
    // as computation were completed by pegnext it may use fractions
    // of current liquid - at current supply not to consume these fractions
    consumereservepegshift(frBalance, frExchange, frPegShift, peglevel_exchange);
    
    CDataStream ss(SER_NETWORK, PROTOCOL_VERSION);
    ss << rawTx;
    
    string txstr = HexStr(ss.begin(), ss.end());
    
    Object result;
    result.push_back(Pair("completed", true));
    result.push_back(Pair("txhash", sTxhash));
    result.push_back(Pair("rawtx", txstr));

    result.push_back(Pair("consumed_inputs", sConsumedInputs));
    result.push_back(Pair("provided_outputs", sProvidedOutputs));

    result.push_back(Pair("created_on_peg", peglevel_net.nSupply));
    result.push_back(Pair("broadcast_on_peg", peglevel_net.nSupplyNext));
    
    printpegbalance(frBalance, peglevel_exchange, result, "balance_", true);
    printpegbalance(frProcessed, peglevel_exchange, result, "processed_", true);
    printpegbalance(frExchange, peglevel_exchange, result, "exchange_", true);
    
    printpegshift(frPegShift, peglevel_net, result, true);
    
    Array changes;
    for(const pair<string, int64_t> & item : mapTxChanges) {
        Object obj;
        obj.push_back(Pair("txout", item.first));
        obj.push_back(Pair("amount", item.second));
        changes.push_back(obj);
    }
    result.push_back(Pair("changes", changes));
    
    return result;
}
