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

#include "ricochet/tui/session.hh"

#include "document_view.hh"
#include "editor.hh"
#include "mailbox.hh"
#include "screen.hh"

#include <algorithm>
#include <chrono>
#include <exception>
#include <map>
#include <string_view>
#include <utility>

namespace ricochet::tui {

namespace {

using Clock = std::chrono::steady_clock;

struct KeyPrefix
{
  char32_t character;
  Clock::time_point deadline;
};

struct TabState
{
  TabInfo info {};
  std::optional<RequestId> navigation {};
  std::optional<detail::DocumentView> view {};
  std::optional<ActionFailed> failure {};
  std::optional<RequestId> copy_request {};
  std::string copy_feedback {};
};

enum class AddressEntry : std::uint8_t
{
  Empty,
  Current,
  Selected
};

int address_label_columns( ScreenSize dimensions )
{ return std::min( 5, std::max( 0, dimensions.columns - 1 ) ); }

struct AddressInput
{
  TabId tab;
  std::optional<ViewSnapshot> restore;
  detail::TextEditor editor;
  bool dragging { false };
  std::wstring notice {};

  AddressInput( TabId source,
                std::optional<ViewSnapshot> snapshot,
                std::string value,
                const detail::TextService& text,
                ScreenSize dimensions )
    : tab( source )
    , restore( std::move( snapshot ) )
    , editor( std::move( value ), {}, text, { 1, dimensions.columns - address_label_columns( dimensions ) } )
  {}
};

// Only fixed ASCII chrome uses this helper. Document text goes through the
// Unicode text service and document layout.
void append_chrome( detail::Frame& frame, CellPoint start, std::wstring_view text, TextStyle style = {} )
{
  int column = start.column;
  for ( const wchar_t character : text ) {
    frame.glyphs.push_back(
      { .position = { start.row, column++ }, .text = std::wstring( 1, character ), .style = style } );
  }
}

} // namespace

class Session::Impl
{
public:
  Impl( std::FILE& input, std::FILE& output, TerminalOptions options )
    : mailbox_( std::make_shared<detail::Mailbox>() )
    , screen_( std::make_unique<detail::Screen>( input, output, options ) )
    , capabilities_( screen_->capabilities() )
  {}

  ~Impl() { close(); }

  void close() noexcept
  {
    mailbox_->close();
    address_.reset();
    screen_.reset();
  }

  void request_exit()
  {
    // A held key or repeated SIGINT must not create concurrent exit requests.
    if ( exit_request_ ) {
      return;
    }
    prefix_.reset();
    const RequestId request { next_request_++ };
    std::vector<ViewSnapshot> views;
    for ( const auto id : tab_order_ ) {
      const auto& tab = tabs_.at( id );
      if ( tab.view ) {
        views.push_back( tab.view->snapshot() );
      }
    }
    actions_.emplace_back( RequestExit { request, active_, std::move( views ) } );
    exit_request_ = request;
    dirty_ = true;
  }

  void apply( SyncTabs update, StepResult& )
  {
    // Cancel against the old tab map before a switch/removal destroys its view.
    if ( address_
         && ( update.active != address_->tab
              || std::ranges::find( update.tabs, address_->tab, &TabInfo::id ) == update.tabs.end() ) ) {
      finish_address();
    }
    if ( active_ != update.active ) {
      stop_drag();
    }
    std::map<TabId, TabState> retained;
    std::vector<TabId> order;
    order.reserve( update.tabs.size() );
    for ( auto& info : update.tabs ) {
      order.push_back( info.id );
      auto node = tabs_.extract( info.id );
      if ( node.empty() ) {
        const auto id = info.id;
        retained.emplace( id, TabState { .info = std::move( info ) } );
      } else {
        node.mapped().info = std::move( info );
        retained.insert( std::move( node ) );
      }
    }
    tabs_ = std::move( retained );
    tab_order_ = std::move( order );
    if ( active_ != update.active ) {
      prefix_.reset();
      notice_.clear();
    }
    active_ = update.active;
    dirty_ = true;
  }

