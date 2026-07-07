// saf_bridge.hh — native ↔ Java bridge for Storage Access Framework file I/O.
//
// Copyright (C) 2026 nava. Licensed under the GNU AGPLv3 or later; see LICENSE.
//
// The app is a NativeActivity subclass (io.nava.calculator.MainActivity) that
// owns the SAF dialogs. These helpers call into it from the native thread to
// launch the system "create document" / "open document" pickers; the imported
// JSON comes back asynchronously (onActivityResult → JNI) and is delivered to
// pollImport() on the render thread.
#pragma once
#include <string>

struct android_app;

namespace saf {

// Launch the system file picker to write `json` to a user-chosen location.
void requestExport(android_app* app, const std::string& json, const std::string& suggestedName);

// Launch the system file picker to choose a JSON file to import.
void requestImport(android_app* app);

// Non-blocking: if an imported document has arrived since the last call, move it
// into `out` and return true (consume-once). Poll each frame from the loop.
bool pollImport(std::string& out);

// Fire the standard Android key-click haptic (MainActivity.hapticTick via JNI).
void haptic(android_app* app);

}  // namespace saf
