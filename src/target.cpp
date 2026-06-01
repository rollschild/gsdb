#include <elf.h>

#include <algorithm>
#include <filesystem>
#include <libgsdb/target.hpp>
#include <libgsdb/types.hpp>
#include <memory>
#include <string>

#include "libgsdb/process.hpp"

namespace {
/**
 * Used inside both `launch` and `attach`.
 * Get the auxiliary vector for the process.
 * Create an `gsdb::elf` object for the file at the given path.
 * Then set the load address of its `.text` section
 */
std::unique_ptr<gsdb::elf> create_loaded_elf(
    const gsdb::process& proc, const std::filesystem::path& path) {
    auto auxv = proc.get_auxv();
    auto obj = std::make_unique<gsdb::elf>(path);
    obj->notify_loaded(
        // by subtracting the load address of the entry point in the ELF header
        // from the actual load address of the entry point
        gsdb::virt_addr(auxv[AT_ENTRY] - obj->get_header().e_entry));
    return obj;
}
}  // namespace

std::unique_ptr<gsdb::target> gsdb::target::launch(
    std::filesystem::path path, std::optional<int> stdout_replacement) {
    auto proc = process::launch(path, true, stdout_replacement);
    auto obj = create_loaded_elf(*proc, path);
    return std::unique_ptr<target>(new target(std::move(proc), std::move(obj)));
}

std::unique_ptr<gsdb::target> gsdb::target::attach(pid_t pid) {
    // Joining paths.
    // The `/proc/<pid>/exe` file is a symbolic link to the executable for the
    // given process.
    auto elf_path =
        std::filesystem::path("/proc/") / std::to_string(pid) / "exe";
    auto proc = process::attach(pid);
    auto obj = create_loaded_elf(*proc, elf_path);
    return std::unique_ptr<target>(new target(std::move(proc), std::move(obj)));
}
