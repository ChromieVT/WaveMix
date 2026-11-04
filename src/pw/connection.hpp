// SPDX-License-Identifier: GPL-3.0-or-later
// Copyright (C) 2026 ChromieVT
//
// A connection to the PipeWire server, running on its own thread.
//
// PipeWire is event-driven: it calls us back on its own loop. We run that loop on a
// background thread (pw_thread_loop) so it never blocks the GLib main loop the D-Bus
// service and the window run on. The rule that comes with it: any call into PipeWire,
// and any access to data its callbacks fill in, must hold the loop's lock. Lock does
// exactly that.

#pragma once

#include <memory>
#include <string>

struct pw_thread_loop;
struct pw_context;
struct pw_core;
struct spa_hook;

namespace wavemix::pw {

// Raises this process's open-file limit to the most it's allowed (the hard limit), for
// a process that hosts audio: every stream it hosts holds about ten files open (its
// buffers and wake-up signals), so a few dozen loopbacks pass the usual 1024. Returns
// the new limit.
long raise_open_file_limit();

class Connection {
public:
    // Connects to the PipeWire server. Throws std::runtime_error if it isn't running.
    explicit Connection(const std::string& client_name);
    // Stops the loop thread and disconnects.
    ~Connection();

    // Not copyable (copy constructor and assignment deleted): it owns the PipeWire thread and connection.
    Connection(const Connection&) = delete;
    Connection& operator=(const Connection&) = delete;

    // Holds the loop lock for as long as it exists. PipeWire callbacks don't run while
    // we hold it, so data they write can be read safely.
    class Lock {
    public:
        // Takes the loop lock of `connection`.
        explicit Lock(const Connection& connection);
        // Releases the loop lock.
        ~Lock();
        // Not copyable (copy constructor and assignment deleted): the lock is held exactly once.
        Lock(const Lock&) = delete;
        Lock& operator=(const Lock&) = delete;

    private:
        pw_thread_loop* loop_;
    };

    // Waits until the server has handled everything we've sent so far, and we've
    // received every event it sent in reply. Call it *without* holding the lock.
    // Returns false if the connection was lost meanwhile.
    //
    // Example: right after connecting, the server sends one event per existing object.
    // One sync later, the registry has seen them all.
    bool sync();

    // False once the server has gone away (PipeWire restarted, or the user logged out).
    bool connected() const;

    // The thread loop, for calls that need it directly (the lock must be held).
    pw_thread_loop* loop() const { return loop_; }
    // The PipeWire context the connection was made from.
    pw_context* context() const { return context_; }
    // The core proxy, for creating objects on the server (with the lock held).
    pw_core* core() const { return core_; }

private:
    struct Callbacks;  // the C callbacks PipeWire calls; defined in connection.cpp

    pw_thread_loop* loop_ = nullptr;
    pw_context* context_ = nullptr;
    pw_core* core_ = nullptr;
    std::unique_ptr<spa_hook> core_listener_;

    // The sequence number of our latest sync request, and of the latest reply. Written
    // by callbacks, so only touched with the lock held.
    int pending_seq_ = 0;
    int done_seq_ = -1;
    bool connected_ = false;
};

}  // namespace wavemix::pw
