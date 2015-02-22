#ifndef _BITS_MEMORY_POOL_H_
#define _BITS_MEMORY_POOL_H_

#include <cassert>
#include <cstdint>
#include <memory>
#include <vector>

struct MemoryPoolStats
{
    size_t block_count = 0;
    size_t allocation_count = 0;
};


namespace detail
{
/// Default index type, this dictates the maximum number of entries in a
/// single pool block.
typedef uint32_t index_t;

/// Base memory pool block. This contains a list of indices of free and used
/// entries and the storage for the entries themselves. Everything is allocated
/// in a single allocation in the static create function, and indices_begin()
/// and memory_begin() methods will return pointers offset from this for their
/// respective data.
template <typename T>
class MemoryPoolBlock
{
    /// Index of the first free entry
    index_t free_head_index_;
    const index_t entries_per_block_;

    MemoryPoolBlock(index_t entries_per_block);
    ~MemoryPoolBlock();

    MemoryPoolBlock(const MemoryPoolBlock &) = delete;
    MemoryPoolBlock & operator=(const MemoryPoolBlock &) = delete;

    /// returns start of indices
    index_t * indices_begin() const;

    /// returns start of pool memory
    T * memory_begin() const;

public:
    static MemoryPoolBlock<T> * create(uint32_t entries_per_block);
    static void destroy(MemoryPoolBlock<T> * ptr);

    /// Allocates a new object from this block. Returns nullptr if there is
    /// no available space.
    template <class... P>
    T * new_object(P&&... params);

    /// Deletes the given pointer. The pointer must be owned by this block.
    void delete_object(const T * ptr);

    /// Calls given function for all allocated entities
    template <typename F>
    void for_each(const F func) const;

    /// returns start of pool memory
    const T * memory_offset() const;

    /// Calculates the number of allocated entries
    index_t count_allocations() const;
};

} // namespace detail

/// FixedMemoryPool contains a single MemoryPoolBlock, it will not grow
/// beyond the max number of entries given at construction time.
template <typename T>
class FixedMemoryPool
{
public:
    typedef detail::index_t index_t;

    FixedMemoryPool(index_t max_entries);
    ~FixedMemoryPool();

    template<class... P>
    T * new_object(P&&... params);

    void delete_object(const T * ptr);

    template <typename F>
    void for_each(const F func) const;

    MemoryPoolStats get_stats() const;

private:
    typedef detail::MemoryPoolBlock<T> Block;
    Block * block_;

    FixedMemoryPool(const FixedMemoryPool &) = delete;
    FixedMemoryPool & operator=(const FixedMemoryPool &) = delete;
};


/// DynamicMemoryPool contains a dynamic array of MemoryPoolBlocks.
template <typename T>
class DynamicMemoryPool
{
public:
    typedef detail::index_t index_t;

    DynamicMemoryPool(index_t entries_per_block);
    ~DynamicMemoryPool();

    template<class... P>
    T * new_object(P&&... params);

    void delete_object(const T * ptr);

    template <typename F>
    void for_each(const F func) const;

    MemoryPoolStats get_stats() const;

private:
    typedef detail::MemoryPoolBlock<T> Block;

    /// The BlockInfo struct keeps regularly accessed block information
    /// packed together for better memory locality.
    struct BlockInfo
    {
        /// cache the number of free entries for this block
        index_t num_free_;
        /// cache the offset of entries memory from the start of the block
        const T * offset_;
        /// pointer to the block itself
        Block * block_;
    };

    /// storage for block info records
    BlockInfo * block_info_;
    /// number of blocks allocated
    index_t num_blocks_;
    /// index of the first block info with space
    index_t free_block_index_;
    /// the number of entries in each block
    const index_t entries_per_block_;

    /// Adds a new block and updates the free_block_index.
    BlockInfo * add_block();

    DynamicMemoryPool(const DynamicMemoryPool &) = delete;
    DynamicMemoryPool & operator=(const DynamicMemoryPool &) = delete;
};

