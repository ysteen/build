/* Synchronous libretro access for browser-managed input synchronization. */
static bool ejs_sync_active;
static bool ejs_sync_present;
static uint16_t ejs_sync_buttons[4];
static struct retro_callbacks ejs_sync_callbacks;
static unsigned ejs_sync_audio_frames;
#ifdef HAVE_GFX_WIDGETS
static bool ejs_sync_widgets_active;
#endif

static void ejs_sync_poll(void) {}

static int16_t ejs_sync_input(unsigned port, unsigned device, unsigned index, unsigned id)
{
   if (port >= 4 || device != RETRO_DEVICE_JOYPAD || index != 0)
      return 0;
   if (id == RETRO_DEVICE_ID_JOYPAD_MASK)
      return (int16_t)ejs_sync_buttons[port];
   return id < 16 ? !!(ejs_sync_buttons[port] & (1u << id)) : 0;
}

static void ejs_sync_video(const void *data, unsigned width, unsigned height, size_t pitch)
{
   if (ejs_sync_present)
   {
#ifdef HAVE_GFX_WIDGETS
      /* Video reinitialization can enable widgets while their runloop is paused. */
      dispwidget_get_ptr()->active = false;
#endif
      ejs_sync_callbacks.frame_cb(data, width, height, pitch);
   }
}

static void ejs_sync_audio(int16_t left, int16_t right)
{
   if (ejs_sync_present)
      ejs_sync_callbacks.sample_cb(left, right);
}

static size_t ejs_sync_audio_batch(const int16_t *data, size_t frames)
{
   ejs_sync_audio_frames += frames;
   return ejs_sync_present ? ejs_sync_callbacks.sample_batch_cb(data, frames) : frames;
}

EMSCRIPTEN_KEEPALIVE unsigned ejs_sync_audio_count(void)
{
   return ejs_sync_audio_frames;
}

EMSCRIPTEN_KEEPALIVE unsigned ejs_sync_audio_status(void)
{
   return audio_state_get_ptr()->flags
      | ((runloop_get_flags() & RUNLOOP_FLAG_PAUSED) ? 0x80000000u : 0);
}

EMSCRIPTEN_KEEPALIVE int ejs_sync_begin(void)
{
   struct retro_core_t *core = &runloop_state_get_ptr()->current_core;
   if (ejs_sync_active)
      return 1;
   if (!core->retro_serialize_size || !core->retro_serialize_size())
      return 0;
   core_set_default_callbacks(&ejs_sync_callbacks);
#ifdef HAVE_GFX_WIDGETS
   ejs_sync_widgets_active = dispwidget_get_ptr()->active;
   dispwidget_get_ptr()->active = false;
#endif
   /* Browser-managed frames still need an unpaused frontend audio callback. */
   command_event(CMD_EVENT_UNPAUSE, NULL);
   EJS_PAUSED = true;
   emscripten_pause_main_loop();
   EJS_MAINLOOP_PAUSED = true;
   core->retro_set_input_poll(ejs_sync_poll);
   core->retro_set_input_state(ejs_sync_input);
   core->retro_set_video_refresh(ejs_sync_video);
   core->retro_set_audio_sample(ejs_sync_audio);
   core->retro_set_audio_sample_batch(ejs_sync_audio_batch);
   ejs_sync_active = true;
   return 1;
}

EMSCRIPTEN_KEEPALIVE int ejs_sync_step_multi(unsigned p1, unsigned p2,
      unsigned p3, unsigned p4, int present)
{
   if (!ejs_sync_active)
      return 0;
   ejs_sync_buttons[0] = p1;
   ejs_sync_buttons[1] = p2;
   ejs_sync_buttons[2] = p3;
   ejs_sync_buttons[3] = p4;
   ejs_sync_present = present != 0;
   if (ejs_sync_present)
   {
#ifdef HAVE_RWEBAUDIO
      rwebaudio_recalibrate_time();
#endif
      /* The regular runloop polls canvas dimensions before presenting. */
      video_driver_state_t *video = video_state_get_ptr();
      if (video->current_video && video->current_video->alive)
         video->current_video->alive(video->data);
   }
   runloop_state_get_ptr()->current_core.retro_run();
   return 1;
}

EMSCRIPTEN_KEEPALIVE int ejs_sync_step(unsigned p1, unsigned p2, int present)
{
   return ejs_sync_step_multi(p1, p2, 0, 0, present);
}

EMSCRIPTEN_KEEPALIVE size_t ejs_sync_state_size(void)
{
   return ejs_sync_active ? core_serialize_size() : 0;
}

EMSCRIPTEN_KEEPALIVE int ejs_sync_save(void *data, size_t size)
{
   if (!ejs_sync_active || !data || size != core_serialize_size())
      return 0;
   /* Some cores leave padding unwritten; hashes must not include stale memory. */
   memset(data, 0, size);
   return runloop_state_get_ptr()->current_core.retro_serialize(data, size);
}

EMSCRIPTEN_KEEPALIVE int ejs_sync_load(const void *data, size_t size)
{
   if (!ejs_sync_active || !data || size != core_serialize_size())
      return 0;
   return runloop_state_get_ptr()->current_core.retro_unserialize(data, size);
}

EMSCRIPTEN_KEEPALIVE void *ejs_sync_memory_data(unsigned id)
{
   return runloop_state_get_ptr()->current_core.retro_get_memory_data(id);
}

EMSCRIPTEN_KEEPALIVE size_t ejs_sync_memory_size(unsigned id)
{
   return runloop_state_get_ptr()->current_core.retro_get_memory_size(id);
}

EMSCRIPTEN_KEEPALIVE void ejs_sync_end(void)
{
   struct retro_core_t *core = &runloop_state_get_ptr()->current_core;
   if (!ejs_sync_active)
      return;
   core->retro_set_input_poll(ejs_sync_callbacks.poll_cb);
   core->retro_set_input_state(ejs_sync_callbacks.state_cb);
   core->retro_set_video_refresh(ejs_sync_callbacks.frame_cb);
   core->retro_set_audio_sample(ejs_sync_callbacks.sample_cb);
   core->retro_set_audio_sample_batch(ejs_sync_callbacks.sample_batch_cb);
   ejs_sync_active = false;
#ifdef HAVE_GFX_WIDGETS
   dispwidget_get_ptr()->active = ejs_sync_widgets_active;
#endif
   /* Stay paused until the caller explicitly resumes offline play. */
}
