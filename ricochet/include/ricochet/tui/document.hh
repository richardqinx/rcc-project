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

#include "ricochet/tui/position.hh"

#include <cstddef>
#include <cstdint>
#include <memory>
#include <optional>
#include <string>
#include <variant>
#include <vector>

namespace ricochet::tui {

struct TextStyle
{
  bool emphasis { false };
  bool strong { false };
  bool code { false };
  bool subdued { false };
  bool warning { false };
};

struct StyledRange
{
  ByteRange bytes {};
  TextStyle style {};
};

// Leaf IDs and structural block IDs share one document-local namespace.
// All text offsets address a Text leaf, not a concatenation of rendered lines.
struct Text
{
  BlockId id {};
  // LF is an explicit hard break; soft wrapping never changes this string.
  std::string utf8 {};
  std::vector<StyledRange> styles {};
};

struct ControlPlaceholder
{ ControlId control {}; };

using Inline = std::variant<Text, ControlPlaceholder>;

struct Paragraph
{ std::vector<Inline> content {}; };

struct Heading
{
  unsigned int level { 1 };
  std::vector<Inline> content {};
};

struct Preformatted
{
  Text content {};
  std::optional<std::string> language {};
};

struct Block;

struct ListItem
{
  std::optional<std::string> marker {};
  std::vector<Block> content {};
};

enum class ListKind : std::uint8_t
{
  Unordered,
  Ordered
};

struct List
{
  ListKind kind { ListKind::Unordered };
  std::int64_t first_number { 1 };
  std::vector<ListItem> items {};
};

struct TableCell
{
  // The producer resolves HTML spans to grid coordinates before delivery.
  std::size_t column { 0 };
  std::size_t column_span { 1 };
  std::size_t row_span { 1 };
  bool header { false };
  std::vector<Block> content {};
};

struct TableRow
{ std::vector<TableCell> cells {}; };

struct Table
{
  std::size_t columns { 0 };
  std::vector<TableRow> rows {};
};

struct Alternative
{ Text description {}; };

struct Separator
{};

struct Block
{
  // Structural identity supports empty blocks and per-block horizontal scroll.
  BlockId id {};
  std::variant<Paragraph, Heading, Preformatted, List, Table, ControlPlaceholder, Alternative, Separator>
    content {};
};

struct Link
{
  LinkId id {};
  std::string target {};
  std::string description {};
  // Multiple ranges can belong to one link, independently of text styling.
  std::vector<TextRange> ranges {};
};

enum class TextControlKind : std::uint8_t
{
  SingleLine,
  MultiLine,
  Password
};

struct TextControl
{
  TextControlKind kind { TextControlKind::SingleLine };
  std::string value {};
  std::string default_value {};
};

struct Checkbox
{
  bool checked { false };
  bool default_checked { false };
};

struct RadioButton
{
  RadioGroupId group {};
  bool checked { false };
  bool default_checked { false };
};

struct SelectOption
{
  OptionId id {};
  std::string label {};
  bool disabled { false };
};

enum class SelectKind : std::uint8_t
{
  Single,
  Multiple
};

struct SelectControl
{
  SelectKind kind { SelectKind::Single };
  std::vector<SelectOption> options {};
  std::vector<OptionId> selected {};
  std::vector<OptionId> default_selected {};
};

enum class ButtonKind : std::uint8_t
{
  Submit,
  Reset
};

struct Button
{ ButtonKind kind { ButtonKind::Submit }; };

struct UnsupportedControl
{ std::string description {}; };

struct Control
{
  ControlId id {};
  std::optional<FormId> form {};
  std::string label {};
  std::string hint {};
  std::optional<std::string> error {};
  bool disabled { false };
  bool read_only { false };
  std::variant<TextControl, Checkbox, RadioButton, SelectControl, Button, UnsupportedControl> content {};
};

struct Form
{
  FormId id {};
  std::string label {};
};

struct Anchor
{
  std::string name {};
  ContentPoint point {};
};

struct DocumentMetadata
{
  std::string title {};
  std::string display_url {};
  std::optional<std::string> language {};
};

// Producers build owned values. No DOM pointers, terminal codes or screen coordinates.
struct DocumentData
{
  DocumentVersion version {};
  DocumentMetadata metadata {};
  std::vector<Block> blocks {};
  std::vector<Link> links {};
  std::vector<Control> controls {};
  std::vector<Form> forms {};
  std::vector<Anchor> anchors {};
};

class Document
{
public:
  explicit Document( DocumentData data );

  const DocumentData& data() const noexcept;

private:
  std::shared_ptr<const DocumentData> data_;
};

} // namespace ricochet::tui
