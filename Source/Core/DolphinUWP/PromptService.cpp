// Copyright 2026 Dolphin Emulator Project
// SPDX-License-Identifier: GPL-2.0-or-later

#include "DolphinUWP/PromptService.h"

#include <algorithm>
#include <utility>
#include <vector>

#include "Common/ScopeGuard.h"
#include "Core/Core.h"

namespace DolphinUWP
{
namespace
{
thread_local bool t_waiting_for_answer = false;
}

PromptService& PromptService::GetInstance()
{
  static PromptService instance;
  return instance;
}

bool PromptService::CanAsk() const
{
  if (t_waiting_for_answer)
    return false;
  return std::this_thread::get_id() != m_ui_thread || m_modal_handler;
}

bool PromptService::Ask(std::string caption, std::string text, Common::MsgType style, bool fallback)
{
  if (!CanAsk())
    return fallback;

  t_waiting_for_answer = true;
  Common::ScopeGuard waiting_guard([] { t_waiting_for_answer = false; });

  if (std::this_thread::get_id() == m_ui_thread)
  {
    const Prompt prompt{.caption = std::move(caption),
                        .text = std::move(text),
                        .style = style,
                        .fallback = fallback,
                        .system_dialog = true};
    return m_modal_handler(prompt).value_or(fallback);
  }

  // The video thread draws the overlay, and without a video thread of its own the CPU thread
  // draws it too, so they wait for an answer from the system dialog instead.
  const bool cpu_thread = Core::IsCPUThread();
  const bool video_thread = Core::IsGPUThread();
  const bool overlay = !video_thread && (!cpu_thread || m_emulation_prompts_allowed);

  std::unique_lock lock(m_mutex);
  if (m_shutdown)
    return fallback;

  const u64 id = m_next_id++;
  m_pending.push_back({.id = id,
                       .caption = std::move(caption),
                       .text = std::move(text),
                       .style = style,
                       .fallback = fallback,
                       .from_cpu_thread = cpu_thread && overlay,
                       .system_dialog = !overlay,
                       .blocks_emulation = cpu_thread || video_thread});
  m_answered.wait(lock, [&] { return m_shutdown || m_answers.contains(id); });

  const auto answer = m_answers.find(id);
  if (answer == m_answers.end())
    return fallback;
  const bool result = answer->second;
  m_answers.erase(answer);
  return result;
}

void PromptService::Inform(std::string caption, std::string text, Common::MsgType style)
{
  std::lock_guard lock(m_mutex);
  if (m_shutdown)
    return;
  m_pending.push_back({.id = m_next_id++,
                       .caption = std::move(caption),
                       .text = std::move(text),
                       .style = style,
                       .question = false});
}

void PromptService::Confirm(std::string caption, std::string text,
                            std::function<void(bool)> callback)
{
  std::lock_guard lock(m_mutex);
  if (m_shutdown)
    return;
  m_pending.push_back({.id = m_next_id++,
                       .caption = std::move(caption),
                       .text = std::move(text),
                       .style = Common::MsgType::Question,
                       .callback = std::move(callback)});
}

std::optional<PromptService::Prompt> PromptService::GetActivePrompt() const
{
  std::lock_guard lock(m_mutex);
  if (m_pending.empty())
    return std::nullopt;
  return m_pending.front();
}

void PromptService::Answer(u64 id, bool yes)
{
  std::function<void(bool)> callback;
  {
    std::lock_guard lock(m_mutex);
    const auto found =
        std::ranges::find(m_pending, id, [](const Prompt& prompt) { return prompt.id; });
    if (found == m_pending.end())
      return;
    callback = std::move(found->callback);
    if (found->question && !callback)
      m_answers[id] = yes;
    m_pending.erase(found);
  }
  if (callback)
    callback(yes);
  m_answered.notify_all();
}

void PromptService::AnswerAll(bool yes)
{
  std::vector<std::function<void(bool)>> callbacks;
  {
    std::lock_guard lock(m_mutex);
    for (Prompt& prompt : m_pending)
    {
      if (prompt.callback)
        callbacks.push_back(std::move(prompt.callback));
      else if (prompt.question)
        m_answers[prompt.id] = yes;
    }
    m_pending.clear();
  }
  for (const auto& callback : callbacks)
    callback(yes);
  m_answered.notify_all();
}

void PromptService::Shutdown()
{
  {
    std::lock_guard lock(m_mutex);
    m_shutdown = true;
    m_pending.clear();
  }
  m_answered.notify_all();
}
}  // namespace DolphinUWP
