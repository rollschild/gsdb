#include <Zycore/Status.h>
#include <Zydis/Disassembler.h>
#include <Zydis/SharedTypes.h>
#include <Zydis/Zydis.h>

#include <libgsdb/disassembler.hpp>
#include <string>
#include <vector>

#include "libgsdb/types.hpp"

std::vector<gsdb::disassembler::instruction> gsdb::disassembler::disassemble(
    std::size_t n_instructions, std::optional<virt_addr> address) {
    std::vector<instruction> ret;
    ret.reserve(n_instructions);

    if (!address) {
        // constructs the contained value in-place
        address.emplace(process_->get_pc());
    }
    // largest x64 instruction is 15 bytes
    auto code =
        process_->read_memory_without_traps(*address, n_instructions * 15);

    ZyanUSize offset = 0;
    ZydisDisassembledInstruction instr;

    // populate `instr` with the disassembled instruction information
    // decodes bytes at the current offset into human-readable ATT-syntax string
    // ie. `movq %rsp, %rbp`
    while (ZYAN_SUCCESS(ZydisDisassembleATT(
               ZYDIS_MACHINE_MODE_LONG_64, address->addr(),
               code.data() + offset, code.size() - offset, &instr)) and
           n_instructions > 0) {
        ret.push_back(instruction{*address, std::string(instr.text)});
        // `instr.info.length`: size in bytes of the instruction that Zydis just
        // decoded
        offset += instr.info.length;
        *address += instr.info.length;
        --n_instructions;
    }

    return ret;
}
