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

#include "document_text.hh"

#include <algorithm>
#include <limits>
#include <type_traits>

namespace ricochet::tui::detail {

std::string control_display_text( const Control& control, const TextService& text )
{
  return std::visit(
    [&control, &text]( const auto& value ) -> std::string {
      using Value = std::decay_t<decltype( value )>;
      if constexpr ( std::is_same_v<Value, TextControl> ) {
        if ( value.kind == TextControlKind::Password ) {
          return std::string( text.analyze( value.value, true, false ).graphemes.size(), '*' );
        }
        return value.value;
      } else if constexpr ( std::is_same_v<Value, Checkbox> || std::is_same_v<Value, RadioButton>
                            || std::is_same_v<Value, Button> ) {
        return control.label;
      } else if constexpr ( std::is_same_v<Value, SelectControl> ) {
        std::string result;
        for ( const auto& option : value.options ) {
          if ( std::ranges::find( value.selected, option.id ) != value.selected.end() ) {
            if ( !result.empty() ) {
              result += ", ";
            }
            result += option.label;
          }
        }
        return result;
      } else {
        return value.description;
      }
    },
    control.content );
}

DocumentText::DocumentText( const Document& document, const TextService& text ) : service_( text )
{
  for ( const auto& control : document.data().controls ) {
    definitions_.emplace( control.id, &control );
  }
  append_blocks( document.data().blocks, 0 );
}

void DocumentText::append_text( const Text& text )
{
  const auto start = text_.size();
  text_ += text.utf8;
  blocks_.emplace( text.id, ByteRange { start, text_.size() } );
}

void DocumentText::append_control( ControlId id )
{
  const auto& control = *definitions_.at( id );
  const auto start = text_.size();
  // State is semantic content when selection crosses the object's opening boundary.
  if ( const auto* checkbox = std::get_if<Checkbox>( &control.content ) ) {
    text_ += checkbox->checked ? "checked " : "unchecked ";
  } else if ( const auto* radio = std::get_if<RadioButton>( &control.content ) ) {
    text_ += radio->checked ? "selected " : "unselected ";
  }
  const auto display = text_.size();
  text_ += control_display_text( control, service_ );
  controls_.emplace( id, ControlSpan { { start, text_.size() }, { display, text_.size() } } );
}

void DocumentText::append_inline( std::span<const Inline> content )
{
  for ( const auto& item : content ) {
    std::visit(
      [this]( const auto& value ) {
        using Value = std::decay_t<decltype( value )>;
        if constexpr ( std::is_same_v<Value, Text> ) {
          append_text( value );
        } else {
          append_control( value.control );
        }
      },
      item );
  }
}

void DocumentText::append_blocks( std::span<const Block> blocks, std::size_t depth )
{
  for ( std::size_t index = 0; index < blocks.size(); ++index ) {
    if ( index != 0 ) {
      text_ += "\n\n";
    }
    append_block( blocks[index], depth );
  }
}

void DocumentText::append_block( const Block& block, std::size_t depth )
{
  const auto start = text_.size();
  std::visit(
    [this, depth]( const auto& value ) {
      using Value = std::decay_t<decltype( value )>;
      if constexpr ( std::is_same_v<Value, Paragraph> || std::is_same_v<Value, Heading> ) {
        append_inline( value.content );
      } else if constexpr ( std::is_same_v<Value, Preformatted> ) {
        append_text( value.content );
      } else if constexpr ( std::is_same_v<Value, Alternative> ) {
        append_text( value.description );
      } else if constexpr ( std::is_same_v<Value, ControlPlaceholder> ) {
        append_control( value.control );
      } else if constexpr ( std::is_same_v<Value, List> ) {
        auto number = value.first_number;
        for ( std::size_t index = 0; index < value.items.size(); ++index ) {
          if ( index != 0 ) {
            text_ += '\n';
          }
          const auto& item = value.items[index];
          const auto prefix = text_.size();
          text_.append( depth * 2, ' ' );
          text_ += item.marker.value_or( value.kind == ListKind::Ordered ? std::to_string( number ) + "." : "-" );
          text_ += ' ';
          prefixes_.push_back( { prefix, text_.size() } );
          append_blocks( item.content, depth + 1 );
          // Match layout's saturating counter at the model's largest list number.
          if ( number < std::numeric_limits<std::int64_t>::max() ) {
            ++number;
          }
        }
      } else if constexpr ( std::is_same_v<Value, Table> ) {
        for ( std::size_t row = 0; row < value.rows.size(); ++row ) {
          if ( row != 0 ) {
            text_ += '\n';
          }
          const auto& cells = value.rows[row].cells;
          for ( std::size_t cell = 0; cell < cells.size(); ++cell ) {
            if ( cell != 0 ) {
              text_ += '\t';
            }
            append_blocks( cells[cell].content, depth );
          }
        }
      }
      // Separators carry positions but no copied decoration.
    },
    block.content );
  blocks_.emplace( block.id, ByteRange { start, text_.size() } );
}

std::size_t DocumentText::offset( const DocumentPoint& point ) const
{
  return std::visit(
    [this]( const auto& value ) -> std::size_t {
      using Value = std::decay_t<decltype( value )>;
      if constexpr ( std::is_same_v<Value, TextPosition> ) {
        return blocks_.at( value.block ).start + value.byte_offset;
      } else if constexpr ( std::is_same_v<Value, ControlTextPosition> ) {
        return controls_.at( value.control ).display.start + value.byte_offset;
      } else if constexpr ( std::is_same_v<Value, ObjectPosition> ) {
        const auto span = std::visit(
          [this]( const auto& id ) -> ByteRange {
            using Id = std::decay_t<decltype( id )>;
            if constexpr ( std::is_same_v<Id, BlockId> ) {
              return blocks_.at( id );
            } else {
              return controls_.at( id ).object;
            }
          },
          value.object );
        return value.side == BoundarySide::Before ? span.start : span.end;
      } else {
        return 0;
      }
    },
    point );
}

ByteRange DocumentText::offsets( const DocumentRange& range ) const
{ return { offset( range.start ), offset( range.end ) }; }

SelectionSnapshot DocumentText::select( const DocumentRange& anchor, const DocumentRange& active ) const
{
  const bool backward = offset( active.start ) < offset( anchor.start );
  // A zero-width boundary can coincide with a nonempty endpoint unit. Keep the
  // whole anchored unit even when the active endpoint comes back to that boundary.
  return { .range = { backward ? active.start : anchor.start,
                      offset( active.end ) > offset( anchor.end ) ? active.end : anchor.end },
           .direction = backward ? SelectionDirection::Backward : SelectionDirection::Forward,
           .anchor = anchor,
           .active = active };
}

std::string DocumentText::extract( const DocumentRange& range ) const
{
  auto bytes = offsets( range );
  if ( bytes.start == bytes.end ) {
    return {};
  }
  // Include a semantic marker only when selection includes the item's start.
  // Inner prefixes expand first, including a surrounding item with a nested list.
  for ( auto prefix = prefixes_.rbegin(); prefix != prefixes_.rend(); ++prefix ) {
    if ( prefix->end == bytes.start ) {
      bytes.start = prefix->start;
    }
  }
  auto result = text_.substr( bytes.start, bytes.end - bytes.start );
  // CRLF is one hard-break grapheme; copied hard breaks use one LF.
  for ( auto position = result.find( "\r\n" ); position != std::string::npos;
        position = result.find( "\r\n", position + 1 ) ) {
    result.erase( position, 1 );
  }
  return result;
}

} // namespace ricochet::tui::detail
