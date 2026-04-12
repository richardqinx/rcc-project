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

#include <expected>
#include <string>
#include <string_view>
#include <unordered_map>

namespace ricochet::net {
/**
 * @brief Encapsulate HTTP response result
 */

struct HttpResponse
{
  int status_code { 0 };
  std::unordered_map<std::string, std::string> headers {};
  std::string body {};
};

class HttpClient
{
public:
  HttpClient() = default;
  ~HttpClient() = default;

  HttpClient( const HttpClient& ) = delete;
  HttpClient& operator=( const HttpClient& ) = delete;
  HttpClient( HttpClient&& ) = delete;
  HttpClient& operator=( HttpClient&& ) = delete;

  /**
   * @brief Initiates an HTTP GET request (only supports http:// in MVP stage)
   * @param url Target URL, for example "http://example.com"
   * @return Returns HttpResponse on success, returns an error message string on failure
   */
  std::expected<HttpResponse, std::string> fetch( std::string_view url,
                                                  std::string_view method = "GET",
                                                  std::string_view body = "" ) const;

private:
};

} // namespace ricochet::net