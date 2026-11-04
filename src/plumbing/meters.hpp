// SPDX-License-Identifier: GPL-3.0-or-later
// Copyright (C) 2026 ChromieVT
//
// Level meters: a capture stream per meter, run by the service, each keeping the loudest
// sample it has read. The window asks for them (GetMeters) while it shows them; the
// mixer stops them a few seconds after the last request, so the mic isn't kept open
// for nothing.
//
// The meters measure your mic before the chain, WaveMix Mic after it, and what plays
// into each channel (its sink, read through its monitor, as OBS reads a mix). (A meter
// per filter isn't possible: PipeWire's filter-chain doesn't pass on the plugins' own
// meter outputs.)
//
// Use it from one thread (the mixer's). The streams' audio runs on PipeWire's thread;
// the peaks cross over as atomics.

#pragma once

#include <map>
#include <memory>
#include <string>

namespace wavemix::pw {
class Connection;
}

namespace wavemix::plumbing {

// What a meter reads: a source, or a sink (through its monitor).
struct MeterSource {
    std::string node;
    bool sink = false;
    // Two sources are equal when they name the same node the same way.
    bool operator==(const MeterSource&) const = default;
};

class Meters {
public:
    // Starts with no meters; watch() creates them.
    explicit Meters(pw::Connection& connection);
    // Stops every meter.
    ~Meters();

    // Not copyable (copy constructor and assignment deleted): it owns PipeWire streams.
    Meters(const Meters&) = delete;
    Meters& operator=(const Meters&) = delete;

    // Measures these: meter name -> what it reads. Starts the ones missing, moves ones
    // whose source changed, and stops the rest.
    void watch(const std::map<std::string, MeterSource>& sources);

    // Stops every meter.
    void stop();

    // The loudest sample each meter read since the last call (linear: 1.0 is full
    // scale), by meter name. A meter that hasn't read anything yet isn't in it.
    std::map<std::string, float> take();

private:
    struct Meter;
    struct Callbacks;  // the C callbacks PipeWire calls; in the .cpp

    pw::Connection& connection_;
    std::map<std::string, std::unique_ptr<Meter>> meters_;
};

}  // namespace wavemix::plumbing
