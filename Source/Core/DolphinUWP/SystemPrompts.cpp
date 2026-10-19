// Copyright 2026 Dolphin Emulator Project
// SPDX-License-Identifier: GPL-2.0-or-later

#include "DolphinUWP/SystemPrompts.h"

#include <chrono>
#include <thread>

#include <winrt/Windows.Foundation.Collections.h>

#include "Common/Logging/Log.h"
#include "DolphinUWP/Translation.h"

namespace DolphinUWP
{
namespace
{
using winrt::Windows::Foundation::AsyncStatus;

constexpr auto MODAL_EVENT_INTERVAL = std::chrono::milliseconds(10);
constexpr int32_t ANSWER_NO = 0;
constexpr int32_t ANSWER_YES = 1;

bool IsYes(const winrt::Windows::UI::Popups::IUICommand& command)
{
  return command && winrt::unbox_value_or<int32_t>(command.Id(), ANSWER_NO) == ANSWER_YES;
}
}  // namespace

SystemPrompts::Operation SystemPrompts::Show(const PromptService::Prompt& prompt)
{
  using winrt::Windows::UI::Popups::MessageDialog;
  using winrt::Windows::UI::Popups::UICommand;
  try
  {
    const auto translate = [](std::string_view text) {
      return winrt::to_hstring(Translation::Translate(text));
    };
    MessageDialog dialog(translate(prompt.text),
                         translate(prompt.caption.empty() ? "Dolphin" : prompt.caption));
    if (prompt.question)
    {
      dialog.Commands().Append(UICommand(translate("Yes"), nullptr, winrt::box_value(ANSWER_YES)));
      dialog.Commands().Append(UICommand(translate("No"), nullptr, winrt::box_value(ANSWER_NO)));
      dialog.DefaultCommandIndex(0);
      dialog.CancelCommandIndex(1);
    }
    else
    {
      dialog.Commands().Append(UICommand(translate("OK"), nullptr, winrt::box_value(ANSWER_YES)));
      dialog.DefaultCommandIndex(0);
      dialog.CancelCommandIndex(0);
    }
    return dialog.ShowAsync();
  }
  catch (const winrt::hresult_error& error)
  {
    ERROR_LOG_FMT(COMMON, "The system dialog could not be shown: {}",
                  winrt::to_string(error.message()));
    return nullptr;
  }
}

void SystemPrompts::Close()
{
  if (m_operation && m_operation.Status() == AsyncStatus::Started)
    m_operation.Cancel();
  m_operation = nullptr;
}

void SystemPrompts::Update()
{
  PromptService& prompts = PromptService::GetInstance();
  const std::optional<PromptService::Prompt> prompt = prompts.GetActivePrompt();
  const u64 id = prompt && prompt->system_dialog ? prompt->id : 0;
  if (id == m_prompt_id)
    return;

  // A prompt that was answered another way, such as by stopping the game, closes its dialog.
  Close();
  m_prompt_id = id;
  if (id == 0)
    return;

  m_operation = Show(*prompt);
  if (!m_operation)
  {
    prompts.Answer(id, prompt->fallback);
    return;
  }
#pragma warning(push)
// C++/WinRT delegate template instantiated with the lambda below.
#pragma warning(disable : 4265)
  m_operation.Completed([id](const Operation& operation, AsyncStatus status) {
    if (status == AsyncStatus::Completed)
      PromptService::GetInstance().Answer(id, IsYes(operation.GetResults()));
  });
#pragma warning(pop)
}

std::optional<bool>
SystemPrompts::AskModal(const winrt::Windows::UI::Core::CoreDispatcher& dispatcher,
                        const PromptService::Prompt& prompt, const std::function<bool()>& cancelled)
{
  // Only one system dialog can be open. A prompt closed here is shown again by the next Update.
  Close();
  m_prompt_id = 0;

  const Operation operation = Show(prompt);
  if (!operation)
    return std::nullopt;

  try
  {
    while (operation.Status() == AsyncStatus::Started && !cancelled())
    {
      dispatcher.ProcessEvents(
          winrt::Windows::UI::Core::CoreProcessEventsOption::ProcessAllIfPresent);
      std::this_thread::sleep_for(MODAL_EVENT_INTERVAL);
    }
  }
  catch (const winrt::hresult_error& error)
  {
    ERROR_LOG_FMT(COMMON, "The system dialog stopped responding: {}",
                  winrt::to_string(error.message()));
  }

  if (operation.Status() != AsyncStatus::Completed)
  {
    if (operation.Status() == AsyncStatus::Started)
      operation.Cancel();
    return std::nullopt;
  }
  return IsYes(operation.GetResults());
}
}  // namespace DolphinUWP
