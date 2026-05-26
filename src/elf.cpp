#include <fcntl.h>
#include <sys/mman.h>
#include <sys/stat.h>
#include <unistd.h>

#include <algorithm>
#include <cstddef>
#include <filesystem>
#include <libgsdb/bit.hpp>
#include <libgsdb/elf.hpp>
#include <libgsdb/error.hpp>

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
}

gsdb::elf::~elf() {
    // unmap the memory
    munmap(data_, file_size_);
    close(fd_);
}
