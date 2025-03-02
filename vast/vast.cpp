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

#include "vast/atoms.hpp"
#include "vast/concept/convertible/to.hpp"
#include "vast/concept/printable/to_string.hpp"
#include "vast/concept/printable/vast/data.hpp"
#include "vast/config.hpp"
#include "vast/data.hpp"
#include "vast/detail/load_plugin.hpp"
#include "vast/detail/settings.hpp"
#include "vast/detail/signal_handlers.hpp"
#include "vast/detail/system.hpp"
#include "vast/error.hpp"
#include "vast/event_types.hpp"
#include "vast/logger.hpp"
#include "vast/plugin.hpp"
#include "vast/schema.hpp"
#include "vast/system/application.hpp"
#include "vast/system/default_configuration.hpp"

#include <caf/actor_system.hpp>

#include <csignal>
#include <cstdlib>
#include <filesystem>
#include <iostream>
#include <string>
#include <vector>

using namespace vast;
using namespace vast::system;

int main(int argc, char** argv) {
  // Set a signal handler for fatal conditions. Prints a backtrace if support
  // for that is enabled.
  std::signal(SIGSEGV, fatal_handler);
  std::signal(SIGABRT, fatal_handler);
  // Set up our configuration, e.g., load of YAML config file(s).
  default_configuration cfg;
  if (auto err = cfg.parse(argc, argv)) {
    std::cerr << "failed to parse configuration: " << to_string(err)
              << std::endl;
    return EXIT_FAILURE;
  }
  // Load plugins.
  auto loaded_plugin_paths = std::vector<std::filesystem::path>{};
  auto plugin_files
    = caf::get_or(cfg, "vast.plugins", std::vector<std::string>{});
#ifdef VAST_ENABLED_PLUGINS
  // If plugins are enabled at compile time to always be loaded, add them here
  // if they were not already configured as part of vast.plugins.
  for (auto&& plugin_file : std::vector<std::string>{VAST_ENABLED_PLUGINS})
    if (std::none_of(plugin_files.begin(), plugin_files.end(),
                     [&](auto&& x) { return x == plugin_file; }))
      plugin_files.push_back(std::move(plugin_file));
#endif
  auto& plugins = plugins::get();
  for (const auto& plugin_file : plugin_files) {
    if (auto loaded_plugin = detail::load_plugin(plugin_file, cfg)) {
      auto&& [path, plugin] = std::move(*loaded_plugin);
      loaded_plugin_paths.push_back(std::move(path));
      plugins.push_back(std::move(plugin));
    } else {
      std::cerr << fmt::format("{}\n", loaded_plugin.error());
      return EXIT_FAILURE;
    }
  }
  // Application setup.
  auto [root, root_factory] = make_application(argv[0]);
  if (!root)
    return EXIT_FAILURE;
  // Parse the CLI.
  auto invocation
    = parse(*root, cfg.command_line.begin(), cfg.command_line.end());
  if (!invocation) {
    if (invocation.error()) {
      render_error(*root, invocation.error(), std::cerr);
      return EXIT_FAILURE;
    }
    // Printing help/documentation texts returns caf::no_error, and we want to
    // indicate success when printing the help/documentation texts.
    return EXIT_SUCCESS;
  }
  // Merge the options from the CLI into the options from the configuration.
  // From here on, options from the command line can be used.
  vast::detail::merge_settings(invocation->options, cfg.content);
  // Create log context as soon as we know the correct configuration.
  auto log_context = vast::create_log_context(*invocation, cfg.content);
  if (!log_context)
    return EXIT_FAILURE;
  // Print the configuration file(s) that were loaded.
  if (!cfg.config_file_path.empty())
    cfg.config_files.emplace_back(std::move(cfg.config_file_path));
  for (auto& file : cfg.config_files)
    VAST_INFO("loaded configuration file: {}", file);
  // Print the plugins that were loaded, and errors that occured during loading.
  for (const auto& file : loaded_plugin_paths)
    VAST_VERBOSE("loaded plugin: {}", file);
  // Initialize successfully loaded plugins.
  for (auto& plugin : plugins) {
    auto key = "plugins."s + plugin->name();
    if (auto opts = caf::get_if<caf::settings>(&cfg, key)) {
      if (auto config = to<data>(*opts)) {
        VAST_DEBUG("initializing plugin with options: {}", *config);
        plugin->initialize(std::move(*config));
      } else {
        VAST_ERROR("invalid plugin configuration for plugin {}",
                   plugin->name());
        plugin->initialize(data{});
      }
    } else {
      VAST_DEBUG("no configuration found for plugin {}", plugin->name());
      plugin->initialize(data{});
    }
  }
  // Set up the event types singleton.
  if (auto schema = load_schema(cfg)) {
    event_types::init(*std::move(schema));
  } else {
    VAST_ERROR("failed to read schema dirs: {}", schema.error());
    return EXIT_FAILURE;
  }
  // Lastly, initialize the actor system context, and execute the given command.
  // From this point onwards, do not execute code that is not thread-safe.
  auto sys = caf::actor_system{cfg};
  auto run_error = caf::error{};
  if (auto result = run(*invocation, sys, root_factory); !result)
    run_error = std::move(result.error());
  else
    result->apply({[&](caf::error& err) { run_error = std::move(err); }});
  if (run_error) {
    render_error(*root, run_error, std::cerr);
    return EXIT_FAILURE;
  }
  return EXIT_SUCCESS;
}
