#ifndef EXSIM_UTIL_OBJECT_POOL_HPP
#define EXSIM_UTIL_OBJECT_POOL_HPP

#include <cstddef>
#include <memory>
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
    T* create(Args&&... args) noexcept {
        static_assert(std::is_nothrow_constructible_v<T, Args...>, "a throwing constructor would lose its node");
        if (!free_list_) [[unlikely]] {
            return nullptr;
        }

        Node* node = free_list_;
        free_list_ = free_list_->next;
        // Constructing the member makes it the union's active member.
        return std::construct_at(std::addressof(node->value), std::forward<Args>(args)...);
    }

    void destroy(T* object) noexcept {
        if (!object) {
            return;
        }

        // A union and its live member are pointer-interconvertible, so take the
        // node before ending the object's lifetime. Assigning `next` then
        // makes it the active member again.
        Node* node = reinterpret_cast<Node*>(object);
        std::destroy_at(object);
        node->next = free_list_;
        free_list_ = node;
    }

private:
    // A free node holds the free-list link; a used node holds the object.
    union Node {
        Node* next;
        T value;
        Node() noexcept : next(nullptr) {}
    };

    std::unique_ptr<Node[]> nodes_;
    Node* free_list_ = nullptr;
};

}  // namespace exsim::util

#endif
