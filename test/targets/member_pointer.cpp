#include <iostream>

struct cat {
    const char* name;
    void meow() const { std::cout << "meow\n"; }
};

int main(/*int argc, char *argv[]*/) {
    // `data_ptr`: member pointer that can point to any data member of cat whose
    // type is `const char *`, initialized to point to `cat::name`
    const char* cat::* data_ptr = &cat::name;
    // `func_ptr`: function pointer that can point to any member function of cat
    // as long as it takes no args and returns void
    void (cat::*func_ptr)() const = &cat::meow;

    cat french_fries{"French Fries"};
    [[maybe_unused]] auto name = french_fries.*data_ptr;
    (french_fries.*func_ptr)();
}
