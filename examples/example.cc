// Generic n producers - m consumers sequenced example.

#include <chrono>
#include <climits>
#include <iostream>
#include <memory>
#include <sstream>
#include <thread>
#include <typeinfo>
#include <vector>

#include "args.hxx"

#include "disruptor/sequencer.h"

#ifdef __GNUG__  // gnu C++ compiler

#include <cxxabi.h>

std::string demangle(const char* mangled_name) {
  std::size_t len = 0;
  int status = 0;
  std::unique_ptr<char, decltype(&std::free)> ptr(
      __cxxabiv1::__cxa_demangle(mangled_name, nullptr, &len, &status),
      &std::free);
  return ptr.get();
}

#else

std::string demangle(const char* name) { return name; }

#endif  // _GNUG_

size_t RING_BUFFER_SIZE = 2048;

// Sum of all the events
std::atomic<int64_t> sum;
// Number of times to loop over buffer in a producer thread
size_t counter = 1000;
// Batch size
size_t delta = 1;

// Number of producer threads
int np = 1;
// Number of consumer threads
int nc = 1;

// The expected value of the cursor in the end
int64_t expectedValue;

// Consume published data
template <typename T, typename C, typename W>
void consume(disruptor::Sequencer<T, C, W>& s, disruptor::Sequence& seq) {
  std::vector<disruptor::Sequence*> depseqs;
  auto barrier = s.NewBarrier(depseqs);

  int64_t next_seq = disruptor::kFirstSequenceValue;

  int exitCtr = 0;
  int64_t localSum = 0;

  while (true) {
#ifdef PRINT_DEBUG_CONS
    std::stringstream ss;
    ss << "Wait for next seq: " << next_seq << ' ' << std::this_thread::get_id()
       << '\n';
    std::cout << ss.str();
#endif

    int64_t available_seq =
        barrier->WaitFor(next_seq, std::chrono::microseconds(10000));

#ifdef PRINT_DEBUG_CONS
    ss.clear();
    ss.str(std::string());
    ss << "Available seq: " << available_seq << ' '
       << std::this_thread::get_id() << '\n';
    std::cout << ss.str();
#endif

    if (available_seq < next_seq) continue;

    // Only required for claim strategy MultiThreadedStrategyEx as it changes
    // the cursor.
    available_seq = s.GetHighestPublishedSequence(next_seq, available_seq);
    if (available_seq < next_seq) {
      ++exitCtr;

#ifdef PRINT_DEBUG_CONS
      ss.clear();
      ss.str(std::string());
      ss << "Highest published seq: " << available_seq << ' '
         << std::this_thread::get_id() << '\n';
      std::cout << ss.str();
#endif
      // Otherwise goes into a busy loop with blocking strategy
      if (exitCtr > 10)
        std::this_thread::sleep_for(std::chrono::microseconds(1));
      continue;
    }

    for (int64_t i = next_seq; i <= available_seq; ++i) {
      const long& ev = s[i];
#ifdef PRINT_DEBUG_CONS
      std::cout << i << " Event: " << ev << '\n';
#endif
      localSum += ev;
    }

    seq.set_sequence(available_seq);

    if (available_seq == expectedValue) break;

    next_seq = available_seq + 1;

    exitCtr = 0;
  }

  sum.fetch_add(localSum, std::memory_order_relaxed);
}

// Publish data
template <typename T, typename C, typename W>
void produce(disruptor::Sequencer<T, C, W>& s) {
  int iterations = counter * RING_BUFFER_SIZE;

  for (int64_t i = 0; i < iterations; ++i) {
    int64_t sequence = s.Claim(delta);

    if (sequence < delta) {
      for (int k = 0; k <= sequence; ++k) {
        s[k] = k;
#ifdef PRINT_DEBUG_PROD
        std::stringstream ss;
        ss << "Publish seq: " << (k) << ' ' << std::this_thread::get_id()
           << '\n';
        std::cout << ss.str();
#endif
      }

      s.Publish(sequence, delta);
      continue;
    }

    for (int64_t k = sequence; k > sequence - delta; --k) {
      s[k] = k;

#ifdef PRINT_DEBUG_PROD
      std::stringstream ss;
      ss << "Publish seq: " << (k) << ' ' << std::this_thread::get_id() << '\n';
      std::cout << ss.str();
#endif
    }

    s.Publish(sequence, delta);
  }
}

