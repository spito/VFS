// -*- C++ -*- (c) 2015 Jiří Weiser

#include <algorithm>
#include <signal.h>

#include "fs-utils.h"
#include "fs-inode.h"
#include "fs-storage.h"

#ifndef _FS_FILE_H_
#define _FS_FILE_H_

namespace divine {
namespace fs {

enum MemoryType{
    Shared = 0,
    Private = 1,
};

struct Link : DataItem {

    Link( utils::String target ) :
        _target( std::move( target ) )
    {
        if ( _target.size() > PATH_LIMIT )
            throw Error( ENAMETOOLONG );
    }

    size_t size() const override {
        return _target.size();
    }

    const utils::String &target() const {
        return _target;
    }

private:
    utils::String _target;
};

struct File : DataItem {

    virtual bool read( char *, size_t, size_t & ) = 0;
    virtual bool write( const char *, size_t, size_t & ) = 0;

    virtual void clear() = 0;
    virtual bool canRead() const = 0;
    virtual bool canWrite() const = 0;

};

struct RegularFile : File {

    RegularFile( const char *content, size_t size ) :
        _snapshot( bool( content ) ),
        _size( content ? size : 0 ),
        _roContent( content ),
        count(0)
    {}

    RegularFile() :
        _snapshot( false ),
        _size( 0 ),
        _roContent( nullptr ),
        count(0)
    {}

    RegularFile( const RegularFile &other ) = default;
    RegularFile( RegularFile &&other ) = default;
    RegularFile &operator=( RegularFile ) = delete;

    size_t size() const override {
        return _size;
    }

    bool canRead() const override {
        return true;
    }
    bool canWrite() const override {
        return true;
    }

    bool read( char *buffer, size_t offset, size_t &length ) override {
        if ( offset >= _size ) {
            length = 0;
            return true;
        }
        const char *source = _isSnapshot() ?
                          _roContent + offset :
                          _content.data() + offset;
        if ( offset + length > _size )
            length = _size - offset;
        std::copy( source, source + length, buffer );
        return true;
    }

    bool write( const char *buffer, size_t offset, size_t &length ) override {
        if ( count ) {
            throw Error( EBUSY );
        }
        if ( _isSnapshot() )
            _copyOnWrite();

        if ( _content.size() < offset + length )
            resize( offset + length );

        std::copy( buffer, buffer + length, _content.begin() + offset );
        return true;
    }

    void clear() override {
        if ( !_size )
            return;

        _snapshot = false;
        resize( 0 );
    }

    void resize( size_t length ) {
        _content.resize( length );
        _size = _content.size();
    }

    char* getPtr(size_t offset)  {
        return &_content[offset];
    }

    void unlockWrite() {
        --count;
    }

    void lockWrite() {
        ++count;
    }

private:

    bool _isSnapshot() const {
        return _snapshot;
    }

    void _copyOnWrite() {
        const char *roContent = _roContent;
        _content.resize( _size );

        std::copy( roContent, roContent + _size, _content.begin() );
        _snapshot = false;
    }

    bool _snapshot;
    size_t _size;
    const char *_roContent;
    utils::Vector< char > _content;
    int count;
};

struct WriteOnlyFile : File {

    size_t size() const override {
        return 0;
    }
    bool canRead() const override {
        return false;
    }
    bool canWrite() const override {
        return true;
    }
    bool read( char *, size_t, size_t & ) override {
        return false;
    }
    bool write( const char*, size_t, size_t & ) override {
        return true;
    }
    void clear() override {
    }
};

struct StandardInput : File {
    StandardInput() :
        _content( nullptr ),
        _size( 0 )
    {}

    StandardInput( const char *content, size_t size ) :
        _content( content ),
        _size( size )
    {}

    size_t size() const override {
        return _size;
    }

