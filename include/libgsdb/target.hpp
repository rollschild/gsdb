#ifndef GSDB_TARGET_HPP
#define GSDB_TARGET_HPP

#include <sys/types.h>

#include <algorithm>
#include <filesystem>
#include <libgsdb/elf.hpp>
#include <libgsdb/process.hpp>
#include <memory>
#include <optional>

namespace gsdb {
/**
 * Manage the symbolic level of the program that we’re debugging, such as
 * storing the `gsdb::elf` object for the program, reading debug information,
 * and carrying out debugger operations at the level of the source code.
 */
class target {
   public:
    target() = delete;
    target(const target&) = delete;
    target& operator=(const target&) = delete;

    /**
     * Parse the ELF file for the relevant process and set the load address of
     * the `.text` section
     */
    static std::unique_ptr<target> launch(
        std::filesystem::path path,
        std::optional<int> stdout_replacement = std::nullopt);
    static std::unique_ptr<target> attach(pid_t pid);

    process& get_process() { return *process_; }
    const process& get_process() const { return *process_; }
    elf& get_elf() { return *elf_; }
    const elf& get_elf() const { return *elf_; }

   private:
    target(std::unique_ptr<process> proc, std::unique_ptr<elf> obj)
        : process_(std::move(proc)), elf_(std::move(obj)) {}

    std::unique_ptr<process> process_;
    std::unique_ptr<elf> elf_;
};
}  // namespace gsdb

#endif
