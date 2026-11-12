#ifndef _WIN32
#ifndef _POSIX_C_SOURCE
#define _POSIX_C_SOURCE 200809L
#endif
#endif

#include "video_rec.h"

#if SNESRECOMP_VIDEO_CAPTURE

#ifdef _WIN32

void VideoRecConfigure(void) {}
bool VideoRecAvailable(void) { return false; }
void VideoRecToggle(void) {}
bool VideoRecActive(void) { return false; }
void VideoRecPresent(const uint8_t *pixels, int pitch, int w, int h,
                     unsigned guest_frame) {
  (void)pixels;
  (void)pitch;
  (void)w;
  (void)h;
  (void)guest_frame;
}
void VideoRecShutdown(void) {}

#else

#include <errno.h>
#include <limits.h>
#include <signal.h>
#include <stdint.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <sys/time.h>
#include <sys/wait.h>
#include <time.h>
#include <unistd.h>

#include "host_paths.h"
#include "snes_osd.h"
#include "state_dump.h"

#ifndef SNES_GAME_VERSION
#define SNES_GAME_VERSION "dev"
#endif

#define VIDEO_REC_PATH_CAP 4096
#define VIDEO_REC_STEM_CAP 128
#define VIDEO_REC_TOAST_CAP 512

extern int snes_frame_counter;
extern int g_ws_extra;

/* Optional desktop-host metadata. Runner-only targets use this weak default;
 * the desktop host can provide a strong definition reading its current config.
 */
#if defined(__GNUC__) || defined(__clang__)
__attribute__((weak))
#endif
void VideoRecHostMeta(int *run_ahead, int *frame_blend) {
  if (run_ahead)
    *run_ahead = 0;
  if (frame_blend)
    *frame_blend = 0;
}

static bool s_configured;
static bool s_available;
static bool s_active;
static bool s_segment_open;
static char s_ffmpeg[VIDEO_REC_PATH_CAP];
static char s_session_stamp[32];
static char s_stem[VIDEO_REC_STEM_CAP];
static char s_mp4_leaf[VIDEO_REC_STEM_CAP + 16];
static char s_json_leaf[VIDEO_REC_STEM_CAP + 16];
static char s_csv_leaf[VIDEO_REC_STEM_CAP + 24];
static FILE *s_encoder;
static FILE *s_metadata;
static FILE *s_index;
static int s_width;
static int s_height;
static unsigned s_segment_number;
static uint64_t s_segment_frames;
static uint64_t s_total_frames;

static uint64_t wall_ms(void) {
  struct timeval tv;
  if (gettimeofday(&tv, NULL) == 0)
    return (uint64_t)tv.tv_sec * 1000u + (uint64_t)tv.tv_usec / 1000u;
  return (uint64_t)time(NULL) * 1000u;
}

static int make_timestamp(char *out, size_t cap) {
  time_t now = time(NULL);
  struct tm local;
  if (!localtime_r(&now, &local))
    return 0;
  return strftime(out, cap, "%Y%m%d_%H%M%S", &local) != 0;
}

static void breadcrumb(const char *message) {
  fprintf(stderr, "video_rec: %s\n", message ? message : "unknown error");
}

static void push_unavailable_toast(void) {
  snes_osd_push("video recording unavailable: ffmpeg not found", 3000);
}

/* Escape a path for a POSIX shell double-quoted word. */
static int shell_double_quote(const char *value, char *out, size_t cap) {
  size_t used = 0;
  if (!value || !out || cap < 3)
    return 0;
  out[used++] = '"';
  for (const unsigned char *p = (const unsigned char *)value; *p; p++) {
    if (*p == '\\' || *p == '"' || *p == '$' || *p == '`') {
      if (used + 2 >= cap)
        return 0;
      out[used++] = '\\';
    } else if (used + 1 >= cap) {
      return 0;
    }
    out[used++] = (char)*p;
  }
  if (used + 2 > cap)
    return 0;
  out[used++] = '"';
  out[used] = '\0';
  return 1;
}

