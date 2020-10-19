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
static const int MAX_LEVEL = 10;

class LFSKNode;

struct EBR {
	LFSKNode* node;
	unsigned int retire_epoch;

	EBR(LFSKNode* n, unsigned int e) : node(n), retire_epoch(e) {}
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

void retire(LFSKNode* ptr) {
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


bool Marked(LFSKNode* curr)
{
	int add = reinterpret_cast<int> (curr);
	return ((add & 0x1) == 0x1);
}

LFSKNode* GetReference(LFSKNode* curr)
{
	int addr = reinterpret_cast<int> (curr);
	return reinterpret_cast<LFSKNode*>(addr & 0xFFFFFFFE);
}

LFSKNode* Get(LFSKNode* curr, bool* marked)
{
	int addr = reinterpret_cast<int> (curr);
	*marked = ((addr & 0x01) != 0);
	return reinterpret_cast<LFSKNode*>(addr & 0xFFFFFFFE);
}

LFSKNode* AtomicMarkableReference(LFSKNode* node, bool mark)
{
	int addr = reinterpret_cast<int>(node);
	if (mark)
		addr = addr | 0x1;
	else
		addr = addr & 0xFFFFFFFE;
	return reinterpret_cast<LFSKNode*>(addr);
}

LFSKNode* Set(LFSKNode* node, bool mark)
{
	int addr = reinterpret_cast<int>(node);
	if (mark)
		addr = addr | 0x1;
	else
		addr = addr & 0xFFFFFFFE;
	return reinterpret_cast<LFSKNode*>(addr);
}

class LFSKNode
{
public:
	int key;
	LFSKNode* next[MAX_LEVEL];
	int topLevel;

	// ���ʳ�忡 ���� ������
	LFSKNode() {
		for (int i = 0; i < MAX_LEVEL; i++) {
			next[i] = AtomicMarkableReference(NULL, false);
		}
		topLevel = MAX_LEVEL;
	}
	LFSKNode(int myKey) {
		key = myKey;
		for (int i = 0; i < MAX_LEVEL; i++) {
			next[i] = AtomicMarkableReference(NULL, false);
		}
		topLevel = MAX_LEVEL;
	}

	// �Ϲݳ�忡 ���� ������
	LFSKNode(int x, int height) {
		key = x;
		for (int i = 0; i < MAX_LEVEL; i++) {
			next[i] = AtomicMarkableReference(NULL, false);
		}
		topLevel = height;
	}

	void InitNode() {
		key = 0;
		for (int i = 0; i < MAX_LEVEL; i++) {
			next[i] = AtomicMarkableReference(NULL, false);
		}
		topLevel = MAX_LEVEL;
	}

	void InitNode(int x, int top) {
		key = x;
		for (int i = 0; i < MAX_LEVEL; i++) {
			next[i] = AtomicMarkableReference(NULL, false);
		}
		topLevel = top;
	}

	bool CompareAndSet(int level, LFSKNode* old_node, LFSKNode* next_node, bool old_mark, bool next_mark) {
		int old_addr = reinterpret_cast<int>(old_node);
		if (old_mark) old_addr = old_addr | 0x1;
		else old_addr = old_addr & 0xFFFFFFFE;
		int next_addr = reinterpret_cast<int>(next_node);
		if (next_mark) next_addr = next_addr | 0x1;
		else next_addr = next_addr & 0xFFFFFFFE;
		return atomic_compare_exchange_strong(reinterpret_cast<atomic_int*>(&next[level]), &old_addr, next_addr);
		//int prev_addr = InterlockedCompareExchange(reinterpret_cast<long *>(&next[level]), next_addr, old_addr);
		//return (prev_addr == old_addr);
	}
};


class LFSKSET
{
public:

	LFSKNode* head;
	LFSKNode* tail;

	LFSKSET() {
		head = new LFSKNode(0x80000000);
		tail = new LFSKNode(0x7FFFFFFF);
		for (int i = 0; i < MAX_LEVEL; i++) {
			head->next[i] = AtomicMarkableReference(tail, false);
		}
	}

	void Init()
	{
		LFSKNode* curr = head->next[0];
		while (curr != tail) {
			LFSKNode* temp = curr;
			curr = GetReference(curr->next[0]);
			delete temp;
		}
		for (int i = 0; i < MAX_LEVEL; i++) {
			head->next[i] = AtomicMarkableReference(tail, false);
		}
	}

	bool Find(int x, LFSKNode* preds[], LFSKNode* succs[])
	{
		int bottomLevel = 0;
		bool marked = false;
		bool snip;
		LFSKNode* pred = NULL;
		LFSKNode* curr = NULL;
		LFSKNode* succ = NULL;
	retry:
		while (true) {
			pred = head;
			for (int level = MAX_LEVEL - 1; level >= bottomLevel; level--) {
				curr = GetReference(pred->next[level]);
				while (true) {
					succ = curr->next[level];
					while (Marked(succ)) { 
						snip = pred->CompareAndSet(level, curr, succ, false, false);
						if (!snip) goto retry;
						if (level == bottomLevel) retire(curr);
						curr = GetReference(pred->next[level]);
						succ = curr->next[level];
					}

					if (curr->key < x) {
						pred = curr;
						curr = GetReference(succ);
					}
					else {
						break;
					}
				}
				preds[level] = pred;
				succs[level] = curr;
			}
			return (curr->key == x);
		}
	}

	bool Add(int x)
	{
		LFSKNode* newNode = new LFSKNode;
		start_op();
		int topLevel = 0;
		while ((rand() % 2) == 1)
		{
			topLevel++;
			if (topLevel >= MAX_LEVEL - 1) break;
		}

		int bottomLevel = 0;
		LFSKNode* preds[MAX_LEVEL];
		LFSKNode* succs[MAX_LEVEL];
		while (true) {
			bool found = Find(x, preds, succs);
			if (found) {
				end_op();
				delete newNode;
				return false;
			}
			else {
				newNode->InitNode(x, topLevel);

				//for (int level = bottomLevel; level <= topLevel; level++) {
				//	LFSKNode* succ = succs[level];
				//	newNode->next[level] = Set(succ, false);
				//}

				LFSKNode* pred = preds[bottomLevel];
				LFSKNode* succ = succs[bottomLevel];

				newNode->next[bottomLevel] = Set(succ, false);

				if (!pred->CompareAndSet(bottomLevel, succ, newNode, false, false))
					continue;

				for (int level = bottomLevel + 1; level <= topLevel; level++) {
					while (true) {
						pred = GetReference(preds[level]);
						succ = GetReference(succs[level]);
						//newNode->next[level] = Set(succ, false);
						//if (pred->CompareAndSet(level, succ, newNode, false, false))
						//	break;
						//Find(x, preds, succs);
						if (newNode->CompareAndSet(level, newNode->next[level], succ, false, false)) {
							if (pred->CompareAndSet(level, succ, newNode, false, false))
								break;
							Find(x, preds, succs);
						}
						else {
							Find(x, preds, succs);
							end_op();
							return true;
						}
					}
				}
				Find(x, preds, succs);
				end_op();
				return true;
			}
		}
	}

	bool Remove(int x)
	{
		start_op();
		int bottomLevel = 0;
		LFSKNode* preds[MAX_LEVEL];
		LFSKNode* succs[MAX_LEVEL];
		LFSKNode* succ;

		while (true) {
			bool found = Find(x, preds, succs);
			if (!found) {
				end_op();
				return false;
			}
			else {
				LFSKNode* nodeToRemove = succs[bottomLevel];
				for (int level = nodeToRemove->topLevel; level >= bottomLevel + 1; level--) {
					succ = nodeToRemove->next[level];
					while (!Marked(succ)) {
						nodeToRemove->CompareAndSet(level, succ, succ, false, true);
						succ = nodeToRemove->next[level];
					}
				}

				bool marked = false;
				succ = nodeToRemove->next[bottomLevel];
				while (true) {
					bool iMarkedIt = nodeToRemove->CompareAndSet(bottomLevel, succ, succ, false, true);
					succ = succs[bottomLevel]->next[bottomLevel];

					if (iMarkedIt) {
						Find(x, preds, succs);
						end_op();
						return true;
					}
					else if (Marked(succ)) {
						end_op();
						return false;
					}
				}
			}
		}
	}

	bool Contains(int x)
	{
		start_op();
		int bottomLevel = 0;
		bool marked = false;
		LFSKNode* pred = head;
		LFSKNode* curr = NULL;
		LFSKNode* succ = NULL;

		for (int level = MAX_LEVEL - 1; level >= bottomLevel; level--) {
			curr = GetReference(pred->next[level]);
			while (true) {
				succ = curr->next[level];
				while (Marked(succ)) {
					curr = GetReference(curr->next[level]);
					succ = curr->next[level];
				}
				if (curr->key < x) {
					pred = curr;
					curr = GetReference(succ);
				}
				else {
					break;
				}
			}
		}
		bool ret = (curr->key == x);
		end_op();
		return ret;
	}
	void Dump()
	{
		LFSKNode* curr = head;
		printf("First 20 entries are : ");
		for (int i = 0; i < 20; ++i) {
			curr = curr->next[0];
			if (NULL == curr) break;
			printf("%d(%d), ", curr->key, curr->topLevel);
		}
		printf("\n");
	}
};

LFSKSET my_set;

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