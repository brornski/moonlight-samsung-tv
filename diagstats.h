#pragma once

#include <stddef.h>
#include <stdint.h>

// Lightweight per-window streaming diagnostics. All functions are thread-safe.
// A report line is produced every DIAG_WINDOW_MS and posted to JS as a LogMsg.
// Only compiled in with MOONLIGHT_DIAG (make DIAG=1); otherwise every call is a no-op.
namespace diag {

#ifdef MOONLIGHT_DIAG

void Reset();

// Decoder thread: returns an id to use as PP_VideoPicture decode_id.
uint32_t BeginSubmit(uint64_t nowMs);
void EndSubmit(uint32_t bytes, bool idr, uint64_t receiveMs, uint64_t enqueueMs,
               uint16_t hostLatencyTenthsMs, uint64_t submitStartMs, uint64_t submitEndMs);

// Main thread
void OnPictureReady(uint32_t decodeId, bool droppedUnpainted);
void OnPaintStart();
void OnPaintFinished();
void OnInputEvent(double delayMs);

// Returns true and fills buf when a report window has elapsed.
bool MaybeFormatReport(char* buf, size_t len);

#else

inline void Reset() {}
inline uint32_t BeginSubmit(uint64_t) { return 0; }
inline void EndSubmit(uint32_t, bool, uint64_t, uint64_t, uint16_t, uint64_t, uint64_t) {}
inline void OnPictureReady(uint32_t, bool) {}
inline void OnPaintStart() {}
inline void OnPaintFinished() {}
inline void OnInputEvent(double) {}
inline bool MaybeFormatReport(char*, size_t) { return false; }

#endif

}  // namespace diag