    bool canRead() const override {
        // simulate user drinking coffee
        if ( _size )
            return FS_CHOICE( 2 ) == FS_CHOICE_GOAL;
        return false;
    }
    bool canWrite() const override {
        return false;
    }
    bool read( char *buffer, size_t offset, size_t &length ) override {
        if ( offset >= _size ) {
            length = 0;
            return true;
        }
        const char *source = _content + offset;
        if ( offset + length > _size )
            length = _size - offset;
        std::copy( source, source + length, buffer );
        return true;
    }
    bool write( const char *, size_t, size_t & ) override {
        return false;
    }
    void clear() override {
    }

private:
    const char *_content;
    size_t _size;
};

struct Pipe : File {

    Pipe() :
        _stream( PIPE_SIZE_LIMIT ),
        _reader( false ),
        _writer( false )
    {}

    Pipe( bool r, bool w ) :
        _stream( PIPE_SIZE_LIMIT ),
        _reader( r ),
        _writer( w )
    {}

    size_t size() const override {
        return _stream.size();
    }

    bool canRead() const override {
        return size() > 0;
    }
    bool canWrite() const override {
        return size() < PIPE_SIZE_LIMIT;
    }

    bool read( char *buffer, size_t, size_t &length ) override {
        if ( length == 0 )
            return true;

        // progress or deadlock
        while ( ( length = _stream.pop( buffer, length ) ) == 0 )
            FS_MAKE_INTERRUPT();

        if ( length == 0 )
            return true;

        return true;
    }

    bool write( const char *buffer, size_t, size_t &length ) override {
        if ( !_reader ) {
            raise( SIGPIPE );
            throw Error( EPIPE );
        }

        // progress or deadlock
        while ( ( length = _stream.push( buffer, length ) ) == 0 )
            FS_MAKE_INTERRUPT();

        return true;
    }

    void clear() override {
        throw Error( EINVAL );
    }

    void releaseReader() {
        _reader = false;
    }

    bool reader() const {
        return _reader;
    }
    bool writer() const {
        return _writer;
    }

    void assignReader() {
        if ( _reader )
            __divine_problem( Other, "Pipe is opened for reading again." );
        _reader = true;
    }
    void assignWriter() {
        if ( _writer )
            __divine_problem( Other, "Pipe is opened for writing again." );
        _writer = true;
    }

private:
    storage::Stream _stream;
    bool _reader;
    bool _writer;
};

struct Socket : File {

    struct Address {

        Address() :
            _anonymous( true ),
            _valid( false )
        {}

        explicit Address( utils::String value, bool anonymous = false ) :
            _value( std::move( value ) ),
            _anonymous( anonymous ),
            _valid( true )
        {}
        Address( const Address & ) = default;
        Address( Address && ) = default;

        Address &operator=( Address other ) {
            swap( other );
            return *this;
        }

        const utils::String &value() const {
            return _value;
        }

        bool anonymous() const {
            return _anonymous;
        }

        bool valid() const {
            return _valid;
        }

        size_t size() const {
            return _value.size();
        }

        explicit operator bool() const {
            return _valid;
        }

        void swap( Address &other ) {
            using std::swap;

            swap( _value, other._value );
            swap( _anonymous, other._anonymous );
            swap( _valid, other._valid );
        }

        bool operator==( const Address &other ) const {
            return
                _valid == other._valid &&
                _anonymous == other._anonymous &&
                _value == other._value;
        }

        bool operator!=( const Address &other ) const {
            return !operator==( other );
        }

    private:
        utils::String _value;
        bool _anonymous;
        bool _valid;
    };


    void clear() override {
    }

    size_t size() const override {
        return 0;
    }

    bool read( char *buffer, size_t, size_t &length ) override {
        Address dummy;
        receive( buffer, length, flags::Message::NoFlags, dummy );
        return true;
    }
    bool write( const char *buffer, size_t, size_t &length ) override {
        send( buffer, length, flags::Message::NoFlags );
        return true;
    }

    const Address &address() const {
        return _address;
    }
    void address( Address addr ) {
        _address.swap( addr );
    }

    virtual Socket &peer() = 0;
    virtual Socket &peerHandle() = 0;

    virtual bool canReceive( size_t ) const = 0;
    virtual bool canConnect() const = 0;

    virtual void listen( int ) = 0;
    virtual Node accept() = 0;
    virtual void addBacklog( Node ) = 0;
    virtual void connected( Node, Node ) = 0;

