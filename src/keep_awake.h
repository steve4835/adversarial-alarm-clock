#pragma once

// Call from alarm.cpp::dismiss() when it silenced an alarm that was actually
// ringing. Begins the 15-min-interval chirp sequence for the next 2 hours.
void startKeepAwake();

// Immediately ends any in-progress keep-awake sequence (silences the buzzer
// if currently chirping). No-op if none is active.
void cancelKeepAwake();

// Call every loop iteration; non-blocking.
void handleKeepAwake();

// POST /dismissKeepAwake — silences the current chirp if one is sounding, or
// pre-empts the next one if called within its 5-minute dismiss window.
// Returns false if called outside any dismiss window (no-op).
bool dismissKeepAwake();

// True while a keep-awake sequence is in progress (waiting or chirping).
bool isKeepAwakeActive();

// True only while a chirp is sounding, or within 5 minutes of the next one —
// i.e. exactly when the dismiss button/endpoint should be usable.
bool keepAwakeDismissWindowOpen();
