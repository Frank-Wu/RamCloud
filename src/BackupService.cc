/* Copyright (c) 2009-2012 Stanford University
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

#include <thread>

#include "BackupService.h"
#include "BackupStorage.h"
#include "Buffer.h"
#include "ClientException.h"
#include "Cycles.h"
#include "Log.h"
#include "LogEntryTypes.h"
#include "MasterClient.h"
#include "Object.h"
#include "ServerConfig.h"
#include "SegmentIterator.h"
#include "ShortMacros.h"
#include "Status.h"
#include "TransportManager.h"

namespace RAMCloud {

namespace {
/**
 * The TSC reading at the start of the last recovery.
 * Used to update #RawMetrics.backup.readingDataTicks.
 */
uint64_t recoveryStart;
} // anonymous namespace

// --- BackupService::SegmentInfo ---

/**
 * Construct a SegmentInfo to manage a segment.
 *
 * \param storage
 *      The storage which this segment will be backed by when closed.
 * \param pool
 *      The pool from which this segment should draw and return memory
 *      when it is moved in and out of memory.
 * \param ioScheduler
 *      The IoScheduler through which IO requests are made.
 * \param masterId
 *      The master id of the segment being managed.
 * \param segmentId
 *      The segment id of the segment being managed.
 * \param segmentSize
 *      The number of bytes contained in this segment.
 * \param primary
 *      True if this is the primary copy of this segment for the master
 *      who stored it.  Determines whether recovery segments are built
 *      at recovery start or on demand.
 */
BackupService::SegmentInfo::SegmentInfo(BackupStorage& storage,
                                        ThreadSafePool& pool,
                                        IoScheduler& ioScheduler,
                                        ServerId masterId,
                                        uint64_t segmentId,
                                        uint32_t segmentSize,
                                        bool primary)
    : masterId(masterId)
    , primary(primary)
    , segmentId(segmentId)
    , createdByCurrentProcess(true)
    , mutex()
    , condition()
    , ioScheduler(ioScheduler)
    , recoveryException()
    , recoveryPartitions()
    , recoverySegments(NULL)
    , recoverySegmentsLength()
    , footerOffset(0)
    , footerEntry()
    , rightmostWrittenOffset(0)
    , segment()
    , segmentSize(segmentSize)
    , state(UNINIT)
    , replicateAtomically(false)
    , storageHandle()
    , pool(pool)
    , storage(storage)
    , storageOpCount(0)
{
}

/**
 * Construct a SegmentInfo to manage a replica which already has an
 * existing representation on storage.
 *
 * \param storage
 *      The storage which this segment will be backed by when closed.
 * \param pool
 *      The pool from which this segment should draw and return memory
 *      when it is moved in and out of memory.
 * \param ioScheduler
 *      The IoScheduler through which IO requests are made.
 * \param masterId
 *      The master id of the segment being managed.
 * \param segmentId
 *      The segment id of the segment being managed.
 * \param segmentSize
 *      The number of bytes contained in this segment.
 * \param segmentFrame
 *      The segment frame number this replica's representation is in.  This
 *      reassociates it with that representation so operations to this
 *      instance affect that segment frame.
 * \param isClosed
 *      Whether the on-storage replica indicates a closed or open status.
 *      This instance will reflect that status.  If this is true the
 *      segment is retreived from storage to simplify some legacy code
 *      that assumes open replicas reside in memory.
 */
BackupService::SegmentInfo::SegmentInfo(BackupStorage& storage,
                                        ThreadSafePool& pool,
                                        IoScheduler& ioScheduler,
                                        ServerId masterId,
                                        uint64_t segmentId,
                                        uint32_t segmentSize,
                                        uint32_t segmentFrame,
                                        bool isClosed)
    : masterId(masterId)
    , primary(false)
    , segmentId(segmentId)
    , createdByCurrentProcess(false)
    , mutex()
    , condition()
    , ioScheduler(ioScheduler)
    , recoveryException()
    , recoveryPartitions()
    , recoverySegments(NULL)
    , recoverySegmentsLength()
    , footerOffset(0)
    , footerEntry()
    , rightmostWrittenOffset(0)
    , segment()
    , segmentSize(segmentSize)
    , state(isClosed ? CLOSED : OPEN)
    , replicateAtomically(false)
    , storageHandle()
    , pool(pool)
    , storage(storage)
    , storageOpCount(0)
{
    storageHandle = storage.associate(segmentFrame);
    if (state == OPEN) {
        // There is some disagreement between the masters and backups about
        // the meaning of "open".  In a few places the backup code assumes
        // open replicas must be in memory.  For now its easiest just to
        // make sure that is the case, so on cold start we have to reload
        // open replicas into ram.
        try {
            // Get memory for staging the segment writes
            segment = static_cast<char*>(pool.malloc());
            storage.getSegment(storageHandle, segment);
        } catch (...) {
            pool.free(segment);
            delete storageHandle;
            throw;
        }
    }

    // TODO(stutsman): Claim this replica is the longest if it was closed
    // and the shortest if it was open.  This field should go away soon, but
    // this should make it so that replicas from backups that haven't crashed
    // are preferred over this one.
    if (state == CLOSED)
        rightmostWrittenOffset = BYTES_WRITTEN_CLOSED;
}

/**
 * Store any open segments to storage and then release all resources
 * associate with them except permanent storage.
 */
BackupService::SegmentInfo::~SegmentInfo()
{
    Lock lock(mutex);
    waitForOngoingOps(lock);

    if (state == OPEN) {
        if (replicateAtomically && state != CLOSED) {
            LOG(NOTICE, "Backup shutting down with open segment <%lu,%lu>, "
                "which was open for atomic replication; discarding since the "
                "replica was incomplete", *masterId, segmentId);
        } else {
            LOG(NOTICE, "Backup shutting down with open segment <%lu,%lu>, "
                "closing out to storage", *masterId, segmentId);
            state = CLOSED;
            CycleCounter<RawMetric> _(&metrics->backup.storageWriteTicks);
            ++metrics->backup.storageWriteCount;
            metrics->backup.storageWriteBytes += segmentSize;
            storage.putSegment(storageHandle, segment);
        }
    }
    if (isRecovered()) {
        delete[] recoverySegments;
        recoverySegments = NULL;
        recoverySegmentsLength = 0;
    }
    if (inStorage()) {
        delete storageHandle;
        storageHandle = NULL;
    }
    if (inMemory()) {
        pool.free(segment);
        segment = NULL;
    }
    // recoveryException cleaned up by unique_ptr
}

/**
 * Append a recovery segment to a Buffer.  This segment must have its
 * recovery segments constructed first; see buildRecoverySegments().
 *
 * \param partitionId
 *      The partition id corresponding to the tablet ranges of the recovery
 *      segment to append to #buffer.
 * \param[out] buffer
 *      A buffer which onto which the requested recovery segment will be
 *      appended.
 * \return
 *      Status code: STATUS_OK if the recovery segment was appended,
 *      STATUS_RETRY if the caller should try again later.
 * \throw BadSegmentIdException
 *      If the segment to which this recovery segment belongs is not yet
 *      recovered or there is no such recovery segment for that
 *      #partitionId.
 * \throw SegmentRecoveryFailedException
 *      If the segment to which this recovery segment belongs failed to
 *      recover.
 */
Status
BackupService::SegmentInfo::appendRecoverySegment(uint64_t partitionId,
                                                  Buffer& buffer)
{
    Lock lock(mutex, std::try_to_lock_t());
    if (!lock.owns_lock()) {
        LOG(DEBUG, "Deferring because couldn't acquire lock immediately");
        return STATUS_RETRY;
    }

    if (state != RECOVERING) {
        LOG(WARNING, "Asked for segment <%lu,%lu> which isn't recovering",
            *masterId, segmentId);
        throw BackupBadSegmentIdException(HERE);
    }

    if (!primary) {
        if (!isRecovered() && !recoveryException) {
            LOG(DEBUG, "Requested segment <%lu,%lu> is secondary, "
                "starting build of recovery segments now",
                *masterId, segmentId);
            waitForOngoingOps(lock);
            ioScheduler.load(*this);
            ++storageOpCount;
            lock.unlock();
            buildRecoverySegments(*recoveryPartitions);
            lock.lock();
        }
    }

    if (!isRecovered() && !recoveryException) {
        LOG(DEBUG, "Deferring because <%lu,%lu> not yet filtered",
            *masterId, segmentId);
        return STATUS_RETRY;
    }
    assert(state == RECOVERING);

    if (primary)
        ++metrics->backup.primaryLoadCount;
    else
        ++metrics->backup.secondaryLoadCount;

    if (recoveryException) {
        auto e = SegmentRecoveryFailedException(*recoveryException);
        recoveryException.reset();
        throw e;
    }

    if (partitionId >= recoverySegmentsLength) {
        LOG(WARNING, "Asked for recovery segment %lu from segment <%lu,%lu> "
                     "but there are only %u partitions",
            partitionId, *masterId, segmentId, recoverySegmentsLength);
        throw BackupBadSegmentIdException(HERE);
    }

    recoverySegments[partitionId].appendToBuffer(buffer);

    LOG(DEBUG, "appendRecoverySegment <%lu,%lu>", *masterId, segmentId);
    return STATUS_OK;
}

