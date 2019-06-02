// Copyright (c) 2018 yshurik
// Distributed under the MIT/X11 software license, see the accompanying
// file COPYING or http://www.opensource.org/licenses/mit-license.php.

#include <map>
#include <set>
#include <cstdint>
#include <type_traits>
#include <fstream>
#include <utility>
#include <algorithm> 

#include <boost/version.hpp>
#include <boost/filesystem.hpp>
#include <boost/filesystem/fstream.hpp>
#include <boost/foreach.hpp>
#include <boost/multiprecision/cpp_int.hpp>
#include <boost/algorithm/string/predicate.hpp>
#include <boost/algorithm/string.hpp>
#include <boost/algorithm/string/split.hpp>

#include <leveldb/env.h>
#include <leveldb/cache.h>
#include <leveldb/filter_policy.h>
#include <memenv/memenv.h>

#include "peg.h"
#include "txdb-leveldb.h"
#include "pegdb-leveldb.h"
#include "util.h"
#include "main.h"
#include "base58.h"

#include <zconf.h>
#include <zlib.h>

using namespace std;
using namespace boost;

int nPegStartHeight = 1000000000; // 1G blocks as high number
int nPegMaxSupplyIndex = 1198;
bool fPegIsActivatedViaTx = false;
bool fPegDemoMode = false;
set<std::string> vPegWhitelist;
uint256 pegWhiteListHash = 0;

static string sBurnAddress =
    "bJnV8J5v74MGctMyVSVPfGu1mGQ9nMTiB3";

extern leveldb::DB *txdb; // global pointer for LevelDB object instance

bool ReadWhitelistInfo() {
    filesystem::path whitelistfile = GetDataDir() / "pegwhitelist.dat";
    ifstream infile(whitelistfile.string());
    if (!infile.is_open())
        return false;
    bool fHasPegStart = false;
    bool fHasAddresses = false;;
    string line;
    string input;
    while (getline(infile, line)) {
        boost::trim(line);
        if (line.length()==34) {
            vPegWhitelist.insert(line);
            fHasAddresses = true;
            input += line;
        }
        if (boost::starts_with(line, "#")) {
            char * pEnd = nullptr;
            auto sPegStartHeight = line.c_str()+1;
            nPegStartHeight = int(strtol(sPegStartHeight, &pEnd, 0));
            bool fValidInt = !(pEnd == sPegStartHeight) && nPegStartHeight > 0;
            if (!fValidInt) {
                return false; // not convertible to block number
            }
            fHasPegStart = true;
        }
    }

    if (!fHasAddresses || !fHasPegStart)
        return false;

    CDataStream ss(SER_GETHASH, 0);
    ss << input;
    pegWhiteListHash = Hash(ss.begin(), ss.end());
    
    fPegDemoMode = true;
    
    return true;
}

bool SetBlocksIndexesReadyForPeg(CTxDB & ctxdb, LoadMsg load_msg) {
    if (!ctxdb.TxnBegin())
        return error("SetBlocksIndexesReadyForPeg() : TxnBegin failed");

    leveldb::Iterator *iterator = txdb->NewIterator(leveldb::ReadOptions());
    // Seek to start key.
    CDataStream ssStartKey(SER_DISK, CLIENT_VERSION);
    ssStartKey << make_pair(string("blockindex"), uint256(0));
    iterator->Seek(ssStartKey.str());
    // Now read each entry.
    int indexCount = 0;
    while (iterator->Valid())
    {
        boost::this_thread::interruption_point();
        // Unpack keys and values.
        CDataStream ssKey(SER_DISK, CLIENT_VERSION);
        ssKey.write(iterator->key().data(), iterator->key().size());
        CDataStream ssValue(SER_DISK, CLIENT_VERSION);
        ssValue.write(iterator->value().data(), iterator->value().size());
        string strType;
        ssKey >> strType;
        // Did we reach the end of the data to read?
        if (strType != "blockindex")
            break;
        CDiskBlockIndex diskindex;
        ssValue >> diskindex;

        uint256 blockHash = diskindex.GetBlockHash();
        CBlockIndex* pindexNew = ctxdb.InsertBlockIndex(blockHash);

        pindexNew->SetPeg(pindexNew->nHeight >= nPegStartHeight);
        diskindex.SetPeg(pindexNew->nHeight >= nPegStartHeight);
        ctxdb.WriteBlockIndex(diskindex);

        iterator->Next();

        indexCount++;
        if (indexCount % 10000 == 0) {
            load_msg(std::string(" update indexes for peg: ")+std::to_string(indexCount));
            // commit on every 10k
            if (!ctxdb.TxnCommit())
                return error("SetBlocksIndexesReadyForPeg() : TxnCommit failed");
            if (!ctxdb.TxnBegin())
                return error("SetBlocksIndexesReadyForPeg() : TxnBegin failed");
        }
    }
    delete iterator;

    if (!ctxdb.WriteBlockIndexIsPegReady(true))
        return error("SetBlocksIndexesReadyForPeg() : flag write failed");

    if (!ctxdb.TxnCommit())
        return error("SetBlocksIndexesReadyForPeg() : TxnCommit failed");

    return true;
}

bool CalculateBlockPegIndex(CBlockIndex* pindex)
{
    if (!pindex->pprev) {
        pindex->nPegSupplyIndex =0;
        return true;
    }
    
    pindex->nPegSupplyIndex = pindex->pprev->GetNextBlockPegSupplyIndex();
    return true;
}

int CBlockIndex::GetNextBlockPegSupplyIndex() const
{
    int nNextHeight = nHeight+1;
    int nPegInterval = Params().PegInterval(nNextHeight);
    
    if (nNextHeight < nPegStartHeight) {
        return 0;
    }
    if (nNextHeight % nPegInterval != 0) {
        return nPegSupplyIndex;
    }

    // back to 2 intervals and -1 to count voice of back-third interval, as votes sum at nPegInterval-1
    auto usevotesindex = this;
    while (usevotesindex->nHeight > (nNextHeight - nPegInterval*2 -1))
        usevotesindex = usevotesindex->pprev;

    // back to 3 intervals and -1 for votes calculations of 2x and 3x
    auto prevvotesindex = this;
    while (prevvotesindex->nHeight > (nNextHeight - nPegInterval*3 -1))
        prevvotesindex = prevvotesindex->pprev;

    return ComputeNextPegSupplyIndex(nPegSupplyIndex, usevotesindex, prevvotesindex);
}

int CBlockIndex::GetNextIntervalPegSupplyIndex() const
{
    if (nHeight < nPegStartHeight) {
        return 0;
    }
    
    int nPegInterval = Params().PegInterval(nHeight);
    int nCurrentInterval = nHeight / nPegInterval;
    int nCurrentIntervalStart = nCurrentInterval * nPegInterval;

    // back to 2 intervals and -1 to count voice of back-third interval, as votes sum at nPegInterval-1
    auto usevotesindex = this;
    while (usevotesindex->nHeight > (nCurrentIntervalStart - nPegInterval*1 -1))
        usevotesindex = usevotesindex->pprev;

    // back to 3 intervals and -1 for votes calculations of 2x and 3x
    auto prevvotesindex = this;
    while (prevvotesindex->nHeight > (nCurrentIntervalStart - nPegInterval*2 -1))
        prevvotesindex = prevvotesindex->pprev;

    return CBlockIndex::ComputeNextPegSupplyIndex(nPegSupplyIndex, usevotesindex, prevvotesindex);
}

