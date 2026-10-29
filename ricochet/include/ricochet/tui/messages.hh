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

#include "ricochet/tui/document.hh"
#include "ricochet/tui/view.hh"

#include <cstdint>
#include <optional>
#include <string>
#include <variant>
#include <vector>

namespace ricochet::tui {

struct TabInfo
{
  TabId id {};
  std::string title {};
  std::string display_url {};
  std::optional<TaskId> loading_task {};
};

struct SyncTabs
{
  // Vector order is tab order. Request IDs acknowledge create/close actions.
  std::vector<TabInfo> tabs {};
  std::optional<TabId> active {};
  std::vector<RequestId> completed_requests {};
};

struct NavigationStarted
{
  TabId tab {};
  RequestId request {};
  TaskId task {};
  std::string display_target {};
};

struct CommitDocument
{
  TabId tab {};
  RequestId navigation {};
  Document document;
  std::optional<ViewSnapshot> restore {};
};

struct TextCorrespondence
{
  // The producer identifies identical text in the two revisions, including every
  // changed portion crossed by a selection. This is not an inferred text diff.
  TextRange source {};
  TextRange target {};
};

struct PositionMapping
{
  DocumentVersion source {};
  DocumentVersion target {};
  std::vector<TextCorrespondence> text {};
};

enum class ViewRetention : std::uint8_t
{
  Reset,
  PreserveUnchanged
};

struct UpdateDocument
{
  TabId tab {};
  RequestId navigation {};
  DocumentVersion source {};
  Document document;
  ViewRetention retention { ViewRetention::PreserveUnchanged };
  std::optional<PositionMapping> mapping {};
};

struct LocateView
{
  DocumentScope scope {};
  std::variant<std::string, DocumentPoint> target {};
};

// Values are keyed by control identity, never by field name or action URL.
// These business messages may carry passwords; ViewSnapshot deliberately does not.
using ControlValue = std::variant<std::string, bool, std::vector<OptionId>>;

struct ControlDraft
{
  ControlId control {};
  ControlValue value {};
};

struct FieldError
{
  ControlId control {};
  std::string message {};
};

struct FormFeedback
{
  DocumentScope scope {};
  FormId form {};
  RequestId request {};
  DraftRevision draft_revision {};
  std::vector<FieldError> errors {};
  std::vector<ControlDraft> normalized_values {};
};

struct ActionFailed
{
  RequestId request {};
  std::optional<TabId> tab {};
  std::string summary {};
  std::optional<std::string> details {};
};

struct ExitDecision
{
  RequestId request {};
  bool approved { false };
  std::optional<std::string> explanation {};
};

enum class CopyStatus : std::uint8_t
{
  Confirmed,
  Dispatched,
  Unavailable,
  Failed
};

// The host's injected clipboard provider completes a CopySelection request once.
// Dispatched must not be used as confirmation that a terminal accepted the text.
struct CopyResult
{
  RequestId request {};
  CopyStatus status { CopyStatus::Unavailable };
  std::string summary {};
};

using HostUpdate = std::variant<SyncTabs,
                                NavigationStarted,
                                CommitDocument,
                                UpdateDocument,
                                LocateView,
                                FormFeedback,
                                CopyResult,
                                ActionFailed,
                                ExitDecision>;

enum class OpenTarget : std::uint8_t
{
  CurrentTab,
  NewTab
};

struct OpenLocation
{
  RequestId request {};
  TabId tab {};
  std::string input {};
  OpenTarget target { OpenTarget::CurrentTab };
};

struct OpenLink
{
  RequestId request {};
  DocumentScope source {};
  LinkId link {};
  std::string target {};
  OpenTarget destination { OpenTarget::CurrentTab };
};

enum class NavigationCommand : std::uint8_t
{
  Back,
  Forward,
  Reload
};

struct Navigate
{
  RequestId request {};
  TabId tab {};
  std::optional<DocumentVersion> document {};
  NavigationCommand command { NavigationCommand::Reload };
};

struct CreateTab
{ RequestId request {}; };

struct CloseTab
{
  RequestId request {};
  TabId tab {};
};

enum class FormOperation : std::uint8_t
{
  Submit,
  Reset
};

struct FormAction
{
  RequestId request {};
  DocumentScope scope {};
  FormId form {};
  std::optional<ControlId> button {};
  DraftRevision draft_revision {};
  FormOperation operation { FormOperation::Submit };
  std::vector<ControlDraft> values {};
};

struct ExportSelection
{
  RequestId request {};
  DocumentScope source {};
  // Already extracted from one view snapshot; passwords are masked.
  std::string text {};
};

struct CopySelection
{
  RequestId request {};
  DocumentScope source {};
  DraftRevision draft_revision {};
  // Frozen before dispatch; later cursor/selection changes cannot change this copy.
  // Password content is already masked. Clipboard providers never receive drafts.
  std::string text {};
};

struct RequestExit
{
  RequestId request {};
  std::optional<TabId> active {};
  std::vector<ViewSnapshot> views {};
};

using UserAction = std::variant<OpenLocation,
                                OpenLink,
                                Navigate,
                                CreateTab,
                                CloseTab,
                                FormAction,
                                ExportSelection,
                                CopySelection,
                                RequestExit>;

} // namespace ricochet::tui