    virtual void send( const char *, size_t &, Flags< flags::Message > ) = 0;
    virtual void sendTo( const char *, size_t &, Flags< flags::Message >, Node ) = 0;

    virtual void receive( char *, size_t &, Flags< flags::Message >, Address & ) = 0;

    virtual void fillBuffer( const char*, size_t & ) = 0;
    virtual void fillBuffer( const Address &, const char *, size_t & ) = 0;

    bool closed() const {
        return _closed;
    }
    void close() {
        _closed = true;
        abort();
    }
protected:
    virtual void abort() = 0;
private:
    Address _address;
    bool _closed = false;
};

inline void swap( Socket::Address &lhs, Socket::Address &rhs ) {
    lhs.swap( rhs );
}

struct ReliableSocket : Socket {

    ReliableSocket() :
            _peer( nullptr ),
            _passive( false ),
            _ready( false ),
            _limit( 0 )
    {}

    ReliableSocket( Node partner ) :
            _peerHandle( std::move( partner ) ),
            _peer( _peerHandle->data()->as< ReliableSocket >() ),
            _passive(false ),
            _ready( true ),
            _limit( 0 )
    {
        _peer->_peer = this;
        _peer->_ready = true;
    }

    virtual Socket &peer() override {
        if ( !_peer )
            throw Error( ENOTCONN );
        return *_peer;
    }

    virtual Socket &peerHandle() override {
        if ( !_peer || !_ready )
            throw Error( ENOTCONN );
        return *_peerHandle->data()->as<Socket>();
    }

    void abort() override {
        _peerHandle.reset();
        _peer = nullptr;
    }

    void setPeerHandle(Node handle) {
        _peerHandle = std::move(handle);
    }

    bool canConnect() const override {
        return _passive && !closed();
    }

    virtual void listen( int limit ) override {
        _passive = true;
        _limit = limit;
    }

    virtual Node accept() override {
        if ( !_passive )
            throw Error( EINVAL );

        // progress or deadlock
        while ( _backlog.empty() )
            FS_MAKE_INTERRUPT();

        Node client( std::move( _backlog.front() ) );
        _backlog.pop();
        return client;
    }

    virtual void addBacklog( Node incomming ) override {
        if ( _backlog.size() == _limit )
            throw Error( ECONNREFUSED );
        _backlog.push( std::move( incomming ) );
    }

protected:
    Node _peerHandle;
    ReliableSocket *_peer;
    utils::Queue< Node > _backlog;
    bool _passive;
    bool _ready;
    int _limit;
};


struct SocketStream : ReliableSocket {

    SocketStream() :
            _stream( 1024 )
    {}

    SocketStream( Node partner ) :
            ReliableSocket(std::move(partner)),
            _stream( 1024 )
    { }

    void connected( Node self, Node model ) override {
        if ( _peer )
            throw Error( EISCONN );

        SocketStream *m = model->data()->as< SocketStream >();

        if (!m)
            throw Error( EBADF );
        if ( !m->canConnect() )
            throw Error( ECONNREFUSED );

        m->addBacklog(std::move(self));
        _peerHandle = std::move(model);
    }

    bool canRead() const override {
        return !_stream.empty();
    }
    bool canWrite() const override {
        return _peer && _peer->canReceive( 1 );
    }
    bool canReceive( size_t amount ) const override {
        return _stream.size() + amount <= _stream.capacity();
    }

    void send( const char *buffer, size_t &length, Flags< flags::Message > fls ) override {
        if ( !_peer )
            throw Error( ENOTCONN );

        if ( !_peerHandle->mode().userWrite() )
            throw Error( EACCES );

        if ( fls.has( flags::Message::DontWait ) && !_peer->canReceive( length ) )
            throw Error( EAGAIN );

        _peer->fillBuffer( buffer, length );
    }

    void sendTo( const char *buffer, size_t &length, Flags< flags::Message > fls, Node ) override {
        send( buffer, length, fls );
    }