int CBlockIndex::GetNextNextIntervalPegSupplyIndex() const
{
    if (nHeight < nPegStartHeight) {
        return 0;
    }
    
    int nPegInterval = Params().PegInterval(nHeight);
    int nCurrentInterval = nHeight / nPegInterval;
    int nCurrentIntervalStart = nCurrentInterval * nPegInterval;

    // back to 2 intervals and -1 to count voice of back-third interval, as votes sum at nPegInterval-1
    auto usevotesindex = this;
    while (usevotesindex->nHeight > (nCurrentIntervalStart - nPegInterval*0 -1))
        usevotesindex = usevotesindex->pprev;

    // back to 3 intervals and -1 for votes calculations of 2x and 3x
    auto prevvotesindex = this;
    while (prevvotesindex->nHeight > (nCurrentIntervalStart - nPegInterval*1 -1))
        prevvotesindex = prevvotesindex->pprev;

    return CBlockIndex::ComputeNextPegSupplyIndex(GetNextIntervalPegSupplyIndex(), usevotesindex, prevvotesindex);
}

// #NOTE15
int CBlockIndex::ComputeNextPegSupplyIndex(int nPegBase,
                                           const CBlockIndex* back2interval,
                                           const CBlockIndex* back3interval) {
    auto usevotesindex = back2interval;
    auto prevvotesindex = back3interval;
    int nNextPegSupplyIndex = nPegBase;
    
    int inflate = usevotesindex->nPegVotesInflate;
    int deflate = usevotesindex->nPegVotesDeflate;
    int nochange = usevotesindex->nPegVotesNochange;

    int inflate_prev = prevvotesindex->nPegVotesInflate;
    int deflate_prev = prevvotesindex->nPegVotesDeflate;
    int nochange_prev = prevvotesindex->nPegVotesNochange;

    if (deflate > inflate && deflate > nochange) {
        nNextPegSupplyIndex++;
        if (deflate > 2*inflate_prev && deflate > 2*nochange_prev) nNextPegSupplyIndex++;
        if (deflate > 3*inflate_prev && deflate > 3*nochange_prev) nNextPegSupplyIndex++;
    }
    if (inflate > deflate && inflate > nochange) {
        nNextPegSupplyIndex--;
        if (inflate > 2*deflate_prev && inflate > 2*nochange_prev) nNextPegSupplyIndex--;
        if (inflate > 3*deflate_prev && inflate > 3*nochange_prev) nNextPegSupplyIndex--;
    }

    // over max
    if (nNextPegSupplyIndex >= nPegMaxSupplyIndex)
        nNextPegSupplyIndex = nPegMaxSupplyIndex;
    // less min
    else if (nNextPegSupplyIndex <0)
        nNextPegSupplyIndex = 0;
    
    return nNextPegSupplyIndex;
}

int CalculatePegVotes(const CFractions & fractions, int nPegSupplyIndex)
{
    int nVotes=1;
    
    int64_t nReserveWeight=0;
    int64_t nLiquidWeight=0;

    fractions.LowPart(nPegSupplyIndex, &nReserveWeight);
    fractions.HighPart(nPegSupplyIndex, &nLiquidWeight);

    if (nLiquidWeight > INT_LEAST64_MAX/(nPegSupplyIndex+2)) {
        // check for rare extreme case when user stake more than about 100M coins
        // in this case multiplication is very close int64_t overflow (int64 max is ~92 GCoins)
        multiprecision::uint128_t nLiquidWeight128(nLiquidWeight);
        multiprecision::uint128_t nPegSupplyIndex128(nPegSupplyIndex);
        multiprecision::uint128_t nPegMaxSupplyIndex128(nPegMaxSupplyIndex);
        multiprecision::uint128_t f128 = (nLiquidWeight128*nPegSupplyIndex128)/nPegMaxSupplyIndex128;
        nLiquidWeight -= f128.convert_to<int64_t>();
    }
    else // usual case, fast calculations
        nLiquidWeight -= nLiquidWeight * nPegSupplyIndex / nPegMaxSupplyIndex;

    int nWeightMultiplier = nPegSupplyIndex/120+1;
    if (nLiquidWeight > (nReserveWeight*4)) {
        nVotes = 4*nWeightMultiplier;
    }
    else if (nLiquidWeight > (nReserveWeight*3)) {
        nVotes = 3*nWeightMultiplier;
    }
    else if (nLiquidWeight > (nReserveWeight*2)) {
        nVotes = 2*nWeightMultiplier;
    }
    
    return nVotes;
}

bool CalculateBlockPegVotes(const CBlock & cblock, CBlockIndex* pindex, CPegDB& pegdb)
{
    int nPegInterval = Params().PegInterval(pindex->nHeight);
    
    if (!cblock.IsProofOfStake() || pindex->nHeight < nPegStartHeight) {
        pindex->nPegVotesInflate =0;
        pindex->nPegVotesDeflate =0;
        pindex->nPegVotesNochange =0;
        return true;
    }

    if (pindex->nHeight % nPegInterval == 0) {
        pindex->nPegVotesInflate =0;
        pindex->nPegVotesDeflate =0;
        pindex->nPegVotesNochange =0;
    }
    else if (pindex->pprev) {
        pindex->nPegVotesInflate = pindex->pprev->nPegVotesInflate;
        pindex->nPegVotesDeflate = pindex->pprev->nPegVotesDeflate;
        pindex->nPegVotesNochange = pindex->pprev->nPegVotesNochange;
    }

    int nVotes=1;

    const CTransaction & tx = cblock.vtx[1];

    size_t n_vin = tx.vin.size();
    if (tx.IsCoinBase()) n_vin = 0;
    for (unsigned int i = 0; i < n_vin; i++)
    {
        CFractions fractions(0, CFractions::STD);
        const COutPoint & prevout = tx.vin[i].prevout;
        if (!pegdb.ReadFractions(uint320(prevout.hash, prevout.n), fractions)) {
            return false;
        }

        nVotes = CalculatePegVotes(fractions, pindex->nPegSupplyIndex);
        break;
    }

    for(const CTxOut & out : tx.vout) {
        const CScript& scriptPubKey = out.scriptPubKey;

        txnouttype type;
        vector<CTxDestination> addresses;
        int nRequired;

        if (!ExtractDestinations(scriptPubKey, type, addresses, nRequired)) {
            continue;
        }

        bool voted = false;
        for(const CTxDestination& addr : addresses) {
            std::string str_addr = CBitcoinAddress(addr).ToString();
            if (str_addr == Params().PegInflateAddr()) {
                pindex->nPegVotesInflate += nVotes;
                voted = true;
                break;
            }
            else if (str_addr == Params().PegDeflateAddr()) {
                pindex->nPegVotesDeflate += nVotes;
                voted = true;
                break;
            }
            else if (str_addr == Params().PegNochangeAddr()) {
                pindex->nPegVotesNochange += nVotes;
                voted = true;
                break;
            }
        }

        if (voted) // only one vote to count
            break;
    }

    return true;
}

CFractions::CFractions()
    :nFlags(VALUE)
{
    f[0] = 0; // fast init first item
}
CFractions::CFractions(int64_t value, uint32_t flags)
    :nFlags(flags)
{
    if (flags & VALUE)
        f[0] = value; // fast init first item
    else if (flags & STD) {
        f[0] = value;
        nFlags = VALUE;
        ToStd();
        nFlags = flags;
    }
    else {
        assert(0);
    }
}
CFractions::CFractions(const CFractions & o)
    :nFlags(o.nFlags)
    ,nLockTime(o.nLockTime)
{
    for(int i=0; i< PEG_SIZE; i++) {
        f[i] = o.f[i];
    }
}

void CFractions::ToDeltas(int64_t* deltas) const
{
    int64_t fp = 0;
    for(int i=0; i<PEG_SIZE; i++) {
        if (i==0) {
            fp = deltas[0] = f[0];
            continue;
        }
        deltas[i] = f[i]-fp*(PEG_RATE-1)/PEG_RATE;
        fp = f[i];
    }
}

void CFractions::FromDeltas(const int64_t* deltas)
{
    int64_t fp = 0;
    for(int i=0; i<PEG_SIZE; i++) {
        if (i==0) {
            fp = f[0] = deltas[0];
            continue;
        }
        f[i] = deltas[i]+fp*(PEG_RATE-1)/PEG_RATE;
        fp = f[i];
    }
}

