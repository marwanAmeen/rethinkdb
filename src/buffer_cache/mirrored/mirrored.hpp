// Copyright 2010-2013 RethinkDB, all rights reserved.
#ifndef BUFFER_CACHE_MIRRORED_MIRRORED_HPP_
#define BUFFER_CACHE_MIRRORED_MIRRORED_HPP_

#include <algorithm>
#include <map>
#include <utility>
#include <vector>

#include "arch/types.hpp"
#include "buffer_cache/types.hpp"
#include "concurrency/access.hpp"
#include "concurrency/coro_fifo.hpp"
#include "concurrency/fifo_checker.hpp"
#include "concurrency/rwi_lock.hpp"
#include "concurrency/cond_var.hpp"
#include "concurrency/mutex.hpp"
#include "containers/intrusive_list.hpp"
#include "containers/scoped.hpp"
#include "buffer_cache/mirrored/config.hpp"
#include "buffer_cache/mirrored/stats.hpp"
#include "repli_timestamp.hpp"

#include "buffer_cache/mirrored/writeback.hpp"

#include "buffer_cache/mirrored/page_repl_random.hpp"

#include "buffer_cache/mirrored/free_list.hpp"

#include "buffer_cache/mirrored/page_map.hpp"


class mc_cache_account_t;

// evictable_t must go before array_map_t::local_buf_t, which
// references evictable_t's cache field.
class mc_inner_buf_t : public evictable_t,
                       private writeback_t::local_buf_t, /* This local_buf_t has state used by the writeback. */
                       public home_thread_mixin_debug_only_t {
    friend class mc_cache_t;
    friend class mc_transaction_t;
    friend class mc_buf_lock_t;
    friend class writeback_t;
    friend class writeback_t::local_buf_t;
    friend class page_repl_random_t;
    friend class array_map_t;

    typedef uint64_t version_id_t;

    class buf_snapshot_t;

    static const version_id_t faux_version_id = 0;  // this version id must be smaller than any valid version id

    // writeback_t::local_buf_t used to be a field, not a privately
    // inherited superclass, so this is the proper way to access its
    // fields.
    writeback_t::local_buf_t& writeback_buf() { return *this; }

    // Functions of the evictable_t interface.
    bool safe_to_unload();
    void unload();

    // Load an existing buf from disk
    mc_inner_buf_t(mc_cache_t *cache, block_id_t block_id, file_account_t *io_account);

    // Load an existing buf but use the provided data buffer (for read ahead)
    mc_inner_buf_t(mc_cache_t *cache, block_id_t block_id,
                   scoped_malloc_t<ser_buffer_t> &&buf,
                   const counted_t<standard_block_token_t>& token,
                   repli_timestamp_t recency_timestamp);

    // Create an entirely new buf
    static mc_inner_buf_t *allocate(mc_cache_t *cache, version_id_t snapshot_version, repli_timestamp_t recency_timestamp);
    mc_inner_buf_t(mc_cache_t *cache, block_id_t block_id, version_id_t snapshot_version, repli_timestamp_t recency_timestamp);

    ~mc_inner_buf_t();

    // Loads data from the serializer.
    void load_inner_buf(bool should_lock, file_account_t *io_account);

    // Informs us that a certain data buffer (whether the current one or one used by a
    // buf_snapshot_t) has been written back to disk; used by writeback
    void update_data_token(const void *data, const counted_t<standard_block_token_t>& token);

    // If required, make a snapshot of the data before being overwritten with new_version
    bool snapshot_if_needed(version_id_t new_version, bool leave_clone);
    // releases a buffer snapshot used by a transaction snapshot
    void release_snapshot(buf_snapshot_t *snapshot);
    // acquires the snapshot data buffer, loading from disk if necessary; must be matched by a call
    // to release_snapshot_data to keep track of when data buffer is in use
    void *acquire_snapshot_data(version_id_t version_to_access, file_account_t *io_account, repli_timestamp_t *recency_out,
                                block_size_t *block_size_out);
    void release_snapshot_data(void *data);

private:
    // Initializes an mc_inner_buf_t for use with a new block.
    // This is used by allocate() and the new buf constructor mc_inner_buf_t(cache, block_id, snapshot_version, recency_timestamp)
    void initialize_to_new(version_id_t snapshot_version, repli_timestamp_t recency_timestamp);

    // Our block's block id.
    block_id_t block_id;

    // The subtree recency value associated with our block.
    repli_timestamp_t subtree_recency;

    // The data for the block.
    block_size_t block_size;
    serializer_data_ptr_t data;
    // The snapshot version id of the block.
    version_id_t version_id;
    /* As long as data has not been changed since the last serializer write, data_token contains a token to the on-serializer block */
    counted_t<standard_block_token_t> data_token;

    // A lock for loading the block.
    rwi_lock_t lock;

    // The number of mc_buf_lock_ts that exist for this mc_inner_buf_t.
    unsigned int refcount;

    // true if this block is to be deleted.
    bool do_delete;

    // number of references from mc_buf_lock_t buffers, which hold a
    // pointer to the data in read_outdated_ok mode.
    size_t cow_refcount;

    // number of references from mc_buf_lock_t buffers which point to the current version of `data` as a
    // snapshot. this is ugly, but necessary to correctly initialize buf_snapshot_t refcounts.
    size_t snap_refcount;

    // snapshot types' implementations are internal and deferred to mirrored.cc
    intrusive_list_t<buf_snapshot_t> snapshots;

    DISABLE_COPYING(mc_inner_buf_t);
};