  void apply( NavigationStarted update, StepResult& )
  {
    const auto found = tabs_.find( update.tab );
    // Closed-tab results are stale; they must not recreate a tab.
    if ( found == tabs_.end() ) {
      return;
    }
    found->second.navigation = update.request;
    found->second.info.loading_task = update.task;
    found->second.failure.reset();
    dirty_ = true;
  }

  void apply( CommitDocument update, StepResult& )
  {
    const auto found = tabs_.find( update.tab );
    if ( found == tabs_.end() || found->second.navigation != update.navigation ) {
      return;
    }
    auto& tab = found->second;
    const auto version = update.document.data().version;
    if ( tab.view && tab.view->document().data().version.document == version.document
         && version.revision < tab.view->document().data().version.revision ) {
      return;
    }
    const DocumentScope scope { update.tab, version };
    ViewSnapshot snapshot { .scope = scope };
    // Initial drafts are revision zero. A history view referring to edited values
    // cannot be restored without the corresponding draft snapshot/mapping.
    if ( update.restore && update.restore->scope == scope && update.restore->draft_revision == DraftRevision {} ) {
      snapshot = std::move( *update.restore );
    }
    tab.info.title = update.document.data().metadata.title;
    tab.info.display_url = update.document.data().metadata.display_url;
    tab.info.loading_task.reset();
    const auto dimensions = screen_->size();
    tab.view.emplace( std::move( update.document ),
                      std::move( snapshot ),
                      text_,
                      ScreenSize { dimensions.rows - 2, dimensions.columns } );
    if ( address_ && address_->tab == update.tab ) {
      // Address editing survives a background commit, but cancellation must now
      // return to the newly committed page, never restore the old view snapshot.
      address_->restore.reset();
    }
    if ( active_ == update.tab ) {
      prefix_.reset();
      notice_.clear();
    }
    tab.failure.reset();
    tab.copy_request.reset();
    tab.copy_feedback.clear();
    dirty_ = true;
  }

  void apply( CopyResult update, StepResult& )
  {
    const auto found = std::ranges::find_if(
      tabs_, [&update]( const auto& entry ) { return entry.second.copy_request == update.request; } );
    // Superseded requests, replaced documents, closed tabs and duplicate results
    // have no pending request; they cannot replace the current operation's feedback.
    if ( found == tabs_.end() ) {
      return;
    }
    auto& tab = found->second;
    tab.copy_request.reset();
    switch ( update.status ) {
      case CopyStatus::Confirmed:
        tab.copy_feedback = "Copied";
        break;
      case CopyStatus::Dispatched:
        tab.copy_feedback = "Copy request sent; clipboard acceptance unconfirmed";
        break;
      case CopyStatus::Unavailable:
        tab.copy_feedback = "Clipboard unavailable; disable mouse capture for terminal selection";
        break;
      case CopyStatus::Failed:
        tab.copy_feedback = "Copy failed | y retry";
        break;
    }
    if ( !update.summary.empty() ) {
      tab.copy_feedback += ": " + update.summary;
    }
    dirty_ = true;
  }

  void apply( ActionFailed update, StepResult& result )
  {
    const auto copy = std::ranges::find_if(
      tabs_, [&update]( const auto& entry ) { return entry.second.copy_request == update.request; } );
    if ( copy != tabs_.end() ) {
      apply( CopyResult { update.request, CopyStatus::Failed, std::move( update.summary ) }, result );
      return;
    }
    if ( !update.tab && exit_request_ == update.request ) {
      exit_request_.reset();
      dirty_ = true;
      return;
    }
    if ( !update.tab ) {
      return;
    }
    const auto found = tabs_.find( *update.tab );
    if ( found == tabs_.end() || found->second.navigation != update.request ) {
      return;
    }
    found->second.info.loading_task.reset();
    found->second.failure = std::move( update );
    dirty_ = true;
  }

  void apply( ExitDecision update, StepResult& )
  {
    if ( exit_request_ != update.request ) {
      return;
    }
    exit_request_.reset();
    if ( update.approved ) {
      close();
    }
    dirty_ = true;
  }

  // Incremental updates and form editing still require the later editing layer.
  void apply( UpdateDocument update, StepResult& result )
  { result.unhandled_updates.emplace_back( std::move( update ) ); }

