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
#include "ricochet/net/ssl_socket.hh"

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
  std::string_view prefix = "http://";

  if ( url.starts_with( "https://" ) ) {
    prefix = "https://";
  } else if ( url.starts_with( "http://" ) ) {
    prefix = "http://";
  } else {
    return std::unexpected( "Only 'http://' or 'https://' URLs are supported." );
  }

  url.remove_prefix( prefix.length() );
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

std::string dechunk( std::string_view body ) // NOLINT(readability-convert-member-functions-to-static)
{
  std::string result;
  std::size_t pos = 0;
  while ( pos < body.size() ) {
    // find hex length end with \r\n
    auto line_end = body.find( "\r\n", pos );
    if ( line_end == std::string_view::npos ) {
      break;
    }

    // parse hex length
    const std::string len_str( body.substr( pos, line_end - pos ) );
    unsigned int chunk_size = 0;
    try {
      chunk_size = std::stoul( len_str, nullptr, 16 );
    } catch ( ... ) {
      break;
    }

    if ( chunk_size == 0 ) {
      break; // end of stream
    }

    // extract data block (skip \r\n)
    pos = line_end + 2;
    if ( pos + chunk_size > body.size() ) {
      break;
    }
    result.append( body.substr( pos, chunk_size ) );

    // skip data block's \r\n
    pos += chunk_size + 2;
  }
  return result;
}

std::string remove_html_comments( std::string html ) // NOLINT(readability-convert-member-functions-to-static)
{
  std::size_t start_pos = 0;
  while ( ( start_pos = html.find( "<!--", start_pos ) ) != std::string::npos ) {
    auto end_pos = html.find( "-->", start_pos );
    if ( end_pos != std::string::npos ) {
      html.erase( start_pos, end_pos - start_pos + 3 );
    } else {
      html.erase( start_pos );
      break;
    }
  }
  return html;
}

std::string remove_tag_blocks( std::string html, const std::string& tag_name ) // NOLINT
{
  const std::string open_tag = "<" + tag_name;
  const std::string close_tag = "</" + tag_name + ">";
  std::size_t start_pos = 0;

  while ( ( start_pos = html.find( open_tag, start_pos ) ) != std::string::npos ) {
    const std::size_t end_pos = html.find( close_tag, start_pos );
    if ( end_pos != std::string::npos ) {
      html.erase( start_pos, end_pos - start_pos + close_tag.length() );
    } else {
      html.erase( start_pos );
      break;
    }
  }
  return html;
}

} // namespace

std::expected<HttpResponse, std::string>
HttpClient::fetch( // NOLINT(readability-convert-member-functions-to-static)
  std::string_view url ) const
{
  auto parsed_url = parse_url( url );
  if ( !parsed_url.has_value() ) {
    return std::unexpected( parsed_url.error() );
  }

  auto [host, path] = parsed_url.value();
  const bool is_https = url.starts_with( "https://" );
  const std::string service = is_https ? "https" : "http";

  try {
    const Address server_addr( host, service );

    const std::string request = std::format( "GET {} HTTP/1.1\r\n"
                                             "Host: {}\r\n"
                                             "User-Agent: Mozilla/5.0 (Ricochet/0.1.0)\r\n"
                                             "Accept: text/html\r\n"
                                             "Accept-Encoding: identity\r\n"
                                             "Connection: close\r\n\r\n",
                                             path,
                                             host );

    std::string raw_response;
    if ( is_https ) {
      SSLSocket ssl_socket;
      ssl_socket.connect( server_addr, host );
      size_t bytes_written = 0;
      while ( bytes_written < request.size() ) {
        bytes_written += ssl_socket.write( request.substr( bytes_written ) );
      }
      while ( !ssl_socket.eof() ) {
        std::string buffer;
        ssl_socket.read( buffer );
        raw_response += buffer;
      }
    } else {
      TCPSocket tcp_socket;
      tcp_socket.connect( server_addr );
      size_t bytes_written = 0;
      while ( bytes_written < request.size() ) {
        bytes_written += tcp_socket.write( request.substr( bytes_written ) );
      }
      while ( !tcp_socket.eof() ) {
        std::string buffer;
        tcp_socket.read( buffer );
        raw_response += buffer;
      }
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

    if ( raw_response.contains( "Transfer-Encoding: chunked" ) ) {
      response.body = dechunk( response.body );
    }

    response.body = remove_html_comments( response.body );
    response.body = remove_tag_blocks( response.body, "script" );
    response.body = remove_tag_blocks( response.body, "style" );
    return response;
  } catch ( const std::exception& e ) {
    return std::unexpected( std::string( "Network Error: " ) + e.what() );
  }
}

} // namespace ricochet::net