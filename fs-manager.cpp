// -*- C++ -*- (c) 2015 Jiří Weiser

#include "fs-manager.h"

#define REMEMBER_DIRECTORY( dirfd, name )                               \
    WeakNode savedDir = _currentDirectory;                              \
    auto d = utils::make_defer( [&]{ _currentDirectory = savedDir; } ); \
    if ( path::isRelative( name ) && dirfd != CURRENT_DIRECTORY )       \
        changeDirectory( dirfd );                                       \
    else                                                                \
        d.pass();

namespace divine {
namespace fs {

Manager::Manager( bool ) :
    _root{ std::allocate_shared< INode >(
        memory::AllocatorPure(), Mode::DIR | Mode::GRANTS
    ) },
    _currentDirectory{ _root },
    _standardIO{ {
        std::allocate_shared< INode >( memory::AllocatorPure(), Mode::FILE | Mode::RUSER ),
        std::allocate_shared< INode >( memory::AllocatorPure(), Mode::FILE | Mode::RUSER )
    } },
    _openFD{
        std::allocate_shared< FileDescriptor >( memory::AllocatorPure(), _standardIO[ 0 ], flags::Open::Read ),// stdin
        std::allocate_shared< FileDescriptor >( memory::AllocatorPure(), _standardIO[ 1 ], flags::Open::Write ),// stdout
        std::allocate_shared< FileDescriptor >( memory::AllocatorPure(), _standardIO[ 1 ], flags::Open::Write )// stderr
    },
    _umask{ Mode::WGROUP | Mode::WOTHER }
{
    _root->assign( new( memory::nofail ) Directory( _root ) );
    _standardIO[ 1 ]->assign( new( memory::nofail ) WriteOnlyFile() );
}


template< typename... Args >
Node Manager::createNodeAt( int dirfd, utils::String name, mode_t mode, Args &&... args ) {
    if ( name.empty() )
        throw Error( ENOENT );

    REMEMBER_DIRECTORY( dirfd, name );

    Node current;
    std::tie( current, name ) = _findDirectoryOfFile( name );

    _checkGrants( current, Mode::WUSER );
    Directory *dir = current->data()->as< Directory >();

    mode &= ~umask() & ( Mode::TMASK | Mode::GRANTS );
    if ( Mode( mode ).isDirectory() )
        mode |= Mode::GUID;

    Node node = std::allocate_shared< INode >( memory::AllocatorPure(), mode );

    switch( mode & Mode::TMASK ) {
    case Mode::SOCKET:
        node->assign( utils::constructIfPossible< SocketDatagram >( std::forward< Args >( args )... ) );
        break;
    case Mode::LINK:
        node->assign( utils::constructIfPossible< Link >( std::forward< Args >( args )... ) );
        break;
    case Mode::FILE:
        node->assign( utils::constructIfPossible< RegularFile >( std::forward< Args >( args )... ) );
        break;
    case Mode::DIR:
        node->assign( utils::constructIfPossible< Directory >( node, current ) );
        break;
    case Mode::FIFO:
        node->assign( utils::constructIfPossible< Pipe >( std::forward< Args >( args )... ) );
        break;
    case Mode::BLOCKD:
    case Mode::CHARD:
        throw Error( EPERM );
    default:
        throw Error( EINVAL );
    }
    if ( !node->data() )
        throw Error( EINVAL );

    dir->create( std::move( name ), node );

    return node;
}

void Manager::createHardLinkAt( int newdirfd, utils::String name, int olddirfd, const utils::String &target, Flags< flags::At > fl ) {
    if ( name.empty() || target.empty() )
        throw Error( ENOENT );

    if ( fl.has( flags::At::Invalid ) )
        throw Error( EINVAL );

    Node current;
    {
        REMEMBER_DIRECTORY( newdirfd, name );
        std::tie( current, name )= _findDirectoryOfFile( name );
    }

    _checkGrants( current, Mode::WUSER );
    Directory *dir = current->data()->as< Directory >();

    Node targetNode;
    {
        REMEMBER_DIRECTORY( olddirfd, target );
        targetNode = findDirectoryItem( target, fl.has( flags::At::SymFollow ) );
    }
    if ( !targetNode )
        throw Error( ENOENT );

    if ( targetNode->mode().isDirectory() )
        throw Error( EPERM );

    dir->create( std::move( name ), targetNode );
}

void Manager::createSymLinkAt( int dirfd, utils::String name, utils::String target ) {
    if ( name.empty() )
        throw Error( ENOENT );
    if ( target.size() > PATH_LIMIT )
        throw Error( ENAMETOOLONG );

    mode_t mode = 0;
    mode |= Mode::RWXUSER | Mode::RWXGROUP | Mode::RWXOTHER;
    mode |= Mode::LINK;

    createNodeAt( dirfd, std::move( name ), mode, std::move( target ) );
}

ssize_t Manager::readLinkAt( int dirfd, utils::String name, char *buf, size_t count ) {
    REMEMBER_DIRECTORY( dirfd, name );

    Node inode = findDirectoryItem( std::move( name ), false );
    if ( !inode )
        throw Error( ENOENT );
    if ( !inode->mode().isLink() )
        throw Error( EINVAL );

    Link *sl = inode->data()->as< Link >();
    const utils::String &target = sl->target();
    auto realLength = std::min( target.size(), count );
    std::copy( target.c_str(), target.c_str() + realLength, buf );
    return realLength;
}

void Manager::accessAt( int dirfd, utils::String name, Flags< flags::Access > mode, Flags< flags::At > fl ) {
    if ( name.empty() )
        throw Error( ENOENT );

    if ( mode.has( flags::Access::Invalid ) ||
        fl.has( flags::At::Invalid ) )
        throw Error( EINVAL );

    REMEMBER_DIRECTORY( dirfd, name );

    Node item = findDirectoryItem( name, !fl.has( flags::At::SymNofollow ) );
    if ( !item )
        throw Error( ENOENT );

    if ( ( mode.has( flags::Access::Read ) && !item->mode().userRead() ) ||
         ( mode.has( flags::Access::Write ) && !item->mode().userWrite() ) ||
         ( mode.has( flags::Access::Execute ) && !item->mode().userExecute() ) )
        throw Error( EACCES );
}

int Manager::openFileAt( int dirfd, utils::String name, Flags< flags::Open > fl, mode_t mode ) {
    REMEMBER_DIRECTORY( dirfd, name );

    Node file = findDirectoryItem( name, !fl.has( flags::Open::SymNofollow ) );

    if ( fl.has( flags::Open::Create ) ) {
        if ( file ) {
            if ( fl.has( flags::Open::Excl ) )
                throw Error( EEXIST );
        }
        else {
            file = createNodeAt( CURRENT_DIRECTORY, std::move( name ), mode | Mode::FILE );
        }
    }
    else if ( !file )
        throw Error( ENOENT );

    if ( file->mode().isSocket() || file->mode().isCharacterDevice() || file->mode().isBlockDevice() )
        throw Error( ENXIO );

    if ( fl.has( flags::Open::Read ) )
        _checkGrants( file, Mode::RUSER );
    if ( fl.has( flags::Open::Write ) ) {
        _checkGrants( file, Mode::WUSER );
        if ( file->mode().isDirectory() )
            throw Error( EISDIR );
        if ( fl.has( flags::Open::Truncate ) )
            if ( auto f = file->data()->as< File >() )
                f->clear();
    }

    if ( fl.has( flags::Open::NoAccess ) ) {
        fl.clear( flags::Open::Read );
        fl.clear( flags::Open::Write );
    }

    if ( file->mode().isFifo() )
        return _getFileDescriptor( std::allocate_shared< PipeDescriptor >( memory::AllocatorPure(), file, fl, true ) );
    return _getFileDescriptor( std::allocate_shared< FileDescriptor >( memory::AllocatorPure(), file, fl ) );
}

void Manager::closeFile( int fd ) {
    getFile( fd ).reset();
}

int Manager::duplicate( int oldfd, int lowEdge ) {
    return _getFileDescriptor( getFile( oldfd ), lowEdge );
}

int Manager::duplicate2( int oldfd, int newfd ) {
    if ( oldfd == newfd )
        return newfd;
    auto f = getFile( oldfd );
    if ( newfd < 0 || newfd > FILE_DESCRIPTOR_LIMIT )
        throw Error( EBADF );
    if ( newfd >= _openFD.size() )
        _openFD.resize( newfd + 1 );
    _openFD[ newfd ] = f;
    return newfd;
}

std::shared_ptr< FileDescriptor > &Manager::getFile( int fd ) {
    if ( fd >= 0 && fd < _openFD.size() && _openFD[ fd ] )
        return _openFD[ fd ];
    throw Error( EBADF );
}

std::shared_ptr< SocketDescriptor > Manager::getSocket( int sockfd ) {
    auto f = std::dynamic_pointer_cast< SocketDescriptor >( getFile( sockfd ) );
    if ( !f )
        throw Error( ENOTSOCK );
    return f;
}

std::pair< int, int > Manager::pipe() {
    mode_t mode = Mode::RWXUSER | Mode::FIFO;

    Node node = std::allocate_shared< INode >( memory::AllocatorPure(), mode );
    node->assign( new( memory::nofail ) Pipe() );
    return {
        _getFileDescriptor( std::allocate_shared< PipeDescriptor >( memory::AllocatorPure(), node, flags::Open::Read ) ),
        _getFileDescriptor( std::allocate_shared< PipeDescriptor >( memory::AllocatorPure(), node, flags::Open::Write ) )
    };
}

void Manager::removeFile( utils::String name ) {
    if ( name.empty() )
        throw Error( ENOENT );

    Node current;
    std::tie( current, name ) = _findDirectoryOfFile( name );

    _checkGrants( current, Mode::WUSER );
    Directory *dir = current->data()->as< Directory >();

    dir->remove( name );
}

void Manager::removeDirectory( utils::String name ) {
    if ( name.empty() )
        throw Error( ENOENT );

    Node current;
    std::tie( current, name ) = _findDirectoryOfFile( name );


    _checkGrants( current, Mode::WUSER );
    Directory *dir = current->data()->as< Directory >();

    dir->removeDirectory( name );
}

void Manager::removeAt( int dirfd, utils::String name, flags::At fl ) {
    REMEMBER_DIRECTORY( dirfd, name );

    switch( fl ) {
    case flags::At::NoFlags:
        removeFile( name );
        break;
    case flags::At::RemoveDir:
        removeDirectory( name );
        break;
    default:
        throw Error( EINVAL );
    }
}

void Manager::renameAt( int newdirfd, utils::String newpath, int olddirfd, utils::String oldpath ) {
    Node oldNode;
    Directory *oldNodeDirectory;
    Node newNode;
    Directory *newNodeDirectory;

    utils::String oldName;
    utils::String newName;

    {
        REMEMBER_DIRECTORY( olddirfd, oldpath );

        std::tie( oldNode, oldName ) = _findDirectoryOfFile( oldpath );
        _checkGrants( oldNode, Mode::WUSER );
        oldNodeDirectory = oldNode->data()->as< Directory >();
    }
    oldNode = oldNodeDirectory->find( oldName );
    if ( !oldNode )
        throw Error( ENOENT );

    {
        REMEMBER_DIRECTORY( newdirfd, newpath );

        newNode = _findDirectoryItem( newpath, false, [&]( Node n ) {
                if ( n == oldNode )
                    throw Error( EINVAL );
            } );
    }

    if ( !newNode ) {
        std::tie( newNode, newName ) = _findDirectoryOfFile( newpath );
        _checkGrants( newNode, Mode::WUSER );

        newNodeDirectory = newNode->data()->as< Directory >();
        newNodeDirectory->create( std::move( newName ), oldNode );
    }
    else {
        if ( oldNode->mode().isDirectory() ) {
            if ( !newNode->mode().isDirectory() )
                throw Error( ENOTDIR );
            if ( newNode->size() > 2 )
                throw Error( ENOTEMPTY );
        }
        else if ( newNode->mode().isDirectory() )
            throw Error( EISDIR );

        newNodeDirectory->replaceEntry( newName, oldNode );
    }
    oldNodeDirectory->forceRemove( oldName );
}

off_t Manager::lseek( int fd, off_t offset, Seek whence ) {
    auto f = getFile( fd );
    if ( f->inode()->mode().isFifo() )
        throw Error( ESPIPE );

    switch( whence ) {
    case Seek::Set:
        if ( offset < 0 )
            throw Error( EINVAL );
        f->offset( offset );
        break;
    case Seek::Current:
        if ( offset > 0 && std::numeric_limits< size_t >::max() - f->offset() < offset )
            throw Error( EOVERFLOW );
        if ( f->offset() < -offset )
            throw Error( EINVAL );
        f->offset( offset + f->offset() );
        break;
    case Seek::End:
        if ( offset > 0 && std::numeric_limits< size_t >::max() - f->size() < offset )
            throw Error( EOVERFLOW );
        if ( offset < 0 && f->size() < -offset )
            throw Error( EINVAL );
        f->offset( f->size() + offset );
        break;
    default:
        throw Error( EINVAL );
    }
    return f->offset();
}

void Manager::truncate( Node inode, off_t length ) {
    if ( !inode )
        throw Error( ENOENT );
    if ( length < 0 )
        throw Error( EINVAL );
    if ( inode->mode().isDirectory() )
        throw Error( EISDIR );
    if ( !inode->mode().isFile() )
        throw Error( EINVAL );

    _checkGrants( inode, Mode::WUSER );

    RegularFile *f = inode->data()->as< RegularFile >();
    f->resize( length );
}

void Manager::changeDirectory( utils::String pathname ) {
    Node item = findDirectoryItem( pathname );
    if ( !item )
        throw Error( ENOENT );
    if ( !item->mode().isDirectory() )
        throw Error( ENOTDIR );
    _checkGrants( item, Mode::XUSER );

    _currentDirectory = item;
}

void Manager::changeDirectory( int dirfd ) {
    Node item = getFile( dirfd )->inode();
    if ( !item )
        throw Error( ENOENT );
    if ( !item->mode().isDirectory() )
        throw Error( ENOTDIR );
    _checkGrants( item, Mode::XUSER );

    _currentDirectory = item;
}

void Manager::chmodAt( int dirfd, utils::String name, mode_t mode, Flags< flags::At > fl ) {
    if ( fl.has( flags::At::Invalid ) )
        throw Error( EINVAL );

    REMEMBER_DIRECTORY( dirfd, name );

    Node inode = findDirectoryItem( std::move( name ), !fl.has( flags::At::SymNofollow ) );
    _chmod( inode, mode );
}

void Manager::chmod( int fd, mode_t mode ) {
    _chmod( getFile( fd )->inode(), mode );
}

DirectoryDescriptor *Manager::openDirectory( int fd ) {
    Node inode = getFile( fd )->inode();
    if ( !inode->mode().isDirectory() )
        throw Error( ENOTDIR );

    _checkGrants( inode, Mode::RUSER | Mode::XUSER );
    _openDD.emplace_back( inode, fd );
    return &_openDD.back();
}
DirectoryDescriptor *Manager::getDirectory( void *descriptor ) {
    for ( auto i = _openDD.begin(); i != _openDD.end(); ++i ) {
        if ( &*i == descriptor ) {
            return &*i;
        }
    }
    throw Error( EBADF );
}
void Manager::closeDirectory( void *descriptor ) {
    for ( auto i = _openDD.begin(); i != _openDD.end(); ++i ) {
        if ( &*i == descriptor ) {
            closeFile( i->fd() );
            _openDD.erase( i );
            return;
        }
    }
    throw Error( EBADF );
}

int Manager::socket( SocketType type, Flags< flags::Open > fl ) {
    Socket *s = nullptr;
    switch ( type ) {
    case SocketType::Stream:
        s = new( memory::nofail ) SocketStream;
        break;
    case SocketType::Datagram:
        s = new( memory::nofail ) SocketDatagram;
        break;
    case SocketType::SeqPacket:
        s = new( memory::nofail ) SeqPacketSocket;
        break;
    default:
        throw Error( EPROTONOSUPPORT );
    }
    std::shared_ptr< SocketDescriptor > sd =
        std::allocate_shared< SocketDescriptor >(
            memory::AllocatorPure(),
            std::allocate_shared< INode >( memory::AllocatorPure(), Mode::GRANTS | Mode::SOCKET, s ),
            fl
        );

    return _getFileDescriptor( std::move( sd ) );
}

std::pair< int, int > Manager::socketpair( SocketType type, Flags< flags::Open > fl ) {
    if ( type != SocketType::Stream && type != SocketType::SeqPacket )
        throw Error( EOPNOTSUPP );
    Node client, server;
    if (type == SocketType::Stream) {
        SocketStream *cl = new( memory::nofail ) SocketStream;

        client = std::allocate_shared< INode >(
                memory::AllocatorPure(),
                Mode::GRANTS | Mode::SOCKET,
                cl );
        server = std::allocate_shared< INode >(
                memory::AllocatorPure(),
                Mode::GRANTS | Mode::SOCKET,
                new( memory::nofail ) SocketStream(client) );

        cl->setPeerHandle(server);
    }else {
        SeqPacketSocket *cl = new( memory::nofail ) SeqPacketSocket;

        client = std::allocate_shared< INode >(
                memory::AllocatorPure(),
                Mode::GRANTS | Mode::SOCKET,
                cl );
        server = std::allocate_shared< INode >(
                memory::AllocatorPure(),
                Mode::GRANTS | Mode::SOCKET,
                new( memory::nofail ) SeqPacketSocket(client) );

        cl->setPeerHandle(server);
    }

    return {
            _getFileDescriptor(
                    std::allocate_shared< SocketDescriptor >(
                            memory::AllocatorPure(),
                            server,
                            fl
                    )
            ),
            _getFileDescriptor(
                    std::allocate_shared< SocketDescriptor >(
                            memory::AllocatorPure(),
                            client,
                            fl
                    )
            )
    };
}

void Manager::bind( int sockfd, Socket::Address address ) {
    auto sd = getSocket( sockfd );

    if (!sd) throw Error ( EDESTADDRREQ );
    Node current;
    utils::String name = address.value();
    std::tie( current, name ) = _findDirectoryOfFile( name );

    Directory *dir = current->data()->as< Directory >();
    if ( dir->find( name ) )
        throw Error( EADDRINUSE );

    if ( sd->address() )
        throw Error( EINVAL );

    dir->create( std::move( name ), sd->inode() );
    sd->address( std::move( address ) );
}

void Manager::connect( int sockfd, const Socket::Address &address ) {
    auto sd = getSocket( sockfd );

    Node model = resolveAddress( address );

    sd->connected( model );
}

int Manager::accept( int sockfd, Socket::Address &address ) {
    Node partner = getSocket( sockfd )->accept();
    if(partner->data()->as<ReliableSocket>() == nullptr) {
        throw Error ( EOPNOTSUPP );
    }
    address = partner->data()->as< Socket >()->address();
    Socket *socket;
    if(partner->data()->as<SocketStream>() != nullptr) {
        socket = new( memory::nofail ) SocketStream(std::move(partner));
    }else {
        socket = new( memory::nofail ) SeqPacketSocket(std::move(partner));
    }
    return _getFileDescriptor(
        std::allocate_shared< SocketDescriptor >(
            memory::AllocatorPure(),
           std::allocate_shared<INode>(memory::AllocatorPure(),Mode::GRANTS | Mode::SOCKET, socket),
            flags::Open::NoFlags
        )
    );
}

Node Manager::resolveAddress( const Socket::Address &address ) {
    Node item = findDirectoryItem( address.value() );

    if ( !item )
        throw Error( ENOENT );
    if ( !item->mode().isSocket() )
        throw Error( ECONNREFUSED );
    _checkGrants( item, Mode::WUSER );
    return item;
}

Node Manager::findDirectoryItem( utils::String name, bool followSymLinks ) {
    return _findDirectoryItem( std::move( name ), followSymLinks, []( Node ){} );
}
template< typename I >
Node Manager::_findDirectoryItem( utils::String name, bool followSymLinks, I itemChecker ) {
    if ( name.size() > PATH_LIMIT )
        throw Error( ENAMETOOLONG );
    name = path::normalize( name );
    Node current = _root;
    if ( path::isRelative( name ) )
        current = currentDirectory();

    Node item = current;
    utils::Queue< utils::String > q( path::splitPath< utils::Deque< utils::String > >( name ) );
    utils::Set< Link * > loopDetector;
    while ( !q.empty() ) {
        if ( !current->mode().isDirectory() )
            throw Error( ENOTDIR );

        _checkGrants( current, Mode::XUSER );

        Directory *dir = current->data()->as< Directory >();

        {
            auto d = utils::make_defer( [&]{ q.pop(); } );
            const auto &subFolder = q.front();
            if ( subFolder.empty() )
                continue;
            if ( subFolder.size() > FILE_NAME_LIMIT )
                throw Error( ENAMETOOLONG );
            item = dir->find( subFolder );
        }

        if ( !item ) {
            if ( q.empty() )
                return nullptr;
            throw Error( ENOENT );
        }

        itemChecker( item );

        if ( item->mode().isDirectory() )
            current = item;
        else if ( item->mode().isLink() && ( followSymLinks || !q.empty() ) ) {
            Link *sl = item->data()->as< Link >();

            if ( !loopDetector.insert( sl ).second )
                throw Error( ELOOP );

            utils::Queue< utils::String > _q( path::splitPath< utils::Deque< utils::String > >( sl->target() ) );
            while ( !q.empty() ) {
                _q.emplace( std::move( q.front() ) );
                q.pop();
            }
            q.swap( _q );
            if ( path::isAbsolute( sl->target() ) ) {
                current = _root;
                item = _root;
            }
            continue;
        }
        else {
            if ( q.empty() )
                break;
            throw Error( ENOTDIR );
        }
    }
    return item;
}

std::pair< Node, utils::String > Manager::_findDirectoryOfFile( utils::String name ) {
    name = path::normalize( name );

    if ( name.size() > PATH_LIMIT )
        throw Error( ENAMETOOLONG );

    utils::String pathname;
    std::tie( pathname, name ) = path::splitFileName( name );
    Node item = findDirectoryItem( pathname );

    if ( !item )
        throw Error( ENOENT );

    if ( !item->mode().isDirectory() )
        throw Error( ENOTDIR );
    _checkGrants( item, Mode::XUSER );
    return { item, name };
}

int Manager::_getFileDescriptor( std::shared_ptr< FileDescriptor > f, int lowEdge ) {
    int i = 0;

    if ( lowEdge < 0 || lowEdge >= FILE_DESCRIPTOR_LIMIT )
        throw Error( EINVAL );

    if ( lowEdge >= _openFD.size() )
        _openFD.resize( lowEdge + 1 );

    for ( auto &fd : utils::withOffset( _openFD, lowEdge ) ) {
        if ( !fd ) {
            fd = f;
            return i;
        }
        ++i;
    }
    if ( _openFD.size() >= FILE_DESCRIPTOR_LIMIT )
        throw Error( ENFILE );

   _openFD.push_back( f );
    return i;
}

void Manager::_insertSnapshotItem( const SnapshotFS &item ) {

    switch( item.type ) {
    case Type::File:
        createNodeAt( CURRENT_DIRECTORY, item.name, item.mode, item.content, item.length );
        break;
    case Type::Directory:
    case Type::Pipe:
    case Type::Socket:
        createNodeAt( CURRENT_DIRECTORY, item.name, item.mode );
        break;
    case Type::SymLink:
        createNodeAt( CURRENT_DIRECTORY, item.name, item.mode, item.content );
        break;
    default:
        break;
    }
}

void Manager::_checkGrants( Node inode, mode_t grant ) const {
    if ( ( inode->mode() & grant ) != grant )
        throw Error( EACCES );
}

void Manager::_chmod( Node inode, mode_t mode ) {
    inode->mode() =
        ( inode->mode() & ~Mode::CHMOD ) |
        ( mode & Mode::CHMOD );
}

void * Manager::mmap(int fd, off_t length, off_t offset, Flags<flags::Mapping> flags) {
    if ( length <= 0 )
        throw Error ( EINVAL );
    auto file = vfs.instance().getFile( fd )->inode()->data()->as< File >();
    if ( !file ) {
        throw Error( EBADF );
    }
    std::unique_ptr< Memory > ptr(new (memory::nofail) Memory(flags, length, offset, file));
    if (!ptr) {
        throw Error ( ENOMEM );
    }
    void* memory = ptr->getPtr();
    if ( !memory ){
        return nullptr;
    }
    _mappedMemory.push_back( std::move( ptr ));
    return memory;
}


void Manager::munmap(void *directory) {
    for ( auto i =_mappedMemory.begin(); i != _mappedMemory.end(); ++i ) {
        if ( i->get()->getPtr() == directory ) {
            _mappedMemory.erase( i );
            return;
        }
    }
    throw Error ( EBADF );
}

} // namespace fs
} // namespace divine
