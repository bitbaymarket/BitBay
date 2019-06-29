// Copyright (c) 2018 yshurik
// Distributed under the MIT/X11 software license, see the accompanying
// file COPYING or http://www.opensource.org/licenses/mit-license.php.

#include "pegdata.h"

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
