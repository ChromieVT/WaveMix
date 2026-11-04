// SPDX-License-Identifier: GPL-3.0-or-later
// Copyright (C) 2026 ChromieVT

#include "plumbing/meters.hpp"

#include <pipewire/pipewire.h>
#include <spa/param/audio/format-utils.h>

#include <atomic>
#include <cmath>

#include "pw/connection.hpp"

namespace wavemix::plumbing {

struct Meters::Meter {
    MeterSource source;
    pw_stream* stream = nullptr;
    spa_hook listener{};
    std::atomic<float> peak{0.0F};
    std::atomic<bool> read{false};  // has it had any audio yet
};

// Runs on PipeWire's audio thread: keep it short, and touch only the atomics.
struct Meters::Callbacks {
    // A buffer of audio: raises the meter's peak to the loudest sample in it.
    static void process(void* data)
    {
        auto* meter = static_cast<Meter*>(data);
        pw_buffer* buffer = pw_stream_dequeue_buffer(meter->stream);
        if (!buffer)
            return;
        const spa_data& block = buffer->buffer->datas[0];
        if (block.data && block.chunk) {
            const auto* samples = static_cast<const float*>(SPA_PTROFF(block.data, block.chunk->offset, void));
            float peak = meter->peak.load(std::memory_order_relaxed);
            for (uint32_t i = 0; i < block.chunk->size / sizeof(float); ++i)
                peak = std::max(peak, std::fabs(samples[i]));
            meter->peak.store(peak, std::memory_order_relaxed);
            meter->read.store(true, std::memory_order_relaxed);
        }
        pw_stream_queue_buffer(meter->stream, buffer);
    }
    static inline const pw_stream_events kEvents = [] {
        pw_stream_events events{};
        events.version = PW_VERSION_STREAM_EVENTS;
        events.process = &process;
        return events;
    }();
};

// Stores the connection; meters are created by watch().
Meters::Meters(pw::Connection& connection) : connection_(connection) {}

// Stops every meter before going away.
Meters::~Meters()
{
    stop();
}

// Destroys every meter's stream (under the loop lock).
void Meters::stop()
{
    pw::Connection::Lock lock(connection_);
    for (auto& [name, meter] : meters_) {
        spa_hook_remove(&meter->listener);
        pw_stream_destroy(meter->stream);
    }
    meters_.clear();
}

// Drops meters that aren't wanted or whose source changed, then starts a mono float
// capture stream for each wanted meter that's missing.
void Meters::watch(const std::map<std::string, MeterSource>& sources)
{
    pw::Connection::Lock lock(connection_);
    for (auto it = meters_.begin(); it != meters_.end();) {
        const auto wanted = sources.find(it->first);
        if (wanted != sources.end() && wanted->second == it->second->source) {
            ++it;
            continue;
        }
        spa_hook_remove(&it->second->listener);
        pw_stream_destroy(it->second->stream);
        it = meters_.erase(it);
    }

    for (const auto& [name, source] : sources) {
        if (meters_.contains(name))
            continue;
        auto meter = std::make_unique<Meter>();
        meter->source = source;
        // Not "wavemix_..." like WaveMix's plumbing: a meter coming or going isn't a
        // change to it. A source's meter isn't passive: while it's measuring, the mic
        // runs. A sink's is: it only reads while something plays, and doesn't keep the
        // channel running for nothing. No node.dont-fallback: WirePlumber drops such a
        // capture stream aimed at a virtual source (an input that's another app's
        // processed mic, say), and a meter that briefly reads another device is
        // harmless; it's only started for a source or sink that's there.
        pw_properties* props = pw_properties_new(
            PW_KEY_MEDIA_TYPE, "Audio", PW_KEY_MEDIA_CATEGORY, "Capture", PW_KEY_MEDIA_NAME,
            ("WaveMix meter (" + name + ")").c_str(), PW_KEY_NODE_NAME, ("wavemix-meter-" + name).c_str(),
            PW_KEY_TARGET_OBJECT, source.node.c_str(), PW_KEY_NODE_LATENCY, "1024/48000", nullptr);
        if (source.sink) {
            pw_properties_set(props, PW_KEY_STREAM_CAPTURE_SINK, "true");
            pw_properties_set(props, PW_KEY_NODE_PASSIVE, "true");
        }
        meter->stream = pw_stream_new(connection_.core(), ("wavemix-meter-" + name).c_str(), props);
        if (!meter->stream)
            continue;
        pw_stream_add_listener(meter->stream, &meter->listener, &Callbacks::kEvents, meter.get());

        // 32-bit float, mono: one number per sample to compare, whatever the source.
        uint8_t buffer[512];
        spa_pod_builder builder = SPA_POD_BUILDER_INIT(buffer, sizeof(buffer));
        spa_audio_info_raw info{};
        info.format = SPA_AUDIO_FORMAT_F32;
        info.channels = 1;
        info.position[0] = SPA_AUDIO_CHANNEL_MONO;
        const spa_pod* params[] = {spa_format_audio_raw_build(&builder, SPA_PARAM_EnumFormat, &info)};
        pw_stream_connect(meter->stream, PW_DIRECTION_INPUT, PW_ID_ANY,
                          static_cast<pw_stream_flags>(PW_STREAM_FLAG_AUTOCONNECT | PW_STREAM_FLAG_MAP_BUFFERS |
                                                       PW_STREAM_FLAG_RT_PROCESS),
                          params, 1);
        meters_[name] = std::move(meter);
    }
}

// Reads and resets each meter's peak, skipping meters that haven't read audio yet.
std::map<std::string, float> Meters::take()
{
    std::map<std::string, float> peaks;
    for (auto& [name, meter] : meters_)
        if (meter->read.load(std::memory_order_relaxed))
            peaks[name] = meter->peak.exchange(0.0F, std::memory_order_relaxed);
    return peaks;
}

}  // namespace wavemix::plumbing
