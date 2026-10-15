#include "pipewire_transport.h"

#include <pipewire/pipewire.h>
#include <pipewire/impl.h>
#include <spa/param/video/format-utils.h>
#include <spa/pod/builder.h>
#include <spa/buffer/buffer.h>
#include <spa/support/log-impl.h>

#include <sys/mman.h>
#include <sys/stat.h>
#include <unistd.h>

#include <errno.h>
#include <stdbool.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>

#define MAX_BUFFER_BYTES (512u * 1024u * 1024u)
#define MAX_DIMENSION 16384u

// The default journal logger leaves libsystemd's process-global socket open
// after pw_deinit. Keep library diagnostics on the already-owned stderr instead
// of allowing demand to acquire a persistent journal/file logging descriptor.
static struct layerc_pw_log {
  struct spa_log log;
  struct spa_log_methods methods;
} layerc_log;

struct layerc_pw_capture {
  struct pw_loop *loop;
  struct pw_context *context;
  struct pw_core *core;
  struct pw_stream *stream;
  struct spa_hook core_listener, stream_listener;
  struct spa_video_info_raw info;
  void *userdata;
  layerc_pw_error_cb error;
  layerc_pw_frame_cb frame;
  unsigned format;
  bool core_hooked, stream_hooked, connected;
};

static void failure(struct layerc_pw_capture *self, const char *reason) {
  if (self->error) self->error(self->userdata, reason);
}

static void core_error(void *data, uint32_t id, int seq, int result, const char *message) {
  (void)seq; (void)message;
  if (id == PW_ID_CORE || result == -EPIPE || result == -ECONNRESET)
    failure(data, "pipewire_connection_lost");
}

static void state_changed(void *data, enum pw_stream_state old,
                          enum pw_stream_state state, const char *message) {
  (void)old; (void)message;
  struct layerc_pw_capture *self = data;
  if (state == PW_STREAM_STATE_ERROR ||
      (self->connected && state == PW_STREAM_STATE_UNCONNECTED))
    failure(self, "pipewire_stream_lost");
  if (state >= PW_STREAM_STATE_PAUSED) self->connected = true;
}

static void param_changed(void *data, uint32_t id, const struct spa_pod *param) {
  struct layerc_pw_capture *self = data;
  if (!param || id != SPA_PARAM_Format) return;
  uint32_t media_type, media_subtype;
  if (spa_format_parse(param, &media_type, &media_subtype) < 0 ||
      media_type != SPA_MEDIA_TYPE_video || media_subtype != SPA_MEDIA_SUBTYPE_raw ||
      spa_format_video_raw_parse(param, &self->info) < 0) {
    failure(self, "pipewire_format_unsupported");
    return;
  }
  switch (self->info.format) {
  case SPA_VIDEO_FORMAT_BGRx: self->format = 1; break;
  case SPA_VIDEO_FORMAT_BGRA: self->format = 2; break;
  case SPA_VIDEO_FORMAT_RGBx: self->format = 3; break;
  case SPA_VIDEO_FORMAT_RGBA: self->format = 4; break;
  default: failure(self, "pipewire_format_unsupported"); return;
  }
  const uint32_t width = self->info.size.width, height = self->info.size.height;
  const uint64_t bytes = (uint64_t)width * height * 4;
  if (!width || !height || width > MAX_DIMENSION || height > MAX_DIMENSION ||
      bytes > MAX_BUFFER_BYTES) {
    failure(self, "pipewire_geometry_unsupported");
    return;
  }
  uint8_t storage[512];
  struct spa_pod_builder builder = SPA_POD_BUILDER_INIT(storage, sizeof(storage));
  const struct spa_pod *params[1];
  params[0] = spa_pod_builder_add_object(&builder,
      SPA_TYPE_OBJECT_ParamBuffers, SPA_PARAM_Buffers,
      SPA_PARAM_BUFFERS_buffers, SPA_POD_CHOICE_RANGE_Int(4, 2, 8),
      SPA_PARAM_BUFFERS_blocks, SPA_POD_Int(1),
      SPA_PARAM_BUFFERS_size, SPA_POD_Int((int32_t)bytes),
      SPA_PARAM_BUFFERS_stride, SPA_POD_Int((int32_t)(width * 4)),
      SPA_PARAM_BUFFERS_dataType, SPA_POD_CHOICE_FLAGS_Int(
          (1u << SPA_DATA_MemPtr) | (1u << SPA_DATA_MemFd)));
  if (!params[0] || pw_stream_update_params(self->stream, params, 1) < 0)
    failure(self, "pipewire_buffer_negotiation_failed");
}

