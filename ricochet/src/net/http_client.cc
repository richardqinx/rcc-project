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

#include "ricochet/net/http_client.hh"

#include "ricochet/net/sys/address.hh"
#include "ricochet/net/sys/socket.hh"

#include <cstddef>
#include <format>
#include <string>

namespace ricochet::net {

namespace {

struct ParsedUrl
{
  std::string host {};
  std::string path {};
};

std::expected<ParsedUrl, std::string> parse_url( std::string_view url )
{
  const std::string_view http_prefix = "http://";

  if ( !url.starts_with( http_prefix ) ) {
    return std::unexpected( "Only 'http://' URLs are supported in MVP. (e.g., http://example.com)" );
  }

  url.remove_prefix( http_prefix.length() );
  auto slash_pos = url.find( '/' );

  ParsedUrl result;
  if ( slash_pos == std::string_view::npos ) {
    result.host = std::string( url );
    result.path = "/";
  } else {
    result.host = std::string( url.substr( 0, slash_pos ) );
    result.path = std::string( url.substr( slash_pos ) );
  }

  return result;
}
} // namespace

std::expected<HttpResponse, std::string> HttpClient::fetch( std::string_view url ) const // NOLINT(readability-convert-member-functions-to-static)
{
  auto parsed_url = parse_url( url );
  if ( !parsed_url.has_value() ) {
    return std::unexpected( parsed_url.error() );
  }

  auto [host, path] = parsed_url.value();

  try {
    const Address server_addr( host, "http" );

    TCPSocket socket;
    socket.connect( server_addr );

    const std::string request = std::format( "GET {} HTTP/1.1\r\n"
                                       "Host: {}\r\n"
                                       "User-Agent: Ricochet/0.1.0\r\n"
                                       "Connection: close\r\n\r\n",
                                       path,
                                       host );

    size_t bytes_written = 0;
    while ( bytes_written < request.size() ) {
      bytes_written += socket.write( request.substr( bytes_written ) );
    }

    std::string raw_response;
    while ( !socket.eof() ) {
      std::string buffer;
      socket.read( buffer );
      raw_response += buffer;
    }

    HttpResponse response;
    auto header_end = raw_response.find( "\r\n\r\n" );

    if ( header_end != std::string::npos ) {
      response.body = raw_response.substr( header_end + 4 );
      if ( raw_response.size() >= 12 ) {
        try {
          response.status_code = std::stoi( raw_response.substr( 9, 3 ) );
        } catch ( ... ) {
          response.status_code = 0;
        }
      }
    } else {
      response.body = raw_response;
    }

    return response;
  } catch ( const std::exception& e ) {
    return std::unexpected( std::string( "Network Error: " ) + e.what() );
  }
}

} // namespace ricochet::net