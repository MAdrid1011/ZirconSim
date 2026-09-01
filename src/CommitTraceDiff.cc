#include <cstdlib>
#include <fstream>
#include <iostream>
#include <stdexcept>
#include <string>

#include "CommitTrace.h"

namespace {

struct Options {
  std::string zircon_trace;
  std::string spike_log;
  std::string sail_log;
  std::string memory_elf;
  std::string backing_memory;
  size_t max_events = 0;
};

size_t parseCount(const char* text) {
  char* end = nullptr;
  const unsigned long long value = std::strtoull(text, &end, 0);
  if (text[0] == '\0' || end == nullptr || *end != '\0' || value == 0) {
    throw std::invalid_argument("--max-events must be a nonzero integer");
  }
  return static_cast<size_t>(value);
}

Options parseOptions(int argc, char** argv) {
  Options options;
  for (int index = 1; index < argc; ++index) {
    const std::string argument = argv[index];
    if ((argument == "--zircon-trace" || argument == "--spike-log" ||
         argument == "--sail-log" || argument == "--memory-elf" ||
         argument == "--backing-memory" ||
         argument == "--max-events") && index + 1 >= argc) {
      throw std::invalid_argument("missing value for " + argument);
    }
    if (argument == "--zircon-trace") {
      options.zircon_trace = argv[++index];
    } else if (argument == "--spike-log") {
      options.spike_log = argv[++index];
    } else if (argument == "--sail-log") {
      options.sail_log = argv[++index];
    } else if (argument == "--memory-elf") {
      options.memory_elf = argv[++index];
    } else if (argument == "--backing-memory") {
      options.backing_memory = argv[++index];
    } else if (argument == "--max-events") {
      options.max_events = parseCount(argv[++index]);
    } else {
      throw std::invalid_argument("unknown option: " + argument);
    }
  }
  if (options.zircon_trace.empty() || options.max_events == 0 ||
      (options.spike_log.empty() == options.sail_log.empty())) {
    throw std::invalid_argument("--zircon-trace, exactly one reference log, and --max-events are required");
  }
  if (options.memory_elf.empty() != options.backing_memory.empty()) {
    throw std::invalid_argument("--memory-elf and --backing-memory must be supplied together");
  }
  return options;
}

}  // namespace

int main(int argc, char** argv) {
  try {
    const Options options = parseOptions(argc, argv);
    std::ifstream zircon_input(options.zircon_trace);
    std::ifstream reference_input(options.spike_log.empty() ? options.sail_log : options.spike_log);
    if (!zircon_input || !reference_input) {
      throw std::runtime_error("cannot open a commit trace input");
    }
    const auto zircon = zircon::sim::parseZirconTrace(zircon_input, options.max_events);
    const bool is_sail = !options.sail_log.empty();
    const auto reference = is_sail ?
      zircon::sim::parseSailCommitLog(reference_input, options.max_events) :
      zircon::sim::parseSpikeCommitLog(reference_input, options.max_events);
    if (zircon.size() != options.max_events || reference.size() != options.max_events) {
      throw std::runtime_error("commit trace is shorter than --max-events");
    }
    if (options.memory_elf.empty()) {
      zircon::sim::compareCommitPrefixes(zircon, reference);
      std::cout << "commit-diff: " << options.max_events << " ordered retirements matched against "
                << (is_sail ? "Sail" : "Spike") << std::endl;
    } else {
      std::ifstream backing_input(options.backing_memory);
      if (!backing_input) {
        throw std::runtime_error("cannot open backing-memory snapshot");
      }
      const auto image = zircon::sim::ElfImage::load(options.memory_elf);
      const auto backing_memory = zircon::sim::parseBackingMemorySnapshot(backing_input);
      zircon::sim::compareCommittedMemory(zircon, reference, image.memory(), backing_memory);
      std::cout << "commit-memory-diff: " << options.max_events
                << " ordered retirements and AXI backing memory matched against "
                << (is_sail ? "Sail" : "Spike") << std::endl;
    }
    return 0;
  } catch (const std::exception& error) {
    std::cerr << "commit-diff: " << error.what() << std::endl;
    return 2;
  }
}
