#pragma once
#include <algorithm>
#include <cmath>
#include <cstdint>
#include <vector>

// Linked 30 ms rolling peak maximum, immediate attack and 50 ms release.
// Only peak levels are retained; no PCM buffering or added audio latency.
class UsbPeakLimiter {
    double gain_ = 1.0, release_ = 0.0;
    int holdFrames_ = 1;
    size_t head_ = 0, tail_ = 0;
    int64_t frame_ = 0;
    std::vector<double> peaks_;
    std::vector<int64_t> frames_;
public:
    void configure(int rate) {
        release_ = std::exp(-1.0 / (rate * 0.050));
        holdFrames_ = std::max(1, int(rate * 0.030));
        peaks_.resize(holdFrames_ + 1);
        frames_.resize(holdFrames_ + 1);
        reset();
    }
    void reset() { gain_ = 1.0; head_ = tail_ = 0; frame_ = 0; }
    bool isUnity() const { return gain_ == 1.0 && head_ == tail_; }
    bool process(float *data, int frames, int channels) {
        bool changed = false;
        for (int f = 0; f < frames; ++f) {
            double peak = 0.0;
            for (int c = 0; c < channels; ++c)
                peak = std::max(peak, std::abs(double(data[f * channels + c])));
            while (head_ != tail_ && frames_[head_] <= frame_ - holdFrames_)
                head_ = (head_ + 1) % peaks_.size();
            if (peak > 1.0) {
                while (head_ != tail_) {
                    const size_t previous = (tail_ + peaks_.size() - 1) % peaks_.size();
                    if (peaks_[previous] > peak) break;
                    tail_ = previous;
                }
                peaks_[tail_] = peak;
                frames_[tail_] = frame_;
                tail_ = (tail_ + 1) % peaks_.size();
            }
            ++frame_;
            const double ceiling = head_ == tail_ ? 1.0 : 1.0 / peaks_[head_];
            gain_ = std::min(ceiling, 1.0 - (1.0 - gain_) * release_);
            // Below float precision, finish recovery exactly so an integer
            // bypass can resume instead of staying in float indefinitely.
            if (head_ == tail_ && 1.0 - gain_ < 1e-9) gain_ = 1.0;
            if (gain_ < 1.0) {
                changed = true;
                for (int c = 0; c < channels; ++c)
                    data[f * channels + c] = float(data[f * channels + c] * gain_);
            }
        }
        return changed;
    }
};