static void consume(struct layerc_pw_capture *self, const struct spa_buffer *buffer) {
  if (!self->format) return;
  if (!buffer || buffer->n_datas != 1) {
    failure(self, "pipewire_buffer_invalid");
    return;
  }
  const struct spa_data *data = &buffer->datas[0];
  if (data->type == SPA_DATA_DmaBuf) {
    failure(self, "dma_buf_only");
    return;
  }
  if (data->type != SPA_DATA_MemPtr && data->type != SPA_DATA_MemFd) {
    failure(self, "pipewire_buffer_unsupported");
    return;
  }
  const struct spa_chunk *chunk = data->chunk;
  if (!chunk || chunk->stride <= 0 || !chunk->size) return; // an empty/dropped frame
  if (data->maxsize > MAX_BUFFER_BYTES || chunk->offset > data->maxsize ||
      chunk->size > data->maxsize - chunk->offset) {
    failure(self, "pipewire_buffer_invalid");
    return;
  }
  const void *pixels = data->data;
  void *mapping = MAP_FAILED;
  size_t length = 0;
  if (!pixels && data->type == SPA_DATA_MemFd) {
    const uint64_t mapped_size = (uint64_t)data->mapoffset + data->maxsize;
    struct stat info;
    if (data->fd < 0 || mapped_size > MAX_BUFFER_BYTES ||
        fstat(data->fd, &info) < 0 || info.st_size < 0 ||
        (uint64_t)info.st_size < mapped_size) {
      failure(self, "pipewire_buffer_invalid");
      return;
    }
    length = (size_t)mapped_size;
    mapping = mmap(NULL, length, PROT_READ, MAP_SHARED, (int)data->fd, 0);
    if (mapping != MAP_FAILED) pixels = (const uint8_t *)mapping + data->mapoffset;
  }
  if (!pixels) {
    failure(self, "pipewire_buffer_unavailable");
  } else {
    const struct layerc_pw_frame frame = {
      .data = (const uint8_t *)pixels + chunk->offset, .size = chunk->size,
      .width = self->info.size.width, .height = self->info.size.height,
      .stride = (uint32_t)chunk->stride, .format = self->format};
    if (self->frame) self->frame(self->userdata, &frame);
  }
  if (mapping != MAP_FAILED) munmap(mapping, length);
}

static void process(void *data) {
  struct layerc_pw_capture *self = data;
  // Keep only the newest complete buffer, bounding dispatch and ownership.
  struct pw_buffer *latest = NULL, *next;
  unsigned count = 0;
  while (count++ < 64 && (next = pw_stream_dequeue_buffer(self->stream))) {
    if (latest) pw_stream_queue_buffer(self->stream, latest);
    latest = next;
  }
  if (!latest) return;
  consume(self, latest->buffer);
  pw_stream_queue_buffer(self->stream, latest);
}