struct i_am_writeback_t { };

// A mc_buf_lock_t acquires and holds an mc_inner_buf_t.  Make sure you call
// release() as soon as it's feasible to do so.  The destructor will
// release the mc_inner_buf_t, so don't worry!
class mc_buf_lock_t : public home_thread_mixin_t {
public:
    mc_buf_lock_t(mc_transaction_t *txn, block_id_t block_id, access_t mode,
            buffer_cache_order_mode_t order_mode = buffer_cache_order_mode_check,
            lock_in_line_callback_t *call_when_in_line = 0) THROWS_NOTHING;
    explicit mc_buf_lock_t(mc_transaction_t *txn) THROWS_NOTHING; // Constructor used to allocate a new block
    mc_buf_lock_t();
    ~mc_buf_lock_t();

    // Swaps this mc_buf_lock_t with another, thus obeying RAII since one
    // mc_buf_lock_t owns up to one mc_inner_buf_t at a time.
    void swap(mc_buf_lock_t& swapee);

    // Releases the buf.  You can only release once (unless you swap
    // in an unreleased mc_buf_lock_t).
    void release();

    // Releases the buf, if it was acquired.
    void release_if_acquired();

    bool is_acquired() const;

    // Get the data buffer for reading
    const void *get_data_read() const;

    // Gets data for writing, also means the block will have to be flushed.  Sets the
    // block size to the full maximum block size for the serializer.
    void *get_data_write();

    // Gets data for writing, also means the block will have to be flushed.  Sets the
    // block size to the specified cache_block_size THIS TIME ONLY!  If you call
    // get_data_write() with no argument later, it'll get reset back to the full block
    // size.
    void *get_data_write(uint32_t cache_block_size);

    uint32_t cache_block_size() const { return block_size.value(); }

    block_id_t get_block_id() const;

    bool is_deleted() const;
    void mark_deleted();

    eviction_priority_t get_eviction_priority() const;
    void set_eviction_priority(eviction_priority_t val);

    repli_timestamp_t get_recency() const;
    void touch_recency(repli_timestamp_t timestamp);

private:
    friend class mc_cache_t;
    friend class mc_transaction_t;
    friend class writeback_t;
    friend class writeback_t::buf_writer_t;

    // Internal functions used during construction
    void initialize(mc_inner_buf_t::version_id_t version, file_account_t *io_account, lock_in_line_callback_t *call_when_in_line);
    void acquire_block(mc_inner_buf_t::version_id_t version_to_access);

    // True if this is an mc_buf_lock_t for a snapshotted view of the buf.
    bool acquired;
    bool snapshotted;
    bool non_locking_access;

    // Used for the pm_bufs_held perfmon.
    ticks_t start_time;

    // Presumably, the mode with which this mc_buf_lock_t holds the inner buf.
    access_t mode;

