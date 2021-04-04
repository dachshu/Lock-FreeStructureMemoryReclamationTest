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
static const int MAX_LEVEL = 10;

class LFSKNode;

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
	atomic_uint32_t refCnt{ 0 };

	// ���ʳ�忡 ���� ������
	LFSKNode() {
		for (int i = 0; i < MAX_LEVEL; i++) {
			next[i] = AtomicMarkableReference(NULL, false);
		}
		topLevel = MAX_LEVEL - 1;
		refCnt.store(topLevel + 1, memory_order_seq_cst);
	}
	LFSKNode(int myKey) {
		key = myKey;
		for (int i = 0; i < MAX_LEVEL; i++) {
			next[i] = AtomicMarkableReference(NULL, false);
		}
		topLevel = MAX_LEVEL - 1;
		refCnt.store(topLevel + 1, memory_order_seq_cst);
	}

	// �Ϲݳ�忡 ���� ������
	LFSKNode(int x, int height) {
		key = x;
		for (int i = 0; i < MAX_LEVEL; i++) {
			next[i] = AtomicMarkableReference(NULL, false);
		}
		topLevel = height;
		refCnt.store(height + 1, memory_order_seq_cst);
	}

	void InitNode() {
		key = 0;
		for (int i = 0; i < MAX_LEVEL; i++) {
			next[i] = AtomicMarkableReference(NULL, false);
		}
		topLevel = MAX_LEVEL;
		refCnt.store(topLevel + 1, memory_order_seq_cst);
	}

	void InitNode(int x, int top) {
		key = x;
		for (int i = 0; i < MAX_LEVEL; i++) {
			next[i] = AtomicMarkableReference(NULL, false);
		}
		topLevel = top;
		refCnt.store(topLevel + 1, memory_order_seq_cst);
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

const unsigned int MAX_THREADS = 16;
const unsigned int MAX_HP = 2;
thread_local list<LFSKNode*> rlist;
// cache invalidation 줄이기
atomic<LFSKNode*>* HPpool[MAX_THREADS][MAX_LEVEL][MAX_HP];

int R;
int num_threads;
thread_local int tid;

void scan() {
	for (auto it = rlist.begin(); it != rlist.end();) {
		bool find = false;
		for (int t = 0; t < num_threads && find == false; ++t) {
			for (int l = 0; l < MAX_LEVEL && find == false; ++l) {
				for (int h = 0; h < MAX_HP; ++h) {
					if (HPpool[t][l][h]->load(memory_order_seq_cst) == (*it)) {
						find = true;
						break;
					}
				}
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

void retire(LFSKNode* old)
{
	rlist.push_back(old);
	if (rlist.size() >= R) {
		scan();
	}
}

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
				int cur_idx = 0;
				while (true) {
					curr = GetReference(pred->next[level]);
					HPpool[tid][level][cur_idx]->store(curr, memory_order_seq_cst);
					bool removed;
					LFSKNode* ptr = Get(pred->next[level], &removed);
					if (removed == true) goto retry;
					if (ptr == curr) break;
				}

				while (true) {
					succ = curr->next[level];
					while (Marked(succ)) { //ǥ�õǾ��ٸ� ����
						snip = pred->CompareAndSet(level, curr, succ, false, false);
						if (!snip) goto retry;
						//if (level == bottomLevel) retire(curr);
						if (curr->refCnt.fetch_add(-1, memory_order_seq_cst) <= 1) retire(curr);
						while (true) {
							curr = GetReference(pred->next[level]);
							HPpool[tid][level][cur_idx]->store(curr, memory_order_seq_cst);
							bool removed;
							LFSKNode* ptr = Get(pred->next[level], &removed);
							//if (ptr == nullptr || (unsigned int)ptr == 0xdddddddc) {
							//	cout << "!";
							//}
							if (removed == true) goto retry;
							if (ptr == curr) break;
						}
						//curr = GetReference(pred->next[level]);
						succ = curr->next[level];
					}
					if (curr->key < x) {
						//HPprev[tid][level]->store(curr, memory_order_seq_cst);
						pred = curr;
						cur_idx = (++cur_idx) % MAX_HP;
						while (true) {
							curr = GetReference(pred->next[level]);
							HPpool[tid][level][cur_idx]->store(curr, memory_order_seq_cst);
							bool removed;
							LFSKNode* ptr = Get(pred->next[level], &removed);
							//if (ptr == nullptr || (unsigned int)ptr == 0xdddddddc) {
							//	cout << "!";
							//}
							if (removed == true) goto retry;
							if (ptr == curr) break;
						}
						//curr = GetReference(succ);		
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
		int topLevel = 0;
		LFSKNode* newNode = new LFSKNode;
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
			// ��� Ű�� ���� ǥ�õ��� ���� ��带 ã���� Ű�� �̹� ���տ� �����Ƿ� false ��ȯ
			if (found) {
				delete newNode;
				for (int l = 0; l < MAX_LEVEL; ++l) {
					for (int h = 0; h < MAX_HP; ++h) {
						HPpool[tid][l][h]->store(nullptr, memory_order_seq_cst);
					}
				}
				return false;
			}
			else {
				newNode->InitNode(x, topLevel);

				//for (int level = bottomLevel; level <= topLevel; level++) {
				//	LFSKNode* succ = succs[level];
				//	// ���� ������� next�� ǥ�õ��� ���� ����, find()�� ��ȯ�� ��带 ����
				//	newNode->next[level] = Set(succ, false);
				//}

				//find���� ��ȯ�� pred�� succ�� ���� �������� ���� ����
				LFSKNode* pred = preds[bottomLevel];
				LFSKNode* succ = succs[bottomLevel];

				newNode->next[bottomLevel] = Set(succ, false);

				//pred->next�� ���� succ�� ����Ű�� �ִ��� �ʾҴ��� Ȯ���ϰ� newNode�� ��������
				if (!pred->CompareAndSet(bottomLevel, succ, newNode, false, false))
					// �����ϰ��� next���� ����Ǿ����Ƿ� �ٽ� ȣ���� ����
					continue;

				for (int level = bottomLevel + 1; level <= topLevel; level++) {
					while (true) {
						pred = GetReference(preds[level]);
						succ = GetReference(succs[level]);
						
						while (true) {
							bool mark;
							LFSKNode* ptr = Get(newNode->next[level], &mark);
							if (true == newNode->CompareAndSet(level, ptr, succ, mark, mark))break;
						}
						if (pred->CompareAndSet(level, succ, newNode, false, false))
							break;
						Find(x, preds, succs);
					}
				}

				for (int l = 0; l < MAX_LEVEL; ++l) {
					for (int h = 0; h < MAX_HP; ++h) {
						HPpool[tid][l][h]->store(nullptr, memory_order_seq_cst);
					}
				}
				//��� ������ ����Ǿ����� true��ȯ
				return true;
			}
		}
	}

	bool Remove(int x)
	{
		int bottomLevel = 0;
		LFSKNode* preds[MAX_LEVEL];
		LFSKNode* succs[MAX_LEVEL];
		LFSKNode* succ;

		while (true) {
			bool found = Find(x, preds, succs);
			if (!found) {
				//�������� �����Ϸ��� ��尡 ���ų�, ¦�� �´� Ű�� ���� ��尡 ǥ�� �Ǿ� �ִٸ� false��ȯ
				for (int l = 0; l < MAX_LEVEL; ++l) {
					for (int h = 0; h < MAX_HP; ++h) {
						HPpool[tid][l][h]->store(nullptr, memory_order_seq_cst);
					}
				}
				return false;
			}
			else {
				LFSKNode* nodeToRemove = succs[bottomLevel];
				//�������� ������ ��� ����� next�� mark�� �а� AttemptMark�� �̿��Ͽ� ���ῡ ǥ��
				for (int level = nodeToRemove->topLevel; level >= bottomLevel + 1; level--) {
					succ = nodeToRemove->next[level];
					// ���� ������ ǥ�õǾ������� �޼���� ���������� �̵�
					// �׷��� ���� ��� �ٸ� �����尡 ������ �޴ٴ� ���̹Ƿ� ���� ���� ������ �ٽ� �а�
					// ���ῡ �ٽ� ǥ���Ϸ��� �õ��Ѵ�.
					while (!Marked(succ)) {
						nodeToRemove->CompareAndSet(level, succ, succ, false, true);
						succ = nodeToRemove->next[level];
					}
				}
				//�̺κп� �Դٴ� ���� �������� ������ ��� ���� ǥ���ߴٴ� �ǹ�

				bool marked = false;
				succ = nodeToRemove->next[bottomLevel];
				while (true) {
					//�������� next������ ǥ���ϰ� ���������� Remove()�Ϸ�
					bool iMarkedIt = nodeToRemove->CompareAndSet(bottomLevel, succ, succ, false, true);
					succ = succs[bottomLevel]->next[bottomLevel];

					if (iMarkedIt) {
						Find(x, preds, succs);
						for (int l = 0; l < MAX_LEVEL; ++l) {
							for (int h = 0; h < MAX_HP; ++h) {
								HPpool[tid][l][h]->store(nullptr, memory_order_seq_cst);
							}
						}
						return true;
					}
					else if (Marked(succ)) {
						for (int l = 0; l < MAX_LEVEL; ++l) {
							for (int h = 0; h < MAX_HP; ++h) {
								HPpool[tid][l][h]->store(nullptr, memory_order_seq_cst);
							}
						}
						return false;
					}
				}
			}
		}
	}

	bool Contains(int x)
	{
		LFSKNode* preds[MAX_LEVEL];
		LFSKNode* succs[MAX_LEVEL];
		bool ret = Find(x, preds, succs);
		for (int l = 0; l < MAX_LEVEL; ++l) {
			for (int h = 0; h < MAX_HP; ++h) {
				HPpool[tid][l][h]->store(nullptr, memory_order_seq_cst);
			}
		}
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
	for (int i = 0; i < MAX_THREADS; ++i) {
		for (int l = 0; l < MAX_LEVEL; ++l) {
			for (int h = 0; h < MAX_HP; ++h) {
				HPpool[i][l][h] = new atomic<LFSKNode*>;
			}
		}
	}


	for (int num_thread = 1; num_thread <= 16; num_thread *= 2) {
		my_set.Init();
		worker.clear();

		for (int i = 0; i < MAX_THREADS; ++i) {
			for (int l = 0; l < MAX_LEVEL; ++l) {
				for (int h = 0; h < MAX_HP; ++h) {
					HPpool[i][l][h]->store(nullptr);
				}
			}
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
		for (int l = 0; l < MAX_LEVEL; ++l) {
			for (int h = 0; h < MAX_HP; ++h) {
				delete HPpool[i][l][h];
			}
		}
	}
	system("pause");
}