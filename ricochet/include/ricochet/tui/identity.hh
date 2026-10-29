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

#include <compare>
#include <cstdint>

namespace ricochet::tui {

template<class Tag>
struct Identity
{
  std::uint64_t value { 0 };

  auto operator<=>( const Identity& ) const = default;
};

using TabId = Identity<struct TabTag>;
using DocumentId = Identity<struct DocumentTag>;
using Revision = Identity<struct RevisionTag>;
using DraftRevision = Identity<struct DraftRevisionTag>;
using BlockId = Identity<struct BlockTag>;
using LinkId = Identity<struct LinkTag>;
using ControlId = Identity<struct ControlTag>;
using FormId = Identity<struct FormTag>;
using OptionId = Identity<struct OptionTag>;
using RadioGroupId = Identity<struct RadioGroupTag>;
using TaskId = Identity<struct TaskTag>;
using RequestId = Identity<struct RequestTag>;

struct DocumentVersion
{
  DocumentId document {};
  Revision revision {};

  bool operator==( const DocumentVersion& ) const = default;
};

struct DocumentScope
{
  TabId tab {};
  DocumentVersion version {};

  bool operator==( const DocumentScope& ) const = default;
};

} // namespace ricochet::tui
