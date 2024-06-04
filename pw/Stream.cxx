#include "Stream.hh"
#include "Thread.hh"

#include <pipewire/impl.h>
#include <spa/monitor/device.h>
#include <spa/monitor/event.h>
#include <spa/monitor/utils.h>
#include <spa/param/audio/raw-utils.h>

#include <cassert>
#include <stdexcept>

// This attempt at making a device is modeled after the module-combine-stream.c code
using namespace pw;

namespace {
   const char* state_str(enum pw_stream_state state)
   {
      static const char* STATE_STR[8] = {
         "UNCONNECTED",
         "CONNECTING",
         "PAUSED",
         "STREAMING",
         "ERROR",
         "ERROR",
         "ERROR",
         "ERROR"
      };
      return STATE_STR[(uint8_t)state & 7];
   }

   double now()
   {
      struct timespec ts;
      clock_gettime(CLOCK_MONOTONIC, &ts);
      return ts.tv_sec + ts.tv_nsec / 1000000000.0;
   }
}


Stream::Stream(
   const std::string& name,
   const std::string& alias,
   EventCallback on_connect,
   EventCallback on_disconnect,
   EventCallback on_start,
   EventCallback on_stop,
   DataCallback on_data):
      m_thread{Thread::Get()},
      m_connect_cb{on_connect},
      m_disconnect_cb{on_disconnect},
      m_start_cb{on_start},
      m_stop_cb{on_stop},
      m_data_cb{on_data}
{
   auto lock = m_thread->Lock();
   // Stream objects will create nodes that will auto-convert to the given
   // format.
   m_info.format = SPA_AUDIO_FORMAT_S16P;
   m_info.channels = 2;
   m_info.position[0] = SPA_AUDIO_CHANNEL_FL;
   m_info.position[1] = SPA_AUDIO_CHANNEL_FR;
   m_info.rate = 16000;

   m_stream = pw_stream_new(m_thread->Core(), "ASHA Device",
      pw_properties_new(
	      PW_KEY_NODE_NAME, name.c_str(),
	      PW_KEY_NODE_DESCRIPTION, alias.c_str(),
         PW_KEY_NODE_VIRTUAL, "false",
	      PW_KEY_MEDIA_CLASS, "Audio/Sink",
      nullptr)
   );
   if (m_stream == nullptr)
      throw std::runtime_error("Failed to allocate stream");

   // These events are called with pipewire thread lock already held.
   static pw_stream_events stream_events {
      .version = PW_VERSION_STREAM_EVENTS,
      .destroy = [](void* d) {
         // When this gets called, m_stream has already been free()d.
         auto* self = (Stream*)d;
         spa_hook_remove(&self->m_stream_listener);
         self->m_stream = nullptr;
      },
      .state_changed = [](void* d, enum pw_stream_state old, enum pw_stream_state state, const char* error) {
         auto* self = (Stream*)d;
         printf("on_change_state old: %s  new: %s  error: %s\n", state_str(old), state_str(state), error ? error : "<null>");

         switch (state)
         {
         case PW_STREAM_STATE_UNCONNECTED: self->m_disconnect_cb(); break;
         case PW_STREAM_STATE_CONNECTING:  self->m_connect_cb();    break;
         case PW_STREAM_STATE_PAUSED:      self->m_stop_cb();       break;
         case PW_STREAM_STATE_STREAMING:
            self->m_encoder.Reset();
            self->m_start_cb();
            break;
         default: break;
         }
      },
      // .param_changed = [](void *data, uint32_t id, const struct spa_pod *param) { /* TODO: Define parameters, like volume? */ },
      .process = [](void* d) { ((Stream*)d)->Process(); },
   };

   pw_stream_add_listener(m_stream, &m_stream_listener, &stream_events, this);

   spa_pod_builder b;
   uint8_t buffer[1024];
	spa_pod_builder_init(&b, buffer, sizeof(buffer));
	const struct spa_pod *params[] = {
      spa_format_audio_raw_build(&b, SPA_PARAM_EnumFormat, &m_info)
   };
   int flags = PW_STREAM_FLAG_AUTOCONNECT
             | PW_STREAM_FLAG_MAP_BUFFERS
             | PW_STREAM_FLAG_RT_PROCESS; // TODO: is the l2cap send() call realtime safe?
   int res = pw_stream_connect(m_stream, SPA_DIRECTION_INPUT, PW_ID_ANY, (pw_stream_flags)flags, params, 1);
   if (res < 0)
   {
      printf("pw_stream_connect returned %d (%s)\n", res, strerror(-res));
      if (m_stream)
         pw_stream_destroy(m_stream);
      throw std::runtime_error("pw_stream_connect returned an error code.");
   }
}

Stream::~Stream()
{
   if (m_stream)
   {
      auto lock = m_thread->Lock();
      pw_stream_destroy(m_stream);
   }
}


void Stream::Process()
{
   // Called from pipewire thread in m_thread, lock already held.
   struct pw_buffer* in = pw_stream_dequeue_buffer(m_stream);
   if (!in)
   {
      printf("Why was process called with no buffers?\n");
      return;
   }

   // TODO: Here, combine just memcpy's the data into the out stream buffers.
   //       We need to actually read it and convert it to g.722
   uint32_t left = in->buffer->n_datas >= 1 ? in->buffer->datas[0].chunk->size : 0;
   uint32_t right = in->buffer->n_datas >= 2 ? in->buffer->datas[1].chunk->size : 0;
   // printf("Received %d channels of sound data (%d, %d). \n", in->buffer->n_datas, left, right);
   assert(left == right);
   if (left == right)
   {
      auto l = in->buffer->datas[0];
      auto loffs = std::min(l.chunk->offset, l.maxsize);
      auto lsize = std::min(l.chunk->size, l.maxsize - loffs);
      auto r = in->buffer->datas[1];
      auto roffs = std::min(r.chunk->offset, r.maxsize);
      auto rsize = std::min(r.chunk->size, r.maxsize - roffs);
      assert(lsize == rsize);
      if (lsize == rsize)
      {
         m_encoder.In(
            SPA_PTROFF(l.data, loffs, int16_t),
            SPA_PTROFF(r.data, roffs, int16_t),
            left / 2, // Convert from bytes to samples.
            m_data_cb
         );
      }
      else
      {
         printf("lsize was not rsize... dropping audio frame");
      }
   }
   else
   {
      printf("Different number of samples from left and right. Dropping audio frame.");
   }

   // m_count += right / 2;
   // double t = now();
   // if (t - m_prev_stamp > 10)
   // {
   //    double hz = m_count / (t - m_prev_stamp);
   //    printf("Receiving data at %u hz", (unsigned)hz);
   //    m_prev_stamp = t;
   //    m_count = 0;
   // }


   // TODO: I have a SEGV here on shutdown sometimes...
   // Place the buffer back so that it can be reused.
   pw_stream_queue_buffer(m_stream, in);
}