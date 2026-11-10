// Copyright 2008 Dolphin Emulator Project
// SPDX-License-Identifier: GPL-2.0-or-later

#pragma once

#include <atomic>

#if defined(HAVE_PULSEAUDIO) && HAVE_PULSEAUDIO
#include <pulse/pulseaudio.h>
#endif

#include "AudioCommon/SoundStream.h"
#include "Common/Flag.h"
#include "Common/Thread.h"

class PulseAudio final : public SoundStream
{
#if defined(HAVE_PULSEAUDIO) && HAVE_PULSEAUDIO
public:
  PulseAudio();
  ~PulseAudio() override;

  bool Init() override;
  bool SetRunning(bool running) override { return true; }
  static bool IsValid() { return true; }
  void StateCallback(pa_context* c);
  void WriteCallback(pa_stream* s, size_t length);
  void UnderflowCallback(pa_stream* s);

private:
  void SoundLoop();

  bool PulseInit();
  void PulseShutdown();

  // wrapper callback functions, last parameter _must_ be PulseAudio*
  static void StateCallback(pa_context* c, void* userdata);
  static void WriteCallback(pa_stream* s, size_t length, void* userdata);
  static void UnderflowCallback(pa_stream* s, void* userdata);

  std::thread m_thread;
  Common::Flag m_run_thread;

  bool m_stereo;  // stereo, else surround
  int m_bytespersample;
  int m_channels;

  int m_pa_error;
  int m_pa_connected;
  // Owned by the loop thread but kept here so ~PulseAudio can interrupt its
  // blocking poll. It is freed by the destructor after the thread has joined,
  // which is why it is atomic: the destructor reads it while the thread runs.
  std::atomic<pa_mainloop*> m_pa_ml{nullptr};
  pa_mainloop_api* m_pa_mlapi;
  pa_context* m_pa_ctx;
  pa_stream* m_pa_s;
  pa_buffer_attr m_pa_ba;
#endif
};
