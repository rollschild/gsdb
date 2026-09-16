#include <iostream>

int main([[maybe_unused]] int argc, [[maybe_unused]] char** argv) {
    int i = 1;
    std::cout << i;
    {
        int i = 2;
        std::cout << i;
        {
            int i = 3;
            std::cout << i;
        }
    }
}