bool CFractions::Pack(CDataStream& out, unsigned long* report_len) const
{
    if (nFlags & VALUE) {
        if (report_len) *report_len = sizeof(int64_t);
        out << uint32_t(nFlags | SER_VALUE);
        out << nLockTime;
        out << f[0];
    } else {
        int64_t deltas[PEG_SIZE];
        ToDeltas(deltas);

        int zlevel = 9;
        unsigned char zout[2*PEG_SIZE*sizeof(int64_t)];
        unsigned long n = PEG_SIZE*sizeof(int64_t);
        unsigned long zlen = PEG_SIZE*2*sizeof(int64_t);
        auto src = reinterpret_cast<const unsigned char *>(deltas);
        int res = ::compress2(zout, &zlen, src, n, zlevel);
        if (res == Z_OK) {
            if (report_len) *report_len = zlen;
            out << uint32_t(nFlags | SER_ZDELTA);
            out << nLockTime;
            auto ser = reinterpret_cast<const char *>(zout);
            out << zlen;
            out.write(ser, zlen);
        }
        else {
            if (report_len) *report_len = PEG_SIZE*sizeof(int64_t);
            out << uint32_t(nFlags | SER_RAW);
            out << nLockTime;
            auto ser = reinterpret_cast<const char *>(f);
            out.write(ser, PEG_SIZE*sizeof(int64_t));
        }
    }
    return true;
}

bool CFractions::Unpack(CDataStream& inp)
{
    uint32_t nSerFlags = 0;
    inp >> nSerFlags;
    inp >> nLockTime;
    if (nSerFlags & SER_VALUE) {
        nFlags = nSerFlags | VALUE;
        inp >> f[0];
    }
    else if (nSerFlags & SER_ZDELTA) {
        unsigned long zlen = 0;
        inp >> zlen;

        if (zlen>(2*PEG_SIZE*sizeof(int64_t))) {
            // data are broken, no read
            return false;
        }

        unsigned char zinp[2*PEG_SIZE*sizeof(int64_t)];
        unsigned long n = PEG_SIZE*sizeof(int64_t);
        auto ser = reinterpret_cast<char *>(zinp);
        inp.read(ser, zlen);

        int64_t deltas[PEG_SIZE];
        auto src = reinterpret_cast<const unsigned char *>(ser);
        auto dst = reinterpret_cast<unsigned char *>(deltas);
        int res = ::uncompress(dst, &n, src, zlen);
        if (res != Z_OK) {
            // data are broken, can not uncompress
            return false;
        }
        FromDeltas(deltas);
        nFlags = nSerFlags | STD;
    }
    else if (nSerFlags & SER_RAW) {
        auto ser = reinterpret_cast<char *>(f);
        inp.read(ser, PEG_SIZE*sizeof(int64_t));
        nFlags = nSerFlags | STD;
    }
    nFlags &= SER_MASK;
    return true;
}

CFractions CFractions::Std() const
{
    if ((nFlags & VALUE) ==0)
        return *this;

    CFractions fstd;
    fstd.nFlags = nFlags;
    fstd.nFlags &= ~uint32_t(VALUE);
    fstd.nFlags |= STD;

    int64_t v = f[0];
    for(int i=0;i<PEG_SIZE;i++) {
        if (i == PEG_SIZE-1) {
            fstd.f[i] = v;
            break;
        }
        int64_t frac = v/PEG_RATE;
        fstd.f[i] = frac;
        v -= frac;
    }
    return fstd;
}

bool CFractions::IsPositive() const 
{
    if (nFlags & VALUE)
        return true;

    for(int i=0;i<PEG_SIZE;i++) {
        if (f[i] <0)
            return false;
    }
    return true;
}

bool CFractions::IsNegative() const 
{
    if (nFlags & VALUE)
        return false;

    for(int i=0;i<PEG_SIZE;i++) {
        if (f[i] >0)
            return false;
    }
    return true;
}

int64_t CFractions::Total() const
{
    int64_t nValue =0;
    if (nFlags & VALUE)
        return f[0];

    for(int i=0;i<PEG_SIZE;i++) {
        nValue += f[i];
    }
    return nValue;
}

int64_t CFractions::Low(int supply) const
{
    int64_t nValue =0;
    if (nFlags & VALUE)
        return Std().Low(supply);

    for(int i=0;i<supply;i++) {
        nValue += f[i];
    }
    return nValue;
}

int64_t CFractions::High(int supply) const
{
    int64_t nValue =0;
    if (nFlags & VALUE)
        return Std().High(supply);

    for(int i=supply;i<PEG_SIZE;i++) {
        nValue += f[i];
    }
    return nValue;
}

int64_t CFractions::Change(int src_supply, int dst_supply) const
{
    int64_t nValueSrc = High(src_supply);
    int64_t nValueDst = High(dst_supply);
    return nValueDst - nValueSrc;
}

void CFractions::ToStd()
{
    if ((nFlags & VALUE) == 0)
        return;

    nFlags &= ~uint32_t(VALUE);
    nFlags |= STD;

    int64_t v = f[0];
    for(int i=0;i<PEG_SIZE;i++) {
        if (i == PEG_SIZE-1) {
            f[i] = v;
            break;
        }
        int64_t frac = v/PEG_RATE;
        f[i] = frac;
        v -= frac;
    }
}

CFractions CFractions::Positive(int64_t* total) const
{
    if ((nFlags & STD) == 0) {
        return Std().Positive(total);
    }
    CFractions frPositive(0, CFractions::STD);
    for(int i=0; i<PEG_SIZE; i++) {
        if (f[i] <=0) continue; 
        frPositive.f[i] = f[i];
        if (total) *total += f[i];
    }
    return frPositive;
}
CFractions CFractions::Negative(int64_t* total) const
{
    if ((nFlags & STD) == 0) {
        return Std().Negative(total);
    }
    CFractions frNegative(0, CFractions::STD);
    for(int i=0; i<PEG_SIZE; i++) {
        if (f[i] >=0) continue; 
        frNegative.f[i] = f[i];
        if (total) *total += f[i];
    }
    return frNegative;
}


CFractions CFractions::LowPart(int supply, int64_t* total) const
{
    if ((nFlags & STD) == 0) {
        return Std().LowPart(supply, total);
    }
    CFractions frLowPart(0, CFractions::STD);
    for(int i=0; i<supply; i++) {
        if (total) *total += f[i];
        frLowPart.f[i] += f[i];
    }
    return frLowPart;
}
CFractions CFractions::HighPart(int supply, int64_t* total) const
{
    if ((nFlags & STD) == 0) {
        return Std().HighPart(supply, total);
    }
    CFractions frHighPart(0, CFractions::STD);
    for(int i=supply; i<PEG_SIZE; i++) {
        if (total) *total += f[i];
        frHighPart.f[i] += f[i];
    }
    return frHighPart;
}

static int64_t RatioPart(int64_t nValue,
                         int64_t nPartValue,
                         int64_t nTotalValue) {
    if (nPartValue == 0 || nTotalValue == 0)
        return 0;
    
    bool has_overflow = false;
    if (std::is_same<int64_t,long>()) {
        long m_test;
        has_overflow = __builtin_smull_overflow(nValue, nPartValue, &m_test);
    } else if (std::is_same<int64_t,long long>()) {
        long long m_test;
        has_overflow = __builtin_smulll_overflow(nValue, nPartValue, &m_test);
    } else {
        assert(0); // todo: compile error
    }

    if (has_overflow) {
        multiprecision::uint128_t v128(nValue);
        multiprecision::uint128_t part128(nPartValue);
        multiprecision::uint128_t f128 = (v128*part128)/nTotalValue;
        return f128.convert_to<int64_t>();
    }
    
    return (nValue*nPartValue)/nTotalValue;
}

