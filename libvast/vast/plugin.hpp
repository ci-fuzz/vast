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

#pragma once

#include "vast/fwd.hpp"

#include "vast/command.hpp"
#include "vast/config.hpp"
#include "vast/system/actors.hpp"

#include <caf/actor_system_config.hpp>
#include <caf/error.hpp>
#include <caf/stream.hpp>
#include <caf/typed_actor.hpp>

#include <cstdint>
#include <memory>
#include <vector>

namespace vast {

// -- plugin singleton ---------------------------------------------------------

namespace plugins {

/// Retrieves the system-wide plugin singleton.
std::vector<plugin_ptr>& get() noexcept;

} // namespace plugins

// -- plugin version -----------------------------------------------------------

/// The version of a plugin in format major.minor.patch.tweak.
extern "C" struct plugin_version {
  uint16_t major;
  uint16_t minor;
  uint16_t patch;
  uint16_t tweak;
};

/// @relates plugin_version
std::string to_string(plugin_version x);

/// Checks if a version meets the plugin version requirements.
/// @param version The version to compare against the requirements.
bool has_required_version(const plugin_version& version) noexcept;

/// Support CAF type-inspection.
/// @relates plugin_version
template <class Inspector>
auto inspect(Inspector& f, plugin_version& x) ->
  typename Inspector::result_type {
  return f(x.major, x.minor, x.patch, x.tweak);
}

// -- plugin type ID blocks ----------------------------------------------------

/// The type ID block used by a plugin as [begin, end).
extern "C" struct plugin_type_id_block {
  uint16_t begin;
  uint16_t end;
};

/// Support CAF type-inspection.
/// @relates plugin_type_id_block
template <class Inspector>
auto inspect(Inspector& f, plugin_type_id_block& x) ->
  typename Inspector::result_type {
  return f(x.begin, x.end);
}

// -- plugin -------------------------------------------------------------------

/// The plugin base class.
class plugin {
public:
  /// The current version of the plugin API. When registering a plugin, set the
  /// corresponding plugin version in the `VAST_REGISTER_PLUGIN` macro.
  constexpr static auto version = plugin_version{0, 1, 0, 0};

  /// Destroys any runtime state that the plugin created. For example,
  /// de-register from existing components, deallocate memory.
  virtual ~plugin() noexcept = default;

  /// Initializes a plugin with its respective entries from the YAML config
  /// file, i.e., `plugin.<NAME>`.
  /// @param config The relevant subsection of the configuration.
  virtual caf::error initialize(data config) = 0;

  /// Returns the unique name of the plugin.
  virtual const char* name() const = 0;
};

// -- analyzer plugin ----------------------------------------------------------

/// A base class for plugins that hook into the input stream.
/// @relates plugin
class analyzer_plugin : public virtual plugin {
public:
  /// Creates an actor that hooks into the input table slice stream.
  /// @param node A pointer to the NODE actor handle.
  /// @returns The actor handle to the analyzer.
  virtual system::analyzer_plugin_actor
  make_analyzer(system::node_actor::pointer node) const = 0;
};

// -- command plugin -----------------------------------------------------------

/// A base class for plugins that add commands.
/// @relates plugin
class command_plugin : public virtual plugin {
public:
  /// Creates additional commands.
  /// @note VAST calls this function before initializing the plugin, which means
  /// that this function cannot depend on any plugin state. The logger is
  /// unavailable when this function is called.
  virtual std::pair<std::unique_ptr<command>, command::factory>
  make_command() const = 0;
};

// -- plugin_ptr ---------------------------------------------------------------

/// An owned plugin and dynamically loaded plugin.
/// @relates plugin
class plugin_ptr final {
public:
  /// Load a plugin from the specified library filename.
  /// @param filename The filename that's passed to 'dlopen'.
  /// @param cfg The actor system config to register type IDs with.
  static caf::expected<plugin_ptr>
  make(const char* filename, caf::actor_system_config& cfg) noexcept;

  /// Unload a plugin and its required resources.
  ~plugin_ptr() noexcept;

  /// Forbid copying of plugins.
  plugin_ptr(const plugin_ptr&) = delete;
  plugin_ptr& operator=(const plugin_ptr&) = delete;