/**
 * Returns true if the entry is alive and should be recovered, otherwise
 * false if it should be ignored.
 *
 * This decision is necessary in order to avoid zombies. For example, when
 * migrating a tablet away and then back again, its possible for old objects
 * to be in the log from the first instance of the tablet that are no longer
 * alive. If the server fails, we need to filter them out (lest they come
 * back to feast on our delicious brains).
 *
 * To combat this, the coordinator keeps the minimum log offset at which
 * valid objects in each tablet may exist. Any objects that come before this
 * point could not have been part of the current tablet.
 *
 * The situation is slightly complicated by the cleaner, since it may create
 * segments with identifiers ahead of the log head despite containing data
 * from only segments before the head at the time of cleaning. To address
 * this, each segment generated by the cleaner includes the head's segment
 * id at the time it was generated. This allows us to logically order cleaner
 * segments and segments written by the log (i.e. that were previously heads).
 *
 * When a tablet is created, the current log head is recorded. Any cleaner-
 * generated segments marked with that head id or lower cannot contain live
 * data for the new tablet. Only cleaner-generated segments with higher IDs
 * could contain valid data. Segments written by the log (i.e. the head or
 * previous heads) will contain valid data for the new tablet only at positions
 * greater than or equal to the recorded log head at tablet instantation time.
 * Importantly, this requires that the hash table be purged of all objects that
 * were previously in a tablet before a tablet is reassigned, since the cleaner
 * uses the presence of a tablet and hash table entries to gauge liveness.
 *
 * \param position
 *      Log::Position indicating where this entry occurred in the log. Used to
 *      determine if it was written before a tablet it could belong to was
 *      created.
 * \param type
 *      Type of the log entry.
 * \param buffer
 *      Buffer containing the log entry.
 * \param tablet
 *      Tablet to which this entry would belong if were current. It's up to
 *      the caller to ensure that the given entry falls within this tablet
 *      range.
 * \param header
 *      The SegmentHeader of the segment that contains this entry. Used to
 *      determine if the segment was generated by the cleaner or not, and
 *      if so, when it was created. This information precludes objects that
 *      pre-existed the current tablet, but were copied ahead in the log by
 *      the cleaner.
 * \return
 *      true if this object belongs to the given tablet, false if it is from
 *      a previous instance of the tablet and should be dropped.
 */
bool
isEntryAlive(Log::Position& position,
             LogEntryType type,
             Buffer& buffer,
             const ProtoBuf::Tablets::Tablet& tablet,
             const SegmentHeader& header)
{
    if (header.generatedByCleaner()) {
        uint64_t headId = header.headSegmentIdDuringCleaning;
        if (headId <= tablet.ctime_log_head_id())
            return false;
    } else {
        if (position.getSegmentId() < tablet.ctime_log_head_id())
            return false;
        if (position.getSegmentId() == tablet.ctime_log_head_id() &&
          position.getSegmentOffset() < tablet.ctime_log_head_offset()) {
            return false;
        }
    }
    return true;
}

/**
 * Find which of a set of partitions this object or tombstone is in.
 *
 * \param position
 *      Log::Position indicating where this entry occurred in the log. Used to
 *      determine if it was written before a tablet it could belong to was
 *      created.
 * \param type
 *      Type of the log entry.
 * \param buffer
 *      Buffer containing the log entry.
 * \param partitions
 *      The set of object ranges into which the object should be placed.
 * \param header
 *      The SegmentHeader of the segment that contains this entry. Used to
 *      determine if the segment was generated by the cleaner or not, and
 *      if so, when it was created. This information precludes objects that
 *      pre-existed the current tablet, but were copied ahead in the log by
 *      the cleaner.
 * \return
 *      The id of the partition which this object or tombstone belongs in.
 * \throw BackupMalformedSegmentException
 *      If the object or tombstone doesn't belong to any of the partitions.
 */
Tub<uint64_t>
whichPartition(Log::Position& position,
               LogEntryType type,
               Buffer& buffer,
               const ProtoBuf::Tablets& partitions,
               const SegmentHeader& header)
{
    uint64_t tableId = -1;
    HashType keyHash = -1;

    if (type == LOG_ENTRY_TYPE_OBJ) {
        Object object(buffer);
        tableId = object.getTableId();
        keyHash = Key::getHash(tableId, object.getKey(), object.getKeyLength());
    } else { // LOG_ENTRY_TYPE_OBJTOMB:
        ObjectTombstone tomb(buffer);
        tableId = tomb.getTableId();
        keyHash = Key::getHash(tableId, tomb.getKey(), tomb.getKeyLength());
    }

    Tub<uint64_t> ret;
    // TODO(stutsman): need to check how slow this is, can do better with a tree
    for (int i = 0; i < partitions.tablet_size(); i++) {
        const ProtoBuf::Tablets::Tablet& tablet(partitions.tablet(i));
        if (tablet.table_id() == tableId &&
            (tablet.start_key_hash() <= keyHash &&
            tablet.end_key_hash() >= keyHash)) {

            if (!isEntryAlive(position, type, buffer, tablet, header)) {
                LOG(NOTICE, "Skipping object with <tableId, keyHash> of "
                    "<%lu,%lu> because it appears to have existed prior "
                    "to this tablet's creation.", tableId, keyHash);
                return ret;
            }

            ret.construct(tablet.user_data());
            return ret;
        }
    }
    LOG(WARNING, "Couldn't place object with <tableId, keyHash> of <%lu,%lu> "
                 "into any of the given "
                 "tablets for recovery; hopefully it belonged to a deleted "
                 "tablet or lives in another log now", tableId, keyHash);
    return ret;
}

/**
 * Construct recovery segments for this segment data splitting data among
 * them according to #partitions.  After completion and setting
 * #recoverySegments #condition is notified to wake up threads waiting
 * for recovery segments for this segment.
 *
 * \param partitions
 *      A set of tablets grouped into partitions which are used to divide
 *      the stored segment data into recovery segments.
 */
void
BackupService::SegmentInfo::buildRecoverySegments(
    const ProtoBuf::Tablets& partitions)
{
    Lock lock(mutex);
    assert(state == RECOVERING);

    if (recoverySegments) {
        LOG(NOTICE, "Recovery segments already built for <%lu,%lu>",
            masterId.getId(), segmentId);
        // Skip if the recovery segments were generated earlier.
        condition.notify_all();
        return;
    }

    waitForOngoingOps(lock);

    assert(inMemory());

    uint64_t start = Cycles::rdtsc();

    recoveryException.reset();

    uint32_t partitionCount = 0;
    for (int i = 0; i < partitions.tablet_size(); ++i) {
        partitionCount = std::max(partitionCount,
                  downCast<uint32_t>(partitions.tablet(i).user_data() + 1));
    }
    LOG(NOTICE, "Building %u recovery segments for %lu",
        partitionCount, segmentId);
    CycleCounter<RawMetric> _(&metrics->backup.filterTicks);

    recoverySegments = new Segment[partitionCount];
    recoverySegmentsLength = partitionCount;

    try {
        const SegmentHeader* header = NULL;
        for (SegmentIterator it(segment, segmentSize);
             !it.isDone();
             it.next())
        {
            LogEntryType type = it.getType();

            if (type == LOG_ENTRY_TYPE_SEGHEADER) {
                Buffer buffer;
                it.appendToBuffer(buffer);
                header = buffer.getStart<SegmentHeader>();
                continue;
            }

            if (type != LOG_ENTRY_TYPE_OBJ && type != LOG_ENTRY_TYPE_OBJTOMB)
                continue;

            if (header == NULL) {
                LOG(WARNING, "Object or tombstone came before header");
                throw Exception(HERE, "catch this and bail");
            }

            Buffer buffer;
            it.appendToBuffer(buffer);

            Log::Position position(segmentId, it.getOffset());

            // find out which partition this entry belongs in
            Tub<uint64_t> partitionId = whichPartition(position,
                                                       type,
                                                       buffer,
                                                       partitions,
                                                       *header);
            if (!partitionId)
                continue;

            bool success = recoverySegments[*partitionId].append(type, buffer);
            if (!success) {
                LOG(WARNING, "Failed to append data to recovery segment");
                throw Exception(HERE, "catch this and bail");
            }
        }
#if TESTING
        for (uint64_t i = 0; i < recoverySegmentsLength; ++i) {
            LOG(DEBUG, "Recovery segment for <%lu,%lu> partition %lu is %u B",
                *masterId, segmentId, i,
                 recoverySegments[i].getAppendedLength());
        }
#endif
    } catch (const SegmentIteratorException& e) {
        LOG(WARNING, "Exception occurred building recovery segments: %s",
            e.what());
        delete[] recoverySegments;
        recoverySegments = NULL;
        recoverySegmentsLength = 0;
        recoveryException.reset(new SegmentRecoveryFailedException(e.where));
        // leave state as RECOVERING, we'll want to garbage collect this
        // segment at the end of the recovery even if we couldn't parse
        // this segment
    } catch (...) {
        LOG(WARNING, "Unknown exception occurred building recovery segments");
        delete[] recoverySegments;
        recoverySegments = NULL;
        recoverySegmentsLength = 0;
        recoveryException.reset(new SegmentRecoveryFailedException(HERE));
        // leave state as RECOVERING, see note in above block
    }
    LOG(DEBUG, "<%lu,%lu> recovery segments took %lu ms to construct, "
               "notifying other threads",
        *masterId, segmentId,
        Cycles::toNanoseconds(Cycles::rdtsc() - start) / 1000 / 1000);
    condition.notify_all();
}

/**
 * Close this segment to permanent storage and free up in memory
 * resources.
 *
 * After this writeSegment() cannot be called for this segment id any
 * more.  The segment will be restored on recovery unless the client later
 * calls freeSegment() on it and will appear to be closed to the recoverer.
 *
 * \throw BackupBadSegmentIdException
 *      If this segment is not open.
 */
