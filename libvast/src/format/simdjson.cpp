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

#include "vast/format/simdjson.hpp"

#include "vast/concept/parseable/vast/address.hpp"
#include "vast/concept/parseable/vast/json.hpp"
#include "vast/concept/parseable/vast/subnet.hpp"
#include "vast/concept/parseable/vast/time.hpp"
#include "vast/concept/printable/stream.hpp"
#include "vast/concept/printable/to_string.hpp"
#include "vast/concept/printable/vast/data.hpp"
#include "vast/concept/printable/vast/json.hpp"
#include "vast/data.hpp"
#include "vast/logger.hpp"
#include "vast/policy/include_field_names.hpp"
#include "vast/table_slice.hpp"
#include "vast/table_slice_builder.hpp"
#include "vast/type.hpp"
#include "vast/view.hpp"

#include <caf/detail/pretty_type_name.hpp>
#include <caf/expected.hpp>
#include <caf/none.hpp>

namespace vast::format::simdjson {
namespace {

// Conversion Implementation Details
//
// The task is the following:
// Simdjson values comes in a set of types provided by the lib
// based on JSON specification (lets say it is a J-set),
// Vast data (variant based type) has its own (and a richer) set of types
// (lets say it is a D-set),
// and the task is to perform conversion from a value of a J-set type
// to a certain value of D-set type which is defined by layout specification
//
// Under above conditions we are to organize a "map" function
// from J-set typed value to a D-set. Not all the conversions are possible
// and more to say most of the J*D conversions are not possible, and only
// certain slots in the J*D table.
//
// For a given J type `from_json_x_converter<J>` provides an array
// of converter callbacks. Each index in this array corresponds to
// some type in the list defined by vast::data::type.
// Each callback is a function `type_biased_convert_impl<J,D>`
// There is a default implementation that handles
// * identity mapping if J, D types match;
// * parses value of type D from J which is string
//   if parsing is available for D;
// * return error.
//
// For the cases of a specific conversion is required a necessary
// template specialization is provided.
//
// So the process of converting has 2 phases:
// * select convert function;
// * perform convert function.
//
// Selecting conversion function is in essence two times lookup.
// First lookup for J type `convert(const ::simdjson::dom::element& e, const
// type& t)` and the second if for target D type.

template <class T>
caf::expected<data> convert_from_impl(T v, const type& t);

caf::expected<data> convert(const ::simdjson::dom::element& e, const type& t);

template <class JsonType, class VastType>
struct parser_traits {
  using from_type = JsonType;
  using to_type = VastType;

