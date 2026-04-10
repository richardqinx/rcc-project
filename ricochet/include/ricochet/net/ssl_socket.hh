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

#pragma once

#include "sys/address.hh"
#include "sys/socket.hh"
#include <memory>
#include <string>
#include <string_view>

extern "C" {
using SSL_CTX = struct ssl_ctx_st;
using SSL = struct ssl_st;
}

namespace ricochet::net {

struct SslCtxDeleter
{
  void operator()( SSL_CTX* ptr ) const;
};
struct SslDeleter
{
  void operator()( SSL* ptr ) const;
};

using UniqueSslCtx = std::unique_ptr<SSL_CTX, SslCtxDeleter>;
using UniqueSsl = std::unique_ptr<SSL, SslDeleter>;

/**
 * @brief Socket wrapper that supports TLS/SSL encrypted handshake
 */
class SSLSocket
{
public:
  SSLSocket();
  ~SSLSocket() = default;

  SSLSocket( const SSLSocket& ) = delete;
  SSLSocket& operator=( const SSLSocket& ) = delete;
  SSLSocket( SSLSocket&& ) = delete;
  SSLSocket& operator=( SSLSocket&& ) = delete;

  /**
   * @brief Connect to a remote host using TLS/SSL
   * @param host The hostname to connect to
   * @param port The port to connect to
   * @return True if the connection was successful, false otherwise
   */
  void connect( const Address& address, const std::string& hostname );

  /**
   * @brief Send data over the TLS/SSL connection
   * @param data The data to send
   * @return The number of bytes sent, or -1 if an error occurred
   */
  std::size_t write( std::string_view data );

  /**
   * @brief Receive data from the TLS/SSL connection
   * @param buffer The buffer to store the received data
   */
  void read( std::string& buffer );

  /**
   * @brief Check if the TLS/SSL connection is closed
   * @return True if the connection is closed, false otherwise
   */
  bool eof() const;

private:
  TCPSocket underlying_socket_ {}; // TCP Socket
  UniqueSslCtx ctx_ { nullptr };   // OpenSSL global context
  UniqueSsl ssl_ { nullptr };      // Current connection's SSL state
  bool eof_ { false };             // EOF flag
};
} // namespace ricochet::net