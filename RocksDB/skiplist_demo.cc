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

    bool search(int value) {
        SkipListNode* current = head;
        for (int i = maxLevel - 1; i >= 0; i--) {
            while (current->next[i] != nullptr && current->next[i]->value < value) {
                current = current->next[i];
            }
        }
        current = current->next[0];
        return current != nullptr && current->value == value;
    }

    void insert(int value) {
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

    void remove(int value) {
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

    void display() {
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
};