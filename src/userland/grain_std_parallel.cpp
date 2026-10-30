// grain on every core: parallel_map(list, fn), spawn(fn, args...), await(task), task_done(task).
//
// A VM is one thread's: its stack, its frames, the inline caches in the chunks it runs, the
// native code it compiles into a function, and lists and maps that anyone holding them may change
// at any moment. Locking all of that would tax every program that never runs two things at once,
// so the parallelism here is between VMs instead. A worker is a VM of its own. The function it
// runs is copied into it with everything the function can reach - its arguments, what its closure
// captured, the globals its code names, and the functions those name in turn - and its result
// comes back when it is done. Nothing is shared but what cannot change (strings, ids, natives), so
// nothing can race, and a program reads the same as it would if each call ran on its own.
//
// What follows from that, and is the language's rule rather than an accident:
//   - a worker changing a list it was given, a global, or a captured variable changes its copy;
//     the program sees only what the function returns
//   - a socket, a database connection, a channel or an entity cannot be copied and is refused by
//     name: open the connection inside the function instead
//   - a worker has no loop of its own, so every(), after(), live(), events() and run_panes()
//     refuse there; a pane a worker made is an error
//   - what a worker prints is shown when its result is collected, in order: parallel_map's in
//     item order, a task's when it is awaited
//
// The threads are one process-wide pool of aios::hal::Threads, as many as the machine has
// hardware threads, started the first time a program asks for one. The caller always runs a lane
// itself and takes back any lane no pool thread has started, so a parallel_map inside a worker,
// or an await() on a task still waiting in the queue, finishes on its own thread instead of
// waiting for a pool that is busy with its caller.
#include <algorithm>
#include <atomic>
#include <deque>
#include <functional>
#include <memory>
#include <mutex>
#include <string>
#include <unordered_map>
#include <unordered_set>
#include <vector>

#include "aios/hal/sync.hpp"
#include "aios/userland/grain.hpp"

namespace aios::userland::grain {
namespace {

// ---- the pool ---------------------------------------------------------------------------------

// One piece of work. Whoever takes it first runs it: a pool thread, or the thread that would
// otherwise wait for it (await, the end of a parallel_map, a VM going away with tasks never awaited).
struct Job {
  std::function<void()> body;
  std::atomic<int> state{0};   // 0 queued, 1 taken, 2 done
  aios::hal::Mutex mu;
  aios::hal::CondVar cv;

  bool take() { int queued = 0; return state.compare_exchange_strong(queued, 1); }
  void run() {
    body();
    body = nullptr;   // what it holds goes now, not when the last handle does
    std::unique_lock<aios::hal::Mutex> lk(mu);
    state.store(2);
    cv.notify_all();
  }
  void wait() {
    std::unique_lock<aios::hal::Mutex> lk(mu);
    cv.wait(lk, [&] { return state.load() == 2; });
  }
  // Runs it here if nobody has started it, else waits for whoever did.
  void finish() { if (take()) run(); else wait(); }
};

class Pool {
 public:
  // Never destroyed: its threads sit in wait() through static teardown, and a task some program
  // left running must not find the queue gone under it.
  static Pool& get() {
    static Pool* pool = new Pool();
    return *pool;
  }
  size_t size() const { return threads_.size(); }
  void submit(const std::shared_ptr<Job>& job) {
    {
      std::unique_lock<aios::hal::Mutex> lk(mu_);
      queue_.push_back(job);
    }
    cv_.notify_one();
  }

