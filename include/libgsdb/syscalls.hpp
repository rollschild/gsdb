#ifndef GSDB_SYSCALLS_HPP
#define GSDB_SYSCALLS_HPP

#include <string_view>

namespace gsdb {
std::string_view syscall_id_to_name(int id);
int syscall_name_to_id(std::string_view name);
}  // namespace gsdb

#endif
