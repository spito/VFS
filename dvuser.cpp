#include <divine.h>
#include <divine/problem.h>
#include <atomic>
#include <errno.h>

extern "C" void *__mmap_anon( size_t len );
extern "C" void __native_putErrStr( const char *, size_t );
extern "C" void __native_putStr( const char *, size_t );
extern "C" _Noreturn void __exit( int );

extern "C" void _init();
extern "C" void exit( int );
extern "C" int main();
extern "C" void __native_start() {
//    _init();
    exit( main() );
}

extern "C" void _Unwind_Resume() { }
extern "C" void clock_gettime() { }

extern "C" _Noreturn void __die( const char *msg, size_t size ) {
    __native_putErrStr( msg, size );
    int *null = nullptr;
    null[0] = 42;
    for ( ;; ) { }
}

#define DIESTR( str ) __die( str, sizeof( str ) - 1 )
#define DIE( str ) __die( str, strlen( str ) )
#define PUTESTR( str ) __native_putErrStr( str, sizeof( str ) - 1 )

struct AllocInfo {
    void *begin = nullptr;
    void *end = nullptr;
};

struct AllocBlock {
    AllocInfo info[4096 / sizeof( AllocInfo ) - sizeof( void * )];
    AllocBlock *next = nullptr;
};

AllocBlock __allocRoot;

struct Working {
    char *block = nullptr;
    size_t size = 0;
};

Working __working;

template< typename T, typename R >
T roundUp( T val, R _radix ) {
    const T radix = _radix;
    return val % radix == 0 ? val : (val / radix + 1) * radix;
}

char *__get_block( size_t size ) noexcept {
    if ( __working.block && __working.size >= size ) {
        char *block = __working.block;
        __working.block += size;
        __working.size -= size;
        return block;
    }
    const auto asz = roundUp( size, 4096 );
    char *block = static_cast< char * >( __mmap_anon( asz ) );
    if ( asz - size > __working.size ) {
        __working.block = block + size;
        __working.size = asz - size;
    }
    return block;
}

void *__divine_malloc( size_t size ) noexcept {
    char *block = __get_block( size );
    AllocInfo bi;
    bi.begin = block;
    bi.end = block + size;

    AllocBlock *meta = &__allocRoot;
    AllocBlock *last = nullptr;
    while ( meta ) {
        for ( auto &i : meta->info ) {
            if ( !i.begin ) {
                i = bi;
                return block;
            }
        }
        last = meta;
        meta = meta->next;
    }
    last->next = reinterpret_cast< AllocBlock * >( __mmap_anon( sizeof( AllocBlock ) ) );
    last->next->info[0] = bi;
    return block;
}

void __divine_free( void *ptr ) noexcept {
}
int __divine_heap_object_size( void *ptr ) noexcept {
    AllocBlock *meta = &__allocRoot;
    while ( meta )
        for ( auto &i : meta->info )
            if ( i.begin <= ptr && ptr < i.end )
                return size_t( i.end ) - size_t( i.begin );
    DIESTR( "__divine_heap_object_size called on pointer which is not on heap" );
}
int __divine_is_private( void * ) noexcept { return false; }

void __divine_assume( int x ) noexcept {
    if ( !x )
        __exit( 1 );
}
_Noreturn void __divine_problem( int p, const char *msg ) noexcept {
    switch ( p ) {
        #define PROBLEM(x) case x: PUTESTR( "DIVINE problem: " #x "\n" ); break;
        #include <divine/problem.def>
        #undef PROBLEM
    }
    DIESTR( "__divine_problem called, terminating\n" );
}
void __divine_assert( int x ) noexcept {
    if ( !x )
        __divine_problem( Assert, nullptr );
}
void __divine_ap( int id ) noexcept { }

int __divine_new_thread( void (*entry)(void *), void *arg ) noexcept {
    DIESTR( "Threading not yet supported" );
}
int __divine_get_tid() noexcept { return 0; }

void *__divine_va_start( void ) noexcept {
    DIESTR( "va_args are not yet supported" );
}

int __divine_choice( int, ... ) noexcept { return 0; }

void *__divine_memcpy( void *_dest, void *_src, size_t count ) noexcept {
    char *dest = static_cast< char * >( _dest );
    char *src = static_cast< char * >( _src );
    while ( count > 0 ) {
        *dest++ = *src++;
        --count;
    }
    return _dest;
}

void __divine_unwind( int, ... ) noexcept {
    DIESTR( "Exceptions are not yet supported" );
}

_DivineLP_Info *__divine_landingpad( int frameid ) noexcept {
    DIESTR( "Exceptions are not yet supported" );
}

std::atomic< int > __interruptMask = ATOMIC_VAR_INIT( -1 );

void __divine_interrupt() noexcept { }
int __divine_interrupt_mask() noexcept {
    int expected = -1;
    int tid = __divine_get_tid();
    while ( !__interruptMask.compare_exchange_weak( expected, tid ) ) {
        if ( expected == tid )
            return 1;
        expected = -1;
    }
    return 0;
}
void __divine_interrupt_unmask() noexcept {
    __interruptMask = -1;
}