/** Take a part as ration part/total where part is also value (sum fraction)
 *  Returned fractions are also adjusted for part for rounding differences.
 *  #NOTE8
 */
CFractions CFractions::RatioPart(int64_t nPartValue) const {
    if ((nFlags & STD) == 0) {
        return Std().RatioPart(nPartValue);
    }
    int64_t nTotalValue = Total();
    int64_t nPartValueSum = 0;
    CFractions fPart(0, CFractions::STD);

    if (nPartValue == 0 && nTotalValue == 0)
        return fPart;
    if (nPartValue == 0)
        return fPart;
    if (nPartValue > nTotalValue)
        return Std();

    int adjust_from = PEG_SIZE;
    for(int i=0; i<PEG_SIZE; i++) {
        int64_t v = f[i];
        
        if (v != 0 && i < adjust_from) {
            adjust_from = i;
        }

        bool has_overflow = false;
        if (std::is_same<int64_t,long>()) {
            long m_test;
            has_overflow = __builtin_smull_overflow(v, nPartValue, &m_test);
        } else if (std::is_same<int64_t,long long>()) {
            long long m_test;
            has_overflow = __builtin_smulll_overflow(v, nPartValue, &m_test);
        } else {
            assert(0); // todo: compile error
        }

        if (has_overflow) {
            multiprecision::uint128_t v128(v);
            multiprecision::uint128_t part128(nPartValue);
            multiprecision::uint128_t f128 = (v128*part128)/nTotalValue;
            fPart.f[i] = f128.convert_to<int64_t>();
        }
        else {
            fPart.f[i] = (v*nPartValue)/nTotalValue;
        }

        nPartValueSum += fPart.f[i];
    }
    
    if (nPartValueSum == nPartValue)
        return fPart;
    if (nPartValueSum > nPartValue)
        return fPart; // todo:peg: validate if possible
    
    int idx = adjust_from;
    int64_t nAdjustValue = nPartValue - nPartValueSum;
    while(nAdjustValue >0) {
        // todo:peg: review all possible cases if rounding mismatch with adjust_from
        if (fPart.f[idx] < f[idx]) {
            nAdjustValue--;
            fPart.f[idx]++;
        }
        idx++;
        if (idx >= PEG_SIZE) {
            idx = adjust_from;
        }
    }
    
    return fPart;
}

/** Take a part as ration part/total where part is also value (sum fraction)
 *  Add taken part into destination fractions. Adjusted from adjust_from.
 *  Returns not completed amount (if source("this") has no enough)
 *  #NOTE8
 */
int64_t CFractions::MoveRatioPartTo(int64_t nValueToMove,
                                    CFractions& b) 
{
    int64_t nTotalValue = Total();
    int64_t nPartValue = nValueToMove;

    if (nTotalValue == 0)
        return nValueToMove;
    if (nValueToMove == 0)
        return 0;
    
    if ((nFlags & STD) == 0)
        ToStd();
    if ((b.nFlags & STD) == 0)
        b.ToStd();
    
    if (nPartValue >= nTotalValue) {
        nPartValue = nTotalValue;
        b += *this; // move all
        for(int i=0; i<PEG_SIZE; i++) f[i] = 0; // taken all
        return nValueToMove - nPartValue;
    }
    
    int64_t nPartValueSum = 0;
    int adjust_from = PEG_SIZE;
    for(int i=0; i<PEG_SIZE; i++) {
        int64_t v = f[i];

        if (v != 0 && i < adjust_from) {
            adjust_from = i;
        }
        
        bool has_overflow = false;
        if (std::is_same<int64_t,long>()) {
            long m_test;
            has_overflow = __builtin_smull_overflow(v, nPartValue, &m_test);
        } else if (std::is_same<int64_t,long long>()) {
            long long m_test;
            has_overflow = __builtin_smulll_overflow(v, nPartValue, &m_test);
        } else {
            assert(0); // todo: compile error
        }

        int64_t vp = 0;
        
        if (has_overflow) {
            multiprecision::uint128_t v128(v);
            multiprecision::uint128_t part128(nPartValue);
            multiprecision::uint128_t f128 = (v128*part128)/nTotalValue;
            vp = f128.convert_to<int64_t>();
        }
        else {
            vp = (v*nPartValue)/nTotalValue;
        }

        nPartValueSum += vp;
        b.f[i] += vp;
        f[i] -= vp;
    }
    
    if (nPartValueSum == nPartValue)
        return 0;
    if (nPartValueSum > nPartValue)
        return 0; // todo:peg: validate if possible
    
    int idx = adjust_from;
    int64_t nAdjustValue = nPartValue - nPartValueSum;
    while(nAdjustValue >0) {
        if (f[idx] >0) {
            nAdjustValue--;
            b.f[idx]++;
            f[idx]--;
        }
        idx++;
        if (idx >= PEG_SIZE) {
            idx = adjust_from;
        }
    }
    
    return 0;
}

CFractions& CFractions::operator+=(const CFractions& b)
{
    if ((b.nFlags & STD) == 0) {
        return operator+=(b.Std());
    }
    if ((nFlags & STD) == 0) {
        ToStd();
    }
    for(int i=0; i<PEG_SIZE; i++) {
        f[i] += b.f[i];
    }
    return *this;
}

CFractions& CFractions::operator-=(const CFractions& b)
{
    if ((b.nFlags & STD) == 0) {
        return operator-=(b.Std());
    }
    if ((nFlags & STD) == 0) {
        ToStd();
    }
    for(int i=0; i<PEG_SIZE; i++) {
        f[i] -= b.f[i];
    }
    return *this;
}

CFractions CFractions::operator&(const CFractions& b) const
{
    CFractions a = *this;
    for(int i=0; i<PEG_SIZE; i++) {
        int64_t va = a.f[i];
        int64_t vb = b.f[i];
        if      (va >=0 && vb >=0) a.f[i] = std::min(va, vb);
        else if (va >=0 && vb < 0) a.f[i] = 0;
        else if (va < 0 && vb >=0) a.f[i] = 0;
        else if (va < 0 && vb < 0) a.f[i] = std::max(va, vb);
    }
    return a;
}

CFractions CFractions::operator-() const
{
    CFractions a = *this;
    for(int i=0; i<PEG_SIZE; i++) {
        int64_t va = a.f[i];
        a.f[i] = -va;
    }
    return a;
}

double CFractions::Distortion(const CFractions& b) const
{
    int64_t nTotalA = Total();
    int64_t nTotalB = b.Total();
    
    if (nTotalA == nTotalB) {
        
        if (nTotalA == 0) {
            return 0;
        }
        
        int64_t nDiff = 0;
        
        for(int i=0; i<PEG_SIZE; i++) {
            int64_t va = f[i];
            int64_t vb = b.f[i];
            if (va > vb) {
                nDiff += (va - vb);
            }
        }
        
        return double(nDiff) / double(nTotalA);
    }
    
    else if (nTotalA < nTotalB) {
        if (nTotalA == 0) {
            return nTotalB; // does not make sense to scale
        }
        
        CFractions b1 = b.RatioPart(nTotalA);
        return Distortion(b1);
    }
    
    else if (nTotalA > nTotalB) {
        if (nTotalB == 0) {
            return nTotalA; // does not make sense to scale
        }
        
        CFractions a1 = RatioPart(nTotalB);
        return a1.Distortion(b);
    }
    
    return 0;
}