 private:
  Pool() {
    const unsigned n = std::max(1u, aios::hal::hardware_threads());
    threads_.reserve(n);
    for (unsigned i = 0; i < n; ++i) threads_.emplace_back([this] { loop(); });
  }
  void loop() {
    for (;;) {
      std::shared_ptr<Job> job;
      {
        std::unique_lock<aios::hal::Mutex> lk(mu_);
        if (queue_.empty()) {   // about to sit idle: give back what a deep call touched
          lk.unlock();
          aios::hal::release_idle_stack();
          lk.lock();
        }
        cv_.wait(lk, [&] { return !queue_.empty(); });
        job = std::move(queue_.front());
        queue_.pop_front();
      }
      if (job->take()) job->run();   // else its waiter ran (or cancelled) it already
    }
  }
  aios::hal::Mutex mu_;
  aios::hal::CondVar cv_;
  std::deque<std::shared_ptr<Job>> queue_;
  std::vector<aios::hal::Thread> threads_;
};

// ---- copying a function and what it reaches into another VM -----------------------------------

// Where an instruction ends, by the same encoding the disassembler reads.
size_t next_instruction(const Chunk& c, size_t at) {
  switch ((Op)c.code[at]) {
    case Op::Constant: case Op::GetGlobal: case Op::SetGlobal: case Op::DefineGlobal:
    case Op::FieldGet: case Op::OptFieldGet: case Op::FieldSet: case Op::BuildList: case Op::BuildMap: case Op::ListConcat:
    case Op::Jump: case Op::JumpIfFalse: case Op::JumpIfTrue: case Op::Loop:
      return at + 3;
    case Op::GetLocal: case Op::SetLocal: case Op::GetUpvalue: case Op::SetUpvalue:
    case Op::Call: case Op::IterNext:
      return at + 2;
    case Op::Closure: {
      const size_t k = ((size_t)c.code[at + 1] << 8) | c.code[at + 2];
      const auto* sub = k < c.constants.size() ? static_cast<const FnObj*>(c.constants[k].obj.get()) : nullptr;
      return at + 3 + 2 * (sub ? sub->upvalue_index.size() : 0);
    }
    default:
      return at + 1;
  }
}

// Copies values out of `from` into fresh objects for `into`, keeping what was shared shared (two
// references to one list stay two references to one copy) and cycles cycles. Every global a
// copied function's code names is brought over too, once, by finish(). Reads `from` only, so
// several lanes may copy out of one VM at once while that VM waits for them.
class Copier {
 public:
  Copier(Vm& from, Vm& into) : from_(from), into_(into) {}

  bool value(const Value& v, Value& out, std::string& error) {
    if (!v.obj) { out = v; return true; }
    switch (v.type) {
      case Type::Str: case Type::Id128: case Type::Native: case Type::Decimal:
        out = v;   // never changed once made: shared, not copied
        return true;
      case Type::Extern: {
        auto* e = static_cast<const ExternObj*>(v.obj.get());
        error = std::string("a ") + e->kind_name + " cannot be sent to a worker, which is a VM of its own; open it inside the function";
        return false;
      }
      case Type::Channel: case Type::MemoryObject: case Type::Entity:
        error = std::string("a ") + type_name(v.type) + " cannot be sent to a worker, which is a VM of its own; make it inside the function";
        return false;
      default:
        break;
    }
    if (auto it = seen_.find(v.obj.get()); it != seen_.end()) { out = Value::object(v.type, it->second); return true; }
    switch (v.type) {
      case Type::List: {
        auto src = static_cast<const ListObj*>(v.obj.get());
        auto dst = std::make_shared<ListObj>(std::vector<Value>{});
        seen_[src] = dst;
        dst->items.resize(src->items.size());
        bool has_c = false;
        for (size_t i = 0; i < src->items.size(); ++i) {
          if (!value(src->items[i], dst->items[i], error)) return false;
          if (dst->items[i].is_container()) has_c = true;
        }
        out = Value::object(Type::List, dst);
        if (has_c) into_.track_container(dst);
        return true;
      }
      case Type::Map: {
        auto src = static_cast<const MapObj*>(v.obj.get());
        auto dst = std::make_shared<MapObj>();
        seen_[src] = dst;
        bool has_c = false;
        for (const auto& [k, item] : src->entries) {
          Value c;
          if (!value(item, c, error)) return false;
          if (c.is_container()) has_c = true;
          dst->put(k, std::move(c));
        }
        out = Value::object(Type::Map, dst);
        if (has_c) into_.track_container(dst);
        return true;
      }
      case Type::Bytes: {
        auto src = static_cast<const BytesObj*>(v.obj.get());
        auto dst = std::make_shared<BytesObj>(src->data);
        seen_[src] = dst;
        out = Value::object(Type::Bytes, dst);
        return true;
      }
      case Type::Fn: {
        std::shared_ptr<Obj> made;
        if (!v.obj->is_closure) {
          auto fn = function(std::static_pointer_cast<FnObj>(v.obj), error);
          if (!fn) return false;
          made = fn;
        } else {
          auto src = static_cast<const ClosureObj*>(v.obj.get());
          auto dst = std::make_shared<ClosureObj>();
          seen_[src] = dst;
          dst->fn = function(src->fn, error);
          if (!dst->fn) return false;
          for (const auto& uv : src->upvalues) {
            auto& copy = upvalues_[uv.get()];
            if (!copy) {
              copy = std::make_shared<Upvalue>();
              copy->is_closed = true;   // the worker's copy of the variable, as it is now
              if (!value(from_.upvalue_value(*uv), copy->closed, error)) return false;
            }
            dst->upvalues.push_back(copy);
          }
          made = dst;
          into_.track_container(dst);
        }
        out = Value::object(Type::Fn, made);
        return true;
      }
      case Type::BoundMethod: {
        auto src = static_cast<const BoundMethodObj*>(v.obj.get());
        Value receiver;
        if (!value(src->receiver, receiver, error)) return false;
        auto dst = std::make_shared<BoundMethodObj>(receiver, src->name, src->min_args, src->max_args, src->fn);
        seen_[src] = dst;
        out = Value::object(Type::BoundMethod, dst);
        return true;
      }
      case Type::Capability: {
        auto src = static_cast<const CapObj*>(v.obj.get());
        auto dst = std::make_shared<CapObj>(src->id, src->resource, src->rights, src->expiry, src->parent_id, src->holder);
        seen_[src] = dst;
        out = Value::object(Type::Capability, dst);
        return true;
      }
      case Type::StateFrame: {
        auto src = static_cast<const FrameObj*>(v.obj.get());
        Value payload;
        if (!value(src->payload, payload, error)) return false;
        auto dst = std::make_shared<FrameObj>(src->id, src->generation, src->publisher, payload, src->timestamp);
        seen_[src] = dst;
        out = Value::object(Type::StateFrame, dst);
        return true;
      }
      default:
        error = std::string("a ") + type_name(v.type) + " cannot be sent to a worker";
        return false;
    }
  }

