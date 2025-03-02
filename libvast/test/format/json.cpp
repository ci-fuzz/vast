/******************************************************************************
 *                    _   _____   __________                                  *
 *                   | | / / _ | / __/_  __/     Visibility                   *
 *                   | |/ / __ |_\ \  / /          Across                     *
 *                   |___/_/ |_/___/ /_/       Space and Time                 *
 *                                                                            *
 * This file is part of VAST. It is subject to the license terms in the       *
 * LICENSE file found in the top-level directory of this distribution and at  *
 * http://vast.io/license. No part of VAST, including this file, may be       *
 * copied, modified, propagated, or distributed except according to the terms *
 * contained in the LICENSE file.                                             *
 ******************************************************************************/

#include "vast/format/json.hpp"

#include "vast/format/json/suricata_selector.hpp"

#define SUITE format

#include "vast/test/fixtures/actor_system.hpp"
#include "vast/test/fixtures/events.hpp"
#include "vast/test/test.hpp"

#include "vast/concept/parseable/to.hpp"
#include "vast/concept/parseable/vast/json.hpp"
#include "vast/concept/parseable/vast/time.hpp"
#include "vast/table_slice_builder_factory.hpp"

using namespace vast;
using namespace std::string_literals;

namespace {

auto http = record_type{{"ts", time_type{}},
                        {"uid", string_type{}},
                        {"id.orig_h", address_type{}},
                        {"id.orig_p", count_type{}},
                        {"id.resp_h", address_type{}},
                        {"id.resp_p", count_type{}},
                        {"trans_depth", count_type{}},
                        {"method", string_type{}},
                        {"host", string_type{}},
                        {"uri", string_type{}},
                        {"version", string_type{}},
                        {"user_agent", string_type{}},
                        {"request_body_len", count_type{}},
                        {"response_body_len", count_type{}},
                        {"status_code", count_type{}},
                        {"status_msg", string_type{}},
                        {"tags", list_type{string_type{}}},
                        {"resp_fuids", list_type{string_type{}}},
                        {"resp_mime_types", list_type{string_type{}}}}
              .name("http");

std::string_view eve_log
  = R"json({"timestamp":"2011-08-12T14:52:57.716360+0200","flow_id":1031464864740687,"pcap_cnt":83,"event_type":"alert","src_ip":"147.32.84.165","src_port":1181,"dest_ip":"78.40.125.4","dest_port":6667,"proto":"TCP","alert":{"action":"allowed","gid":1,"signature_id":2017318,"rev":4,"signature":"ET CURRENT_EVENTS SUSPICIOUS IRC - PRIVMSG *.(exe|tar|tgz|zip)  download command","category":"Potentially Bad Traffic","severity":2},"flow":{"pkts_toserver":27,"pkts_toclient":35,"bytes_toserver":2302,"bytes_toclient":4520,"start":"2011-08-12T14:47:24.357711+0200"},"payload":"UFJJVk1TRyAjemFyYXNhNDggOiBzbXNzLmV4ZSAoMzY4KQ0K","payload_printable":"PRIVMSG #zarasa48 : smss.exe (368)\r\n","stream":0,"packet":"AB5J2xnDCAAntbcZCABFAABMGV5AAIAGLlyTIFSlTih9BASdGgvw0QvAxUWHdVAY+rCL4gAAUFJJVk1TRyAjemFyYXNhNDggOiBzbXNzLmV4ZSAoMzY4KQ0K","packet_info":{"linktype":1}}
  {"timestamp":"2011-08-12T14:52:57.716360+0200","flow_id":1031464864740687,"pcap_cnt":83,"event_type":"alert","src_ip":"147.32.84.165","src_port":1181,"dest_ip":"78.40.125.4","dest_port":6667,"proto":"TCP","alert":{"action":"allowed","gid":1,"signature_id":2017318,"rev":4,"signature":"ET CURRENT_EVENTS SUSPICIOUS IRC - PRIVMSG *.(exe|tar|tgz|zip)  download command","category":"Potentially Bad Traffic","severity":2},"flow":{"pkts_toserver":27,"pkts_toclient":35,"bytes_toserver":2302,"bytes_toclient":4520,"start":"2011-08-12T14:47:24.357711+0200"},"payload":"UFJJVk1TRyAjemFyYXNhNDggOiBzbXNzLmV4ZSAoMzY4KQ0K","payload_printable":"PRIVMSG #zarasa48 : smss.exe (368)\r\n","stream":0,"packet":"AB5J2xnDCAAntbcZCABFAABMGV5AAIAGLlyTIFSlTih9BASdGgvw0QvAxUWHdVAY+rCL4gAAUFJJVk1TRyAjemFyYXNhNDggOiBzbXNzLmV4ZSAoMzY4KQ0K","packet_info":{"linktype":1},"resp_mime_types":null})json";

} // namespace

FIXTURE_SCOPE(zeek_reader_tests, fixtures::deterministic_actor_system)