  /// Move-construction and move-assignment.
  plugin_ptr(plugin_ptr&& other) noexcept;
  plugin_ptr& operator=(plugin_ptr&& rhs) noexcept;

  /// Pointer facade.
  explicit operator bool() noexcept;
  const plugin* operator->() const noexcept;
  plugin* operator->() noexcept;
  const plugin& operator*() const noexcept;
  plugin& operator&() noexcept;

  /// Upcast a plugin to a more specific plugin type.
  /// @tparam Plugin The specific plugin type to try to upcast to.
  /// @returns A pointer to the upcasted plugin, or 'nullptr' on failure.
  template <class Plugin>
  const Plugin* as() const {
    static_assert(std::is_base_of_v<plugin, Plugin>, "'Plugin' must be derived "
                                                     "from 'vast::plugin'");
    return dynamic_cast<const Plugin*>(instance_);
  }

  /// Upcast a plugin to a more specific plugin type.
  /// @tparam Plugin The specific plugin type to try to upcast to.
  /// @returns A pointer to the upcasted plugin, or 'nullptr' on failure.
  template <class Plugin>
  Plugin* as() {
    static_assert(std::is_base_of_v<plugin, Plugin>, "'Plugin' must be derived "
                                                     "from 'vast::plugin'");
    return dynamic_cast<Plugin*>(instance_);
  }

  /// Returns the plugin version.
  plugin_version version() const;

private:
  /// Create a plugin_ptr.
  plugin_ptr(void* library, plugin* instance,
             void (*deleter)(plugin*)) noexcept;

  /// Implementation details.
  void* library_ = {};
  plugin* instance_ = {};
  void (*deleter_)(plugin*) = {};
};

} // namespace vast

// -- helper macros ------------------------------------------------------------

// NOLINTNEXTLINE
#define VAST_REGISTER_PLUGIN(name, major, minor, tweak, patch)                 \
  extern "C" ::vast::plugin* vast_plugin_create() {                            \
    return new (name);                                                         \
  }                                                                            \
  extern "C" void vast_plugin_destroy(class ::vast::plugin* plugin) {          \
    delete plugin;                                                             \
  }                                                                            \
  extern "C" struct ::vast::plugin_version vast_plugin_version() {             \
    return {major, minor, tweak, patch};                                       \
  }                                                                            \
  extern "C" const char* vast_libvast_version() {                              \
    return ::vast::version::version;                                           \
  }                                                                            \
  extern "C" const char* vast_libvast_build_tree_hash() {                      \
    return ::vast::version::build_tree_hash;                                   \
  }

// NOLINTNEXTLINE
#define VAST_REGISTER_PLUGIN_TYPE_ID_BLOCK(...)                                \
  VAST_PP_OVERLOAD(VAST_REGISTER_PLUGIN_TYPE_ID_BLOCK_, __VA_ARGS__)           \
  (__VA_ARGS__)

// NOLINTNEXTLINE
#define VAST_REGISTER_PLUGIN_TYPE_ID_BLOCK_1(name)                             \
  extern "C" void vast_plugin_register_type_id_block(                          \
    ::caf::actor_system_config& cfg) {                                         \
    cfg.add_message_types<::caf::id_block::name>();                            \
  }                                                                            \
  extern "C" ::vast::plugin_type_id_block vast_plugin_type_id_block() {        \
    return {::caf::id_block::name::begin, ::caf::id_block::name::end};         \
  }

// NOLINTNEXTLINE
#define VAST_REGISTER_PLUGIN_TYPE_ID_BLOCK_2(name1, name2)                     \
  extern "C" void vast_plugin_register_type_id_block(                          \
    ::caf::actor_system_config& cfg) {                                         \
    cfg.add_message_types<::caf::id_block::name1>();                           \
    cfg.add_message_types<::caf::id_block::name2>();                           \
  }                                                                            \
  extern "C" ::vast::plugin_type_id_block vast_plugin_type_id_block() {        \
    return {::caf::id_block::name1::begin < ::caf::id_block::name2::begin      \
              ? ::caf::id_block::name1::begin                                  \
              : ::caf::id_block::name2::begin,                                 \
            ::caf::id_block::name1::end > ::caf::id_block::name2::end          \
              ? ::caf::id_block::name1::end                                    \
              : ::caf::id_block::name2::end};                                  \
  }