    void receive( char *buffer, size_t &length, Flags< flags::Message > fls, Address &address ) override {
        if ( !_peer && !closed() )
            throw Error( ENOTCONN );

        while ( _stream.empty()  )
            FS_MAKE_INTERRUPT();

        if ( fls.has( flags::Message::WaitAll ) ) {
            while ( _stream.size() < length )
                FS_MAKE_INTERRUPT();
        }

        if ( fls.has( flags::Message::Peek ) )
            length = _stream.peek( buffer, length );
        else
            length = _stream.pop( buffer, length );

        address = _peer->address();
    }

    void fillBuffer( const Address &, const char *, size_t & ) override {
        throw Error( EPROTOTYPE );
    }

    void fillBuffer( const char *buffer, size_t &length ) override {
        if ( closed() ) {
            abort();
            throw Error( ECONNRESET );
        }

        length = _stream.push( buffer, length );
    }

private:
    storage::Stream _stream;
};

struct SeqPacketSocket : ReliableSocket {

    SeqPacketSocket() {}

    SeqPacketSocket(Node partner) :
        ReliableSocket(std::move(partner))
    {}

    void connected( Node self, Node model ) override {
        if ( _peer )
            throw Error( EISCONN );

        SeqPacketSocket *m = model->data()->as< SeqPacketSocket >();

        if (!m)
            throw Error( EBADF );
        if ( !m->canConnect() )
            throw Error( ECONNREFUSED );

        m->addBacklog(std::move(self));
        _peerHandle = std::move(model);
    }

    bool canRead() const override {
        return !_packets.empty();
    }

    bool canWrite() const override {
        return _peer->canReceive(0);
    }

    bool canReceive( size_t ) const override {
        return !closed();
    }

    void sendTo( const char *buffer, size_t &length, Flags< flags::Message > fls, Node ) override {
        send( buffer, length, fls );
    }

    void send( const char *buffer, size_t &length, Flags< flags::Message > fls ) override {
        if ( !_peer )
            throw Error( ENOTCONN );

        if ( !_peerHandle->mode().userWrite() )
            throw Error( EACCES );

        if ( fls.has( flags::Message::DontWait ) && !_peer->canReceive( length ) )
            throw Error( EAGAIN );
        _peer->fillBuffer(buffer, length);

    }

    void fillBuffer( const Address &, const char *, size_t & ) override {
        throw Error( EPROTOTYPE );
    }

    void fillBuffer( const char *buffer, size_t &length ) override {
        if ( closed() ) {
            abort();
            throw Error( ECONNRESET );
        }

        _packets.emplace( buffer, length );
    }

    void receive( char *buffer, size_t &length, Flags< flags::Message > fls, Address &address ) override {

        if ( fls.has( flags::Message::DontWait ) && _packets.empty() )
            throw Error( EAGAIN );

        if ( !_peer && !closed() )
            throw Error( ENOTCONN );

        while ( _packets.empty() )
            FS_MAKE_INTERRUPT();

        length = _packets.front().read( buffer, length );
        if ( !fls.has( flags::Message::Peek ) )
            _packets.pop();

        address = _peer->address();
    }

private:
    struct Packet {

        Packet( const char *data, size_t length ) :
                _data( data, data + length )
        {}

        Packet( const Packet & ) = delete;
        Packet( Packet && ) = default;
        Packet &operator=( Packet other ) {
            swap( other );
            return *this;
        }

        size_t read( char *buffer, size_t max ) const {
            size_t result = std::min( max, _data.size() );
            std::copy( _data.begin(), _data.begin() + result, buffer );
            return result;
        }

        void swap( Packet &other ) {
            using std::swap;
            swap( _data, other._data );
        }

    private:
        utils::Vector< char > _data;
    };
    utils::Queue< Packet > _packets;

};

struct SocketDatagram : Socket {

    SocketDatagram()
    {}

    Socket &peer() override {
        if ( auto dr = _defaultRecipient.lock() ) {
            SocketDatagram *defRec = dr->data()->as< SocketDatagram >();
            if ( auto self = defRec->_defaultRecipient.lock() ) {
                if ( self->data() == this )
                    return *defRec;
            }
        }
        throw Error( ENOTCONN );
    }

    Socket &peerHandle() override {
        throw Error( EOPNOTSUPP );
    }


    bool canRead() const override {
        return !_packets.empty();
    }