void
BackupService::SegmentInfo::close()
{
    Lock lock(mutex);

    if (state == CLOSED)
        return;
    else if (state != OPEN)
        throw BackupBadSegmentIdException(HERE);

    applyFooterEntry();

    state = CLOSED;
    rightmostWrittenOffset = BYTES_WRITTEN_CLOSED;

    assert(storageHandle);
    ioScheduler.store(*this);
    ++storageOpCount;
}

/**
 * Release all resources related to this segment including storage.
 * This will block for all outstanding storage operations before
 * freeing the storage resources.
 */
void
BackupService::SegmentInfo::free()
{
    Lock lock(mutex);
    waitForOngoingOps(lock);

    // Don't wait for secondary segment recovery segments
    // they aren't running in a separate thread.
    while (state == RECOVERING &&
           primary &&
           !isRecovered() &&
           !recoveryException)
        condition.wait(lock);

    if (inMemory())
        pool.free(segment);
    segment = NULL;
    storage.free(storageHandle);
    storageHandle = NULL;
    state = FREED;
}

/**
 * Open the segment.  After this the segment is in memory and mutable
 * with reserved storage.  Any controlled shutdown of the server will
 * ensure the segment reaches stable storage unless the segment is
 * freed in the meantime.
 *
 * The backing memory is zeroed out, though the storage may still have
 * its old contents.
 */
void
BackupService::SegmentInfo::open()
{
    Lock lock(mutex);
    assert(state == UNINIT);
    // Get memory for staging the segment writes
    char* segment = static_cast<char*>(pool.malloc());
    BackupStorage::Handle* handle;
    try {
        // Reserve the space for this on disk
        handle = storage.allocate();
    } catch (...) {
        // Release the staging memory if storage.allocate throws
        pool.free(segment);
        throw;
    }

    this->segment = segment;
    this->storageHandle = handle;
    state = OPEN;
}

/**
 * Set the state to #RECOVERING from #OPEN or #CLOSED.  This can only be called
 * on a primary segment.  Returns true if the segment was already #RECOVERING.
 */
bool
BackupService::SegmentInfo::setRecovering()
{
    Lock lock(mutex);
    assert(primary);
    applyFooterEntry();
    bool wasRecovering = state == RECOVERING;
    state = RECOVERING;
    return wasRecovering;
}

/**
 * Set the state to #RECOVERING from #OPEN or #CLOSED and store a copy of the
 * supplied tablet information in case construction of recovery segments is
 * needed later for this secondary segment. Returns true if the segment was
 * already #RECOVERING.
 */
bool
BackupService::SegmentInfo::setRecovering(const ProtoBuf::Tablets& partitions)
{
    Lock lock(mutex);
    assert(!primary);
    applyFooterEntry();
    bool wasRecovering = state == RECOVERING;
    state = RECOVERING;
    // Make a copy of the partition list for deferred filtering.
    recoveryPartitions.construct(partitions);
    return wasRecovering;
}


/**
 * Begin reading of segment from disk to memory. Marks the segment
 * CLOSED (immutable) as well.  Once the segment is in memory
 * #segment will be set and waiting threads will be notified via
 * #condition.
 */
void
BackupService::SegmentInfo::startLoading()
{
    Lock lock(mutex);
    waitForOngoingOps(lock);

    ioScheduler.load(*this);
    ++storageOpCount;
}

/**
 * Store data from a buffer into the backup segment.
 * Tracks, for this segment, which is the rightmost written byte which
 * is used as a proxy for "segment length" for the return of
 * startReadingData calls.
 *
 * \param src
 *      The Buffer from which data is to be stored.
 * \param srcOffset
 *      Offset in bytes at which to start copying.
 * \param length
 *      The number of bytes to be copied.
 * \param destOffset
 *      Offset into the backup segment in bytes of where the data is to
 *      be copied.
 * \param footerEntry
 *      New footer which should be written at #destOffset + #length and
 *      at the end of the segment frame if the replica is closed or used
 *      for recovery. NULL if the master provided no footer for this write.
 *      Note this means this write isn't committed on the backup until the
 *      next footered write. See #footerEntry.
 * \param atomic
 *      If true then this replica is considered invalid until a closing
 *      write (or subsequent call to write with \a atomic set to false).
 *      This means that the data will never be written to disk and will
 *      not be reported to or used in recoveries unless the replica is
 *      closed.  This allows masters to create replicas of segments
 *      without the threat that they'll be detected as the head of the
 *      log.  Each value of \a atomic for each write call overrides the
 *      last, so in order to atomically write an entire segment all
 *      writes must have \a atomic set to true (though, it is
 *      irrelvant for the last, closing write).  A call with atomic
 *      set to false will make that replica available for normal
 *      treatment as an open segment.
 */
void
BackupService::SegmentInfo::write(Buffer& src,
                                  uint32_t srcOffset,
                                  uint32_t length,
                                  uint32_t destOffset,
                                  const Segment::OpaqueFooterEntry* footerEntry,
                                  bool atomic)
{
    Lock lock(mutex);
    assert(state == OPEN && inMemory());
    src.copy(srcOffset, length, &segment[destOffset]);
    replicateAtomically = atomic;
    rightmostWrittenOffset = std::max(rightmostWrittenOffset,
                                      destOffset + length);
    if (footerEntry) {
        const uint32_t targetFooterOffset = destOffset + length;
        if (targetFooterOffset < footerOffset) {
            LOG(ERROR, "Write to <%lu,%lu> included a footer which was "
                "requested to be written at offset %u but a prior write "
                "placed a footer later in the segment at %u",
                masterId.getId(), segmentId, targetFooterOffset, footerOffset);
            throw BackupSegmentOverflowException(HERE);
        }
        if (targetFooterOffset + 2 * sizeof(*footerEntry) > segmentSize) {
            // Need room for two copies of the footer. One at the end of the
            // data and the other aligned to the end of the segment which
            // can be found during restart without reading the whole segment.
            LOG(ERROR, "Write to <%lu,%lu> included a footer which was "
                "requested to be written at offset %u but there isn't "
                "enough room in the segment for the footer", masterId.getId(),
                segmentId, targetFooterOffset);
            throw BackupSegmentOverflowException(HERE);
        }
        this->footerEntry = *footerEntry;
        footerOffset = targetFooterOffset;
    }
}

/**
 * Scan the current Segment for a LogDigest. If it exists, append it to the
 * given buffer.
 *
 * \param[out] digestBuffer
 *      Buffer to append the digest to, if found. May be NULL if the data is
 *      not desired and only the presence is needed.
 * \return
 *      True if the digest was found and appended to the given buffer, otherwise
 *      false.
 */
bool
BackupService::SegmentInfo::getLogDigest(Buffer* digestBuffer)
{
    Lock lock(mutex);
    // If the Segment is malformed somehow, just ignore it. The
    // coordinator will have to deal.
    try {
        SegmentIterator it(segment, segmentSize);
        while (!it.isDone()) {
            if (it.getType() == LOG_ENTRY_TYPE_LOGDIGEST) {
                if (digestBuffer != NULL)
                    it.appendToBuffer(*digestBuffer);
                return true;
            }
            it.next();
        }
    } catch (SegmentIteratorException& e) {
        LOG(WARNING, "SegmentIterator constructor failed: %s", e.str().c_str());
    }
    return false;
}

// - private -

/**
 * Flatten #footerEntry into two locations in the in-memory buffer (#segment).
 * The first goes where the master asked (#footerOffset), the second goes at
 * the end of the footer, which makes it efficient to find on restart.
 * This has the effect of truncating any non-footered writes since the last
 * footered-write. See #footerEntry for details.
 */
void
BackupService::SegmentInfo::applyFooterEntry()
{
    if (state != OPEN)
        return;
    memcpy(segment + footerOffset, &footerEntry, sizeof(footerEntry));
    memcpy(segment + segmentSize - sizeof(footerEntry),
           &footerEntry, sizeof(footerEntry));
}

// --- BackupService::IoScheduler ---

/**
 * Construct an IoScheduler.  There is just one instance of this
 * with its operator() running in a new thread which is instantated
 * by the BackupService.
 */
BackupService::IoScheduler::IoScheduler()
    : queueMutex()
    , queueCond()
    , loadQueue()
    , storeQueue()
    , running(true)
    , outstandingStores(0)
{
}

/**
 * Dequeue IO requests and process them on a thread separate from the
 * main thread.  Processes just one IO at a time.  Prioritizes loads
 * over stores.
 */
void
BackupService::IoScheduler::operator()()
try {
    while (true) {
        SegmentInfo* info = NULL;
        bool isLoad = false;
        {
            Lock lock(queueMutex);
            while (loadQueue.empty() && storeQueue.empty()) {
                if (!running)
                    return;
                queueCond.wait(lock);
            }
            isLoad = !loadQueue.empty();
            if (isLoad) {
                info = loadQueue.front();
                loadQueue.pop();
            } else {
                info = storeQueue.front();
                storeQueue.pop();
            }
        }

        if (isLoad) {
            LOG(DEBUG, "Dispatching load of <%lu,%lu>",
                *info->masterId, info->segmentId);
            doLoad(*info);
        } else {
            LOG(DEBUG, "Dispatching store of <%lu,%lu>",
                *info->masterId, info->segmentId);
            doStore(*info);
        }
    }
} catch (const std::exception& e) {
    LOG(ERROR, "Fatal error in BackupService::IoScheduler: %s", e.what());
    throw;
} catch (...) {
    LOG(ERROR, "Unknown fatal error in BackupService::IoScheduler.");
    throw;
}

