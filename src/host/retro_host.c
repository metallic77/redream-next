#include <glad/glad.h>
#include <libretro.h>
#include <stdio.h>
#include <stdlib.h>
#include "core/assert.h"
#include "core/filesystem.h"
#include "core/profiler.h"
#include "emulator.h"
#include "host/host.h"
#include "render/render_backend.h"

#define AUDIO_FREQ 44100

/* native Dreamcast resolution */
#define VIDEO_WIDTH_NATIVE  640
#define VIDEO_HEIGHT_NATIVE 480

/* resolution multipliers — HALF is stored as -1, treated specially */
#define RES_SCALE_HALF -1
#define RES_SCALE_1X    1
#define RES_SCALE_2X    2
#define RES_SCALE_3X    3
#define RES_SCALE_4X    4

static int g_res_scale = RES_SCALE_1X;

/* VIDEO_WIDTH/HEIGHT account for half (320x240) and integer multiples */
#define VIDEO_WIDTH  (g_res_scale < 0 ? VIDEO_WIDTH_NATIVE  / 2 : VIDEO_WIDTH_NATIVE  * g_res_scale)
#define VIDEO_HEIGHT (g_res_scale < 0 ? VIDEO_HEIGHT_NATIVE / 2 : VIDEO_HEIGHT_NATIVE * g_res_scale)

/* -------------------------------------------------------------------------
   core options
   ---------------------------------------------------------------------- */

static struct retro_variable core_vars[] = {
    { "redream_resolution",
      "Internal Resolution; 1x (640x480)|Half (320x240)|2x (1280x960)|3x (1920x1440)|4x (2560x1920)" },
    { "redream_sh4_speed",
      "SH4 CPU Speed; 100%|200%|400%|50%" },
    { "redream_frameskip",
      "Auto Frameskip; disabled|1|2|3" },
    { "redream_cable",
      "Cable Type; Composite|VGA|RGB" },
    { "redream_region",
      "System Region; America|Europe|Japan" },
    { "redream_broadcast",
      "Broadcast Mode; NTSC|PAL|PAL-M|PAL-N" },
    { NULL, NULL },
};

/* cable/broadcast options map to redream's internal option names */
/* (these are read from the config; we override them at runtime) */

/* declared by DEFINE_PERSISTENT_OPTION_STRING in bios.c */
extern char OPTION_region[];
extern char OPTION_broadcast[];

/* declared in sh4.c */
extern int sh4_speed_percent;
extern int sh4_cable_type;

/* forward declaration — defined below after the libretro callbacks */
static retro_environment_t env_cb;

static void retro_apply_options() {
  struct retro_variable var;

  /* resolution */
  var.key   = "redream_resolution";
  var.value = NULL;
  if (env_cb(RETRO_ENVIRONMENT_GET_VARIABLE, &var) && var.value) {
    if      (strstr(var.value, "Half")) g_res_scale = RES_SCALE_HALF;
    else if (strstr(var.value, "4x"))   g_res_scale = RES_SCALE_4X;
    else if (strstr(var.value, "3x"))   g_res_scale = RES_SCALE_3X;
    else if (strstr(var.value, "2x"))   g_res_scale = RES_SCALE_2X;
    else                                g_res_scale = RES_SCALE_1X;
  }

  /* auto frameskip */
  var.key   = "redream_frameskip";
  var.value = NULL;
  if (env_cb(RETRO_ENVIRONMENT_GET_VARIABLE, &var) && var.value) {
    if      (strstr(var.value, "1")) emu_frameskip_max = 1;
    else if (strstr(var.value, "2")) emu_frameskip_max = 2;
    else if (strstr(var.value, "3")) emu_frameskip_max = 3;
    else                             emu_frameskip_max = 0;
  }

  /* cable type */
  var.key   = "redream_cable";
  var.value = NULL;
  if (env_cb(RETRO_ENVIRONMENT_GET_VARIABLE, &var) && var.value) {
    if      (strstr(var.value, "VGA")) sh4_cable_type = 0;
    else if (strstr(var.value, "RGB")) sh4_cable_type = 2;
    else                               sh4_cable_type = 3;
  }
  var.key   = "redream_sh4_speed";
  var.value = NULL;
  if (env_cb(RETRO_ENVIRONMENT_GET_VARIABLE, &var) && var.value) {
    if      (strstr(var.value, "200")) sh4_speed_percent = 200;
    else if (strstr(var.value, "400")) sh4_speed_percent = 400;
    else if (strstr(var.value, "50"))  sh4_speed_percent = 50;
    else                               sh4_speed_percent = 100;
  }

  /* region — "america" / "europe" / "japan" */
  var.key   = "redream_region";
  var.value = NULL;
  if (env_cb(RETRO_ENVIRONMENT_GET_VARIABLE, &var) && var.value) {
    if      (strstr(var.value, "Europe")) strncpy(OPTION_region, "europe",  OPTION_MAX_LENGTH);
    else if (strstr(var.value, "Japan"))  strncpy(OPTION_region, "japan",   OPTION_MAX_LENGTH);
    else                                  strncpy(OPTION_region, "america", OPTION_MAX_LENGTH);
  }

  /* broadcast — "ntsc" / "pal" / "pal_m" / "pal_n" */
  var.key   = "redream_broadcast";
  var.value = NULL;
  if (env_cb(RETRO_ENVIRONMENT_GET_VARIABLE, &var) && var.value) {
    if      (strstr(var.value, "PAL-M")) strncpy(OPTION_broadcast, "pal_m", OPTION_MAX_LENGTH);
    else if (strstr(var.value, "PAL-N")) strncpy(OPTION_broadcast, "pal_n", OPTION_MAX_LENGTH);
    else if (strstr(var.value, "PAL"))   strncpy(OPTION_broadcast, "pal",   OPTION_MAX_LENGTH);
    else                                 strncpy(OPTION_broadcast, "ntsc",  OPTION_MAX_LENGTH);
  }
}