  static constexpr auto can_be_parsed
    = std::is_same_v<from_type, std::string_view> && has_parser_v<to_type>;
};

/// A default implementation ot conversion from JSON type
/// (known as one of simdjson element_type) to an internal data type.
template <class JsonType, class VastType>
caf::expected<data> type_biased_convert_impl(JsonType j, const type& t) {
  using ptraits = parser_traits<JsonType, VastType>;
  static_cast<void>(t);
  static_cast<void>(j);

  if constexpr (std::is_same_v<JsonType, VastType>) {
    // No convertion needed case.
    return j;
  } else if constexpr (ptraits::can_be_parsed) {
    // Value can be parsed from string.
    using value_type = typename ptraits::to_type;
    value_type x;
    if (!make_parser<value_type>{}(std::string{j}, x))
      return make_error(ec::parse_error, "unable to parse",
                        caf::detail::pretty_type_name(typeid(value_type)), ":",
                        std::string{j});

    return x;
  } else {
    // No conversion case.
    VAST_ERROR_ANON("json-reader cannot convert field  to a propper type", t);

    return make_error(ec::syntax_error, "conversion not implemented");
  }
}

// BOOL Convertions.
template <>
caf::expected<data>
type_biased_convert_impl<std::string_view, bool>(std::string_view s,
                                                 const type&) {
  if (s == "true")
    return true;
  else if (s == "false")
    return false;

  return make_error(ec::convert_error, "cannot convert from", std::string{s},
                    "to bool");
}

// INTEGER Conversions
template <>
caf::expected<data>
type_biased_convert_impl<std::string_view, integer>(std::string_view s,
                                                    const type&) {
  // simdjson cannot be reused here as it doesn't accept hex numbers.
  if (integer x; parsers::json_int(s, x))
    return x;
  if (real x; parsers::json_number(s, x)) {
    VAST_WARNING_ANON("json-reader narrowed", std::string{s}, "to type int");
    return detail::narrow_cast<integer>(x);
  }
  return make_error(ec::convert_error, "cannot convert from", std::string{s},
                    "to int");
}

// COUNT Conversions
template <>
caf::expected<data>
type_biased_convert_impl<integer, count>(integer n, const type&) {
  return detail::narrow_cast<count>(n);
}

template <>
caf::expected<data>
type_biased_convert_impl<std::string_view, count>(std::string_view s,
                                                  const type&) {
  if (count x; parsers::json_count(s, x))
    return x;
  if (real x; parsers::json_number(s, x)) {
    VAST_WARNING_ANON("json-reader narrowed", std::string{s}, "to type count");
    return detail::narrow_cast<count>(x);
  }
  return make_error(ec::convert_error, "cannot convert from", std::string{s},
                    "to count");
}

// REAL Conversions
template <>
caf::expected<data>
type_biased_convert_impl<integer, real>(integer n, const type&) {
  return detail::narrow_cast<real>(n);
}

template <>
caf::expected<data>
type_biased_convert_impl<count, real>(count n, const type&) {
  return detail::narrow_cast<real>(n);
}

template <>
caf::expected<data>
type_biased_convert_impl<std::string_view, real>(std::string_view s,
                                                 const type&) {
  if (real x; parsers::json_number(s, x))
    return x;
  return make_error(ec::convert_error, "cannot convert from", std::string{s},
                    "to real");
}

// Time conversions
template <>
caf::expected<data>
type_biased_convert_impl<integer, time>(integer s, const type&) {
  auto secs = std::chrono::duration<real>(s);
  return time{std::chrono::duration_cast<duration>(secs)};
}

template <>
caf::expected<data>
type_biased_convert_impl<count, time>(count s, const type&) {
  auto secs = std::chrono::duration<real>(s);
  return time{std::chrono::duration_cast<duration>(secs)};
}

template <>
caf::expected<data> type_biased_convert_impl<real, time>(real s, const type&) {
  auto secs = std::chrono::duration<real>(s);
  return time{std::chrono::duration_cast<duration>(secs)};
}

// Duration conversions
template <>
caf::expected<data>
type_biased_convert_impl<integer, duration>(integer s, const type&) {
  auto secs = std::chrono::duration<real>(s);
  return std::chrono::duration_cast<duration>(secs);
}

template <>
caf::expected<data>
type_biased_convert_impl<count, duration>(count s, const type&) {
  auto secs = std::chrono::duration<real>(s);
  return std::chrono::duration_cast<duration>(secs);
}

template <>
caf::expected<data>
type_biased_convert_impl<real, duration>(real s, const type&) {
  auto secs = std::chrono::duration<real>(s);
  return std::chrono::duration_cast<duration>(secs);
}

// STRING Conversions
template <>
caf::expected<data>
type_biased_convert_impl<std::string_view, std::string>(std::string_view s,
                                                        const type&) {
  return std::string{s};
}

// ENUMERATION Conversions
template <>
caf::expected<data>
type_biased_convert_impl<std::string_view, enumeration>(std::string_view s,
                                                        const type& t) {
  const auto& e = dynamic_cast<const enumeration_type&>(*t);

  const auto i = std::find(e.fields.begin(), e.fields.end(), s);
  if (i == e.fields.end())
    return make_error(ec::parse_error, "invalid:", std::string{s});
  return detail::narrow_cast<enumeration>(std::distance(e.fields.begin(), i));
}

// ARRAY Conversions
template <>
caf::expected<data>
type_biased_convert_impl<::simdjson::dom::array, list>(::simdjson::dom::array a,
                                                       const type& t) {
  const auto& v = dynamic_cast<const list_type&>(*t);

  list xs;
  xs.reserve(a.size());

  for (const auto x : a) {
    if (auto elem = convert(x, v.value_type))
      xs.push_back(*std::move(elem));
    else
      return elem;
  }
  return xs;
}

// MAP Conversions
template <>
caf::expected<data> type_biased_convert_impl<::simdjson::dom::object, map>(
  ::simdjson::dom::object o, const type& t) {
  const auto& m = dynamic_cast<const map_type&>(*t);

  map xs;
  xs.reserve(o.size());
  for (auto [k, v] : o) {
    // TODO: Properly unwrap the key type instead of wrapping is in json.
    auto key = convert_from_impl<std::string_view>(k, m.key_type);
    if (!key)
      return key.error();
    auto val = convert(v, m.value_type);
    if (!val)
      return val.error();
    xs[*key] = *val;
  }
  return xs;
}

/// A converter from a given JSON type to vast::data.
/// Relies on a specification of type_biased_convert_impl template function.
/// If no specialiation from a given JSON type to data is provided
/// the the default implementation is used which does one of the following
/// returns a untouched value if source and dest type mach, performs string
/// parsing if possible or return error.
template <class JsonType>
struct from_json_x_converter {
  /// Converter callback.
  /// This is an alias for function which takes an instance of JsonType
  /// and returns data innstance or an error.
  using converter_callback = caf::expected<data> (*)(JsonType, const type& t);