static string toAddress(const CScript& scriptPubKey,
                        bool* ptrIsNotary = nullptr,
                        string* ptrNotary = nullptr) {
    int nRequired;
    txnouttype type;
    vector<CTxDestination> addresses;
    if (ExtractDestinations(scriptPubKey, type, addresses, nRequired)) {
        std::string str_addr_all;
        bool fNone = true;
        for(const CTxDestination& addr : addresses) {
            std::string str_addr = CBitcoinAddress(addr).ToString();
            if (!str_addr_all.empty())
                str_addr_all += "\n";
            str_addr_all += str_addr;
            fNone = false;
        }
        if (!fNone)
            return str_addr_all;
    }

    if (ptrNotary || ptrIsNotary) {
        if (ptrIsNotary) *ptrIsNotary = false;
        if (ptrNotary) *ptrNotary = "";

        opcodetype opcode1;
        vector<unsigned char> vch1;
        CScript::const_iterator pc1 = scriptPubKey.begin();
        if (scriptPubKey.GetOp(pc1, opcode1, vch1)) {
            if (opcode1 == OP_RETURN && scriptPubKey.size()>1) {
                if (ptrIsNotary) *ptrIsNotary = true;
                if (ptrNotary) {
                    unsigned long len_bytes = scriptPubKey[1];
                    if (len_bytes > scriptPubKey.size()-2)
                        len_bytes = scriptPubKey.size()-2;
                    for (uint32_t i=0; i< len_bytes; i++) {
                        ptrNotary->push_back(char(scriptPubKey[i+2]));
                    }
                }
            }
        }
    }

    string as_bytes;
    unsigned long len_bytes = scriptPubKey.size();
    for(unsigned int i=0; i< len_bytes; i++) {
        as_bytes += char(scriptPubKey[i]);
    }
    return as_bytes;
}

bool IsPegWhiteListed(const CTransaction & tx,
                      MapPrevTx & txInputs)
{
    if (!fPegDemoMode)
        return true;
    
    size_t n_vin = tx.vin.size();
    if (tx.IsCoinBase()) n_vin = 0;
    for (unsigned int i = 0; i < n_vin; i++)
    {
        const COutPoint & prevout = tx.vin[i].prevout;
        CTransaction& txPrev = txInputs[prevout.hash].second;
        if (prevout.n >= txPrev.vout.size()) {
            continue;
        }
        
        auto sAddress = toAddress(txPrev.vout[prevout.n].scriptPubKey);
        if (vPegWhitelist.count(sAddress)) {
            return true;
        }
    }
    return false;
}

bool IsPegWhiteListed(const CTransaction & tx,
                      MapPrevOut & inputs)
{
    if (!fPegDemoMode)
        return true;
    
    size_t n_vin = tx.vin.size();
    if (tx.IsCoinBase()) n_vin = 0;
    for (unsigned int i = 0; i < n_vin; i++)
    {
        const COutPoint & prevout = tx.vin[i].prevout;
        auto fkey = uint320(prevout.hash, prevout.n);
        if (!inputs.count(fkey)) {
            continue;
        }
        const CTxOut & prevtxout = inputs[fkey];
        
        auto sAddress = toAddress(prevtxout.scriptPubKey);
        if (vPegWhitelist.count(sAddress)) {
            return true;
        }
    }
    return false;
}

bool CalculateStandardFractions(const CTransaction & tx,
                                int nSupply,
                                unsigned int nTime,
                                MapPrevTx & mapTxInputs,
                                MapFractions& mapInputsFractions,
                                MapFractions& mapTestFractionsPool,
                                CFractions& feesFractions,
                                std::string& sFailCause)
{
    MapPrevOut mapInputs;
    size_t n_vin = tx.vin.size();
        
    if (tx.IsCoinBase()) n_vin = 0;
    for (unsigned int i = 0; i < n_vin; i++)
    {
        const COutPoint & prevout = tx.vin[i].prevout;
        CTransaction& txPrev = mapTxInputs[prevout.hash].second;
        if (prevout.n >= txPrev.vout.size()) {
            sFailCause = "PI02: Refered output out of range";
            return false;
        }

        auto fkey = uint320(prevout.hash, prevout.n);
        mapInputs[fkey] = txPrev.vout[prevout.n];
    }
    
    return CalculateStandardFractions(tx,
                                      nSupply,
                                      nTime,
                                      mapInputs,
                                      mapInputsFractions,
                                      mapTestFractionsPool,
                                      feesFractions,
                                      sFailCause);
}

