//  Copyright 2013, bitdewy@gmail.com
//  Distributed under the Boost Software License, Version 1.0.
//  You may obtain a copy of the License at
//
//  http://www.boost.org/LICENSE_1_0.txt

#include "interprocess/acceptor.h"
#include <windows.h>
#include <string>

namespace interprocess {

Acceptor::Acceptor(const std::string& endpoint)
  : pipe_name_(std::string("\\\\.\\pipe\\").append(endpoint)),
    next_pipe_(NULL),
    close_event_(CreateEvent(NULL, FALSE, FALSE, NULL)) {
  ZeroMemory(&connect_overlap_, sizeof connect_overlap_);
}

Acceptor::~Acceptor() {
  Stop();
  CloseHandle(next_pipe_);
  CloseHandle(close_event_);
}

void Acceptor::Listen() {
  listen_thread_.swap(
    std::thread(std::bind(&Acceptor::LinstenInThread, this)));
}

void Acceptor::Stop() {
  SetEvent(close_event_);
  DisconnectNamedPipe(next_pipe_);
  if (listen_thread_.joinable()) {
    listen_thread_.join();
  }
}

void Acceptor::SetNewConnectionCallback(const NewConnectionCallback& cb) {
  new_connection_callback_ = cb;
}

void Acceptor::SetExceptionCallback(const ExceptionCallback& cb) {
  exception_callback_ = cb;
}

void Acceptor::MoveIOFunctionToAlertableThread(
  const std::function<void()>& cb) {
  async_io_callback_ = cb;
}


void Acceptor::LinstenInThread() {
  std::exception_ptr eptr;
  try {
    HANDLE conn_event = CreateEvent(NULL, TRUE, TRUE, NULL);
    ScopeGuard auto_release_conn_event([&] { CloseHandle(conn_event); });
    if (conn_event == NULL) {
      auto_release_conn_event.Dismiss();
      auto msg = std::string("CreateEvent (connect event) failed GLE = ");
      msg.append(std::to_string(GetLastError()));
      std::rethrow_exception(
        std::make_exception_ptr(ConnectionExcepton(msg)));
    }
    HANDLE write_event = CreateEvent(NULL, FALSE, FALSE, NULL);
    ScopeGuard auto_release_write_event([&] { CloseHandle(write_event); });
    if (write_event == NULL) {
      auto_release_write_event.Dismiss();
      auto msg = std::string("CreateEvent (write event) failed GLE = ");
      msg.append(std::to_string(GetLastError()));
      std::rethrow_exception(
        std::make_exception_ptr(ConnectionExcepton(msg)));
    }

    HANDLE events[3] = { conn_event, write_event, close_event_ };
    connect_overlap_.hEvent = conn_event;
    bool pedding = CreateConnectInstance();

    for (;;) {
      auto wait = WaitForMultipleObjectsEx(
        3,
        events,         // events object to wait for
        FALSE,          // waits any one
        INFINITE,       // waits indefinitely
        TRUE);          // alertable wait enabled

      switch (wait) {
      case WAIT_OBJECT_0:
        // If an operation is pending, get the result of the
        // connect operation.
        if (pedding) {
          DWORD ret = 0;
          auto success = GetOverlappedResult(
            next_pipe_,         // pipe handle
            &connect_overlap_,  // OVERLAPPED structure
            &ret,               // bytes transferred
            FALSE);             // does not wait
          if (!success) {
            auto msg = std::string("ConnectNamedPipe failed GLE = ");
            msg.append(std::to_string(GetLastError()));
            std::rethrow_exception(
              std::make_exception_ptr(ConnectionExcepton(msg)));
          }
        }
        if (new_connection_callback_) {
          new_connection_callback_(next_pipe_, write_event);
        }
        pedding = CreateConnectInstance();
        break;

      // Send operation pendding
      case WAIT_OBJECT_0 + 1:
        if (async_io_callback_) {
          async_io_callback_();
        }
        break;

      case WAIT_OBJECT_0 + 2:
        return;

      // The wait is satisfied by a completed read or write
      // operation. This allows the system to execute the
      // completion routine.
      case WAIT_IO_COMPLETION:
        break;

      // An error occurred in the wait function.
      default:
        auto msg = std::string("Unexpected error GLE = ");
        msg.append(std::to_string(GetLastError()));
        std::rethrow_exception(
          std::make_exception_ptr(ConnectionExcepton(msg)));
      }
    }
  } catch(...) {
    eptr = std::current_exception();
  }

  if (exception_callback_) {
    exception_callback_(eptr);
  }
}

bool Acceptor::CreateConnectInstance() {
  next_pipe_ = CreateNamedPipe(
    pipe_name_.c_str(),        // pipe name
    PIPE_ACCESS_DUPLEX |       // read/write access
    FILE_FLAG_OVERLAPPED,      // overlapped mode
    PIPE_TYPE_MESSAGE |        // message-type pipe
    PIPE_READMODE_MESSAGE |    // message read mode
    PIPE_WAIT,                 // blocking mode
    PIPE_UNLIMITED_INSTANCES,  // unlimited instances
    kBufferSize,               // output buffer size
    kBufferSize,               // input buffer size
    kTimeout,                  // client time-out
    NULL);                     // default security attributes

  if (next_pipe_ == INVALID_HANDLE_VALUE) {
    auto msg = std::string("CreateNamedPipe failed GLE = ");
    msg.append(std::to_string(GetLastError()));
    std::rethrow_exception(
      std::make_exception_ptr(ConnectionExcepton(msg)));
  }

  bool pendding = false;

  // Start an overlapped connection for this pipe instance.
  auto connected = ConnectNamedPipe(next_pipe_, &connect_overlap_);

  // Overlapped ConnectNamedPipe should return zero.
  if (connected) {
    auto msg = std::string("ConnectNamedPipe failed GLE = ");
    msg.append(std::to_string(GetLastError()));
    std::rethrow_exception(
      std::make_exception_ptr(ConnectionExcepton(msg)));
  }

  switch (GetLastError()) {
  // The overlapped connection in progress.
  case ERROR_IO_PENDING:
    pendding = true;
    break;

  // Client is already connected, so signal an event.
  case ERROR_PIPE_CONNECTED:
    if (SetEvent(connect_overlap_.hEvent)) {
      break;
    }
    // jump to default

  // If an error occurs during the connect operation...
  default:
    auto msg = std::string("ConnectNamedPipe failed GLE = ");
    msg.append(std::to_string(GetLastError()));
    std::rethrow_exception(
      std::make_exception_ptr(ConnectionExcepton(msg)));
  }
  return pendding;
}

}  // namespace interprocess
