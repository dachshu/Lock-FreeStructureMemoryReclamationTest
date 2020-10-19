#include <mutex>
#include <thread>
#include <iostream>
#include <chrono>
#include <vector>
#include <mutex>
#include <memory>
#include <atomic>


using namespace std;
using namespace chrono;

static const int NUM_TEST = 4000000;
static const int RANGE = 1000;
static const int MAX_LEVEL = 10;

class LZSKNode
{
public:
	recursive_mutex m_lock;
	int key;
	shared_ptr<LZSKNode> next[MAX_LEVEL];
	int topLevel;
	volatile bool marked = false;
	volatile bool fullyLinked = false;

	// ���ʳ�忡 ���� ������
	LZSKNode() {
		for (int i = 0; i < MAX_LEVEL; i++) {
			next[i] = nullptr;
		}
		topLevel = MAX_LEVEL;
	}
	LZSKNode(int myKey) {
		key = myKey;
		for (int i = 0; i < MAX_LEVEL; i++) {
			next[i] = nullptr;
		}
		topLevel = MAX_LEVEL;
	}

	// �Ϲݳ�忡 ���� ������
	LZSKNode(int x, int height) {
		key = x;
		for (int i = 0; i < MAX_LEVEL; i++) {
			next[i] = nullptr;
		}
		topLevel = height;
	}

	void InitNode() {
		key = 0;
		for (int i = 0; i < MAX_LEVEL; i++) {
			next[i] = nullptr;
		}
		topLevel = MAX_LEVEL;
		marked = false;
		fullyLinked = false;
	}

	void InitNode(int x, int top) {
		key = x;
		for (int i = 0; i < MAX_LEVEL; i++) {
			next[i] = nullptr;
		}
		topLevel = top;
		marked = false;
		fullyLinked = false;
	}

	void lock() { m_lock.lock(); }
	void unlock() { m_lock.unlock(); }
};

class LZSKSET
{
public:

	shared_ptr<LZSKNode> head;
	shared_ptr<LZSKNode> tail;

	LZSKSET() {
		head = make_shared<LZSKNode>(0x80000000);
		tail = make_shared<LZSKNode>(0x7FFFFFFF);
		for (int i = 0; i < MAX_LEVEL; i++) {
			head->next[i] = tail;
		}
	}

	void Init()
	{
		shared_ptr<LZSKNode> curr = head->next[0];
		while (curr != tail) {
			//LZSKNode* temp = curr;
			curr = curr->next[0];
			//delete temp;
		}
		for (int i = 0; i < MAX_LEVEL; i++) {
			head->next[i] = tail;
		}
	}

	int Find(int x, shared_ptr<LZSKNode> preds[], shared_ptr<LZSKNode> succs[])
	{
		int bottomLevel = 0;
		int lFound = -1;
		bool marked = false;
		shared_ptr<LZSKNode> pred = head;
		for (int level = MAX_LEVEL - 1; level >= bottomLevel; --level) {
			shared_ptr<LZSKNode> curr = atomic_load(&pred->next[level]);
			while (x > curr->key) {
				pred = curr; //
				curr = atomic_load(&pred->next[level]);
			}
			if (lFound == -1 && x == curr->key) {
				lFound = level;
			}
			preds[level] = pred; //
			succs[level] = curr; //
		}
		return lFound;
	}