namespace detail
{

const uint32_t MIN_BLOCK_ALIGN = 64;

void * aligned_malloc(size_t size, size_t align);
void aligned_free(void * ptr);

size_t align_to(size_t n, size_t align);

template <typename T>
MemoryPoolBlock<T> * MemoryPoolBlock<T>::create(index_t entries_per_block)
{
    // the header size
    const size_t header_size = sizeof(MemoryPoolBlock<T>);
    // extend indices size by alignment of T
    const size_t indices_size =
            align_to(sizeof(index_t) * entries_per_block, alignof(T));
    // align block to cache line size, or entry alignment if larger
    const size_t entries_size = sizeof(T) * entries_per_block;
    // block size includes indices + entry alignment + entries
    const size_t block_size = header_size + indices_size + entries_size;
    MemoryPoolBlock<T> * ptr = reinterpret_cast<MemoryPoolBlock<T>*>(
                aligned_malloc(block_size, MIN_BLOCK_ALIGN));
    if (ptr)
    {
        new (ptr) MemoryPoolBlock(entries_per_block);
    }
    assert(reinterpret_cast<uint8_t*>(ptr->indices_begin())
           == reinterpret_cast<uint8_t*>(ptr) + header_size);
    assert(reinterpret_cast<uint8_t*>(ptr->memory_begin())
           == reinterpret_cast<uint8_t*>(ptr) + header_size + indices_size);
    return ptr;
}

template <typename T>
void MemoryPoolBlock<T>::destroy(MemoryPoolBlock<T> * ptr)
{
    ptr->~MemoryPoolBlock();
    aligned_free(ptr);
}

template <typename T>
MemoryPoolBlock<T>::MemoryPoolBlock(index_t entries_per_block) :
    free_head_index_(0),
    entries_per_block_(entries_per_block)
{
    index_t * indices = indices_begin();
    for (index_t i = 0; i < entries_per_block; ++i)
    {
        indices[i] = i + 1;
    }
}

template <typename T>
MemoryPoolBlock<T>::~MemoryPoolBlock()
{
    assert(count_allocations() == 0);
}

template <typename T>
index_t * MemoryPoolBlock<T>::indices_begin() const
{
    return reinterpret_cast<index_t*>(
                const_cast<MemoryPoolBlock<T>*>(this + 1));
}

template <typename T>
T * MemoryPoolBlock<T>::memory_begin() const
{
    return reinterpret_cast<T*>(indices_begin() + entries_per_block_);
}

template <typename T>
const T * MemoryPoolBlock<T>::memory_offset() const
{
    return memory_begin();
}

template <typename T>
template <class... P>
T * MemoryPoolBlock<T>::new_object(P&&... params)
{
    // get the head of the free list
    const index_t index = free_head_index_;
    if (index != entries_per_block_)
    {
        index_t * indices = indices_begin();
        // assert that this index is not in use
        assert(indices[index] != index);
        // update head of the free list
        free_head_index_ = indices[index];
        // flag index as used
        indices[index] = index;
        T * ptr = memory_begin() + index;
        new (ptr) T(std::forward<P>(params)...);
        return ptr;
    }
    return nullptr;
}

template <typename T>
void MemoryPoolBlock<T>::delete_object(const T * ptr)
{
    if (ptr)
    {
        // assert that pointer is in range
        const T * begin = memory_begin();
        const T * end = begin + entries_per_block_;
        assert(ptr >= begin && ptr < end);

        // destruct this object
        ptr->~T();

        // get the index of this pointer
        const index_t index = ptr - begin;

        index_t * indices = indices_begin();

        // assert this index is allocated
        assert(indices[index] == index);

        // remove index from used list
        indices[index] = free_head_index_;

        // store index of next free entry in this pointer
        free_head_index_ = index;
    }
}

template <typename T>
template <typename F>
void MemoryPoolBlock<T>::for_each(const F func) const
{
    const index_t * indices = indices_begin();
    T * first = memory_begin();
    for (index_t i = 0, count = entries_per_block_; i != count; ++i)
    {
        if (indices[i] == i)
        {
            func(first + i);
        }
    }
}

template <typename T>
index_t MemoryPoolBlock<T>::count_allocations() const
{
    index_t num_allocations = 0;
    for_each([&num_allocations](const T *){ ++num_allocations; });
    return num_allocations;
}

} // namespace detail

template <typename T>
FixedMemoryPool<T>::FixedMemoryPool(index_t max_entries) :
    block_(Block::create(max_entries)) {}

template <typename T>
FixedMemoryPool<T>::~FixedMemoryPool()
{
    assert(get_stats().allocation_count == 0);
    Block::destroy(block_);
}

