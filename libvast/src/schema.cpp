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

#include "vast/schema.hpp"

#include "vast/concept/parseable/parse.hpp"
#include "vast/concept/parseable/to.hpp"
#include "vast/concept/parseable/vast/schema.hpp"
#include "vast/concept/printable/to_string.hpp"
#include "vast/concept/printable/vast/schema.hpp"
#include "vast/concept/printable/vast/type.hpp"
#include "vast/data.hpp"
#include "vast/detail/filter_dir.hpp"
#include "vast/detail/process.hpp"
#include "vast/detail/string.hpp"
#include "vast/error.hpp"
#include "vast/event_types.hpp"
#include "vast/logger.hpp"
#include "vast/path.hpp"

#include <caf/actor_system_config.hpp>

#include <filesystem>

namespace vast {

caf::expected<schema> schema::merge(const schema& s1, const schema& s2) {
  auto result = s2;
  for (auto& t : s1) {
    if (auto u = s2.find(t.name())) {
      if (t != *u && t.name() == u->name())
        // Type clash: cannot accommodate two types with same name.
        return caf::make_error(ec::format_error,
                               "type clash: cannot accommodate two types with "
                               "the "
                               "same name:",
                               t.name());
    } else {
      result.types_.push_back(t);
    }
  }
  return result;
}

schema schema::combine(const schema& s1, const schema& s2) {
  auto result = s1;
  for (auto& t : s2) {
    if (auto x = result.find(t.name()))
      *x = t;
    else
      result.add(t);
  }
  return result;
}

bool schema::add(const type& t) {
  if (caf::holds_alternative<none_type>(t) || t.name().empty()
      || find(t.name()))
    return false;
  types_.push_back(std::move(t));
  return true;
}

type* schema::find(std::string_view name) {
  for (auto& t : types_)
    if (t.name() == name)
      return &t;
  return nullptr;
}

const type* schema::find(std::string_view name) const {
  for (auto& t : types_)
    if (t.name() == name)
      return &t;
  return nullptr;
}

schema::const_iterator schema::begin() const {
  return types_.begin();
}

schema::const_iterator schema::end() const {
  return types_.end();
}

size_t schema::size() const {
  return types_.size();
}

bool schema::empty() const {
  return types_.empty();
}

void schema::clear() {
  types_.clear();
}

bool operator==(const schema& x, const schema& y) {
  return x.types_ == y.types_;
}

// TODO: we should figure out a better way to (de)serialize: use manual pointer
// tracking to save types exactly once. Something along those lines:
//
// #include <utility>
//
// namespace {
//
// struct pointer_hash {
//  size_t operator()(const type& t) const noexcept {
//    return reinterpret_cast<size_t>(std::launder(t.ptr_.get()));
//  }
//};
//
// using type_cache = std::unordered_set<type, pointer_hash>;
//
// template <class Serializer>
// struct type_serializer {
//
//  type_serializer(Serializer& sink, type_cache& cache)
//    : sink_{sink}, cache_{cache} {
//  }
//
//  void save_type(type const t) const {
//    if (t.name().empty()) {
//      visit(*this, t); // recurse
//      return;
//    }
//    if (cache_.count(t)) {
//      sink_ << t.name();
//      return;
//    }
//    visit(*this, t); // recurse
//    cache_.insert(t.name());
//  }
//
//  template <class T>
//  void operator()(const T& x) const {
//    sink_ << x;
//  };
//
//  void operator()(const vector_type& t) const {
//    save_type(t.value_type);
//  }
//
//  void operator()(const table_type& t) const {
//    save_type(t.key_type);
//    save_type(t.value_type);
//  }
//
//  void operator()(const record_type& t) const {
//    auto size = t.fields.size();
//    sink_.begin_sequence(size);
//    for (auto& f : t.fields) {
//      sink_ << f.name;
//      save_type(f.type);
//    }
//    sink_.end_sequence();
//  }
//
//  Serializer& sink_;
//  type_cache& cache_;
//};
//
//} // namespace <anonymous>

void serialize(caf::serializer& sink, const schema& sch) {
  sink << to_string(sch);
}

void serialize(caf::deserializer& source, schema& sch) {
  std::string str;
  source >> str;
  if (str.empty())
    return;
  sch.clear();
  auto i = str.begin();
  parse(i, str.end(), sch);
}

bool convert(const schema& s, data& d) {
  record o;
  list a;
  std::transform(s.begin(), s.end(), std::back_inserter(a),
                 [](auto& t) { return to_data(t); });
  o["types"] = std::move(a);
  d = std::move(o);
  return true;
}

caf::expected<schema> get_schema(const caf::settings& options) {
  // Get the default schema from the registry.
  auto schema_reg_ptr = event_types::get();
  auto schema = schema_reg_ptr ? *schema_reg_ptr : vast::schema{};
  // Update with an alternate schema, if requested.
  auto sc = caf::get_if<std::string>(&options, "vast.import.schema");
  auto sf = caf::get_if<std::string>(&options, "vast.import.schema-file");
  if (sc && sf)
    caf::make_error(ec::invalid_configuration,
                    "had both schema and schema-file "
                    "provided");
  if (!sc && !sf)
    return schema;
  caf::expected<vast::schema> update = caf::no_error;
  if (sc)
    update = to<vast::schema>(*sc);
  else
    update = load_schema(*sf);
  if (!update)
    return update.error();
  return schema::combine(schema, *update);
}

detail::stable_set<std::filesystem::path>
get_schema_dirs(const caf::actor_system_config& cfg,
                std::vector<const void*> objpath_addresses) {
  detail::stable_set<std::filesystem::path> result;
  if (caf::get_or(cfg, "vast.no-default-schema", false)) {
    VAST_WARN("the option 'vast.no-default-schema' is deprecated and will be "
              "removed in a future release");
  } else {
#if !VAST_ENABLE_RELOCATABLE_INSTALLATIONS
    result.insert(VAST_DATADIR "/vast/schema");
#endif
    // Get filesystem path to the executable.
    for (const void* addr : objpath_addresses) {
      if (const auto& binary = detail::objectpath(addr); binary)
        result.insert(binary->parent_path().parent_path() / "share" / "vast"
                      / "schema");
      else
        VAST_ERROR("{} failed to get program path", __func__);
    }
    result.insert(std::filesystem::path{VAST_SYSCONFDIR} / "vast" / "schema");
    if (const char* xdg_config_home = std::getenv("XDG_CONFIG_HOME"))
      result.insert(std::filesystem::path{xdg_config_home} / "vast" / "schema");
    else if (const char* home = std::getenv("HOME"))
      result.insert(std::filesystem::path{home} / ".config" / "vast"
                    / "schema");
  }
  if (auto dirs = caf::get_if<std::vector<std::string>>( //
        &cfg, "vast.schema-dirs"))
    result.insert(dirs->begin(), dirs->end());
  return result;
}

caf::expected<schema> load_schema(const path& schema_file) {
  if (schema_file.empty())
    return caf::make_error(ec::filesystem_error, "empty path");
  auto str = load_contents(schema_file);
  if (!str)
    return str.error();
  return to<schema>(*str);
}

caf::error load_symbols(const path& schema_file, symbol_map& local) {
  if (schema_file.empty())
    return caf::make_error(ec::filesystem_error, "empty path");
  auto str = load_contents(schema_file);
  if (!str)
    return str.error();
  auto p = symbol_map_parser{};
  if (!p(*str, local))
    return caf::make_error(ec::parse_error, "failed to load symbols from",
                           schema_file);
  return caf::none;
}

caf::expected<schema>
load_schema(const detail::stable_set<std::filesystem::path>& schema_dirs,
            size_t max_recursion) {
  if (max_recursion == 0)
    return ec::recursion_limit_reached;
  vast::schema types;
  symbol_map global_symbols;
  for (const auto& dir : schema_dirs) {
    VAST_VERBOSE("loading schemas from {}", dir);
    std::error_code err{};
    const auto file_exists = std::filesystem::exists(dir, err);
    if (!file_exists || err) {
      VAST_DEBUG("{} skips non-existing directory: {}", __func__, dir);
      continue;
    }
    auto filter = [](const std::filesystem::path& f) {
      return detail::ends_with(f.string(), ".schema");
    };
    auto schema_files
      = detail::filter_dir(dir, std::move(filter), max_recursion);
    if (!schema_files)
      return caf::make_error(ec::filesystem_error,
                             fmt::format("failed to filter schema dir at {}: "
                                         "{}",
                                         dir, schema_files.error()));
    symbol_map local_symbols;
    for (const auto& f : *schema_files) {
      VAST_DEBUG("loading schema {}", f);
      if (auto err = load_symbols(path{f.string()}, local_symbols))
        return err;
    }
    auto r = symbol_resolver{global_symbols, local_symbols};
    auto directory_schema = r.resolve();
    if (!directory_schema)
      return caf::make_error(ec::format_error, "failed to resolve types in",
                             dir.string(), directory_schema.error().context());
    local_symbols.merge(std::move(global_symbols));
    global_symbols = std::move(local_symbols);
    types = schema::combine(types, *directory_schema);
  }
  return types;
}

caf::expected<vast::schema> load_schema(const caf::actor_system_config& cfg) {
  return load_schema(get_schema_dirs(cfg));
}

} // namespace vast
