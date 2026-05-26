#ifndef GSDB_ELF_HPP
#define GSDB_ELF_HPP

#include <elf.h>

#include <cstddef>
#include <filesystem>

namespace gsdb {
class elf {
   public:
    // Takes path to an ELF file on disk
    elf(const std::filesystem::path& path);
    ~elf();

    elf(const elf&) = delete;
    elf& operator=(const elf&) = delete;

    std::filesystem::path path() const { return path_; }
    const Elf64_Ehdr& get_header() const { return header_; }

   private:
    int fd_;
    std::filesystem::path path_;
    std::size_t file_size_;
    std::byte* data_;
    Elf64_Ehdr header_;
};
}  // namespace gsdb

#endif
