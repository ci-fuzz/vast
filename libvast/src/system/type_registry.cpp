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

#include "vast/system/type_registry.hpp"

#include "vast/as_bytes.hpp"
#include "vast/defaults.hpp"
#include "vast/detail/fill_status_map.hpp"
#include "vast/error.hpp"
#include "vast/event_types.hpp"
#include "vast/io/read.hpp"
#include "vast/io/save.hpp"
#include "vast/logger.hpp"
#include "vast/system/report.hpp"
#include "vast/system/status_verbosity.hpp"
#include "vast/table_slice.hpp"

#include <caf/attach_stream_sink.hpp>
#include <caf/binary_deserializer.hpp>
#include <caf/binary_serializer.hpp>
#include <caf/expected.hpp>
#include <caf/settings.hpp>

namespace vast::system {

report type_registry_state::telemetry() const {
  // TODO: Generate a status report for the accountant.
  return {};
}

caf::dictionary<caf::config_value>
type_registry_state::status(status_verbosity v) const {
  auto result = caf::settings{};
  auto& tr_status = put_dictionary(result, "type-registry");
  if (v >= status_verbosity::detailed) {
    // The list of defined concepts
    if (v >= status_verbosity::debug) {
      auto& concepts_status = put_list(tr_status, "concepts");
      for (auto& [name, definition] : taxonomies.concepts) {
        auto& concept_status = concepts_status.emplace_back().as_dictionary();
        concept_status["name"] = name;
        concept_status["description"] = definition.description;
        concept_status["fields"] = definition.fields;
        concept_status["concepts"] = definition.concepts;
      }
      auto& models_status = put_list(tr_status, "models");
      for (auto& [name, definition] : taxonomies.models) {
        auto& model_status = models_status.emplace_back().as_dictionary();
        model_status["name"] = name;
        model_status["description"] = definition.description;
        model_status["definition"] = definition.definition;
      }
      // Sorted list of all keys.
      auto keys = std::vector<std::string>(data.size());
      std::transform(data.begin(), data.end(), keys.begin(),
                     [](const auto& x) { return x.first; });
      std::sort(keys.begin(), keys.end());
      caf::put(tr_status, "types", keys);
      // The usual per-component status.
      detail::fill_status_map(tr_status, self);
    }
  }
  return result;
}

vast::path type_registry_state::filename() const {
  return dir / name;
}

caf::error type_registry_state::save_to_disk() const {
  std::vector<char> buffer;
  caf::binary_serializer sink{self->system(), buffer};
  if (auto error = sink(data))
    return error;
  return io::save(filename(), as_bytes(buffer));
}

caf::error type_registry_state::load_from_disk() {
  // Nothing to load is not an error.
  if (!exists(dir)) {
    VAST_DEBUG("{} found no directory to load from", self);
    return caf::none;
  }
  if (auto fname = filename(); exists(fname)) {
    auto buffer = io::read(fname);
    if (!buffer)
      return buffer.error();
    caf::binary_deserializer source{self->system(), *buffer};
    if (auto error = source(data))
      return error;
    VAST_DEBUG("{} loaded state from disk", self);
  }
  return caf::none;
}

void type_registry_state::insert(vast::type layout) {
  auto& old_layouts = data[layout.name()];
  // Insert into the existing bucket.
  auto [hint, success] = old_layouts.insert(std::move(layout));
  if (success) {
    // Check whether the new layout is compatible with the latest, i.e., whether
    // the new layout is a superset of it.
    if (old_layouts.begin() != hint) {
      if (!is_subset(*old_layouts.begin(), *hint))
        VAST_WARN("{} detected an incompatible layout change for {}", self,
                  hint->name());
      else
        VAST_INFO("{} detected a layout change for {}", self, hint->name());
    }
    VAST_DEBUG("{} registered {}", self, hint->name());
  }
  // Move the newly inserted layout to the front.
  std::rotate(old_layouts.begin(), hint, std::next(hint));
}

type_set type_registry_state::types() const {
  auto result = type_set{};
  for ([[maybe_unused]] auto& [k, v] : data)
    for (auto& x : v)
      result.insert(x);
  for (auto& x : configuration_schema)
    result.insert(x);
  return result;
}

type_registry_actor::behavior_type
type_registry(type_registry_actor::stateful_pointer<type_registry_state> self,
              const path& dir) {
  self->state.self = self;
  self->state.dir = dir;
  // Register the exit handler.
  self->set_exit_handler([self](const caf::exit_msg& msg) {
    VAST_DEBUG("{} got EXIT from {}", self, msg.source);
    if (auto telemetry = self->state.telemetry(); !telemetry.empty())
      self->send(self->state.accountant, std::move(telemetry));
    if (auto err = self->state.save_to_disk())
      VAST_ERROR("{} failed to persist state to disk: {}", self,
                 self->system().render(err));
    self->quit(msg.reason);
  });
  // Load existing state from disk if possible.
  if (auto err = self->state.load_from_disk())
    self->quit(std::move(err));
  // Load loaded schema types from the singleton.
  // TODO: Move to the load handler and re-parse the files.
  if (auto schema = vast::event_types::get())
    self->state.configuration_schema = *schema;
  // The behavior of the type-registry.
  return {
    [self](atom::telemetry) {
      if (auto telemetry = self->state.telemetry(); !telemetry.empty()) {
        VAST_TRACE_SCOPE("{} sends out a telemetry report to the {}", self,
                         VAST_ARG("accountant", self->state.accountant));
        self->send(self->state.accountant, std::move(telemetry));
      }
      self->delayed_send(self, defaults::system::telemetry_rate,
                         atom::telemetry_v);
    },
    [self](atom::status, status_verbosity v) {
      VAST_TRACE_SCOPE("{} sends out a status report", self);
      return self->state.status(v);
    },
    [self](
      caf::stream<table_slice> in) -> caf::inbound_stream_slot<table_slice> {
      VAST_TRACE_SCOPE("{} attaches to {}", self, VAST_ARG("stream", in));
      auto result = caf::attach_stream_sink(
        self, in,
        [=](caf::unit_t&) {
          // nop
        },
        [=](caf::unit_t&, table_slice x) { self->state.insert(x.layout()); });
      return result.inbound_slot();
    },
    [self](atom::put, vast::type x) {
      VAST_TRACE_SCOPE("{} tries to add {}", self, VAST_ARG("type", x.name()));
      self->state.insert(std::move(x));
    },
    [self](atom::put, vast::schema x) {
      VAST_TRACE_SCOPE("{} tries to add {}", self, VAST_ARG("schema", x));
      for (auto& type : x)
        self->state.insert(std::move(type));
    },
    [self](atom::get) {
      VAST_TRACE_SCOPE("{} retrieves a list of all known types", self);
      return self->state.types();
    },
    [self](atom::put, taxonomies t) {
      VAST_TRACE_SCOPE("");
      self->state.taxonomies = std::move(t);
    },
    [self](atom::get, atom::taxonomies) {
      VAST_TRACE_SCOPE("");
      return self->state.taxonomies;
    },
    [self](atom::load) -> caf::result<atom::ok> {
      VAST_DEBUG("{} loads taxonomies", self);
      auto dirs = get_schema_dirs(self->system().config());
      concepts_map concepts;
      models_map models;
      for (const auto& dir : dirs) {
        if (!exists(dir))
          continue;
        auto yamls = load_yaml_dir(dir);
        if (!yamls)
          return yamls.error();
        for (auto& [file, yaml] : *yamls) {
          VAST_DEBUG("{} extracts taxonomies from {}", self, file.string());
          if (auto err = extract_concepts(yaml, concepts))
            return caf::make_error(ec::parse_error,
                                   "failed to extract concepts from file",
                                   file.string(), err.context());
          for (auto& [name, definition] : concepts) {
            VAST_DEBUG("{} extracted concept {} with {} fields", self, name,
                       definition.fields.size());
            for (auto& field : definition.fields)
              VAST_TRACE("{} uses concept mapping {} -> {}", self, name, field);
          }
          if (auto err = extract_models(yaml, models))
            return caf::make_error(ec::parse_error,
                                   "failed to extract models from file",
                                   file.string(), err.context());
          for (auto& [name, definition] : models) {
            VAST_DEBUG("{} extracted model {} with {} fields", self, name,
                       definition.definition.size());
            VAST_TRACE("{} uses model mapping {} -> {}", self, name,
                       definition.definition);
          }
        }
      }
      self->state.taxonomies
        = taxonomies{std::move(concepts), std::move(models)};
      return atom::ok_v;
    },
    [self](atom::resolve, const expression& e) {
      return resolve(self->state.taxonomies, e, self->state.data);
    },
    [self](accountant_actor accountant) {
      VAST_ASSERT(accountant);
      VAST_DEBUG("{} connects to {}", self, VAST_ARG(accountant));
      self->state.accountant = accountant;
      self->send(self->state.accountant, atom::announce_v, self->name());
      self->delayed_send(self, defaults::system::telemetry_rate,
                         atom::telemetry_v);
    },
  };
}

} // namespace vast::system
