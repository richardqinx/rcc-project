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

#include "screen.hh"

#include <algorithm>
#include <array>
#include <atomic>
#include <cerrno>
#include <climits>
#include <csignal>
#include <curses.h>
#include <cwchar>
#include <langinfo.h>
#include <locale.h>
#include <map>
#include <mutex>
#include <poll.h>
#include <stdexcept>
#include <string_view>
#include <system_error>
#include <type_traits>
#include <unistd.h>
#include <utility>

namespace ricochet::tui::detail {

namespace {

using Clock = std::chrono::steady_clock;
using namespace std::chrono_literals;

std::mutex terminal_mutex;
// atomic_flag is guaranteed lock-free. Clear means pending; test_and_set consumes
// the signal without a read/reset race, including delivery on a worker thread.
std::atomic_flag interrupt_clear = ATOMIC_FLAG_INIT;
std::atomic_flag termination_clear = ATOMIC_FLAG_INIT;

extern "C" void record_signal( int signal )
{
  switch ( signal ) {
    case SIGINT:
      interrupt_clear.clear( std::memory_order_relaxed );
      break;
    case SIGTERM:
    case SIGHUP:
      termination_clear.clear( std::memory_order_relaxed );
      break;
    default:
      break;
  }
}

std::unique_lock<std::mutex> acquire_terminal()
{
  std::unique_lock lock( terminal_mutex, std::try_to_lock );
  // The non-reentrant ncurses build has process-global current-screen state.
  if ( !lock.owns_lock() ) {
    throw std::runtime_error( "A terminal session is already active" );
  }
  return lock;
}

class ThreadLocale
{
public:
  ThreadLocale() : locale_( newlocale( LC_CTYPE_MASK, "", nullptr ), freelocale ), previous_( nullptr )
  {
    if ( !locale_ ) {
      throw std::system_error( errno, std::generic_category(), "newlocale" );
    }
    previous_ = uselocale( locale_.get() );
    if ( previous_ == nullptr ) {
      throw std::system_error( errno, std::generic_category(), "uselocale" );
    }
  }

  ~ThreadLocale() { uselocale( previous_ ); }
  ThreadLocale( const ThreadLocale& ) = delete;
  ThreadLocale& operator=( const ThreadLocale& ) = delete;

private:
  std::unique_ptr<std::remove_pointer_t<locale_t>, decltype( &freelocale )> locale_;
  locale_t previous_;
};

class Signals
{
public:
  Signals()
  {
    for ( std::size_t i = 0; i < numbers_.size(); ++i ) {
      if ( sigaction( numbers_[i], nullptr, &previous_[i] ) == -1 ) {
        throw std::system_error( errno, std::generic_category(), "Reading signal disposition" );
      }
    }
  }

  ~Signals()
  {
    for ( std::size_t i = 0; i < numbers_.size(); ++i ) {
      sigaction( numbers_[i], &previous_[i], nullptr );
    }
  }

  Signals( const Signals& ) = delete;
  Signals& operator=( const Signals& ) = delete;

  void prepare()
  {
    interrupt_clear.test_and_set( std::memory_order_relaxed );
    termination_clear.test_and_set( std::memory_order_relaxed );
    struct sigaction action {};
    sigemptyset( &action.sa_mask );
    action.sa_handler = SIG_DFL;
    // newterm owns SIGWINCH during the session; restore the host's handler later.
    if ( sigaction( SIGWINCH, &action, nullptr ) == -1 ) {
      throw std::system_error( errno, std::generic_category(), "Preparing resize signal disposition" );
    }
  }

