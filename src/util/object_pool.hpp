#ifndef EXSIM_UTIL_OBJECT_POOL_HPP
#define EXSIM_UTIL_OBJECT_POOL_HPP

#include <cstddef>
#include <memory>
#include <new>
#include <type_traits>
#include <utility>

namespace exsim::util {

// Fixed-capacity free-list pool. The constructor allocates every node in one
// contiguous block, so create() and destroy() never allocate. Live objects
// are released with the block, so T must not need its destructor run.
template <typename T>
class ObjectPool {
    static_assert(std::is_trivially_destructible_v<T>, "the pool block is freed without running destructors");

public:
    explicit ObjectPool(std::size_t capacity) : nodes_(new Node[capacity]) {
        // Linked back to front so create() hands nodes out in address order.
        for (std::size_t i = capacity; i-- > 0;) {
            nodes_[i].next = free_list_;
            free_list_ = &nodes_[i];
        }
    }

    ObjectPool(const ObjectPool&) = delete;
    ObjectPool& operator=(const ObjectPool&) = delete;

    // Returns nullptr when every node is in use.
    template <typename... Args>
    T* create(Args&&... args) {
        if (!free_list_) [[unlikely]] {
            return nullptr;
        }

        Node* node = free_list_;
        free_list_ = free_list_->next;
        try {
            return ::new (static_cast<void*>(node->storage)) T(std::forward<Args>(args)...);
        } catch (...) {
            node->next = free_list_;
            free_list_ = node;
            throw;
        }
    }

    void destroy(T* object) noexcept {
        if (!object) {
            return;
        }

        object->~T();
        // The Node union whose storage holds `object` is still alive, so launder
        // yields a usable pointer to it at the same address.
        Node* node = std::launder(reinterpret_cast<Node*>(object));
        node->next = free_list_;
        free_list_ = node;
    }

private:
    union Node {
        alignas(T) unsigned char storage[sizeof(T)];
        Node* next;
    };

    std::unique_ptr<Node[]> nodes_;
    Node* free_list_ = nullptr;
};

}  // namespace exsim::util

#endif
