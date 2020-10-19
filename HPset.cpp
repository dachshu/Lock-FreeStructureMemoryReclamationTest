#include <mutex>
#include <thread>
#include <iostream>
#include <chrono>
#include <vector>
#include <mutex>
#include <memory>
#include <atomic>
#include <list>

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


const unsigned int MAX_THREADS = 16;
thread_local list<LFNODE*> rlist;
atomic<LFNODE*>* HPprev[MAX_THREADS];
atomic<LFNODE*>* HPcurr[MAX_THREADS];

int R;
int num_threads;
thread_local int tid;

void scan() {
	for (auto it = rlist.begin(); it != rlist.end();) {
		bool find = false;
		for (int t = 0; t < num_threads; ++t) {
			if (HPprev[t]->load(memory_order_acquire) == (*it)
				|| HPcurr[t]->load(memory_order_acquire) == (*it)) {
				find = true;
				break;
			}
		}
		if (find == false) {
			delete (*it);
			it = rlist.erase(it);
		}
		else {
			++it;
		}
	}
}

void retire(LFNODE* old)
{
	rlist.push_back(old);
	if (rlist.size() >= R) {
		scan();
	}
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
		LFNODE* cu;
		do {
			cu = pr->GetNext();
			HPcurr[tid]->store(cu, memory_order_release);
		} while (pr->GetNext() != cu);
		
		while (true) {
			bool removed;
			LFNODE* su = cu->GetNextWithMark(&removed);
			while (true == removed) {
				if (false == pr->CAS(cu, su, false, false))
					goto retry;
				retire(cu);
				while (true)
				{
					cu = pr->GetNext();
					HPcurr[tid]->store(cu, memory_order_release);
					bool marked;
					LFNODE* ptr = pr->GetNextWithMark(&marked);
					if (marked == true) goto retry;
					if (ptr == cu) break;
				}
				su = cu->GetNextWithMark(&removed);
			}
			if (cu->key >= x) {
				*pred = pr; *curr = cu;
				return;
			}
			HPprev[tid]->store(cu, memory_order_release);
			pr = cu;
			while (true)
			{
				cu = pr->GetNext();
				HPcurr[tid]->store(cu, memory_order_release);
				bool marked;
				LFNODE* ptr = pr->GetNextWithMark(&marked);
				if (marked == true) goto retry;
				if (ptr == cu) break;
			}
		}
	}
	bool Add(int x)
	{
		LFNODE* pred, * curr;
		LFNODE* e = new LFNODE(x);
		while (true) {
			Find(x, &pred, &curr);

			if (curr->key == x) {
				delete e;
				HPprev[tid]->store(nullptr, memory_order_release);
				HPcurr[tid]->store(nullptr, memory_order_release);
				return false;
			}
			else {
				e->SetNext(curr);
				if (false == pred->CAS(curr, e, false, false))
					continue;
				HPprev[tid]->store(nullptr, memory_order_release);
				HPcurr[tid]->store(nullptr, memory_order_release);
				return true;
			}
		}
	}
	bool Remove(int x)
	{
		LFNODE* pred, * curr;
		while (true) {
			Find(x, &pred, &curr);

			if (curr->key != x) {
				HPprev[tid]->store(nullptr, memory_order_release);
				HPcurr[tid]->store(nullptr, memory_order_release);
				return false;
			}
			else {
				LFNODE* succ = curr->GetNext();
				if (false == curr->TryMark(succ)) continue;
				if (true == pred->CAS(curr, succ, false, false)) {
					retire(curr);
					// delete curr;
				}
				HPprev[tid]->store(nullptr, memory_order_release);
				HPcurr[tid]->store(nullptr, memory_order_release);
				return true;
			}
		}
	}
	bool Contains(int x)
	{
		LFNODE* prev = &head;
		LFNODE* curr;
		do {
			curr = prev->GetNext();
			HPcurr[tid]->store(curr, memory_order_release);
		} while (prev->GetNext() != curr);

		while (curr->key < x) {
			prev = curr;
			HPprev[tid]->store(curr, memory_order_release);
			while (true)
			{
				curr = prev->GetNext();
				HPcurr[tid]->store(curr, memory_order_release);
				bool marked;
				LFNODE* ptr = prev->GetNextWithMark(&marked);
				if (marked == true) {
					Find(x, &prev, &curr);
					bool ret = (curr->key == x && (false == curr->IsMarked()));
					HPprev[tid]->store(nullptr, memory_order_release);
					HPcurr[tid]->store(nullptr, memory_order_release);
					return ret;
				}
				if (ptr == curr) break;
			}
		}
		bool ret = (false == curr->IsMarked()) && (x == curr->key);
		HPprev[tid]->store(nullptr, memory_order_release);
		HPcurr[tid]->store(nullptr, memory_order_release);
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
	for (int i = 0; i < MAX_THREADS; ++i) {
		HPprev[i] = new atomic<LFNODE*>;
		HPcurr[i] = new atomic<LFNODE*>;

	}
	for (int num_thread = 1; num_thread <= 16; num_thread *= 2) {
		my_set.Init();
		worker.clear();
		for (int i = 0; i < MAX_THREADS; ++i) {
			HPprev[i]->store(nullptr);
			HPcurr[i]->store(nullptr);
		}
		num_threads = num_thread;
		R = 3 * num_thread * 2;

		auto start_t = high_resolution_clock::now();
		for (int i = 0; i < num_thread; ++i)
			worker.push_back(thread{ benchmark, num_thread, i });
		for (auto& th : worker) th.join();
		auto du = high_resolution_clock::now() - start_t;
		my_set.Dump();

		cout << num_thread << " Threads,  Time = ";
		cout << duration_cast<milliseconds>(du).count() << " ms\n";
	}

	for (int i = 0; i < MAX_THREADS; ++i) {
		delete HPprev[i];
		delete HPcurr[i];
	}
	system("pause");
}