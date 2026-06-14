#include <cxxabi.h>
#include <elf.h>
#include <fcntl.h>
#include <sys/mman.h>
#include <sys/stat.h>
#include <unistd.h>

#include <algorithm>
#include <cstddef>
#include <cstdlib>
#include <filesystem>
#include <iterator>
#include <libgsdb/bit.hpp>
#include <libgsdb/elf.hpp>
#include <libgsdb/error.hpp>
#include <optional>
#include <string_view>

#include "libgsdb/types.hpp"

gsdb::elf::elf(const std::filesystem::path& path) {
    path_ = path;

    if ((fd_ = open(path.c_str(), O_RDONLY)) < 0) {
        error::send_errno("Could NOT open ELF file!");
    }

    struct stat stats;
    if (fstat(fd_, &stats) < 0) {
        error::send_errno("Could NOT retrieve ELF file stats!");
    }
    file_size_ = stats.st_size;

    void* ret;

    // Use mmap syscall to map the large file into the virtual memory of the
    // process.
    // First arg: 0 - instructs mmap to choose where to map this memory
    // Last arg: 0 - offset; map the entire file
    if ((ret = mmap(0, file_size_, PROT_READ, MAP_SHARED, fd_, 0)) ==
        MAP_FAILED) {
        close(fd_);
        error::send_errno("Could NOT mmap ELF file!");
    }

    data_ = reinterpret_cast<std::byte*>(ret);

    std::copy(data_, data_ + sizeof(header_), as_bytes(header_));

    parse_section_headers();

    build_section_map();

    parse_symbol_table();

    build_symbol_maps();
}

gsdb::elf::~elf() {
    // unmap the memory
    munmap(data_, file_size_);
    close(fd_);
}

void gsdb::elf::parse_section_headers() {
    // If a file has 0xff00 sections or more, it sets e_shnum to 0 and stores
    // the number of sections in the sh_size field of the first section header.
    auto n_headers = header_.e_shnum;
    // e_shentsize: the section header size element
    if (n_headers == 0 and header_.e_shentsize != 0) {
        n_headers = from_bytes<Elf64_Shdr>(data_ + header_.e_shoff).sh_size;
    }
    section_headers_.resize(n_headers);
    std::copy(data_ + header_.e_shoff,
              data_ + header_.e_shoff + sizeof(Elf64_Shdr) * n_headers,
              reinterpret_cast<std::byte*>(section_headers_.data()));
}

std::string_view gsdb::elf::get_section_name(std::size_t index) const {
    // e_shstrndx: identifies the section that stores the string table for
    // section names (usually `.shstrtab`)
    auto& section = section_headers_[header_.e_shstrndx];
    return {reinterpret_cast<char*>(data_) + section.sh_offset + index};
}

void gsdb::elf::build_section_map() {
    for (auto& section : section_headers_) {
        section_map_[get_section_name(section.sh_name)] = &section;
    }
}

std::optional<const Elf64_Shdr*> gsdb::elf::get_section(
    std::string_view name) const {
    if (section_map_.count(name) == 0) {
        return std::nullopt;
    }

    return section_map_.at(name);
}

gsdb::span<const std::byte> gsdb::elf::get_section_contents(
    std::string_view name) const {
    if (auto sect = get_section(name); sect) {
        return {data_ + sect.value()->sh_offset, sect.value()->sh_size};
    }
    return {nullptr, std::size_t(0)};
}

std::string_view gsdb::elf::get_string(std::size_t index) const {
    auto opt_strtab = get_section(".strtab");
    if (!opt_strtab) {
        opt_strtab = get_section(".dynstr");
        if (!opt_strtab) {
            return "";
        }
    }
    return {reinterpret_cast<char*>(data_) + opt_strtab.value()->sh_offset +
            index};
}

const Elf64_Shdr* gsdb::elf::get_section_containing_address(
    gsdb::file_addr addr) const {
    if (addr.elf_file() != this) {
        return nullptr;
    }
    for (auto& section : section_headers_) {
        if (section.sh_addr <= addr.addr() and
            section.sh_addr + section.sh_size > addr.addr()) {
            return &section;
        }
    }

    return nullptr;
}

const Elf64_Shdr* gsdb::elf::get_section_containing_address(
    gsdb::virt_addr addr) const {
    for (auto& section : section_headers_) {
        if (addr >= load_bias_ + section.sh_addr and
            load_bias_ + section.sh_addr + section.sh_size > addr) {
            return &section;
        }
    }

    return nullptr;
}