  void apply( LocateView update, StepResult& )
  {
    const auto found = tabs_.find( update.scope.tab );
    // A late anchor request must never move another tab/document revision.
    if ( found == tabs_.end() || !found->second.view || found->second.view->snapshot().scope != update.scope ) {
      return;
    }
    auto& view = *found->second.view;
    const auto dimensions = screen_->size();
    view.resize( { dimensions.rows - 2, dimensions.columns } );
    std::optional<DocumentPoint> point;
    if ( const auto* name = std::get_if<std::string>( &update.target ) ) {
      const auto& anchors = view.document().data().anchors;
      const auto anchor = std::ranges::find( anchors, *name, &Anchor::name );
      if ( anchor != anchors.end() ) {
        point = std::visit( []( const auto& value ) -> DocumentPoint { return value; }, anchor->point );
      }
    } else {
      point = std::get<DocumentPoint>( update.target );
    }
    const bool located = point && view.locate( *point );
    if ( active_ == update.scope.tab ) {
      prefix_.reset();
      notice_ = located ? L"" : L"Location not found";
    }
    dirty_ = true;
  }

  void apply( FormFeedback update, StepResult& result )
  { result.unhandled_updates.emplace_back( std::move( update ) ); }

  std::size_t receive( StepResult& result )
  {
    // A finite batch prevents continuous producers from starving terminal input.
    auto updates = mailbox_->take( 64 );
    const auto count = updates.size();
    for ( auto& update : updates ) {
      std::visit( [this, &result]( auto message ) { apply( std::move( message ), result ); }, std::move( update ) );
      if ( !screen_ ) {
        break; // An approved exit ends this batch before any further UI work.
      }
    }
    return count;
  }

  detail::DocumentView* active_view()
  {
    if ( !active_ ) {
      return nullptr;
    }
    const auto found = tabs_.find( *active_ );
    const auto dimensions = screen_->size();
    // Selection/copy/cancel still address the active document in compact mode.
    if ( found == tabs_.end() || !found->second.view ) {
      return nullptr;
    }
    auto& view = *found->second.view;
    view.resize( { dimensions.rows - 2, dimensions.columns } );
    return &view;
  }

  void move( detail::ViewMotion motion )
  {
    const auto dimensions = screen_->size();
    if ( dimensions.rows < 3 || dimensions.columns < 1 ) {
      return; // No body rows exist for a visual-row/page motion.
    }
    if ( auto* view = active_view() ) {
      view->move( motion );
      tabs_.at( view->snapshot().scope.tab ).copy_feedback.clear();
      notice_.clear();
      dirty_ = true;
    }
  }

  void stop_drag()
  {
    if ( auto* view = active_view() ) {
      view->stop_drag();
    }
  }

  void focus( bool backward )
  {
    const auto dimensions = screen_->size();
    if ( dimensions.rows < 3 || dimensions.columns < 1 ) {
      return; // Object positioning requires a visible document region.
    }
    auto* view = active_view();
    if ( !view || view->snapshot().selection ) {
      return; // Tab must not move the cursor or extend/cancel an active selection.
    }
    const auto result = view->focus( backward );
    switch ( result ) {
      case detail::FocusResult::Empty:
        notice_ = L"No focusable links or controls";
        break;
      case detail::FocusResult::Moved:
        notice_.clear();
        break;
      case detail::FocusResult::Wrapped:
        notice_ = backward ? L"Focus wrapped to last object" : L"Focus wrapped to first object";
        break;
    }
    tabs_.at( view->snapshot().scope.tab ).copy_feedback.clear();
    dirty_ = true;
  }

  void activate_link()
  {
    auto* view = active_view();
    if ( !view || view->snapshot().selection ) {
      return; // Selection mode consumes activation without navigating.
    }
    const auto* link = view->focused_link();
    if ( !link ) {
      return; // Plain text and controls do not produce link navigation requests.
    }
    const RequestId request { next_request_++ };
    const auto source = view->snapshot().scope;
    actions_.emplace_back( OpenLink { request, source, link->id, link->target, OpenTarget::CurrentTab } );
    auto& tab = tabs_.at( source.tab );
    // Correlate rejection even when the host fails before NavigationStarted.
    tab.navigation = request;
    tab.failure.reset();
    tab.copy_feedback.clear();
    notice_.clear();
    dirty_ = true;
  }

