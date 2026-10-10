#pragma once

#include <stddef.h>
#include <stdint.h>

// Lightweight per-window streaming diagnostics. All functions are thread-safe.
// A report line is produced every DIAG_WINDOW_MS and posted to JS as a LogMsg.
namespace diag {

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

}  // namespace diag
