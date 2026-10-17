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

#include "layout.hh"
#include "document_text.hh"

#include <algorithm>
#include <limits>
#include <map>
#include <numeric>
#include <stdexcept>
#include <string>
#include <string_view>
#include <tuple>
#include <type_traits>
#include <utility>

namespace ricochet::tui::detail {
namespace {

constexpr int tab_columns = 8;
constexpr std::int64_t cell_gap = 3;
constexpr std::int64_t minimum_cell_columns = 4;

TextStyle overlay( TextStyle base, TextStyle extra )
{
  return { base.emphasis || extra.emphasis,
           base.strong || extra.strong,
           base.code || extra.code,
           base.subdued || extra.subdued,
           base.warning || extra.warning };
}

struct Atom
{
  Grapheme glyph {};
  DocumentRange source {};
  TextStyle style {};
  std::optional<HitTarget> target {};
  bool decoration { false };
};

struct TextSpan
{
  const Text* leaf;
  ByteRange bytes;
};

DocumentPoint text_point( std::span<const TextSpan> spans, std::size_t offset, bool end )
{
  for ( const auto& span : spans ) {
    // At an inline seam an end belongs to the preceding leaf, a start to the next.
    if ( offset < span.bytes.end || ( end && offset == span.bytes.end ) ) {
      return TextPosition { span.leaf->id, offset - span.bytes.start };
    }
  }
  const auto& last = spans.back();
  return TextPosition { last.leaf->id, last.leaf->utf8.size() };
}

LayoutGlyph anchor( DocumentPoint point, LayoutPoint location, std::int64_t width )
{ return { .position = location, .source = { point, point }, .clip_right = width }; }

void append_box( LayoutData& destination, LayoutData source, std::size_t row, std::int64_t column )
{
  destination.rows = std::max( destination.rows, row + source.rows );
  destination.extent = std::max( destination.extent, column + source.extent );
  for ( auto& glyph : source.glyphs ) {
    glyph.position.row += row;
    glyph.position.column += column;
    glyph.line_start += column;
    glyph.clip_left += column;
    glyph.clip_right += column;
    destination.glyphs.push_back( std::move( glyph ) );
  }
}

class LayoutBuilder
{
public:
  LayoutBuilder( const DocumentData& document, const TextService& text, std::span<const HorizontalScroll> scrolls )
    : document_( document ), text_( text ), scrolls_( scrolls )
  {
    for ( const auto& control : document.controls ) {
      controls_.emplace( control.id, &control );
    }
  }

  LayoutData blocks( std::span<const Block> content, std::int64_t width, TextStyle style = {} ) const
  {
    LayoutData result;
    std::size_t row = 0;
    for ( const auto& block : content ) {
      auto box = block_box( block, width, style );
      const auto height = box.rows;
      append_box( result, std::move( box ), row, 0 );
      row += height;
    }
    return result;
  }

private:
  void text_run( std::vector<Atom>& atoms,
                 std::span<const TextSpan> spans,
                 const std::string& utf8,
                 bool tabs,
                 TextStyle base ) const
  {
    const auto analysis = text_.analyze( utf8, true, tabs );
    for ( const auto& glyph : analysis.graphemes ) {
      Atom atom { .glyph = glyph,
                  .source
                  = { text_point( spans, glyph.bytes.start, false ), text_point( spans, glyph.bytes.end, true ) },
                  .style = base };
      for ( const auto& span : spans ) {
        // Only source portions intersecting this grapheme can contribute style/link.
        if ( span.bytes.start >= glyph.bytes.end || span.bytes.end <= glyph.bytes.start ) {
          continue;
        }
        const ByteRange local { std::max( span.bytes.start, glyph.bytes.start ) - span.bytes.start,
                                std::min( span.bytes.end, glyph.bytes.end ) - span.bytes.start };
        for ( const auto& styled : span.leaf->styles ) {
          if ( styled.bytes.start < local.end && local.start < styled.bytes.end ) {
            atom.style = overlay( atom.style, styled.style );
          }
        }
        for ( const auto& link : document_.links ) {
          for ( const auto& range : link.ranges ) {
            if ( range.block == span.leaf->id && range.bytes.start < local.end && local.start < range.bytes.end ) {
              const HitTarget target = link.id;
              // One grapheme cannot activate two links, including across leaf seams.
              if ( atom.target && atom.target != target ) {
                throw std::invalid_argument( "Overlapping links address the same grapheme" );
              }
              atom.target = target;
              atom.style.emphasis = true;
            }
          }
        }
      }
      atoms.push_back( std::move( atom ) );
    }
    // Empty text still has a real byte boundary; no fabricated display character.
    if ( utf8.empty() ) {
      const auto point = text_point( spans, 0, false );
      atoms.push_back( { .glyph = { .columns = 0 }, .source = { point, point }, .style = base } );
    }
  }

