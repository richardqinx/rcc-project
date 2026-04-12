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
#include <string_view>
#include <unordered_map>

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

  const auto sep_pos = url.find_first_of( "/?#" );

  ParsedUrl result;
  if ( sep_pos == std::string_view::npos ) {
    result.host = std::string( url );
    result.path = "/";
  } else {
    result.host = std::string( url.substr( 0, sep_pos ) );
    result.path = std::string( url.substr( sep_pos ) );
    if ( result.path[0] != '/' ) {
      result.path.insert( 0, "/" );
    }
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

std::string extract_redirect_url( const std::string& raw_response, bool is_https, const std::string& host )
{
  auto loc_pos = raw_response.find( "Location: " );
  if ( loc_pos == std::string::npos ) {
    loc_pos = raw_response.find( "location: " );
  }
  if ( loc_pos != std::string::npos ) {
    loc_pos += 10;
    auto end_pos = raw_response.find( '\r', loc_pos );
    if ( end_pos == std::string::npos ) {
      end_pos = raw_response.find( '\n', loc_pos );
    }
    if ( end_pos != std::string::npos ) {
      std::string next_url = raw_response.substr( loc_pos, end_pos - loc_pos );
      while ( !next_url.empty() && std::isspace( static_cast<unsigned char>( next_url.front() ) ) ) {
        next_url.erase( 0, 1 );
      }
      if ( next_url.starts_with( "/" ) ) {
        next_url = ( is_https ? "https://" : "http://" ) + host + next_url;
      }
      return next_url;
    }
  }
  return "";
}

std::unordered_map<std::string, std::string>& get_cookie_jar()
{
  static std::unordered_map<std::string, std::string> jar;
  return jar;
}

void update_cookies( const std::string& raw_response )
{
  std::size_t pos = 0;
  while ( ( pos = raw_response.find( "Set-Cookie: ", pos ) ) != std::string::npos ) {
    pos += 12;
    const std::size_t end_pos = raw_response.find( '\r', pos );
    if ( end_pos == std::string::npos ) {
      break;
    }

    const std::string cookie_line = raw_response.substr( pos, end_pos - pos );
    const std::size_t semi_pos = cookie_line.find( ';' );
    const std::string cookie_pair
      = ( semi_pos == std::string::npos ) ? cookie_line : cookie_line.substr( 0, semi_pos );

    const std::size_t eq_pos = cookie_pair.find( '=' );
    if ( eq_pos != std::string::npos ) {
      const std::string key = cookie_pair.substr( 0, eq_pos );
      const std::string val = cookie_pair.substr( eq_pos + 1 );
      get_cookie_jar()[key] = val;
    }
  }
}

std::string build_cookie_header()
{
  const auto& jar = get_cookie_jar();
  if ( jar.empty() ) {
    return "";
  }
  std::string header = "Cookie: ";
  bool first = true;
  for ( const auto& [k, v] : jar ) {
    if ( !first ) {
      header += "; ";
    }
    header += k;
    header += "=";
    header += v;
    first = false;
  }
  header += "\r\n";
  return header;
}

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
    std::string request
      = std::format( "{} {} HTTP/1.1\r\n"
                     "Host: {}\r\n"
                     "User-Agent: Mozilla/5.0 (Windows NT 10.0; Win64; x64) AppleWebKit/537.36 (KHTML, like Gecko) "
                     "Chrome/124.0.0.0 Safari/537.36\r\n"
                     "Accept: text/html,application/xhtml+xml,application/xml;q=0.9,*/*;q=0.8\r\n"
                     "Accept-Language: en-US,en;q=0.9\r\n"
                     "DNT: 1\r\n"
                     "{}"
                     "Connection: close\r\n",
                     method,
                     path,
                     host,
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

} // namespace

std::expected<HttpResponse, std::string> HttpClient::fetch( std::string_view url, // NOLINT
                                                            std::string_view method,
                                                            std::string_view body ) const
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

    const std::string cookie_header = build_cookie_header();

    const FetchResult net_res
      = do_network_request( host, service, path, is_https, current_method, current_body, cookie_header );
    if ( !net_res.error.empty() ) {
      return std::unexpected( net_res.error );
    }
    const std::string& raw_response = net_res.raw_response;

    HttpResponse response;
    const auto header_end = raw_response.find( "\r\n\r\n" );
    const std::string headers
      = ( header_end != std::string::npos ) ? raw_response.substr( 0, header_end ) : raw_response;
    update_cookies( headers );

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
    return response;
  }

  return std::unexpected( "Too many redirects" );
}

} // namespace ricochet::net