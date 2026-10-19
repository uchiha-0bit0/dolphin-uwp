// Copyright 2026 Dolphin Emulator Project
// SPDX-License-Identifier: GPL-2.0-or-later

#pragma once

#include <functional>
#include <optional>

#include <winrt/Windows.Foundation.h>
#include <winrt/Windows.UI.Core.h>
#include <winrt/Windows.UI.Popups.h>

#include "Common/CommonTypes.h"
#include "DolphinUWP/PromptService.h"

namespace DolphinUWP
{
// Shows prompts with the system dialog while the interface that would draw them waits for the
// answer. Only the UI thread may use it.
class SystemPrompts
{
public:
  // Shows the active prompt when it needs the system dialog.
  void Update();

  // Shows a prompt raised by the UI thread and processes window events until it is answered.
  std::optional<bool> AskModal(const winrt::Windows::UI::Core::CoreDispatcher& dispatcher,
                               const PromptService::Prompt& prompt,
                               const std::function<bool()>& cancelled);

private:
  using Operation =
      winrt::Windows::Foundation::IAsyncOperation<winrt::Windows::UI::Popups::IUICommand>;

  static Operation Show(const PromptService::Prompt& prompt);
  void Close();

  Operation m_operation{nullptr};
  u64 m_prompt_id = 0;
};
}  // namespace DolphinUWP
