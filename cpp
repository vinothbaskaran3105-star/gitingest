// lock_free_priority_queue.hpp
#ifndef LOCK_FREE_PRIORITY_QUEUE_HPP
#define LOCK_FREE_PRIORITY_QUEUE_HPP

#include <atomic>
#include <memory>
#include <optional>
#include <vector>
#include <algorithm>
#include <thread>
#include <cstdint>

template<typename T>
class LockFreePriorityQueue {
private:
    struct Node {
        T value;
        int priority;
        std::atomic<Node*> next;
        
        Node(T val, int prio) : value(std::move(val)), priority(prio), next(nullptr) {}
    };

    // Heap-based array structure for lock-free priority queue
    struct HeapArray {
        struct HeapItem {
            T value;
            int priority;
            std::atomic<bool> valid;
            
            HeapItem() : valid(false) {}
            HeapItem(T val, int prio) : value(std::move(val)), priority(prio), valid(true) {}
        };
        
        std::atomic<size_t> size;
        std::vector<std::atomic<HeapItem*>> heap;
        std::atomic<size_t> capacity;
        
        HeapArray(size_t cap) : size(0), capacity(cap) {
            heap.resize(cap);
            for (size_t i = 0; i < cap; ++i) {
                heap[i].store(nullptr, std::memory_order_relaxed);
            }
        }
        
        ~HeapArray() {
            for (size_t i = 0; i < capacity.load(); ++i) {
                HeapItem* item = heap[i].load();
                if (item) delete item;
            }
        }
    };

    std::unique_ptr<HeapArray> heap_array;
    std::atomic<HeapArray*> current_heap;
    
    // Helper functions for heap operations
    size_t parent(size_t i) { return (i - 1) / 2; }
    size_t left_child(size_t i) { return 2 * i + 1; }
    size_t right_child(size_t i) { return 2 * i + 2; }
    
    bool heapify_up(size_t index) {
        while (index > 0) {
            size_t p = parent(index);
            HeapArray::HeapItem* child_item = heap_array->heap[index].load();
            HeapArray::HeapItem* parent_item = heap_array->heap[p].load();
            
            if (!child_item || !child_item->valid || !parent_item || !parent_item->valid) {
                return false;
            }
            
            if (child_item->priority > parent_item->priority) {
                // Try to swap child and parent
                if (heap_array->heap[index].compare_exchange_strong(child_item, parent_item) &&
                    heap_array->heap[p].compare_exchange_strong(parent_item, child_item)) {
                    index = p;
                } else {
                    return false;
                }
            } else {
                break;
            }
        }
        return true;
    }
    
    bool heapify_down(size_t index, size_t heap_size) {
        while (true) {
            size_t left = left_child(index);
            size_t right = right_child(index);
            size_t largest = index;
            
            HeapArray::HeapItem* current_item = heap_array->heap[index].load();
            if (!current_item || !current_item->valid) return false;
            
            // Check left child
            if (left < heap_size) {
                HeapArray::HeapItem* left_item = heap_array->heap[left].load();
                if (left_item && left_item->valid && left_item->priority > current_item->priority) {
                    largest = left;
                    current_item = left_item;
                }
            }
            
            // Check right child
            if (right < heap_size) {
                HeapArray::HeapItem* right_item = heap_array->heap[right].load();
                HeapArray::HeapItem* largest_item = heap_array->heap[largest].load();
                if (right_item && right_item->valid && 
                    right_item->priority > (largest_item ? largest_item->priority : -1)) {
                    largest = right;
                }
            }
            
            if (largest != index) {
                HeapArray::HeapItem* index_item = heap_array->heap[index].load();
                HeapArray::HeapItem* largest_item = heap_array->heap[largest].load();
                
                if (!index_item || !index_item->valid || !largest_item || !largest_item->valid) {
                    return false;
                }
                
                if (heap_array->heap[index].compare_exchange_strong(index_item, largest_item) &&
                    heap_array->heap[largest].compare_exchange_strong(largest_item, index_item)) {
                    index = largest;
                } else {
                    return false;
                }
            } else {
                break;
            }
        }
        return true;
    }

public:
    LockFreePriorityQueue(size_t initial_capacity = 1024) {
        heap_array = std::make_unique<HeapArray>(initial_capacity);
        current_heap.store(heap_array.get());
    }
    
    ~LockFreePriorityQueue() {
        current_heap.store(nullptr);
    }
    
    // Insert operation (enqueue)
    bool enqueue(T value, int priority) {
        while (true) {
            HeapArray* heap = current_heap.load();
            size_t current_size = heap->size.load();
            size_t cap = heap->capacity.load();
            
            if (current_size >= cap) {
                // Resize operation would go here (implemented with CAS)
                // For simplicity, we'll just fail if full
                return false;
            }
            
            // Try to increment size
            if (heap->size.compare_exchange_weak(current_size, current_size + 1)) {
                // Create new item
                auto new_item = new HeapArray::HeapItem(std::move(value), priority);
                
                // Place item at the end
                size_t index = current_size;
                HeapArray::HeapItem* expected = nullptr;
                if (heap->heap[index].compare_exchange_strong(expected, new_item)) {
                    // Heapify up
                    heapify_up(index);
                    return true;
                } else {
                    delete new_item;
                    heap->size.fetch_sub(1);  // Rollback size increment
                }
            }
        }
    }
    
    // Extract operation (dequeue)
    std::optional<T> dequeue() {
        while (true) {
            HeapArray* heap = current_heap.load();
            size_t current_size = heap->size.load();
            
            if (current_size == 0) {
                return std::nullopt;
            }
            
            // Get the root item
            HeapArray::HeapItem* root_item = heap->heap[0].load();
            if (!root_item || !root_item->valid) {
                continue;
            }
            
            // Try to get the last item
            HeapArray::HeapItem* last_item = heap->heap[current_size - 1].load();
            if (!last_item || !last_item->valid) {
                continue;
            }
            
            // Try to move last item to root
            if (heap->heap[0].compare_exchange_strong(root_item, last_item)) {
                // Mark last position as empty
                HeapArray::HeapItem* expected_last = last_item;
                if (heap->heap[current_size - 1].compare_exchange_strong(expected_last, nullptr)) {
                    // Decrement size
                    heap->size.fetch_sub(1);
                    
                    // Extract value
                    T result = std::move(root_item->value);
                    delete root_item;
                    
                    // Heapify down
                    if (current_size > 1) {
                        heapify_down(0, current_size - 1);
                    }
                    
                    return result;
                } else {
                    // Rollback root change
                    heap->heap[0].compare_exchange_strong(last_item, root_item);
                }
            }
        }
    }
    
    bool empty() const {
        HeapArray* heap = current_heap.load();
        return heap->size.load() == 0;
    }
    
    size_t size() const {
        HeapArray* heap = current_heap.load();
        return heap->size.load();
    }
};