TEST(json to data) {
  auto layout = record_type{{"b", bool_type{}},
                            {"c", count_type{}},
                            {"r", real_type{}},
                            {"i", integer_type{}},
                            {"s", string_type{}},
                            {"a", address_type{}},
                            {"sn", subnet_type{}},
                            {"t", time_type{}},
                            {"d", duration_type{}},
                            {"d2", duration_type{}},
                            {"e", enumeration_type{{"FOO", "BAR", "BAZ"}}},
                            {"lc", list_type{count_type{}}},
                            {"lt", list_type{time_type{}}},
                            {"rec", record_type{{"c", count_type{}},
                                                {"s", string_type{}}}},
                            {"msa", map_type{string_type{}, address_type{}}},
                            {"mcs", map_type{count_type{}, string_type{}}}}
                  .name("layout");
  auto builder = factory<table_slice_builder>::make(
    defaults::import::table_slice_type, layout);
  std::string_view str = R"json({
    "b": true,
    "c": 424242,
    "r": 4.2,
    "i": -1337,
    "s": "0123456789®\r\n",
    "a": "147.32.84.165",
    "sn": "192.168.0.1/24",
    "t": "2011-08-12+14:59:11.994970",
    "d": "42s",
    "d2": 3.006088,
    "e": "BAZ",
    "lc": [ "0x3e7", 19, 5555, 0 ],
    "lt": [ 1556624773, "2019-04-30T11:46:13Z" ],
    "rec": { "c": 421, "s":"test" },
    "msa": { "foo": "1.2.3.4", "bar": "2001:db8::" },
    "mcs": { "1": "FOO", "1024": "BAR!" }
  })json";
  ::simdjson::dom::parser p;
  auto el = p.parse(str);
  CHECK(el.error() == ::simdjson::error_code::SUCCESS);
  auto obj = el.value().get_object();
  CHECK(obj.error() == ::simdjson::error_code::SUCCESS);
  format::json::add(*builder, obj.value(), layout);
  auto slice = builder->finish();
  REQUIRE_NOT_EQUAL(slice.encoding(), table_slice_encoding::none);
  CHECK(slice.at(0, 0) == data{true});
  CHECK(slice.at(0, 1) == data{count{424242}});
  CHECK(slice.at(0, 2).is<real>());
  const auto r = slice.at(0, 2).get_data().get(
    std::integral_constant<
      int, caf::detail::tl_index_of<data::types, real>::value>());
  CHECK(std::abs(r - real{4.2}) < 0.000001);
  CHECK(slice.at(0, 3) == data{integer{-1337}});
  CHECK_EQUAL(slice.at(0, 4), data{std::string{"0123456789®\r\n"}});
  std::array<std::uint8_t, 4> addr1{147, 32, 84, 165};
  CHECK(slice.at(0, 5)
        == data{address::v4(addr1.data(), address::byte_order::network)});
  std::array<std::uint8_t, 4> addr2{192, 168, 0, 1};
  CHECK(slice.at(0, 6)
        == data{
          subnet{address::v4(addr2.data(), address::byte_order::network), 24}});
  CHECK(slice.at(0, 10) == data{enumeration{2}});
  const list lc
    = {data{count{0x3e7}}, data{count{19}}, data{count{5555}}, data{count{0}}};
  CHECK(slice.at(0, 11) == data{lc});
  CHECK(slice.at(0, 13) == data{count{421}});
  CHECK(slice.at(0, 14) == data{std::string{"test"}});
  auto reference = map{};
  reference[count{1}] = data{"FOO"};
  reference[count{1024}] = data{"BAR!"};
  CHECK_EQUAL(materialize(slice.at(0, 16)), data{reference});
}

TEST_DISABLED(json suricata) {
  using reader_type = format::json::reader<format::json::suricata_selector>;
  auto input = std::make_unique<std::istringstream>(std::string{eve_log});
  reader_type reader{caf::settings{}, std::move(input)};
  std::vector<table_slice> slices;
  auto add_slice
    = [&](table_slice slice) { slices.emplace_back(std::move(slice)); };
  auto [err, num] = reader.read(2, 5, add_slice);
  CHECK_EQUAL(err, ec::end_of_input);
  REQUIRE_EQUAL(num, 2u);
  CHECK_EQUAL(slices[0].columns(), 36u);
  CHECK_EQUAL(slices[0].rows(), 2u);
  CHECK(slices[0].at(0, 19) == data{count{4520}});
}

TEST(json hex number parser) {
  using namespace parsers;
  double x;
  CHECK(json_number("123.0", x));
  CHECK_EQUAL(x, 123.0);
  CHECK(json_number("-123.0", x));
  CHECK_EQUAL(x, -123.0);
  CHECK(json_number("123", x));
  CHECK_EQUAL(x, 123.0);
  CHECK(json_number("+123", x));
  CHECK_EQUAL(x, 123.0);
  CHECK(json_number("0xFF", x));
  CHECK_EQUAL(x, 255.0);
}

FIXTURE_SCOPE_END()
