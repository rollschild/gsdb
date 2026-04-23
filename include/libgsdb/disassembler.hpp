#ifndef GSDB_DISASSEMBLER_HPP
#define GSDB_DISASSEMBLER_HPP

#include <libgsdb/process.hpp>
#include <optional>
#include <string>
#include <vector>

#include "libgsdb/types.hpp"

namespace gsdb {
class disassembler {
    struct instruction {
        virt_addr address;
        std::string text;
    };

   public:
    disassembler(process& proc) : process_(&proc) {}

    /**
     * By default it uses the current program counter value
     */
    std::vector<instruction> disassemble(
        std::size_t n_instructions,
        std::optional<virt_addr> address = std::nullopt);

   private:
    process* process_;
};
}  // namespace gsdb

#endif