/**
 * Queue a segment load operation to be done on a separate thread.
 *
 * \param info
 *      The SegmentInfo whose data will be loaded from storage.
 */
void
BackupService::IoScheduler::load(SegmentInfo& info)
{
#ifdef SINGLE_THREADED_BACKUP
    doLoad(info);
#else
    Lock lock(queueMutex);
    loadQueue.push(&info);
    uint32_t count = downCast<uint32_t>(loadQueue.size() + storeQueue.size());
    LOG(DEBUG, "Queued load of <%lu,%lu> (%u segments waiting for IO)",
        *info.masterId, info.segmentId, count);
    queueCond.notify_all();
#endif
}

/**
 * Flush all data to storage.
 * Returns once all dirty buffers have been written to storage.
 */
void
BackupService::IoScheduler::quiesce()
{
    while (outstandingStores > 0) {
        /* pass */;
    }
}

/**
 * Queue a segment store operation to be done on a separate thread.
 *
 * \param info
 *      The SegmentInfo whose data will be stored.
 */
void
BackupService::IoScheduler::store(SegmentInfo& info)
{
#ifdef SINGLE_THREADED_BACKUP
    doStore(info);
#else
    ++outstandingStores;
    Lock lock(queueMutex);
    storeQueue.push(&info);
    uint32_t count = downCast<uint32_t>(loadQueue.size() + storeQueue.size());
    LOG(DEBUG, "Queued store of <%lu,%lu> (%u segments waiting for IO)",
        *info.masterId, info.segmentId, count);
    queueCond.notify_all();
#endif
}

/**
 * Wait for the IO scheduler to finish currently queued requests and
 * return once the scheduler has terminated and its thread is disposed
 * of.
 *
 * \param ioThread
 *      The thread this IoScheduler is running on.  It will be joined
 *      to ensure the scheduler has fully exited before this call
 *      returns.
 */
void
BackupService::IoScheduler::shutdown(std::thread& ioThread)
{
    {
        Lock lock(queueMutex);
        LOG(DEBUG, "IoScheduler thread exiting");
        uint32_t count = downCast<uint32_t>(loadQueue.size() +
                                            storeQueue.size());
        if (count)
            LOG(DEBUG, "IoScheduler must service %u pending IOs before exit",
                count);
        running = false;
        queueCond.notify_one();
    }
    ioThread.join();
}

// - private -

/**
 * Load a segment from disk into a valid buffer in memory.  Locks
 * the #SegmentInfo::mutex to ensure other operations aren't performed
 * on the segment in the meantime.
 *
 * \param info
 *      The SegmentInfo whose data will be loaded from storage.
 */
void
BackupService::IoScheduler::doLoad(SegmentInfo& info) const
{
#ifndef SINGLE_THREADED_BACKUP
    SegmentInfo::Lock lock(info.mutex);
#endif
    ReferenceDecrementer<int> _(info.storageOpCount);

    LOG(DEBUG, "Loading segment <%lu,%lu>", *info.masterId, info.segmentId);
    if (info.inMemory()) {
        LOG(DEBUG, "Already in memory, skipping load on <%lu,%lu>",
            *info.masterId, info.segmentId);
        info.condition.notify_all();
        return;
    }

    char* segment = static_cast<char*>(info.pool.malloc());
    try {
        CycleCounter<RawMetric> _(&metrics->backup.storageReadTicks);
        ++metrics->backup.storageReadCount;
        metrics->backup.storageReadBytes += info.segmentSize;
        uint64_t startTime = Cycles::rdtsc();
        info.storage.getSegment(info.storageHandle, segment);
        uint64_t transferTime = Cycles::toNanoseconds(Cycles::rdtsc() -
            startTime);
        LOG(DEBUG, "Load of <%lu,%lu> took %lu us (%f MB/s)",
            *info.masterId, info.segmentId,
            transferTime / 1000,
            (info.segmentSize / (1 << 20)) /
            (static_cast<double>(transferTime) / 1000000000lu));
    } catch (...) {
        info.pool.free(segment);
        segment = NULL;
        info.condition.notify_all();
        throw;
    }
    info.segment = segment;
    info.condition.notify_all();
    metrics->backup.readingDataTicks = Cycles::rdtsc() - recoveryStart;
}

/**
 * Store a segment to disk from a valid buffer in memory.  Locks
 * the #SegmentInfo::mutex to ensure other operations aren't performed
 * on the segment in the meantime.
 *
 * \param info
 *      The SegmentInfo whose data will be stored.
 */
void
BackupService::IoScheduler::doStore(SegmentInfo& info) const
{
#ifndef SINGLE_THREADED_BACKUP
    SegmentInfo::Lock lock(info.mutex);
#endif
    ReferenceDecrementer<int> _(info.storageOpCount);

    LOG(DEBUG, "Storing segment <%lu,%lu>", *info.masterId, info.segmentId);
    try {
        CycleCounter<RawMetric> _(&metrics->backup.storageWriteTicks);
        ++metrics->backup.storageWriteCount;
        metrics->backup.storageWriteBytes += info.segmentSize;
        uint64_t startTime = Cycles::rdtsc();
        info.storage.putSegment(info.storageHandle, info.segment);
        uint64_t transferTime = Cycles::toNanoseconds(Cycles::rdtsc() -
            startTime);
        LOG(DEBUG, "Store of <%lu,%lu> took %lu us (%f MB/s)",
            *info.masterId, info.segmentId,
            transferTime / 1000,
            (info.segmentSize / (1 << 20)) /
            (static_cast<double>(transferTime) / 1000000000lu));
    } catch (...) {
        LOG(WARNING, "Problem storing segment <%lu,%lu>",
            *info.masterId, info.segmentId);
        info.condition.notify_all();
        throw;
    }
    info.pool.free(info.segment);
    info.segment = NULL;
    --outstandingStores;
    LOG(DEBUG, "Done storing segment <%lu,%lu>",
        *info.masterId, info.segmentId);
    info.condition.notify_all();
}


// --- BackupService::RecoverySegmentBuilder ---

/**
 * Constructs a RecoverySegmentBuilder which handles recovery
 * segment construction for a single recovery.
 * Makes a copy of each of the arguments as a thread local copy
 * for use as the thread runs.
 *
 * \param context
 *      The context in which the new thread will run (same as that of the
 *      parent thread).
 * \param infos
 *      Pointers to SegmentInfo which will be loaded from storage
 *      and split into recovery segments.  Notice, the copy here is
 *      only of the pointer vector and not of the SegmentInfo objects.
 *      Sharing of the SegmentInfo objects is controlled as described
 *      in RecoverySegmentBuilder::infos.
 * \param partitions
 *      The partitions among which the segment should be split for recovery.
 * \param recoveryThreadCount
 *      Reference to an atomic count for tracking number of running recoveries.
 * \param segmentSize
 *      The size of segments stored on masters and in the backup's storage.
 */
BackupService::RecoverySegmentBuilder::RecoverySegmentBuilder(
        Context& context,
        const vector<SegmentInfo*>& infos,
        const ProtoBuf::Tablets& partitions,
        Atomic<int>& recoveryThreadCount,
        uint32_t segmentSize)
    : context(context)
    , infos(infos)
    , partitions(partitions)
    , recoveryThreadCount(recoveryThreadCount)
    , segmentSize(segmentSize)
{
}

/**
 * In order, load each of the segments into memory, meanwhile constructing
 * the recovery segments for the previously loaded segment.
 *
 * Notice this runs in a separate thread and maintains exclusive access to the
 * SegmentInfo objects using #SegmentInfo::mutex.  As
 * the recovery segments for each SegmentInfo are constructed ownership of the
 * SegmentInfo is released and #SegmentInfo::condition is notified to wake
 * up any threads waiting for the recovery segments.
 */
void
BackupService::RecoverySegmentBuilder::operator()()
try {
    uint64_t startTime = Cycles::rdtsc();
    ReferenceDecrementer<Atomic<int>> _(recoveryThreadCount);
    LOG(DEBUG, "Building recovery segments on new thread");

    if (infos.empty())
        return;

    SegmentInfo* loadingInfo = infos[0];
    SegmentInfo* buildingInfo = NULL;

    // Bring the first segment into memory
    loadingInfo->startLoading();

    for (uint32_t i = 1;; i++) {
        assert(buildingInfo != loadingInfo);
        buildingInfo = loadingInfo;
        if (i < infos.size()) {
            loadingInfo = infos[i];
            LOG(DEBUG, "Starting load of %uth segment (<%lu,%lu>)", i,
                loadingInfo->masterId.getId(), loadingInfo->segmentId);
            loadingInfo->startLoading();
        }
        buildingInfo->buildRecoverySegments(partitions);
        LOG(DEBUG, "Done building recovery segments for %uth segment "
            "(<%lu,%lu>)", i - 1,
            buildingInfo->masterId.getId(), buildingInfo->segmentId);
        if (i == infos.size())
            break;
    }
    LOG(DEBUG, "Done building recovery segments, thread exiting");
    uint64_t totalTime = Cycles::toNanoseconds(Cycles::rdtsc() - startTime);
    LOG(DEBUG, "RecoverySegmentBuilder took %lu ms to filter %lu segments "
               "(%f MB/s)",
        totalTime / 1000 / 1000,
        infos.size(),
        static_cast<double>(segmentSize * infos.size() / (1 << 20)) /
        static_cast<double>(totalTime) / 1e9);
} catch (const std::exception& e) {
    LOG(ERROR, "Fatal error in BackupService::RecoverySegmentBuilder: %s",
        e.what());
    throw;
} catch (...) {
    LOG(ERROR, "Unknown fatal error in BackupService::RecoverySegmentBuilder.");
    throw;
}