  void control_atoms( std::vector<Atom>& atoms, ControlId id, TextStyle base ) const
  {
    // A missing ID is a malformed document reference; map::at reports it directly.
    const auto& control = *controls_.at( id );
    const auto* input = std::get_if<TextControl>( &control.content );
    const bool multiline = input && input->kind == TextControlKind::MultiLine;
    const auto before = ObjectPosition { id, BoundarySide::Before };
    const auto after = ObjectPosition { id, BoundarySide::After };
    const auto style = overlay( base, { .subdued = control.disabled, .warning = control.error.has_value() } );
    std::wstring_view opening = L"[";
    std::wstring_view closing = L"]";
    if ( const auto* checkbox = std::get_if<Checkbox>( &control.content ) ) {
      opening = checkbox->checked ? L"[x] " : L"[ ] ";
      closing = L"";
    } else if ( const auto* radio = std::get_if<RadioButton>( &control.content ) ) {
      opening = radio->checked ? L"(*) " : L"( ) ";
      closing = L"";
    }
    for ( const auto character : opening ) {
      atoms.push_back( { .glyph = { .display = std::wstring( 1, character ) },
                         .source = { before, before },
                         .style = style,
                         .target = id } );
    }
    const auto display = control_display_text( control, text_ );
    const auto analysis = text_.analyze( display, multiline, false );
    for ( const auto& glyph : analysis.graphemes ) {
      atoms.push_back( { .glyph = glyph,
                         .source = { ControlTextPosition { id, {}, glyph.bytes.start },
                                     ControlTextPosition { id, {}, glyph.bytes.end } },
                         .style = style,
                         .target = id } );
    }
    if ( display.empty() ) {
      const auto point = ControlTextPosition { id, {}, 0 };
      atoms.push_back( { .glyph = { .display = L" " }, .source = { point, point }, .style = style, .target = id } );
    }
    for ( const auto character : closing ) {
      atoms.push_back( { .glyph = { .display = std::wstring( 1, character ), .break_after = true },
                         .source = { after, after },
                         .style = style,
                         .target = id } );
    }
    // Checkbox/radio controls have no closing bracket but still have an after boundary.
    atoms.push_back( { .glyph = { .columns = 0, .break_after = true },
                       .source = { after, after },
                       .style = style,
                       .target = id } );
  }

  std::vector<Atom> inline_atoms( std::span<const Inline> content, TextStyle style ) const
  {
    std::vector<Atom> result;
    std::vector<TextSpan> spans;
    std::string utf8;
    const auto flush = [&] {
      if ( !spans.empty() ) {
        text_run( result, spans, utf8, false, style );
        spans.clear();
        utf8.clear();
      }
    };
    for ( const auto& item : content ) {
      if ( const auto* text = std::get_if<Text>( &item ) ) {
        const auto start = utf8.size();
        utf8 += text->utf8;
        spans.push_back( { text, { start, utf8.size() } } );
      } else {
        flush();
        control_atoms( result, std::get<ControlPlaceholder>( item ).control, style );
      }
    }
    flush();
    return result;
  }

