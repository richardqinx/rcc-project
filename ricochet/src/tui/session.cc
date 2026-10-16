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

#include "mailbox.hh"
#include "screen.hh"

#include <algorithm>
#include <exception>
#include <map>
#include <string_view>
#include <utility>

namespace ricochet::tui {

namespace {

struct TabState
{
  TabInfo info {};
  std::optional<RequestId> navigation {};
  std::optional<Document> document {};
  std::optional<ViewSnapshot> view {};
  std::optional<ActionFailed> failure {};
};

// Only fixed ASCII chrome uses this helper. Document text goes through the
// forthcoming Unicode layout service, not a second ad-hoc text renderer.
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
    screen_.reset();
  }

  void request_exit()
  {
    // A held key or repeated SIGINT must not create concurrent exit requests.
    if ( exit_request_ ) {
      return;
    }
    const RequestId request { next_request_++ };
    std::vector<ViewSnapshot> views;
    for ( const auto id : tab_order_ ) {
      const auto& tab = tabs_.at( id );
      if ( tab.view ) {
        views.push_back( *tab.view );
      }
    }
    actions_.emplace_back( RequestExit { request, active_, std::move( views ) } );
    exit_request_ = request;
    dirty_ = true;
  }

  void apply( SyncTabs update, StepResult& )
  {
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
    if ( tab.document && tab.document->data().version.document == version.document
         && version.revision < tab.document->data().version.revision ) {
      return;
    }
    const DocumentScope scope { update.tab, version };
    tab.view = ViewSnapshot { .scope = scope };
    // Initial drafts are revision zero. A history view referring to edited values
    // cannot be restored without the corresponding draft snapshot/mapping.
    if ( update.restore && update.restore->scope == scope && update.restore->draft_revision == DraftRevision {} ) {
      tab.view = std::move( update.restore );
    }
    tab.info.title = update.document.data().metadata.title;
    tab.info.display_url = update.document.data().metadata.display_url;
    tab.info.loading_task.reset();
    tab.document = std::move( update.document );
    tab.failure.reset();
    dirty_ = true;
  }

  void apply( ActionFailed update, StepResult& )
  {
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

  // These messages require document-view/editor state, which this backend stage
  // does not implement. Returning ownership keeps the incomplete boundary explicit.
  void apply( UpdateDocument update, StepResult& result )
  { result.unhandled_updates.emplace_back( std::move( update ) ); }

  void apply( LocateView update, StepResult& result )
  { result.unhandled_updates.emplace_back( std::move( update ) ); }

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

  void dispatch( const InputEvent& event, StepResult& result )
  {
    if ( const auto* closed = std::get_if<TerminalClosed>( &event ) ) {
      result.terminal_closed = *closed;
      close();
    } else if ( std::holds_alternative<Resize>( event ) ) {
      dirty_ = true;
    } else if ( std::holds_alternative<Paste>( event ) ) {
      // The baseline session is Browse: a paste never executes its text as keys.
      notice_ = L"Enter an input field before pasting";
      dirty_ = true;
    } else if ( const auto* text = std::get_if<TextInput>( &event ) ) {
      if ( text->character == U'q' ) {
        request_exit();
      }
    } else if ( const auto* key = std::get_if<Key>( &event ) ) {
      if ( key->code == KeyCode::Character && key->modifiers == Modifiers { .control = true } ) {
        switch ( key->character ) {
          case U'c':
            request_exit();
            break;
          case U'z':
            notice_ = L"Suspend is unavailable";
            dirty_ = true;
            break;
          default:
            break;
        }
      }
    }
    // Mouse/navigation/editor bindings belong to the upcoming document-view layer.
  }

  bool read_input( StepResult& result )
  {
    const auto batch = screen_->read_events( 64 );
    for ( const auto& event : batch.events ) {
      dispatch( event, result );
      if ( !screen_ ) {
        break;
      }
    }
    return batch.consumed || !batch.events.empty();
  }

  void draw()
  {
    detail::Frame frame;
    const auto dimensions = screen_->size();
    // With fewer than three rows, header/body/footer cannot coexist.
    if ( dimensions.rows < 3 ) {
      append_chrome( frame, { 0, 0 }, L"Resize | q quit" );
    } else {
      append_chrome( frame, { 0, 0 }, L"Ricochet", { .strong = true } );
      std::wstring_view status = L"Browse | q quit";
      if ( !capabilities_.bracketed_paste ) {
        status = L"Paste boundaries unavailable; multiline paste may confirm input | q quit";
      }
      if ( !notice_.empty() ) {
        status = notice_;
      }
      if ( active_ ) {
        const auto found = tabs_.find( *active_ );
        if ( found != tabs_.end() ) {
          if ( found->second.info.loading_task ) {
            status = L"Loading | q quit";
          }
          if ( found->second.failure ) {
            status = L"Navigation failed | q quit";
          }
        }
      }
      if ( exit_request_ ) {
        status = L"Waiting for exit decision";
      }
      append_chrome( frame, { dimensions.rows - 1, 0 }, status );
    }
    screen_->draw( frame );
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
      const auto received = receive( result );
      if ( screen_ ) {
        const bool input = read_input( result );
        if ( screen_ && received == 0 && !input && !dirty_ ) {
          const auto closed = screen_->wait( mailbox_->descriptor(), max_wait );
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
  std::map<TabId, TabState> tabs_ {};
  std::vector<TabId> tab_order_ {};
  std::optional<TabId> active_ {};
  std::optional<RequestId> exit_request_ {};
  std::uint64_t next_request_ { 1 };
  std::vector<UserAction> actions_ {};
  std::wstring notice_ {};
  bool dirty_ { true };
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
  if ( found == impl_->tabs_.end() ) {
    return std::nullopt;
  }
  return found->second.view;
}

TerminalCapabilities Session::capabilities() const
{ return impl_->capabilities_; }

void Session::close() noexcept
{ impl_->close(); }

} // namespace ricochet::tui