// --- BackupService::GarbageCollectReplicasFoundOnStorageTask ---

/**
 * Try to garbage collect a replica found on disk until it is finally
 * removed. Usually replicas are freed explicitly by masters, but this
 * doesn't work for cases where the replica was found on disk as part
 * of an old master.
 *
 * This task may generate RPCs to the master to determine the
 * status of the replica which survived on-storage across backup
 * failures.
 *
 * \param service
 *      Backup which is trying to garbage collect the replica.
 * \param masterId
 *      Id of the master which originally created the replica.
 */
BackupService::
    GarbageCollectReplicasFoundOnStorageTask::
    GarbageCollectReplicasFoundOnStorageTask(BackupService& service,
                                            ServerId masterId)
    : Task(service.gcTaskQueue)
    , service(service)
    , masterId(masterId)
    , segmentIds()
    , rpc()
{
}

/**
 * Add a segmentId for a replica which should be considered for garbage
 * collection. Garbage collection won't actually start until schedule() is
 * called. All calls to addSegment must precede the call to schedule() the
 * task.
 *
 * \param segmentId
 *      Id of the segment a particular replica is associated with that was
 *      found on disk storage. Once this task is scheduled it will be
 *      considered for garbage collection until freed.
 */
void
BackupService::GarbageCollectReplicasFoundOnStorageTask::
    addSegmentId(uint64_t segmentId)
{
    segmentIds.push_back(segmentId);
}

/**
 * Try to make progress in garbage collecting replicas without blocking.
 * Attempts one replica at a time in order to prevent flooding the
 * master this task is associated with.
 */
void
BackupService::GarbageCollectReplicasFoundOnStorageTask::performTask()
{
    if (!service.config.backup.gc || segmentIds.empty()) {
        delete this;
        return;
    }
    uint64_t segmentId = segmentIds.front();
    bool done = tryToFreeReplica(segmentId);
    if (done)
        segmentIds.pop_front();
    schedule();
}

/**
 * Only used internally; try to make progress in garbage collecting one replica
 * without blocking.
 *
 * \param segmentId
 *      Id of the segment of the replica which is on the chopping block. Once
 *      a value is passed as \a segmentId that value must be passed on all
 *      subsequent calls until true is returned.
 * \return
 *      True if no additional work is need to free the segment from storage,
 *      false otherwise. See important notes on \a segmentId.
 */
bool
BackupService::GarbageCollectReplicasFoundOnStorageTask::
    tryToFreeReplica(uint64_t segmentId)
{
    {
        BackupService::Lock _(service.mutex);
        auto* replica = service.findSegmentInfo(masterId, segmentId);
        if (!replica)
            return true;
    }

    // Due to RAM-447 it is a bit tricky to decipher when a server is in
    // crashed status. It is done here outside the context of the
    // ServerDoesntExistException because of that bug.
    if (service.context.serverList->contains(masterId) &&
        !service.context.serverList->isUp(masterId))
    {
        // In the server list but not up implies crashed.
        // Since server has crashed just let
        // GarbageCollectDownServerTask free it. It will get
        // scheduled when master recovery finishes for masterId.
        LOG(DEBUG, "Server %lu marked crashed; waiting for cluster "
            "to recover from its failure before freeing <%lu,%lu>",
            masterId.getId(), masterId.getId(), segmentId);
        return true;
    }

    if (rpc) {
        if (rpc->isReady()) {
            bool needed = true;
            try {
                needed = rpc->wait();
            } catch (const ServerDoesntExistException& e) {
                needed = false;
                LOG(DEBUG, "Server %lu marked down; cluster has recovered "
                    "from its failure", masterId.getId());
            }
            rpc.destroy();
            if (!needed) {
                LOG(DEBUG, "Server has recovered from lost replica; "
                    "freeing replica for <%lu,%lu>",
                    masterId.getId(), segmentId);
                deleteReplica(segmentId);
                return true;
            } else {
                LOG(DEBUG, "Server has not recovered from lost "
                    "replica; retaining replica for <%lu,%lu>; will "
                    "probe replica status again later",
                    masterId.getId(), segmentId);
            }
        }
    } else {
        rpc.construct(service.context, masterId, service.serverId, segmentId);
    }
    return false;
}

/**
 * Only used internally; safely frees a replica and removes metadata about
 * it from the backup's segments map.
 */
void
BackupService::GarbageCollectReplicasFoundOnStorageTask::
    deleteReplica(uint64_t segmentId)
{
    SegmentInfo* replica = NULL;
    {
        BackupService::Lock _(service.mutex);
        replica = service.findSegmentInfo(masterId, segmentId);
        if (!replica)
            return;
        service.segments.erase({masterId, segmentId});
    }
    replica->free();
    delete replica;
}

// --- BackupService::GarbageCollectDownServerTask ---

/**
 * Try to garbage collect all replicas stored by a master which is now
 * known to have been completely recovered and removed from the cluster.
 * Usually replicas are freed explicitly by masters, but this
 * doesn't work for cases where the replica was created by a master which
 * has crashed.
 *
 * \param service
 *      Backup trying to garbage collect replicas from some removed master.
 * \param masterId
 *      Id of the master now known to have been removed from the cluster.
 */
BackupService::
    GarbageCollectDownServerTask::
    GarbageCollectDownServerTask(BackupService& service,
                                 ServerId masterId)
    : Task(service.gcTaskQueue)
    , service(service)
    , masterId(masterId)
{}

/**
 * Try to make progress in garbage collecting replicas without blocking.
 */
void
BackupService::GarbageCollectDownServerTask::performTask()
{
    if (!service.config.backup.gc) {
        delete this;
        return;
    }
    BackupService::Lock _(service.mutex);
    auto key = MasterSegmentIdPair(masterId, 0lu);
    auto it = service.segments.upper_bound(key);
    if (it != service.segments.end() && it->second->masterId == masterId) {
        LOG(DEBUG, "Server %lu marked down; cluster has recovered "
                "from its failure; freeing replica <%lu,%lu>",
            masterId.getId(), masterId.getId(), it->first.segmentId);
        it->second->free();
        service.segments.erase(it->first);
        delete it->second;
        schedule();
    } else {
        delete this;
    }
}

// --- BackupService ---

/**
 * Create a BackupService.
 *
 * \param context
 *      Overall information about the RAMCloud server.
 * \param config
 *      Settings for this instance. The caller guarantees that config will
 *      exist for the duration of this BackupService's lifetime.
 */
BackupService::BackupService(Context& context,
                             const ServerConfig& config)
    : context(context)
    , mutex()
    , config(config)
    , formerServerId()
    , serverId(0)
    , recoveryTicks()
    , pool(config.segmentSize)
    , recoveryThreadCount{0}
    , segments()
    , segmentSize(config.segmentSize)
    , storage()
    , storageBenchmarkResults()
    , bytesWritten(0)
    , ioScheduler()
#ifdef SINGLE_THREADED_BACKUP
    , ioThread()
#else
    , ioThread(std::ref(ioScheduler))
#endif
    , initCalled(false)
    , replicationId(0)
    , replicationGroup()
    , gcTracker(context, this)
    , gcThread()
    , testingDoNotStartGcThread(false)
    , gcTaskQueue()
{
    if (config.backup.inMemory)
        storage.reset(new InMemoryStorage(config.segmentSize,
                                          config.backup.numSegmentFrames));
    else
        storage.reset(new SingleFileStorage(config.segmentSize,
                                            config.backup.numSegmentFrames,
                                            config.backup.file.c_str(),
                                            O_DIRECT | O_SYNC));

    try {
        recoveryTicks.construct(); // make unit tests happy

        // Prime the segment pool. This removes an overhead that would
        // otherwise be seen during the first recovery.
        void* mem[100];
        foreach(auto& m, mem)
            m = pool.malloc();
        foreach(auto& m, mem)
            pool.free(m);
    } catch (...) {
        ioScheduler.shutdown(ioThread);
        throw;
    }

    BackupStorage::Superblock superblock = storage->loadSuperblock();
    if (config.clusterName == "__unnamed__") {
        LOG(NOTICE, "Cluster '__unnamed__'; ignoring existing backup storage. "
            "Any replicas stored will not be reusable by future backups. "
            "Specify clusterName for persistence across backup restarts.");
    } else {
        LOG(NOTICE, "Backup storing replicas with clusterName '%s'. Future "
            "backups must be restarted with the same clusterName for replicas "
            "stored on this backup to be reused.", config.clusterName.c_str());
        if (config.clusterName == superblock.getClusterName()) {
            LOG(NOTICE, "Replicas stored on disk have matching clusterName "
                "('%s'). Scanning storage to find all replicas and to make "
                "them available to recoveries.",
                superblock.getClusterName());
            formerServerId = superblock.getServerId();
            LOG(NOTICE, "Will enlist as a replacement for formerly crashed "
                "server %lu which left replicas behind on disk",
                formerServerId.getId());
            restartFromStorage();
        } else {
            LOG(NOTICE, "Replicas stored on disk have a different clusterName "
                "('%s'). Scribbling storage to ensure any stale replicas left "
                "behind by old backups aren't used by future backups",
                superblock.getClusterName());
            killAllStorage();
        }
    }
}

/**
 * Flush open segments to disk and shutdown.
 */
