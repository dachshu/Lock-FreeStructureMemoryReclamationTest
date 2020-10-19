#include <iostream>
#include <thread>
#include <chrono>
#include <atomic>
#include <mutex>
#include <vector>
#include <algorithm>
#include <queue>

using namespace std;
using namespace std::chrono;

class NODE {
public:
	int key;
	NODE* next;

	NODE() { next = nullptr; }
	NODE(int key_value) {
		next = nullptr;
		key = key_value;
	}
	~NODE() {}
};

struct EBR {
	NODE* node;
	unsigned int retire_epoch;

	EBR(NODE* n, unsigned int e) : node(n), retire_epoch(e) {}
};


const int MAX_THREADS = 16;
thread_local queue<EBR> retired;
atomic_uint reservations[MAX_THREADS];

atomic_uint epoch = 0;

thread_local int counter = 0;
const unsigned int epoch_freq = 2;
const unsigned int empty_freq = 10;

int num_threads;
thread_local int tid;


unsigned int get_min_reservation() {
	unsigned int min_re = 0xffffffff;
	for (int i = 0; i < num_threads; ++i) {
		min_re = min(min_re, reservations[i].load(memory_order_acquire));
	}
	return min_re;
}

void empty() {
	unsigned int max_safe_epoch = get_min_reservation();

	// queue에 오래된것부터 쌓인다
	while (false == retired.empty())
	{
		if (retired.front().retire_epoch < max_safe_epoch) {
			delete retired.front().node;
			retired.pop();
		}
		break;
	}
}

void retire(NODE* ptr) {
	retired.push({ ptr, epoch.load(memory_order_acquire) });

	counter++;
	if (counter % epoch_freq == 0)
		epoch.fetch_add(1, memory_order_release);
	if (retired.size() % empty_freq == 0)
		empty();
}

void start_op() {
	//reservations[tid].store(epoch.fetch_add(1, memory_order_relaxed), memory_order_relaxed);
	reservations[tid].store(epoch.load(memory_order_acquire), memory_order_release);

}

void end_op() {
	reservations[tid].store(0xffffffff, memory_order_release);
}

class LFQUEUE {
	NODE* volatile head;
	NODE* volatile tail;
public:
	LFQUEUE()
	{
		head = tail = new NODE(0);
	}
	~LFQUEUE() {}

	void Init()
	{
		NODE* ptr;
		while (head->next != nullptr) {
			ptr = head->next;
			head->next = head->next->next;
			delete ptr;
		}
		tail = head;
	}
	bool CAS(NODE* volatile* addr, NODE* old_node, NODE* new_node)
	{
		return atomic_compare_exchange_strong(reinterpret_cast<volatile atomic_int*>(addr),
			reinterpret_cast<int*>(&old_node),
			reinterpret_cast<int>(new_node));
	}
	void Enq(int key)
	{
		start_op();
		NODE* e = new NODE(key);
		while (true) {
			NODE* last = tail;
			NODE* next = last->next;
			if (last != tail) continue;
			if (next != nullptr) {
				CAS(&tail, last, next);
				continue;
			}
			if (false == CAS(&last->next, nullptr, e)) continue;
			CAS(&tail, last, e);
			end_op();
			return;
		}
	}
	int Deq()
	{
		start_op();
		while (true) {
			NODE* first = head;
			NODE* next = first->next;
			NODE* last = tail;
			NODE* lastnext = last->next;
			if (first != head) continue;
			if (last == first) {
				if (lastnext == nullptr) {
					cout << "EMPTY!!!\n";
					this_thread::sleep_for(1ms);
					end_op();
					return -1;
				}
				else
				{
					CAS(&tail, last, lastnext);
					continue;
				}
			}
			if (nullptr == next) continue;
			int result = next->key;
			if (false == CAS(&head, first, next)) continue;
			first->next = nullptr;
			//delete first;
			retire(first);
			end_op();
			return result;
		}
	}

	void display20()
	{
		int c = 20;
		NODE* p = head->next;
		while (p != nullptr)
		{
			cout << p->key << ", ";
			p = p->next;
			c--;
			if (c == 0) break;
		}
		cout << endl;
	}
};

const auto NUM_TEST = 10000000;

LFQUEUE my_queue;
void ThreadFunc(int num_thread, int t)
{
	tid = t;
	for (int i = 0; i < NUM_TEST / num_thread; i++) {
		if ((rand() % 2 == 0) || (i < (10000 / num_thread))) {
			my_queue.Enq(i);
		}
		else {
			int key = my_queue.Deq();
		}
	}
}

int main()
{
	for (auto n = 1; n <= 16; n *= 2) {
		my_queue.Init();
		for (int r = 0; r < MAX_THREADS; ++r)
			reservations[r] = 0xffffffff;
		num_threads = n;
		epoch.store(1);

		vector <thread> threads;
		auto s = high_resolution_clock::now();
		for (int i = 0; i < n; ++i)
			threads.emplace_back(ThreadFunc, n, i);
		for (auto& th : threads) th.join();
		auto d = high_resolution_clock::now() - s;
		my_queue.display20();
		//my_queue.recycle_freelist();
		cout << n << "Threads,  ";
		cout << ",  Duration : " << duration_cast<milliseconds>(d).count() << " msecs.\n";
	}

	system("pause");
}