  void install()
  {
    for ( const auto number : numbers_ ) {
      if ( number == SIGWINCH ) {
        continue; // Keep the resize handler installed by newterm.
      }
      struct sigaction action {};
      sigemptyset( &action.sa_mask );
      switch ( number ) {
        case SIGTSTP:
        case SIGPIPE:
          // No suspend support; output errors must reach the normal cleanup path.
          action.sa_handler = SIG_IGN;
          break;
        default:
          action.sa_handler = record_signal;
          break;
      }
      if ( sigaction( number, &action, nullptr ) == -1 ) {
        throw std::system_error( errno, std::generic_category(), "Installing signal disposition" );
      }
    }
  }

private:
  static constexpr std::array numbers_ { SIGINT, SIGTERM, SIGHUP, SIGTSTP, SIGWINCH, SIGPIPE };
  std::array<struct sigaction, numbers_.size()> previous_ {};
};

struct ScreenDeleter
{
  std::FILE* output;
  bool paste_enabled { false };

  void operator()( SCREEN* screen ) const noexcept
  {
    // Only undo the extension this session enabled. The rest belongs to ncurses.
    if ( paste_enabled ) {
      std::fputs( "\x1b[?2004l", output );
      std::fflush( output );
    }
    mousemask( 0, nullptr );
    endwin();
    delscreen( screen );
  }
};

Key character_key( char32_t character, Modifiers modifiers )
{ return { .code = KeyCode::Character, .modifiers = modifiers, .character = character }; }

InputEvent character_event( char32_t character )
{
  // These aliases are indistinguishable in the traditional terminal encoding.
  switch ( character ) {
    case U'\r':
    case U'\n':
      return Key { .code = KeyCode::Enter };
    case U'\t':
      return Key { .code = KeyCode::Tab };
    case U'\b':
    case 127:
      return Key { .code = KeyCode::Backspace };
    case 27:
      return Key { .code = KeyCode::Escape };
    case 0:
      return character_key( U' ', { .control = true } );
    case 28:
      return character_key( U'\\', { .control = true } );
    case 29:
      return character_key( U']', { .control = true } );
    case 30:
      return character_key( U'^', { .control = true } );
    case 31:
      return character_key( U'_', { .control = true } );
    default:
      break;
  }
  // The remaining C0 letters encode Ctrl+A through Ctrl+Z.
  if ( character >= 1 && character <= 26 ) {
    return character_key( U'a' + character - 1, { .control = true } );
  }
  return TextInput { character };
}

Mouse mouse_event( const MEVENT& mouse )
{
  return { .position = { mouse.y, mouse.x },
           .modifiers = { ( mouse.bstate & BUTTON_SHIFT ) != 0,
                          ( mouse.bstate & BUTTON_CTRL ) != 0,
                          ( mouse.bstate & BUTTON_ALT ) != 0 },
           .pressed = { ( mouse.bstate & BUTTON1_PRESSED ) != 0,
                        ( mouse.bstate & BUTTON2_PRESSED ) != 0,
                        ( mouse.bstate & BUTTON3_PRESSED ) != 0 },
           .released = { ( mouse.bstate & BUTTON1_RELEASED ) != 0,
                         ( mouse.bstate & BUTTON2_RELEASED ) != 0,
                         ( mouse.bstate & BUTTON3_RELEASED ) != 0 },
           .motion = ( mouse.bstate & REPORT_MOUSE_POSITION ) != 0,
           .wheel_steps = static_cast<int>( ( mouse.bstate & BUTTON5_PRESSED ) != 0 )
                          - static_cast<int>( ( mouse.bstate & BUTTON4_PRESSED ) != 0 ) };
}

} // namespace

class Screen::Impl
{
public:
  Impl( std::FILE& input, std::FILE& output, TerminalOptions options )
    : lock_( acquire_terminal() )
    , locale_()
    , signals_()
    , screen_( nullptr, ScreenDeleter { &output } )
    , input_fd_( fileno( &input ) )
    , options_( options )
  {
    // This backend requires a terminal, not a pipe with EOF-looking input.
    if ( isatty( input_fd_ ) != 1 || isatty( fileno( &output ) ) != 1 ) {
      throw std::runtime_error( "The TUI requires terminal input and output" );
    }
    if ( std::string_view( nl_langinfo( CODESET ) ) != "UTF-8" ) {
      throw std::runtime_error( "The TUI requires a UTF-8 locale" );
    }
    signals_.prepare();
    use_env( false );
    use_tioctl( true );
    screen_.reset( newterm( nullptr, &output, &input ) );
    // Unlike initscr, newterm reports failure without exiting the host process.
    if ( !screen_ ) {
      throw std::runtime_error( "ncurses newterm failed" );
    }
    // ncurses documents application signal handlers being installed after newterm.
    signals_.install();
    if ( raw() == ERR || noecho() == ERR || nonl() == ERR || meta( stdscr, true ) == ERR
         || keypad( stdscr, true ) == ERR ) {
      throw std::runtime_error( "ncurses input-mode setup failed" );
    }
    const auto delay = std::min<std::chrono::milliseconds::rep>( options.escape_delay.count(), INT_MAX );
    if ( set_escdelay( static_cast<int>( delay ) ) == ERR ) {
      throw std::runtime_error( "ncurses rejected the escape delay" );
    }
    options_.escape_delay = std::chrono::milliseconds( delay );
    wtimeout( stdscr, 0 );
    curs_set( 0 );      // ERR means cursor visibility cannot be changed on this terminal.
    mouseinterval( 0 ); // Deliver press/release immediately; no click aggregation delay.
    capabilities_.colors = has_colors() && start_color() != ERR;
    if ( options.capture_mouse ) {
      const auto mask = mousemask( ALL_MOUSE_EVENTS | REPORT_MOUSE_POSITION, nullptr );
      capabilities_.mouse_buttons = ( mask & BUTTON1_PRESSED ) != 0;
      capabilities_.mouse_motion = ( mask & REPORT_MOUSE_POSITION ) != 0;
    }
    install_keys();
    if ( options.bracketed_paste ) {
      if ( define_key( "\x1b[200~", paste_key_ ) == ERR ) {
        throw std::runtime_error( "ncurses could not register the paste delimiter" );
      }
      // Mark before writing so partial initialization also disables the extension.
      screen_.get_deleter().paste_enabled = true;
      if ( std::fputs( "\x1b[?2004h", &output ) == EOF || std::fflush( &output ) == EOF ) {
        throw std::system_error( errno, std::generic_category(), "Enabling bracketed paste" );
      }
      capabilities_.bracketed_paste = true;
    }
  }

