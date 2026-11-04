// SPDX-License-Identifier: GPL-3.0-or-later
// Copyright (C) 2026 ChromieVT

#include "plumbing/pulse_control.hpp"

#include <pulse/pulseaudio.h>

#include <stdexcept>

namespace wavemix::plumbing {

namespace {

// Holds the libpulse loop lock while it exists, like pw::Connection::Lock.
class Lock {
public:
    // Takes the libpulse loop lock.
    explicit Lock(pa_threaded_mainloop* loop) : loop_(loop) { pa_threaded_mainloop_lock(loop_); }
    // Releases the libpulse loop lock.
    ~Lock() { pa_threaded_mainloop_unlock(loop_); }
    // Not copyable (copy constructor and assignment deleted): the lock is held exactly once.
    Lock(const Lock&) = delete;
    Lock& operator=(const Lock&) = delete;

private:
    pa_threaded_mainloop* loop_;
};

// Where a request's callback leaves its answer for the waiting caller.
struct LoadResult {
    pa_threaded_mainloop* loop;
    uint32_t index = PA_INVALID_INDEX;
};

struct SuccessResult {
    pa_threaded_mainloop* loop;
    bool success = false;
};

struct ListResult {
    pa_threaded_mainloop* loop;
    std::vector<PulseModule> modules;
};

}  // namespace

// libpulse calls these on its own thread with the loop lock held. Each one stores its
// result and wakes up the caller waiting in PulseControl::wait().
struct PulseControl::Callbacks {
    // The context's state changed: wakes connect(), which checks the new state.
    static void on_state(pa_context* /*context*/, void* loop)
    {
        pa_threaded_mainloop_signal(static_cast<pa_threaded_mainloop*>(loop), 0);
    }

    // A module load finished: stores its index (PA_INVALID_INDEX on failure).
    static void on_loaded(pa_context* /*context*/, uint32_t index, void* data)
    {
        auto* result = static_cast<LoadResult*>(data);
        result->index = index;
        pa_threaded_mainloop_signal(result->loop, 0);
    }

    // A request that only succeeds or fails finished: stores which.
    static void on_success(pa_context* /*context*/, int success, void* data)
    {
        auto* result = static_cast<SuccessResult*>(data);
        result->success = success != 0;
        pa_threaded_mainloop_signal(result->loop, 0);
    }