static struct retro_hw_render_callback hw_render;
static retro_audio_sample_t audio_cb;
static retro_audio_sample_batch_t audio_batch_cb;
static retro_video_refresh_t video_cb;
static retro_input_poll_t input_poll_cb;
static retro_input_state_t input_state_cb;

/*
 * libretro host implementation
 */

/* clang-format off */
#define NUM_CONTROLLER_DESC          (array_size(controller_desc)-1)

#define CONTROLLER_DESC(port)                                                                                 \
  { port, RETRO_DEVICE_JOYPAD, 0,                              RETRO_DEVICE_ID_JOYPAD_A,     "B" },           \
  { port, RETRO_DEVICE_JOYPAD, 0,                              RETRO_DEVICE_ID_JOYPAD_B,     "A" },           \
  { port, RETRO_DEVICE_JOYPAD, 0,                              RETRO_DEVICE_ID_JOYPAD_START, "Start" },       \
  { port, RETRO_DEVICE_JOYPAD, 0,                              RETRO_DEVICE_ID_JOYPAD_UP,    "D-Pad Up" },    \
  { port, RETRO_DEVICE_JOYPAD, 0,                              RETRO_DEVICE_ID_JOYPAD_DOWN,  "D-Pad Down" },  \
  { port, RETRO_DEVICE_JOYPAD, 0,                              RETRO_DEVICE_ID_JOYPAD_LEFT,  "D-Pad Left" },  \
  { port, RETRO_DEVICE_JOYPAD, 0,                              RETRO_DEVICE_ID_JOYPAD_RIGHT, "D-Pad Right" }, \
  { port, RETRO_DEVICE_JOYPAD, 0,                              RETRO_DEVICE_ID_JOYPAD_X,     "Y" },           \
  { port, RETRO_DEVICE_JOYPAD, 0,                              RETRO_DEVICE_ID_JOYPAD_Y,     "X" },           \
  { port, RETRO_DEVICE_ANALOG, RETRO_DEVICE_INDEX_ANALOG_LEFT, RETRO_DEVICE_ID_ANALOG_X,     "Analog X" },    \
  { port, RETRO_DEVICE_ANALOG, RETRO_DEVICE_INDEX_ANALOG_LEFT, RETRO_DEVICE_ID_ANALOG_Y,     "Analog Y" },    \
  { port, RETRO_DEVICE_JOYPAD, 0,                              RETRO_DEVICE_ID_JOYPAD_L2,    "L" },           \
  { port, RETRO_DEVICE_JOYPAD, 0,                              RETRO_DEVICE_ID_JOYPAD_R2,    "R" }

#define CONTROLLER_BUTTONS(port) \
  K_CONT_B,                      \
  K_CONT_A,                      \
  K_CONT_START,                  \
  K_CONT_DPAD_UP,                \
  K_CONT_DPAD_DOWN,              \
  K_CONT_DPAD_LEFT,              \
  K_CONT_DPAD_RIGHT,             \
  K_CONT_Y,                      \
  K_CONT_X,                      \
  K_CONT_JOYX,                   \
  K_CONT_JOYY,                   \
  K_CONT_LTRIG,                  \
  K_CONT_RTRIG

