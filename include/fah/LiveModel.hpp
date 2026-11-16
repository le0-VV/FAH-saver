#pragma once

#include "fah/Source.hpp"
#include <memory>
#include <stdexcept>

namespace fah {

class LiveModel {
    std::shared_ptr<const Capture> display_;
    std::shared_ptr<Capture> live_;
    std::shared_ptr<const Topology> pendingTopology_, latestTopology_;
    std::shared_ptr<const Frame> latestFrame_;
    std::string pendingUnit_;
    std::size_t retainedBytes_ = 0;
    std::uint64_t revision_ = 0;
    bool fallback_ = true;

    void useLatest() {
        if (fallback_) return;
        if (!latestTopology_ || !latestFrame_)
            throw std::logic_error("Live model lacks a complete latest snapshot");
        auto snapshot = std::make_shared<Capture>();
        snapshot->topology = *latestTopology_;
        snapshot->frames.push_back(*latestFrame_);
        display_ = std::move(snapshot);
        live_.reset();
        retainedBytes_ = 0;
        fallback_ = true;
        ++revision_;
    }
public:
    const std::shared_ptr<const Capture>& capture() const { return display_; }
    std::uint64_t revision() const { return revision_; }
    bool available() const { return display_ && !display_->frames.empty(); }
    bool usingFallback() const { return fallback_ && available(); }
    const std::shared_ptr<const Topology>& latestTopology() const { return latestTopology_; }
    const std::shared_ptr<const Frame>& latestFrame() const { return latestFrame_; }

    void clear() {
        pendingTopology_.reset(); latestTopology_.reset(); latestFrame_.reset();
        pendingUnit_.clear(); display_.reset(); live_.reset();
        retainedBytes_ = 0; fallback_ = true; ++revision_;
    }

    void seed(std::shared_ptr<const Capture> snapshot) {
        if (!snapshot || snapshot->frames.size() != 1 || snapshot->topology.atoms.empty() ||
            snapshot->frames.front().size() != snapshot->topology.atoms.size())
            throw std::runtime_error("Cached model must contain one complete snapshot");
        pendingTopology_.reset(); pendingUnit_.clear(); live_.reset();
        latestTopology_ = std::shared_ptr<const Topology>(snapshot, &snapshot->topology);
        latestFrame_ = std::shared_ptr<const Frame>(snapshot, &snapshot->frames.front());
        display_ = std::move(snapshot);
        retainedBytes_ = 0; fallback_ = true; ++revision_;
    }

    // Source and cache parsers validate coordinates; this checks complete topology/frame pairs.
    bool receive(const SourceEvent& event, bool loop) {
        switch (event.kind) {
        case SourceEvent::Kind::Reset:
        case SourceEvent::Kind::Error:
            pendingTopology_.reset(); pendingUnit_.clear();
            useLatest();
            break;
        case SourceEvent::Kind::Topology:
            if (!event.topology || event.topology->atoms.empty())
                throw std::runtime_error("Source delivered an empty topology event");
            pendingTopology_ = event.topology;
            pendingUnit_ = event.unit;
            useLatest();
            break;
        case SourceEvent::Kind::Frame: {
            if (!event.frame || !pendingTopology_ || event.frame->size() != pendingTopology_->atoms.size() ||
                event.unit != pendingUnit_)
                throw std::runtime_error("Source delivered a frame without matching topology and work unit");
            const auto retained = fallback_ || !loop ? 0 : retainedBytes_;
            const auto count = fallback_ || !loop ? 0 : live_->frames.size();
            if (count >= 100000 || event.frame->size() > (128u*1024*1024-retained)/sizeof(Vec3))
                throw std::runtime_error("Visualisation history exceeds the 128 MiB or 100,000 snapshot limit");
            if (fallback_) {
                auto snapshot = std::make_shared<Capture>();
                snapshot->topology = *pendingTopology_;
                snapshot->frames.push_back(*event.frame);
                live_ = std::move(snapshot);
                display_ = live_;
                ++revision_;
            } else if (!loop) {
                live_->frames.resize(1);
                live_->frames.front() = *event.frame;
                ++revision_;
            } else live_->frames.push_back(*event.frame);
            retainedBytes_ = retained + event.frame->size()*sizeof(Vec3);
            latestTopology_ = pendingTopology_;
            latestFrame_ = event.frame;
            fallback_ = false;
            return true;
        }
        case SourceEvent::Kind::Status: break;
        }
        return false;
    }
};

} // namespace fah
