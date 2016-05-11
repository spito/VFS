extern "C" {
extern void abort();
void __divine_problem( int, const char * ) noexcept {
    abort();
}
void __divine_interrupt_mask() noexcept {}
void __divine_interrupt_unmask() noexcept {}
void __divine_assert( int value ) noexcept {
    if ( !value ) abort();
}
}
