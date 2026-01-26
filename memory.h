#ifndef UMA_MEMORY_H
#define UMA_MEMORY_H

#include <cstdint>
#include <cstddef>
#include <atomic>
#include <new>
#include <type_traits>
#include <array>
#include <thread>
#include <optional>
#include <utility>
#include <vector>


class GlobalGC;
class TaggedPtr {
    uint64_t bits_;

public:
    static constexpr uint64_t ADDR_MASK = 0x0000FFFFFFFFFFFF;
    static constexpr uint64_t META_MASK = ~ADDR_MASK;
    static constexpr int GEN_SHIFT = 52;
    static constexpr int STATE_SHIFT = 49;

    enum State : uint8_t {
        LOCAL = 0,
        SHARED = 1,
        GC_CANDIDATE = 2,
        PINNED = 3
    };

    TaggedPtr() : bits_(0) {}
    explicit TaggedPtr(void* ptr, uint8_t type_id = 0, uint8_t gen = 0)
        : bits_(reinterpret_cast<uint64_t>(ptr) |
            (static_cast<uint64_t>(type_id) << 56) |
            (static_cast<uint64_t>(gen) << GEN_SHIFT)) {
    }

    void* ptr() const { return reinterpret_cast<void*>(bits_ & ADDR_MASK); }
    uint8_t generation() const { return (bits_ >> GEN_SHIFT) & 0xF; }
    uint8_t type_id() const { return bits_ >> 56; }
    State state() const { return static_cast<State>((bits_ >> STATE_SHIFT) & 0x7); }

    TaggedPtr with_gen(uint8_t g) const {
        TaggedPtr t;
        t.bits_ = (bits_ & ~(0xFULL << GEN_SHIFT)) | (static_cast<uint64_t>(g) << GEN_SHIFT);
        return t;
    }

    TaggedPtr with_state(State s) const {
        TaggedPtr t;
        t.bits_ = (bits_ & ~(0x7ULL << STATE_SHIFT)) | (static_cast<uint64_t>(s) << STATE_SHIFT);
        return t;
    }

    bool cas(TaggedPtr expected, TaggedPtr desired) {
        return std::atomic_compare_exchange_strong(
            reinterpret_cast<std::atomic<uint64_t>*>(&bits_),
            reinterpret_cast<uint64_t*>(&expected.bits_),
            desired.bits_
        );
    }
};

class ThreadLocalHeap {
    struct Block {
        char* bump;
        char* end;
        char memory[1024 * 1024];
    };

    Block* current_;
    std::vector<Block*> full_blocks_;

    struct SideEntry {
        uint32_t size;
        uint16_t ref_count;
        uint16_t weak_count;
        void(*drop_fn)(void*);
    };

    static constexpr size_t SIDE_TABLE_SIZE = 65536;
    std::array<SideEntry, SIDE_TABLE_SIZE> side_table_;

    struct DeathEntry {
        void* ptr;
        size_t size;
        uint8_t gen;
    };
    std::vector<DeathEntry> death_row_;

public:
    ThreadLocalHeap();

    template<size_t Size, size_t Align = 8>
    void* allocate_fast() {
        static_assert(Size <= 1024 * 512, "Use slow path for large objects");

        char* addr = current_->bump;
        char* new_bump = addr + ((Size + Align - 1) & ~(Align - 1));

        if (new_bump <= current_->end) [[likely]] {
            current_->bump = new_bump;
            return addr;
        }
        return allocate_slow(Size, Align);
    }

    void deallocate(void* ptr, size_t size);
    void safepoint();

private:
    void* allocate_slow(size_t size, size_t align);
    void scavenge();
    void reuse_young(void* ptr, size_t size);
    void flush_death_row();

    SideEntry* get_side_entry(void* ptr) {
        size_t idx = (reinterpret_cast<uintptr_t>(ptr) >> 4) & (SIDE_TABLE_SIZE - 1);
        return &side_table_[idx];
    }
};

class GlobalGC {
    static std::atomic<uint64_t> global_epoch_;

public:
    static void mark(TaggedPtr ptr) {
        auto new_ptr = ptr.with_state(TaggedPtr::GC_CANDIDATE);
        (void)ptr.cas(ptr, new_ptr);
    }

    static void submit_old(void* ptr) {
        // 加入全局老年代队列
        (void)ptr; // 简化实现
    }

    static void register_root(void* ptr) {
        (void)ptr; // 简化
    }

    static void background_reclaim() {
        while (true) {
            std::this_thread::sleep_for(std::chrono::seconds(1));
        }
    }
};

inline ThreadLocalHeap::ThreadLocalHeap() {
    current_ = new Block();
    current_->bump = current_->memory;
    current_->end = current_->memory + sizeof(current_->memory);
}

inline void ThreadLocalHeap::deallocate(void* ptr, size_t size) {
    auto entry = get_side_entry(ptr);
    entry->ref_count--;

    if (entry->ref_count == 0) {
        if (entry->drop_fn) entry->drop_fn(ptr);

        TaggedPtr tp(ptr);
        death_row_.push_back({ ptr, size, tp.generation() });

        if (death_row_.size() > 1000) {
            flush_death_row();
        }
    }
}