static struct retro_input_descriptor controller_desc[] = {
  CONTROLLER_DESC(0),
  CONTROLLER_DESC(1),
  CONTROLLER_DESC(2),
  CONTROLLER_DESC(3),
  { 0 },
};

static int controller_buttons[] = {
  CONTROLLER_BUTTONS(0),
  CONTROLLER_BUTTONS(1),
  CONTROLLER_BUTTONS(2),
  CONTROLLER_BUTTONS(3),
};
/* clang-format on */

struct retro_host {
  struct host;

  int16_t controller_state[NUM_CONTROLLER_DESC];
};

struct retro_host *g_host;
struct emu *g_emu;

/*
 * audio
 */
void audio_push(struct host *base, const int16_t *data, int frames) {
  audio_batch_cb(data, frames);
}

/*
 * video
 */
void video_unbind_context(struct host *host) {
  CHECK(0);
}

void video_bind_context(struct host *base, struct render_backend *r) {
  CHECK(0);
}

void video_destroy_renderer(struct host *base, struct render_backend *r) {
  r_destroy(r);
}

struct render_backend *video_create_renderer(struct host *base) {
  return r_create(NULL);
}

int video_supports_multiple_threads(struct host *host) {
  return 0;
}

int video_height(struct host *base) {
  return VIDEO_HEIGHT;
}

int video_width(struct host *base) {
  return VIDEO_WIDTH;
}

/*
 * input
 */
void input_poll(struct host *base) {
  struct retro_host *host = (struct retro_host *)base;

  input_poll_cb();

  /* send updates for any inputs that've changed */
  for (int i = 0; i < NUM_CONTROLLER_DESC; i++) {
    struct retro_input_descriptor *desc = &controller_desc[i];
    int16_t value =
        input_state_cb(desc->port, desc->device, desc->index, desc->id);

    /* retroarch's API provides a binary [0, 1] value for the triggers. map from
       this to [0, INT16_MAX] as our host layer expects */
    if (desc->id == RETRO_DEVICE_ID_JOYPAD_L2 ||
        desc->id == RETRO_DEVICE_ID_JOYPAD_R2) {
      value = value ? INT16_MAX : 0;
    }

    if (g_host->controller_state[i] == value) {
      continue;
    }

    if (g_host->input_keydown) {
      int button = controller_buttons[i];
      g_host->input_keydown(g_host->userdata, desc->port, button, value);
    }

    g_host->controller_state[i] = value;
  }
}

/*
 * core
 */
static void video_context_destroyed() {
  if (!g_host->video_context_destroyed) {
    return;
  }

  g_host->video_context_destroyed(g_host->userdata);
}

static void video_context_reset() {
  /* link in gl functions at runtime */
  int res = gladLoadGLLoader((GLADloadproc)hw_render.get_proc_address);
  CHECK_EQ(res, 1, "GL initialization failed");

  if (!g_host->video_context_reset) {
    return;
  }

  g_host->video_context_reset(g_host->userdata);
}

static void host_destroy(struct retro_host *host) {
  free(host);
}

struct retro_host *host_create() {
  struct retro_host *host = calloc(1, sizeof(struct retro_host));

  /* let retroarch know about our controller mappings */
  env_cb(RETRO_ENVIRONMENT_SET_INPUT_DESCRIPTORS, controller_desc);

  /* request an initial OpenGL context */
  hw_render.context_type = RETRO_HW_CONTEXT_OPENGL_CORE;
  hw_render.version_major = 3;
  hw_render.version_minor = 3;
  hw_render.context_reset = &video_context_reset;
  hw_render.context_destroy = &video_context_destroyed;
  hw_render.depth = true;
  hw_render.bottom_left_origin = true;

  bool ret = env_cb(RETRO_ENVIRONMENT_SET_HW_RENDER, &hw_render);
  if (!ret) {
    LOG_WARNING("Failed to initialize hardware renderer");
    host_destroy(host);
    return NULL;
  }

  return host;
}

/*
 * libretro core implementation
 */
void retro_init() {
  /* set application directory */
  const char *sysdir = NULL;
  if (env_cb(RETRO_ENVIRONMENT_GET_SYSTEM_DIRECTORY, &sysdir)) {
    fs_set_appdir(sysdir);
  }

  /* load base options from config */
  const char *appdir = fs_appdir();
  char config[PATH_MAX] = {0};
  snprintf(config, sizeof(config), "%s" PATH_SEPARATOR "config", appdir);
  options_read(config);
}