    // Our pointer to an inner_buf -- we have a bunch of mc_buf_lock_t's
    // all pointing at an inner buf.
    mc_inner_buf_t *inner_buf;

    // Usually the same as inner_buf->data. If a COW happens or this
    // mc_buf_lock_t is part of a snapshotted transaction, it reference a
    // different buffer however.
    block_size_t block_size;
    void *data;

    // Similarly, usually the same as inner_buf->subtree_recency.  If
    // a COW happens or this mc_buf_lock_t is part of a snapshotted
    // transaction, it may have a different value.
    repli_timestamp_t subtree_recency;

    // Used solely for asserting (with guarantee, in release mode!)
    // that there are no acquired buf locks upon destruction of the
    // transaction.
    mc_transaction_t *parent_transaction;

    DISABLE_COPYING(mc_buf_lock_t);
};

struct write_token_pair_t;

/* Transaction class. */
class mc_transaction_t :
    public home_thread_mixin_debug_only_t
{
    friend class mc_buf_lock_t;
    friend class mc_cache_t;
    friend class writeback_t;

public:
    mc_transaction_t(mc_cache_t *cache, access_t access, int expected_change_count, repli_timestamp_t recency_timestamp, order_token_t order_token, write_durability_t durability);

    // For read transactions.
    mc_transaction_t(mc_cache_t *cache, access_t access, order_token_t order_token);

    mc_transaction_t(mc_cache_t *cache, access_t access, i_am_writeback_t i_am_writeback);
    ~mc_transaction_t();

    mc_cache_t *get_cache() const { return cache; }
    access_t get_access() const { return access; }

    void get_subtree_recencies(block_id_t *block_ids, size_t num_block_ids, repli_timestamp_t *recencies_out, get_subtree_recencies_callback_t *cb);

    // This just sets the snapshotted flag, we finalize the snapshot as soon as the first block has been acquired (see finalize_version() )
    void snapshot();

    void set_account(mc_cache_account_t *cache_account);

    void set_token_pair(write_token_pair_t *_token_pair);

private:
    void register_buf_snapshot(mc_inner_buf_t *inner_buf, mc_inner_buf_t::buf_snapshot_t *snap);

    // If not done before, sets snapshot_version, if in snapshotted mode also registers the snapshot
    void maybe_finalize_version();

    file_account_t *get_io_account() const;

    // Note: Make sure that no automatic destructors do anything
    // interesting, they could get run on the WRONG THREAD!
    mc_cache_t *const cache;

    ticks_t start_time;
    const int expected_change_count;
    access_t access;
    repli_timestamp_t recency_timestamp;
    mc_inner_buf_t::version_id_t snapshot_version;
    bool snapshotted;

    mc_cache_account_t *cache_account;

    std::vector<std::pair<mc_inner_buf_t*, mc_inner_buf_t::buf_snapshot_t*> > owned_buf_snapshots;

    int64_t num_buf_locks_acquired;

    const bool is_writeback_transaction;

    const write_durability_t durability;

    write_token_pair_t *token_pair; /* Used in assertions. */

    DISABLE_COPYING(mc_transaction_t);
};

class mc_cache_account_t {
public:
    ~mc_cache_account_t();
private:
    friend class mc_cache_t;
    friend class mc_transaction_t;
    mc_cache_account_t(int thread, file_account_t *io_account);
    int thread_;
    file_account_t *io_account_;
    DISABLE_COPYING(mc_cache_account_t);
};

class mc_cache_t : public home_thread_mixin_t, public serializer_read_ahead_callback_t {
    friend class mc_inner_buf_t;
    friend class mc_buf_lock_t;
    friend class mc_transaction_t;
    friend class writeback_t;
    friend class writeback_t::local_buf_t;
    friend class page_repl_random_t;
    friend class evictable_t;
    friend class array_map_t;

public:
    typedef mc_buf_lock_t buf_lock_type;
    typedef mc_transaction_t transaction_type;
    typedef mc_cache_account_t cache_account_type;

    static void create(serializer_t *serializer);
    mc_cache_t(serializer_t *serializer, const mirrored_cache_config_t &dynamic_config, perfmon_collection_t *);
    ~mc_cache_t();

    block_size_t get_block_size() const;

