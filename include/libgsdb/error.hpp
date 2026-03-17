#ifndef GSDB_ERROR_HPP
#define GSDB_ERROR_HPP

#include <cerrno>
#include <cstring>
#include <stdexcept>

namespace gsdb {
class error : public std::runtime_error {
   public:
    [[noreturn]]
    static void send(const std::string& what) {
        throw error(what);
    }

    /**
     * Uses the contents of errno as the error description, adding the message
     * we provide as a prefix.
     */
    [[noreturn]]
    static void send_errno(const std::string& prefix) {
        throw error(prefix + ": " + std::strerror(errno));
    }

   private:
    error(const std::string& what) : std::runtime_error(what) {}
};
}  // namespace gsdb

#endif