struct layerc_pw_capture *layerc_pw_capture_new(
    int fd, uint32_t node, void *userdata, layerc_pw_error_cb error,
    layerc_pw_frame_cb frame, const char **reason) {
  unsigned major = 0, minor = 0;
  if (sscanf(pw_get_library_version(), "%u.%u", &major, &minor) != 2 ||
      (major == 0 || (major == 1 && minor < 2))) {
    if (fd >= 0) close(fd);
    *reason = "pipewire_version_requires_1_2";
    return NULL;
  }
  // Custom client configs may execute programs or enable data-loop threads.
  // Refuse that override rather than changing the process-wide environment.
  const char *config = getenv("PIPEWIRE_CONFIG_NAME");
  if (config && strcmp(config, "null") != 0) {
    if (fd >= 0) close(fd);
    *reason = "pipewire_config_override";
    return NULL;
  }
  if (fd < 0 || node == SPA_ID_INVALID) {
    if (fd >= 0) close(fd);
    *reason = "pipewire_remote_invalid";
    return NULL;
  }
  struct layerc_pw_capture *self = calloc(1, sizeof(*self));
  if (!self) {
    close(fd); *reason = "pipewire_allocation_failed"; return NULL;
  }
  // PipeWire >=0.3.49 pairs each init/deinit. The library frees its
  // global resources when the last demanded capture drops its lease.
  layerc_log = (struct layerc_pw_log)SPA_LOG_IMPL_INIT(layerc_log);
  pw_log_set(&layerc_log.log);
  pw_init(NULL, NULL);
  self->userdata = userdata; self->error = error; self->frame = frame;
  self->loop = pw_loop_new(NULL);
  if (!self->loop) goto failed;
  pw_loop_enter(self->loop); // this host thread owns the loop for its lifetime
  struct pw_properties *properties = pw_properties_new(
      PW_KEY_CONFIG_NAME, "null", "context.num-data-loops", "0",
      "context.data-loops", "[]", PW_KEY_APP_NAME, "LayerC", NULL);
  if (!properties) goto failed;
  self->context = pw_context_new(self->loop, properties, 0);
  if (!self->context) goto failed;
  pw_context_add_spa_lib(self->context, "video.convert.*", "videoconvert/libspa-videoconvert");
  if (!pw_context_load_module(self->context, "libpipewire-module-protocol-native", NULL, NULL) ||
      !pw_context_load_module(self->context, "libpipewire-module-client-node", NULL, NULL) ||
      !pw_context_load_module(self->context, "libpipewire-module-adapter", NULL, NULL))
    goto failed;
  self->core = pw_context_connect_fd(self->context, fd, NULL, 0);
  fd = -1; // connect_fd closes the descriptor even on error.
  if (!self->core) goto failed;
  static const struct pw_core_events core_events = {
      PW_VERSION_CORE_EVENTS, .error = core_error};
  pw_core_add_listener(self->core, &self->core_listener, &core_events, self);
  self->core_hooked = true;
  self->stream = pw_stream_new(self->core, "LayerC ScreenCast",
      pw_properties_new(PW_KEY_MEDIA_TYPE, "Video", PW_KEY_MEDIA_CATEGORY, "Capture",
                        PW_KEY_MEDIA_ROLE, "Screen", NULL));
  if (!self->stream) goto failed;
  static const struct pw_stream_events events = {
      PW_VERSION_STREAM_EVENTS, .state_changed = state_changed,
      .param_changed = param_changed, .process = process};
  pw_stream_add_listener(self->stream, &self->stream_listener, &events, self);
  self->stream_hooked = true;

  uint8_t storage[1024];
  struct spa_pod_builder builder = SPA_POD_BUILDER_INIT(storage, sizeof(storage));
  const struct spa_pod *params[1];
  params[0] = spa_pod_builder_add_object(&builder,
      SPA_TYPE_OBJECT_Format, SPA_PARAM_EnumFormat,
      SPA_FORMAT_mediaType, SPA_POD_Id(SPA_MEDIA_TYPE_video),
      SPA_FORMAT_mediaSubtype, SPA_POD_Id(SPA_MEDIA_SUBTYPE_raw),
      SPA_FORMAT_VIDEO_format, SPA_POD_CHOICE_ENUM_Id(5, SPA_VIDEO_FORMAT_BGRx,
          SPA_VIDEO_FORMAT_BGRx, SPA_VIDEO_FORMAT_BGRA, SPA_VIDEO_FORMAT_RGBx, SPA_VIDEO_FORMAT_RGBA),
      SPA_FORMAT_VIDEO_size, SPA_POD_CHOICE_RANGE_Rectangle(
          &SPA_RECTANGLE(1920, 1080), &SPA_RECTANGLE(1, 1), &SPA_RECTANGLE(MAX_DIMENSION, MAX_DIMENSION)),
      SPA_FORMAT_VIDEO_framerate, SPA_POD_CHOICE_RANGE_Fraction(
          &SPA_FRACTION(30, 1), &SPA_FRACTION(0, 1), &SPA_FRACTION(60, 1)));
  const enum pw_stream_flags flags = PW_STREAM_FLAG_AUTOCONNECT |
      PW_STREAM_FLAG_MAP_BUFFERS | PW_STREAM_FLAG_DONT_RECONNECT |
      PW_STREAM_FLAG_NO_CONVERT; // never RT_PROCESS
  if (!params[0] || pw_stream_connect(self->stream, PW_DIRECTION_INPUT, node, flags, params, 1) < 0)
    goto failed;
  *reason = NULL;
  return self;
failed:
  if (fd >= 0) close(fd);
  layerc_pw_capture_destroy(self);
  *reason = "pipewire_start_failed";
  return NULL;
}

void layerc_pw_capture_destroy(struct layerc_pw_capture *self) {
  if (!self) return;
  if (self->stream_hooked) spa_hook_remove(&self->stream_listener);
  if (self->stream) pw_stream_destroy(self->stream);
  if (self->core_hooked) spa_hook_remove(&self->core_listener);
  if (self->core) pw_core_disconnect(self->core);
  if (self->context) pw_context_destroy(self->context);
  if (self->loop) { pw_loop_leave(self->loop); pw_loop_destroy(self->loop); }
  free(self);
  pw_deinit();
}

int layerc_pw_capture_fd(struct layerc_pw_capture *self) {
  return self ? pw_loop_get_fd(self->loop) : -1;
}
int layerc_pw_capture_iterate(struct layerc_pw_capture *self) {
  if (!self) return 0;
  return pw_loop_iterate(self->loop, 0);
}