  LayoutData flow( std::span<const Atom> atoms, std::int64_t width, bool wrap ) const
  {
    LayoutData result;
    std::size_t start = 0;
    std::size_t row = 0;
    while ( start < atoms.size() ) {
      std::size_t end = start;
      std::size_t opportunity = start;
      std::int64_t column = 0;
      while ( end < atoms.size() && atoms[end].glyph.kind != TextUnitKind::LineBreak ) {
        const auto& glyph = atoms[end].glyph;
        const int cells = glyph.kind == TextUnitKind::Tab ? tab_columns - column % tab_columns : glyph.columns;
        if ( wrap && column + cells > width && end > start ) {
          break;
        }
        column += cells;
        ++end;
        if ( glyph.break_after ) {
          opportunity = end;
        }
      }
      // Prefer a Unicode line boundary; an overlong word breaks at a grapheme.
      if ( end < atoms.size() && atoms[end].glyph.kind != TextUnitKind::LineBreak && opportunity > start ) {
        end = opportunity;
      }
      column = 0;
      for ( auto index = start; index < end; ++index ) {
        const auto& atom = atoms[index];
        const bool tab = atom.glyph.kind == TextUnitKind::Tab;
        const int cells = tab ? tab_columns - column % tab_columns : atom.glyph.columns;
        result.glyphs.push_back( { .position = { row, column },
                                   .columns = cells,
                                   .text = atom.glyph.display,
                                   .style = atom.style,
                                   .source = atom.source,
                                   .target = atom.target,
                                   .clip_right = wrap ? width : std::numeric_limits<std::int64_t>::max(),
                                   .decoration = atom.decoration,
                                   .fill = tab } );
        column += cells;
      }
      result.extent = std::max( result.extent, column );
      if ( end < atoms.size() && atoms[end].glyph.kind == TextUnitKind::LineBreak ) {
        const auto& atom = atoms[end++];
        result.glyphs.push_back( { .position = { row, column },
                                   .source = atom.source,
                                   .target = atom.target,
                                   .clip_right = width,
                                   .line_break = true } );
        // The endpoint of a hard LF belongs to the next line, including a
        // trailing empty line; it must also be reachable by hit testing.
        auto boundary = anchor( atom.source.end, { row + 1, 0 }, width );
        boundary.target = atom.target;
        result.glyphs.push_back( std::move( boundary ) );
        result.rows = row + 2;
      } else {
        result.rows = row + 1;
      }
      start = end;
      ++row;
    }
    return result;
  }

  void horizontal( LayoutData& box, BlockId block, std::int64_t width ) const
  {
    const auto scroll = std::ranges::find( scrolls_, block, &HorizontalScroll::block );
    const auto requested = scroll == scrolls_.end() ? 0 : scroll->columns;
    const auto maximum = std::max<std::int64_t>( 0, box.extent - width );
    const auto shift = static_cast<std::int64_t>( std::min<std::uint64_t>( requested, maximum ) );
    for ( auto& glyph : box.glyphs ) {
      glyph.position.column -= shift;
      glyph.line_start -= shift;
      glyph.clip_left = std::max<std::int64_t>( 0, glyph.clip_left - shift );
      glyph.clip_right = std::min( width, glyph.clip_right - shift );
    }
  }

  LayoutData list_box( const List& list, std::int64_t width, TextStyle style ) const
  {
    LayoutData result;
    std::size_t row = 0;
    auto number = list.first_number;
    for ( const auto& item : list.items ) {
      const auto marker
        = item.marker.value_or( list.kind == ListKind::Ordered ? std::to_string( number ) + "." : "-" );
      const auto analysis = text_.analyze( marker, false, false );
      const auto marker_width
        = std::accumulate( analysis.graphemes.begin(),
                           analysis.graphemes.end(),
                           std::int64_t { 0 },
                           []( auto total, const auto& glyph ) { return total + glyph.columns; } );
      // Deep nesting must leave at least one body cell; clipped markers stay decoration.
      const auto indent = std::min( marker_width + 1, width - 1 );
      auto body = blocks( item.content, width - indent, style );
      const auto height = body.rows;
      append_box( result, std::move( body ), row, indent );
      std::int64_t column = 0;
      for ( const auto& glyph : analysis.graphemes ) {
        result.glyphs.push_back( { .position = { row, column },
                                   .columns = glyph.columns,
                                   .text = glyph.display,
                                   .style = style,
                                   .clip_right = indent,
                                   .decoration = true } );
        column += glyph.columns;
      }
      row += height;
      // Ordered counters can start at the model's largest int64 value.
      if ( number < std::numeric_limits<std::int64_t>::max() ) {
        ++number;
      }
    }
    return result;
  }