bool CalculateStandardFractions(const CTransaction & tx,
                                int nSupply,
                                unsigned int nTime,
                                MapPrevOut & mapInputs,
                                MapFractions& mapInputsFractions,
                                MapFractions& mapTestFractionsPool,
                                CFractions& feesFractions,
                                std::string& sFailCause)
{
    size_t n_vin = tx.vin.size();
    size_t n_vout = tx.vout.size();

    if (!IsPegWhiteListed(tx, mapInputs)) {
        sFailCause = "PI01: Not whitelisted";
        return true;
    }

    int64_t nValueIn = 0;
    int64_t nReservesTotal =0;
    int64_t nLiquidityTotal =0;

    map<string, CFractions> poolReserves;
    map<string, CFractions> poolLiquidity;
    map<long, FrozenTxOut> poolFrozen;
    bool fFreezeAll = false;

    set<string> setInputAddresses;

    if (tx.IsCoinBase()) n_vin = 0;
    for (unsigned int i = 0; i < n_vin; i++)
    {
        const COutPoint & prevout = tx.vin[i].prevout;
        auto fkey = uint320(prevout.hash, prevout.n);
        if (!mapInputs.count(fkey)) {
            sFailCause = "PI02: Refered output is not found";
            return false;
        }
        const CTxOut & prevtxout = mapInputs[fkey];
        
        int64_t nValue = prevtxout.nValue;
        nValueIn += nValue;
        auto sAddress = toAddress(prevtxout.scriptPubKey);
        setInputAddresses.insert(sAddress);

        if (mapInputsFractions.find(fkey) == mapInputsFractions.end()) {
            sFailCause = "PI03: No input fractions found";
            return false;
        }

        auto frInp = mapInputsFractions[fkey].Std();
        if (frInp.Total() != prevtxout.nValue) {
            sFailCause = "PI04: Input fraction total mismatches value";
            return false;
        }
        
        if (frInp.nFlags & CFractions::NOTARY_F) {
            if (frInp.nLockTime > tx.nTime) {
                sFailCause = "PI05: Frozen input used before time expired";
                return false;
            }
        }

        if (frInp.nFlags & CFractions::NOTARY_V) {
            if (frInp.nLockTime > tx.nTime) {
                sFailCause = "PI06: Voluntary frozen input used before time expired";
                return false;
            }
        }
        
        int64_t nReserveIn = 0;
        auto & frReserve = poolReserves[sAddress];
        frReserve += frInp.LowPart(nSupply, &nReserveIn);

        int64_t nLiquidityIn = 0;
        auto & frLiquidity = poolLiquidity[sAddress];
        frLiquidity += frInp.HighPart(nSupply, &nLiquidityIn);

        // check if intend to transfer frozen
        // if so need to do appropriate deductions from pools
        // if there is a notary on same position as input
        if (i < n_vout) {
            string sNotary;
            bool fNotary = false;
            {
                opcodetype opcode1;
                vector<unsigned char> vch1;
                const CScript& script1 = tx.vout[i].scriptPubKey;
                CScript::const_iterator pc1 = script1.begin();
                if (script1.GetOp(pc1, opcode1, vch1)) {
                    if (opcode1 == OP_RETURN && script1.size()>1) {
                        unsigned long len_bytes = script1[1];
                        if (len_bytes > script1.size()-2)
                            len_bytes = script1.size()-2;
                        fNotary = true;
                        for (uint32_t i=0; i< len_bytes; i++) {
                            sNotary.push_back(char(script1[i+2]));
                        }
                    }
                }
            }

            bool fNotaryF = boost::starts_with(sNotary, "**F**");
            bool fNotaryV = boost::starts_with(sNotary, "**V**");
            bool fNotaryL = boost::starts_with(sNotary, "**L**");
            
            // #NOTE5
            if (fNotary && (fNotaryF || fNotaryV || fNotaryL)) {
                bool fSharedFreeze = false;
                auto sOutputDef = sNotary.substr(5 /*length **F** */);
                vector<long> vFrozenIndexes;
                set<long> setFrozenIndexes;
                vector<string> vOutputArgs;
                boost::split(vOutputArgs, sOutputDef, boost::is_any_of(":"));
                for(string sOutputArg : vOutputArgs) {
                    char * pEnd = nullptr;
                    long nFrozenIndex = strtol(sOutputArg.c_str(), &pEnd, 0);
                    bool fValidIndex = !(pEnd == sOutputArg.c_str()) && nFrozenIndex >= 0 && size_t(nFrozenIndex) < n_vout;
                    if (!fValidIndex) {
                        sFailCause = "PI07: Freeze notary: not convertible to output index";
                        return false;
                    }
                    if (nFrozenIndex == i) {
                        sFailCause = "PI08: Freeze notary: output refers itself";
                        return false;
                    }
                    
                    int64_t nFrozenValueOut = tx.vout[size_t(nFrozenIndex)].nValue;
                    auto & frozenTxOut = poolFrozen[nFrozenIndex];
                    frozenTxOut.nValue = nFrozenValueOut;
                    frozenTxOut.sAddress = sAddress;
                    frozenTxOut.nFairWithdrawFromEscrowIndex1 = -1;
                    frozenTxOut.nFairWithdrawFromEscrowIndex2 = -1;
                    if (fNotaryF) frozenTxOut.fractions.nFlags |= CFractions::NOTARY_F;
                    if (fNotaryV) frozenTxOut.fractions.nFlags |= CFractions::NOTARY_V;
                    if (fNotaryL) frozenTxOut.fractions.nFlags |= CFractions::NOTARY_L;
                    vFrozenIndexes.push_back(nFrozenIndex);
                    setFrozenIndexes.insert(nFrozenIndex);
                }
                
                if (vOutputArgs.size() > 1) {
                    fFreezeAll = true;
                    fSharedFreeze = true;
                }
                if (vFrozenIndexes.size() == 2) {
                    long nFrozenIndex1 = vFrozenIndexes.front();
                    long nFrozenIndex2 = vFrozenIndexes.back();
                    if (nFrozenIndex1 > nFrozenIndex2) {
                        swap(nFrozenIndex1, nFrozenIndex2);
                    }
                    auto & frozenTxOut = poolFrozen[nFrozenIndex1];
                    frozenTxOut.nFairWithdrawFromEscrowIndex1 = nFrozenIndex1;
                    frozenTxOut.nFairWithdrawFromEscrowIndex2 = nFrozenIndex2;
                }
                
                if (vFrozenIndexes.size() == 1) {
                    long nFrozenIndex = vFrozenIndexes.front();
                
                    int64_t nFrozenValueOut = tx.vout[size_t(nFrozenIndex)].nValue;
                    auto & frozenTxOut = poolFrozen[nFrozenIndex];
                    
                    if (fNotaryF && nReserveIn < nFrozenValueOut) {
                        fFreezeAll = true;
                        fSharedFreeze = true;
                    }
                    else if (fNotaryV && nLiquidityIn < nFrozenValueOut) {
                        fFreezeAll = true;
                        fSharedFreeze = true;
                    }
                    else if (fNotaryL && nLiquidityIn < nFrozenValueOut) {
                        sFailCause = "PI10: Freeze notary: not enough input liquidity";
                        return false;
                    }
    
                    // deductions if not shared freeze
                    if (!fSharedFreeze) {
                        if (fNotaryF) {
                            CFractions frozenOut(0, CFractions::STD);
                            frReserve.MoveRatioPartTo(nFrozenValueOut, frozenOut);
                            frozenTxOut.fractions += frozenOut;
                            frozenTxOut.fractions.nFlags |= CFractions::NOTARY_F;
                            frInp -= frozenOut;
                            nReserveIn -= nFrozenValueOut;
                        }
                        else if (fNotaryV) {
                            CFractions frozenOut(0, CFractions::STD);
                            frLiquidity.MoveRatioPartTo(nFrozenValueOut, frozenOut);
                            frozenTxOut.fractions += frozenOut;
                            frozenTxOut.fractions.nFlags |= CFractions::NOTARY_V;
                            frInp -= frozenOut;
                            nLiquidityIn -= nFrozenValueOut;
                        }
                        else if (fNotaryL) {
                            CFractions frozenOut(0, CFractions::STD);
                            frLiquidity.MoveRatioPartTo(nFrozenValueOut, frozenOut);
                            frozenTxOut.fractions += frozenOut;
                            frozenTxOut.fractions.nFlags |= CFractions::NOTARY_L;
                            frInp -= frozenOut;
                            nLiquidityIn -= nFrozenValueOut;
                        }
                    }
                }
            }
        }

        nReservesTotal += nReserveIn;
        nLiquidityTotal += nLiquidityIn;
    }

    // #NOTE6
    CFractions frCommonLiquidity(0, CFractions::STD);
    for(const auto & item : poolLiquidity) {
        frCommonLiquidity += item.second;
    }

    int64_t nValueOut = 0;
    int64_t nCommonLiquidity = nLiquidityTotal;

    bool fFailedPegOut = false;
    unsigned int nLatestPegOut = 0;

    // Calculation of outputs
    for (unsigned int i = 0; i < n_vout; i++)
    {
        nLatestPegOut = i;
        int64_t nValue = tx.vout[i].nValue;
        nValueOut += nValue;

        auto fkey = uint320(tx.GetHash(), i);
        auto & frOut = mapTestFractionsPool[fkey];

        string sNotary;
        bool fNotary = false;
        auto sAddress = toAddress(tx.vout[i].scriptPubKey, &fNotary, &sNotary);

        // #NOTE7
        if (fFreezeAll && poolFrozen.count(i)) {

            if (poolFrozen[i].fractions.Total() >0) {
                frOut = poolFrozen[i].fractions;
            }
            else {
                if (poolFrozen[i].fractions.nFlags & CFractions::NOTARY_V) {
                    frOut.nFlags |= CFractions::NOTARY_V;
                    frOut.nLockTime = nTime + Params().PegVFrozenTime();
                    frCommonLiquidity.MoveRatioPartTo(nValue, frOut);
                    nCommonLiquidity -= nValue;
                }
                else if (poolFrozen[i].fractions.nFlags & CFractions::NOTARY_F) {
                    frOut.nFlags |= CFractions::NOTARY_F;
                    frOut.nLockTime = nTime + Params().PegFrozenTime();

                    vector<string> vAddresses;
                    auto sFrozenAddress = poolFrozen[i].sAddress;
                    vAddresses.push_back(sFrozenAddress); // make it first
                    for(auto it = poolReserves.begin(); it != poolReserves.end(); it++) {
                        if (it->first == sFrozenAddress) continue;
                        vAddresses.push_back(it->first);
                    }

                    int64_t nValueLeft = nValue;
                    int64_t nValueToTakeReserves = nValueLeft;
                    if (poolFrozen[i].nFairWithdrawFromEscrowIndex1 == i) {
                        if (poolFrozen.size()==2) {
                            long nIndex1 = poolFrozen[i].nFairWithdrawFromEscrowIndex1;
                            long nIndex2 = poolFrozen[i].nFairWithdrawFromEscrowIndex2;
                            if (nIndex1 <0 || nIndex2 <0 || 
                                size_t(nIndex1) >= n_vout || 
                                size_t(nIndex2) >= n_vout) {
                                sFailCause = "P09: Wrong refering output for fair withdraw from escrow";
                                fFailedPegOut = true;
                                break;
                            }
                            // Making an fair withdraw of reserves funds from escrow.
                            // Takes proportionally less from input address to freeze into 
                            // first output - to leave fair amount of reserve for second.
                            int64_t nValue1 = poolFrozen[nIndex1].nValue;
                            int64_t nValue2 = poolFrozen[nIndex2].nValue;
                            if (poolReserves.count(sFrozenAddress) > 0) {
                                auto & frReserve = poolReserves[sFrozenAddress];
                                int64_t nReserve = frReserve.Total();
                                if (nReserve <= (nValue1+nValue2) && (nValue1+nValue2)>0) {
                                    int64_t nScaledValue1 = RatioPart(nReserve, nValue1, nValue1+nValue2);
                                    int64_t nScaledValue2 = RatioPart(nReserve, nValue2, nValue1+nValue2);
                                    int64_t nRemain = nReserve - nScaledValue1 - nScaledValue2;
                                    nValueToTakeReserves = nScaledValue1+nRemain;
                                }
                            }
                        }
                    }
                    
                    for(const string & sAddress : vAddresses) {
                        if (poolReserves.count(sAddress) == 0) continue;
                        auto & frReserve = poolReserves[sAddress];
                        int64_t nReserve = frReserve.Total();
                        if (nReserve ==0) continue;
                        int64_t nValueToTake = nValueToTakeReserves;
                        if (nValueToTake > nReserve)
                            nValueToTake = nReserve;

                        frReserve.MoveRatioPartTo(nValueToTake, frOut);
                        nValueLeft -= nValueToTake;
                        nValueToTakeReserves -= nValueToTake;

                        if (nValueToTakeReserves == 0) {
                            break;
                        }
                    }

                    if (nValueLeft > 0) {
                        if (nValueLeft > nCommonLiquidity) {
                            sFailCause = "P12: No liquidity left";
                            fFailedPegOut = true;
                            break;
                        }
                        frCommonLiquidity.MoveRatioPartTo(nValueLeft, frOut);
                        nCommonLiquidity -= nValueLeft;
                    }
                }
            }
        }
        else {
            if (!poolFrozen.count(i)) { // not frozen
                if (poolReserves.count(sAddress)) { // to reserve
                    int64_t nValueLeft = nValue;
                    int64_t nValueToTake = nValueLeft;

                    auto & frReserve = poolReserves[sAddress];
                    int64_t nReserve = frReserve.Total();
                    if (nReserve >0) {
                        if (nValueToTake > nReserve)
                            nValueToTake = nReserve;

                        frReserve.MoveRatioPartTo(nValueToTake, frOut);
                        nValueLeft -= nValueToTake;
                    }

                    if (nValueLeft > 0) {
                        if (nValueLeft > nCommonLiquidity) {
                            sFailCause = "P13: No liquidity left";
                            fFailedPegOut = true;
                            break;
                        }
                        frCommonLiquidity.MoveRatioPartTo(nValueLeft, frOut);
                        nCommonLiquidity -= nValueLeft;
                    }
                }
                else { // move liquidity out
                    if (sAddress == sBurnAddress || fNotary) {

                        vector<string> vAddresses;
                        for(auto it = poolReserves.begin(); it != poolReserves.end(); it++) {
                            vAddresses.push_back(it->first);
                        }

                        int64_t nValueLeft = nValue;
                        for(const string & sAddress : vAddresses) {
                            auto & frReserve = poolReserves[sAddress];
                            int64_t nReserve = frReserve.Total();
                            if (nReserve ==0) continue;
                            int64_t nValueToTake = nValueLeft;
                            if (nValueToTake > nReserve)
                                nValueToTake = nReserve;

                            frReserve.MoveRatioPartTo(nValueToTake, frOut);
                            nValueLeft -= nValueToTake;

                            if (nValueLeft == 0) {
                                break;
                            }
                        }

                        if (nValueLeft > 0) {
                            if (nValueLeft > nCommonLiquidity) {
                                sFailCause = "P14: No liquidity left";
                                fFailedPegOut = true;
                                break;
                            }
                            frCommonLiquidity.MoveRatioPartTo(nValueLeft, frOut);
                            nCommonLiquidity -= nValueLeft;
                        }
                    }
                    else {
                        int64_t nValueLeft = nValue;
                        if (nValueLeft > nCommonLiquidity) {
                            sFailCause = "P15: No liquidity left";
                            fFailedPegOut = true;
                            break;
                        }
                        frCommonLiquidity.MoveRatioPartTo(nValueLeft, frOut);
                        nCommonLiquidity -= nValueLeft;
                    }
                }
            }
            else { // frozen, but no fFreezeAll
                frOut = poolFrozen[i].fractions;
            }
        }
    }

    if (!fFailedPegOut) {
        // lets do some extra checks for totals
        for (unsigned int i = 0; i < n_vout; i++)
        {
            auto fkey = uint320(tx.GetHash(), i);
            auto f = mapTestFractionsPool[fkey];
            int64_t nValue = tx.vout[i].nValue;
            if (nValue != f.Total() || !f.IsPositive()) {
                sFailCause = "P16: Total mismatch on output "+std::to_string(i);
                fFailedPegOut = true;
                break;
            }
        }
    }

    // when finished all outputs, poolReserves and frCommonLiquidity are fees
    int64_t nFee = nValueIn - nValueOut;
    CFractions txFeeFractions(0, CFractions::STD);
    txFeeFractions += frCommonLiquidity;
    for(const auto & item : poolReserves) {
        txFeeFractions += item.second;
    }
    if (nFee != txFeeFractions.Total() || !txFeeFractions.IsPositive()) {
        sFailCause = "P17: Total mismatch on fee fractions";
        fFailedPegOut = true;
    }
    
    if (fFailedPegOut) {
        // remove failed fractions from pool
        auto fkey = uint320(tx.GetHash(), nLatestPegOut);
        if (mapTestFractionsPool.count(fkey)) {
            auto it = mapTestFractionsPool.find(fkey);
            mapTestFractionsPool.erase(it);
        }
        return false;
    }

    feesFractions += txFeeFractions;
    
    // now all outputs are ready, place them as inputs for next tx in the list
    for (unsigned int i = 0; i < n_vout; i++)
    {
        auto fkey = uint320(tx.GetHash(), i);
        mapInputsFractions[fkey] = mapTestFractionsPool[fkey];
    }

    return true;
}