    // TODO: Come up with a consistent priority scheme, i.e. define a "default" priority etc.
    // TODO: As soon as we can support it, we might consider supporting a mem_cap paremeter.
    void create_cache_account(int priority, scoped_ptr_t<mc_cache_account_t> *out);

    bool contains_block(block_id_t block_id);

    unsigned int num_blocks();

    mc_inner_buf_t::version_id_t get_current_version_id() { return next_snapshot_version; }

    // must be O(1)
    mc_inner_buf_t::version_id_t get_min_snapshot_version(mc_inner_buf_t::version_id_t default_version) const {
        return no_active_snapshots() ? default_version : active_snapshots.begin()->first;
    }
    // must be O(1)
    mc_inner_buf_t::version_id_t get_max_snapshot_version(mc_inner_buf_t::version_id_t default_version) const {
        return no_active_snapshots() ? default_version : active_snapshots.rbegin()->first;
    }

    void register_snapshot(mc_transaction_t *txn);
    void unregister_snapshot(mc_transaction_t *txn);

private:
    bool no_active_snapshots() const { return active_snapshots.empty(); }
    bool no_active_snapshots(mc_inner_buf_t::version_id_t from_version, mc_inner_buf_t::version_id_t to_version) const {
        return active_snapshots.lower_bound(from_version) == active_snapshots.upper_bound(to_version);
    }

    size_t register_buf_snapshot(mc_inner_buf_t *inner_buf, mc_inner_buf_t::buf_snapshot_t *snap, mc_inner_buf_t::version_id_t snapshotted_version, mc_inner_buf_t::version_id_t new_version);
    size_t calculate_snapshots_affected(mc_inner_buf_t::version_id_t snapshotted_version, mc_inner_buf_t::version_id_t new_version);

    mc_inner_buf_t *find_buf(block_id_t block_id);
    void on_transaction_commit(mc_transaction_t *txn);

public:
    void offer_read_ahead_buf(block_id_t block_id,
                              scoped_malloc_t<ser_buffer_t> *buf,
                              const counted_t<standard_block_token_t>& token,
                              repli_timestamp_t recency_timestamp);

private:
    // Takes ownership of buf.
    void offer_read_ahead_buf_home_thread(
            block_id_t block_id,
            ser_buffer_t *buf,
            const counted_t<standard_block_token_t> &token,
            repli_timestamp_t recency_timestamp);
    bool can_read_ahead_block_be_accepted(block_id_t block_id);
    void maybe_unregister_read_ahead_callback();

public:
    coro_fifo_t& co_begin_coro_fifo() { return co_begin_coro_fifo_; }

private:
    mirrored_cache_config_t dynamic_config; // Local copy of our initial configuration

    // TODO: how do we design communication between cache policies?
    // Should they all have access to the cache, or should they only
    // be given access to each other as necessary? The first is more
    // flexible as anyone can access anyone else, but encourages too
    // many dependencies. The second is more strict, but might not be
    // extensible when some policy implementation requires access to
    // components it wasn't originally given.

    serializer_t *serializer;
    scoped_ptr_t<mc_cache_stats_t> stats;

    // We use a separate IO account for reads and writes, so reads can pass ahead
    // of active writebacks. Otherwise writebacks could badly block out readers,
    // thereby blocking user queries.
    scoped_ptr_t<file_account_t> reads_io_account;
    scoped_ptr_t<file_account_t> writes_io_account;

    array_map_t page_map;
    page_repl_random_t page_repl;
    writeback_t writeback;
    array_free_list_t free_list;

    // More fields
    bool shutting_down;

    // Used to keep track of how many transactions there are so that
    // we can wait for transactions to complete before shutting down,
    // and assert that there are no non-writeback transactions when
    // the cache destructor is called.
    int num_live_writeback_transactions;
    int num_live_non_writeback_transactions;

    cond_t *to_pulse_when_last_transaction_commits;

    bool read_ahead_registered;

    std::map<mc_inner_buf_t::version_id_t, mc_transaction_t *> active_snapshots;
    mc_inner_buf_t::version_id_t next_snapshot_version;

    coro_fifo_t co_begin_coro_fifo_;

    DISABLE_COPYING(mc_cache_t);
};

#endif // BUFFER_CACHE_MIRRORED_MIRRORED_HPP_
