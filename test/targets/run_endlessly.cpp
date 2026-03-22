int main() {
    // volatile prevents the compiler from optimizing away the store
    volatile int i;
    while (true) {
        i = 42;
        (void)i;
    }
}
