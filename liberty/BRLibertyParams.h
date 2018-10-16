//
//  BRBCashParams.h
//
//  Created by Aaron Voisine on 1/10/18.
//  Copyright (c) 2019 breadwallet LLC
//
//  Permission is hereby granted, free of charge, to any person obtaining a copy
//  of this software and associated documentation files (the "Software"), to deal
//  in the Software without restriction, including without limitation the rights
//  to use, copy, modify, merge, publish, distribute, sublicense, and/or sell
//  copies of the Software, and to permit persons to whom the Software is
//  furnished to do so, subject to the following conditions:
//
//  The above copyright notice and this permission notice shall be included in
//  all copies or substantial portions of the Software.
//
//  THE SOFTWARE IS PROVIDED "AS IS", WITHOUT WARRANTY OF ANY KIND, EXPRESS OR
//  IMPLIED, INCLUDING BUT NOT LIMITED TO THE WARRANTIES OF MERCHANTABILITY,
//  FITNESS FOR A PARTICULAR PURPOSE AND NONINFRINGEMENT. IN NO EVENT SHALL THE
//  AUTHORS OR COPYRIGHT HOLDERS BE LIABLE FOR ANY CLAIM, DAMAGES OR OTHER
//  LIABILITY, WHETHER IN AN ACTION OF CONTRACT, TORT OR OTHERWISE, ARISING FROM,
//  OUT OF OR IN CONNECTION WITH THE SOFTWARE OR THE USE OR OTHER DEALINGS IN
//  THE SOFTWARE.

#ifndef BRBCashParams_h
#define BRBCashParams_h

#include "BRChainParams.h"
#include "BRPeer.h"
#include "BRInt.h"

static const char *BRLibertyDNSSeeds[] = {
    "liberty.mn.zone.", "node-01.nethash.io.", "node-02.nethash.io.", "node-03.nethash.io.", "node-04.nethash.io.", "node-05.nethash.io.",
    "node-06.nethash.io.", "node-07.nethash.io.", "node-08.nethash.io.", "node-09.nethash.io.", "node-10.nethash.io.", NULL
};

static const char *BRLibertyTestNetDNSSeeds[] = {};

static const BRCheckPoint BRLibertyTestNetCheckpoints[] = {};

// blockchain checkpoints - these are also used as starting points for partial chain downloads, so they must be at
// difficulty transition boundaries in order to verify the block difficulty at the immediately following transition
static const BRCheckPoint BRLibertyCheckpoints[] = {};

static const BRMerkleBlock *_medianBlock(const BRMerkleBlock *b, const BRSet *blockSet)
{
    const BRMerkleBlock *b0 = NULL, *b1 = NULL, *b2 = b;

    b1 = (b2) ? BRSetGet(blockSet, &b2->prevBlock) : NULL;
    b0 = (b1) ? BRSetGet(blockSet, &b1->prevBlock) : NULL;
    if (b0 && b2 && b0->timestamp > b2->timestamp) b = b0, b0 = b2, b2 = b;
    if (b0 && b1 && b0->timestamp > b1->timestamp) b = b0, b0 = b1, b1 = b;
    if (b1 && b2 && b1->timestamp > b2->timestamp) b = b1, b1 = b2, b2 = b;
    return (b0 && b1 && b2) ? b1 : NULL;
}

static int BRLibertyVerifyDifficulty(const BRMerkleBlock *block, const BRSet *blockSet)
{  
    return 1;
}

static const BRChainParams BRLibertyParams = {
    BRLibertyDNSSeeds,
    10417,                // standardPort
    0x8fafaeac,          // magicNumber
    0x0, // services
    BRLibertyVerifyDifficulty,
    BRLibertyCheckpoints,
    sizeof(BRLibertyCheckpoints)/sizeof(*BRLibertyCheckpoints),
};

static const BRChainParams BRLibertyestNetParams = {
    BRLibertyTestNetDNSSeeds,
    20417,               // standardPort
    0xacaeaf8f,          // magicNumber
    0x0, // services
    BRLibertyVerifyDifficulty,
    BRLibertyTestNetCheckpoints,
    sizeof(BRLibertyTestNetCheckpoints)/sizeof(*BRLibertyTestNetCheckpoints)
};

#endif // BRChainParams_h
