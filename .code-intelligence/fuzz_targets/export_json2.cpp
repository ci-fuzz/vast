#include <stddef.h>
#include <stdint.h>

#include "vast/event_types.hpp"
#include "vast/logger.hpp"
#include "vast/schema.hpp"
#include "vast/system/application.hpp"
#include "vast/system/default_configuration.hpp"

#include <caf/actor_system.hpp>

// extern "C" int FUZZ_INIT_WITH_ARGS(int *argc, char ***argv) {
extern "C" int FUZZ_INIT() {

  // Add global setup code here - called once before fuzzing starts

  return 0; // Non-zero return values are reserved for future use.
}

extern "C" int FUZZ(const char *Data, size_t Size) {
  // char* argv = "program_name";
  char *argv[2];
  argv[0] = "name";
  int argc = 2;
  vast::system::default_configuration cfg;
  auto [root, root_factory] = vast::system::make_application(argv[0]);
  if (!root)
    return 0;
  //const char* query = argc > 1 ? argv[1] : Data;

  std::string s(reinterpret_cast<const char*>(Data), Size);

  std::vector<std::string> command_line{"--node", "export", "json", s.c_str()};
  auto invocation = parse(*root, command_line.begin(), command_line.end());

  // process fuzzer input (*Data) and setup local objects necessary to call the function under test

  //CallYourAPI(Data, Size); // TODO call your API here

  // reset state and free all locally allocated resources

  return 0; // Non-zero return values are reserved for future use.
}