  void copy_selection()
  {
    auto* view = active_view();
    if ( !view ) {
      return;
    }
    auto selected = view->selected_text();
    if ( !selected || selected->empty() ) {
      notice_ = selected ? L"Selection is empty" : L"No selection | v select";
      dirty_ = true;
      return;
    }
    const RequestId request { next_request_++ };
    const auto& snapshot = view->snapshot();
    actions_.emplace_back(
      CopySelection { request, snapshot.scope, snapshot.draft_revision, std::move( *selected ) } );
    auto& tab = tabs_.at( snapshot.scope.tab );
    tab.copy_request = request;
    tab.copy_feedback = "Copy pending";
    notice_.clear();
    dirty_ = true;
  }

  void open_address( AddressEntry entry )
  {
    const auto found = active_ ? tabs_.find( *active_ ) : tabs_.end();
    if ( found == tabs_.end() ) {
      notice_ = L"No active tab";
      dirty_ = true;
      return;
    }
    auto& tab = found->second;
    if ( tab.view && tab.view->snapshot().selection ) {
      return; // Address shortcuts belong to Browse, not the document selection mode.
    }
    stop_drag();
    const auto snapshot = tab.view ? std::optional( tab.view->snapshot() ) : std::nullopt;
    address_.emplace(
      tab.info.id, snapshot, entry == AddressEntry::Empty ? "" : tab.info.display_url, text_, screen_->size() );
    if ( entry == AddressEntry::Selected ) {
      address_->editor.execute( detail::EditorCommand::SelectAll );
    }
    prefix_.reset();
    notice_.clear();
    dirty_ = true;
  }

  void finish_address()
  {
    auto& tab = tabs_.at( address_->tab );
    const auto& restore = address_->restore;
    if ( restore && tab.view && tab.view->snapshot().scope == restore->scope
         && tab.view->snapshot().draft_revision == restore->draft_revision ) {
      // Copy the shared document before emplace destroys the current view.
      auto document = tab.view->document();
      const auto dimensions = screen_->size();
      tab.view.emplace(
        std::move( document ), *restore, text_, ScreenSize { dimensions.rows - 2, dimensions.columns } );
    }
    address_.reset();
    notice_.clear();
    dirty_ = true;
  }

  void submit_address()
  {
    const auto& value = address_->editor.value();
    if ( text_.analyze( value, false, false ).whitespace_only ) {
      address_->notice = L"Enter an address | Esc cancel";
      dirty_ = true;
      return;
    }
    const RequestId request { next_request_++ };
    actions_.emplace_back( OpenLocation { request, address_->tab, value, OpenTarget::CurrentTab } );
    auto& tab = tabs_.at( address_->tab );
    // A host rejection may precede NavigationStarted, just as for OpenLink.
    tab.navigation = request;
    tab.failure.reset();
    tab.copy_feedback.clear();
    finish_address();
  }

  void address_mouse( const Mouse& mouse )
  {
    const auto dimensions = screen_->size();
    const auto label = address_label_columns( dimensions );
    const CellPoint point { 0, mouse.position.column - label };
    if ( address_->dragging ) {
      if ( mouse.released.left || mouse.motion || mouse.pressed.left ) {
        address_->editor.place( point, true );
        address_->dragging = !mouse.released.left;
        dirty_ = true;
      }
      return;
    }
    if ( mouse.pressed.left && mouse.position.row == 0 && mouse.position.column >= label
         && mouse.position.column < dimensions.columns
         && ( mouse.modifiers == Modifiers {} || mouse.modifiers == Modifiers { .shift = true } ) ) {
      address_->editor.place( point, mouse.modifiers.shift );
      address_->dragging = true;
      dirty_ = true;
    }
    // Clicks and wheel events outside the input never reach the document underneath.
  }