BackupService::~BackupService()
{
    // Stop the garbage collector.
    gcTaskQueue.halt();
    if (gcThread)
        gcThread->join();
    gcThread.destroy();

    int lastThreadCount = 0;
    while (recoveryThreadCount > 0) {
        if (lastThreadCount != recoveryThreadCount) {
            LOG(DEBUG, "Waiting for recovery threads to terminate before "
                "deleting BackupService, %d threads "
                "still running", static_cast<int>(recoveryThreadCount));
            lastThreadCount = recoveryThreadCount;
        }
    }
    Fence::enter();

    ioScheduler.shutdown(ioThread);

    // Clean up any extra SegmentInfos laying around
    // but don't free them; perhaps we want to load them up
    // on a cold start.  The infos destructor will ensure the
    // segment is properly stored before termination.
    foreach (const SegmentsMap::value_type& value, segments) {
        SegmentInfo* info = value.second;
        delete info;
    }
    segments.clear();
}

/**
 * If this server is rejoining a cluster the previous server id it
 * operated under is returned, otherwise an invalid server is returned.
 * "Rejoining" means this backup service may have segment replicas stored
 * that were created by masters in the cluster.
 * In this case, the coordinator must be made told of the former server
 * id under which these replicas were created in order to ensure that
 * all masters are made aware of the former server's crash before learning
 * of its re-enlistment.
 */
ServerId
BackupService::getFormerServerId() const
{
    return formerServerId;
}

/// Returns the serverId granted to this backup by the coordinator.
ServerId
BackupService::getServerId() const
{
    return serverId;
}

/**
 * Perform an initial benchmark of the storage system. This is later passed to
 * the coordinator during init() so that masters can intelligently select
 * backups.
 *
 * This is not part of the constructor because we don't want the benchmark
 * running during unit tests. It's not part of init() because that method must
 * be fast (right now, as soon as init runs for some service, clients can start
 * accessing the service; however, the dispatch loop won't run until init
 * completes for all services).
 */
void
BackupService::benchmark(uint32_t& readSpeed, uint32_t& writeSpeed) {
    auto strategy = static_cast<BackupStrategy>(config.backup.strategy);
    storageBenchmarkResults = storage->benchmark(strategy);
    readSpeed = storageBenchmarkResults.first;
    writeSpeed = storageBenchmarkResults.second;
}

// See Server::dispatch.
void
BackupService::dispatch(WireFormat::Opcode opcode, Rpc& rpc)
{
    Lock _(mutex); // Lock out GC while any RPC is being processed.

    // This is a hack. We allow the AssignGroup Rpc to be processed before
    // initCalled is set to true, since it is sent during initialization.
    assert(initCalled || opcode == WireFormat::BackupAssignGroup::opcode);
    CycleCounter<RawMetric> serviceTicks(&metrics->backup.serviceTicks);

    switch (opcode) {
        case WireFormat::BackupAssignGroup::opcode:
            callHandler<WireFormat::BackupAssignGroup, BackupService,
                        &BackupService::assignGroup>(rpc);
            break;
        case WireFormat::BackupFree::opcode:
            callHandler<WireFormat::BackupFree, BackupService,
                        &BackupService::freeSegment>(rpc);
            break;
        case WireFormat::BackupGetRecoveryData::opcode:
            callHandler<WireFormat::BackupGetRecoveryData, BackupService,
                        &BackupService::getRecoveryData>(rpc);
            break;
        case WireFormat::BackupQuiesce::opcode:
            callHandler<WireFormat::BackupQuiesce, BackupService,
                        &BackupService::quiesce>(rpc);
            break;
        case WireFormat::BackupRecoveryComplete::opcode:
            callHandler<WireFormat::BackupRecoveryComplete, BackupService,
                        &BackupService::recoveryComplete>(rpc);
            break;
        case WireFormat::BackupStartReadingData::opcode:
            callHandler<WireFormat::BackupStartReadingData, BackupService,
                        &BackupService::startReadingData>(rpc);
            break;
        case WireFormat::BackupWrite::opcode:
            callHandler<WireFormat::BackupWrite, BackupService,
                        &BackupService::writeSegment>(rpc);
            break;
        default:
            throw UnimplementedRequestError(HERE);
    }
}

/**
 * Assign a replication group to a backup, and notifies it of its peer group
 * members. The replication group serves as a set of backups that store all
 * the replicas of a segment.
 *
 * \param reqHdr
 *      Header of the Rpc request containing the replication group Ids.
 *
 * \param respHdr
 *      Header for the Rpc response.
 *
 * \param rpc
 *      The Rpc being serviced.
 */
void
BackupService::assignGroup(
        const WireFormat::BackupAssignGroup::Request& reqHdr,
        WireFormat::BackupAssignGroup::Response& respHdr,
        Rpc& rpc)
{
    replicationId = reqHdr.replicationId;
    uint32_t reqOffset = sizeof32(reqHdr);
    replicationGroup.clear();
    for (uint32_t i = 0; i < reqHdr.numReplicas; i++) {
        const uint64_t *backupId =
            rpc.requestPayload.getOffset<uint64_t>(reqOffset);
        replicationGroup.push_back(ServerId(*backupId));
        reqOffset += sizeof32(*backupId);
    }
}

/**
 * Removes the specified segment from permanent storage and releases
 * any in memory copy if it exists.
 *
 * After this call completes the segment number segmentId will no longer
 * be in permanent storage and will not be recovered during recovery.
 *
 * This is a no-op if the backup is unaware of this segment.
 *
 * \param reqHdr
 *      Header of the Rpc request containing the segment number to free.
 * \param respHdr
 *      Header for the Rpc response.
 * \param rpc
 *      The Rpc being serviced.
 */
void
BackupService::freeSegment(const WireFormat::BackupFree::Request& reqHdr,
                           WireFormat::BackupFree::Response& respHdr,
                           Rpc& rpc)
{
    LOG(DEBUG, "Freeing replica for master %lu segment %lu",
        reqHdr.masterId, reqHdr.segmentId);

    SegmentsMap::iterator it =
        segments.find(MasterSegmentIdPair(ServerId(reqHdr.masterId),
                                                   reqHdr.segmentId));
    if (it == segments.end()) {
        LOG(WARNING, "Master tried to free non-existent segment: <%lu,%lu>",
            reqHdr.masterId, reqHdr.segmentId);
        return;
    }

    it->second->free();
    segments.erase(it);
    delete it->second;
}

/**
 * Find SegmentInfo for a segment or NULL if we don't know about it.
 *
 * \param masterId
 *      The master id of the master of the segment whose info is being sought.
 * \param segmentId
 *      The segment id of the segment whose info is being sought.
 * \return
 *      A pointer to the SegmentInfo for the specified segment or NULL if
 *      none exists.
 */
BackupService::SegmentInfo*
BackupService::findSegmentInfo(ServerId masterId, uint64_t segmentId)
{
    SegmentsMap::iterator it =
        segments.find(MasterSegmentIdPair(masterId, segmentId));
    if (it == segments.end())
        return NULL;
    return it->second;
}


/**
 * Return the data for a particular tablet that was recovered by a call
 * to startReadingData().
 *
 * \param reqHdr
 *      Header of the Rpc request which contains the Rpc arguments.
 * \param respHdr
 *      Header for the Rpc response containing a count of RecoveredObjects being
 *      returned.
 * \param rpc
 *      The Rpc being serviced.  A back-to-back list of RecoveredObjects
 *      follows the respHdr in this buffer of length
 *      respHdr->recoveredObjectCount.
 *
 * \throw BackupBadSegmentIdException
 *      If the segment has not had recovery started for it (startReadingData()
 *      must have been called for its corresponding master id).
 */
void
BackupService::getRecoveryData(
    const WireFormat::BackupGetRecoveryData::Request& reqHdr,
    WireFormat::BackupGetRecoveryData::Response& respHdr,
    Rpc& rpc)
{
    LOG(DEBUG, "getRecoveryData masterId %lu, segmentId %lu, partitionId %lu",
        reqHdr.masterId, reqHdr.segmentId, reqHdr.partitionId);

    SegmentInfo* info = findSegmentInfo(ServerId(reqHdr.masterId),
                                                 reqHdr.segmentId);
    if (!info) {
        LOG(WARNING, "Asked for bad segment <%lu,%lu>",
            reqHdr.masterId, reqHdr.segmentId);
        throw BackupBadSegmentIdException(HERE);
    }

    Status status = info->appendRecoverySegment(reqHdr.partitionId,
                                                rpc.replyPayload);
    if (status != STATUS_OK) {
        respHdr.common.status = status;
        return;
    }

    ++metrics->backup.readCompletionCount;
    LOG(DEBUG, "getRecoveryData complete");
}

/**
 * Perform once-only initialization for the backup service after having
 * enlisted the process with the coordinator
 */
void
BackupService::init(ServerId id)
{
    assert(!initCalled);

    serverId = id;
    LOG(NOTICE, "My server ID is %lu", *id);
    if (metrics->serverId == 0) {
        metrics->serverId = *serverId;
    }

    storage->resetSuperblock(serverId, config.clusterName);
    LOG(NOTICE, "Backup %lu will store replicas under cluster name '%s'",
        serverId.getId(), config.clusterName.c_str());
    initCalled = true;
}

/**
 * Scribble a bit over all the headers of all the segment frames to prevent
 * what is already on disk from being reused in future runs.
 * This is called whenever the user requests to not use the stored segment
 * frames.  Not doing this could yield interesting surprises to the user.
 * For example, one might find replicas for the same segment id with
 * different contents during future runs.
 */
