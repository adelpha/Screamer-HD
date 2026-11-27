#pragma once
// Debug builds: assertions write the call stack to assert.txt beside the executable.
// no_dialogs: assertions and abort() never open a window (headless runs, tests, tools).
void install_debug_report(bool no_dialogs);

// Windows-subsystem builds (release packages): with no standard output given (started from a
// console, not piped), print to the parent's console, if there is one, so --help and the log
// show there. Does nothing elsewhere.
void attach_parent_console();
