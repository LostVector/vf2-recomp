// A small pool of worker threads for splitting a frame's drawing by rows.
#pragma once
#ifdef __APPLE__
#include <pthread.h>
#endif
#include <algorithm>
#include <atomic>
#include <chrono>
#include <condition_variable>
#include <cstdint>
#include <cstdlib>
#include <functional>
#include <mutex>
#include <thread>
#include <vector>
#if defined(__ANDROID__)
#include <pthread.h>
#include <unistd.h>
#endif

class row_pool
{
public:
	static row_pool &get() { static row_pool p; return p; }
	int threads() const { return int(m_workers.size()) + 1; }
	class lock_wait_sample {
	public:
		explicit lock_wait_sample(int64_t *total) : previous(t_lock_wait_ns)
		{
			if (total) t_lock_wait_ns = total;
		}
		~lock_wait_sample() { t_lock_wait_ns = previous; }
		lock_wait_sample(const lock_wait_sample &) = delete;
		lock_wait_sample &operator=(const lock_wait_sample &) = delete;
	private:
		int64_t *previous;
	};
#if defined(__ANDROID__)
	// The workers' ids as the kernel knows them, with this thread's first: the threads a frame's
	// drawing runs on, which is what Android's performance hints (a session, ADPF) are asked over.
	// (bionic's pthread_t is a pointer to its own thread record, not the kernel's tid: gettid() for
	// this thread, pthread_gettid_np() for the workers. ADPF wants the tids.)
	std::vector<long> worker_tids()
	{
		std::vector<long> ids;
		ids.push_back(long(gettid()));
		for (auto &w : m_workers) ids.push_back(long(pthread_gettid_np(w.native_handle())));
		return ids;
	}
#endif
	// called on the thread that takes each band, as it takes it (the host's log of where the
	// drawing runs: src/host/sysinfo.h)
	static inline void (*band_hook)() = nullptr;

	// Calls f(begin, end) over [0, n) in per_thread bands for each thread, and waits. With more
	// than one band a thread, each thread takes the next band when it's done with its last, so a
	// thread with heavy rows (the floor) doesn't leave the rest waiting.
	// tasks: f(i, i + 1) for each of n jobs, however few, as threads free up (n bands).
	void run(int n, const std::function<void(int, int)> &f, int per_thread = 1, bool tasks = false)
	{
		const int t = threads();
		// inside a band (a worker's, or this thread's mid-run): no pool to hand it to, so here
		if ((n < 2 * t && !tasks) || n < 2 || t == 1 || t_in_pool) { f(0, n); return; }
		// one caller at a time (another thread drawing at once would share the counters)
		using clock = std::chrono::steady_clock;
		const auto wait_started = t_lock_wait_ns ? clock::now() : clock::time_point{};
		std::lock_guard<std::mutex> one(m_run_mu);
		if (t_lock_wait_ns)
			*t_lock_wait_ns += std::chrono::duration_cast<std::chrono::nanoseconds>(clock::now() - wait_started).count();
		struct in_pool { in_pool() { t_in_pool = true; } ~in_pool() { t_in_pool = false; } } mark;
		const int parts = tasks ? n : std::min(n, t * std::max(1, per_thread));
		{
			std::lock_guard<std::mutex> l(m_mu);
			m_job = &f;
			m_n = n;
			m_parts = parts;
			m_next.store(0, std::memory_order_relaxed);
			m_left.store(t - 1, std::memory_order_relaxed);   // the workers still to finish
			m_gen++;
		}
		m_cv.notify_all();
		take_bands(f, n, parts);
		// Phases arrive in bursts a few milliseconds apart. A bounded spin on the
		// join counter avoids a sleep/wake round trip on the render thread; the
		// counter is atomic, so this runs without the lock (which the workers
		// need to take their decrement).
		if (const int spin_us = join_spin_us(); spin_us > 0)
			spin_until([&] { return m_left.load(std::memory_order_acquire) == 0; }, spin_us);
		std::unique_lock<std::mutex> l(m_mu);
		m_done.wait(l, [&] { return m_left.load(std::memory_order_acquire) == 0; });
		m_job = nullptr;
	}

private:
	std::vector<std::thread> m_workers;
	std::mutex m_mu, m_run_mu;
	static inline thread_local bool t_in_pool = false;
	static inline thread_local int64_t *t_lock_wait_ns = nullptr;
	std::condition_variable m_cv, m_done;
	const std::function<void(int, int)> *m_job = nullptr;
	int m_n = 0, m_parts = 0;
	std::atomic<int> m_left{0};
	std::atomic<unsigned> m_next{0};
	unsigned m_gen = 0;
	std::atomic<bool> m_quit{false};

	// A bounded spin on the join counter instead of parking the render thread
	// for the last band. Phases within a frame finish a few hundred
	// microseconds apart, and a condvar wake there costs ~1 ms; the condvar
	// remains the fallback when the spin expires. Spin only in the caller:
	// workers that spin were measured to starve the render and sound threads
	// (audio underruns), so they keep parking as before.
	static int join_spin_us()
	{
		static const int us = [] {
			const char *v = std::getenv("VF2_POOL_JOIN_SPIN_US");
			return v ? std::max(0, std::atoi(v)) : 800;
		}();
		return us;
	}
	static inline void cpu_pause()
	{
#if defined(__arm__) || defined(__aarch64__)
		__asm__ __volatile__("yield");
#else
		std::this_thread::yield();
#endif
	}
	template<class Done> static void spin_until(Done done, int us)
	{
		const auto until = std::chrono::steady_clock::now() + std::chrono::microseconds(us);
		while (!done() && std::chrono::steady_clock::now() < until) cpu_pause();
	}