  ScreenSize size() const { return { getmaxy( stdscr ), getmaxx( stdscr ) }; }

  void install_keys()
  {
    keys_ = { { KEY_ENTER, { .code = KeyCode::Enter } },
              { KEY_BACKSPACE, { .code = KeyCode::Backspace } },
              { KEY_BTAB, { .code = KeyCode::Tab, .modifiers = { .shift = true } } },
              { KEY_LEFT, { .code = KeyCode::Left } },
              { KEY_RIGHT, { .code = KeyCode::Right } },
              { KEY_UP, { .code = KeyCode::Up } },
              { KEY_DOWN, { .code = KeyCode::Down } },
              { KEY_HOME, { .code = KeyCode::Home } },
              { KEY_END, { .code = KeyCode::End } },
              { KEY_PPAGE, { .code = KeyCode::PageUp } },
              { KEY_NPAGE, { .code = KeyCode::PageDown } },
              { KEY_IC, { .code = KeyCode::Insert } },
              { KEY_DC, { .code = KeyCode::Delete } },
              { KEY_SLEFT, { .code = KeyCode::Left, .modifiers = { .shift = true } } },
              { KEY_SRIGHT, { .code = KeyCode::Right, .modifiers = { .shift = true } } },
              { KEY_SR, { .code = KeyCode::Up, .modifiers = { .shift = true } } },
              { KEY_SF, { .code = KeyCode::Down, .modifiers = { .shift = true } } },
              { KEY_SHOME, { .code = KeyCode::Home, .modifiers = { .shift = true } } },
              { KEY_SEND, { .code = KeyCode::End, .modifiers = { .shift = true } } } };
    for ( unsigned int n = 1; n <= 63; ++n ) {
      keys_.emplace( KEY_F( n ), Key { .code = KeyCode::Function, .function = n } );
    }
    const std::array extended { std::pair { "kLFT3", Key { .code = KeyCode::Left, .modifiers = { .alt = true } } },
                                std::pair { "kRIT3", Key { .code = KeyCode::Right, .modifiers = { .alt = true } } },
                                std::pair { "kUP3", Key { .code = KeyCode::Up, .modifiers = { .alt = true } } },
                                std::pair { "kDN3", Key { .code = KeyCode::Down, .modifiers = { .alt = true } } } };
    int code = paste_key_ + 1;
    for ( const auto& [name, key] : extended ) {
      const char* sequence = tigetstr( name );
      // tigetstr distinguishes absent capabilities from a non-string capability.
      if ( sequence == nullptr || sequence == reinterpret_cast<char*>( -1 ) ) {
        continue;
      }
      if ( define_key( sequence, code ) == ERR ) {
        throw std::runtime_error( "ncurses could not register an extended key" );
      }
      keys_.emplace( code++, key );
    }
  }