static int preflight_ffmpeg(void) {
  if (strchr(s_ffmpeg, '/'))
    return access(s_ffmpeg, X_OK) == 0;

  char quoted[VIDEO_REC_PATH_CAP * 2 + 4];
  char command[VIDEO_REC_PATH_CAP * 2 + 64];
  if (!shell_double_quote(s_ffmpeg, quoted, sizeof(quoted)))
    return 0;
  int n =
      snprintf(command, sizeof(command), "%s -version >/dev/null 2>&1", quoted);
  if (n < 0 || (size_t)n >= sizeof(command))
    return 0;
  FILE *probe = popen(command, "r");
  if (!probe)
    return 0;
  int status = pclose(probe);
  return status >= 0 && WIFEXITED(status) && WEXITSTATUS(status) == 0;
}

void VideoRecConfigure(void) {
  if (s_configured)
    return;
  s_configured = true;
  if (signal(SIGPIPE, SIG_IGN) == SIG_ERR) {
    breadcrumb("could not ignore SIGPIPE; capture disabled");
    push_unavailable_toast();
    return;
  }

  const char *configured = getenv("SNESRECOMP_FFMPEG");
  if (!configured || !configured[0])
    configured = "ffmpeg";
  int n = snprintf(s_ffmpeg, sizeof(s_ffmpeg), "%s", configured);
  if (n < 0 || (size_t)n >= sizeof(s_ffmpeg)) {
    breadcrumb("SNESRECOMP_FFMPEG path is too long");
    push_unavailable_toast();
    return;
  }

  s_available = preflight_ffmpeg();
  if (!s_available) {
    char message[VIDEO_REC_PATH_CAP + 64];
    snprintf(message, sizeof(message), "ffmpeg preflight failed: %s", s_ffmpeg);
    breadcrumb(message);
    push_unavailable_toast();
  }
}

bool VideoRecAvailable(void) {
  VideoRecConfigure();
  return s_available;
}

bool VideoRecActive(void) { return s_active; }

static int artifact_file_path(const char *leaf, char *out, size_t cap) {
  if (!snesrecomp_artifact_path(leaf, out, cap)) {
    breadcrumb("could not resolve artifact path");
    return 0;
  }
  return 1;
}

static int make_segment_leaves(unsigned number) {
  int n;
  if (number == 1)
    n = snprintf(s_stem, sizeof(s_stem), "video_%s", s_session_stamp);
  else
    n = snprintf(s_stem, sizeof(s_stem), "video_%s_%02u", s_session_stamp,
                 number);
  if (n < 0 || (size_t)n >= sizeof(s_stem))
    return 0;
  n = snprintf(s_mp4_leaf, sizeof(s_mp4_leaf), "%s.mp4", s_stem);
  if (n < 0 || (size_t)n >= sizeof(s_mp4_leaf))
    return 0;
  n = snprintf(s_json_leaf, sizeof(s_json_leaf), "%s.json", s_stem);
  if (n < 0 || (size_t)n >= sizeof(s_json_leaf))
    return 0;
  n = snprintf(s_csv_leaf, sizeof(s_csv_leaf), "%s.frames.csv", s_stem);
  if (n < 0 || (size_t)n >= sizeof(s_csv_leaf))
    return 0;
  return 1;
}

static int path_exists(const char *leaf) {
  char path[VIDEO_REC_PATH_CAP];
  return artifact_file_path(leaf, path, sizeof(path)) &&
         access(path, F_OK) == 0;
}

