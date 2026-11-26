// Fixed worker threads for splitting one computation across cores.
//
// parallel_for(n, f) runs f(0..n-1), f(0) in the caller and the rest on the
// compute pool, and returns when all are done. The pool's tasks never wait
// on anything, so a caller may itself be a worker of another pool (the
// station's decode pool) without risk of deadlock. Never nest parallel_for
// inside a parallel_for task.
//
// Threads: SIGNUM_COMPUTE_THREADS (default: hardware threads - 1, at least
// 1); 0 runs everything in the caller (single-core targets). Started once,
// so no thread is created while audio runs, at the worker priority below.
#pragma once

#include <pthread.h>
#include <sched.h>

#include <algorithm>
#include <atomic>
#include <condition_variable>
#include <cstdlib>
#include <deque>
#include <functional>
#include <mutex>
#include <thread>
#include <vector>

namespace fmodem::par {

// Priority of every worker thread (this pool and the station's decode pool):
// SCHED_FIFO SIGNUM_WORKER_PRIO (default 20), so a decode is not delayed by
// Pat, rigctld or a browser, yet stays below the audio loop (FIFO 80) and
// the kernel's threaded interrupts (FIFO 50, the sound card's among them).
// 0 = SCHED_OTHER. Without permission (no CAP_SYS_NICE / rtprio limit) the
// thread stays SCHED_OTHER, never inheriting the audio thread's FIFO 80.
// Returns the priority obtained (0 = normal).
inline int worker_prio()
{
	static const int want = [] {
		const char *e = std::getenv("SIGNUM_WORKER_PRIO");
		const int v = e ? std::atoi(e) : 20;
		return v < 0 ? 0 : v > 79 ? 79 : v;
	}();
	return want;
}
// workers started so far at real-time priority, and at normal priority
inline std::atomic<int> &workers_rt() { static std::atomic<int> n{0}; return n; }
inline std::atomic<int> &workers_normal() { static std::atomic<int> n{0}; return n; }
// A thread doing background work (the listen monitor, src/util/monitor_thread.hpp) marks itself; the
// decode pools it creates then start their workers at normal priority, so background work never
// competes with the station's real-time threads.
inline bool &background_thread() { thread_local bool b = false; return b; }
inline int set_worker_sched(bool background = false)
{
	sched_param sp{};
	if (worker_prio() > 0 && !background) {
		sp.sched_priority = worker_prio();
		if (pthread_setschedparam(pthread_self(), SCHED_FIFO, &sp) == 0) { workers_rt()++; return worker_prio(); }
	}
	sp.sched_priority = 0;
	pthread_setschedparam(pthread_self(), SCHED_OTHER, &sp);
	workers_normal()++;
	return 0;
}

// The calling worker at SCHED_FIFO `prio` for a scope, back to its own priority after. Only for a
// thread that runs SCHED_FIFO (with real-time permission); otherwise nothing changes. Used to order
// work that shares a core: a thread at a lower FIFO priority runs only when nothing above it is
// runnable (threads at one FIFO priority do not time-slice: the first runs to its end).
class FifoPrioScope {
public:
	explicit FifoPrioScope(int prio)
	{
		int pol = 0;
		sched_param sp{};
		if (prio < 1 || pthread_getschedparam(pthread_self(), &pol, &sp) != 0 || pol != SCHED_FIFO || sp.sched_priority == prio) return;
		old_ = sp.sched_priority;
		sched_param n{};
		n.sched_priority = prio;
		on_ = pthread_setschedparam(pthread_self(), SCHED_FIFO, &n) == 0;
	}
	~FifoPrioScope()
	{
		if (!on_) return;
		sched_param sp{};
		sp.sched_priority = old_;
		pthread_setschedparam(pthread_self(), SCHED_FIFO, &sp);
	}
	FifoPrioScope(const FifoPrioScope &) = delete;
	FifoPrioScope &operator=(const FifoPrioScope &) = delete;
private:
	int old_ = 0;
	bool on_ = false;
};

class ComputePool {
public:
	explicit ComputePool(int n)
	{
		for (int i = 0; i < n; i++)
			th_.emplace_back([this] {
				set_worker_sched();
				for (;;) {
					std::function<void()> f;
					{
						std::unique_lock<std::mutex> lk(m_);
						cv_.wait(lk, [this] { return stop_ || !q_.empty(); });
						if (q_.empty()) return;
						f = std::move(q_.front()); q_.pop_front();
					}
					f();
				}
			});
	}
	~ComputePool()
	{
		{ std::lock_guard<std::mutex> lk(m_); stop_ = true; }
		cv_.notify_all();
		for (auto &t : th_) t.join();
	}
	int size() const { return int(th_.size()); }
	void post(std::function<void()> f)
	{
		{ std::lock_guard<std::mutex> lk(m_); q_.push_back(std::move(f)); }
		cv_.notify_one();
	}
private:
	std::mutex m_;
	std::condition_variable cv_;
	std::deque<std::function<void()>> q_;
	std::vector<std::thread> th_;
	bool stop_ = false;
};

inline int compute_threads()
{
#ifdef SIGNUM_COMPUTE_THREADS
	return SIGNUM_COMPUTE_THREADS;
#else
	return std::max(1, int(std::thread::hardware_concurrency()) - 1);
#endif
}

inline ComputePool &pool()
{
	static ComputePool p(compute_threads());
	return p;
}

// How many pieces to split work into: the caller plus every pool thread.
inline int ways() { return pool().size() + 1; }

template <class F> void parallel_for(int n, F f)
{
	if (n <= 1 || pool().size() == 0) {
		for (int i = 0; i < n; i++) f(i);
		return;
	}
	std::mutex m;
	std::condition_variable cv;
	int left = n - 1;
	for (int i = 1; i < n; i++)
		pool().post([&, i] {
			f(i);
			std::lock_guard<std::mutex> lk(m);
			if (--left == 0) cv.notify_one();
		});
	f(0);
	std::unique_lock<std::mutex> lk(m);
	cv.wait(lk, [&] { return left == 0; });
}

}  // namespace fmodem::par
