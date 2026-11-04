// SPDX-License-Identifier: GPL-3.0-or-later
// Copyright (C) 2026 ChromieVT

#include "pw/connection.hpp"

#include "core/log.hpp"

#include <pipewire/pipewire.h>

#include <cerrno>
#include <cstring>
#include <dlfcn.h>
#include <sys/resource.h>

#include <filesystem>
#include <string>
#include <mutex>
#include <stdexcept>

namespace wavemix::pw {

namespace {

// Inside the Flatpak, PipeWire's filter-chain module and the filter-graph plugins it runs
// come with the app (the runtime leaves them out): PipeWire looks in the app's folders
// first, then the runtime's, which are next to libpipewire itself. Elsewhere, and if
// they're set already, the defaults stand.
void use_bundled_pipewire_plugins()
{
    namespace fs = std::filesystem;
    std::error_code error;
    if (!fs::exists("/app/lib/pipewire-0.3", error))
        return;
    Dl_info info{};
    if (!dladdr(reinterpret_cast<void*>(&pw_init), &info) || !info.dli_fname)
        return;
    const fs::path lib = fs::path(info.dli_fname).parent_path();  // /usr/lib/x86_64-linux-gnu
    setenv("PIPEWIRE_MODULE_DIR", ("/app/lib/pipewire-0.3:" + (lib / "pipewire-0.3").string()).c_str(), 0);
    setenv("SPA_PLUGIN_DIR", ("/app/lib/spa-0.2:" + (lib / "spa-0.2").string()).c_str(), 0);
}

}  // namespace

// Lifts the soft RLIMIT_NOFILE to the hard limit and returns the result (-1 on error).
long raise_open_file_limit()
{
    rlimit limit{};
    if (getrlimit(RLIMIT_NOFILE, &limit) != 0)
        return -1;
    if (limit.rlim_cur < limit.rlim_max) {
        limit.rlim_cur = limit.rlim_max;
        if (setrlimit(RLIMIT_NOFILE, &limit) != 0)
            return -1;
    }
    return static_cast<long>(limit.rlim_cur);
}

// PipeWire calls these plain C functions; `data` is the Connection they belong to.
// Like every PipeWire callback, they run on the loop thread with the lock held.
struct Connection::Callbacks {
    // A sync reply: records it and wakes sync() once it answers our latest request.
    static void on_done(void* data, uint32_t id, int seq)
    {
        auto* self = static_cast<Connection*>(data);
        if (id == PW_ID_CORE && seq >= self->pending_seq_) {
            self->done_seq_ = seq;
            pw_thread_loop_signal(self->loop_, false);  // wake up sync()
        }
    }

    // A server error: logs errors on the core, and marks the connection lost on -EPIPE.
    static void on_error(void* data, uint32_t id, int /*seq*/, int res, const char* message)
    {
        auto* self = static_cast<Connection*>(data);
        // Errors about one object (a failed module load, say) are reported to whoever
        // made that request. Only an error on the core itself means the connection is
        // gone; -EPIPE is what we get when the server goes away.
        if (id == PW_ID_CORE) {
            core::log::warning("pw", std::string("PipeWire connection error: ") + (message ? message : "") + " (" +
                                         std::strerror(-res) + ")");
            if (res == -EPIPE) {
                self->connected_ = false;
                pw_thread_loop_signal(self->loop_, false);  // don't leave sync() hanging
            }
        }
    }

    // Only the events we need; the rest stay null.
    static inline const pw_core_events kEvents = {
        .version = PW_VERSION_CORE_EVENTS,
        .done = &on_done,
        .error = &on_error,
    };
};

// Initialises PipeWire once, starts the loop thread and connects to the server,
// cleaning up and throwing if any step fails.
Connection::Connection(const std::string& client_name)
    : core_listener_(std::make_unique<spa_hook>())
{
    // pw_init() sets up the library once per process. It's safe to call more than
    // once, but there's no reason to.
    static std::once_flag initialised;
    std::call_once(initialised, [] {
        use_bundled_pipewire_plugins();
        pw_init(nullptr, nullptr);
    });

    loop_ = pw_thread_loop_new("wavemix-pipewire", nullptr);
    if (!loop_)
        throw std::runtime_error("could not create the PipeWire loop");

    // The client name shows up in pw-top, pw-dump and so on, so the user can tell what
    // the connection belongs to.
    context_ = pw_context_new(pw_thread_loop_get_loop(loop_),
                              pw_properties_new(PW_KEY_APP_NAME, client_name.c_str(), nullptr), 0);
    if (!context_) {
        pw_thread_loop_destroy(loop_);
        throw std::runtime_error("could not create the PipeWire context");
    }

    if (pw_thread_loop_start(loop_) < 0) {
        pw_context_destroy(context_);
        pw_thread_loop_destroy(loop_);
        throw std::runtime_error("could not start the PipeWire thread");
    }

    pw_thread_loop_lock(loop_);
    core_ = pw_context_connect(context_, nullptr, 0);
    if (!core_) {
        const std::string reason = std::strerror(errno);
        // Unlock before tearing down: pw_thread_loop_stop waits for the thread, and
        // the thread needs the lock to finish.
        pw_thread_loop_unlock(loop_);
        pw_thread_loop_stop(loop_);
        pw_context_destroy(context_);
        pw_thread_loop_destroy(loop_);
        throw std::runtime_error("could not connect to PipeWire: " + reason);
    }
    *core_listener_ = {};
    pw_core_add_listener(core_, core_listener_.get(), &Callbacks::kEvents, this);
    connected_ = true;
    pw_thread_loop_unlock(loop_);
}

// Stops the loop thread first, then removes the listener and frees everything.
Connection::~Connection()
{
    // Stop the thread first, so no callback can run while we tear things down.
    pw_thread_loop_stop(loop_);
    spa_hook_remove(core_listener_.get());
    pw_core_disconnect(core_);
    pw_context_destroy(context_);
    pw_thread_loop_destroy(loop_);
}

// Locks the connection's thread loop.
Connection::Lock::Lock(const Connection& connection) : loop_(connection.loop_)
{
    pw_thread_loop_lock(loop_);
}

// Unlocks the thread loop.
Connection::Lock::~Lock()
{
    pw_thread_loop_unlock(loop_);
}

// Sends a core sync and waits (lock released) for its reply or a lost connection.
bool Connection::sync()
{
    Lock lock(*this);
    if (!connected_)
        return false;
    pending_seq_ = pw_core_sync(core_, PW_ID_CORE, pending_seq_);
    // pw_thread_loop_wait releases the lock while it waits, so the loop thread can run
    // our callbacks; on_done (or on_error) wakes us up again.
    while (connected_ && done_seq_ < pending_seq_)
        pw_thread_loop_wait(loop_);
    return connected_;
}

// Reads the connected flag under the lock.
bool Connection::connected() const
{
    Lock lock(*this);
    return connected_;
}

}  // namespace wavemix::pw