  void append_event( InputEvent event, std::vector<InputEvent>& events )
  {
    // Decoding a function key can itself consume the remaining Escape deadline.
    if ( escape_deadline_ && Clock::now() >= *escape_deadline_ ) {
      events.emplace_back( Key { .code = KeyCode::Escape } );
      escape_deadline_.reset();
    }
    // An Escape prefix combines only with a key arriving inside its deadline.
    if ( escape_deadline_ ) {
      if ( auto* text = std::get_if<TextInput>( &event ) ) {
        event = character_key( text->character, { .alt = true } );
      } else if ( auto* key = std::get_if<Key>( &event ); key != nullptr && key->code != KeyCode::Escape ) {
        key->modifiers.alt = true;
      } else {
        events.emplace_back( Key { .code = KeyCode::Escape } );
      }
      escape_deadline_.reset();
    }
    if ( const auto* key = std::get_if<Key>( &event ); key != nullptr && key->code == KeyCode::Escape ) {
      escape_deadline_ = Clock::now() + options_.escape_delay;
      return;
    }
    events.push_back( std::move( event ) );
  }

  void append_character( char32_t character, std::vector<InputEvent>& events )
  {
    if ( paste_ ) {
      paste_->push_back( character );
      constexpr std::u32string_view end = U"\x1b[201~";
      if ( paste_->ends_with( end ) ) {
        paste_->resize( paste_->size() - end.size() );
        events.emplace_back( Paste { std::move( *paste_ ) } );
        paste_.reset();
        if ( keypad( stdscr, true ) == ERR ) {
          throw std::runtime_error( "ncurses could not leave paste input mode" );
        }
      }
    } else {
      append_event( character_event( character ), events );
    }
  }

  void decode_byte( unsigned char byte, std::vector<InputEvent>& events )
  {
    // wget_wch keeps its conversion buffer local to one call; nodelay can split a
    // character across calls. Retain the standard conversion state across steps.
    // Evidence: ncurses/widechar/lib_get_wch.c, wget_wch's local count/buffer.
    const bool had_partial = std::mbsinit( &text_state_ ) == 0;
    const char input = static_cast<char>( byte );
    wchar_t character {};
    const auto converted = std::mbrtowc( &character, &input, 1, &text_state_ );
    if ( converted == static_cast<std::size_t>( -2 ) ) {
      return; // The standard library needs continuation bytes, possibly next step.
    }
    if ( converted == static_cast<std::size_t>( -1 ) ) {
      text_state_ = {};
      append_character( U'\uFFFD', events );
      // Retry the offending byte as a new sequence so an ASCII character following
      // an incomplete sequence is not lost. Reset state bounds this to one retry.
      if ( had_partial ) {
        decode_byte( byte, events );
      }
      return;
    }
    append_character( static_cast<char32_t>( character ), events );
  }

