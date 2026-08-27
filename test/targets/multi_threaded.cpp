#include <pthread.h>
#include <unistd.h>

#include <iostream>
#include <vector>

void* say_hi(void*) {
    std::cout << "Thread " << gettid() << " reporting in\n";
    return nullptr;
}

int main() {
    std::vector<pthread_t> threads(10);

    for (auto& thread : threads) {
        pthread_create(&thread, nullptr, say_hi, nullptr);
    }

    for (auto& thread : threads) {
        pthread_join(thread, nullptr);
    }
}