std::optional<gsdb::file_addr> gsdb::elf::get_section_start_address(
    std::string_view name) const {
    if (auto sect = get_section(name); sect) {
        return file_addr{*this, sect.value()->sh_addr};
    }
    return std::nullopt;
}

void gsdb::elf::parse_symbol_table() {
    auto opt_symtab = get_section(".symtab");
    if (!opt_symtab) {
        opt_symtab = get_section(".dynsym");
        if (!opt_symtab) return;
    }

    auto symtab = *opt_symtab;
    // sh_entsize: size of a single entry
    symbol_table_.resize(symtab->sh_size / symtab->sh_entsize);
    std::copy(data_ + symtab->sh_offset,
              data_ + symtab->sh_offset + symtab->sh_size,
              reinterpret_cast<std::byte*>(symbol_table_.data()));
}

void gsdb::elf::build_symbol_maps() {
    // name of symbol potentially mangled
    for (auto& symbol : symbol_table_) {
        auto mangled_name = get_string(symbol.st_name);
        int demangle_status;
        // if output name buffer is nullptr, it dynamically allocates a string
        // and returns it instead - what's happing here
        auto demangled_name = abi::__cxa_demangle(mangled_name.data(), nullptr,
                                                  nullptr, &demangle_status);
        if (demangle_status == 0) {
            symbol_name_map_.insert({demangled_name, &symbol});
            free(demangled_name);
        }
        symbol_name_map_.insert({mangled_name, &symbol});

        // if the symbol has an address and a name
        if (symbol.st_value != 0 and symbol.st_name != 0 and
            // and does _NOT_ point to thread_local storage
            ELF64_ST_TYPE(symbol.st_info) != STT_TLS) {
            // maps the symbol's address range to a pointer to the symbol entry
            auto addr_range =
                std::pair(file_addr{*this, symbol.st_value},
                          file_addr{*this, symbol.st_value + symbol.st_size});
            symbol_addr_map_.insert({addr_range, &symbol});
        }
    }
}

std::vector<const Elf64_Sym*> gsdb::elf::get_symbols_by_name(
    std::string_view name) const {
    // structured bindings
    auto [begin, end] = symbol_name_map_.equal_range(name);

    std::vector<const Elf64_Sym*> ret;
    std::transform(begin, end, std::back_inserter(ret),
                   [](auto& pair) { return pair.second; });
    return ret;
}

std::optional<const Elf64_Sym*> gsdb::elf::get_symbol_at_address(
    gsdb::file_addr address) const {
    if (address.elf_file() != this) {
        return std::nullopt;
    }

    file_addr null_addr;
    // find the element in the map whose start address matches the given one
    auto it = symbol_addr_map_.find({address, null_addr});
    if (it == std::end(symbol_addr_map_)) return std::nullopt;

    return it->second;
}

std::optional<const Elf64_Sym*> gsdb::elf::get_symbol_at_address(
    gsdb::virt_addr address) const {
    return get_symbol_at_address(address.to_file_addr(*this));
}

/**
 * O(log n)
 */
std::optional<const Elf64_Sym*> gsdb::elf::get_symbol_containing_address(
    gsdb::file_addr address) const {
    if (address.elf_file() != this or symbol_addr_map_.empty()) {
        return std::nullopt;
    }

    file_addr null_addr;
    auto it = symbol_addr_map_.lower_bound({address, null_addr});
    if (it != std::end(symbol_addr_map_)) {
        if (auto [key, value] = *it; key.first == address) {
            return value;
        }
    }

    // If we didn't get an exact start match, the containing symbol (if any)
    // must be the one before it — a symbol that starts below address and
    // extends past it.
    if (it == std::begin(symbol_addr_map_)) {
        return std::nullopt;
    }

    // Step back to the symbol with the largest start address strictly less than
    // `address`
    --it;
    if (auto [key, value] = *it; key.first < address and key.second > address) {
        return value;
    }

    // Otherwise the address lands in a gap between symbols (e.g. past the end
    // of the previous symbol but before the next one starts)
    return std::nullopt;
}

std::optional<const Elf64_Sym*> gsdb::elf::get_symbol_containing_address(
    gsdb::virt_addr address) const {
    return get_symbol_containing_address(address.to_file_addr(*this));
}
