#pragma once

#include <chrono>
#include <cstddef>
#include <memory>
#include <mutex>
#include <optional>
#include <stdexcept>
#include <unordered_map>
#include <utility>

#include "llmgate/util/clock.h"

namespace llmgate {

// A bounded cache that evicts the Least Recently Used entry when full, and
// treats entries older than `ttl` as missing. Thread-safe (one mutex).
//
// How it stays O(1):
//   * map_  : key -> Node*          finds an entry in O(1)
//   * list  : doubly linked list    keeps entries ordered by recency
//             head_ = most recently used, tail_ = least recently used
// Every get/put moves the touched node to the front. When the cache is full,
// the node at the tail is the one to evict.
//
// The TTL is counted from the last put() of a key. get() does NOT extend it.
template <typename K, typename V>
class LruCache {
public:
    LruCache(std::size_t capacity, std::chrono::milliseconds ttl, Clock clock)
        : capacity_(capacity), ttl_(ttl), clock_(std::move(clock)) {
        if (capacity_ == 0) {
            throw std::invalid_argument("LruCache: capacity must be > 0");
        }
        if (ttl_ <= std::chrono::milliseconds::zero()) {
            throw std::invalid_argument("LruCache: ttl must be > 0");
        }
    }

    ~LruCache() {
        Node* n = head_;
        while (n != nullptr) {
            Node* next = n->next;
            delete n;
            n = next;
        }
    }

    // Non-copyable: the list owns raw pointers.
    LruCache(const LruCache&) = delete;
    LruCache& operator=(const LruCache&) = delete;

    // Returns the value, or nullopt if the key is absent or expired.
    std::optional<V> get(const K& key) {
        std::lock_guard<std::mutex> lock(mu_);
        auto it = map_.find(key);
        if (it == map_.end()) {
            return std::nullopt;
        }
        Node* n = it->second;
        if (clock_() >= n->expires_at) {  // expired: remove it, report a miss
            remove_node(n);
            return std::nullopt;
        }
        unlink(n);       // touched: move to the front (most recently used)
        push_front(n);
        return n->value;
    }

    // Inserts or updates. May evict the least recently used entry.
    void put(const K& key, V value) {
        std::lock_guard<std::mutex> lock(mu_);
        const TimePoint expires_at = clock_() + ttl_;

        auto it = map_.find(key);
        if (it != map_.end()) {  // update in place
            Node* n = it->second;
            n->value = std::move(value);
            n->expires_at = expires_at;
            unlink(n);
            push_front(n);
            return;
        }

        if (map_.size() >= capacity_) {
            evict_lru();
        }
        // unique_ptr keeps this exception-safe: if emplace throws, the node
        // is freed automatically instead of leaking.
        std::unique_ptr<Node> node(new Node{key, std::move(value), expires_at});
        map_.emplace(key, node.get());
        push_front(node.release());
    }

    // Number of stored entries. May include expired entries that have not
    // been touched since they expired (they are removed lazily).
    std::size_t size() const {
        std::lock_guard<std::mutex> lock(mu_);
        return map_.size();
    }

private:
    struct Node {
        K key;
        V value;
        TimePoint expires_at;
        Node* prev = nullptr;
        Node* next = nullptr;
    };

    // All private helpers assume mu_ is already held.

    // Detach a node from the list (does not delete it).
    void unlink(Node* n) {
        if (n->prev != nullptr) {
            n->prev->next = n->next;
        } else {
            head_ = n->next;
        }
        if (n->next != nullptr) {
            n->next->prev = n->prev;
        } else {
            tail_ = n->prev;
        }
        n->prev = nullptr;
        n->next = nullptr;
    }

    // Attach a detached node at the front of the list.
    void push_front(Node* n) {
        n->prev = nullptr;
        n->next = head_;
        if (head_ != nullptr) {
            head_->prev = n;
        }
        head_ = n;
        if (tail_ == nullptr) {
            tail_ = n;
        }
    }

    // Remove a node completely: list, map, and memory.
    void remove_node(Node* n) {
        unlink(n);
        map_.erase(n->key);  // must happen before delete: it reads n->key
        delete n;
    }

    void evict_lru() {
        if (tail_ != nullptr) {
            remove_node(tail_);
        }
    }

    std::size_t capacity_;
    std::chrono::milliseconds ttl_;
    Clock clock_;
    mutable std::mutex mu_;
    std::unordered_map<K, Node*> map_;
    Node* head_ = nullptr;  // most recently used
    Node* tail_ = nullptr;  // least recently used
};

}  // namespace llmgate