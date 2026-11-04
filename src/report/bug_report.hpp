// SPDX-License-Identifier: GPL-3.0-or-later
// Copyright (C) 2026 ChromieVT
//
// The bug report: one text file with what's needed to understand a problem on a
// system we can't test on.
//
//   - WaveMix's version, how it was installed, and whether the service is running;
//   - the distribution, desktop, session type and kernel;
//   - the PipeWire and WirePlumber versions, and how many files their processes have
//     open (running out of them takes audio down; see docs/plumbing-prototypes.md);
//   - WaveMix's part of the audio graph: the outputs, its sinks and loopbacks, the
//     apps playing through it, and anything the config expects that's missing;
//   - config.json, state.json and the login file;
//   - the end of the log.
//
// It names audio devices and the apps playing through WaveMix, and says so at the
// top. Other apps, and anything else about the system, stay out. The home folder is
// written as "~", so the user's name isn't in it either. Nothing is sent anywhere:
// the user attaches the file to a GitHub issue.
//
// Made in the process that asks for it (`wavemix --bug-report`, or Save bug report
// in the window), not by the service: a report is most needed when the service isn't
// working.

#pragma once

#include <string>

namespace wavemix::report {

// Builds the whole report as text, with the home folder written as "~".
std::string bug_report();

// A file name for saving it, with the date and time: "wavemix-bug-report-20261027-0013.txt".
std::string suggested_file_name();

}  // namespace wavemix::report
