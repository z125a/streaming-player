#pragma once
#include <mutex>
#include <condition_variable>
#include <queue>

extern "C" {
#include <libavutil/frame.h>
}

namespace sp {

// Thread-safe queue for decoded AVFrame between decoder and renderer.
class FrameQueue {
public:
    explicit FrameQueue(size_t max_size = 16) : max_size_(max_size) {}

    ~FrameQueue() { flush(); }

    bool push(AVFrame* frame) {
        std::unique_lock<std::mutex> lock(mutex_);
        not_full_.wait(lock, [this] { return queue_.size() < max_size_ || abort_; });
        if (abort_) return false;

        AVFrame* copy = av_frame_alloc();
        av_frame_move_ref(copy, frame);
        queue_.push(copy);
        not_empty_.notify_one();
        return true;
    }

    bool pop(AVFrame* frame) {
        std::unique_lock<std::mutex> lock(mutex_);
        not_empty_.wait(lock, [this] { return !queue_.empty() || abort_; });
        if (abort_ && queue_.empty()) return false;

        AVFrame* front = queue_.front();
        queue_.pop();
        av_frame_move_ref(frame, front);
        av_frame_free(&front);
        not_full_.notify_one();
        return true;
    }

    // Non-blocking peek at front frame's pts. Returns false if empty.
    bool peek_pts(double* pts) const {
        std::lock_guard<std::mutex> lock(mutex_);
        if (queue_.empty()) return false;
        *pts = static_cast<double>(queue_.front()->pts);
        return true;
    }

    void flush() {
        std::lock_guard<std::mutex> lock(mutex_);
        while (!queue_.empty()) {
            AVFrame* f = queue_.front();
            queue_.pop();
            av_frame_free(&f);
        }
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

private:
    mutable std::mutex mutex_;
    std::condition_variable not_empty_;
    std::condition_variable not_full_;
    std::queue<AVFrame*> queue_;
    size_t max_size_;
    bool abort_ = false;
};

} // namespace sp
