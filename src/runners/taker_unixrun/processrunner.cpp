/*
 * Copyright (C) 2018-2019  Alexander Kernozhitsky <sh200105@mail.ru>
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
 * along with this program.  If not, see <http://www.gnu.org/licenses/>.
 */

#include "processrunner.hpp"
#include <errno.h>
#include <fcntl.h>
#include <signal.h>
#include <sys/resource.h>
#include <sys/types.h>
#include <sys/wait.h>
#include <unistd.h>
#include <cassert>
#include <cmath>
#include <cstring>
#include <fstream>
#include <iostream>
#include <map>
#include <sstream>
#include "config.hpp"
#include "utils.hpp"

#ifndef _GNU_SOURCE
extern char **environ;
#endif

namespace UnixRunner {

pid_t g_activeChild = 0;

void termSignal(int) {
  if (g_activeChild != 0) {
    kill(g_activeChild, SIGKILL);
  }
  kill(0, SIGKILL);
}

class ActiveChildLock {
 private:
  struct sigaction oldActions_[3];

 public:
  ActiveChildLock(pid_t pid) {
    if (g_activeChild != 0) {
      throw std::runtime_error("active child already set");
    }
    g_activeChild = pid;
    struct sigaction sigHandler;
    zeroMem(sigHandler);
    for (int i = 0; i < 3; ++i) {
      zeroMem(oldActions_[i]);
    }
    sigHandler.sa_handler = termSignal;
    sigaction(SIGINT, &sigHandler, &oldActions_[0]);
    sigaction(SIGTERM, &sigHandler, &oldActions_[1]);
    sigaction(SIGQUIT, &sigHandler, &oldActions_[2]);
  }