	static std::pair<int, int> band(int i, int n, int parts) { return { n * i / parts, n * (i + 1) / parts }; }

	// A generation's band layout and callable stay fixed until the caller
	// and every worker finish. Only the assignment counter is shared
	// while bands are executing.
	void take_bands(const std::function<void(int, int)> &f, int n, int parts)
	{
		static const bool atomic_claims = std::getenv("VF2_ATOMIC_ROW_CLAIMS") != nullptr;
		static const bool direct_tasks = std::getenv("VF2_DIRECT_TASK_RANGES") != nullptr;
		const auto claim = [&] {
			if (atomic_claims) return m_next.fetch_add(1, std::memory_order_relaxed);
			std::lock_guard<std::mutex> l(m_mu);
			const unsigned i = m_next.load(std::memory_order_relaxed);
			if (i < unsigned(parts)) m_next.store(i + 1, std::memory_order_relaxed);
			return i;
		};
		for (;;) {
			const unsigned i = claim();
			if (i >= unsigned(parts)) return;
			if (band_hook) band_hook();
			if (direct_tasks && n == parts) f(int(i), int(i) + 1);
			else {
				auto [b, e] = band(int(i), n, parts);
				f(b, e);
			}
		}
	}

	row_pool()
	{
		// the CPU and sound keep theirs; 12 threads in all at most (from 8: at 3x on an M4 Max's 14
		// cores a fight's picture took 3.9 ms on 8, 3.3 on 12)
		int n = std::clamp(int(std::thread::hardware_concurrency()) - 2, 0, 11);
		if (const char *t = getenv("VF2_THREADS")) n = std::clamp(atoi(t) - 1, 0, 31);   // testing: threads in all
		for (int i = 0; i < n; i++)
			m_workers.emplace_back([this] {
#ifdef __APPLE__
				pthread_set_qos_class_self_np(QOS_CLASS_USER_INTERACTIVE, 0);   // (the performance cores)
#endif
				unsigned seen = 0;
				for (;;) {
					std::unique_lock<std::mutex> l(m_mu);
					m_cv.wait(l, [&] { return m_quit.load(std::memory_order_relaxed) || m_gen != seen; });
					if (m_quit.load(std::memory_order_relaxed)) return;
					seen = m_gen;
					const auto *job = m_job;
					const int n = m_n, parts = m_parts;
					l.unlock();
					t_in_pool = true;
					take_bands(*job, n, parts);
					t_in_pool = false;
					l.lock();
					if (m_left.fetch_sub(1, std::memory_order_acq_rel) == 1) m_done.notify_one();
				}
			});
	}
	~row_pool()
	{
		{ std::lock_guard<std::mutex> l(m_mu); m_quit.store(true); }
		m_cv.notify_all();
		for (auto &w : m_workers) w.join();
	}
};

inline void parallel_rows(int n, const std::function<void(int, int)> &f, int per_thread = 1) { row_pool::get().run(n, f, per_thread); }
// n jobs at once, each f(i) on a thread of the pool's
inline void parallel_tasks(int n, const std::function<void(int)> &f) { row_pool::get().run(n, [&](int a, int b) { for (int i = a; i < b; i++) f(i); }, 1, true); }

// One job at a time on a thread of its own (the host's pipelining: a picture drawn while the board
// runs on). The job can use parallel_rows: it's a caller like any other.
class background_job
{
public:
	~background_job()
	{
		if (!m_thread.joinable()) return;
		{ std::lock_guard<std::mutex> l(m_mu); m_quit = true; }
		m_cv.notify_all();
		m_thread.join();
	}
	// waits for the last job, then starts this one
	void start(std::function<void()> f)
	{
		wait();
		if (!m_thread.joinable()) m_thread = std::thread([this] {
#ifdef __APPLE__
			pthread_set_qos_class_self_np(QOS_CLASS_USER_INTERACTIVE, 0);
#endif
			loop();
		});
		{ std::lock_guard<std::mutex> l(m_mu); m_job = std::move(f); m_busy = true; }
		m_cv.notify_all();
	}
	void wait()
	{
		std::unique_lock<std::mutex> l(m_mu);
		m_done.wait(l, [&] { return !m_busy; });
	}

private:
	std::thread m_thread;
	std::mutex m_mu;
	std::condition_variable m_cv, m_done;
	std::function<void()> m_job;
	bool m_busy = false, m_quit = false;

	void loop()
	{
		for (;;) {
			std::function<void()> f;
			{
				std::unique_lock<std::mutex> l(m_mu);
				m_cv.wait(l, [&] { return m_quit || m_job; });
				if (m_quit) return;
				f = std::move(m_job);
				m_job = nullptr;
			}
			f();
			{ std::lock_guard<std::mutex> l(m_mu); m_busy = false; }
			m_done.notify_all();
		}
	}
};