void retro_deinit() {}

unsigned retro_api_version() {
  return RETRO_API_VERSION;
}

void retro_get_system_info(struct retro_system_info *info) {
  info->library_name = "redream";
  info->library_version = "0.0";
  info->valid_extensions = "gdi|cdi|chd";
  info->need_fullpath = true;
  info->block_extract = false;
}

void retro_get_system_av_info(struct retro_system_av_info *info) {
  info->geometry.base_width   = VIDEO_WIDTH;
  info->geometry.base_height  = VIDEO_HEIGHT;
  info->geometry.max_width    = VIDEO_WIDTH_NATIVE  * RES_SCALE_4X;
  info->geometry.max_height   = VIDEO_HEIGHT_NATIVE * RES_SCALE_4X;
  info->geometry.aspect_ratio = (float)VIDEO_WIDTH_NATIVE / (float)VIDEO_HEIGHT_NATIVE;
  info->timing.fps            = 60;
  info->timing.sample_rate    = AUDIO_FREQ;
}

void retro_set_environment(retro_environment_t env) {
  env_cb = env;
  env_cb(RETRO_ENVIRONMENT_SET_VARIABLES, core_vars);
}

void retro_set_video_refresh(retro_video_refresh_t video) {
  video_cb = video;
}

void retro_set_audio_sample(retro_audio_sample_t audio) {
  audio_cb = audio;
}

void retro_set_audio_sample_batch(retro_audio_sample_batch_t audio_batch) {
  audio_batch_cb = audio_batch;
}

void retro_set_input_poll(retro_input_poll_t input_poll) {
  input_poll_cb = input_poll;
}

void retro_set_input_state(retro_input_state_t input_state) {
  input_state_cb = input_state;
}

void retro_set_controller_port_device(unsigned port, unsigned device) {}

void retro_reset() {}

void retro_run() {
  /* check if the user changed any core options */
  bool options_changed = false;
  env_cb(RETRO_ENVIRONMENT_GET_VARIABLE_UPDATE, &options_changed);
  if (options_changed) {
    int old_scale = g_res_scale;
    retro_apply_options();

    if (g_res_scale != old_scale) {
      /* tell the emulator to recreate its framebuffer at the new size —
         video_width()/video_height() already return the new dimensions since
         g_res_scale is updated, so emu_host_resized will pick them up */
      if (g_host->video_resized) {
        g_host->video_resized(g_host->userdata);
      }

      /* also notify RetroArch of the new output geometry */
      struct retro_system_av_info av_info;
      retro_get_system_av_info(&av_info);
      env_cb(RETRO_ENVIRONMENT_SET_GEOMETRY, &av_info.geometry);
    }
  }

  /* bind the framebuffer provided by retroarch before calling into the
     emulator */
  uintptr_t fb = hw_render.get_current_framebuffer();
  glBindFramebuffer(GL_FRAMEBUFFER, fb);

  emu_run_frame(g_emu);

  /* call back into retroarch, letting it know a frame has been rendered */
  video_cb(RETRO_HW_FRAME_BUFFER_VALID, VIDEO_WIDTH, VIDEO_HEIGHT, 0);
}

size_t retro_serialize_size() {
  return 0;
}

bool retro_serialize(void *data, size_t size) {
  return false;
}

bool retro_unserialize(const void *data, size_t size) {
  return false;
}

void retro_cheat_reset() {}

void retro_cheat_set(unsigned index, bool enabled, const char *code) {}

bool retro_load_game(const struct retro_game_info *info) {
  /* read options before creating the host so video_width/height are correct */
  retro_apply_options();

  g_host = host_create();
  if (!g_host) {
    return false;
  }

  g_emu = emu_create((struct host *)g_host);
  if (!g_emu) {
    host_destroy(g_host);
    g_host = NULL;
    return false;
  }

  return emu_load_game(g_emu, info->path);
}

bool retro_load_game_special(unsigned game_type,
                             const struct retro_game_info *info,
                             size_t num_info) {
  return false;
}

void retro_unload_game() {
  emu_destroy(g_emu);
  g_emu = NULL;

  host_destroy(g_host);
  g_host = NULL;
}

unsigned retro_get_region() {
  return RETRO_REGION_NTSC;
}

void *retro_get_memory_data(unsigned id) {
  return NULL;
}

size_t retro_get_memory_size(unsigned id) {
  return 0;
}
