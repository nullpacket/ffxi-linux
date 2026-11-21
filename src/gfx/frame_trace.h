// SPDX-License-Identifier: GPL-3.0-or-later
#pragma once

#include <algorithm>
#include <array>
#include <chrono>
#include <cstdio>
#include <cstdlib>
#include <string>
#include <vector>

#include <spdlog/spdlog.h>

namespace xi::gfx
{

// XI_FRAME_TRACE=N: frame-time trace. Every N seconds (default 10) logs the frame time and each stage's CPU time as
// p50 / p95 / p99 / max, plus the GPU time of the frame's command buffer (timestamps). XI_FRAME_TRACE_CSV=path also
// writes one row per frame. Stages are marked in order through the frame; each mark closes the stage before it.
class FrameTrace
{
public:
    enum Stage
    {
        Game,    // input, session, movement, camera
        Ui,      // ImGui + the game's windows
        Actors,  // character models: load, pose, instances, labels
        Effects, // effect schedulers + particle geometry
        Wait,    // beginFrame: fence + acquire (GPU or present bound)
        Record,  // command buffer recording
        Submit,  // submit + present
        Count
    };

    FrameTrace()
    {
        if (const char* v = std::getenv("XI_FRAME_TRACE"))
        {
            enabled_ = true;
            period_  = std::atoi(v) > 0 ? std::atoi(v) : 10;
        }
        if (const char* csv = std::getenv("XI_FRAME_TRACE_CSV"); enabled_ && csv)
        {
            csv_ = std::fopen(csv, "w");
            if (csv_)
            {
                std::fprintf(csv_, "t,frame,game,ui,actors,effects,wait,record,submit,gpu,actors_n\n");
            }
        }
    }
    ~FrameTrace()
    {
        if (csv_)
        {
            std::fclose(csv_);
        }
    }
    FrameTrace(const FrameTrace&)            = delete;
    FrameTrace& operator=(const FrameTrace&) = delete;

    [[nodiscard]] bool enabled() const
    {
        return enabled_;
    }

    // Start of a frame: closes the previous one (its total is start to start).
    void begin(double gpuMs, int actors)
    {
        if (!enabled_)
        {
            return;
        }
        const auto now = Clock::now();
        if (started_)
        {
            cur_[Count] = ms(frameStart_, now);
            cur_[Count + 1] = gpuMs;
            for (int i = 0; i < Count + 2; ++i)
            {
                samples_[i].push_back(cur_[i]);
            }
            actors_ += actors;
            if (csv_)
            {
                std::fprintf(csv_, "%.3f,%.3f,%.3f,%.3f,%.3f,%.3f,%.3f,%.3f,%.3f,%.3f,%d\n", ms(origin_, frameStart_) / 1000.0, cur_[Count], cur_[Game], cur_[Ui],
                             cur_[Actors], cur_[Effects], cur_[Wait], cur_[Record], cur_[Submit], gpuMs, actors);
            }
            if (now - windowStart_ > std::chrono::seconds(period_))
            {
                report();
                windowStart_ = now;
            }
        }
        else
        {
            origin_ = windowStart_ = now;
            started_ = true;
        }
        cur_.fill(0.0);
        frameStart_ = mark_ = Clock::now(); // after the report and the CSV row: the trace does not time itself
    }

    void mark(Stage s)
    {
        if (!enabled_ || !started_)
        {
            return;
        }
        const auto now = Clock::now();
        cur_[s] += ms(mark_, now);
        mark_ = now;
    }

private:
    using Clock = std::chrono::steady_clock;
    static double ms(Clock::time_point a, Clock::time_point b)
    {
        return std::chrono::duration<double, std::milli>(b - a).count();
    }

    void report()
    {
        static constexpr std::array<const char*, Count + 2> names{ "game", "ui", "actors", "effects", "wait", "record", "submit", "frame", "gpu" };
        const std::size_t n = samples_[Count].size();
        if (n == 0)
        {
            return;
        }
        double sum = 0;
        for (const double v : samples_[Count])
        {
            sum += v;
        }
        spdlog::info("frame trace: {} frames, {:.0f} fps, {:.1f} actors avg", n, 1000.0 * static_cast<double>(n) / sum,
                     static_cast<double>(actors_) / static_cast<double>(n));
        for (const int i : { int{ Count }, Count + 1, 0, 1, 2, 3, 4, 5, 6 })
        {
            auto& v = samples_[i];
            std::sort(v.begin(), v.end());
            const auto pct = [&](double p) { return v[std::min(v.size() - 1, static_cast<std::size_t>(p * static_cast<double>(v.size())))]; };
            spdlog::info("frame trace: {:8s} p50 {:6.2f}  p95 {:6.2f}  p99 {:6.2f}  max {:6.2f} ms", names[i], pct(0.50), pct(0.95), pct(0.99), v.back());
            v.clear();
        }
        actors_ = 0;
    }

    bool                                       enabled_ = false;
    bool                                       started_ = false;
    int                                        period_  = 10;
    std::FILE*                                 csv_     = nullptr;
    Clock::time_point                          origin_, windowStart_, frameStart_, mark_;
    std::array<double, Count + 2>              cur_{};
    std::array<std::vector<double>, Count + 2> samples_;
    long long                                  actors_ = 0;
};

} // namespace xi::gfx