  // Brings over every global the copied code names that the worker does not have yet: what the
  // program's own thread would have found under that name, copied the same way.
  bool finish(std::string& error) {
    while (!names_.empty()) {
      std::string name = std::move(names_.back());
      names_.pop_back();
      if (into_.find_global(name)) continue;
      Value* g = from_.find_global(name);
      if (!g) continue;   // undefined there too: the worker reports it where the code uses it
      Value c;
      if (!value(*g, c, error)) {
        error += " (the global '" + name + "')";
        return false;
      }
      into_.define_global(name, std::move(c));
    }
    return true;
  }

 private:
  // A function's own copy: the same code with fresh inline caches and no native code yet, both of
  // which the worker fills in for itself.
  std::shared_ptr<FnObj> function(const std::shared_ptr<FnObj>& src, std::string& error) {
    if (!src) return nullptr;
    if (auto it = seen_.find(src.get()); it != seen_.end()) return std::static_pointer_cast<FnObj>(it->second);
    auto dst = std::make_shared<FnObj>();
    seen_[src.get()] = dst;
    dst->name = src->name;
    dst->arity = src->arity;
    dst->local_slots = src->local_slots;
    dst->upvalue_from_local = src->upvalue_from_local;
    dst->upvalue_index = src->upvalue_index;
    dst->chunk.code = src->chunk.code;
    dst->chunk.lines = src->chunk.lines;
    dst->chunk.constants.resize(src->chunk.constants.size());
    for (size_t k = 0; k < src->chunk.constants.size(); ++k)
      if (!value(src->chunk.constants[k], dst->chunk.constants[k], error)) return nullptr;
    const Chunk& c = src->chunk;
    for (size_t at = 0; at < c.code.size(); at = next_instruction(c, at)) {
      const Op op = (Op)c.code[at];
      if (op != Op::GetGlobal && op != Op::SetGlobal) continue;
      const size_t k = ((size_t)c.code[at + 1] << 8) | c.code[at + 2];
      if (k < c.constants.size() && c.constants[k].type == Type::Str && named_.insert(c.constants[k].str()).second)
        names_.push_back(c.constants[k].str());
    }
    return dst;
  }

