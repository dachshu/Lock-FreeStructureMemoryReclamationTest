#include <mutex>
#include <thread>
#include <iostream>
#include <chrono>
#include <vector>
#include <mutex>
#include <memory>
#include <atomic>
#include <algorithm>
#include <queue>

using namespace std;
using namespace chrono;

static const int NUM_TEST = 4000000;
static const int RANGE = 1000;

class LFNODE {
public:
	int key;
	unsigned next;

	LFNODE() {
		next = 0;
	}
	LFNODE(int x) {
		key = x;
		next = 0;
	}
	~LFNODE() {
	}
	LFNODE* GetNext() {
		return reinterpret_cast<LFNODE*>(next & 0xFFFFFFFE);
	}

	void SetNext(LFNODE* ptr) {
		next = reinterpret_cast<unsigned>(ptr);
	}

	LFNODE* GetNextWithMark(bool* mark) {
		int temp = next;
		*mark = (temp % 2) == 1;
		return reinterpret_cast<LFNODE*>(temp & 0xFFFFFFFE);
	}

	bool CAS(int old_value, int new_value)
	{
		return atomic_compare_exchange_strong(
			reinterpret_cast<atomic_int*>(&next),
			&old_value, new_value);
	}

	bool CAS(LFNODE* old_next, LFNODE* new_next, bool old_mark, bool new_mark) {
		unsigned old_value = reinterpret_cast<unsigned>(old_next);
		if (old_mark) old_value = old_value | 0x1;
		else old_value = old_value & 0xFFFFFFFE;
		unsigned new_value = reinterpret_cast<unsigned>(new_next);
		if (new_mark) new_value = new_value | 0x1;
		else new_value = new_value & 0xFFFFFFFE;
		return CAS(old_value, new_value);
	}

	bool TryMark(LFNODE* ptr)
	{
		unsigned old_value = reinterpret_cast<unsigned>(ptr) & 0xFFFFFFFE;
		unsigned new_value = old_value | 1;
		return CAS(old_value, new_value);
	}

	bool IsMarked() {
		return (0 != (next & 1));
	}
};

struct EBR {
	LFNODE* node;
	unsigned int retire_epoch;

	EBR(LFNODE* n, unsigned int e) : node(n), retire_epoch(e) {}
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

void retire(LFNODE* ptr) {
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

class LFSET
{
	LFNODE head, tail;
public:
	LFSET()
	{
		head.key = 0x80000000;
		tail.key = 0x7FFFFFFF;
		head.SetNext(&tail);
	}
	void Init()
	{
		while (head.GetNext() != &tail) {
			LFNODE* temp = head.GetNext();
			head.next = temp->next;
			delete temp;
		}
	}

	void Dump()
	{
		LFNODE* ptr = head.GetNext();
		cout << "Result Contains : ";
		for (int i = 0; i < 20; ++i) {
			cout << ptr->key << ", ";
			if (&tail == ptr) break;
			ptr = ptr->GetNext();
		}
		cout << endl;
	}

	void Find(int x, LFNODE** pred, LFNODE** curr)
	{
	retry:
		LFNODE* pr = &head;
		LFNODE* cu = pr->GetNext();
		while (true) {
			bool removed;
			LFNODE* su = cu->GetNextWithMark(&removed);
			while (true == removed) {
				if (false == pr->CAS(cu, su, false, false))
					goto retry;
				retire(cu);
				cu = su;
				su = cu->GetNextWithMark(&removed);
			}
			if (cu->key >= x) {
				*pred = pr; *curr = cu;
				return;
			}
			pr = cu;
			cu = cu->GetNext();
		}
	}
	bool Add(int x)
	{
		LFNODE* e = new LFNODE(x);
		start_op();
		LFNODE* pred, * curr;
		while (true) {
			Find(x, &pred, &curr);

			if (curr->key == x) {
				end_op();
				delete e;
				return false;
			}
			else {
				e->SetNext(curr);
				if (false == pred->CAS(curr, e, false, false))
					continue;
				end_op();
				return true;
			}
		}
	}
	bool Remove(int x)
	{
		start_op();
		LFNODE* pred, * curr;
		while (true) {
			Find(x, &pred, &curr);

			if (curr->key != x) {
				end_op();
				return false;
			}
			else {
				LFNODE* succ = curr->GetNext();
				if (false == curr->TryMark(succ)) continue;
				bool su = pred->CAS(curr, succ, false, false);
				// delete curr;
				//
				if (su) retire(curr);
				end_op();
				return true;
			}
		}
	}
	bool Contains(int x)
	{
		start_op();
		LFNODE* curr = &head;
		while (curr->key < x) {
			curr = curr->GetNext();
		}
		int ret = (false == curr->IsMarked()) && (x == curr->key);
		end_op();
		return ret;
	}
};

LFSET my_set;

void benchmark(int num_thread, int t)
{
	tid = t;
	for (int i = 0; i < NUM_TEST / num_thread; ++i) {
		//	if (0 == i % 100000) cout << ".";
		switch (rand() % 3) {
		case 0: my_set.Add(rand() % RANGE); break;
		case 1: my_set.Remove(rand() % RANGE); break;
		case 2: my_set.Contains(rand() % RANGE); break;
		default: cout << "ERROR!!!\n"; exit(-1);
		}
	}
}

int main()
{
	vector <thread> worker;
	for (int num_thread = 1; num_thread <= 16; num_thread *= 2) {
		my_set.Init();
		worker.clear();
		epoch.store(1);
		for (int r = 0; r < MAX_THREADS; ++r)
			reservations[r] = 0xffffffff;
		num_threads = num_thread;


		auto start_t = high_resolution_clock::now();
		for (int i = 0; i < num_thread; ++i)
			worker.push_back(thread{ benchmark, num_thread, i });
		for (auto& th : worker) th.join();
		auto du = high_resolution_clock::now() - start_t;
		my_set.Dump();

		cout << num_thread << " Threads,  Time = ";
		cout << duration_cast<milliseconds>(du).count() << " ms\n";
	}
	system("pause");
}