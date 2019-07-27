// Copyright (c) 2018 yshurik
// Distributed under the MIT/X11 software license, see the accompanying
// file COPYING or http://www.opensource.org/licenses/mit-license.php.

#ifndef BITBAY_PEGPACK_H
#define BITBAY_PEGPACK_H

#include <string>

class CPegLevel;
class CFractions;

namespace pegops {

extern std::string packpegdata(const CFractions &     fractions,
                               const CPegLevel &      peglevel,
                               int64_t                nReserve,
                               int64_t                nLiquid);

extern bool unpackbalance(
        const std::string & inp_pegdata64,
        std::string         inp_tag,
        
        CFractions &    out_fractions,
        CPegLevel &     out_peglevel,
        int64_t &       out_reserve,
        int64_t &       out_liquid,
        std::string &   out_err);

// json

extern bool unpackbalance(

        const std::string &   pegdata64,
        std::string           tag,
        
        CFractions &          fractions,
        int64_t &             nReserve,
        int64_t &             nLiquid,
        CPegLevel &           peglevel
        );

}

#endif // BITBAY_PEGPACK_H
