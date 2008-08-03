/*
 * Copyright (c) 2004-2005 The Regents of The University of Michigan
 * All rights reserved.
 *
 * Redistribution and use in source and binary forms, with or without
 * modification, are permitted provided that the following conditions are
 * met: redistributions of source code must retain the above copyright
 * notice, this list of conditions and the following disclaimer;
 * redistributions in binary form must reproduce the above copyright
 * notice, this list of conditions and the following disclaimer in the
 * documentation and/or other materials provided with the distribution;
 * neither the name of the copyright holders nor the names of its
 * contributors may be used to endorse or promote products derived from
 * this software without specific prior written permission.
 *
 * THIS SOFTWARE IS PROVIDED BY THE COPYRIGHT HOLDERS AND CONTRIBUTORS
 * "AS IS" AND ANY EXPRESS OR IMPLIED WARRANTIES, INCLUDING, BUT NOT
 * LIMITED TO, THE IMPLIED WARRANTIES OF MERCHANTABILITY AND FITNESS FOR
 * A PARTICULAR PURPOSE ARE DISCLAIMED. IN NO EVENT SHALL THE COPYRIGHT
 * OWNER OR CONTRIBUTORS BE LIABLE FOR ANY DIRECT, INDIRECT, INCIDENTAL,
 * SPECIAL, EXEMPLARY, OR CONSEQUENTIAL DAMAGES (INCLUDING, BUT NOT
 * LIMITED TO, PROCUREMENT OF SUBSTITUTE GOODS OR SERVICES; LOSS OF USE,
 * DATA, OR PROFITS; OR BUSINESS INTERRUPTION) HOWEVER CAUSED AND ON ANY
 * THEORY OF LIABILITY, WHETHER IN CONTRACT, STRICT LIABILITY, OR TORT
 * (INCLUDING NEGLIGENCE OR OTHERWISE) ARISING IN ANY WAY OUT OF THE USE
 * OF THIS SOFTWARE, EVEN IF ADVISED OF THE POSSIBILITY OF SUCH DAMAGE.
 *
 * Authors: Lisa Hsu
 */

/**
 * @file
 * Declaration of a LIFO tag store usable in a partitioned cache.
 */

#ifndef __SPLIT_LIFO_HH__
#define __SPLIT_LIFO_HH__

#include <cstring>
#include <list>

#include "mem/cache/blk.hh" // base class
#include "mem/cache/tags/split_blk.hh"
#include "mem/packet.hh" // for inlined functions
#include "base/hashmap.hh"
#include <assert.h>
#include "mem/cache/tags/base.hh"

class BaseCache;

/**
 * A LIFO set of cache blks
 */
class LIFOSet {
  public:
    /** the number of blocks in this set */
    int ways;

    /** Cache blocks in this set, maintained in LIFO order where
        0 = Last in (head) */
    SplitBlk *lastIn;
    SplitBlk *firstIn;

    /** has the initial "filling" of this set finished? i.e., have you had
     * 'ways' number of compulsory misses in this set yet? if withValue == ways,
     * then yes.  withValue is meant to be the number of blocks in the set that have
     * gone through their first compulsory miss.
     */
    int withValue;

    /**
     * Find a block matching the tag in this set.
     * @param asid The address space ID.
     * @param tag the Tag you are looking for
     * @return Pointer to the block, if found, NULL otherwise
     */
    SplitBlk* findBlk(Addr tag) const;

    void moveToLastIn(SplitBlk *blk);
    void moveToFirstIn(SplitBlk *blk);

    LIFOSet()
        : ways(-1), lastIn(NULL), firstIn(NULL), withValue(0)
    {}
};

/**
 * A LIFO cache tag store.
 */
class SplitLIFO : public BaseTags
{
  public:
    /** Typedef the block type used in this tag store. */
    typedef SplitBlk BlkType;
    /** Typedef for a list of pointers to the local block class. */
    typedef std::list<SplitBlk*> BlkList;
  protected:
    /** The number of bytes in a block. */
    const int blkSize;
    /** the size of the cache in bytes */
    const int size;
    /** the number of blocks in the cache */
    const int numBlks;
    /** the number of sets in the cache */
    const int numSets;
    /** the number of ways in the cache */
    const int ways;
    /** The hit latency. */
    const int hitLatency;
    /** whether this is a "2 queue" replacement @sa moveToLastIn @sa moveToFirstIn */
    const bool twoQueue;
    /** indicator for which partition this is */
    const int part;

    /** The cache blocks. */
    SplitBlk *blks;
    /** The Cache sets */
    LIFOSet *sets;
    /** The data blocks, 1 per cache block. */
    uint8_t *dataBlks;

