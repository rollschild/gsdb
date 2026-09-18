#include <cstdint>

std::uint64_t g_int = 0;

int main() {
    g_int = 1;
    g_int = 42;
}

struct cat {
    const char* name;
    int age : 5;
    int color : 3;
};

struct person {
    const char* name;
    int age;
    cat* pets;
    int num_pets;
};

cat french_fries{"French Fries", 1, 1};
cat yumi{"Yumi", 0, 2};
cat milk_shake{"Milkshake", 4, 3};
cat cats[] = {french_fries, yumi, milk_shake};
person me{"me", 33, cats, 3};