template <typename T>
template <class... P>
T * FixedMemoryPool<T>::new_object(P&&... params)
{
    return block_->new_object(std::forward<P>(params)...);
}

template <typename T>
void FixedMemoryPool<T>::delete_object(const T * ptr)
{
    block_->delete_object(ptr);
}

template <typename T>
template <typename F>
void FixedMemoryPool<T>::for_each(const F func) const
{
    block_->for_each(func);
}

template <typename T>
MemoryPoolStats FixedMemoryPool<T>::get_stats() const
{
    MemoryPoolStats stats;
    stats.block_count = 1;
    stats.allocation_count = 0;
    block_->for_each([&stats](T *) { ++stats.allocation_count; });
    return stats;
}

template <typename T>
DynamicMemoryPool<T>::DynamicMemoryPool(index_t entries_per_block) :
    block_info_(nullptr),
    num_blocks_(0),
    free_block_index_(0),
    entries_per_block_(entries_per_block)
{
    add_block();
}

template <typename T>
DynamicMemoryPool<T>::~DynamicMemoryPool()
{
    assert(get_stats().allocation_count == 0);
}

template <typename T>
typename DynamicMemoryPool<T>::BlockInfo * DynamicMemoryPool<T>::add_block()
{
    assert(free_block_index_ == num_blocks_);
    if (Block * block = Block::create(entries_per_block_))
    {
        // update the number of blocks
        num_blocks_++;
        // allocate space for new block info
        block_info_ = reinterpret_cast<BlockInfo*>(
                    realloc(block_info_, num_blocks_ * sizeof(BlockInfo)));
        // initialise the new block info structure
        BlockInfo & info = block_info_[free_block_index_];
        info.num_free_ = entries_per_block_;
        info.offset_ = const_cast<const Block*>(block)->memory_offset();
        info.block_ = block;
        return &info;
    }
    return nullptr;
}

template <typename T>
template <typename... P>
T * DynamicMemoryPool<T>::new_object(P&&... params)
{
    assert(free_block_index_ < num_blocks_);

    // search for a block with free space
    BlockInfo * p_info = block_info_ + free_block_index_;
    for (const BlockInfo * p_end = block_info_ + num_blocks_;
         p_info != p_end && p_info->num_free_ == 0; ++p_info) {}

    // update the free block index
    free_block_index_ = p_info - block_info_;

    // if no free blocks found then create a new one
    if (free_block_index_ == num_blocks_)
    {
        p_info = add_block();
        if (!p_info)
        {
            return nullptr;
        }
    }

    // construct the new object
    T * ptr = p_info->block_->new_object(std::forward<P>(params)...);
    assert(ptr != nullptr);
    // update num free count
    --p_info->num_free_;
    return ptr;
}

template <typename T>
void DynamicMemoryPool<T>::delete_object(const T * ptr)
{
    BlockInfo * p_info = block_info_;
    for (auto end = p_info + num_blocks_; p_info != end; ++p_info)
    {
        const T * p_entries_begin = p_info->offset_;
        const T * p_entries_end = p_entries_begin + entries_per_block_;
        if (ptr >= p_entries_begin && ptr < p_entries_end)
        {
            p_info->block_->delete_object(ptr);
            ++p_info->num_free_;
            const index_t free_block = p_info - block_info_;
            if (free_block < free_block_index_)
            {
                free_block_index_ = free_block;
            }
            return;
        }
    }
}

template <typename T>
template <typename F>
void DynamicMemoryPool<T>::for_each(const F func) const
{
    for (const BlockInfo * p_info = block_info_,
         * p_end = block_info_ + num_blocks_; p_info != p_end; ++p_info)
    {
        if (p_info->num_free_ < entries_per_block_)
        {
            p_info->block_->for_each(func);
        }
    }
}

template <typename T>
MemoryPoolStats DynamicMemoryPool<T>::get_stats() const
{
    MemoryPoolStats stats;
    stats.block_count = 0;
    stats.allocation_count = 0;
    for (const BlockInfo * p_info = block_info_,
         * p_end = block_info_ + num_blocks_; p_info != p_end; ++p_info)
    {
        ++stats.block_count;
        if (p_info->num_free_ < entries_per_block_)
        {
            stats.allocation_count += p_info->block_->count_allocations();
        }
    }
    return stats;
}

#endif // _BITS_MEMORY_POOL_H_

