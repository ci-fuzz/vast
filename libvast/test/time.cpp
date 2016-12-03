#include "vast/concept/parseable/to.hpp"
#include "vast/concept/parseable/vast/time.hpp"
#include "vast/concept/printable/std/chrono.hpp"
#include "vast/concept/printable/to_string.hpp"
#include "vast/time.hpp"

#define SUITE time
#include "test.hpp"

using namespace vast;
using namespace std::chrono;

TEST(printable) {
  CHECK_EQUAL(to_string(nanoseconds(42)), "+42ns");
  CHECK_EQUAL(to_string(microseconds(42)), "+42us");
  CHECK_EQUAL(to_string(milliseconds(42)), "+42ms");
  CHECK_EQUAL(to_string(seconds(42)), "+42s");
  CHECK_EQUAL(to_string(minutes(42)), "+42min");
  CHECK_EQUAL(to_string(hours(42)), "+42h");
}

TEST(parseable) {
  timespan sp;
  MESSAGE("nanoseconds");
  CHECK(parsers::timespan("42 nsecs", sp));
  CHECK(sp == nanoseconds(42));
  CHECK(parsers::timespan("43nsecs", sp));
  CHECK(sp == nanoseconds(43));
  CHECK(parsers::timespan("44ns", sp));
  CHECK(sp == nanoseconds(44));
  MESSAGE("microseconds");
  CHECK(parsers::timespan("42 usecs", sp));
  CHECK(sp == microseconds(42));
  CHECK(parsers::timespan("43usecs", sp));
  CHECK(sp == microseconds(43));
  CHECK(parsers::timespan("44us", sp));
  CHECK(sp == microseconds(44));
  MESSAGE("milliseconds");
  CHECK(parsers::timespan("42 msecs", sp));
  CHECK(sp == milliseconds(42));
  CHECK(parsers::timespan("43msecs", sp));
  CHECK(sp == milliseconds(43));
  CHECK(parsers::timespan("44ms", sp));
  CHECK(sp == milliseconds(44));
  MESSAGE("seconds");
  CHECK(parsers::timespan("-42 secs", sp));
  CHECK(sp == seconds(-42));
  CHECK(parsers::timespan("-43secs", sp));
  CHECK(sp == seconds(-43));
  CHECK(parsers::timespan("-44s", sp));
  CHECK(sp == seconds(-44));
  MESSAGE("minutes");
  CHECK(parsers::timespan("-42 mins", sp));
  CHECK(sp == minutes(-42));
  CHECK(parsers::timespan("-43min", sp));
  CHECK(sp == minutes(-43));
  CHECK(parsers::timespan("44m", sp));
  CHECK(sp == minutes(44));
  MESSAGE("hours");
  CHECK(parsers::timespan("42 hours", sp));
  CHECK(sp == hours(42));
  CHECK(parsers::timespan("-43hrs", sp));
  CHECK(sp == hours(-43));
  CHECK(parsers::timespan("44h", sp));
  CHECK(sp == hours(44));
// TODO
// MESSAGE("compound");
// CHECK(parsers::timespan("5m99s", sp));
// CHECK(sp.count() == 399000000000ll);
  timestamp ts;
// FIXME
  MESSAGE("YYYY-MM-DD+HH:MM:SS");
  CHECK(parsers::timestamp("2012-08-12+23:55:04", ts));
//  CHECK(ts == point::utc(2012, 8, 12, 23, 55, 4));
  MESSAGE("YYYY-MM-DD+HH:MM");
  CHECK(parsers::timestamp("2012-08-12+23:55", ts));
//  CHECK(ts == point::utc(2012, 8, 12, 23, 55));
  MESSAGE("YYYY-MM-DD+HH");
  CHECK(parsers::timestamp("2012-08-12+23", ts));
//  CHECK(ts == point::utc(2012, 8, 12, 23));
  MESSAGE("YYYY-MM-DD");
  CHECK(parsers::timestamp("2012-08-12", ts));
//  CHECK(ts == point::utc(2012, 8, 12));
  MESSAGE("YYYY-MM");
  CHECK(parsers::timestamp("2012-08", ts));
//  CHECK(ts == point::utc(2012, 8));
  MESSAGE("UNIX epoch");
  CHECK(parsers::timestamp("@1444040673", ts));
  CHECK(ts.time_since_epoch() == seconds{1444040673});
  CHECK(parsers::timestamp("@1398933902.686337", ts));
  CHECK(ts.time_since_epoch() == double_seconds{1398933902.686337});
  MESSAGE("now");
  CHECK(parsers::timestamp("now", ts));
  CHECK(ts > timestamp::clock::now() - minutes{1});
  CHECK(ts < timestamp::clock::now() + minutes{1});
  CHECK(parsers::timestamp("now - 1m", ts));
  CHECK(ts < timestamp::clock::now());
  CHECK(parsers::timestamp("now + 1m", ts));
  CHECK(ts > timestamp::clock::now());
  MESSAGE("ago");
  CHECK(parsers::timestamp("10 days ago", ts));
  CHECK(ts < timestamp::clock::now());
  MESSAGE("in");
  CHECK(parsers::timestamp("in 1 year", ts));
  CHECK(ts > timestamp::clock::now());
}