  void address_input( const InputEvent& event )
  {
    if ( const auto* mouse = std::get_if<Mouse>( &event ) ) {
      address_mouse( *mouse );
      return;
    }
    const auto prefix = address_->editor.prefix_deadline();
    const auto intent = address_->editor.handle( event );
    if ( std::holds_alternative<Tick>( event ) ) {
      dirty_ = dirty_ || prefix != address_->editor.prefix_deadline();
      return;
    }
    address_->dragging = false;
    address_->notice.clear();
    dirty_ = true;
    switch ( intent ) {
      case detail::EditorIntent::Enter:
        submit_address();
        break;
      case detail::EditorIntent::Escape:
      case detail::EditorIntent::Cancel:
        finish_address();
        break;
      case detail::EditorIntent::Redraw:
        redraw_ = true;
        break;
      case detail::EditorIntent::Menu:
        address_->notice = L"Menu unavailable | Enter open | Esc cancel";
        break;
      default:
        break; // Tab/history/unbound keys have no second recipient in this mode.
    }
  }

  void draw_address( detail::Frame& frame, ScreenSize dimensions )
  {
    const auto label = address_label_columns( dimensions );
    append_chrome( frame, { 0, 0 }, std::wstring_view( L"URL: " ).substr( 0, label ), { .strong = true } );
    address_->editor.resize( { 1, dimensions.columns - label } );
    address_->editor.draw( frame, { 0, label } );
    if ( dimensions.rows > 1 ) {
      append_chrome( frame, { dimensions.rows - 1, 0 }, std::wstring( dimensions.columns, L' ' ) );
      std::wstring_view status
        = capabilities_.bracketed_paste
            ? L"Address | Enter open | Esc cancel"
            : L"Enter open | Esc cancel | Paste boundaries unavailable; multiline paste may confirm";
      if ( !address_->notice.empty() ) {
        status = address_->notice;
      }
      if ( address_->editor.prefix_deadline() ) {
        status = L"Ctrl+X ... | Ctrl+U undo | Esc cancel prefix";
      }
      append_chrome( frame, { dimensions.rows - 1, 0 }, status );
    }
  }

  void expire_prefix()
  {
    if ( prefix_ && Clock::now() >= prefix_->deadline ) {
      prefix_.reset();
      dirty_ = true;
    }
  }

  void browse_text( char32_t character )
  {
    using Motion = detail::ViewMotion;
    if ( prefix_ ) {
      const auto prefix = std::exchange( prefix_, std::nullopt )->character;
      dirty_ = true;
      if ( prefix == U'g' && character == U'g' ) {
        move( Motion::DocumentStart );
      } else if ( prefix == U'z' && character == U'h' ) {
        move( Motion::PanLeft );
      } else if ( prefix == U'z' && character == U'l' ) {
        move( Motion::PanRight );
      }
      // A mismatched sequence is consumed; gq must not turn into a quit action.
      return;
    }
    switch ( character ) {
      case U'o':
        open_address( AddressEntry::Empty );
        break;
      case U'O':
        open_address( AddressEntry::Current );
        break;
      case U'h':
        move( Motion::Left );
        break;
      case U'j':
        move( Motion::Down );
        break;
      case U'k':
        move( Motion::Up );
        break;
      case U'l':
        move( Motion::Right );
        break;
      case U'w':
        move( Motion::NextWord );
        break;
      case U'b':
        move( Motion::PreviousWord );
        break;
      case U'e':
        move( Motion::WordEnd );
        break;
      case U'G':
        move( Motion::DocumentEnd );
        break;
      case U'g':
      case U'z':
        prefix_ = KeyPrefix { character, Clock::now() + std::chrono::seconds( 1 ) };
        dirty_ = true;
        break;
      case U'v':
        if ( auto* view = active_view(); view && !view->snapshot().selection ) {
          view->begin_selection();
          tabs_.at( view->snapshot().scope.tab ).copy_feedback.clear();
          notice_.clear();
          dirty_ = true;
        }
        break;
      case U'y':
        copy_selection();
        break;
      case U' ': {
        auto* view = active_view();
        if ( view ) {
          const auto target = view->focused_target();
          // Selection owns Space; browse mode reserves control Space for activation.
          if ( view->snapshot().selection || !target || !std::holds_alternative<ControlId>( *target ) ) {
            move( Motion::PageDown );
          }
        }
        break;
      }
      case U'q': {
        const auto* view = active_view();
        if ( !view || !view->snapshot().selection ) {
          request_exit();
        }
        break;
      }
      default:
        break;
    }
  }

