#pragma once

#include <cassert>
#include <coroutine>
#include <exception>
#include <functional>
#include <future>

#include "rdmapp/detail/debug.h"
#include "rdmapp/detail/noncopyable.h"

namespace rdmapp {

template <class T> class value_returner {
public:
  std::promise<T> promise_;
  void return_value(T &&value) { promise_.set_value(std::forward<T>(value)); }
};

template <> class value_returner<void> {
public:
  std::promise<void> promise_;
  void return_void() { promise_.set_value(); }
};

template <class T, class CoroutineHandle>
struct promise_base : public value_returner<T> {
  std::suspend_never initial_suspend() { return {}; }
  auto final_suspend() noexcept {
    struct awaiter {
      std::coroutine_handle<> release_detached_;
      bool await_ready() noexcept { return false; }
      // 返回类型是`coroutine_handle`，返回的`coroutine_handle`会被立即执行
      std::coroutine_handle<>
      await_suspend(CoroutineHandle suspended) noexcept {
        // 支持嵌套`co_await`，`suspended`是当前`co_return`的coroutine的handle
        // `continuation_`是对当前coroutine进行`co_await`调用的上层coroutine的handle
        if (suspended.promise().continuation_) {
          return suspended.promise().continuation_;
        } else {
          if (release_detached_) {
            release_detached_.destroy();
          }
          return std::noop_coroutine();
        }
      }
      void await_resume() noexcept {}
    };
    return awaiter{release_detached_};
  }

  std::coroutine_handle<> continuation_;
  std::coroutine_handle<> release_detached_;
};

template <class T> struct task {
  struct promise_type
      : public promise_base<T, std::coroutine_handle<promise_type>> {
    task<T> get_return_object() {
      return std::coroutine_handle<promise_type>::from_promise(*this);
    }
    void unhandled_exception() {
      this->promise_.set_exception(std::current_exception());
    }
    promise_type() : future_(this->promise_.get_future()) {}
    // 用于支持coroutine后台执行语义（不去`co_await` coroutine而是直接拿到coroutine返回的`task`），promise的值在`co_return`时被设置
    std::future<T> &get_future() { return future_; }
    void set_detached_task(std::coroutine_handle<promise_type> h) {
      this->release_detached_ = h;
    }
    std::future<T> future_;
  };

  // coroA() {
  //     value = co_await coroB();
  // }
  // =>
  // coroA() {
  //     if (!AwaiterB.ready()) {
  //         AwaiterB.suspend(handleA);
  //         <return-to-caller-or-resumer>
  //     }
  //     // 如果`!ready()`，后面这些代码会在`coroB`调用`co_return`后执行
  //     value = AwaiterB.resume();
  // }
  struct task_awaiter {
    std::coroutine_handle<promise_type> h_;
    task_awaiter(std::coroutine_handle<promise_type> h) : h_(h) {}
    // 如果`co_await`的coroutine已经完成，直接走`await_resume`拿到coroutine的结果
    bool await_ready() { return h_.done(); }
    // `co_await`下游coroutine（返回该`task_awaiter`的coroutine）时，将当前coroutine的handle（`suspended`）传给下游coroutine的promise
    auto await_suspend(std::coroutine_handle<> suspended) {
      h_.promise().continuation_ = suspended;
    }
    // `co_await` coroutine的语义是等待coroutine完成并返回，因此`await_resume`需要拿到coroutine的promise中的值
    auto await_resume() { return h_.promise().future_.get(); }
  };

  using coroutine_handle_type = std::coroutine_handle<promise_type>;

  auto operator co_await() const { return task_awaiter(h_); }

  ~task() {
    if (!detached_) {
      if (!h_.done()) {
        h_.promise().set_detached_task(h_);
        get_future().wait();
      } else {
        h_.destroy();
      }
    }
  }
  task(task &&other)
      : h_(std::exchange(other.h_, nullptr)),
        detached_(std::exchange(other.detached_, true)) {}
  task(coroutine_handle_type h) : h_(h), detached_(false) {}
  coroutine_handle_type h_;
  bool detached_;
  operator coroutine_handle_type() const { return h_; }
  std::future<T> &get_future() const { return h_.promise().get_future(); }
  void detach() {
    assert(!detached_);
    h_.promise().set_detached_task(h_);
    detached_ = true;
  }
};

} // namespace rdmapp