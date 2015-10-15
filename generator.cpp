#include <fstream>
#include <iostream>
#include <brick-fs.h>
#include <cctype>
#include <algorithm>
#include <iterator>

using uchar = unsigned char;
std::string encode( uchar c ) {
    std::string result = "\\x";
    
    if ( ( (c >> 4) & 15 ) < 10 )
        result += ( (c >> 4) & 15 ) + '0';
    else
        result += ( (c >> 4) & 15 ) - 10 + 'a';

    if ( ( c & 15 ) < 10 )
        result += ( c & 15 ) + '0';
    else
        result += ( c & 15 ) - 10 + 'a';

    return result;
}

std::string stringify( const std::string &str ) {
    std::string result;
    for ( char c : str ) {
        result += encode( c );
    }
    return result;
}

size_t stringify( std::ifstream &in, std::ofstream &out ) {
    size_t length = 0;
    std::for_each( std::istream_iterator< char >( in ), std::istream_iterator< char >(), [&]( char c ) {
        out << encode( c );
        ++length;
    } );
    return length;
}

const char *resolveType( unsigned mode ) {
    if ( ( mode & S_IFLNK ) == S_IFLNK )
        return "SymLink";
    if ( ( mode & S_IFREG ) == S_IFREG )
        return "File";
    if ( ( mode & S_IFIFO ) == S_IFIFO )
        return "Pipe";
    return nullptr;
}

int main( int argc, char **argv ) {
    if ( argc > 3 ) {
        std::cerr << "invalid number of arguments" << std::endl;
        return -1;
    }

    std::ofstream file( "snapshot.cpp" );

    file << "#include \"fs-manager.h\"\n"
         << "namespace divine{ namespace fs {\n"
         << "VFS vfs{\n";

    if ( argc == 3 ) {
        std::ifstream in( argv[ 2 ] );
        file << "\"";
        size_t length = stringify( in, file );
        file << "\", " << length << ",{\n";
    }
    if ( argc >= 2 ) {
        brick::fs::traverseDirectoryTree( argv[ 1 ],
            [&]( std::string path ) {
                auto shrinked = brick::fs::distinctPaths( argv[ 1 ], path );
                if ( !shrinked.empty() ) {
                    auto st = brick::fs::stat( path );
                    file << "{\"" << stringify( shrinked ) << "\", Type::Directory, " << st->st_mode << ", nullptr, 0 },\n";
                }
                return true;
            },
            []( std::string ){},
            [&]( std::string path ) {
                auto st = brick::fs::lstat( path );
                const char *type = resolveType( st->st_mode );
                file << "{\"" << stringify( brick::fs::distinctPaths( argv[ 1 ], path ) ) << "\", Type::" << type << ", "<< st->st_mode << ", ";

                if ( std::string( "File" ) == type ) {
                    file << "\"";
                    std::ifstream input( path, std::ios::binary );
                    input >> std::noskipws;
                    size_t length = stringify( input, file );

                    file << "\", " << length;
                }
                else if ( std::string( "Pipe" ) == type ) {
                    file << "\"\", 0";
                }
                else if ( std::string( "SymLink" ) == type ) {
                    std::string link( st->st_size, '-' );
                    readlink( path.c_str(), &link.front(), st->st_size );
                    file << "\"" << stringify( link ) << "\", " << st->st_size;
                }
                else
                    file << "nullptr, 0";
                file << "},\n";
            }
        );
    }

    file << "{ nullptr, Type::Nothing, 0, nullptr, 0 }";
    if ( argc == 3 )
        file << "}";
    file <<"};}}\n" << std::endl;

    return 0;
}