  void browse_key( const Key& key )
  {
    using Motion = detail::ViewMotion;
    if ( key.code == KeyCode::Escape ) {
      // A pending prefix is the top input layer; one Esc cancels only that layer.
      if ( !prefix_ ) {
        if ( auto* view = active_view() ) {
          view->cancel_selection();
          tabs_.at( view->snapshot().scope.tab ).copy_feedback.clear();
        }
      }
      prefix_.reset();
      notice_.clear();
      dirty_ = true;
      return;
    }
    if ( prefix_ ) {
      // Ctrl+C cancels the prefix first; no key falls through into another action.
      prefix_.reset();
      dirty_ = true;
      return;
    }
    if ( key.modifiers == Modifiers { .control = true } && key.code == KeyCode::Character ) {
      switch ( key.character ) {
        case U'l':
          open_address( AddressEntry::Selected );
          break;
        case U'c':
          if ( auto* view = active_view(); view && view->snapshot().selection ) {
            view->cancel_selection();
            tabs_.at( view->snapshot().scope.tab ).copy_feedback.clear();
            notice_.clear();
            dirty_ = true;
          } else {
            request_exit();
          }
          break;
        case U'u':
          move( Motion::HalfPageUp );
          break;
        case U'd':
          move( Motion::HalfPageDown );
          break;
        case U'z':
          notice_ = L"Suspend is unavailable";
          dirty_ = true;
          break;
        default:
          break;
      }
      return;
    }
    if ( key.modifiers == Modifiers { .alt = true }
         && ( key.code == KeyCode::Left || key.code == KeyCode::Right ) ) {
      const auto* view = active_view();
      if ( view && view->snapshot().selection ) {
        return; // Selection mode does not turn navigation into implicit activation.
      }
      if ( active_ ) {
        const auto found = tabs_.find( *active_ );
        if ( found != tabs_.end() ) {
          std::optional<DocumentVersion> version;
          if ( found->second.view ) {
            version = found->second.view->document().data().version;
          }
          actions_.emplace_back(
            Navigate { RequestId { next_request_++ },
                       *active_,
                       version,
                       key.code == KeyCode::Left ? NavigationCommand::Back : NavigationCommand::Forward } );
        }
      }
      return;
    }
    if ( key.code == KeyCode::Tab
         && ( key.modifiers == Modifiers {} || key.modifiers == Modifiers { .shift = true } ) ) {
      focus( key.modifiers.shift );
      return;
    }
    if ( key.modifiers != Modifiers {} ) {
      return; // Modified keys have distinct bindings; do not alias them to arrows.
    }
    switch ( key.code ) {
      case KeyCode::Function:
        if ( key.function == 6 ) {
          open_address( AddressEntry::Selected );
        }
        break;
      case KeyCode::Enter:
        activate_link();
        break;
      case KeyCode::Left:
        move( Motion::Left );
        break;
      case KeyCode::Right:
        move( Motion::Right );
        break;
      case KeyCode::Up:
        move( Motion::Up );
        break;
      case KeyCode::Down:
        move( Motion::Down );
        break;
      case KeyCode::Home:
        move( Motion::LineStart );
        break;
      case KeyCode::End:
        move( Motion::LineEnd );
        break;
      case KeyCode::PageUp:
        move( Motion::PageUp );
        break;
      case KeyCode::PageDown:
        move( Motion::PageDown );
        break;
      default:
        break;
    }
  }

  void browse_mouse( const Mouse& mouse )
  {
    auto* view = active_view();
    if ( !view ) {
      return;
    }
    const CellPoint point { mouse.position.row - 1, mouse.position.column };
    // Capture owns motion/release outside the body as well, so releasing on the
    // header/footer cannot leave a background autoscroll running.
    if ( view->dragging() ) {
      if ( mouse.released.left ) {
        view->release( point );
      } else if ( mouse.motion || mouse.pressed.left ) {
        view->drag( point );
      } else if ( mouse.wheel_steps != 0 ) {
        view->wheel( mouse.wheel_steps );
      }
      dirty_ = true;
      return;
    }
    const auto dimensions = screen_->size();
    if ( point.row < 0 || point.row >= dimensions.rows - 2 || point.column < 0 || point.column >= dimensions.columns
         || mouse.modifiers != Modifiers {} ) {
      return;
    }
    if ( mouse.wheel_steps != 0 ) {
      prefix_.reset();
      view->wheel( mouse.wheel_steps );
      dirty_ = true;
    } else if ( mouse.pressed.left ) {
      prefix_.reset();
      view->press( point, Clock::now() );
      tabs_.at( view->snapshot().scope.tab ).copy_feedback.clear();
      notice_.clear();
      dirty_ = true;
    }
  }

