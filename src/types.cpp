#include <cassert>
#include <libgsdb/elf.hpp>
#include <libgsdb/types.hpp>

gsdb::virt_addr gsdb::file_addr::to_virt_addr() const {
    assert(elf_ && "to_virt_addr called on NULL address");
    // locate the section that contains the file address to ensure
    // this file address is valid for the ELF file
    auto section = elf_->get_section_containing_address(*this);
    if (!section) {
        return virt_addr{};
    }

    return virt_addr{addr_ + elf_->load_bias().addr()};
}

gsdb::file_addr gsdb::virt_addr::to_file_addr(const elf& obj) const {
    auto section = obj.get_section_containing_address(*this);
    if (!section) {
        return file_addr{};
    }

    return file_addr{obj, addr_ - obj.load_bias().addr()};
}
