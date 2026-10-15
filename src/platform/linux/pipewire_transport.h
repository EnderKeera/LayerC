#pragma once

#include <stddef.h>
#include <stdint.h>

#ifdef __cplusplus
extern "C" {
#endif

// C confinement for SPA's C compound-literal pod builders. Buffer storage is
// borrowed only for the duration of frame(); no SPA type reaches the host.
struct layerc_pw_capture;
struct layerc_pw_frame {
  const void *data;
  size_t size;
  uint32_t width, height, stride;
  unsigned format; // 1 BGRx, 2 BGRA, 3 RGBx, 4 RGBA
};
typedef void (*layerc_pw_error_cb)(void *, const char *);
typedef void (*layerc_pw_frame_cb)(void *, const struct layerc_pw_frame *);

// Consumes fd on success AND failure. Uses no data-loop threads; runtimes
// before PipeWire 1.2 fail before context creation.
struct layerc_pw_capture *layerc_pw_capture_new(
    int fd, uint32_t node, void *userdata, layerc_pw_error_cb error,
    layerc_pw_frame_cb frame, const char **reason);
void layerc_pw_capture_destroy(struct layerc_pw_capture *capture);
int layerc_pw_capture_fd(struct layerc_pw_capture *capture);
int layerc_pw_capture_iterate(struct layerc_pw_capture *capture);

#ifdef __cplusplus
}
#endif
