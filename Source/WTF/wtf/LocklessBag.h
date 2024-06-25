/*
 * Copyright (C) 2017-2024 Apple Inc. All rights reserved.
 *
 * Redistribution and use in source and binary forms, with or without
 * modification, are permitted provided that the following conditions
 * are met:
 * 1. Redistributions of source code must retain the above copyright
 *    notice, this list of conditions and the following disclaimer.
 * 2. Redistributions in binary form must reproduce the above copyright
 *    notice, this list of conditions and the following disclaimer in the
 *    documentation and/or other materials provided with the distribution.
 *
 * THIS SOFTWARE IS PROVIDED BY APPLE INC. ``AS IS'' AND ANY
 * EXPRESS OR IMPLIED WARRANTIES, INCLUDING, BUT NOT LIMITED TO, THE
 * IMPLIED WARRANTIES OF MERCHANTABILITY AND FITNESS FOR A PARTICULAR
 * PURPOSE ARE DISCLAIMED.  IN NO EVENT SHALL APPLE INC. OR
 * CONTRIBUTORS BE LIABLE FOR ANY DIRECT, INDIRECT, INCIDENTAL, SPECIAL,
 * EXEMPLARY, OR CONSEQUENTIAL DAMAGES (INCLUDING, BUT NOT LIMITED TO,
 * PROCUREMENT OF SUBSTITUTE GOODS OR SERVICES; LOSS OF USE, DATA, OR
 * PROFITS; OR BUSINESS INTERRUPTION) HOWEVER CAUSED AND ON ANY THEORY
 * OF LIABILITY, WHETHER IN CONTRACT, STRICT LIABILITY, OR TORT
 * (INCLUDING NEGLIGENCE OR OTHERWISE) ARISING IN ANY WAY OUT OF THE USE
 * OF THIS SOFTWARE, EVEN IF ADVISED OF THE POSSIBILITY OF SUCH DAMAGE.
 */

#pragma once

#include <wtf/Atomics.h>
#include <wtf/Noncopyable.h>
#include <wtf/StdLibExtras.h>

namespace WTF {

// This a simple single consumer, multiple producer or multiple reader/producer (no-consumer) Bag data structure.

template<typename T>
class LocklessBag {
    WTF_MAKE_FAST_ALLOCATED;
    WTF_MAKE_NONCOPYABLE(LocklessBag);
public:
    struct Node {
        WTF_MAKE_FAST_ALLOCATED;
    public:
        using Data = T;
        Node(Data&& value) : data(std::forward<Data>(value)) { }

        Node* next { nullptr };
        Data data;
    };

    LocklessBag() = default;

    enum PushResult { Empty, NonEmpty };
    PushResult add(T&& element)
    {
        return add(std::make_unique<Node>(std::forward<T>(element)));
    }

    // This is useful when you want to fill up a node on one thread then push it once it's full.
    PushResult add(std::unique_ptr<Node>&& node)
    {
        Node* newNode = node.release();

        Node* oldHead;
        m_head.transaction([&] (Node*& head) {
            oldHead = head;
            newNode->next = head;
            head = newNode;
            return true;
        }, std::memory_order_release);

        return oldHead == nullptr ? Empty : NonEmpty;
    }

    // READER/CONSUMER FUNCTIONS: Everything below here is only safe to call from the consumer, if there is one.
    // If there is no consumer anyone can call these.

    bool isEmpty() const { return !m_head.load(std::memory_order_relaxed); }
    const Node* head() const { return m_head.load(std::memory_order_acquire); }

    void iterate(NOESCAPE const Invocable<void(const T&)> auto& func)
    {
        const Node* node = head();
        while (node) {
            func(node->data);
            node = node->next;
        }
    }

    // CONSUMER FUNCTIONS: Everything below here is only safe to call from the consumer thread.
    void consumeAll(NOESCAPE const Invocable<void(T&&)> auto& func)
    {
        consumeAllWithNode([&] (T&& data, Node*) {
            func(std::forward<T>(data));
        });
    }

    void consumeAllWithNode(NOESCAPE const Invocable<void(T&&, Node*)> auto& func)
    {
        Node* node = takeBag();
        while (node) {
            Node* oldNode = node;
            node = node->next;
            func(std::forward<T>(oldNode->data), oldNode);
            delete oldNode;
        }
    }

    ~LocklessBag()
    {
        consumeAll([] (T&&) { });
    }

    Node* takeBag() { return m_head.exchange(nullptr, std::memory_order_acquire); }

private:
    Atomic<Node*> m_head { nullptr };
};

// This a simple lock-free, single consumer, multiple producer queue data structure.

enum class DequeueOrdering { FIFO, Any };
template<typename T, DequeueOrdering dequeueOrdering = DequeueOrdering::FIFO>
class LocklessQueue : private LocklessBag<T> {
    WTF_MAKE_FAST_ALLOCATED;
    WTF_MAKE_NONCOPYABLE(LocklessQueue);
    using Base = LocklessBag<T>;
public:
    using Node = Base::Node;
    using QueueResult = Base::PushResult;

    LocklessQueue() = default;

    QueueResult queue(T&& data) { return Base::add(std::forward<T>(data)); }
    QueueResult queue(std::unique_ptr<Node>&& node) { return Base::add(WTFMove(node)); }

    // CONSUMER FUNCTIONS: Everything below here is only safe to call from the consumer thread.

    bool isEmpty() const { return !m_head && Base::isEmpty(); }
    // std::optional<U&> is "ill-formed" for some reason. This is practically std::optional<T>
    using DequeueResultType = std::optional<std::conditional_t<std::is_reference_v<T>, std::reference_wrapper<std::remove_reference_t<T>>, T>>;
    DequeueResultType dequeue()
    {
        refillIfNeeded();
        if (m_head) {
            Node* node = m_head;
            m_head = m_head->next;
            T result = std::forward<T>(node->data);
            delete node;
            return result;
        }
        return { };
    }

    const Node* peek()
    {
        refillIfNeeded();
        return m_head;
    }

    ~LocklessQueue()
    {
        while(!isEmpty())
            dequeue();
    }

private:
    void refillIfNeeded()
    {
        if (m_head)
            return;

        Node* newHead = Base::takeBag();
        if constexpr (dequeueOrdering == DequeueOrdering::FIFO) {
            Node* previous = nullptr;
            while (newHead) {
                Node* next = newHead->next;
                newHead->next = previous;
                previous = newHead;
                newHead = next;
            }
            newHead = previous;
        }
        m_head = newHead;
    }

    Node* m_head { nullptr };
};

} // namespace WTF

using WTF::LocklessBag;
using WTF::DequeueOrdering;
using WTF::LocklessQueue;
