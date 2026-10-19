// Copyright 2026 Dolphin Emulator Project
// SPDX-License-Identifier: GPL-2.0-or-later

#include "DolphinUWP/BackgroundTask.h"

#include <utility>

#include "Common/Thread.h"

namespace DolphinUWP
{
void BackgroundTask::Context::SetProgress(float fraction, std::string detail)
{
  std::lock_guard lock(m_task.m_mutex);
  m_task.m_snapshot.fraction = fraction;
  m_task.m_snapshot.detail = std::move(detail);
}

void BackgroundTask::Context::Post(std::function<void()> work)
{
  std::lock_guard lock(m_task.m_mutex);
  m_task.m_posted.push_back(std::move(work));
}

BackgroundTask::~BackgroundTask()
{
  m_cancelled = true;
  if (m_thread.joinable())
    m_thread.join();
}

bool BackgroundTask::Start(std::string title, bool cancellable, Job job)
{
  if (m_running)
    return false;
  if (m_thread.joinable())
    m_thread.join();

  {
    std::lock_guard lock(m_mutex);
    m_snapshot = {.title = std::move(title), .cancellable = cancellable};
    m_completion = {};
  }
  m_cancelled = false;
  m_finished = false;
  m_running = true;

  m_thread = std::thread([this, job = std::move(job)] {
    Common::SetCurrentThreadName("Background Task");
    Context context(*this);
    Completion completion = job(context);
    {
      std::lock_guard lock(m_mutex);
      m_completion = std::move(completion);
    }
    m_finished = true;
  });
  return true;
}

std::optional<BackgroundTask::Snapshot> BackgroundTask::GetSnapshot() const
{
  if (!m_running)
    return std::nullopt;
  std::lock_guard lock(m_mutex);
  Snapshot snapshot = m_snapshot;
  snapshot.cancelled = m_cancelled;
  return snapshot;
}

void BackgroundTask::Tick()
{
  std::vector<std::function<void()>> posted;
  {
    std::lock_guard lock(m_mutex);
    posted.swap(m_posted);
  }
  for (const auto& work : posted)
    work();

  if (!m_running || !m_finished)
    return;

  m_thread.join();
  Completion completion;
  {
    std::lock_guard lock(m_mutex);
    completion = std::move(m_completion);
  }
  m_running = false;
  if (completion)
    completion();
}
}  // namespace DolphinUWP