    // One module of the list (or the end of it): collects it, and wakes the caller at the end.
    static void on_module(pa_context* /*context*/, const pa_module_info* info, int end_of_list, void* data)
    {
        auto* result = static_cast<ListResult*>(data);
        if (end_of_list || !info) {
            pa_threaded_mainloop_signal(result->loop, 0);
            return;
        }
        result->modules.push_back({info->index, info->name ? info->name : "",
                                   info->argument ? info->argument : ""});
    }
};

// Starts the libpulse thread loop and connects, cleaning up if either fails.
PulseControl::PulseControl(const std::string& client_name) : client_name_(client_name)
{
    loop_ = pa_threaded_mainloop_new();
    if (!loop_)
        throw std::runtime_error("could not create the PulseAudio loop");
    if (pa_threaded_mainloop_start(loop_) < 0) {
        pa_threaded_mainloop_free(loop_);
        throw std::runtime_error("could not start the PulseAudio loop");
    }
    try {
        Lock lock(loop_);
        connect();
    } catch (...) {
        pa_threaded_mainloop_stop(loop_);
        pa_threaded_mainloop_free(loop_);
        throw;
    }
}

// Disconnects, stops the loop thread and frees the context and loop.
PulseControl::~PulseControl()
{
    {
        Lock lock(loop_);
        if (context_)
            pa_context_disconnect(context_);
    }
    pa_threaded_mainloop_stop(loop_);
    if (context_)
        pa_context_unref(context_);
    pa_threaded_mainloop_free(loop_);
}

// Creates a context and waits until it's ready; throws if pipewire-pulse isn't there.
void PulseControl::connect()
{
    context_ = pa_context_new(pa_threaded_mainloop_get_api(loop_), client_name_.c_str());
    if (!context_)
        throw std::runtime_error("could not create the PulseAudio context");
    pa_context_set_state_callback(context_, &Callbacks::on_state, loop_);

    // PA_CONTEXT_NOAUTOSPAWN: never start a PulseAudio daemon ourselves. If
    // pipewire-pulse isn't there, that's an error to report, not something to "fix".
    pa_context_state_t state = PA_CONTEXT_FAILED;
    if (pa_context_connect(context_, nullptr, PA_CONTEXT_NOAUTOSPAWN, nullptr) >= 0) {
        // Wait until the connection is ready, or has definitely failed.
        while ((state = pa_context_get_state(context_)) != PA_CONTEXT_READY && PA_CONTEXT_IS_GOOD(state))
            pa_threaded_mainloop_wait(loop_);
    }
    if (state != PA_CONTEXT_READY) {
        const std::string reason = pa_strerror(pa_context_errno(context_));
        pa_context_unref(context_);
        context_ = nullptr;
        throw std::runtime_error("could not connect to pipewire-pulse: " + reason);
    }
}

// Replaces a context that's no longer ready with a fresh connection.
void PulseControl::reconnect_if_lost()
{
    // pipewire-pulse restarted (or crashed and came back): the old connection is dead
    // for good, so every request would fail. Connect to the new one instead.
    if (context_ && pa_context_get_state(context_) == PA_CONTEXT_READY)
        return;
    if (context_) {
        pa_context_disconnect(context_);
        pa_context_unref(context_);
        context_ = nullptr;
    }
    connect();
}

// Waits for a libpulse operation to finish; throws if it couldn't be started.
void PulseControl::wait(pa_operation* operation)
{
    if (!operation)
        throw std::runtime_error(std::string("pipewire-pulse request failed: ") +
                                 pa_strerror(pa_context_errno(context_)));
    // The operation stays RUNNING until its callback has fired. If the connection
    // drops, libpulse cancels it, so this can't hang forever.
    while (pa_operation_get_state(operation) == PA_OPERATION_RUNNING)
        pa_threaded_mainloop_wait(loop_);
    pa_operation_unref(operation);
}

// Asks pipewire-pulse to load a module and returns its index, throwing on failure.
uint32_t PulseControl::load(const std::string& name, const std::string& arguments)
{
    Lock lock(loop_);
    reconnect_if_lost();
    LoadResult result{loop_};
    wait(pa_context_load_module(context_, name.c_str(), arguments.c_str(), &Callbacks::on_loaded, &result));
    if (result.index == PA_INVALID_INDEX)
        throw std::runtime_error("pipewire-pulse refused to load " + name + ": " +
                                 pa_strerror(pa_context_errno(context_)));
    return result.index;
}

// Asks pipewire-pulse to unload a module; false if it was already gone.
bool PulseControl::unload(uint32_t index)
{
    Lock lock(loop_);
    reconnect_if_lost();
    SuccessResult result{loop_};
    wait(pa_context_unload_module(context_, index, &Callbacks::on_success, &result));
    return result.success;
}

// Asks pipewire-pulse to move a sink-input to the named sink.
bool PulseControl::move_stream(uint32_t stream, const std::string& sink_name)
{
    Lock lock(loop_);
    reconnect_if_lost();
    SuccessResult result{loop_};
    wait(pa_context_move_sink_input_by_name(context_, stream, sink_name.c_str(), &Callbacks::on_success, &result));
    return result.success;
}

// Asks pipewire-pulse for every loaded module.
std::vector<PulseModule> PulseControl::list()
{
    Lock lock(loop_);
    reconnect_if_lost();
    ListResult result{loop_};
    wait(pa_context_get_module_info_list(context_, &Callbacks::on_module, &result));
    return std::move(result.modules);
}

}  // namespace wavemix::plumbing
