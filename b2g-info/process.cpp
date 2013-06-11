/*
 * Copyright (C) 2013 Mozilla Foundation
 *
 * Licensed under the Apache License, Version 2.0 (the "License");
 * you may not use this file except in compliance with the License.
 * You may obtain a copy of the License at
 *
 *      http://www.apache.org/licenses/LICENSE-2.0
 *
 * Unless required by applicable law or agreed to in writing, software
 * distributed under the License is distributed on an "AS IS" BASIS,
 * WITHOUT WARRANTIES OR CONDITIONS OF ANY KIND, either express or implied.
 * See the License for the specific language governing permissions and
 * limitations under the License.
 */

#include "process.h"
#include <dirent.h>
#include <errno.h>
#include <fcntl.h>
#include <pwd.h>
#include <sys/stat.h>

using namespace std;

Task::Task(pid_t pid)
  : m_task_id(pid)
  , m_got_stat(false)
  , m_ppid(-1)
  , m_nice(0)
{
  char procdir[128];
  snprintf(procdir, sizeof(procdir), "/proc/%d/", pid);
  m_proc_dir = procdir;
}

Task::Task(pid_t pid, pid_t tid)
  : m_task_id(tid)
  , m_got_stat(false)
  , m_ppid(-1)
  , m_nice(0)
{
  char procdir[128];
  snprintf(procdir, sizeof(procdir), "/proc/%d/task/%d/", pid, tid);
  m_proc_dir = procdir;
}

pid_t
Task::task_id()
{
  return m_task_id;
}

pid_t
Task::ppid()
{
  ensure_got_stat();
  return m_ppid;
}

const string&
Task::name()
{
  ensure_got_stat();
  return m_name;
}

int
Task::nice()
{
  ensure_got_stat();
  return m_nice;
}

void
Task::ensure_got_stat()
{
  if (m_got_stat) {
    return;
  }

  char filename[128];
  snprintf(filename, sizeof(filename), "%s/stat", m_proc_dir.c_str());

  // If anything goes wrong after this point, we still want to say that we read
  // the stat file; there's no use in reading it a second time if we failed
  // once.
  m_got_stat = true;

  FILE* stat_file = fopen(filename, "r");
  if (!stat_file) {
    // We expect ENOENT; that indicates that the file doesn't exist (maybe the
    // process exited or something).  If we get anything else, print a warning
    // to the console.
    if (errno != ENOENT) {
      perror("Unable to open /proc/<pid>/stat");
    }
    return;
  }

  int pid2, ppid;
  char comm[32];
  long int niceness;
  int nread =
    fscanf(stat_file,
           "%d "   // pid
           "%17[^)]) "// comm
           "%*c "  // state
           "%d "   // ppid
           "%*d "  // pgrp
           "%*d "  // session
           "%*d "  // tty_nr
           "%*d "  // tpgid
           "%*u "  // flags
           "%*u "  // minflt (%lu)
           "%*u "  // cminflt (%lu)
           "%*u "  // majflt (%lu)
           "%*u "  // cmajflt (%lu)
           "%*u "  // utime (%lu)
           "%*u "  // stime (%ld)
           "%*d "  // cutime (%ld)
           "%*d "  // cstime (%ld)
           "%*d "  // priority (%ld)
           "%ld ", // niceness
           &pid2, comm, &ppid, &niceness);

  fclose(stat_file);

  if (nread != 4) {
    fprintf(stderr, "Expected to read 4 fields from fscanf(%s), but got %d.\n",
            filename, nread);
    return;
  }

  if (task_id() != pid2) {
    fprintf(stderr, "When reading %s, got pid %d, but expected pid %d.\n",
            filename, pid2, task_id());
    return;
  }

  // Okay, everything worked out.  Store the data we collected.

  m_ppid = ppid;
  m_nice = niceness;

  if (comm[0] != '\0') {
    // If comm is non-empty, it should start with a paren, which we strip off.
    // If it's empty, we don't need to assign anything to m_name.
    m_name = comm + 1;
  }
};

Thread::Thread(pid_t pid, pid_t tid)
  : Task(pid, tid)
  , m_tid(tid)
{}

pid_t
Thread::tid()
{
  return m_tid;
}

Process::Process(pid_t pid)
  : Task(pid)
  , m_pid(pid)
  , m_got_threads(false)
  , m_got_exe(false)
  , m_got_meminfo(false)
  , m_vsize_kb(-1)
  , m_rss_kb(-1)
  , m_pss_kb(-1)
  , m_uss_kb(-1)
{}

pid_t
Process::pid()
{
  return m_pid;
}

