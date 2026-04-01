#include <iostream>
#include <vector>
#include <cstdlib>
#include <ctime>

class SkipListNode {
private:
    int value;
    std::vector<SkipListNode*> next;
public:
    SkipListNode(int val, int level) : value(val), next(level, nullptr) {}
};

class SkipList {
private:
    SkipListNode* head;
    int maxLevel;
    float p;   // 随机概率

private:
    int randomLevel() {
        int level = 1;
        while (rand() % 100 < p * 100 && level < maxLevel) {
            level++;
        }
        return level;
    }

public:
    SkipList(int maxLevel, float p) : maxLevel(maxLevel), p(p) {
        head = new SkipListNode(-1, maxLevel);
    }

    ~SkipList() {
        SkipListNode* current = head;
        while (current != nullptr) {
            SkipListNode* next = current->next[0];
            delete current;
            current = next;
        }
    }

    bool search(int value);
    void insert(int value);
    void remove(int value);
    void display();
};

bool SkipList::search(int value) {
    SkipListNode* current = head;
    for (int i = maxLevel - 1; i >= 0; i--) {
        while (current->next[i] != nullptr && current->next[i]->value < value) {
            current = current->next[i];
        }
    }
    current = current->next[0];
    return current != nullptr && current->value == value;
}

void SkipList::insert(int value) {
    std::vector<SkipListNode*> update(maxLevel, nullptr);
    SkipListNode* current = head;
    for (int i = maxLevel - 1; i >= 0; i--) {
        while (current->next[i] != nullptr && current->next[i]->value < value) {
            current = current->next[i];
        }
        update[i] = current;
    }

    int level = randomLevel();
    SkipListNode* newNode = new SkipListNode(value, level);
    for (int i = 0; i < level; i++) {
        newNode->next[i] = update[i]->next[i];
        update[i]->next[i] = newNode;
    }
}

void SkipList::remove(int value) {
    std::vector<SkipListNode*> update(maxLevel, nullptr);
    SkipListNode* current = head;
    for (int i = maxLevel - 1; i >= 0; i--) {
        while (current->next[i] != nullptr && current->next[i]->value < value) {
            current = current->next[i];
        }
        update[i] = current;
    }

    current = current->next[0];
    if (current != nullptr && current->value == value) {
        for (int i = 0; i < maxLevel; i++) {
            if (update[i]->next[i] != nullptr && update[i]->next[i]->value == value) {
                update[i]->next[i] = current->next[i];
            }
        }
        delete current;
    }
}

void SkipList::display() {
    for (int i = 0; i < maxLevel; i++) {
        SkipListNode* current = head->next[i];
        std::cout << "Level " << i << ": ";
        while (current != nullptr) {
            std::cout << current->value << " ";
            current = current->next[i];
        }
        std::cout << std::endl;
    }
}

int main() {
    srand(time(0));
    SkipList skipList(4, 0.5);

    skipList.insert(3);
    skipList.insert(6);
    skipList.insert(7);
    skipList.insert(9);
    skipList.insert(12);
    skipList.insert(19);
    skipList.insert(17);
    skipList.insert(26);
    skipList.insert(21);
    skipList.insert(25);

    std::cout << "Skip List:" << std::endl;
    skipList.display();

    std::cout << "Search 19: " << (skipList.search(19) ? "Found" : "Not Found") << std::endl;
    std::cout << "Search 18: " << (skipList.search(18) ? "Found" : "Not Found") << std::endl;

    std::cout << "Remove 19" << std::endl;
    skipList.remove(19);
    skipList.display();

    return 0;
}

/*
***************************test result**************************
Skip List:
Level 0: 3 6 7 9 12 17 19 21 25 26 
Level 1: 3 6 12 17 21 25 26 
Level 2: 3 12 17 25 26 
Level 3: 3 12 25 26 
Search 19: Found
Search 18: Not Found
Remove 19
Level 0: 3 6 7 9 12 17 21 25 26 
Level 1: 3 6 12 17 21 25 26 
Level 2: 3 12 17 25 26 
Level 3: 3 12 25 26
**************************************************************** 
*/