  InputBatch read_events( std::size_t limit )
  {
    InputBatch batch;
    if ( !termination_clear.test_and_set( std::memory_order_relaxed ) ) {
      batch.events.emplace_back( TerminalClosed { TerminalCloseReason::TerminationSignal } );
      return batch;
    }
    // Check disconnection even when continuous host updates prevent an idle wait.
    if ( const auto closed = wait( -1, 0ms ) ) {
      batch.events.emplace_back( *closed );
      return batch;
    }
    if ( !interrupt_clear.test_and_set( std::memory_order_relaxed ) ) {
      batch.events.emplace_back( character_key( U'c', { .control = true } ) );
      escape_deadline_.reset();
    }
    if ( escape_deadline_ && Clock::now() >= *escape_deadline_ ) {
      batch.events.emplace_back( Key { .code = KeyCode::Escape } );
      escape_deadline_.reset();
    }
    for ( std::size_t n = 0; n < limit; ++n ) {
      errno = 0;
      const int value = wgetch( stdscr );
      if ( value == ERR ) {
        // nodelay has no byte available; signals may interrupt the ncurses reader.
        if ( errno != 0 && errno != EAGAIN && errno != EINTR ) {
          throw std::system_error( errno, std::generic_category(), "ncurses input" );
        }
        break;
      }
      batch.consumed = true;
      // wgetch returns bytes below KEY_MIN and decoded function keys above it.
      if ( value >= KEY_MIN ) {
        // Resize is out-of-band and can occur between UTF-8 continuation bytes.
        // Other function keys interrupt an unfinished text sequence.
        if ( value != KEY_RESIZE && std::mbsinit( &text_state_ ) == 0 ) {
          text_state_ = {};
          append_character( U'\uFFFD', batch.events );
        }
        switch ( value ) {
          case KEY_RESIZE:
            batch.events.emplace_back( Resize { size() } );
            break;
          case KEY_MOUSE: {
            MEVENT mouse {};
            if ( getmouse( &mouse ) == ERR ) {
              throw std::runtime_error( "ncurses mouse event could not be read" );
            }
            append_event( mouse_event( mouse ), batch.events );
            break;
          }
          case paste_key_:
            // A preceding standalone Escape cannot become a modifier on pasted text.
            if ( escape_deadline_ ) {
              batch.events.emplace_back( Key { .code = KeyCode::Escape } );
              escape_deadline_.reset();
            }
            paste_.emplace();
            // Preserve literal escape sequences inside pasted text. Only ncurses
            // reads bytes; the backend recognizes the protocol's end delimiter.
            if ( keypad( stdscr, false ) == ERR ) {
              throw std::runtime_error( "ncurses could not enter paste input mode" );
            }
            break;
          default: {
            const auto found = keys_.find( static_cast<int>( value ) );
            if ( found != keys_.end() ) {
              append_event( found->second, batch.events );
            } else {
              append_event( Key { .code = KeyCode::Unsupported }, batch.events );
            }
            break;
          }
        }
      } else {
        decode_byte( static_cast<unsigned char>( value ), batch.events );
      }
    }
    return batch;
  }

  std::optional<TerminalClosed> wait( int wake_fd, std::chrono::milliseconds max_wait ) const
  {
    // The signal may arrive on a producer thread. This bound also lets wgetch
    // observe ncurses' SIGWINCH flag when no terminal bytes have arrived.
    auto delay = std::clamp( max_wait, 0ms, 50ms );
    if ( escape_deadline_ ) {
      delay = std::min(
        delay, std::max( 0ms, std::chrono::ceil<std::chrono::milliseconds>( *escape_deadline_ - Clock::now() ) ) );
    }
    std::array<pollfd, 2> descriptors { pollfd { input_fd_, POLLIN, 0 }, pollfd { wake_fd, POLLIN, 0 } };
    const int result = poll( descriptors.data(), descriptors.size(), static_cast<int>( delay.count() ) );
    if ( result == -1 && errno != EINTR ) {
      throw std::system_error( errno, std::generic_category(), "poll" );
    }
    if ( ( descriptors[0].revents & ( POLLHUP | POLLERR | POLLNVAL ) ) != 0 ) {
      return TerminalClosed { TerminalCloseReason::Disconnected };
    }
    if ( ( descriptors[1].revents & ( POLLHUP | POLLERR | POLLNVAL ) ) != 0 ) {
      throw std::runtime_error( "The UI wake channel was disconnected" );
    }
    return std::nullopt;
  }

