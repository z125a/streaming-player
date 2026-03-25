#pragma once
#include <mutex>
#include <condition_variable>
#include <queue>

extern "C" {
#include <libavcodec/avcodec.h>
}

namespace sp {

// Thread-safe queue for compressed AVPacket between demuxer and decoder.
class PacketQueue {
public:
    explicit PacketQueue(size_t max_size = 256) : max_size_(max_size) {}

    ~PacketQueue() { flush(); }

    // Push a packet (takes ownership via av_packet_move_ref).
    // Blocks if queue is full. Returns false if aborted.
    bool push(AVPacket* pkt) {
        std::unique_lock<std::mutex> lock(mutex_);
        not_full_.wait(lock, [this] { return queue_.size() < max_size_ || abort_; });
        if (abort_) return false;

        AVPacket* copy = av_packet_alloc();
        av_packet_move_ref(copy, pkt);
        queue_.push(copy);
        duration_ += copy->duration;
        not_empty_.notify_one();
        return true;
    }

    // Pop a packet. Blocks if empty. Returns false if aborted.
    bool pop(AVPacket* pkt) {
        std::unique_lock<std::mutex> lock(mutex_);
        not_empty_.wait(lock, [this] { return !queue_.empty() || abort_; });
        if (abort_ && queue_.empty()) return false;

        AVPacket* front = queue_.front();
        queue_.pop();
        duration_ -= front->duration;
        av_packet_move_ref(pkt, front);
        av_packet_free(&front);
        not_full_.notify_one();
        return true;
    }

    void flush() {
        std::lock_guard<std::mutex> lock(mutex_);
        while (!queue_.empty()) {
            AVPacket* pkt = queue_.front();
            queue_.pop();
            av_packet_free(&pkt);
        }
        duration_ = 0;
        not_full_.notify_all();
    }

    void abort() {
        std::lock_guard<std::mutex> lock(mutex_);
        abort_ = true;
        not_empty_.notify_all();
        not_full_.notify_all();
    }

    void resume() {
        std::lock_guard<std::mutex> lock(mutex_);
        abort_ = false;
    }

    size_t size() const {
        std::lock_guard<std::mutex> lock(mutex_);
        return queue_.size();
    }

    int64_t duration() const {
        std::lock_guard<std::mutex> lock(mutex_);
        return duration_;
    }

private:
    mutable std::mutex mutex_;
    std::condition_variable not_empty_;
    std::condition_variable not_full_;
    std::queue<AVPacket*> queue_;
    size_t max_size_;
    int64_t duration_ = 0;
    bool abort_ = false;
};

} // namespace sp
