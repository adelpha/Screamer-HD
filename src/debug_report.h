#pragma once
// Debug builds: assertions write the call stack to assert.txt beside the executable.
// no_dialogs: assertions and abort() never open a window (headless runs, tests, tools).
void install_debug_report(bool no_dialogs);
