#include "vast/ingestor.h"

#include "vast/segment.h"
#include "vast/source/file.h"
#include "vast/io/serialization.h"

#ifdef VAST_HAVE_BROCCOLI
#include "vast/source/broccoli.h"
#endif

namespace vast {

using namespace cppa;

ingestor_actor::ingestor_actor(path dir,
                               actor_ptr receiver,
                               size_t max_events_per_chunk,
                               size_t max_segment_size,
                               uint64_t batch_size)
  : dir_{std::move(dir)},
    receiver_{receiver},
    max_events_per_chunk_{max_events_per_chunk},
    max_segment_size_{max_segment_size},
    batch_size_{batch_size}
{
}

void ingestor_actor::act()
{
  trap_exit(true);
  become(
      on(atom("terminate")) >> [=]
      {
        assert(! segments_.empty());
        VAST_LOG_ACTOR_INFO("writes un-acked segments to stable storage");

        auto segment_dir = dir_ / "segments";
        if (! exists(segment_dir) && ! mkdir(segment_dir))
        {
          VAST_LOG_ACTOR_ERROR("failed to create directory " << segment_dir);
        }
        else
        {
          for (auto& p : segments_)
          {
            auto const path = segment_dir / to<string>(p.first);
            VAST_LOG_ACTOR_INFO("saves " << path);
            io::archive(path, p.second);
          }
        }

        quit(exit::error);
      },
      on(atom("shutdown"), arg_match) >> [=](uint32_t reason)
      {
        if (segments_.empty())
        {
          quit(reason);
        }
        else
        {
          delayed_send(self, std::chrono::seconds(30), atom("terminate"));
          VAST_LOG_ACTOR_INFO(
              "waits 30 seconds for " << segments_.size() << "segment ACKs");
        }
      },
      on(atom("EXIT"), arg_match) >> [=](uint32_t /* reason */)
      {
        // Tell all sources to exit, they will in turn propagate the exit
        // message to the sinks.
        VAST_LOG_ACTOR_DEBUG("got EXIT from " << VAST_ACTOR_ID(last_sender()));
        for (auto& src : sources_)
          send_exit(src, exit::stop);
      },
      on(atom("DOWN"), arg_match) >> [=](uint32_t /* reason */)
      {
        VAST_LOG_ACTOR_DEBUG("got DOWN from " << VAST_ACTOR_ID(last_sender()));
        auto i = sinks_.find(last_sender());
        assert(i != sinks_.end());  // We only monitor sinks.
        sinks_.erase(i);
        if (sinks_.empty())
          delayed_send(self, std::chrono::seconds(5), atom("shutdown"), exit::done);
      },
#ifdef VAST_HAVE_BROCCOLI
      on(atom("ingest"), atom("broccoli"), arg_match) >>
        [=](std::string const& host, unsigned port,
            std::vector<std::string> const& events)
      {
        auto src = make_source<source::broccoli>(host, port);
        send(src, atom("subscribe"), events);
        send(src, atom("run"));
      },
#endif
      on(atom("ingest"), "bro15conn", arg_match) >> [=](std::string const& file)
      {
        auto src = make_source<source::bro15conn>(file);
        send(src, atom("run"));
      },
      on(atom("ingest"), "bro2", arg_match) >> [=](std::string const& file)
      {
        auto src = make_source<source::bro2>(file);
        send(src, atom("run"));
      },
      on(atom("ingest"), val<std::string>, arg_match) >> [=](std::string const&)
      {
        VAST_LOG_ACTOR_ERROR("got invalid ingestion file type");
      },
      on(atom("run")) >> [=]
      {
        delayed_send(
            self,
            std::chrono::seconds(2),
            atom("statistics"), atom("print"), uint64_t(0));
      },
      on(atom("statistics"), arg_match) >> [=](uint64_t rate)
      {
        assert(sinks_.find(last_sender()) != sinks_.end());
        sinks_[last_sender()] = rate;
      },
      on(atom("statistics"), atom("print"), arg_match) >> [=](uint64_t last)
      {
        uint64_t sum = 0;
        for (auto& pair : sinks_)
          sum += pair.second;

        if (sum != last)
          VAST_LOG_ACTOR_INFO("ingests at rate " << sum << " events/sec");

        if (! sinks_.empty())
          delayed_send(
              self,
              std::chrono::seconds(1),
              atom("statistics"), atom("print"), sum);
      },
      on_arg_match >> [=](segment& s)
      {
        VAST_LOG_ACTOR_DEBUG(
            "relays segment " << s.id() << " to " << VAST_ACTOR_ID(receiver_));

        auto cs = cow<segment>{std::move(s)};
        segments_[cs->id()] = cs;
        receiver_ << cs;
      },
      on(atom("ack"), arg_match) >> [=](uuid const& id)
      {
        VAST_LOG_ACTOR_DEBUG("got ack for segment " << id);
        auto i = segments_.find(id);
        assert(i != segments_.end());
        segments_.erase(i);
      },
      on(atom("nack"), arg_match) >> [=](uuid const& id)
      {
        VAST_LOG_ACTOR_ERROR("got nack for segment " << id);
        quit(exit::error);
      });
}

char const* ingestor_actor::description() const
{
  return "ingestor";
}

} // namespace vast