static int write_json_string(FILE *f, const char *value) {
  if (fputc('"', f) == EOF)
    return 0;
  for (const unsigned char *p = (const unsigned char *)(value ? value : ""); *p;
       p++) {
    switch (*p) {
    case '"':
      if (fputs("\\\"", f) == EOF)
        return 0;
      break;
    case '\\':
      if (fputs("\\\\", f) == EOF)
        return 0;
      break;
    case '\b':
      if (fputs("\\b", f) == EOF)
        return 0;
      break;
    case '\f':
      if (fputs("\\f", f) == EOF)
        return 0;
      break;
    case '\n':
      if (fputs("\\n", f) == EOF)
        return 0;
      break;
    case '\r':
      if (fputs("\\r", f) == EOF)
        return 0;
      break;
    case '\t':
      if (fputs("\\t", f) == EOF)
        return 0;
      break;
    default:
      if (*p < 0x20) {
        if (fprintf(f, "\\u%04x", (unsigned)*p) < 0)
          return 0;
      } else if (fputc(*p, f) == EOF) {
        return 0;
      }
      break;
    }
  }
  return fputc('"', f) != EOF;
}

static int choose_segment_paths(char *mp4_path, size_t mp4_cap, char *json_path,
                                size_t json_cap, char *csv_path,
                                size_t csv_cap) {
  char artifact_dir[VIDEO_REC_PATH_CAP];
  if (!snesrecomp_artifact_dir(artifact_dir, sizeof(artifact_dir))) {
    breadcrumb("could not create artifact directory");
    return 0;
  }
  (void)artifact_dir;

  for (unsigned number = s_segment_number + 1; number != 0; number++) {
    if (!make_segment_leaves(number))
      return 0;
    if (path_exists(s_mp4_leaf) || path_exists(s_json_leaf) ||
        path_exists(s_csv_leaf))
      continue;
    if (!artifact_file_path(s_mp4_leaf, mp4_path, mp4_cap) ||
        !artifact_file_path(s_json_leaf, json_path, json_cap) ||
        !artifact_file_path(s_csv_leaf, csv_path, csv_cap))
      return 0;
    s_segment_number = number;
    return 1;
  }
  return 0;
}

static int write_segment_metadata(unsigned guest_frame) {
  int run_ahead = 0;
  int frame_blend = 0;
  VideoRecHostMeta(&run_ahead, &frame_blend);
  uint64_t start_ms = wall_ms();
  double display_aspect = (double)s_width / (double)s_height;

  if (fprintf(s_metadata,
              "{\n  \"start_wall_ms\": %llu,\n"
              "  \"start_guest_frame\": %u,\n"
              "  \"width\": %d,\n  \"height\": %d,\n"
              "  \"fps_nominal\": 60,\n"
              "  \"fps_note\": \"fixed 60 fps; guest NTSC is ~60.0988 fps "
              "(0.16%% drift)\",\n"
              "  \"wall_ms_note\": \"Wall-clock gaps between rows preserve "
              "timing across skipped overlay presents.\",\n"
              "  \"g_ws_extra\": %d,\n"
              "  \"display_aspect\": %.9g,\n"
              "  \"run_ahead\": %d,\n"
              "  \"frame_blend\": %d,\n"
              "  \"build_version\": ",
              (unsigned long long)start_ms, guest_frame, s_width, s_height,
              g_ws_extra, display_aspect, run_ahead, frame_blend) < 0 ||
      !write_json_string(s_metadata, SNES_GAME_VERSION) ||
      fputs("\n}\n", s_metadata) == EOF || fflush(s_metadata) != 0) {
    breadcrumb("could not write video sidecar");
    return 0;
  }

  if (fprintf(s_index, "present_index,guest_frame,wall_ms\n") < 0 ||
      fflush(s_index) != 0) {
    breadcrumb("could not initialize frame index");
    return 0;
  }
  return 1;
}