  void dispatch( const InputEvent& event, StepResult& result )
  {
    if ( const auto* closed = std::get_if<TerminalClosed>( &event ) ) {
      result.terminal_closed = *closed;
      close();
    } else if ( std::holds_alternative<Resize>( event ) ) {
      stop_drag();
      if ( address_ ) {
        address_->dragging = false;
        const auto dimensions = screen_->size();
        address_->editor.resize( { 1, dimensions.columns - address_label_columns( dimensions ) } );
      }
      dirty_ = true;
    } else if ( address_ ) {
      address_input( event );
    } else if ( const auto* tick = std::get_if<Tick>( &event ) ) {
      if ( auto* view = active_view() ) {
        dirty_ = view->tick( tick->now ) || dirty_;
      }
    } else if ( std::holds_alternative<Paste>( event ) ) {
      stop_drag();
      prefix_.reset();
      notice_ = L"Enter an input field before pasting";
      dirty_ = true;
    } else if ( const auto* text = std::get_if<TextInput>( &event ) ) {
      stop_drag();
      browse_text( text->character );
    } else if ( const auto* key = std::get_if<Key>( &event ) ) {
      stop_drag();
      browse_key( *key );
    } else if ( const auto* mouse = std::get_if<Mouse>( &event ) ) {
      browse_mouse( *mouse );
    }
  }

  bool read_input( StepResult& result )
  {
    const auto batch = screen_->read_events( 64 );
    for ( const auto& event : batch.events ) {
      expire_prefix();
      dispatch( event, result );
      if ( !screen_ ) {
        break;
      }
    }
    expire_prefix();
    return batch.consumed || !batch.events.empty();
  }

  void draw()
  {
    detail::Frame frame;
    const auto dimensions = screen_->size();
    // With fewer than three rows, header/body/footer cannot coexist.
    if ( dimensions.rows < 3 ) {
      const auto* view = active_view();
      append_chrome( frame,
                     { 0, 0 },
                     view && view->snapshot().selection ? L"Resize | Esc cancel selection" : L"Resize | q quit" );
    } else {
      append_chrome( frame, { 0, 0 }, L"Ricochet", { .strong = true } );
      std::wstring_view status = L"Browse | v select | q quit";
      std::string_view copy_feedback;
      if ( !capabilities_.bracketed_paste ) {
        status = L"Paste boundaries unavailable; multiline paste may confirm input | q quit";
      }
      if ( active_ ) {
        const auto found = tabs_.find( *active_ );
        if ( found != tabs_.end() ) {
          if ( found->second.view ) {
            found->second.view->resize( { dimensions.rows - 2, dimensions.columns } );
            found->second.view->draw( frame, { 1, 0 } );
            if ( found->second.view->snapshot().selection && notice_.empty() ) {
              status = L"Select | move extends | y copy | Esc cancel";
            }
          }
          copy_feedback = found->second.copy_feedback;
          if ( found->second.info.loading_task ) {
            status = L"Loading | q quit";
          }
          if ( found->second.failure ) {
            status = L"Navigation failed | q quit";
          }
        }
      }
      // Immediate input feedback (including focus wrap) must remain visible while
      // the page has a background navigation status.
      if ( !notice_.empty() ) {
        status = notice_;
      }
      if ( prefix_ ) {
        status = prefix_->character == U'g' ? L"g ... | Esc cancel" : L"z ... | Esc cancel";
      }
      if ( exit_request_ ) {
        status = L"Waiting for exit decision";
      }
      if ( !copy_feedback.empty() && notice_.empty() && !prefix_ && !exit_request_ ) {
        int column = 0;
        for ( const auto& glyph : text_.analyze( copy_feedback, false, false ).graphemes ) {
          if ( glyph.columns > dimensions.columns - column ) {
            break; // Provider messages are clipped at a complete grapheme boundary.
          }
          frame.glyphs.push_back(
            { .position = { dimensions.rows - 1, column }, .width = glyph.columns, .text = glyph.display } );
          column += glyph.columns;
        }
      } else {
        append_chrome( frame, { dimensions.rows - 1, 0 }, status );
      }
    }
    if ( address_ ) {
      draw_address( frame, dimensions );
    }
    screen_->draw( frame, std::exchange( redraw_, false ) );
    dirty_ = false;
  }