extern bool CalculateStakingFractions_testnet200k(const CTransaction & tx,
                                                  const CBlockIndex* pindexBlock,
                                                  MapPrevTx & inputs,
                                                  MapFractions& fInputs,
                                                  std::map<uint256, CTxIndex>& mapTestPool,
                                                  MapFractions& mapTestFractionsPool,
                                                  const CFractions& feesFractions,
                                                  int64_t nCalculatedStakeRewardWithoutFees,
                                                  std::string& sFailCause);

bool CalculateStakingFractions_v2(const CTransaction & tx,
                                  const CBlockIndex* pindexBlock,
                                  MapPrevTx & inputs,
                                  MapFractions& fInputs,
                                  std::map<uint256, CTxIndex>& mapTestPool,
                                  MapFractions& mapTestFractionsPool,
                                  const CFractions& feesFractions,
                                  int64_t nCalculatedStakeRewardWithoutFees,
                                  std::string& sFailCause);

bool CalculateStakingFractions(const CTransaction & tx,
                               const CBlockIndex* pindexBlock,
                               MapPrevTx & inputs,
                               MapFractions& fInputs,
                               std::map<uint256, CTxIndex>& mapTestPool,
                               MapFractions& mapTestFractionsPool,
                               const CFractions& feesFractions,
                               int64_t nCalculatedStakeRewardWithoutFees,
                               std::string& sFailCause)
{
    if (!pindexBlock) {
        // need a block info
        return false;
    }
    if (TestNet()) {
        if (pindexBlock->nHeight < 200000) {
            return CalculateStakingFractions_testnet200k(
                        tx,
                        pindexBlock,
                        inputs,
                        fInputs,
                        mapTestPool,
                        mapTestFractionsPool,
                        feesFractions,
                        nCalculatedStakeRewardWithoutFees,
                        sFailCause);
        }
    }
    
    return CalculateStakingFractions_v2(
                tx,
                pindexBlock,
                inputs,
                fInputs,
                mapTestPool,
                mapTestFractionsPool,
                feesFractions,
                nCalculatedStakeRewardWithoutFees,
                sFailCause);
}

