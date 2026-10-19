// Copyright 2026 Dolphin Emulator Project
// SPDX-License-Identifier: GPL-2.0-or-later

#pragma once

#include <atomic>
#include <condition_variable>
#include <deque>
#include <functional>
#include <mutex>
#include <optional>
#include <string>
#include <thread>
#include <unordered_map>

#include "Common/CommonTypes.h"
#include "Common/MsgHandler.h"

namespace DolphinUWP
{
// Answers Dolphin's yes/no alerts with a dialog drawn by the shell or the in-game overlay.
class PromptService
{
public:
  struct Prompt
  {
    u64 id = 0;
    std::string caption;
    std::string text;
    Common::MsgType style = Common::MsgType::Information;
    bool question = true;
    bool fallback = true;
    std::function<void(bool)> callback;
    // The emulated CPU waits for the answer, so nothing else presents frames.
    bool from_cpu_thread = false;
    // The thread that would draw the prompt waits for the answer, so the system dialog shows it.
    bool system_dialog = false;
    // An emulation thread waits for the answer, so the core cannot pause until it is answered.
    bool blocks_emulation = false;
  };

  // Shows a prompt raised by the UI thread and returns the answer, if there is one.
  using ModalHandler = std::function<std::optional<bool>(const Prompt&)>;

  static PromptService& GetInstance();

  void SetUIThread(std::thread::id id) { m_ui_thread = id; }
  void SetModalHandler(ModalHandler handler) { m_modal_handler = std::move(handler); }

  // False while the calling thread already waits for an answer.
  bool CanAsk() const;

  // Blocks until the prompt is answered, or returns the fallback during shutdown.
  bool Ask(std::string caption, std::string text, Common::MsgType style, bool fallback);

  // Queues a message that is dismissed with OK. Callable from any thread.
  void Inform(std::string caption, std::string text,
              Common::MsgType style = Common::MsgType::Information);

  // Queues a yes/no question without blocking. The callback runs on the thread that answers.
  void Confirm(std::string caption, std::string text, std::function<void(bool)> callback);

  // Whether the overlay can present questions from the CPU thread, which needs a running video
  // thread of its own.
  void SetEmulationPromptsAllowed(bool allowed) { m_emulation_prompts_allowed = allowed; }

  std::optional<Prompt> GetActivePrompt() const;
  void Answer(u64 id, bool yes);
  // Answers every waiting question, so blocked threads can finish.
  void AnswerAll(bool yes);
  void Shutdown();

private:
  mutable std::mutex m_mutex;
  std::condition_variable m_answered;
  std::deque<Prompt> m_pending;
  std::unordered_map<u64, bool> m_answers;
  u64 m_next_id = 1;
  bool m_shutdown = false;
  std::thread::id m_ui_thread;
  ModalHandler m_modal_handler;
  std::atomic<bool> m_emulation_prompts_allowed = false;
};
}  // namespace DolphinUWP