static int start_segment(int w, int h, unsigned guest_frame) {
  char mp4_path[VIDEO_REC_PATH_CAP];
  char json_path[VIDEO_REC_PATH_CAP];
  char csv_path[VIDEO_REC_PATH_CAP];
  char quoted_ffmpeg[VIDEO_REC_PATH_CAP * 2 + 4];
  char quoted_mp4[VIDEO_REC_PATH_CAP * 2 + 4];
  char command[VIDEO_REC_PATH_CAP * 4 + 1024];

  s_width = w;
  s_height = h;
  if (!choose_segment_paths(mp4_path, sizeof(mp4_path), json_path,
                            sizeof(json_path), csv_path, sizeof(csv_path))) {
    breadcrumb("could not select video artifact paths");
    return 0;
  }

  s_metadata = fopen(json_path, "w");
  s_index = fopen(csv_path, "w");
  if (!s_metadata || !s_index) {
    breadcrumb("could not open video sidecar or frame index");
    if (s_metadata)
      fclose(s_metadata);
    if (s_index)
      fclose(s_index);
    s_metadata = NULL;
    s_index = NULL;
    return 0;
  }
  if (!write_segment_metadata(guest_frame)) {
    fclose(s_metadata);
    fclose(s_index);
    s_metadata = NULL;
    s_index = NULL;
    return 0;
  }

  if (!shell_double_quote(s_ffmpeg, quoted_ffmpeg, sizeof(quoted_ffmpeg)) ||
      !shell_double_quote(mp4_path, quoted_mp4, sizeof(quoted_mp4))) {
    breadcrumb("could not quote video command path");
    fclose(s_metadata);
    fclose(s_index);
    s_metadata = NULL;
    s_index = NULL;
    return 0;
  }
  int n = snprintf(command, sizeof(command),
                   "%s -hide_banner -loglevel error -f rawvideo -pix_fmt bgr0 "
                   "-s %dx%d -r 60 -i pipe:0 "
                   "-vf 'pad=ceil(iw/2)*2:ceil(ih/2)*2' "
                   "-c:v libx264 -preset ultrafast -crf 18 -pix_fmt yuv420p "
                   "-movflags +faststart %s",
                   quoted_ffmpeg, w, h, quoted_mp4);
  if (n < 0 || (size_t)n >= sizeof(command)) {
    breadcrumb("video command is too long");
    fclose(s_metadata);
    fclose(s_index);
    s_metadata = NULL;
    s_index = NULL;
    return 0;
  }

  s_encoder = popen(command, "w");
  if (!s_encoder) {
    breadcrumb("could not start ffmpeg encoder");
    fclose(s_metadata);
    fclose(s_index);
    s_metadata = NULL;
    s_index = NULL;
    return 0;
  }

  s_segment_frames = 0;
  s_segment_open = true;
  return 1;
}

static int close_segment(void) {
  int ok = 1;
  if (!s_segment_open)
    return 1;

  if (s_index) {
    if (fflush(s_index) != 0)
      ok = 0;
    if (fclose(s_index) != 0)
      ok = 0;
    s_index = NULL;
  }
  if (s_metadata) {
    if (fflush(s_metadata) != 0)
      ok = 0;
    if (fclose(s_metadata) != 0)
      ok = 0;
    s_metadata = NULL;
  }
  if (s_encoder) {
    if (fflush(s_encoder) != 0)
      ok = 0;
    int status = pclose(s_encoder);
    s_encoder = NULL;
    if (status < 0 || !WIFEXITED(status) || WEXITSTATUS(status) != 0)
      ok = 0;
  }
  s_total_frames += s_segment_frames;
  s_segment_frames = 0;
  s_segment_open = false;
  return ok;
}

static void write_stop_dump(void) {
  char dir[VIDEO_REC_PATH_CAP];
  char stamp[32];
  char tag[64];
  uint32_t frame = snes_frame_counter ? (uint32_t)(snes_frame_counter - 1) : 0;
  if (!snesrecomp_artifact_dir(dir, sizeof(dir))) {
    breadcrumb("could not resolve artifact directory for stop dump");
    return;
  }
  if (!make_timestamp(stamp, sizeof(stamp)))
    strcpy(stamp, "unknown");
  snprintf(tag, sizeof(tag), "rec_stop_%s", stamp);
  if (snes_state_dump(dir, tag, NULL, 0, 0, 0, frame) != 0)
    breadcrumb("state dump failed after recording stop");
}