  LayoutData table_box( const Table& table, BlockId id, std::int64_t width, TextStyle style ) const
  {
    if ( table.columns == 0 ) {
      return {};
    }
    std::vector<std::int64_t> widths( table.columns, 1 );
    std::vector<std::int64_t> desired( table.columns, 1 );
    for ( std::size_t row = 0; row < table.rows.size(); ++row ) {
      for ( const auto& cell : table.rows[row].cells ) {
        // Spans index the supplied grid. Reject zero/out-of-grid spans before indexing.
        if ( cell.column >= table.columns || cell.column_span == 0 || cell.row_span == 0
             || cell.column_span > table.columns - cell.column || cell.row_span > table.rows.size() - row ) {
          throw std::invalid_argument( "Table cell span lies outside its resolved grid" );
        }
        const auto measure = blocks( cell.content, width, style );
        const auto span = static_cast<std::int64_t>( cell.column_span );
        const auto preferred = std::max<std::int64_t>( 1, ( measure.extent + span - 1 ) / span );
        for ( auto column = cell.column; column < cell.column + cell.column_span; ++column ) {
          desired[column] = std::max( desired[column], preferred );
          widths[column] = std::max( widths[column], std::min( preferred, minimum_cell_columns ) );
        }
      }
    }
    const auto gaps = cell_gap * static_cast<std::int64_t>( table.columns - 1 );
    auto remaining = width - gaps - std::accumulate( widths.begin(), widths.end(), std::int64_t { 0 } );
    // Grow content-bearing columns toward their preferred width, within the viewport.
    while ( remaining > 0 ) {
      bool grew = false;
      for ( std::size_t column = 0; column < widths.size() && remaining > 0; ++column ) {
        if ( widths[column] < desired[column] ) {
          ++widths[column];
          --remaining;
          grew = true;
        }
      }
      if ( !grew ) {
        break;
      }
    }
    struct CellBox
    {
      const TableCell* cell;
      std::size_t row;
      LayoutData box;
    };
    std::vector<CellBox> cells;
    std::vector<std::size_t> heights( table.rows.size(), 1 );
    for ( std::size_t row = 0; row < table.rows.size(); ++row ) {
      for ( const auto& cell : table.rows[row].cells ) {
        const auto first = widths.begin() + static_cast<std::ptrdiff_t>( cell.column );
        const auto last = first + static_cast<std::ptrdiff_t>( cell.column_span );
        const auto cell_width = std::accumulate( first, last, std::int64_t { 0 } )
                                + cell_gap * static_cast<std::int64_t>( cell.column_span - 1 );
        auto box = blocks( cell.content, cell_width, overlay( style, { .strong = cell.header } ) );
        const auto begin = heights.begin() + static_cast<std::ptrdiff_t>( row );
        const auto end = begin + static_cast<std::ptrdiff_t>( cell.row_span );
        const auto height = std::accumulate( begin, end, std::size_t { 0 } );
        if ( box.rows > height ) {
          heights[row + cell.row_span - 1] += box.rows - height;
        }
        cells.push_back( { &cell, row, std::move( box ) } );
      }
    }
    std::vector<std::size_t> row_offsets( heights.size() + 1, 0 );
    std::partial_sum( heights.begin(), heights.end(), row_offsets.begin() + 1 );
    std::vector<std::int64_t> column_offsets( widths.size(), 0 );
    for ( std::size_t column = 1; column < widths.size(); ++column ) {
      column_offsets[column] = column_offsets[column - 1] + widths[column - 1] + cell_gap;
    }
    LayoutData result;
    result.extent = std::accumulate( widths.begin(), widths.end(), gaps );
    result.rows = std::max<std::size_t>( 1, row_offsets.back() );
    for ( auto& cell : cells ) {
      const auto column = column_offsets[cell.cell->column];
      append_box( result, std::move( cell.box ), row_offsets[cell.row], column );
      if ( cell.cell->column != 0 ) {
        for ( auto row = row_offsets[cell.row]; row < row_offsets[cell.row + cell.cell->row_span]; ++row ) {
          result.glyphs.push_back( { .position = { row, column - 2 },
                                     .columns = 1,
                                     .text = L"|",
                                     .style = { .subdued = true },
                                     .clip_right = result.extent,
                                     .decoration = true } );
        }
      }
    }
    // A nested preformatted block scrolls inside its cell; its natural extent
    // must not enlarge the enclosing table's horizontal scroll range.
    result.extent = std::accumulate( widths.begin(), widths.end(), gaps );
    horizontal( result, id, width );
    return result;
  }