  void draw( const Frame& frame ) const
  {
    const auto dimensions = size();
    if ( werase( stdscr ) == ERR ) {
      throw std::runtime_error( "ncurses could not clear its window buffer" );
    }
    for ( const auto& glyph : frame.glyphs ) {
      const auto [row, column] = glyph.position;
      // A resize can clip a previously prepared frame. Never wrap clipped glyphs.
      if ( row < 0 || row >= dimensions.rows || column < 0 || column >= dimensions.columns
           || glyph.width > dimensions.columns - column ) {
        continue;
      }
      // setcchar would silently truncate longer combining sequences.
      if ( glyph.text.size() >= CCHARW_MAX ) {
        throw std::runtime_error( "Glyph exceeds the ncurses complex-character capacity" );
      }
      const attr_t attributes
        = ( glyph.style.strong ? A_BOLD : A_NORMAL ) | ( glyph.style.emphasis ? A_UNDERLINE : A_NORMAL )
          | ( glyph.style.subdued ? A_DIM : A_NORMAL ) | ( glyph.style.warning ? A_REVERSE : A_NORMAL )
          | ( glyph.selected ? A_REVERSE | A_UNDERLINE : A_NORMAL ) | ( glyph.cursor ? A_BOLD : A_NORMAL );
      cchar_t cell {};
      if ( setcchar( &cell, glyph.text.c_str(), attributes, 0, nullptr ) == ERR
           || mvwadd_wchnstr( stdscr, row, column, &cell, 1 ) == ERR ) {
        throw std::runtime_error( "ncurses could not paint a glyph" );
      }
    }
    // add_wchnstr does not advance or wrap the cursor at the bottom-right cell.
    if ( frame.cursor && frame.cursor->row >= 0 && frame.cursor->row < dimensions.rows && frame.cursor->column >= 0
         && frame.cursor->column < dimensions.columns ) {
      if ( wmove( stdscr, frame.cursor->row, frame.cursor->column ) == ERR ) {
        throw std::runtime_error( "ncurses could not position the cursor" );
      }
      curs_set( 1 );
    } else {
      curs_set( 0 );
    }
    if ( wnoutrefresh( stdscr ) == ERR || doupdate() == ERR ) {
      throw std::runtime_error( "ncurses screen refresh failed" );
    }
  }

  std::unique_lock<std::mutex> lock_;
  ThreadLocale locale_;
  Signals signals_;
  std::unique_ptr<SCREEN, ScreenDeleter> screen_;
  int input_fd_;
  TerminalOptions options_;
  TerminalCapabilities capabilities_ {};
  std::map<int, Key> keys_ {};
  std::optional<Clock::time_point> escape_deadline_ {};
  std::optional<std::u32string> paste_ {};
  std::mbstate_t text_state_ {};
  static constexpr int paste_key_ = 0x10000;
};

Screen::Screen( std::FILE& input, std::FILE& output, TerminalOptions options )
  : impl_( std::make_unique<Impl>( input, output, options ) )
{}

Screen::~Screen() = default;

std::size_t Screen::glyph_capacity()
{ return CCHARW_MAX - 1; }

ScreenSize Screen::size() const
{ return impl_->size(); }

TerminalCapabilities Screen::capabilities() const
{ return impl_->capabilities_; }

InputBatch Screen::read_events( std::size_t limit )
{ return impl_->read_events( limit ); }

std::optional<TerminalClosed> Screen::wait( int wake_fd, std::chrono::milliseconds max_wait )
{ return impl_->wait( wake_fd, max_wait ); }

void Screen::draw( const Frame& frame )
{ impl_->draw( frame ); }

} // namespace ricochet::tui::detail
