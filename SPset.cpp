#include <iostream>
#include <cstdlib>
#include <mutex>
#include <thread>
#include <vector>
#include <algorithm>
#include <iterator>
#include <chrono>

using namespace std;

static constexpr int NUM_TEST = 4000000;
static constexpr int RANGE = 1000;

struct Node {
public:
	int key;
	shared_ptr<Node> next;
	mutex m_lock;
	bool deleted{ false };

	Node() : next{ nullptr } {}
	Node(int key) : key{ key }, next{ nullptr } {}
	~Node() {}
	void lock() { m_lock.lock(); }
	void unlock() { m_lock.unlock(); }
};

class ZSet {
	shared_ptr<Node> head;
	shared_ptr<Node> tail;
public:
	ZSet() {
		head = make_shared<Node>(0x80000000);
		tail = make_shared<Node>(0x7fffffff);
		head->next = tail;
	}


	bool add(int x) {
		shared_ptr<Node> pred;
		shared_ptr<Node> curr;
		shared_ptr<Node> e = make_shared<Node>(x);
		while (true)
		{
			pred = head; //
			curr = atomic_load(&(pred->next));

			while (curr->key < x) {
				pred = curr;
				curr = atomic_load(&(curr->next));
			}
			{
				lock_guard<mutex> pl(pred->m_lock);
				lock_guard<mutex> cl(curr->m_lock);
				if (validate(pred, curr)) {
					if (curr->key == x) { return false; }
					else {
						//auto e = new Node{ x };
						e->next = curr; //
						atomic_store(&pred->next, e);
						return true;
					}
				}
			}
		}
	}

	bool remove(int x) {
		shared_ptr<Node> pred;
		shared_ptr<Node> curr;
		while (true)
		{
			pred = head;
			curr = atomic_load(&(pred->next));

			while (curr->key < x) {
				pred = curr;
				curr = atomic_load(&(curr->next));
			}

			{
				lock_guard<mutex> pl(pred->m_lock);
				lock_guard<mutex> cl(curr->m_lock);
				if (validate(pred, curr))
				{
					if (curr->key != x) { return false; }
					else {
						curr->deleted = true;
						//pred->next = curr->next;
						atomic_store(&pred->next, curr->next);
						return true;
					}
				}
			}
		}
	}

	bool contains(int x) {
		shared_ptr<Node> pred;
		shared_ptr<Node> curr;
		while (true)
		{
			pred = head;
			curr = atomic_load(&(pred->next));

			while (curr->key < x) {
				pred = curr;
				curr = atomic_load(&(curr->next));
			}
			return curr->key == x && !curr->deleted;
		}
	}

	void clear() {
		while (head->next != tail) {
			//auto temp = head.next;
			head->next = head->next->next;
			//delete temp;
		}
	}

	bool validate(shared_ptr<Node>& pred, shared_ptr<Node>& curr) {
		return !pred->deleted && !curr->deleted && pred->next == curr;
	}

	void dump(size_t count) {
		auto ptr = head->next;
		cout << count << " Result : ";
		for (auto i = 0; i < count && ptr != tail; ++i) {
			cout << ptr->key << " ";
			ptr = ptr->next;
		}
		cout << "\n";
	}
} mySet;

void benchMark(int num_thread) {
	for (int i = 0; i < NUM_TEST / num_thread; ++i) {
		switch (rand() % 3)
		{
		case 0:
			mySet.add(rand() % RANGE);
			break;
		case 1:
			mySet.remove(rand() % RANGE);
			break;
		case 2:
			mySet.contains(rand() % RANGE);
			break;
		default:
			cout << "Error\n";
			exit(1);
		}
	}
}

int main() {
	vector<thread> threads;

	for (auto thread_num = 1; thread_num <= 16; thread_num *= 2) {
		mySet.clear();
		threads.clear();

		auto start_t = chrono::high_resolution_clock::now();
		generate_n(back_inserter(threads), thread_num, [thread_num]() {return thread{ benchMark, thread_num }; });
		for_each(threads.begin(), threads.end(), [](auto& t) {t.join(); });
		auto du = chrono::high_resolution_clock::now() - start_t;

		mySet.dump(20);

		cout << thread_num << "Threads, Time = ";
		cout << chrono::duration_cast<chrono::milliseconds>(du).count() << "ms \n";
	}

	system("pause");
}