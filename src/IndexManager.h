/* Copyright (c) 2014 Stanford University
 *
 * Permission to use, copy, modify, and distribute this software for any
 * purpose with or without fee is hereby granted, provided that the above
 * copyright notice and this permission notice appear in all copies.
 *
 * THE SOFTWARE IS PROVIDED "AS IS" AND THE AUTHOR(S) DISCLAIM ALL WARRANTIES
 * WITH REGARD TO THIS SOFTWARE INCLUDING ALL IMPLIED WARRANTIES OF
 * MERCHANTABILITY AND FITNESS. IN NO EVENT SHALL AUTHORS BE LIABLE FOR
 * ANY SPECIAL, DIRECT, INDIRECT, OR CONSEQUENTIAL DAMAGES OR ANY DAMAGES
 * WHATSOEVER RESULTING FROM LOSS OF USE, DATA OR PROFITS, WHETHER IN AN
 * ACTION OF CONTRACT, NEGLIGENCE OR OTHER TORTIOUS ACTION, ARISING OUT OF
 * OR IN CONNECTION WITH THE USE OR PERFORMANCE OF THIS SOFTWARE.
 */

#ifndef RAMCLOUD_INDEXMANAGER_H
#define RAMCLOUD_INDEXMANAGER_H

#include "Buffer.h"
#include "Common.h"
#include "Key.h"
#include "Status.h"

namespace RAMCloud {

/**
 * The IndexManager class is responsible for storing index entries in an
 * index server. This partition is called an indexlet.
 * This class interfaces with ObjectManager and index tree code to manage
 * index entries.
 * 
 * Note: Every master server is also an index server for some partition
 * of some index (independent from data partition located on that master).
 */

class IndexManager {

  public:
    explicit IndexManager(Context* context);
    virtual ~IndexManager();

    Status createIndexlet(uint64_t indexletId,
                          Key& firstKey, Key& lastKey);
    Status dropIndexlet(uint64_t indexletId);
    Status indexedRead(uint64_t tableId, uint64_t pKHash,
                       uint8_t indexletId, Key& firstKey, Key& lastKey,
                       Buffer* outBuffer, uint64_t* outVersion);
    Status insertEntry(uint64_t indexletId,
                       Key indexKey, uint64_t pKHash);
    Status lookupIndexKeys(uint64_t indexletId,
                           Key& firstKey, Key& lastKey,
                           uint32_t* count, Buffer* outBuffer);
    Status removeEntry(uint64_t indexletId, Key indexKey);


  private:
    /// Shared RAMCloud information.
    Context* context;

    DISALLOW_COPY_AND_ASSIGN(IndexManager);
};

} // namespace RAMCloud

#endif