    bool canWrite() const override {
        if ( auto dr = _defaultRecipient.lock() ) {
            return !dr->data()->as< Socket >()->canReceive( 0 );
        }
        return true;
    }

    bool canReceive( size_t ) const override {
        return !closed();
    }

    bool canConnect() const override {
        return false;
    }

    void listen( int ) override {
        throw Error( EOPNOTSUPP );
    }

    Node accept() override {
        throw Error( EOPNOTSUPP );
    }

    void addBacklog( Node ) override {
    }

    void connected( Node, Node defaultRecipient ) override {
        _defaultRecipient = defaultRecipient;
    }

    void send( const char *buffer, size_t &length, Flags< flags::Message > fls ) override {
        SocketDatagram::sendTo( buffer, length, fls, _defaultRecipient.lock() );
    }

    void sendTo( const char *buffer, size_t &length, Flags< flags::Message > fls, Node target ) override {
        if ( !target )
            throw Error( EDESTADDRREQ );

        if ( !target->mode().userWrite() )
            throw Error( EACCES );

        Socket *socket = target->data()->as< Socket >();
        socket->fillBuffer( address(), buffer, length );
    }

    void receive( char *buffer, size_t &length, Flags< flags::Message > fls, Address &address ) override {

        if ( fls.has( flags::Message::DontWait ) && _packets.empty() )
            throw Error( EAGAIN );

        while ( _packets.empty() )
            FS_MAKE_INTERRUPT();

        length = _packets.front().read( buffer, length );
        address = _packets.front().from();
        if ( !fls.has( flags::Message::Peek ) )
            _packets.pop();

    }

    void fillBuffer( const char *buffer, size_t &length ) override {
        throw Error( EPROTOTYPE );
    }
    void fillBuffer( const Address &sender, const char *buffer, size_t &length ) override {
        if ( closed() )
            throw Error( ECONNREFUSED );
        _packets.emplace( sender, buffer, length );
    }

    void abort() override {
    }


private:
    struct Packet {

        Packet( Address from, const char *data, size_t length ) :
            _from( std::move( from ) ),
            _data( data, data + length )
        {}

        Packet( const Packet & ) = delete;
        Packet( Packet && ) = default;
        Packet &operator=( Packet other ) {
            swap( other );
            return *this;
        }

        size_t read( char *buffer, size_t max ) const {
            size_t result = std::min( max, _data.size() );
            std::copy( _data.begin(), _data.begin() + result, buffer );
            return result;
        }

        const Address &from() const {
            return _from;
        }

        void swap( Packet &other ) {
            using std::swap;

            swap( _from, other._from );
            swap( _data, other._data );
        }

    private:
        Address _from;
        utils::Vector< char > _data;
    };

    utils::Queue< Packet > _packets;
    WeakNode _defaultRecipient;

};

struct Memory {

    Memory(Flags <flags::Mapping> flags, size_t length, size_t offset, File *target) : offset( offset ) {
        if ( flags.has(flags::Mapping::MapAnon )) {
            type = Private;
            memory = new( memory::nofail ) char[length]();
            if ( memory == nullptr ) {
                throw Error(ENOMEM);
            }
        } else {
            file = target->as<RegularFile>();
            if ( !file ) {
                return;
            }
            if ( flags.has( flags::Mapping::MapPrivate )) {
                type = Private;
                memory = new(memory::nofail) char[length];
                char *dst = reinterpret_cast< char * >( memory );
                target->read(dst, offset, length);
            } else {
                type = Shared;
                file->lockWrite();
            }
        }
    }

    Memory(const Memory &) = delete;
    Memory &operator=(const Memory &) = delete;

    void *getPtr() const {
        if ( type == Private ) {
            return memory;
        }
        return file ? file->getPtr( offset ) : nullptr;
    }

    ~Memory() {
        if ( type == Private ) {
            delete[] memory;
        }
        if ( type == Shared ) {
            file->unlockWrite();
        }
    }

private:
    MemoryType type;
    size_t offset;
    union{
        void *memory;
        RegularFile *file;
    };
};

} // namespace fs
} // namespace divine

#endif