	bool Add(int x)
	{
		int topLevel = 0;
		while ((rand() % 2) == 1)
		{
			topLevel++;
			if (topLevel >= MAX_LEVEL - 1) break;
		}

		int bottomLevel = 0;
		shared_ptr<LZSKNode> preds[MAX_LEVEL];
		shared_ptr<LZSKNode> succs[MAX_LEVEL];

		while (true)
		{
			int lFound = Find(x, preds, succs);
			if (lFound != -1) {
				shared_ptr<LZSKNode> nodeFound = succs[lFound]; //
				if (!nodeFound->marked) {
					while (!nodeFound->fullyLinked) {}
					return false;
				}
				continue;
			}
			int highestLocked = -1;
			{
				shared_ptr<LZSKNode> pred;
				shared_ptr<LZSKNode> succ;
				bool valid = true;
				for (int level = 0; valid && (level <= topLevel); ++level) {
					pred = preds[level]; //
					succ = succs[level]; //
					pred->lock();
					highestLocked = level;
					valid = !pred->marked && !succ->marked && pred->next[level] == succ;
				}
				if (!valid) {
					for (int level = 0; level <= highestLocked; ++level) {
						preds[level]->unlock();
					}
					continue;
				}
				shared_ptr<LZSKNode> newNode = make_shared<LZSKNode>( x, topLevel );
				for (int level = 0; level <= topLevel; ++level) {
					newNode->next[level] = succs[level];
					//atomic_store(&newNode->next[level], succs[level]); //
				}
				for (int level = 0; level <= topLevel; ++level) {
					//preds[level]->next[level] = newNode;
					atomic_store(&preds[level]->next[level], newNode);
				}
				newNode->fullyLinked = true;
				for (int level = 0; level <= highestLocked; ++level) {
					preds[level]->unlock();
				}
				return true;
			}

		}
	}

	bool Remove(int x)
	{
		int bottomLevel = 0;
		shared_ptr<LZSKNode> preds[MAX_LEVEL];
		shared_ptr<LZSKNode> succs[MAX_LEVEL];
		shared_ptr<LZSKNode> victim = nullptr;
		bool isMarked = false;
		int topLevel = -1;

		while (true)
		{
			int lFound = Find(x, preds, succs);
			if (lFound != -1) victim = succs[lFound];
			if (isMarked
				|| (lFound != -1 && (victim->fullyLinked && victim->topLevel == lFound && !victim->marked))) {
				if (!isMarked) {
					topLevel = victim->topLevel;
					victim->lock();
					if (victim->marked) {
						victim->unlock();
						return false;
					}
					victim->marked = true;
					isMarked = true;
				}
				int highestLocked = -1;
				{
					shared_ptr<LZSKNode> pred;
					bool valid = true;
					for (int level = 0; valid && (level <= topLevel); ++level) {
						pred = preds[level]; //
						pred->lock();
						highestLocked = level;
						valid = !pred->marked && pred->next[level] == victim;
					}
					if (!valid) {
						for (int level = 0; level <= highestLocked; ++level) {
							preds[level]->unlock();
						}
						continue;
					}
					for (int level = topLevel; level >= 0; --level) {
						//preds[level]->next[level] = victim->next[level];
						atomic_store(&preds[level]->next[level], victim->next[level]); 
					}
					victim->unlock();
					for (int level = 0; level <= highestLocked; ++level) {
						preds[level]->unlock();
					}
					return true;
				}
			}
			else {
				return false;
			}
		}
	}

	bool Contains(int x)
	{
		int bottomLevel = 0;
		shared_ptr<LZSKNode> preds[MAX_LEVEL];
		shared_ptr<LZSKNode> succs[MAX_LEVEL];
		int lFound = Find(x, preds, succs);
		return (lFound != -1 && succs[lFound]->fullyLinked && !succs[lFound]->marked);
	}
	void Dump()
	{
		shared_ptr<LZSKNode> curr = head;
		printf("First 20 entries are : ");
		for (int i = 0; i < 20; ++i) {
			curr = curr->next[0];
			if (NULL == curr) break;
			printf("%d(%d), ", curr->key, curr->topLevel);
		}
		printf("\n");
	}
};

LZSKSET my_set;

void benchmark(int num_thread)
{
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

		auto start_t = high_resolution_clock::now();
		for (int i = 0; i < num_thread; ++i)
			worker.push_back(thread{ benchmark, num_thread });
		for (auto& th : worker) th.join();
		auto du = high_resolution_clock::now() - start_t;
		my_set.Dump();

		cout << num_thread << " Threads,  Time = ";
		cout << duration_cast<milliseconds>(du).count() << " ms\n";
	}
	system("pause");
}