  LayoutData block_box( const Block& block, std::int64_t width, TextStyle style ) const
  {
    auto result = std::visit(
      [this, &block, width, style]( const auto& value ) -> LayoutData {
        using Value = std::decay_t<decltype( value )>;
        if constexpr ( std::is_same_v<Value, Paragraph> ) {
          return flow( inline_atoms( value.content, style ), width, true );
        } else if constexpr ( std::is_same_v<Value, Heading> ) {
          return flow( inline_atoms( value.content, overlay( style, { .strong = true } ) ), width, true );
        } else if constexpr ( std::is_same_v<Value, Preformatted> || std::is_same_v<Value, Alternative> ) {
          const Text& leaf = [&]() -> const Text& {
            if constexpr ( std::is_same_v<Value, Preformatted> ) {
              return value.content;
            } else {
              return value.description;
            }
          }();
          const TextSpan span { &leaf, { 0, leaf.utf8.size() } };
          std::vector<Atom> atoms;
          constexpr bool preformatted = std::is_same_v<Value, Preformatted>;
          text_run(
            atoms, std::span( &span, 1 ), leaf.utf8, preformatted, overlay( style, { .code = preformatted } ) );
          auto box = flow( atoms, width, !preformatted );
          if constexpr ( preformatted ) {
            horizontal( box, block.id, width );
          }
          return box;
        } else if constexpr ( std::is_same_v<Value, List> ) {
          return list_box( value, width, style );
        } else if constexpr ( std::is_same_v<Value, Table> ) {
          return table_box( value, block.id, width, style );
        } else if constexpr ( std::is_same_v<Value, ControlPlaceholder> ) {
          std::vector<Atom> atoms;
          control_atoms( atoms, value.control, style );
          return flow( atoms, width, true );
        } else {
          LayoutData box;
          box.extent = width;
          for ( std::int64_t column = 0; column < width; ++column ) {
            box.glyphs.push_back( { .position = { 0, column },
                                    .columns = 1,
                                    .text = L"-",
                                    .style = { .subdued = true },
                                    .clip_right = width,
                                    .decoration = true } );
          }
          return box;
        }
      },
      block.content );
    const bool has_text
      = std::ranges::any_of( result.glyphs, []( const auto& glyph ) { return !glyph.decoration; } );
    auto before = anchor( ObjectPosition { block.id, BoundarySide::Before }, {}, width );
    auto after = anchor( ObjectPosition { block.id, BoundarySide::After },
                         { result.rows - 1, std::min( result.extent, width ) },
                         width );
    before.boundary_only = has_text;
    after.boundary_only = has_text;
    result.glyphs.push_back( std::move( before ) );
    result.glyphs.push_back( std::move( after ) );
    return result;
  }