  ~ActiveChildLock() {
    g_activeChild = 0;
    sigaction(SIGINT, &oldActions_[0], nullptr);
    sigaction(SIGTERM, &oldActions_[1], nullptr);
    sigaction(SIGQUIT, &oldActions_[2], nullptr);
  }
};

RunnerError::RunnerError(const std::string &comment)
    : std::runtime_error(comment) {}

RunnerValidateError::RunnerValidateError(const std::string &comment)
    : RunnerError(comment) {}

#define VALIDATE_ASSERT(cond) \
  if (!(cond)) throw RunnerValidateError("assertion failed: " #cond);

void ProcessRunner::Parameters::validate() {
  VALIDATE_ASSERT(workingDir.empty() || directoryIsGood(workingDir));

  DirectoryChanger changer(workingDir);

  VALIDATE_ASSERT(timeLimit > 0);
  VALIDATE_ASSERT(idleLimit > 0);
  VALIDATE_ASSERT(memoryLimit > 0);
  VALIDATE_ASSERT(fileIsExecutable(executable));
  VALIDATE_ASSERT(stdinRedir.empty() || fileIsReadable(stdinRedir));
}

#undef VALIDATE_ASSERT

void ProcessRunner::Parameters::loadFromJson(const Json::Value &value) {
  using Json::Value;
  timeLimit = value.get("time-limit", Value(timeLimit)).asDouble();
  idleLimit = value.get("idle-limit", Value(timeLimit * 3.5)).asDouble();
  memoryLimit = value.get("memory-limit", Value(memoryLimit)).asDouble();
  executable = value.get("executable", Value("")).asString();
  clearEnv = value.get("clear-env", Value(clearEnv)).asBool();
  if (value.isMember("env")) {
    auto envNode = value["env"];
    if (!envNode.isObject()) {
      throw std::runtime_error("env is not an object");
    }
    env.clear();
    for (const std::string &name : envNode.getMemberNames()) {
      Json::Value strValue = envNode[name];
      if (strValue.isConvertibleTo(Json::stringValue)) {
        env[name] = strValue.asString();
      }
    }
  }
  if (value.isMember("args")) {
    auto argNode = value["args"];
    if (!argNode.isArray()) {
      throw std::runtime_error("args is not an array");
    }
    args.resize(argNode.size());
    for (Json::ArrayIndex i = 0; i < argNode.size(); ++i) {
      args[i] = argNode[i].asString();
    }
  } else {
    args.clear();
  }
  workingDir = value.get("working-dir", Value("")).asString();
  stdinRedir = value.get("stdin-redir", Value("")).asString();
  stdoutRedir = value.get("stdout-redir", Value("")).asString();
  stderrRedir = value.get("stderr-redir", Value("")).asString();
  isolateDir = value.get("isolate-dir", Value("")).asString();
  isolatePolicy = strToIsolatePolicy(
      value.get("isolate-policy", Value("normal")).asString());
}

void ProcessRunner::Parameters::loadFromJsonStr(const std::string &json) {
  std::istringstream stream(json);
  Json::Value value;
  stream >> value;
  return loadFromJson(value);
}

const char *ProcessRunner::runStatusToStr(ProcessRunner::RunStatus status) {
  static const char *RUN_STATUS_STRS[] = {
      "ok",           "time-limit",    "idle-limit",
      "memory-limit", "runtime-error", "security-error",
      "run-fail",     "running",       "none"};
  return RUN_STATUS_STRS[static_cast<int>(status)];
}

ProcessRunner::IsolatePolicy ProcessRunner::strToIsolatePolicy(
    const std::string &value) {
  static const char *ISOLATE_POLICY_STRS[] = {"none", "normal", "compile",
                                              "strict"};
  int strCount = sizeof(ISOLATE_POLICY_STRS) / sizeof(ISOLATE_POLICY_STRS[0]);
  for (int i = 0; i < strCount; ++i) {
    if (ISOLATE_POLICY_STRS[i] == value) {
      return static_cast<IsolatePolicy>(i);
    }
  }
  throw RunnerValidateError(value + " is invalid isolate-policy");
}

Json::Value ProcessRunner::RunResults::saveToJson() const {
  Json::Value value;
  value["time"] = time;
  value["clock-time"] = clockTime;
  value["memory"] = memory;
  value["exitcode"] = exitCode;
  value["signal"] = signal;
  if (signal != 0) {
    char *signalName = strsignal(signal);
    value["signal-name"] = (signalName == nullptr) ? "unknown" : signalName;
  }
  value["status"] = ProcessRunner::runStatusToStr(status);
  value["comment"] = comment;
  return value;
}

std::string ProcessRunner::RunResults::saveToJsonStr() const {
  return saveToJson().toStyledString();
}

Json::Value ProcessRunner::runnerInfoJson() const {
  Json::Value res;
  res["name"] = "Taker UNIX Runner";
  res["description"] =
      "A simple runner for UNIX-like systems (like GNU/Linux, macOS and "
      "FreeBSD)";
  res["author"] = "Alexander Kernozhitsky";
  res["version"] = TAKER_UNIXRUN_VERSION;
  res["version-number"] = TAKER_UNIXRUN_VERSION_NUMBER;
  res["license"] = "GPL-3+";
  res["features"] = Json::Value(Json::arrayValue);
  return res;
}

ProcessRunner::Parameters &ProcessRunner::parameters() { return parameters_; }

const ProcessRunner::Parameters &ProcessRunner::parameters() const {
  return parameters_;
}

const ProcessRunner::RunResults &ProcessRunner::results() const {
  return results_;
}

void ProcessRunner::execute() {
  if (results_.status == RunStatus::RUNNING) {
    throw std::runtime_error("process is already running");
  }
  try {
    doExecute();
  } catch (const std::exception &e) {
    results_.status = RunStatus::RUN_FAIL;
    results_.comment = getFullExceptionMessage(e);
  }
}

void ProcessRunner::doExecute() {
  parameters_.validate();
  results_ = RunResults();
  results_.status = RunStatus::RUNNING;
#ifdef HAVE_PIPE2
  if (pipe2(pipe_, O_CLOEXEC) != 0) {
#else
  // empty comments prevent clang-format from removing line breaks
  if (pipe(pipe_) != 0 ||                           //
      fcntl(pipe_[0], F_SETFD, FD_CLOEXEC) != 0 ||  //
      fcntl(pipe_[1], F_SETFD, FD_CLOEXEC) != 0) {
#endif
    throw RunnerError(getFullErrorMessage("unable to create pipe", errno));
  }
  pid_ = fork();
  if (pid_ < 0) {
    int errCode = errno;
    close(pipe_[0]);
    close(pipe_[1]);
    throw RunnerError(getFullErrorMessage("unable to fork()", errCode));
  }
  if (pid_ == 0) {
    close(pipe_[0]);
    try {
      handleChild();
    } catch (const std::exception &e) {
      childFailure(getFullExceptionMessage(e));
    }
  }
  ActiveChildLock lock(pid_);
  close(pipe_[1]);
  handleParent();
}

void ProcessRunner::handleParent() {
  FileDescriptorOwner fdOwner(pipe_[0]);
  timer_.start();

  // check for RUN_FAIL
  int msgSize;
  int bytesRead = read(pipe_[0], &msgSize, sizeof(msgSize));
  if (bytesRead < 0) {
    parentFailure("unable to read from pipe", errno);
  }
  if (bytesRead > 0) {
    if (bytesRead != sizeof(msgSize)) {
      parentFailure("unexpected child/parent protocol error");
    }
    char *message = new char[msgSize + 1];
    message[msgSize] = 0;
    int bytesExpected = sizeof(char) * msgSize;
    trySyscall(read(pipe_[0], message, bytesExpected) == bytesExpected,
               "unexpected child/parent protocol error (message length"
               " must be " +
                   std::to_string(bytesExpected) + ", not " +
                   std::to_string(bytesRead) + ")");
    results_.status = RunStatus::RUN_FAIL;
    results_.comment = message;
    delete[] message;
    waitpid(pid_, nullptr, 0);
    return;
  }

  // initialize results
  results_.exitCode = results_.signal = 0;
  results_.time = results_.clockTime = 0.0;
  results_.memory = 0.0;
  results_.status = RunStatus::RUNNING;

  // wait for process
  while (results_.status == RunStatus::RUNNING) {
    // check for time and memory limits
    updateResultsOnRun();
    updateVerdicts();
    if (results_.status != RunStatus::RUNNING) {
      kill(pid_, SIGKILL);
      trySyscall(waitpid(pid_, nullptr, 0) >= 0, "unable to wait for process");
      break;
    }
    // check if the process has terminated
    int status = -1;
    struct rusage resources;
    zeroMem(resources);
    int pidWaited = wait4(pid_, &status, WNOHANG | WUNTRACED, &resources);
    if (pidWaited == -1) {
      // wait4() error
      int errCode = errno;
      kill(pid_, SIGKILL);
      parentFailure("unable to wait for process", errCode);
    }
    if (pidWaited != 0) {
      // the process has changed the state
      // FIXME : handle stopped/continued processes
      updateResultsOnTerminate(resources, status);
      if (results_.status == RunStatus::RUNNING) {
        kill(pid_, SIGKILL);
        waitpid(pid_, nullptr, 0);
        parentFailure(
            "unexpected process status: waitpid() returned, "
            "but the process is still alive (status = " +
            std::to_string(status) + ")");
      }
      updateVerdicts();
      break;
    }
    // wait a little
    usleep(1'000);
  }
}

#ifdef __linux__

bool ProcessRunner::updateTimeFromProcStat() {
  std::string procFileName = "/proc/" + std::to_string(pid_) + "/stat";
  std::ifstream procStats(procFileName);
  if (!procStats.good()) {
    return false;
  }
  std::string procStr;
  if (!std::getline(procStats, procStr)) {
    return false;
  }
  auto bracketPos = procStr.rfind(')');
  if (bracketPos == std::string::npos || bracketPos == procStr.size()) {
    return false;
  }
  procStr.erase(0, bracketPos + 1);
  std::istringstream tokenizer(procStr);
  unsigned long utime, stime;
  std::string unused;
  for (int field = 3; field <= 42; ++field) {
    if (field == 14) {
      tokenizer >> utime;
    } else if (field == 15) {
      tokenizer >> stime;
    } else {
      tokenizer >> unused;
    }
    if (!tokenizer.good()) {
      return false;
    }
  }
  results_.time = 1.0 * (utime + stime) / sysconf(_SC_CLK_TCK);
  return true;
}

bool ProcessRunner::updateMemFromProcStatus() {
  const std::map<std::string, double> multipliers = {
      {"kB", 1.0 / 1024}, {"KB", 1.0 / 1024}, {"kb", 1.0 / 1024}, {"MB", 1.0},
      {"mb", 1.0},        {"GB", 1024.0},     {"gb", 1024.0}};
  std::string procFileName = "/proc/" + std::to_string(pid_) + "/status";
  std::ifstream procStatus(procFileName);
  if (!procStatus.good()) {
    return false;
  }
  std::string fieldName;
  while (procStatus >> fieldName) {
    if (fieldName == "VmPeak:") {
      int64_t value;
      std::string measureUnit;
      if (!(procStatus >> value >> measureUnit)) {
        return false;
      }
      if (multipliers.find(measureUnit) == end(multipliers)) {
        return false;
      }
      results_.memory =
          std::max(results_.memory, value * multipliers.at(measureUnit));
      return true;
    } else {
      std::string unused;
      if (!std::getline(procStatus, unused)) {
        return false;
      }
    }
  }
  return false;
}

#endif  // __linux__

void ProcessRunner::updateResultsOnRun() {
#ifdef __linux__
  updateTimeFromProcStat();
  updateMemFromProcStatus();
#endif
  results_.clockTime = timer_.getTime();
}

void ProcessRunner::updateVerdicts() {
  if (results_.time > parameters_.timeLimit) {
    results_.status = RunStatus::TIME_LIMIT;
  }
  if (results_.clockTime > parameters_.idleLimit) {
    results_.status = RunStatus::IDLE_LIMIT;
  }
  if (results_.memory > parameters_.memoryLimit) {
    results_.status = RunStatus::MEMORY_LIMIT;
  }
}

void ProcessRunner::updateResultsOnTerminate(const struct rusage &resources,
                                             int status) {
  if (WIFEXITED(status)) {
    results_.exitCode = WEXITSTATUS(status);
    if (results_.exitCode == 0) {
      results_.status = RunStatus::OK;
    } else {
      results_.status = RunStatus::RUNTIME_ERROR;
    }
  }
  if (WIFSIGNALED(status)) {
    results_.signal = WTERMSIG(status);
    results_.status = RunStatus::RUNTIME_ERROR;
  }
  results_.time =
      timevalToDouble(timeSum(resources.ru_stime, resources.ru_utime));
  results_.clockTime = timer_.getTime();
  if (results_.memory == 0) {
    // FIXME : if the memory usage wasn't updated, maybe use smth better than
    // maxrss?
    results_.comment = "memory measurement is not precise!";
    results_.memory = resources.ru_maxrss / 1048576.0 * maxRssBytes;
  }
}

void ProcessRunner::trySyscall(bool success, const std::string &errorName) {
  if (success) {
    return;
  }
  if (pid_ == 0) {
    childFailure(errorName, errno);
  } else {
    parentFailure(errorName, errno);
  }
}

void ProcessRunner::handleChild() {
  setsid();

  trySyscall(updateLimit(RLIMIT_CORE, 0), "could not disable core dumps");

  // FIXME : avoid overflow when handling very large time and memory limits

  int64_t integralTimeLimit =
      static_cast<int64_t>(ceil(parameters_.timeLimit + 0.2));
  trySyscall(updateLimit(RLIMIT_CPU, integralTimeLimit),
             "could not set time limit");

  // FIXME : distinguish between RE and ML better

  int64_t memLimitBytes =
      static_cast<int64_t>(ceil(parameters_.memoryLimit * 1048576));
  trySyscall(updateLimit(RLIMIT_AS, memLimitBytes * 2),
             "could not set memory limit");
  trySyscall(updateLimit(RLIMIT_DATA, memLimitBytes * 2),
             "could not set memory limit");
  trySyscall(updateLimit(RLIMIT_STACK, memLimitBytes * 2),
             "could not set memory limit");

  if (!parameters_.workingDir.empty()) {
    trySyscall(chdir(parameters_.workingDir.c_str()) == 0,
               "could not change directory");
  }

  trySyscall(
      redirectDescriptor(STDIN_FILENO, parameters_.stdinRedir, O_RDONLY),
      "unable to redirect stdin into \"" + parameters_.stdinRedir + "\"");
  trySyscall(
      redirectDescriptor(STDOUT_FILENO, parameters_.stdoutRedir,
                         O_CREAT | O_TRUNC | O_WRONLY),
      "unable to redirect stdout into \"" + parameters_.stdoutRedir + "\"");
  trySyscall(
      redirectDescriptor(STDERR_FILENO, parameters_.stderrRedir,
                         O_CREAT | O_TRUNC | O_WRONLY),
      "unable to redirect stderr into \"" + parameters_.stderrRedir + "\"");

  if (parameters_.clearEnv) {
#ifdef HAVE_CLEARENV
    trySyscall(clearenv() == 0, "could not clear environment");
#else
    // I didn't expect clearing the environment is a difficult task :(
    // For example, environ = nullptr doesn't work on macOS. I implemented the
    // solution found here:
    // https://lists.freebsd.org/pipermail/freebsd-stable/2008-June/043136.html
    environ = new char *[1];
    environ[0] = nullptr;
#endif
  }
  for (const auto &iter : parameters_.env) {
    const std::string &key = iter.first;
    const std::string &value = iter.second;
    trySyscall(setenv(key.c_str(), value.c_str(), true) == 0,
               "could not set environment \"" + key + "\"");
  }

  size_t argc = parameters_.args.size() + 1;
  char **argv = new char *[argc + 1];
  argv[0] = strdup(parameters_.executable.c_str());
  for (size_t i = 0; i + 1 < argc; ++i) {
    const std::string &argument = parameters_.args[i];
    argv[i + 1] = strdup(argument.c_str());
  }
  argv[argc] = nullptr;

  trySyscall(execv(argv[0], argv) == 0,
             "failed to run \"" + parameters_.executable + "\"");
  childFailure("handleChild() has reached the end");
}

[[noreturn]] void ProcessRunner::childFailure(const std::string &message,
                                              int errcode) {
  std::string fullMsg = getFullErrorMessage(message, errcode);
  int msgSize = static_cast<int>(fullMsg.size());
  write(pipe_[1], &msgSize, sizeof(msgSize));
  write(pipe_[1], fullMsg.c_str(), msgSize * sizeof(char));
  close(pipe_[1]);
  _exit(42);
}

void ProcessRunner::parentFailure(const std::string &message, int errcode) {
  throw RunnerError(getFullErrorMessage(message, errcode));
}

ProcessRunner::ProcessRunner() {}

}  // namespace UnixRunner