  /// A list of types which are possible destination types
  /// in the scope of conversion from JSON
  using dest_types_list = data::types;

  template <std::size_t N>
  using dest_type_at_t = typename caf::detail::tl_at_t<dest_types_list, N>;

  /// A total number of types possible with data.
  /// This is also exactly the nubmer of possible conversion cases.
  static constexpr std::size_t dest_types_count
    = caf::detail::tl_size<dest_types_list>::value;

  using callbacks_array = std::array<converter_callback, dest_types_count>;

  template <std::size_t... ConcreteTypeIndex>
  static constexpr callbacks_array
  make_callbacks_array_impl(std::index_sequence<ConcreteTypeIndex...>) noexcept {
    callbacks_array result = {
      type_biased_convert_impl<JsonType, dest_type_at_t<ConcreteTypeIndex>>...};
    return result;
  }

  template <typename ConcreteTypeIndexSeq
            = std::make_index_sequence<dest_types_count>>
  static constexpr callbacks_array make_callbacks_array() noexcept {
    return make_callbacks_array_impl(ConcreteTypeIndexSeq{});
  }

  static constexpr callbacks_array callbacks = make_callbacks_array();
};

template <typename T>
caf::expected<data> convert_from_impl(T v, const type& t) {
  const auto type_index = t->index();

  using converter = from_json_x_converter<T>;

  if (0L <= type_index
      && static_cast<int>(converter::dest_types_count) > type_index) {
    return converter::callbacks[type_index](v, t);
  }

  return make_error(ec::syntax_error, "invalid field type");
}

template <typename T>
caf::expected<data>
convert_from(::simdjson::simdjson_result<T> r, const type& t) {
  VAST_ASSERT(r.error() == ::simdjson::SUCCESS);

  return convert_from_impl<T>(r.value(), t);
}

caf::expected<data> convert(const ::simdjson::dom::element& e, const type& t) {
  switch (e.type()) {
    case ::simdjson::dom::element_type::ARRAY:
      return convert_from(e.get_array(), t);
    case ::simdjson::dom::element_type::OBJECT:
      return convert_from(e.get_object(), t);
    case ::simdjson::dom::element_type::INT64:
      return convert_from(e.get_int64(), t);
    case ::simdjson::dom::element_type::UINT64:
      return convert_from(e.get_uint64(), t);
    case ::simdjson::dom::element_type::DOUBLE:
      return convert_from(e.get_double(), t);
    case ::simdjson::dom::element_type::STRING:
      return convert_from(e.get_string(), t);
    case ::simdjson::dom::element_type::BOOL:
      return convert_from(e.get_bool(), t);
    case ::simdjson::dom::element_type::NULL_VALUE:
      return caf::none;
  }

  return make_error(ec::syntax_error, "invalid json type");
}

::simdjson::simdjson_result<::simdjson::dom::element>
lookup(std::string_view field, const ::simdjson::dom::object& xs) {
  VAST_ASSERT(!field.empty());
  auto i = field.find('.');
  if (i == std::string_view::npos)
    return xs.at_key(field);

  // We have to deal with a nested field name in a potentially nested JSON
  // object.
  const auto [r, key_er] = xs.at_key(field.substr(0, i));

  if (key_er != ::simdjson::error_code::SUCCESS)
    // Attempt to access JSON field with flattened name.
    return xs.at_key(field);

  const auto [obj, to_object_er] = r.get_object();

  if (to_object_er != ::simdjson::SUCCESS)
    return ::simdjson::error_code::INCORRECT_TYPE;

  field.remove_prefix(i + 1);
  return lookup(field, obj);
}

} // namespace

caf::error add(table_slice_builder& builder, const ::simdjson::dom::object& xs,
               const record_type& layout) {
  for (auto& field : layout.fields) {
    auto [el, er] = lookup(field.name, xs);
    // Non-existing fields are treated as empty (unset).
    if (er != ::simdjson::SUCCESS) {
      if (!builder.add(make_data_view(caf::none)))
        return make_error(ec::unspecified, "failed to add caf::none to table "
                                           "slice builder");
      continue;
    }

    auto x = convert(el, field.type);
    if (!x) {
      return make_error(ec::convert_error, x.error().context(),
                        "could not convert", field.name);
    }

    if (!builder.add(make_data_view(*x)))
      return make_error(ec::type_clash, "unexpected type", field.name);
  }
  return caf::none;
}

} // namespace vast::format::simdjson
