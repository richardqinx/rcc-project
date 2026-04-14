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

#include "ricochet/net/config.hh"
#include "ricochet/net/cookie_jar.hh"
#include "ricochet/net/http_utils.hh"
#include "ricochet/net/ssl_socket.hh"
#include "ricochet/net/sys/address.hh"
#include "ricochet/net/sys/socket.hh"
#include "ricochet/net/url_parser.hh"

#include <cstddef>
#include <format>
#include <string>
#include <string_view>

namespace ricochet::net {

namespace {

struct FetchResult
{
  std::string raw_response {};
  std::string error {};
};

FetchResult do_network_request( const std::string& host,
                                const std::string& service, // NOLINT(bugprone-easily-swappable-parameters)
                                const std::string& path,    // NOLINT(bugprone-easily-swappable-parameters)
                                bool is_https,
                                const std::string& method, // NOLINT(bugprone-easily-swappable-parameters)
                                const std::string& body,
                                const std::string& cookie_header ) // NOLINT(bugprone-easily-swappable-parameters)
{
  FetchResult res;
  try {
    const Address server_addr( host, service );
    std::string request = std::format( "{} {} HTTP/1.1\r\n"
                                       "Host: {}\r\n"
                                       "User-Agent: {}\r\n"
                                       "Accept: text/html,application/xhtml+xml,application/xml;q=0.9,*/*;q=0.8\r\n"
                                       "Accept-Language: en-US,en;q=0.9\r\n"
                                       "DNT: 1\r\n"
                                       "{}"
                                       "Connection: close\r\n",
                                       method,
                                       path,
                                       host,
                                       get_browser_config().user_agent,
                                       cookie_header );
    if ( !body.empty() ) {
      request += std::format( "Content-Length: {}\r\n"
                              "Content-Type: application/x-www-form-urlencoded\r\n",
                              body.length() );
    }
    request += "\r\n" + body;

    if ( is_https ) {
      SSLSocket ssl_socket;
      ssl_socket.connect( server_addr, host );
      for ( size_t bw = 0; bw < request.size(); ) {
        bw += ssl_socket.write( request.substr( bw ) );
      }
      while ( !ssl_socket.eof() ) {
        std::string buffer;
        ssl_socket.read( buffer );
        res.raw_response += buffer;
      }
    } else {
      TCPSocket tcp_socket;
      tcp_socket.connect( server_addr );
      for ( size_t bw = 0; bw < request.size(); ) {
        bw += tcp_socket.write( request.substr( bw ) );
      }
      while ( !tcp_socket.eof() ) {
        std::string buffer;
        tcp_socket.read( buffer );
        res.raw_response += buffer;
      }
    }
  } catch ( const std::exception& e ) {
    res.error = std::string( "Network Error: " ) + e.what();
  }
  return res;
}

void parse_response_headers( const std::string& raw_response, // NOLINT(bugprone-easily-swappable-parameters)
                             const std::string& host,         // NOLINT(bugprone-easily-swappable-parameters)
                             HttpResponse& response,
                             std::string& headers )
{
  const auto header_end = raw_response.find( "\r\n\r\n" );
  headers = ( header_end != std::string::npos ) ? raw_response.substr( 0, header_end ) : raw_response;
  update_cookies( headers, host );

  if ( header_end != std::string::npos ) {
    response.body = raw_response.substr( header_end + 4 );
    std::size_t ct_pos = headers.find( "Content-Type: " );
    if ( ct_pos == std::string::npos ) {
      ct_pos = headers.find( "content-type: " );
    }
    if ( ct_pos != std::string::npos ) {
      ct_pos += 14;
      const std::size_t end_pos = headers.find( '\r', ct_pos );
      if ( end_pos != std::string::npos ) {
        response.content_type = headers.substr( ct_pos, end_pos - ct_pos );
      }
    }
    if ( raw_response.size() >= 12 ) {
      try {
        response.status_code = std::stoi( raw_response.substr( 9, 3 ) );
      } catch ( ... ) {
        response.status_code = 0;
      }
    }
  } else {
    response.body = raw_response;
    response.status_code = 0;
  }
}

void sanitize_response_body( HttpResponse& response, const std::string& raw_response )
{
  if ( raw_response.contains( "Transfer-Encoding: chunked" ) ) {
    response.body = dechunk( response.body );
  }
  std::string ct_lower = response.content_type;
  for ( char& c : ct_lower ) {
    if ( c >= 'A' && c <= 'Z' ) {
      c = static_cast<char>( c + 32 );
    }
  }

  if ( ct_lower.starts_with( "text/" ) || ct_lower.contains( "xml" ) ) {
    response.body = remove_html_comments( response.body );
    response.body = remove_tag_blocks( response.body, "script" );
    response.body = remove_tag_blocks( response.body, "style" );
  }
}

} // namespace

std::expected<HttpResponse, std::string>
HttpClient::fetch(              // NOLINT(readability-convert-member-functions-to-static)
  std::string_view url,         // NOLINT(bugprone-easily-swappable-parameters)
  std::string_view method,      // NOLINT(bugprone-easily-swappable-parameters)
  std::string_view body ) const // NOLINT(bugprone-easily-swappable-parameters)
{
  std::string current_url( url );
  std::string current_method( method );
  std::string current_body( body );

  for ( int redirect_count = 0; redirect_count < 5; ++redirect_count ) {
    const auto parsed_url = parse_url( current_url );
    if ( !parsed_url.has_value() ) {
      return std::unexpected( parsed_url.error() );
    }

    const auto [host, path] = parsed_url.value();
    const bool is_https = current_url.starts_with( "https://" );
    const std::string service = is_https ? "https" : "http";

    const std::string cookie_header = build_cookie_header( host );

    const FetchResult net_res
      = do_network_request( host, service, path, is_https, current_method, current_body, cookie_header );
    if ( !net_res.error.empty() ) {
      return std::unexpected( net_res.error );
    }
    const std::string& raw_response = net_res.raw_response;

    HttpResponse response;
    std::string headers;
    parse_response_headers( raw_response, host, response, headers );

    if ( response.status_code >= 300 && response.status_code < 400 ) {
      const std::string next_url = extract_redirect_url( headers, is_https, host );
      if ( !next_url.empty() ) {
        current_url = next_url;
        if ( response.status_code == 301 || response.status_code == 302 || response.status_code == 303 ) {
          current_method = "GET";
          current_body.clear();
        }
        continue;
      }
    }

    sanitize_response_body( response, raw_response );
    return response;
  }

  return std::unexpected( "Too many redirects" );
}

} // namespace ricochet::net