template <typename T, typename C, typename W>
void runOnce() {
  std::cout << "Staring run " << std::endl;

  sum.store(0);

  disruptor::Sequence* sequences = new disruptor::Sequence[nc];

  std::vector<disruptor::Sequence*> seqs;
  disruptor::Sequencer<T, C, W> s(RING_BUFFER_SIZE);

  for (int i = 0; i < nc; ++i) seqs.push_back(&sequences[i]);

  s.set_gating_sequences(seqs);

  std::thread* tc = new std::thread[nc];
  for (int i = 0; i < nc; ++i)
    tc[i] = std::thread(consume<T, C, W>, std::ref(s), std::ref(sequences[i]));

  std::thread* tp = new std::thread[np];

  auto start_time = std::chrono::high_resolution_clock::now();

  for (int i = 0; i < np; ++i)
    tp[i] = std::thread(produce<T, C, W>, std::ref(s));

  // std::this_thread::sleep_for(std::chrono::seconds(3));

  for (int i = 0; i < np; ++i) tp[i].join();
  for (int i = 0; i < nc; ++i) tc[i].join();

  int64_t cursor = s.GetCursor();
  std::cout << "\nBatch size: " << delta
            << " Ring buffer size: " << RING_BUFFER_SIZE << '\n';
  std::cout << "Cursor: " << cursor << '\n';
  std::cout << "Sum: " << sum.load() << " Expected sum: "
            << (expectedValue * (expectedValue + 1) * nc) / 2 << '\n';

  auto end_time = std::chrono::high_resolution_clock::now();
  auto diff = end_time - start_time;

  //std::cout.precision(15);
  std::cout << np << "P-" << nc << "C " << demangle(typeid(C).name()) << " "
            << demangle(typeid(W).name()) << '\n';
  std::cout << (cursor * 1000) /
                   (std::chrono::duration_cast<std::chrono::milliseconds>(diff)
                        .count())
            << " ops/secs"
            << "\n\n";

  delete[] tc;
  delete[] tp;
  delete[] sequences;
}

int main(int argc, char** argv) {
  std::cout.imbue(std::locale(""));

  args::ArgumentParser parser(
      "This is an example program that demonstrates disruptor-- usage.", "");
  args::HelpFlag help(parser, "help", "Display this help menu", {'h', "help"});
  args::ValueFlag<int> num_prod(parser, "num_prod", "Number of producers",
                                {"np"}, np);
  args::ValueFlag<int> num_cons(parser, "num_cons", "Number of consumers",
                                {"nc"}, nc);
  args::ValueFlag<size_t> batch_size(parser, "batch_size", "Batch size", {"bs"},
                                     delta);
  args::ValueFlag<int> multi(parser, "multi",
                             "Multithreaded claim strategy (1 old, 2 new)",
                             {"mt"}, false);
  args::ValueFlag<size_t> looper(parser, "looper",
                                 "Number of times loop over the ring buffer.",
                                 {'l', "loop"}, counter);
  args::ValueFlag<size_t> ring_buffer_size(
      parser, "ring_buffer_size", "Ring buffer size", {"rb"}, RING_BUFFER_SIZE);

  try {
    parser.ParseCLI(argc, argv);
  } catch (args::Help) {
    std::cout << parser;
    return 0;
  } catch (args::ParseError e) {
    std::cerr << e.what() << std::endl;
    std::cerr << parser;
    return 1;
  }

  delta = batch_size.Get();
  counter = looper.Get();
  nc = num_cons.Get();
  RING_BUFFER_SIZE = ring_buffer_size.Get();

  expectedValue = (RING_BUFFER_SIZE * delta * counter * num_prod.Get()) - 1;

  if (delta > RING_BUFFER_SIZE) {
    std::cout << "Batch size cannot be greater than ring buffer size.\n";
    return 1;
  }

  if (multi.Get() == 0) {
    runOnce<long, disruptor::SingleThreadedStrategy,
            disruptor::SleepingStrategy<> >();

    std::this_thread::sleep_for(std::chrono::milliseconds(500));

    runOnce<long, disruptor::SingleThreadedStrategy,
            disruptor::YieldingStrategy<> >();

    std::this_thread::sleep_for(std::chrono::milliseconds(500));

    runOnce<long, disruptor::SingleThreadedStrategy,
            disruptor::BusySpinStrategy>();

    std::this_thread::sleep_for(std::chrono::milliseconds(500));

    runOnce<long, disruptor::SingleThreadedStrategy,
            disruptor::BlockingStrategy>();

  } else {
    np = num_prod.Get();

    switch (multi.Get()) {
      case 1: {
        runOnce<long, disruptor::MultiThreadedStrategy,
                disruptor::SleepingStrategy<> >();

        std::this_thread::sleep_for(std::chrono::milliseconds(500));

        runOnce<long, disruptor::MultiThreadedStrategy,
                disruptor::YieldingStrategy<> >();

        std::this_thread::sleep_for(std::chrono::milliseconds(500));

        runOnce<long, disruptor::MultiThreadedStrategy,
                disruptor::BusySpinStrategy>();

        std::this_thread::sleep_for(std::chrono::milliseconds(500));

        runOnce<long, disruptor::MultiThreadedStrategy,
                disruptor::BlockingStrategy>();
      } break;

      case 2: {
        runOnce<long, disruptor::MultiThreadedStrategyEx,
                disruptor::SleepingStrategy<> >();

        std::this_thread::sleep_for(std::chrono::milliseconds(500));

        runOnce<long, disruptor::MultiThreadedStrategyEx,
                disruptor::YieldingStrategy<> >();

        std::this_thread::sleep_for(std::chrono::milliseconds(500));

        runOnce<long, disruptor::MultiThreadedStrategyEx,
                disruptor::BusySpinStrategy>();

        std::this_thread::sleep_for(std::chrono::milliseconds(500));

        runOnce<long, disruptor::MultiThreadedStrategyEx,
                disruptor::BlockingStrategy>();
      } break;
    }
  }

  return 0;
}