void
BackupService::killAllStorage()
{
    CycleCounter<> killTime;
    for (uint32_t frame = 0; frame < config.backup.numSegmentFrames; ++frame) {
        std::unique_ptr<BackupStorage::Handle>
            handle(storage->associate(frame));
        if (handle)
            storage->free(handle.release());
    }

    LOG(NOTICE, "On-storage replica destroyed: %f ms",
        1000. * Cycles::toSeconds(killTime.stop()));
}

/**
 * Flush all data to storage.
 * Returns once all dirty buffers have been written to storage.
 * \param reqHdr
 *      Header of the Rpc request.
 * \param respHdr
 *      Header for the Rpc response.
 * \param rpc
 *      The Rpc being serviced.
 */
void
BackupService::quiesce(const WireFormat::BackupQuiesce::Request& reqHdr,
                       WireFormat::BackupQuiesce::Response& respHdr,
                       Rpc& rpc)
{
    TEST_LOG("Backup at %s quiescing", config.localLocator.c_str());
    ioScheduler.quiesce();
}

/**
 * Clean up state associated with the given master after recovery.
 * \param reqHdr
 *      Header of the Rpc request containing the segment number to free.
 * \param respHdr
 *      Header for the Rpc response.
 * \param rpc
 *      The Rpc being serviced.
 */
void
BackupService::recoveryComplete(
        const WireFormat::BackupRecoveryComplete::Request& reqHdr,
        WireFormat::BackupRecoveryComplete::Response& respHdr,
        Rpc& rpc)
{
    LOG(DEBUG, "masterID: %lu", reqHdr.masterId);
    rpc.sendReply();
    recoveryTicks.destroy();
}

/**
 * Returns true if left points to a SegmentInfo with a lesser
 * segmentId than that pointed to by right.
 *
 * \param left
 *      A pointer to a SegmentInfo to be compared to #right.
 * \param right
 *      A pointer to a SegmentInfo to be compared to #left.
 */
bool
BackupService::segmentInfoLessThan(SegmentInfo* left,
                                   SegmentInfo* right)
{
    return *left < *right;
}

/**
 * Scans storage to find replicas left behind by former backups and
 * incorporates them into this backup.  This should only be called before
 * the backup has serviced any requests.
 *
 * Only replica headers and footers are scanned.  Any replica with a valid
 * header but no discernable footer is included and considered open.  If a
 * header is found and a reasonable footer then the segment is considered
 * closed.  At some point backups will want to write a checksum even for open
 * segments so we'll need some changes to the logic of this method to deal
 * with that.  Also, at that point we should augment the checksum to cover
 * the log entry type of the footer as well.
 */
void
BackupService::restartFromStorage()
{
    CycleCounter<> restartTime;

// This is all broken. BackupService should not dig so deeply into the internals
// of segments.
    return;
#if 0
    struct HeaderAndFooter {
        SegmentEntry headerEntry;
        SegmentHeader header;
        SegmentEntry footerEntry;
        SegmentFooter footer;
    } __attribute__ ((packed));
    assert(sizeof(HeaderAndFooter) ==
           2 * sizeof(SegmentEntry) +
           sizeof(SegmentHeader) +
           sizeof(SegmentFooter));

    std::unique_ptr<char[]> allHeadersAndFooters =
        storage->getAllHeadersAndFooters(
            sizeof(SegmentEntry) + sizeof(SegmentHeader),
            sizeof(SegmentEntry) + sizeof(SegmentFooter));

    if (!allHeadersAndFooters) {
        LOG(NOTICE, "Selected backup storage does not support restart of "
            "backups, continuing as an empty backup");
        return;
    }

    const HeaderAndFooter* entries =
        reinterpret_cast<const HeaderAndFooter*>(allHeadersAndFooters.get());

    // Keeps track of which task is garbage collecting replicas for each
    // masterId.
    std::unordered_map<ServerId, GarbageCollectReplicasFoundOnStorageTask*>
        gcTasks;

    for (uint32_t frame = 0; frame < config.backup.numSegmentFrames; ++frame) {
        auto& entry = entries[frame];
        // Check header.
        if (entry.headerEntry.type != LOG_ENTRY_TYPE_SEGHEADER) {
            TEST_LOG("Log entry type for header does not match in frame %u",
                     frame);
            continue;
        }
        if (entry.headerEntry.length != sizeof32(SegmentHeader)) {
            LOG(WARNING, "Unexpected log entry length while reading segment "
                "replica header from backup storage, discarding replica, "
                "(expected length %lu, stored length %u)",
                sizeof(SegmentHeader), entry.headerEntry.length);
            continue;
        }
        if (entry.header.segmentCapacity != segmentSize) {
            LOG(ERROR, "An on-storage segment header indicates a different "
                "segment size than the backup service was told to use; "
                "ignoring the problem; note ANY call to open a segment "
                "on this backup could cause the overwrite of your strangely "
                "sized replicas.");
        }

        const uint64_t logId = entry.header.logId;
        const uint64_t segmentId = entry.header.segmentId;
        // Check footer.
        bool wasClosedOnStorage = false;
        if (entry.footerEntry.type == LOG_ENTRY_TYPE_SEGFOOTER &&
            entry.footerEntry.length == sizeof(SegmentFooter)) {
            wasClosedOnStorage = true;
        }
        // TODO(stutsman): Eventually will need open segment checksums.
        LOG(DEBUG, "Found stored replica <%lu,%lu> on backup storage in "
                   "frame %u which was %s",
            entry.header.logId, entry.header.segmentId, frame,
            wasClosedOnStorage ? "closed" : "open");

        // Add this replica to metadata.
        const ServerId masterId(logId);
        auto* info = new SegmentInfo(*storage, pool, ioScheduler,
                               masterId, segmentId, segmentSize,
                               frame, wasClosedOnStorage);
        segments[MasterSegmentIdPair(masterId, segmentId)] = info;
        if (gcTasks.find(masterId) == gcTasks.end()) {
            gcTasks[masterId] =
                new GarbageCollectReplicasFoundOnStorageTask(*this, masterId);
        }
        gcTasks[masterId]->addSegmentId(segmentId);
    }

    // Kick off the garbage collection for replicas stored on disk.
    foreach (const auto& value, gcTasks)
        value.second->schedule();

    LOG(NOTICE, "Replica information retreived from storage: %f ms",
        1000. * Cycles::toSeconds(restartTime.stop()));
#endif
}

/**
 * Begin reading disk data for a Master and bucketing the objects in the
 * Segments according to a requested TabletMap, returning a list of backed
 * up Segments this backup has for the Master.
 *
 * \param reqHdr
 *      Header of the Rpc request which contains the Rpc arguments.
 * \param respHdr
 *      Header for the Rpc response containing a count of segmentIds being
 *      returned.
 * \param rpc
 *      The Rpc being serviced.  A back-to-back list of uint64_t segmentIds
 *      follows the respHdr in this buffer of length respHdr->segmentIdCount.
 */
void
BackupService::startReadingData(
        const WireFormat::BackupStartReadingData::Request& reqHdr,
        WireFormat::BackupStartReadingData::Response& respHdr,
        Rpc& rpc)
{
    recoveryTicks.construct(&metrics->backup.recoveryTicks);
    recoveryStart = Cycles::rdtsc();
    metrics->backup.recoveryCount++;
    metrics->backup.storageType = static_cast<uint64_t>(storage->storageType);

    ProtoBuf::Tablets partitions;
    ProtoBuf::parseFromResponse(rpc.requestPayload, sizeof(reqHdr),
                                reqHdr.partitionsLength, partitions);
    LOG(DEBUG, "Backup preparing for recovery of crashed server %lu; "
        "loading replicas and filtering them according to the following "
        "partitions:\n%s",
        reqHdr.masterId, partitions.DebugString().c_str());

    uint64_t logDigestLastId = ~0UL;
    uint32_t logDigestLastLen = 0;
    Buffer logDigest;

    vector<SegmentInfo*> primarySegments;
    vector<SegmentInfo*> secondarySegments;

    for (SegmentsMap::iterator it = segments.begin();
         it != segments.end(); it++)
    {
        ServerId masterId = it->first.masterId;
        SegmentInfo* info = it->second;

        if (*masterId == reqHdr.masterId) {
            if (!info->satisfiesAtomicReplicationGuarantees()) {
                LOG(WARNING, "Asked for replica <%lu,%lu> which was being "
                    "replicated atomically but which has yet to be closed; "
                    "ignoring the replica", masterId.getId(), info->segmentId);
                continue;
            }
            (info->primary ?
                primarySegments :
                secondarySegments).push_back(info);
        }
    }

    // Shuffle the primary entries, this helps all recovery
    // masters to stay busy even if the log contains long sequences of
    // back-to-back segments that only have objects for a particular
    // partition.  Secondary order is irrelevant.
    std::random_shuffle(primarySegments.begin(),
                        primarySegments.end(),
                        randomNumberGenerator);

    typedef WireFormat::BackupStartReadingData::Replica Replica;

    bool allRecovered = true;
    foreach (auto info, primarySegments) {
        new(&rpc.replyPayload, APPEND) Replica
            {info->segmentId, info->getRightmostWrittenOffset()};
        LOG(DEBUG, "Crashed master %lu had segment %lu (primary) with len %u",
            *info->masterId, info->segmentId,
            info->getRightmostWrittenOffset());
        bool wasRecovering = info->setRecovering();
        allRecovered &= wasRecovering;
    }
    foreach (auto info, secondarySegments) {
        new(&rpc.replyPayload, APPEND) Replica
            {info->segmentId, info->getRightmostWrittenOffset()};
        LOG(DEBUG, "Crashed master %lu had segment %lu (secondary) with len %u"
            ", stored partitions for deferred recovery segment construction",
            *info->masterId, info->segmentId,
            info->getRightmostWrittenOffset());
        bool wasRecovering = info->setRecovering(partitions);
        allRecovered &= wasRecovering;
    }
    respHdr.segmentIdCount = downCast<uint32_t>(primarySegments.size() +
                                                secondarySegments.size());
    respHdr.primarySegmentCount = downCast<uint32_t>(primarySegments.size());
    LOG(DEBUG, "Sending %u segment ids for this master (%u primary)",
        respHdr.segmentIdCount, respHdr.primarySegmentCount);

    vector<SegmentInfo*> allSegments[2] = {primarySegments, secondarySegments};
    foreach (auto segments, allSegments) {
        foreach (auto info, segments) {
            // Obtain the LogDigest from the lowest Segment Id of any
            // open replica.
            if (info->isOpen() && info->segmentId <= logDigestLastId) {
                if (info->getLogDigest(NULL)) {
                    logDigest.reset();
                    info->getLogDigest(&logDigest);
                    logDigestLastId = info->segmentId;
                    logDigestLastLen = info->getRightmostWrittenOffset();
                    LOG(DEBUG, "Segment %lu's LogDigest queued for response",
                        info->segmentId);
                }
            }
        }
    }

    respHdr.digestSegmentId  = logDigestLastId;
    respHdr.digestSegmentLen = logDigestLastLen;
    respHdr.digestBytes = logDigest.getTotalLength();
    if (respHdr.digestBytes > 0) {
        void* out = new(&rpc.replyPayload, APPEND) char[respHdr.digestBytes];
        memcpy(out,
               logDigest.getRange(0, respHdr.digestBytes),
               respHdr.digestBytes);
        LOG(DEBUG, "Sent %u bytes of LogDigest to coord", respHdr.digestBytes);
    }

    rpc.sendReply();

    // Don't spin up a thread if there isn't anything that needs to be built.
    if (allRecovered)
        return;

#ifndef SINGLE_THREADED_BACKUP
    RecoverySegmentBuilder builder(context,
                                   primarySegments,
                                   partitions,
                                   recoveryThreadCount,
                                   segmentSize);
    ++recoveryThreadCount;
    std::thread builderThread(builder);
    builderThread.detach();
    LOG(DEBUG, "Kicked off building recovery segments; "
               "main thread going back to dispatching requests");
#endif
}