inline void ThreadLocalHeap::safepoint() {
    if (!death_row_.empty()) {
        flush_death_row();
    }
    scavenge();
}

inline void ThreadLocalHeap::flush_death_row() {
    for (auto& e : death_row_) {
        if (e.gen < 3) {
            reuse_young(e.ptr, e.size);
        }
        else {
            GlobalGC::submit_old(e.ptr);
        }
    }
    death_row_.clear();
}

inline void* ThreadLocalHeap::allocate_slow(size_t size, size_t align) {
    (void)align;
    // 分配新块
    full_blocks_.push_back(current_);
    current_ = new Block();
    current_->bump = current_->memory;
    current_->end = current_->memory + sizeof(current_->memory);

    char* addr = current_->bump;
    current_->bump += size;
    return addr;
}

inline void ThreadLocalHeap::scavenge() {
    // 实现晋升逻辑
}

inline void ThreadLocalHeap::reuse_young(void* ptr, size_t size) {
    (void)ptr;
    (void)size;
    // 实现内存复用
}

class EpochReclaimer {
    static constexpr size_t EPOCH_COUNT = 3;

    struct RetiredPtr {
        void* ptr;
        void(*deleter)(void*);
        uint64_t retired_epoch;
    };

    alignas(64) static inline std::atomic<uint64_t> current_epoch_{ 0 };
    static inline std::array<std::vector<RetiredPtr>, EPOCH_COUNT> global_retired_;

public:
    static void retire(void* ptr, void(*del)(void*) = default_delete) {
        auto epoch = current_epoch_.load(std::memory_order_relaxed);
        global_retired_[epoch % EPOCH_COUNT].push_back({ ptr, del, epoch });
    }

    static void safepoint() {
        static thread_local uint64_t local_count = 0;
        if (++local_count % 256 != 0) return;

        auto global = current_epoch_.load(std::memory_order_acquire);
        if (can_advance_epoch()) {
            auto reclaim_epoch = (global + EPOCH_COUNT - 1) % EPOCH_COUNT;

            for (auto& r : global_retired_[reclaim_epoch]) {
                r.deleter(r.ptr);
            }
            global_retired_[reclaim_epoch].clear();

            current_epoch_.store(global + 1, std::memory_order_release);
        }
    }

private:
    static bool can_advance_epoch() { return true; }
    static void default_delete(void* p) { ::operator delete(p); }
};


template<typename T>
struct TypeRegistry {
    static uint8_t id() { return 0; } // 简化技术债
};

template<typename T, uint8_t Strategy>
class UniPtr {
    TaggedPtr ptr_;

    static constexpr bool IS_UNIQUE = (Strategy & 0x1);
    static constexpr bool IS_GC = (Strategy & 0x2);
    static constexpr bool IS_EPOCH = (Strategy & 0x4);

public:
    explicit UniPtr(T* raw) {
        if constexpr (IS_UNIQUE) {
            ptr_ = TaggedPtr(raw, TypeRegistry<T>::id(), 0).with_state(TaggedPtr::LOCAL);
        }
        else if constexpr (IS_GC) {
            ptr_ = TaggedPtr(raw, TypeRegistry<T>::id(), 0).with_state(TaggedPtr::SHARED);
        }
    }

    ~UniPtr() {
        if constexpr (IS_UNIQUE) {
            ThreadLocalHeap::current().deallocate(ptr_.ptr(), sizeof(T));
        }
        else if constexpr (IS_EPOCH) {
            EpochReclaimer::retire(ptr_.ptr());
        }
    }

    T* operator->() const { return static_cast<T*>(ptr_.ptr()); }
    T& operator*() const { return *static_cast<T*>(ptr_.ptr()); }

    void set_field(size_t offset, UniPtr& other) {
        if constexpr (IS_GC) {
            write_barrier(ptr_.ptr(), other.ptr_.ptr());
        }
        *reinterpret_cast<void**>(static_cast<char*>(ptr_.ptr()) + offset) = other.ptr_.ptr();
    }

private:
    static void write_barrier(void* obj, void* ref) {
        TaggedPtr tp_obj(obj), tp_ref(ref);
        if (tp_obj.generation() > tp_ref.generation()) {
            // RememberedSet::insert(obj, ref);
        }
    }
};

template<typename T> using Box = UniPtr<T, 0x1>;
template<typename T> using Gc = UniPtr<T, 0x2>;
template<typename T> using Arc = UniPtr<T, 0x5>;

class Allocator {
public:
    static ThreadLocalHeap& current() {
        thread_local ThreadLocalHeap heap;
        return heap;
    }

    template<typename T, typename... Args>
    static auto make_box(Args&&... args) -> Box<T> {
        void* mem = current().allocate_fast<sizeof(T), alignof(T)>();
        new (mem) T(std::forward<Args>(args)...);
        return Box<T>(static_cast<T*>(mem));
    }

    template<typename T, typename... Args>
    static auto make_gc(Args&&... args) -> Gc<T> {
        void* mem = current().allocate_fast<sizeof(T), alignof(T)>();
        new (mem) T(std::forward<Args>(args)...);
        GlobalGC::register_root(mem);
        return Gc<T>(static_cast<T*>(mem));
    }
};

#endif // UMA_MEMORY_H