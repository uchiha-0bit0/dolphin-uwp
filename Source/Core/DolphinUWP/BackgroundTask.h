// Copyright 2026 Dolphin Emulator Project
// SPDX-License-Identifier: GPL-2.0-or-later

#pragma once

#include <atomic>
#include <functional>
#include <mutex>
#include <optional>
#include <string>
#include <thread>
#include <vector>

namespace DolphinUWP
{
// Runs a blocking operation off the UI thread while the shell shows its progress.
class BackgroundTask
{
public:
  class Context
  {
  public:
    explicit Context(BackgroundTask& task) : m_task(task) {}

    // A negative fraction shows an indeterminate state.
    void SetProgress(float fraction, std::string detail = {});
    bool IsCancelled() const { return m_task.m_cancelled; }

    // Runs work on the UI thread during the next Tick.
    void Post(std::function<void()> work);

  private:
    BackgroundTask& m_task;
  };

  struct Snapshot
  {
    std::string title;
    std::string detail;
    float fraction = -1.0f;
    bool cancellable = false;
    bool cancelled = false;
  };

  // The job returns a completion that runs on the UI thread.
  using Completion = std::function<void()>;
  using Job = std::function<Completion(Context& context)>;

  BackgroundTask() = default;
  ~BackgroundTask();

  BackgroundTask(const BackgroundTask&) = delete;
  BackgroundTask& operator=(const BackgroundTask&) = delete;

  bool Start(std::string title, bool cancellable, Job job);
  void Cancel() { m_cancelled = true; }
  bool IsRunning() const { return m_running; }
  std::optional<Snapshot> GetSnapshot() const;

  // Call on the UI thread to run the completion of a finished job.
  void Tick();

private:
  mutable std::mutex m_mutex;
  Snapshot m_snapshot;
  Completion m_completion;
  std::vector<std::function<void()>> m_posted;
  std::thread m_thread;
  std::atomic<bool> m_running = false;
  std::atomic<bool> m_finished = false;
  std::atomic<bool> m_cancelled = false;
};
}  // namespace DolphinUWP