  Vm& from_;
  Vm& into_;
  std::unordered_map<const Obj*, std::shared_ptr<Obj>> seen_;
  std::unordered_map<const Upvalue*, std::shared_ptr<Upvalue>> upvalues_;
  std::vector<std::string> names_;
  std::unordered_set<std::string> named_;
};

// A VM for one worker: the program's natives (so it can do what the program can, and no more),
// its storage, its step limit, and its own place to print.
std::unique_ptr<Vm> make_worker(Vm& parent, std::vector<std::string>* sink) {
  auto w = std::make_unique<Vm>();
  w->worker = true;
  w->files = parent.files;
  w->step_limit = parent.step_limit;
  w->jit_enabled = parent.jit_enabled;
  w->settings = parent.settings;
  w->kv = parent.kv;   // the same kv_* store as the program that spawned it
  w->allow_process = parent.allow_process;
  w->allow_env = parent.allow_env;
  w->on_print = parent.on_print;
  w->stop_flag = parent.stop_flag;
  w->sink = sink;
  for (const auto& name : parent.global_names()) {
    Value* g = parent.find_global(name);
    if (g && g->type == Type::Native) w->define_global(name, *g);
  }
  return w;
}

// A pane belongs to the VM that made it; a worker's would never be shown, so making one is an error.
bool check_no_panes(Vm& w, std::string& error) {
  if (pane_list(w).empty()) return true;
  drop_panes(w);
  error = "a worker (parallel_map, spawn) cannot show a pane; make panes in the program itself";
  return false;
}

bool callable(const Value& v) { return v.type == Type::Fn || v.type == Type::Native || v.type == Type::BoundMethod; }

// ---- parallel_map -----------------------------------------------------------------------------

// parallel_map(list, fn) -> [fn(list[0]), fn(list[1]), ...], the calls spread over every core.
Value bi_parallel_map(Vm& vm, std::vector<Value>& args, std::string& error) {
  if (args[0].type != Type::List) { error = std::string("parallel_map() wants a list, got a ") + type_name(args[0].type); return Value::nil(); }
  if (!callable(args[1])) { error = std::string("parallel_map() wants a function to call, got a ") + type_name(args[1].type); return Value::nil(); }
  const std::vector<Value>& items = args[0].list_items();
  const Value& fn = args[1];
  const size_t n = items.size();
  if (n == 0) return Value::list({});

  struct Run {
    std::atomic<size_t> next{0};
    std::atomic<bool> failed{false};
    std::vector<Value> results;
    std::vector<std::vector<std::string>> printed;
    aios::hal::Mutex mu;            // guards the error
    size_t error_at = SIZE_MAX;     // the first item (lowest index) that failed
    std::string error;
    void fail(size_t at, const std::string& message) {
      std::unique_lock<aios::hal::Mutex> lk(mu);
      if (at < error_at) { error_at = at; error = message; }
      failed.store(true);
    }
  };
  Run run;
  run.results.resize(n);
  run.printed.resize(n);

  // One lane: a worker VM that takes items until none are left. `vm`, `items` and `fn` are only
  // read, and the caller does not return before every lane that started has finished.
  auto lane = [&run, &vm, &items, &fn, n]() {
    if (run.next.load() >= n || run.failed.load()) return;   // started after the work ran out
    std::vector<std::string> sink;
    auto w = make_worker(vm, &sink);
    Copier copier(vm, *w);
    std::string err;
    Value wfn;
    if (!copier.value(fn, wfn, err) || !copier.finish(err)) { run.fail(n, err); return; }   // n: the function, not an item
    for (;;) {
      if (run.failed.load()) break;
      const size_t i = run.next.fetch_add(1);
      if (i >= n) break;
      Value item;
      if (!copier.value(items[i], item, err) || !copier.finish(err)) { run.fail(i, err); break; }
      Value r = w->call(wfn, {item}, err);
      if (err.empty()) check_no_panes(*w, err);
      run.printed[i].swap(sink);
      sink.clear();
      if (!err.empty()) { run.fail(i, err); break; }
      run.results[i] = std::move(r);
    }
  };

  Pool& pool = Pool::get();
  const size_t lanes = std::min(n, pool.size());
  std::vector<std::shared_ptr<Job>> jobs;
  jobs.reserve(lanes > 0 ? lanes - 1 : 0);
  for (size_t k = 1; k < lanes; ++k) {
    auto job = std::make_shared<Job>();
    job->body = lane;
    pool.submit(job);
    jobs.push_back(std::move(job));
  }
  lane();   // this thread is a lane too
  // A lane no pool thread has started is cancelled here; one that started is waited for.
  for (auto& job : jobs) {
    if (job->take()) { job->body = nullptr; job->state.store(2); }
    else job->wait();
  }

  for (size_t i = 0; i < n; ++i) for (const auto& line : run.printed[i]) vm.emit(line);
  if (run.failed.load()) {
    error = "parallel_map(): " + (run.error_at == n ? std::string() : "item " + std::to_string(run.error_at) + ": ") + run.error;
    return Value::nil();
  }
  return Value::list(std::move(run.results));
}

// ---- spawn / await ----------------------------------------------------------------------------

// A spawned call: its worker VM (made and filled on the spawning thread, run on a pool thread or
// on whoever awaits it first), and when it is done, its result.
struct Task {
  std::shared_ptr<Job> job;
  std::unique_ptr<Vm> vm;
  std::vector<std::string> printed;
  Value fn;
  std::vector<Value> args;
  Value result;
  std::string error;
  bool shown = false;   // its printed lines went to an awaiter already
};

// The tasks a VM has started; going away with the VM, it sees each one finished.
struct TaskSet {
  aios::hal::Mutex mu;
  std::vector<std::shared_ptr<Job>> jobs;
  void add(std::shared_ptr<Job> job) {
    std::unique_lock<aios::hal::Mutex> lk(mu);
    if (jobs.size() >= 64) {   // a program that spawns for hours keeps only the unfinished ones
      jobs.erase(std::remove_if(jobs.begin(), jobs.end(), [](const std::shared_ptr<Job>& j) { return j->state.load() == 2; }), jobs.end());
    }
    jobs.push_back(std::move(job));
  }
  ~TaskSet() { for (auto& job : jobs) job->finish(); }
};

constexpr const char* kTaskKind = "task";

std::shared_ptr<Task> task_of(const Value& v) {
  if (v.type != Type::Extern || !v.obj) return nullptr;
  auto* e = static_cast<const ExternObj*>(v.obj.get());
  if (e->kind_name != kTaskKind) return nullptr;
  return std::static_pointer_cast<Task>(e->ptr);
}

// spawn(fn, args...) -> task: fn(args...) on another core, while the program goes on.
Value bi_spawn(Vm& vm, std::vector<Value>& args, std::string& error) {
  if (!callable(args[0])) { error = std::string("spawn() wants a function to call, got a ") + type_name(args[0].type); return Value::nil(); }
  auto task = std::make_shared<Task>();
  task->vm = make_worker(vm, &task->printed);
  Copier copier(vm, *task->vm);
  bool ok = copier.value(args[0], task->fn, error);
  task->args.resize(args.size() - 1);
  for (size_t i = 1; ok && i < args.size(); ++i) ok = copier.value(args[i], task->args[i - 1], error);
  if (!ok || !copier.finish(error)) { error = "spawn(): " + error; return Value::nil(); }

  task->job = std::make_shared<Job>();
  // The job holds its task until it has run (run() lets the body go), so a program that drops the
  // handle without awaiting it still gets its call made, and the VM's TaskSet still waits for it.
  task->job->body = [t = task]() {
    t->result = t->vm->call(t->fn, t->args, t->error);
    if (t->error.empty()) check_no_panes(*t->vm, t->error);
    if (!t->error.empty()) t->result = Value::nil();
    t->fn = Value::nil();
    t->args.clear();
    t->vm.reset();   // its globals go with it; the result and what it printed stay
  };
  if (!vm.tasks) vm.tasks = std::make_shared<TaskSet>();
  static_cast<TaskSet*>(vm.tasks.get())->add(task->job);
  Pool::get().submit(task->job);
  return Value::object(Type::Extern, std::make_shared<ExternObj>(kTaskKind, task));
}

// await(task) -> what the function returned, once it has; its error, raised here, if it failed.
Value bi_await(Vm& vm, std::vector<Value>& args, std::string& error) {
  auto task = task_of(args[0]);
  if (!task) { error = std::string("await() wants a task from spawn(), got a ") + type_name(args[0].type); return Value::nil(); }
  task->job->finish();
  if (!task->shown) {
    task->shown = true;
    for (const auto& line : task->printed) vm.emit(line);
    task->printed.clear();
  }
  if (!task->error.empty()) { error = "await(): the task failed: " + task->error; return Value::nil(); }
  return task->result;
}

// task_done(task) -> true once await() would not wait.
Value bi_task_done(Vm&, std::vector<Value>& args, std::string& error) {
  auto task = task_of(args[0]);
  if (!task) { error = std::string("task_done() wants a task from spawn(), got a ") + type_name(args[0].type); return Value::nil(); }
  return Value::boolean(task->job->state.load() == 2);
}

}  // namespace

void WorkerCall::run() {
  result = vm->call(fn, args, error);
  if (error.empty()) check_no_panes(*vm, error);
  if (!error.empty()) result = Value::nil();
  fn = Value::nil();
  args.clear();
  vm.reset();   // its globals go with it; the result and what it printed stay
}

std::unique_ptr<WorkerCall> prepare_worker_call(Vm& vm, const Value& fn, std::vector<Value> args, std::string& error) {
  auto call = std::make_unique<WorkerCall>();
  call->vm = make_worker(vm, &call->printed);
  Copier copier(vm, *call->vm);
  bool ok = copier.value(fn, call->fn, error);
  call->args = std::move(args);   // plain data made for this call: nothing in it belongs to `vm`
  if (!ok || !copier.finish(error)) return nullptr;
  return call;
}

void install_parallel_builtins(Vm& vm) {
  vm.define_native("parallel_map", 2, 2, bi_parallel_map);
  vm.define_native("spawn", 1, -1, bi_spawn);
  vm.define_native("await", 1, 1, bi_await);
  vm.define_native("task_done", 1, 1, bi_task_done);
}

}  // namespace aios::userland::grain
