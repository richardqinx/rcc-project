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

#include "ricochet/tui/input.hh"
#include "ricochet/tui/messages.hh"

#include <chrono>
#include <cstdio>
#include <memory>
#include <optional>
#include <string>
#include <vector>

namespace ricochet::tui {

namespace detail {
class Mailbox;
}

struct TerminalOptions
{
  bool capture_mouse { true };
  // Explicit host opt-in: terminfo does not establish bracketed-paste support.
  bool bracketed_paste { false };
  std::chrono::milliseconds escape_delay { 25 };
};

struct TerminalCapabilities
{
  bool colors { false };
  bool mouse_buttons { false };
  bool mouse_motion { false };
  bool bracketed_paste { false };
};

enum class PostResult : std::uint8_t
{
  Queued,
  Closed
};

// A sender can outlive the UI session. Posting is thread-safe; it never calls curses.
class UpdateSender
{
public:
  PostResult post_update( HostUpdate update ) const;

private:
  friend class Session;
  explicit UpdateSender( std::shared_ptr<detail::Mailbox> mailbox );
  std::shared_ptr<detail::Mailbox> mailbox_;
};

struct StepResult
{
  bool running { true };
  // Incremental document updates and editing are subsequent milestones. Until then,
  // unsupported updates are returned intact rather than silently acknowledged.
  std::vector<HostUpdate> unhandled_updates {};
  std::optional<TerminalClosed> terminal_closed {};
  std::optional<std::string> error {};
};

class Session
{
public:
  explicit Session( TerminalOptions options = {} );
  // Streams must remain open until destruction. Construction, step, close and
  // destruction run on the same UI thread; only one terminal session may exist.
  Session( std::FILE& input, std::FILE& output, TerminalOptions options = {} );
  ~Session();

  Session( const Session& ) = delete;
  Session& operator=( const Session& ) = delete;
  Session( Session&& ) = delete;
  Session& operator=( Session&& ) = delete;

  UpdateSender sender() const;
  PostResult post_update( HostUpdate update );
  // max_wait bounds idle waiting. Decoding a partial terminal key sequence can
  // additionally wait for escape_delay; processing batches is bounded separately.
  StepResult step( std::chrono::milliseconds max_wait );
  std::vector<UserAction> take_actions();
  std::optional<ViewSnapshot> snapshot( TabId tab ) const;
  TerminalCapabilities capabilities() const;
  void close() noexcept;

private:
  class Impl;
  std::unique_ptr<Impl> impl_;
};

} // namespace ricochet::tui