bool CalculateStakingFractions_v2(const CTransaction & tx,
                                  const CBlockIndex* pindexBlock,
                                  MapPrevTx & inputs,
                                  MapFractions& fInputs,
                                  std::map<uint256, CTxIndex>& mapTestPool,
                                  MapFractions& mapTestFractionsPool,
                                  const CFractions& feesFractions,
                                  int64_t nCalculatedStakeRewardWithoutFees,
                                  std::string& sFailCause)
{
    size_t n_vin = tx.vin.size();
    size_t n_vout = tx.vout.size();

    if (!IsPegWhiteListed(tx, inputs)) {
        sFailCause = "Not whitelisted";
        return true;
    }

    if (n_vin != 1) {
        sFailCause = "More than one input";
        return false;
    }
    
    if (n_vout > 8) {
        sFailCause = "More than 8 outputs";
        return false;
    }
    
    int64_t nValueStakeIn = 0;
    CFractions frStake(0, CFractions::STD);

    string sInputAddress;
    
    // only one input
    {
        unsigned int i = 0;
        const COutPoint & prevout = tx.vin[i].prevout;
        CTransaction& txPrev = inputs[prevout.hash].second;
        if (prevout.n >= txPrev.vout.size()) {
            sFailCause = "PI02: Refered output out of range";
            return false;
        }

        int64_t nValue = txPrev.vout[prevout.n].nValue;
        nValueStakeIn = nValue;
        
        auto sAddress = toAddress(txPrev.vout[prevout.n].scriptPubKey);
        sInputAddress = sAddress;

        auto fkey = uint320(prevout.hash, prevout.n);
        if (fInputs.find(fkey) == fInputs.end()) {
            sFailCause = "PI03: No input fractions found";
            return false;
        }

        frStake = fInputs[fkey].Std();
        if (frStake.Total() != nValueStakeIn) {
            sFailCause = "PI04: Input fraction total mismatches value";
            return false;
        }
    }

    // Check funds to be returned to same address
    int64_t nValueReturn = 0;
    for (unsigned int i = 0; i < n_vout; i++) {
        std::string sAddress = toAddress(tx.vout[i].scriptPubKey);
        if (sInputAddress == sAddress) {
            nValueReturn += tx.vout[i].nValue;
        }
    }
    if (nValueReturn < nValueStakeIn) {
        sFailCause = "PI05: No enough funds returned to input address";
        return false;
    }
    
    CFractions frReward(nCalculatedStakeRewardWithoutFees, CFractions::STD);
    frReward += feesFractions;
    
    int64_t nValueRewardLeft = frReward.Total();
    
    bool fFailedPegOut = false;
    unsigned int nStakeOut = 0;

    // Transfer mark and set stake output 
    for (unsigned int i = 0; i < n_vout; i++)
    {
        int64_t nValue = tx.vout[i].nValue;
        
        auto fkey = uint320(tx.GetHash(), i);
        auto & frOut = mapTestFractionsPool[fkey];
        
        string sNotary;
        bool fNotary = false;
        auto sAddress = toAddress(tx.vout[i].scriptPubKey, &fNotary, &sNotary);
        
        // first output returning on same address and greater or equal stake value
        if (nValue >= nValueStakeIn && sInputAddress == sAddress) {
            
            if (nValue > (nValueStakeIn + nValueRewardLeft)) {
                sFailCause = "PO01: No enough coins for stake output";
                fFailedPegOut = true;
            }
            
            int64_t nValueToTake = nValue;
            int64_t nStakeToTake = nValue;
            
            if (nStakeToTake > nValueStakeIn) {
                nStakeToTake = nValueStakeIn;
            }
            
            // first take whole stake input
            nValueToTake -= nStakeToTake;
            frOut = frStake;
            
            // leftover value take from reward
            if (nValueToTake >0) {
                nValueRewardLeft -= nValueToTake;
                frReward.MoveRatioPartTo(nValueToTake, frOut);
            }
            
            // transfer marks and locktime
            if (frStake.nFlags & CFractions::NOTARY_F) {
                frOut.nFlags |= CFractions::NOTARY_F;
                frOut.nLockTime = frStake.nLockTime;
            }
            else if (frStake.nFlags & CFractions::NOTARY_V) {
                frOut.nFlags |= CFractions::NOTARY_V;
                frOut.nLockTime = frStake.nLockTime;
            }
            
            nStakeOut = i;
            break;
        }
    }
    
    if (nStakeOut == 0 && !fFailedPegOut) {
        sFailCause = "PO02: No stake funds returned to input address";
        fFailedPegOut = true;
    }
    
    // Calculation of outputs
    for (unsigned int i = 0; i < n_vout; i++)
    {
        int64_t nValue = tx.vout[i].nValue;
        
        if (i == nStakeOut) {
            // already calculated and marked
            continue;
        }

        auto fkey = uint320(tx.GetHash(), i);
        auto & frOut = mapTestFractionsPool[fkey];

        if (nValue > nValueRewardLeft) {
            sFailCause = "PO03: No coins left";
            fFailedPegOut = true;
            break;
        }
        
        frReward.MoveRatioPartTo(nValue, frOut);
        nValueRewardLeft -= nValue;
    }
    
    if (!fFailedPegOut) {
        // lets do some extra checks for totals
        for (unsigned int i = 0; i < n_vout; i++)
        {
            auto fkey = uint320(tx.GetHash(), i);
            auto f = mapTestFractionsPool[fkey];
            int64_t nValue = tx.vout[i].nValue;
            if (nValue != f.Total() || !f.IsPositive()) {
                sFailCause = "PO04: Total mismatch on output "+std::to_string(i);
                fFailedPegOut = true;
                break;
            }
        }
    }

    if (fFailedPegOut) {
        // while the peg system is in the testing mode:
        // for now remove failed fractions from pool so they
        // are not written to db
        for (unsigned int i = 0; i < n_vout; i++) {
            auto fkey = uint320(tx.GetHash(), i);
            if (mapTestFractionsPool.count(fkey)) {
                auto it = mapTestFractionsPool.find(fkey);
                mapTestFractionsPool.erase(it);
            }
        }
        return false;
    }

    return true;
}

void PrunePegForBlock(const CBlock& blockprune, CPegDB& pegdb)
{
    return;
    for(size_t i=0; i<blockprune.vtx.size(); i++) {
        const CTransaction& tx = blockprune.vtx[i];
        for (size_t j=0; j< tx.vin.size(); j++) {
            COutPoint prevout = tx.vin[j].prevout;
            auto fkey = uint320(prevout.hash, prevout.n);
            pegdb.Erase(fkey);
        }
        if (!tx.IsCoinStake()) 
            continue;
        
        for (size_t j=0; j< tx.vout.size(); j++) {
            CTxOut out = tx.vout[j];
            const CScript& scriptPubKey = out.scriptPubKey;
    
            txnouttype type;
            vector<CTxDestination> addresses;
            int nRequired;
    
            if (!ExtractDestinations(scriptPubKey, type, addresses, nRequired))
                continue;
    
            bool voted = false;
            for(const CTxDestination& addr : addresses) {
                std::string str_addr = CBitcoinAddress(addr).ToString();
                if (str_addr == Params().PegInflateAddr()) { voted = true; }
                else if (str_addr == Params().PegDeflateAddr()) { voted = true; }
                else if (str_addr == Params().PegNochangeAddr()) { voted = true; }
            }
            if (!voted) 
                continue;
            
            auto fkey = uint320(tx.GetHash(), j);
            pegdb.Erase(fkey);
        }
    }
}