  StepResult step( std::chrono::milliseconds max_wait )
  {
    StepResult result;
    // Repeated step/close calls are part of the closed-session contract.
    if ( !screen_ ) {
      result.running = false;
      return result;
    }
    try {
      expire_prefix();
      const auto received = receive( result );
      if ( screen_ ) {
        const bool input = read_input( result );
        if ( screen_ && received == 0 && !input && !dirty_ ) {
          auto wait = max_wait;
          const auto deadline = address_  ? address_->editor.prefix_deadline()
                                : prefix_ ? std::optional( prefix_->deadline )
                                          : std::nullopt;
          if ( deadline ) {
            wait = std::min( wait,
                             std::max( std::chrono::milliseconds::zero(),
                                       std::chrono::ceil<std::chrono::milliseconds>( *deadline - Clock::now() ) ) );
          }
          const auto closed = screen_->wait( mailbox_->descriptor(), wait );
          if ( closed ) {
            dispatch( *closed, result );
          } else {
            receive( result );
            if ( screen_ ) {
              read_input( result );
            }
          }
        }
      }
      if ( screen_ ) {
        // Advance drag dwell even on a quiet terminal or a continuously busy input queue.
        dispatch( Tick { Clock::now() }, result );
      }
      if ( screen_ && dirty_ ) {
        draw();
      }
    } catch ( const std::exception& error ) {
      // Restore the terminal before the host can print or present the failure.
      close();
      result.error = error.what();
    }
    result.running = screen_ != nullptr;
    return result;
  }

  std::shared_ptr<detail::Mailbox> mailbox_;
  std::unique_ptr<detail::Screen> screen_;
  TerminalCapabilities capabilities_;
  detail::TextService text_ { detail::Screen::glyph_capacity() };
  std::map<TabId, TabState> tabs_ {};
  std::vector<TabId> tab_order_ {};
  std::optional<TabId> active_ {};
  std::optional<RequestId> exit_request_ {};
  std::optional<KeyPrefix> prefix_ {};
  std::optional<AddressInput> address_ {};
  std::uint64_t next_request_ { 1 };
  std::vector<UserAction> actions_ {};
  std::wstring notice_ {};
  bool dirty_ { true };
  bool redraw_ { false };
};

UpdateSender::UpdateSender( std::shared_ptr<detail::Mailbox> mailbox ) : mailbox_( std::move( mailbox ) ) {}

PostResult UpdateSender::post_update( HostUpdate update ) const
{ return mailbox_->post( std::move( update ) ); }

Session::Session( TerminalOptions options ) : Session( *stdin, *stdout, options ) {}

Session::Session( std::FILE& input, std::FILE& output, TerminalOptions options )
  : impl_( std::make_unique<Impl>( input, output, options ) )
{}

Session::~Session() = default;

UpdateSender Session::sender() const
{ return UpdateSender( impl_->mailbox_ ); }

PostResult Session::post_update( HostUpdate update )
{ return impl_->mailbox_->post( std::move( update ) ); }

StepResult Session::step( std::chrono::milliseconds max_wait )
{ return impl_->step( max_wait ); }

std::vector<UserAction> Session::take_actions()
{ return std::exchange( impl_->actions_, {} ); }

std::optional<ViewSnapshot> Session::snapshot( TabId tab ) const
{
  const auto found = impl_->tabs_.find( tab );
  if ( found == impl_->tabs_.end() || !found->second.view ) {
    return std::nullopt;
  }
  return found->second.view->snapshot();
}

TerminalCapabilities Session::capabilities() const
{ return impl_->capabilities_; }

void Session::close() noexcept
{ impl_->close(); }

} // namespace ricochet::tui