/**
 * Store an opaque string of bytes in a currently open segment on
 * this backup server.  This data is guaranteed to be considered on recovery
 * of the master to which this segment belongs to the best of the ability of
 * this BackupService and will appear open unless it is closed
 * beforehand.
 *
 * If BackupWriteRpc::OPEN flag is set then space is allocated to receive
 * backup writes for a segment.  If this call succeeds the Backup must not
 * reject subsequent writes to this segment for lack of space.  The caller must
 * ensure that this masterId,segmentId pair is unique and hasn't been used
 * before on this backup or any other.  The storage space remains in use until
 * the master calls freeSegment(), the segment is involved in a recovery that
 * completes successfully, or until the backup crashes.
 *
 * If BackupWriteRpc::CLOSE flag is set then once this call succeeds the
 * segment will be considered closed and immutable and the Backup will
 * use this as a hint to move the segment to appropriate storage.
 *
 * \param reqHdr
 *      Header of the Rpc request which contains the Rpc arguments except
 *      the data to be written.
 * \param respHdr
 *      Header for the Rpc response.
 * \param rpc
 *      The Rpc being serviced, used for access to the opaque bytes to
 *      be written which follow reqHdr.
 *
 * \throw BackupSegmentOverflowException
 *      If the write request is beyond the end of the segment.
 * \throw BackupBadSegmentIdException
 *      If the segment is not open.
 */
void
BackupService::writeSegment(const WireFormat::BackupWrite::Request& reqHdr,
                            WireFormat::BackupWrite::Response& respHdr,
                            Rpc& rpc)
{
    ServerId masterId(reqHdr.masterId);
    uint64_t segmentId = reqHdr.segmentId;

    // The default value for numReplicas is 0.
    respHdr.numReplicas = 0;

    SegmentInfo* info = findSegmentInfo(masterId, segmentId);
    if (info && !info->createdByCurrentProcess) {
        if ((reqHdr.flags & WireFormat::BackupWrite::OPEN)) {
            // This can happen if a backup crashes, restarts, reloads a
            // replica from disk, and then the master detects the crash and
            // tries to re-replicate the segment which lost a replica on
            // the restarted backup.
            LOG(NOTICE, "Master tried to open replica for <%lu,%lu> but "
                "another replica was recovered from storage with the same id; "
                "rejecting open request", masterId.getId(), segmentId);
            throw BackupOpenRejectedException(HERE);
        } else {
            // This should never happen.
            LOG(ERROR, "Master tried to write replica for <%lu,%lu> but "
                "another replica was recovered from storage with the same id; "
                "rejecting write request", masterId.getId(), segmentId);
            // This exception will crash the calling master.
            throw BackupBadSegmentIdException(HERE);
        }
    }

    // Perform open, if any.
    if ((reqHdr.flags & WireFormat::BackupWrite::OPEN)) {
#ifdef SINGLE_THREADED_BACKUP
        bool primary = false;
#else
        bool primary = reqHdr.flags & WireFormat::BackupWrite::PRIMARY;
#endif
        if (primary) {
            uint32_t numReplicas = downCast<uint32_t>(
                replicationGroup.size());
            // Respond with the replication group members.
            respHdr.numReplicas = numReplicas;
            if (numReplicas > 0) {
                uint64_t* dest =
                    new(&rpc.replyPayload, APPEND) uint64_t[numReplicas];
                int i = 0;
                foreach(ServerId id, replicationGroup) {
                    dest[i] = id.getId();
                    i++;
                }
            }
        }
        if (!info) {
            LOG(DEBUG, "Opening <%lu,%lu>", *masterId, segmentId);
            try {
                info = new SegmentInfo(*storage,
                                       pool,
                                       ioScheduler,
                                       masterId,
                                       segmentId,
                                       segmentSize,
                                       primary);
                segments[MasterSegmentIdPair(masterId, segmentId)] = info;
                info->open();
            } catch (const BackupStorageException& e) {
                segments.erase(MasterSegmentIdPair(masterId, segmentId));
                delete info;
                LOG(NOTICE, "Master tried to open replica for <%lu,%lu> but "
                    "there was a problem allocating storage space; "
                    "rejecting open request: %s", masterId.getId(), segmentId,
                    e.what());
                throw BackupOpenRejectedException(HERE);
            } catch (...) {
                segments.erase(MasterSegmentIdPair(masterId, segmentId));
                delete info;
                throw;
            }
        }
    }

    // Perform write.
    if (!info) {
        LOG(WARNING, "Tried write to a replica of segment <%lu,%lu> but "
            "no such replica was open on this backup (server id %lu)",
            masterId.getId(), segmentId, serverId.getId());
        throw BackupBadSegmentIdException(HERE);
    } else if (info->isOpen()) {
        // Need to check all three conditions because overflow is possible
        // on the addition
        if (reqHdr.length > segmentSize ||
            reqHdr.offset > segmentSize ||
            reqHdr.length + reqHdr.offset > segmentSize)
            throw BackupSegmentOverflowException(HERE);

        {
            CycleCounter<RawMetric> __(&metrics->backup.writeCopyTicks);
            auto* footerEntry = reqHdr.footerIncluded ?
                                                &reqHdr.footerEntry : NULL;
            info->write(rpc.requestPayload, sizeof(reqHdr),
                        reqHdr.length, reqHdr.offset,
                        footerEntry, reqHdr.atomic);
        }
        metrics->backup.writeCopyBytes += reqHdr.length;
        bytesWritten += reqHdr.length;
    } else {
        if (!(reqHdr.flags & WireFormat::BackupWrite::CLOSE)) {
            LOG(WARNING, "Tried write to a replica of segment <%lu,%lu> but "
                "the replica was already closed on this backup (server id %lu)",
                masterId.getId(), segmentId, serverId.getId());
            throw BackupBadSegmentIdException(HERE);
        }
        LOG(WARNING, "Closing segment write after close, may have been "
            "a redundant closing write, ignoring");
    }

    // Perform close, if any.
    if (reqHdr.flags & WireFormat::BackupWrite::CLOSE)
        info->close();
}

/**
 * Runs garbage collection tasks.
 */
void
BackupService::gcMain()
try {
    gcTaskQueue.performTasksUntilHalt();
} catch (const std::exception& e) {
    LOG(ERROR, "Fatal error in BackupService::gcThread: %s", e.what());
    throw;
} catch (...) {
    LOG(ERROR, "Unknown fatal error in BackupService::gcThread.");
    throw;
}

/**
 * Start garbage collection of replicas for any server that gets removed
 * from the cluster. Called when changes to the server wide server list
 * are enqueued.
 */
void
BackupService::trackerChangesEnqueued()
{
    // Start the replica garbage collector if it hasn't been started yet.
    // Starting late ensures that garbage collection tasks don't get
    // processed before the first push to the server list from the coordinator.
    if (!initCalled)
        return;
    if (!gcThread && !testingDoNotStartGcThread) {
        LOG(NOTICE, "Starting backup replica garbage collector thread");
        gcThread.construct(&BackupService::gcMain, this);
    }
    ServerDetails server;
    ServerChangeEvent event;
    while (gcTracker.getChange(server, event)) {
        if (event == SERVER_REMOVED) {
            (new GarbageCollectDownServerTask(*this,
                                              server.serverId))->schedule();
        }
    }
}

} // namespace RAMCloud