static void stop_recording(bool encoder_error, bool dump_state) {
  if (!s_active)
    return;
  if (!close_segment())
    encoder_error = true;
  s_active = false;

  if (dump_state)
    write_stop_dump();

  char toast[VIDEO_REC_TOAST_CAP];
  if (s_stem[0]) {
    snprintf(toast, sizeof(toast),
             "%s: %s.mp4 (+%s.json, %s.frames.csv; %llu frames)",
             encoder_error ? "recording stopped (encoder error)"
                           : "REC stopped",
             s_stem, s_stem, s_stem, (unsigned long long)s_total_frames);
  } else {
    snprintf(toast, sizeof(toast), "%s%s",
             encoder_error ? "recording stopped (encoder error)"
                           : "REC stopped",
             s_total_frames ? "" : " (no frames)");
  }
  snes_osd_push(toast, 3500);
  if (encoder_error)
    breadcrumb("recording stopped after an encoder or file error");
}

static int begin_recording(void) {
  char dir[VIDEO_REC_PATH_CAP];
  if (!VideoRecAvailable())
    return 0;
  if (!snesrecomp_artifact_dir(dir, sizeof(dir))) {
    breadcrumb("could not create artifact directory");
    snes_osd_push("recording could not start: artifact directory unavailable",
                  3000);
    return 0;
  }
  if (!make_timestamp(s_session_stamp, sizeof(s_session_stamp))) {
    breadcrumb("could not create recording timestamp");
    snes_osd_push("recording could not start: clock unavailable", 3000);
    return 0;
  }
  (void)dir;
  s_stem[0] = '\0';
  s_segment_number = 0;
  s_segment_frames = 0;
  s_total_frames = 0;
  s_active = true;
  return 1;
}

void VideoRecToggle(void) {
  if (s_active) {
    stop_recording(false, true);
    return;
  }
  (void)begin_recording();
}

void VideoRecPresent(const uint8_t *pixels, int pitch, int w, int h,
                     unsigned guest_frame) {
  if (!s_active)
    return;
  if (!pixels || w <= 0 || h <= 0 || w > INT_MAX / 4 || pitch < w * 4) {
    breadcrumb("invalid frame passed to active recorder");
    stop_recording(true, true);
    return;
  }

  if (!s_segment_open) {
    if (!start_segment(w, h, guest_frame)) {
      stop_recording(true, true);
      return;
    }
  } else if (w != s_width || h != s_height) {
    if (!close_segment()) {
      stop_recording(true, true);
      return;
    }
    if (!start_segment(w, h, guest_frame)) {
      stop_recording(true, true);
      return;
    }
  }

  const size_t row_bytes = (size_t)w * 4u;
  for (int y = 0; y < h; y++) {
    const uint8_t *row = pixels + (size_t)y * (size_t)pitch;
    errno = 0;
    if (fwrite(row, 1, row_bytes, s_encoder) != row_bytes) {
      if (!errno)
        errno = EIO;
      char message[160];
      snprintf(message, sizeof(message), "encoder pipe write failed: %s",
               strerror(errno));
      breadcrumb(message);
      stop_recording(true, true);
      return;
    }
  }

  uint64_t present_ms = wall_ms();
  if (fprintf(s_index, "%llu,%u,%llu\n", (unsigned long long)s_segment_frames,
              guest_frame, (unsigned long long)present_ms) < 0 ||
      fflush(s_index) != 0) {
    breadcrumb("could not append frame index");
    stop_recording(true, true);
    return;
  }
  s_segment_frames++;
}

void VideoRecShutdown(void) {
  if (s_active)
    stop_recording(false, true);
}

#endif /* !_WIN32 */
#endif /* SNESRECOMP_VIDEO_CAPTURE */