const vector<Thread*>&
Process::threads()
{
  if (m_got_threads) {
    return m_threads;
  }

  m_got_threads = true;

  DIR* tasks = opendir((m_proc_dir + "task").c_str());
  if (!tasks) {
    return m_threads;
  }

  dirent *de;
  while ((de = readdir(tasks))) {
    int tid;
    if (str_to_int(de->d_name, &tid) && tid != pid()) {
      m_threads.push_back(new Thread(m_pid, tid));
    }
  }

  closedir(tasks);

  return m_threads;
}

const string&
Process::exe()
{
  if (m_got_exe) {
    return m_exe;
  }

  char filename[128];
  snprintf(filename, sizeof(filename), "/proc/%d/exe", pid());

  char link[128];
  ssize_t link_length = readlink(filename, link, sizeof(link) - 1);
  if (link_length == -1) {
    // Maybe this process doesn't exist anymore, or maybe |exe| is a broken
    // link.  If so, that's OK; just let m_exe be the empty string.
    link[0] = '\0';
  } else {
    link[link_length] = '\0';
  }

  m_exe = link;

  m_got_exe = true;
  return m_exe;
}

int
Process::get_int_file(const char* name)
{
  // TODO: Use a cache?

  char filename[128];
  snprintf(filename, sizeof(filename), "/proc/%d/%s", pid(), name);

  int fd = TEMP_FAILURE_RETRY(open(filename, O_RDONLY));
  if (fd == -1) {
    return -1;
  }

  char buf[32];
  int nread = TEMP_FAILURE_RETRY(read(fd, buf, sizeof(buf) - 1));
  TEMP_FAILURE_RETRY(close(fd));

  if (nread == -1) {
    return -1;
  }

  buf[nread] = '\0';
  return str_to_int(buf, -1);
}

int
Process::oom_score()
{
  return get_int_file("oom_score");
}

int
Process::oom_score_adj()
{
  return get_int_file("oom_score_adj");
}

int
Process::oom_adj()
{
  return get_int_file("oom_adj");
}


void
Process::ensure_got_meminfo()
{
  if (m_got_meminfo) {
    return;
  }

  // If anything goes wrong after this point (e.g. smaps doesn't exist), we
  // still want to say that we got meminfo; there's no point in trying again.
  m_got_meminfo = true;

  // Android has this pm_memusage interface to get the data we collect here.
  // But collecting the data from smaps isn't hard, and doing it this way
  // doesn't rely on any external code, which is nice.
  // 
  // Also, the vsize value I get out of procrank (which uses pm_memusage) is
  // way lower than what I get out of statm (which matches smaps).  I presume
  // that statm is correct here.

  char filename[128];
  snprintf(filename, sizeof(filename), "/proc/%d/smaps", pid());
  FILE *f = fopen(filename, "r");
  if (!f) {
    return;
  }

  m_vsize_kb = m_rss_kb = m_pss_kb = m_uss_kb = 0;

  char line[256];
  while(fgets(line, sizeof(line), f)) {
      int val = 0;
      if (sscanf(line, "Size: %d kB", &val) == 1) {
        m_vsize_kb += val;
      } else if (sscanf(line, "Rss: %d kB", &val) == 1) {
        m_rss_kb += val;
      } else if (sscanf(line, "Pss: %d kB", &val) == 1) {
        m_pss_kb += val;
      } else if (sscanf(line, "Private_Dirty: %d kB", &val) == 1 ||
                 sscanf(line, "Private_Clean: %d kB", &val) == 1) {
        m_uss_kb += val;
      }
  }

  fclose(f);
}

int
Process::vsize_kb()
{
  ensure_got_meminfo();
  return m_vsize_kb;
}

int
Process::rss_kb()
{
  ensure_got_meminfo();
  return m_rss_kb;
}

int
Process::pss_kb()
{
  ensure_got_meminfo();
  return m_pss_kb;
}

int
Process::uss_kb()
{
  ensure_got_meminfo();
  return m_uss_kb;
}

const string&
Process::user()
{
  if (m_user.length()) {
    return m_user;
  }

  char filename[128];
  snprintf(filename, sizeof(filename), "/proc/%d", pid());

  struct stat st;
  if (stat(filename, &st) == -1) {
    m_user = "?";
    return m_user;
  }

  passwd* pw = getpwuid(st.st_uid);
  if (pw) {
    m_user = pw->pw_name;
  } else {
    char uid[32];
    snprintf(uid, sizeof(uid), "%lu", st.st_uid);
    m_user = uid;
  }

  return m_user;
}
