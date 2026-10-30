// Copyright 2026 The Wolang Authors. Licensed under the Apache
// License, Version 2.0; see LICENSE.
//
// Native dialogs on Linux, by running `zenity` or `kdialog` (D247).
//
// A toolkit linked in would be a dependency to ship for a message box,
// and these two are on nearly every desktop. The program is run with
// fork and exec rather than a shell: a title or a path is only ever an
// argument, never something a shell can read. With neither installed
// every call says so and the program decides what to do.

#include <cstddef>
#include <cstdint>
#include <cstdlib>
#include <fcntl.h>
#include <string>
#include <sys/wait.h>
#include <unistd.h>
#include <vector>

#include "platform/common.h"

namespace {

enum class Tool { kNone, kZenity, kKdialog };

bool OnPath(const char* program) {
  const char* path = std::getenv("PATH");
  if (path == nullptr) {
    return false;
  }
  std::string rest = path;
  size_t start = 0;
  while (start <= rest.size()) {
    size_t end = rest.find(':', start);
    if (end == std::string::npos) {
      end = rest.size();
    }
    const std::string candidate =
        rest.substr(start, end - start) + "/" + program;
    if (::access(candidate.c_str(), X_OK) == 0) {
      return true;
    }
    start = end + 1;
  }
  return false;
}

Tool Pick() {
  static Tool tool = OnPath("zenity")    ? Tool::kZenity
                     : OnPath("kdialog") ? Tool::kKdialog
                                         : Tool::kNone;
  return tool;
}

// Runs |args| and collects its standard output. Answers the exit status,
// or -1 when it could not be run at all.
int Run(const std::vector<std::string>& args, std::string* out) {
  int fds[2];
  if (::pipe(fds) != 0) {
    return -1;
  }
  const pid_t child = ::fork();
  if (child < 0) {
    ::close(fds[0]);
    ::close(fds[1]);
    return -1;
  }
  if (child == 0) {
    ::dup2(fds[1], 1);
    ::close(fds[0]);
    ::close(fds[1]);
    const int null = ::open("/dev/null", O_RDWR);
    if (null >= 0) {
      ::dup2(null, 0);
      ::dup2(null, 2);
    }
    std::vector<char*> argv;
    for (const std::string& arg : args) {
      argv.push_back(const_cast<char*>(arg.c_str()));
    }
    argv.push_back(nullptr);
    ::execvp(argv[0], argv.data());
    ::_exit(127);
  }
  ::close(fds[1]);
  char buffer[512];
  for (;;) {
    const ssize_t got = ::read(fds[0], buffer, sizeof(buffer));
    if (got <= 0) {
      break;
    }
    if (out != nullptr) {
      out->append(buffer, static_cast<size_t>(got));
    }
  }
  ::close(fds[0]);
  int status = 0;
  if (::waitpid(child, &status, 0) < 0 || !WIFEXITED(status)) {
    return -1;
  }
  const int code = WEXITSTATUS(status);
  return code == 127 ? -1 : code;
}

std::string& Held() {
  static std::string held;
  return held;
}

const char* PathFrom(std::string out) {
  while (!out.empty() && (out.back() == '\n' || out.back() == '\r')) {
    out.pop_back();
  }
  Held() = out;
  return Held().empty() ? nullptr : Held().c_str();
}

std::string Text(const char* text) { return text != nullptr ? text : ""; }

}  // namespace

extern "C" int32_t wolang_dialog_available(void) {
  return Pick() != Tool::kNone ? 1 : 0;
}

extern "C" int32_t wolang_dialog_message(const char* title, const char* text,
                                         int32_t level, int32_t buttons) {
  const Tool tool = Pick();
  if (tool == Tool::kNone) {
    return kWolangAnswerUnavailable;
  }
  std::vector<std::string> args;
  if (tool == Tool::kZenity) {
    args = {"zenity"};
    if (buttons == kWolangButtonsOk) {
      args.push_back(level == kWolangDialogError     ? "--error"
                     : level == kWolangDialogWarning ? "--warning"
                                                     : "--info");
    } else {
      args.push_back("--question");
      args.push_back(level == kWolangDialogError ? "--icon-name=dialog-error"
                     : level == kWolangDialogWarning
                         ? "--icon-name=dialog-warning"
                         : "--icon-name=dialog-information");
      const bool yes_no = buttons == kWolangButtonsYesNo;
      args.push_back(yes_no ? "--ok-label=Yes" : "--ok-label=OK");
      args.push_back(yes_no ? "--cancel-label=No" : "--cancel-label=Cancel");
    }
    args.push_back("--title=" + Text(title));
    args.push_back("--text=" + Text(text));
    args.push_back("--no-markup");
  } else {
    args = {"kdialog"};
    if (buttons == kWolangButtonsOk) {
      args.push_back(level == kWolangDialogError     ? "--error"
                     : level == kWolangDialogWarning ? "--sorry"
                                                     : "--msgbox");
    } else {
      args.push_back(level == kWolangDialogInfo ? "--yesno"
                                                : "--warningyesno");
      if (buttons == kWolangButtonsOkCancel) {
        args.push_back("--yes-label");
        args.push_back("OK");
        args.push_back("--no-label");
        args.push_back("Cancel");
      }
    }
    args.push_back(Text(text));
    args.push_back("--title");
    args.push_back(Text(title));
  }
  const int status = Run(args, nullptr);
  if (status < 0) {
    return kWolangAnswerUnavailable;
  }
  if (buttons == kWolangButtonsOk) {
    return kWolangAnswerOk;
  }
  const bool yes = status == 0;
  if (buttons == kWolangButtonsYesNo) {
    return yes ? kWolangAnswerYes : kWolangAnswerNo;
  }
  return yes ? kWolangAnswerOk : kWolangAnswerCancel;
}

extern "C" const char* wolang_dialog_open_file(const char* title) {
  const Tool tool = Pick();
  if (tool == Tool::kNone) {
    return nullptr;
  }
  std::vector<std::string> args;
  if (tool == Tool::kZenity) {
    args = {"zenity", "--file-selection", "--title=" + Text(title)};
  } else {
    args = {"kdialog", "--getopenfilename", ".", "--title", Text(title)};
  }
  std::string out;
  if (Run(args, &out) != 0) {
    return nullptr;
  }
  return PathFrom(out);
}

extern "C" const char* wolang_dialog_save_file(const char* title,
                                               const char* start_name) {
  const Tool tool = Pick();
  if (tool == Tool::kNone) {
    return nullptr;
  }
  const std::string name = Text(start_name);
  std::vector<std::string> args;
  if (tool == Tool::kZenity) {
    args = {"zenity", "--file-selection", "--save", "--confirm-overwrite",
            "--title=" + Text(title)};
    if (!name.empty()) {
      args.push_back("--filename=" + name);
    }
  } else {
    args = {"kdialog", "--getsavefilename", name.empty() ? "." : name,
            "--title", Text(title)};
  }
  std::string out;
  if (Run(args, &out) != 0) {
    return nullptr;
  }
  return PathFrom(out);
}
