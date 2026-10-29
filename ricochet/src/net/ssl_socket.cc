/*
 * Ricochet - A minimalist CLI web browser for fast, distraction-free navigation.
 * Copyright (C) 2026  Richard Qin
 *
 * This program is free software: you can redistribute it and/or modify
 * it under the terms of the GNU General Public License as published by
 * the Free Software Foundation, either version 3 of the License, or
 * (at your option) any later version.
 *
 * This program is distributed in the hope that it will be useful,
 * but WITHOUT ANY WARRANTY; without even the implied warranty of
 * MERCHANTABILITY or FITNESS FOR A PARTICULAR PURPOSE.  See the
 * GNU General Public License for more details.
 *
 * You should have received a copy of the GNU General Public License
 * along with this program.  If not, see <https://www.gnu.org/licenses/>.
 */

#include "ricochet/net/ssl_socket.hh"
#include <iostream>
#include <openssl/err.h>
#include <openssl/ssl.h>
#include <stdexcept>

namespace ricochet::net {

// --- RAII Deleters ---

void SslCtxDeleter::operator()( SSL_CTX* ptr ) const
{
  if ( ptr != nullptr ) {
    SSL_CTX_free( ptr );
  }
}

void SslDeleter::operator()( SSL* ptr ) const
{
  if ( ptr != nullptr ) {
    SSL_free( ptr );
  }
}

// SSLSocket Implementation

SSLSocket::SSLSocket()
{
  const SSL_METHOD* method = TLS_client_method();
  ctx_.reset( SSL_CTX_new( method ) );
  if ( !ctx_ ) {
    throw std::runtime_error( "Failed to create SSL_CTX" );
  }

  if ( SSL_CTX_set_default_verify_paths( ctx_.get() ) != 1 ) {
    throw std::runtime_error( "Failed to load default CA certificates" );
  }
}

void SSLSocket::connect( const Address& address, const std::string& hostname )
{
  // TCP Connect
  underlying_socket_.connect( address );

  ssl_.reset( SSL_new( ctx_.get() ) );
  if ( !ssl_ ) {
    throw std::runtime_error( "Failed to create SSL object" );
  }

  // Setup SNI (Server Name Indication)
  if ( SSL_set_tlsext_host_name( ssl_.get(), hostname.c_str() ) != 1 ) {
    // Some old servers do not support SNI, here we can ignore the error or throw an exception, depending on the
    // requirements
    std::cerr << "Warning: Failed to set SNI" << '\n';
  }

  SSL_set_fd( ssl_.get(), underlying_socket_.fd_num() );

  if ( SSL_connect( ssl_.get() ) != 1 ) {
    throw std::runtime_error( "TLS Handshake failed!" );
  }
}

std::size_t SSLSocket::write( std::string_view data )
{
  if ( !ssl_ ) {
    return 0;
  }

  const int bytes_written = SSL_write( ssl_.get(), data.data(), static_cast<int>( data.size() ) );
  if ( bytes_written <= 0 ) {
    throw std::runtime_error( "SSL write failed" );
  }
  return static_cast<std::size_t>( bytes_written );
}

void SSLSocket::read( std::string& buffer )
{
  if ( !ssl_ ) {
    return;
  }

  std::array<char, 4096> buf {};
  // Read decrypted data from the encrypted channel
  const int bytes_read = SSL_read( ssl_.get(), buf.data(), static_cast<int>( buf.size() ) );

  if ( bytes_read > 0 ) {
    buffer.append( buf.data(), static_cast<std::size_t>( bytes_read ) );
  } else {
    // Read 0 or negative, representing the server closed the connection (EOF)
    eof_ = true;
  }
}

bool SSLSocket::eof() const
{
  return eof_;
}

} // namespace ricochet::net