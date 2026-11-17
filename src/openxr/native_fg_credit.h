#pragma once
#include <condition_variable>
#include <functional>
#include <mutex>
#include <thread>
#include "runtime_gate.h"

namespace edvr::openxr {
// The idle pump already owns the exclusive runtime lease. A synchronous
// source-credit callback borrows that exact lease instead of entering twice.
// Ordinary calls still acquire the gate; the loan never permits concurrency.
class NativeFgCreditLoan final {
 public:
  struct Admission {
    RuntimeGate::Lease acquired;
    const NativeFgCreditLoan* loan=nullptr;
    uint64_t generation=0;
    explicit operator bool() const {return bool(acquired)||(loan&&loan->valid(generation));}
  };
  Admission admit(RuntimeGate& gate,uint64_t generation,bool owner) {
    if(!owner)return {};
    if(gate_==&gate&&valid(generation))return {RuntimeGate::Lease{},this,generation};
    return {gate.tryEnter(generation),nullptr,0};
  }
  bool run(RuntimeGate& gate,const RuntimeGate::Lease& operation,uint64_t generation,bool owner,
           const std::function<void()>& callback) {
    if(!owner||operation_||!operation||!operation.belongsTo(gate)||
       operation.generation()!=generation||!callback)return false;
    operation_=&operation;gate_=&gate;thread_=std::this_thread::get_id();
    try{callback();}catch(...){clear();throw;}
    clear();return true;
  }
 private:
  bool valid(uint64_t generation) const {
    return operation_&&thread_==std::this_thread::get_id()&&bool(*operation_)&&operation_->generation()==generation;
  }
  void clear(){operation_=nullptr;gate_=nullptr;thread_={};}
  const RuntimeGate::Lease* operation_=nullptr;
  RuntimeGate* gate_=nullptr;
  std::thread::id thread_{};
};
// Shared by an outstanding game caller and the XR owner. Only the caller
// waits; owner completion/cancellation never waits for the caller to resume.
struct NativeFgCreditRequest {
  std::mutex mutex;std::condition_variable cv;std::function<void()> callback;
  bool done=false,success=false;
  void finish(bool ok) {
    std::lock_guard<std::mutex> lock(mutex);if(done)return;
    success=ok;done=true;callback={};cv.notify_one();
  }
  void run(){try{callback();finish(true);}catch(...){finish(false);}}
  bool wait(){std::unique_lock<std::mutex> lock(mutex);cv.wait(lock,[&]{return done;});return success;}
};
} // namespace edvr::openxr
