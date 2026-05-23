#include <signal.h>
#include <unistd.h>

#include <cstdio>
#include <numeric>

// std::puts(s) writes the null-terminated string s to stdout followed by a
// newline
void an_innocent_function() { std::puts("Vim better than Emacs..."); }

void an_innocent_function_end() {}

/**
 * section hashing
 */
int checksum() {
    // volatile: tell the compiler that these bytes could change at any time, so
    // it can't optimize the code by removing any loads from the memory
    auto start = reinterpret_cast<volatile const char*>(&an_innocent_function);
    auto end =
        reinterpret_cast<volatile const char*>(&an_innocent_function_end);
    return std::accumulate(start, end, 0);
}

int main() {
    // simulate situations where no breakpoint is set
    auto safe = checksum();

    // write address of the function to stdout
    // 8 bytes
    // On Linux, PIPE_BUF is 4096 bytes - max size for which write() to a pipe
    // is guaranteed atomic
    auto ptr = reinterpret_cast<void*>(&an_innocent_function);
    write(STDOUT_FILENO, &ptr, sizeof(void*));
    fflush(stdout);

    raise(SIGTRAP);

    while (true) {
        if (checksum() == safe) {
            an_innocent_function();
        } else {
            puts("NeoVim is even better than Vim...");
        }

        fflush(stdout);
        raise(SIGTRAP);
    }
}
