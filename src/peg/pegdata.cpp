// Copyright (c) 2018 yshurik
// Distributed under the MIT/X11 software license, see the accompanying
// file COPYING or http://www.opensource.org/licenses/mit-license.php.

#include "pegdata.h"
#include "pegutil.h"

#include <map>
#include <set>
#include <cstdint>
#include <utility>
#include <algorithm> 
#include <type_traits>

#include <boost/multiprecision/cpp_int.hpp>

#include <zconf.h>
#include <zlib.h>

using namespace std;
using namespace boost;
using namespace pegutil;

int64_t RatioPart(int64_t nValue,
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

CPegData::CPegData(std::string pegdata64)
{
    if (pegdata64.empty()) {
        peglevel = CPegLevel(0,0,0,0,0);
        return; // defaults, zeros
    }
    
    string pegdata = DecodeBase64(pegdata64);
    CDataStream finp(pegdata.data(), 
                     pegdata.data() + pegdata.size(),
                     SER_DISK, CLIENT_VERSION);
    
    bool ok = Unpack(finp);
    if (!ok) { 
        // try prev version
        CDataStream finp2(pegdata.data(), 
                          pegdata.data() + pegdata.size(),
                          SER_DISK, CLIENT_VERSION);
        ok = Unpack1(finp2);
        
        if (!ok) { 
            // peglevel to inicate invalid
            peglevel = CPegLevel();
        }
    }
}

bool CPegData::IsValid() const
{
    return peglevel.IsValid();
}

bool CPegData::Pack(CDataStream & fout) const {
    //fout << nVersion;
    fractions.Pack(fout);
    peglevel.Pack(fout);
    fout << nReserve;
    fout << nLiquid;
    return true;
}

bool CPegData::Unpack(CDataStream & finp) {
    try {
        //finp >> nVersion;
        if (!fractions.Unpack(finp)) return false;
        if (!peglevel.Unpack(finp)) return false;
        finp >> nReserve;
        finp >> nLiquid;
        
        // match total
        if ((nReserve+nLiquid) != fractions.Total()) return false;
        
        // validate liquid/reserve match peglevel
        int nSupplyEffective = peglevel.nSupply+peglevel.nShift;
        bool fPartial = peglevel.nShiftLastPart >0 && peglevel.nShiftLastTotal >0;
        if (fPartial) {
            nSupplyEffective++;
            int64_t nLiquidWithoutPartial = fractions.High(nSupplyEffective);
            int64_t nReserveWithoutPartial = fractions.Low(nSupplyEffective-1);
            if (nLiquid < nLiquidWithoutPartial) return false;
            if (nReserve < nReserveWithoutPartial) return false;
        }
        else {
            int64_t nLiquidCalc = fractions.High(nSupplyEffective);
            int64_t nReserveCalc = fractions.Low(nSupplyEffective);
            if (nLiquid != nLiquidCalc) return false;
            if (nReserve != nReserveCalc) return false;
        }
    }
    catch (std::exception &) {
        return false;
    }
    
    fractions = fractions.Std();
    return true;
}

std::string CPegData::ToString() const {
    CDataStream fout(SER_DISK, CLIENT_VERSION);
    Pack(fout);
    return EncodeBase64(fout.str());
}