    /** The amount to shift the address to get the set. */
    int setShift;
    /** The amount to shift the address to get the tag. */
    int tagShift;
    /** Mask out all bits that aren't part of the set index. */
    unsigned setMask;
    /** Mask out all bits that aren't part of the block offset. */
    unsigned blkMask;


    /** the number of hit in this partition */
    Stats::Scalar<> hits;
    /** the number of blocks brought into this partition (i.e. misses) */
    Stats::Scalar<> misses;
    /** the number of invalidations in this partition */
    Stats::Scalar<> invalidations;

public:
    /**
     * Construct and initialize this tag store.
     * @param _numSets The number of sets in the cache.
     * @param _blkSize The number of bytes in a block.
     * @param _assoc The associativity of the cache.
     * @param _hit_latency The latency in cycles for a hit.
     */
    SplitLIFO(int _blkSize, int _size, int _ways, int _hit_latency, bool twoQueue, int _part);

    /**
     * Destructor
     */
    virtual ~SplitLIFO();

    /**
     * Register the statistics for this object
     * @param name The name to precede the stat
     */
    void regStats(const std::string &name);

    /**
     * Return the block size.
     * @return the block size.
     */
    int getBlockSize()
    {
        return blkSize;
    }

    /**
     * Return the subblock size. In the case of LIFO it is always the block
     * size.
     * @return The block size.
     */
    int getSubBlockSize()
    {
        return blkSize;
    }

    /**
     * Search for the address in the cache.
     * @param asid The address space ID.
     * @param addr The address to find.
     * @return True if the address is in the cache.
     */
    bool probe( Addr addr) const;

    /**
     * Invalidate the given block.
     * @param blk The block to invalidate.
     */
    void invalidateBlk(BlkType *blk);

    /**
     * Finds the given address in the cache and update replacement data.
     * Returns the access latency as a side effect.
     * @param addr The address to find.
     * @param asid The address space ID.
     * @param lat The access latency.
     * @return Pointer to the cache block if found.
     */
    SplitBlk* findBlock(Addr addr, int &lat);

    /**
     * Finds the given address in the cache, do not update replacement data.
     * @param addr The address to find.
     * @param asid The address space ID.
     * @return Pointer to the cache block if found.
     */
    SplitBlk* findBlock(Addr addr) const;

    /**
     * Find a replacement block for the address provided.
     * @param pkt The request to a find a replacement candidate for.
     * @param writebacks List for any writebacks to be performed.
     * @return The block to place the replacement in.
     */
    SplitBlk* findReplacement(Addr addr, PacketList &writebacks);

    /**
     * Generate the tag from the given address.
     * @param addr The address to get the tag from.
     * @return The tag of the address.
     */
    Addr extractTag(Addr addr) const
    {
        return (addr >> tagShift);
    }

   /**
     * Calculate the set index from the address.
     * @param addr The address to get the set from.
     * @return The set index of the address.
     */
    int extractSet(Addr addr) const
    {
        return ((addr >> setShift) & setMask);
    }

    /**
     * Get the block offset from an address.
     * @param addr The address to get the offset of.
     * @return The block offset.
     */
    int extractBlkOffset(Addr addr) const
    {
        return (addr & blkMask);
    }

    /**
     * Align an address to the block size.
     * @param addr the address to align.
     * @return The block address.
     */
    Addr blkAlign(Addr addr) const
    {
        return (addr & ~(Addr)blkMask);
    }

    /**
     * Regenerate the block address from the tag.
     * @param tag The tag of the block.
     * @param set The set of the block.
     * @return The block address.
     */
    Addr regenerateBlkAddr(Addr tag, unsigned set) const
    {
        return ((tag << tagShift) | ((Addr)set << setShift));
    }

    /**
     * Return the hit latency.
     * @return the hit latency.
     */
    int getHitLatency() const
    {
        return hitLatency;
    }

    /**
     * Read the data out of the internal storage of the given cache block.
     * @param blk The cache block to read.
     * @param data The buffer to read the data into.
     * @return The cache block's data.
     */
    void readData(SplitBlk *blk, uint8_t *data)
    {
        std::memcpy(data, blk->data, blk->size);
    }

    /**
     * Write data into the internal storage of the given cache block. Since in
     * LIFO does not store data differently this just needs to update the size.
     * @param blk The cache block to write.
     * @param data The data to write.
     * @param size The number of bytes to write.
     * @param writebacks A list for any writebacks to be performed. May be
     * needed when writing to a compressed block.
     */
    void writeData(SplitBlk *blk, uint8_t *data, int size,
                   PacketList & writebacks)
    {
        assert(size <= blkSize);
        blk->size = size;
    }

    /**
     * Called at end of simulation to complete average block reference stats.
     */
    virtual void cleanupRefs();
};

#endif