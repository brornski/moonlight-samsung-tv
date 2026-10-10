#include "diagstats.h"

#include <Limelight.h>

#include <pthread.h>
#include <stdio.h>
#include <string.h>

#define DIAG_WINDOW_MS 2000
#define DIAG_RING_SIZE 512

namespace diag {

namespace {

struct Stat {
    uint64_t sum;
    uint64_t max;
    uint32_t count;

    void Add(uint64_t v) {
        sum += v;
        if (v > max) max = v;
        count++;
    }
    uint64_t Avg() const { return count ? sum / count : 0; }
};

struct Window {
    uint64_t startMs;
    uint64_t bytes;
    uint32_t idrFrames;
    Stat hostLatency;   // tenths of a millisecond, as reported by the host
    Stat receive;       // first packet -> frame fully reassembled
    Stat queueWait;     // reassembled -> handed to our decoder callback
    Stat decodeBlock;   // time spent inside pp::VideoDecoder::Decode()
    Stat decodeLatency; // Decode() called -> picture ready on main thread
    Stat paint;         // draw + SwapBuffers completion
    Stat inputDelay;    // input event timestamp -> handled by the plugin
    uint32_t picturesDropped;
};

pthread_mutex_t s_Lock = PTHREAD_MUTEX_INITIALIZER;
Window s_Window;
uint64_t s_SubmitTimes[DIAG_RING_SIZE];
uint32_t s_NextDecodeId;
uint64_t s_PaintStartMs;

}  // namespace

void Reset() {
    pthread_mutex_lock(&s_Lock);
    memset(&s_Window, 0, sizeof(s_Window));
    memset(s_SubmitTimes, 0, sizeof(s_SubmitTimes));
    s_Window.startMs = LiGetMillis();
    s_PaintStartMs = 0;
    pthread_mutex_unlock(&s_Lock);
}

uint32_t BeginSubmit(uint64_t nowMs) {
    pthread_mutex_lock(&s_Lock);
    uint32_t id = ++s_NextDecodeId;
    s_SubmitTimes[id % DIAG_RING_SIZE] = nowMs;
    pthread_mutex_unlock(&s_Lock);
    return id;
}

void EndSubmit(uint32_t bytes, bool idr, uint64_t receiveMs, uint64_t enqueueMs,
               uint16_t hostLatencyTenthsMs, uint64_t submitStartMs, uint64_t submitEndMs) {
    pthread_mutex_lock(&s_Lock);
    s_Window.bytes += bytes;
    if (idr) s_Window.idrFrames++;
    if (hostLatencyTenthsMs != 0) s_Window.hostLatency.Add(hostLatencyTenthsMs);
    if (receiveMs != 0 && enqueueMs >= receiveMs) s_Window.receive.Add(enqueueMs - receiveMs);
    if (enqueueMs != 0 && submitStartMs >= enqueueMs) s_Window.queueWait.Add(submitStartMs - enqueueMs);
    s_Window.decodeBlock.Add(submitEndMs - submitStartMs);
    pthread_mutex_unlock(&s_Lock);
}

void OnPictureReady(uint32_t decodeId, bool droppedUnpainted) {
    uint64_t now = LiGetMillis();
    pthread_mutex_lock(&s_Lock);
    uint64_t submitted = s_SubmitTimes[decodeId % DIAG_RING_SIZE];
    if (submitted != 0 && now >= submitted) s_Window.decodeLatency.Add(now - submitted);
    if (droppedUnpainted) s_Window.picturesDropped++;
    pthread_mutex_unlock(&s_Lock);
}

void OnPaintStart() {
    uint64_t now = LiGetMillis();
    pthread_mutex_lock(&s_Lock);
    s_PaintStartMs = now;
    pthread_mutex_unlock(&s_Lock);
}

void OnPaintFinished() {
    uint64_t now = LiGetMillis();
    pthread_mutex_lock(&s_Lock);
    if (s_PaintStartMs != 0 && now >= s_PaintStartMs) s_Window.paint.Add(now - s_PaintStartMs);
    s_PaintStartMs = 0;
    pthread_mutex_unlock(&s_Lock);
}

void OnInputEvent(double delayMs) {
    if (delayMs < 0) delayMs = 0;
    pthread_mutex_lock(&s_Lock);
    s_Window.inputDelay.Add((uint64_t)delayMs);
    pthread_mutex_unlock(&s_Lock);
}

bool MaybeFormatReport(char* buf, size_t len) {
    uint64_t now = LiGetMillis();
    pthread_mutex_lock(&s_Lock);
    uint64_t elapsed = now - s_Window.startMs;
    if (s_Window.startMs == 0 || elapsed < DIAG_WINDOW_MS) {
        pthread_mutex_unlock(&s_Lock);
        return false;
    }
    Window w = s_Window;
    memset(&s_Window, 0, sizeof(s_Window));
    s_Window.startMs = now;
    pthread_mutex_unlock(&s_Lock);

    uint32_t rtt = 0, rttVar = 0;
    LiGetEstimatedRttInfo(&rtt, &rttVar);

    snprintf(buf, len,
             "fps=%.1f mbps=%.1f idr=%u host=%llu.%llu/%llu.%llums recv=%llu/%llums qwait=%llu/%llums "
             "decblock=%llu/%llums declat=%llu/%llums paint=%llu/%llums painted=%u dropped=%u "
             "input=%u delay=%llu/%llums rtt=%u+-%ums",
             w.decodeBlock.count * 1000.0 / elapsed,
             w.bytes * 8.0 / 1000.0 / elapsed,
             w.idrFrames,
             (unsigned long long)(w.hostLatency.Avg() / 10), (unsigned long long)(w.hostLatency.Avg() % 10),
             (unsigned long long)(w.hostLatency.max / 10), (unsigned long long)(w.hostLatency.max % 10),
             (unsigned long long)w.receive.Avg(), (unsigned long long)w.receive.max,
             (unsigned long long)w.queueWait.Avg(), (unsigned long long)w.queueWait.max,
             (unsigned long long)w.decodeBlock.Avg(), (unsigned long long)w.decodeBlock.max,
             (unsigned long long)w.decodeLatency.Avg(), (unsigned long long)w.decodeLatency.max,
             (unsigned long long)w.paint.Avg(), (unsigned long long)w.paint.max,
             w.paint.count, w.picturesDropped,
             w.inputDelay.count,
             (unsigned long long)w.inputDelay.Avg(), (unsigned long long)w.inputDelay.max,
             rtt, rttVar);
    return true;
}

}  // namespace diag
