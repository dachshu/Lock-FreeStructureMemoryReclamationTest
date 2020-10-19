#include <iostream>
#include <thread>
#include <chrono>
#include <atomic>
#include <mutex>
#include <vector>
#include <list>

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

const unsigned int MAX_THREADS = 16;
thread_local list<NODE*> rlist;
atomic<NODE*>* HPfirst[MAX_THREADS];
atomic<NODE*>* HPnext[MAX_THREADS];
atomic<NODE*>* HPlast[MAX_THREADS];

int R;
int num_thread;
thread_local int tid;

void scan() {
	for (auto it = rlist.begin(); it != rlist.end();) {
		bool find = false;
		for (int t = 0; t < num_thread; ++t) {
			if (HPfirst[t]->load(memory_order_acquire) == (*it)
				|| HPnext[t]->load(memory_order_acquire) == (*it)
				|| HPlast[t]->load(memory_order_acquire) == (*it)) {
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

void retire(NODE* old)
{
	rlist.push_back(old);
	if (rlist.size() >= R) {
		scan();
	}
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
		NODE* e = new NODE(key);
		while (true) {
			NODE* volatile last;
			do {
				last = tail;
				HPlast[tid]->store(last, memory_order_release);
			} while (last != tail);
			NODE* next = last->next;
			if (last != tail) continue;
			if (next != nullptr) {
				CAS(&tail, last, next);
				continue;
			}
			if (false == CAS(&last->next, nullptr, e)) continue;
			CAS(&tail, last, e);
			HPlast[tid]->store(nullptr, memory_order_release);
			return;
		}
	}
	int Deq()
	{
		while (true) {
			NODE* volatile first;
			do {
				first = head;
				HPfirst[tid]->store(first, memory_order_release);
			} while (first != head);

			// queue는 중간에 insert 되는 경우 X
			// 순서 바뀌는 경우 X
			// head, tail 만 계속 변한다
			NODE* next = first->next;
			HPnext[tid]->store(next, memory_order_release);

			NODE* last;
			do {
				last = tail;
				HPlast[tid]->store(last, memory_order_release);
			} while (last != tail);
			NODE* lastnext = last->next;
			if (first != head) continue;
			if (last == first) {
				if (lastnext == nullptr) {
					cout << "EMPTY!!!\n";
					this_thread::sleep_for(1ms);
					HPfirst[tid]->store(nullptr, memory_order_release);
					HPnext[tid]->store(nullptr, memory_order_release);
					HPlast[tid]->store(nullptr, memory_order_release);
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
			retire(first);
			//delete first;
			HPfirst[tid]->store(nullptr, memory_order_release);
			HPnext[tid]->store(nullptr, memory_order_release);
			HPlast[tid]->store(nullptr, memory_order_release);
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
	for (int i = 0; i < MAX_THREADS; ++i) {
		HPfirst[i] = new atomic<NODE*>;
		HPnext[i] = new atomic<NODE*>;
		HPlast[i] = new atomic<NODE*>;

	}

	for (auto n = 1; n <= 16; n *= 2) {
		my_queue.Init();
		for (int i = 0; i < MAX_THREADS; ++i) {
			HPfirst[i]->store(nullptr);
			HPnext[i]->store(nullptr);
			HPlast[i]->store(nullptr);
		}
		num_thread = n;
		R = 3 * num_thread * 2;

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


	for (int i = 0; i < MAX_THREADS; ++i) {
		delete HPfirst[i];
		delete HPnext[i];
		delete HPlast[i];

	}
	system("pause");
}

