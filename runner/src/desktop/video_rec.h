#ifndef SNESRECOMP_VIDEO_REC_H
#define SNESRECOMP_VIDEO_REC_H

#include <stdbool.h>
#include <stdint.h>

#ifndef SNESRECOMP_VIDEO_CAPTURE
#define SNESRECOMP_VIDEO_CAPTURE 0
#endif

#ifdef __cplusplus
extern "C" {
#endif

#if SNESRECOMP_VIDEO_CAPTURE
void VideoRecConfigure(void);
bool VideoRecAvailable(void);
void VideoRecToggle(void);
bool VideoRecActive(void);
/* guest_frame is the just-completed simulated frame, normally
 * snes_frame_counter ? snes_frame_counter - 1 : 0. It is not the host's
 * post-increment present counter; the index maps each present to guest time. */
void VideoRecPresent(const uint8_t *pixels, int pitch, int w, int h,
                     unsigned guest_frame);
/* Shutdown/stop waits for ffmpeg to finish its faststart moov rewrite, which
 * can briefly block on large captures. Crash exits bypass the stop-state dump,
 * but closing the pipe still lets ffmpeg finalize a playable MP4. */
void VideoRecShutdown(void);
#else
static inline void VideoRecConfigure(void) {}
static inline bool VideoRecAvailable(void) { return false; }
static inline void VideoRecToggle(void) {}
static inline bool VideoRecActive(void) { return false; }
static inline void VideoRecPresent(const uint8_t *pixels, int pitch, int w,
                                   int h, unsigned guest_frame) {
  (void)pixels;
  (void)pitch;
  (void)w;
  (void)h;
  (void)guest_frame;
}
static inline void VideoRecShutdown(void) {}
#endif

#ifdef __cplusplus
}
#endif

#endif /* SNESRECOMP_VIDEO_REC_H */
