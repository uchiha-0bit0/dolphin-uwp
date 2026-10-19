// Copyright 2026 Dolphin Emulator Project
// SPDX-License-Identifier: GPL-2.0-or-later

#include "DolphinUWP/GameTextInput.h"

#include <algorithm>
#include <atomic>
#include <chrono>
#include <climits>
#include <string_view>
#include <utility>

#include <winrt/Windows.Foundation.Collections.h>
#include <winrt/Windows.Foundation.h>
#include <winrt/Windows.UI.Text.Core.h>
#include <winrt/Windows.UI.ViewManagement.Core.h>
#include <winrt/Windows.UI.ViewManagement.h>

#include "Common/CommonTypes.h"
#include "Common/Logging/Log.h"
#include "Common/StringUtil.h"

#pragma warning(push)
// C++/WinRT delegate templates instantiated with the lambdas below.
#pragma warning(disable : 4265 4946)

namespace DolphinUWP::GameTextInput
{
namespace
{
namespace TextCore = winrt::Windows::UI::Text::Core;
namespace ViewCore = winrt::Windows::UI::ViewManagement::Core;
using winrt::Windows::System::VirtualKey;

struct State
{
  bool active = false;
  bool multiline = false;
  std::wstring text;
  TextCore::CoreTextRange selection{0, 0};
  Result result = Result::None;
  TextCore::CoreTextEditContext context{nullptr};
  ViewCore::CoreInputView input_view{nullptr};
  u64 generation = 0;
};

State s_state;
std::atomic<bool> s_keyboard_visible = false;

s32 Length()
{
  return static_cast<s32>(std::min<size_t>(s_state.text.size(), INT32_MAX));
}

s32 SelectionStart()
{
  return std::clamp(
      std::min(s_state.selection.StartCaretPosition, s_state.selection.EndCaretPosition), 0,
      Length());
}

s32 SelectionEnd()
{
  return std::clamp(
      std::max(s_state.selection.StartCaretPosition, s_state.selection.EndCaretPosition), 0,
      Length());
}

void NotifySelection()
{
  if (!s_state.context)
    return;
  try
  {
    s_state.context.NotifySelectionChanged(s_state.selection);
  }
  catch (const winrt::hresult_error&)
  {
  }
}

// Replaces the selection, as typing does, and tells the text services.
void Replace(std::wstring_view replacement)
{
  const s32 start = SelectionStart();
  const s32 end = SelectionEnd();
  s_state.text.replace(static_cast<size_t>(start), static_cast<size_t>(end - start), replacement);
  const s32 caret = start + static_cast<s32>(replacement.size());
  s_state.selection = {caret, caret};
  if (!s_state.context)
    return;
  try
  {
    s_state.context.NotifyTextChanged({start, end}, static_cast<s32>(replacement.size()),
                                      s_state.selection);
  }
  catch (const winrt::hresult_error&)
  {
  }
}

void SetCaret(s32 caret)
{
  caret = std::clamp(caret, 0, Length());
  s_state.selection = {caret, caret};
  NotifySelection();
}

s32 LineStart(s32 position)
{
  const size_t found = position > 0 ? s_state.text.rfind(L'\n', static_cast<size_t>(position - 1)) :
                                      std::wstring::npos;
  return found == std::wstring::npos ? 0 : static_cast<s32>(found) + 1;
}

s32 LineEnd(s32 position)
{
  const size_t found = s_state.text.find(L'\n', static_cast<size_t>(position));
  return found == std::wstring::npos ? Length() : static_cast<s32>(found);
}

void MoveLine(bool down)
{
  const s32 caret = SelectionEnd();
  const s32 start = LineStart(caret);
  const s32 column = caret - start;
  if (down)
  {
    const s32 end = LineEnd(caret);
    if (end >= Length())
      return;
    SetCaret(std::min(end + 1 + column, LineEnd(end + 1)));
  }
  else if (start > 0)
  {
    const s32 previous = LineStart(start - 1);
    SetCaret(std::min(previous + column, start - 1));
  }
}

void ReleaseContext()
{
  ++s_state.generation;
  if (!s_state.context)
    return;
  try
  {
    s_state.context.NotifyFocusLeave();
  }
  catch (const winrt::hresult_error&)
  {
  }
  s_state.context = nullptr;
}

void WatchKeyboard()
{
  if (s_state.input_view)
    return;
  try
  {
    s_state.input_view = ViewCore::CoreInputView::GetForCurrentView();
    s_state.input_view.OcclusionsChanged(
        [](auto&&, const ViewCore::CoreInputViewOcclusionsChangedEventArgs& args) {
          s_keyboard_visible = args.Occlusions().Size() != 0;
        });
  }
  catch (const winrt::hresult_error& error)
  {
    ERROR_LOG_FMT(COMMON, "The on-screen keyboard can't be watched: {}",
                  winrt::to_string(error.message()));
  }
}

bool CreateContext()
{
  try
  {
    const u64 generation = ++s_state.generation;
    s_state.context = TextCore::CoreTextServicesManager::GetForCurrentView().CreateEditContext();
    s_state.context.Name(L"Dolphin overlay");
    s_state.context.InputScope(TextCore::CoreTextInputScope::Text);
    s_state.context.InputPaneDisplayPolicy(TextCore::CoreTextInputPaneDisplayPolicy::Manual);
    s_state.context.TextRequested(
        [generation](auto&&, const TextCore::CoreTextTextRequestedEventArgs& args) {
          if (generation != s_state.generation)
            return;
          const auto request = args.Request();
          const s32 start = std::clamp(request.Range().StartCaretPosition, 0, Length());
          const s32 end = std::clamp(request.Range().EndCaretPosition, start, Length());
          request.Text(winrt::hstring(
              s_state.text.substr(static_cast<size_t>(start), static_cast<size_t>(end - start))));
        });
    s_state.context.SelectionRequested(
        [generation](auto&&, const TextCore::CoreTextSelectionRequestedEventArgs& args) {
          if (generation == s_state.generation)
            args.Request().Selection(s_state.selection);
        });
    s_state.context.LayoutRequested(
        [](auto&&, const TextCore::CoreTextLayoutRequestedEventArgs& args) {
          const auto bounds = args.Request().LayoutBounds();
          bounds.TextBounds({0.0f, 0.0f, 1.0f, 1.0f});
          bounds.ControlBounds({0.0f, 0.0f, 1.0f, 1.0f});
        });
    s_state.context.TextUpdating([generation](auto&&,
                                              const TextCore::CoreTextTextUpdatingEventArgs& args) {
      const auto range = args.Range();
      if (generation != s_state.generation || range.StartCaretPosition < 0 ||
          range.StartCaretPosition > range.EndCaretPosition || range.EndCaretPosition > Length())
      {
        args.Result(TextCore::CoreTextTextUpdatingResult::Failed);
        return;
      }
      std::wstring inserted(args.Text());
      bool rewritten = false;
      if (inserted.find_first_of(L"\r\n") != std::wstring::npos)
      {
        rewritten = true;
        if (s_state.multiline)
        {
          for (size_t found = inserted.find(L"\r\n"); found != std::wstring::npos;
               found = inserted.find(L"\r\n", found))
          {
            inserted.erase(found, 1);
          }
          std::ranges::replace(inserted, L'\r', L'\n');
        }
        else
        {
          // The Enter key of the on-screen keyboard submits a single line.
          std::erase_if(inserted, [](wchar_t c) { return c == L'\r' || c == L'\n'; });
          s_state.result = Result::Submitted;
        }
      }
      s_state.text.replace(static_cast<size_t>(range.StartCaretPosition),
                           static_cast<size_t>(range.EndCaretPosition - range.StartCaretPosition),
                           inserted);
      if (rewritten)
      {
        const s32 caret = range.StartCaretPosition + static_cast<s32>(inserted.size());
        s_state.selection = {caret, caret};
      }
      else
      {
        const auto selection = args.NewSelection();
        s_state.selection = {std::clamp(selection.StartCaretPosition, 0, Length()),
                             std::clamp(selection.EndCaretPosition, 0, Length())};
      }
      args.Result(TextCore::CoreTextTextUpdatingResult::Succeeded);
    });
    s_state.context.SelectionUpdating(
        [generation](auto&&, const TextCore::CoreTextSelectionUpdatingEventArgs& args) {
          const auto selection = args.Selection();
          if (generation != s_state.generation || selection.StartCaretPosition < 0 ||
              selection.EndCaretPosition > Length() ||
              selection.StartCaretPosition > selection.EndCaretPosition)
          {
            args.Result(TextCore::CoreTextSelectionUpdatingResult::Failed);
            return;
          }
          s_state.selection = selection;
          args.Result(TextCore::CoreTextSelectionUpdatingResult::Succeeded);
        });
    s_state.context.CompositionStarted([](auto&&, auto&&) {});
    s_state.context.CompositionCompleted([](auto&&, auto&&) {});
    s_state.context.FocusRemoved([generation](auto&&, auto&&) {
      // The window lost focus. Showing the keyboard again makes a new context.
      if (generation == s_state.generation)
        s_state.context = nullptr;
    });
    s_state.context.NotifyFocusEnter();
    return true;
  }
  catch (const winrt::hresult_error& error)
  {
    ERROR_LOG_FMT(COMMON, "Text input could not start: {}", winrt::to_string(error.message()));
    s_state.context = nullptr;
    return false;
  }
}
}  // namespace

void Begin(std::string text, bool multiline, bool show_keyboard)
{
  End();
  s_state.active = true;
  s_state.multiline = multiline;
  s_state.text = UTF8ToWString(text);
  s_state.selection = {Length(), Length()};
  s_state.result = Result::None;
  WatchKeyboard();
  CreateContext();
  if (show_keyboard)
    ShowKeyboard();
}

void End()
{
  if (!s_state.active)
    return;
  s_state.active = false;
  ReleaseContext();
  try
  {
    if (s_state.input_view)
      s_state.input_view.TryHide();
    winrt::Windows::UI::ViewManagement::InputPane::GetForCurrentView().TryHide();
  }
  catch (const winrt::hresult_error&)
  {
  }
  s_keyboard_visible = false;
}

bool IsActive()
{
  return s_state.active;
}

bool IsMultiline()
{
  return s_state.multiline;
}

void ShowKeyboard()
{
  if (!s_state.active)
    return;
  if (!s_state.context)
    CreateContext();
  try
  {
    if (s_state.input_view && s_state.input_view.TryShow(ViewCore::CoreInputViewKind::Keyboard))
      return;
  }
  catch (const winrt::hresult_error&)
  {
  }
  try
  {
    winrt::Windows::UI::ViewManagement::InputPane::GetForCurrentView().TryShow();
  }
  catch (const winrt::hresult_error& error)
  {
    ERROR_LOG_FMT(COMMON, "The on-screen keyboard could not be shown: {}",
                  winrt::to_string(error.message()));
  }
}

bool IsKeyboardVisible()
{
  return s_keyboard_visible;
}

std::string GetText()
{
  return WStringToUTF8(s_state.text);
}

size_t GetCaret()
{
  return WStringToUTF8(
             std::wstring_view(s_state.text).substr(0, static_cast<size_t>(SelectionEnd())))
      .size();
}

bool HandleKey(VirtualKey key)
{
  if (!s_state.active)
    return false;
  const s32 start = SelectionStart();
  const s32 end = SelectionEnd();
  switch (key)
  {
  case VirtualKey::Back:
    if (start == end && start > 0)
      s_state.selection = {start - 1, start};
    Replace({});
    return true;
  case VirtualKey::Delete:
    if (start == end && end < Length())
      s_state.selection = {end, end + 1};
    Replace({});
    return true;
  case VirtualKey::Left:
    SetCaret(start == end ? start - 1 : start);
    return true;
  case VirtualKey::Right:
    SetCaret(start == end ? end + 1 : end);
    return true;
  case VirtualKey::Home:
    SetCaret(s_state.multiline ? LineStart(start) : 0);
    return true;
  case VirtualKey::End:
    SetCaret(s_state.multiline ? LineEnd(end) : Length());
    return true;
  case VirtualKey::Up:
  case VirtualKey::Down:
    if (s_state.multiline)
      MoveLine(key == VirtualKey::Down);
    return true;
  case VirtualKey::Tab:
    if (s_state.multiline)
      Replace(L"    ");
    return true;
  case VirtualKey::Enter:
    if (s_state.multiline)
      Replace(L"\n");
    else
      s_state.result = Result::Submitted;
    return true;
  case VirtualKey::Escape:
    s_state.result = s_state.multiline ? Result::Submitted : Result::Cancelled;
    return true;
  default:
    return false;
  }
}

void MoveCaret(int delta)
{
  if (s_state.active)
    SetCaret(SelectionEnd() + delta);
}

void Submit()
{
  if (s_state.active)
    s_state.result = Result::Submitted;
}

void Cancel()
{
  if (s_state.active)
    s_state.result = Result::Cancelled;
}

Result TakeResult()
{
  return std::exchange(s_state.result, Result::None);
}
}  // namespace DolphinUWP::GameTextInput

#pragma warning(pop)
