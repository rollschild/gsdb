#include <fcntl.h>
#include <unistd.h>

#include <cstddef>
#include <libgsdb/error.hpp>
#include <libgsdb/pipe.hpp>
#include <utility>
#include <vector>

gsdb::pipe::pipe(bool close_on_exec) {
    if (pipe2(fds_, close_on_exec ? O_CLOEXEC : 0) < 0) {
        error::send_errno("Pipe creation failed!");
    }
}

gsdb::pipe::~pipe() {
    close_read();
    close_write();
}

/**
 * Return the current file descriptor for the relevant end of the pipe and set
 * it to some empty file descriptor
 */
int gsdb::pipe::release_read() { return std::exchange(fds_[read_fd], -1); }
int gsdb::pipe::release_write() { return std::exchange(fds_[write_fd], -1); }

void gsdb::pipe::close_read() {
    if (fds_[read_fd] != -1) {
        close(fds_[read_fd]);
        fds_[read_fd] = -1;
    }
}
void gsdb::pipe::close_write() {
    if (fds_[write_fd] != -1) {
        close(fds_[write_fd]);
        fds_[write_fd] = -1;
    }
}

std::vector<std::byte> gsdb::pipe::read() {
    char buf[1024];
    int chars_read;

    // :: global scope resolution
    // calling `read()` from `<unistd.h>`
    if ((chars_read = ::read(fds_[read_fd], buf, sizeof(buf))) < 0) {
        error::send_errno("Could not read from pipe");
    }

    auto bytes = reinterpret_cast<std::byte*>(buf);
    // raw pointers are valid iterators
    return std::vector<std::byte>(bytes, bytes + chars_read);
}

void gsdb::pipe::write(std::byte* from, std::size_t bytes) {
    if (::write(fds_[write_fd], from, bytes) < 0) {
        error::send_errno("Could not write to pipe!");
    }
}
