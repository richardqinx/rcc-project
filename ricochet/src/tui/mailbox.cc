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

#include "mailbox.hh"

#include <algorithm>
#include <cerrno>
#include <sys/eventfd.h>
#include <system_error>
#include <unistd.h>
#include <utility>

namespace ricochet::tui::detail {

Mailbox::Mailbox() : event_fd_( eventfd( 0, EFD_CLOEXEC | EFD_NONBLOCK ) )
{
  if ( event_fd_ == -1 ) {
    throw std::system_error( errno, std::generic_category(), "eventfd" );
  }
}

Mailbox::~Mailbox()
{ ::close( event_fd_ ); }

PostResult Mailbox::post( HostUpdate update )
{
  const std::lock_guard lock( mutex_ );
  // Closing rejects later producers while existing sender handles remain alive.
  if ( closed_ ) {
    return PostResult::Closed;
  }
  updates_.push_back( std::move( update ) );
  int result;
  do {
    result = eventfd_write( event_fd_, 1 );
  } while ( result == -1 && errno == EINTR );
  // EAGAIN means the counter is already readable, so the loop is already awake.
  if ( result == -1 && errno != EAGAIN ) {
    const int error = errno;
    updates_.pop_back();
    throw std::system_error( error, std::generic_category(), "eventfd_write" );
  }
  return PostResult::Queued;
}

std::vector<HostUpdate> Mailbox::take( std::size_t limit )
{
  const std::lock_guard lock( mutex_ );
  eventfd_t counter {};
  int result;
  do {
    result = eventfd_read( event_fd_, &counter );
  } while ( result == -1 && errno == EINTR );
  // An empty queue has no wake token. Other read errors are transport failures.
  if ( result == -1 && errno != EAGAIN ) {
    throw std::system_error( errno, std::generic_category(), "eventfd_read" );
  }
  const auto count = std::min( limit, updates_.size() );
  std::vector<HostUpdate> batch;
  batch.reserve( count );
  for ( std::size_t i = 0; i < count; ++i ) {
    batch.push_back( std::move( updates_.front() ) );
    updates_.pop_front();
  }
  return batch;
}

int Mailbox::descriptor() const noexcept
{ return event_fd_; }

void Mailbox::close() noexcept
{
  const std::lock_guard lock( mutex_ );
  closed_ = true;
  updates_.clear();
}

} // namespace ricochet::tui::detail
