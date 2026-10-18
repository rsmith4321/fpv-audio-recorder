#pragma once
// Host test: no USB host, so re-attach requests are no-ops.
inline bool tud_mounted() { return false; }
inline bool tud_disconnect() { return true; }
inline bool tud_connect() { return true; }