  const DocumentData& document_;
  const TextService& text_;
  std::span<const HorizontalScroll> scrolls_;
  std::map<ControlId, const Control*> controls_ {};
};

LayoutPoint end_point( const LayoutGlyph& glyph )
{
  // Object decorations and empty fields do not create a second text boundary.
  if ( glyph.source.start == glyph.source.end ) {
    return glyph.position;
  }
  if ( glyph.line_break ) {
    return { glyph.position.row + 1, glyph.line_start };
  }
  return { glyph.position.row, glyph.position.column + glyph.columns };
}

} // namespace

DocumentLayout::DocumentLayout( const Document& document,
                                const TextService& text,
                                int columns,
                                std::span<const HorizontalScroll> scrolls )
  : columns_( std::max( 1, columns ) )
  , data_( LayoutBuilder( document.data(), text, scrolls ).blocks( document.data().blocks, columns_ ) )
{
  // An empty document still has a location for its cursor/viewport anchor.
  if ( data_.glyphs.empty() ) {
    data_.glyphs.push_back( anchor( DocumentStart {}, {}, columns_ ) );
  }
}

int DocumentLayout::columns() const
{ return columns_; }
std::size_t DocumentLayout::rows() const
{ return data_.rows; }

std::optional<LayoutPoint> DocumentLayout::locate( const DisplayPosition& position ) const
{
  if ( std::holds_alternative<DocumentStart>( position.point ) ) {
    return LayoutPoint {};
  }
  std::optional<LayoutPoint> fallback;
  for ( const auto& glyph : data_.glyphs ) {
    if ( glyph.decoration ) {
      continue;
    }
    if ( position.point == glyph.source.start ) {
      if ( position.affinity == WrapAffinity::NextLine ) {
        return glyph.position;
      }
      fallback = glyph.position;
    }
    if ( position.point == glyph.source.end ) {
      if ( position.affinity == WrapAffinity::PreviousLine ) {
        return end_point( glyph );
      }
      fallback = end_point( glyph );
    }
  }
  return fallback;
}

std::optional<LayoutHit> DocumentLayout::hit( LayoutPoint point ) const
{
  const LayoutGlyph* nearest = nullptr;
  auto distance
    = std::tuple { std::numeric_limits<std::size_t>::max(), std::numeric_limits<std::int64_t>::max(), true };
  for ( const auto& glyph : data_.glyphs ) {
    // Decorations and partially clipped wide glyphs have no independent text hit.
    if ( glyph.decoration || glyph.boundary_only ) {
      continue;
    }
    const auto left = std::max( glyph.position.column, glyph.clip_left );
    const auto edge = std::min( glyph.position.column + glyph.columns, glyph.clip_right );
    // Tab blanks may be clipped individually; a wide glyph remains indivisible.
    if ( ( glyph.fill && left >= edge )
         || ( !glyph.fill
              && ( glyph.position.column < glyph.clip_left
                   || glyph.position.column + glyph.columns > glyph.clip_right ) ) ) {
      continue;
    }
    const auto row_distance
      = glyph.position.row > point.row ? glyph.position.row - point.row : point.row - glyph.position.row;
    const auto right = std::max( left, edge - 1 );
    const auto column_distance = std::max( { left - point.column, point.column - right, std::int64_t { 0 } } );
    const auto candidate = std::tuple { row_distance, column_distance, glyph.columns == 0 };
    if ( candidate < distance ) {
      nearest = &glyph;
      distance = candidate;
    }
  }
  if ( !nearest ) {
    return std::nullopt;
  }
  const bool after = !nearest->line_break && point.column >= nearest->position.column + nearest->columns;
  return LayoutHit { .position = { after ? nearest->source.end : nearest->source.start,
                                   after ? WrapAffinity::PreviousLine : WrapAffinity::NextLine },
                     .unit = nearest->source,
                     .target = nearest->target };
}

void DocumentLayout::append_frame( Frame& frame,
                                   std::size_t first_row,
                                   CellPoint origin,
                                   ScreenSize viewport ) const
{
  for ( const auto& glyph : data_.glyphs ) {
    if ( glyph.text.empty() || glyph.position.row < first_row
         || glyph.position.row - first_row >= static_cast<std::size_t>( std::max( 0, viewport.rows ) ) ) {
      continue;
    }
    const auto left = std::max<std::int64_t>( 0, glyph.clip_left );
    const auto right = std::min<std::int64_t>( viewport.columns, glyph.clip_right );
    const auto row = origin.row + static_cast<int>( glyph.position.row - first_row );
    if ( glyph.fill ) {
      // Expanded tabs are many blank cells but keep a single source range/hit unit.
      for ( auto column = std::max( left, glyph.position.column );
            column < std::min( right, glyph.position.column + glyph.columns );
            ++column ) {
        frame.glyphs.push_back(
          { .position = { row, origin.column + static_cast<int>( column ) }, .text = L" ", .style = glyph.style } );
      }
    } else if ( glyph.position.column >= left && glyph.position.column + glyph.columns <= right ) {
      frame.glyphs.push_back( { .position = { row, origin.column + static_cast<int>( glyph.position.column ) },
                                .width = glyph.columns,
                                .text = glyph.text,
                                .style = glyph.style } );
    }
  }
}

} // namespace ricochet::tui::detail
