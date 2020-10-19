#include <iostream>
#include <thread>
#include <chrono>
#include <atomic>
#include <mutex>
#include <vector>
#include <memory>

using namespace std;
using namespace std::chrono;

class NODE {
public:
	int key;
	shared_ptr<NODE> next;

	NODE() { next = nullptr; }
	NODE(int key_value) {
		next = nullptr;
		key = key_value;
	}
	~NODE() {}
};


class LFQUEUE {
	shared_ptr<NODE> head;
	shared_ptr<NODE> tail;
public:
	LFQUEUE()
	{
		head = make_shared<NODE>(0);
		tail = head;
	}
	~LFQUEUE() {}

	void Init()
	{
		while (head->next != nullptr) {
			head->next = head->next->next;
		}
		tail = head;
	}
	bool CAS(shared_ptr<NODE>* addr, shared_ptr<NODE>& old_node, shared_ptr<NODE>& new_node)
	{
		return atomic_compare_exchange_strong(addr, (&old_node), new_node);
	}

	void Enq(int key)
	{
		//shared_ptr<NODE> e(new NODE(key));
		shared_ptr<NODE> e = make_shared<NODE>(key);

		while (true) {
			shared_ptr<NODE> last = atomic_load(&tail);
			shared_ptr<NODE> next = atomic_load(&last->next);
			if (last != tail) continue;
			if (next != nullptr) {
				CAS(&tail, last, next);
				continue;
			}
			shared_ptr<NODE> null_sp;
			if (false == CAS(&(last->next), null_sp, e)) continue;
			CAS(&tail, last, e);
			return;
		}
	}
	int Deq()
	{
		while (true) {
			shared_ptr<NODE> first = atomic_load(&head);
			shared_ptr<NODE> next = atomic_load(&(first->next));
			shared_ptr<NODE> last = atomic_load(&tail);
			shared_ptr<NODE> lastnext = atomic_load(&last->next);
			if (first != head) continue;
			if (last == first) {
				if (lastnext == nullptr) {
					cout << "EMPTY!!!\n";
					this_thread::sleep_for(1ms);
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
			return result;
		}
	}

	void display20()
	{
		int c = 20;
		shared_ptr<NODE> p = head->next;
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
void ThreadFunc(int num_thread)
{
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
		vector <thread> threads;
		auto s = high_resolution_clock::now();
		for (int i = 0; i < n; ++i)
			threads.emplace_back(ThreadFunc, n);
		for (auto& th : threads) th.join();
		auto d = high_resolution_clock::now() - s;
		my_queue.display20();
		//my_queue.recycle_freelist();
		cout << n << "Threads,  ";
		cout << ",  Duration : " << duration_cast<milliseconds>(d).count() << " msecs.\n";
	}

	system("pause");
}

