// Merge Two Sorted Lists
// 第一种解法
#include <iostream>

// Definition for singly-linked list.
struct ListNode {
    int val;
    ListNode *next;
    ListNode() : val(0), next(nullptr) {}
    ListNode(int x) : val(x), next(nullptr) {}
    ListNode(int x, ListNode *next) : val(x), next(next) {}
 };

class Solution {
public:
    // Use recursion to solve the problem
    ListNode* mergeTwoLists(ListNode* l1, ListNode* l2) {

        if (l1 == NULL) {
            return l2;
        }
        if (l2 == NULL) {
            return l1;
        }
        if (l1->val < l2->val) {
            l1->next = mergeTwoLists(l1->next, l2);
            return l1;
        }
        else {
            l2->next = mergeTwoLists(l1, l2->next);
            return l2;
        }
    }
};

int main() {
    Solution A;
    ListNode Node1(4);
    ListNode Node2(3, &Node1);
    ListNode Node6(1, &Node2);
    ListNode Node3(3);
    ListNode Node4(2, &Node3);
    ListNode Node5(1, &Node4);

    ListNode* p = A.mergeTwoLists(&Node5, &Node6);
    while (p) {
        std::cout <<p->val << " ";
        p = p->next;
    }

    return 1;
}

// [Running] cd "d:\CodeTrainning\leetcode\.vscode\" && g++ tempCodeRunnerFile.cc -o tempCodeRunnerFile && "d:\CodeTrainning\leetcode\.vscode\"tempCodeRunnerFile
// 1 1 2 3 3 4 
// [Done] exited with code=1 in 0.997 seconds


// 第二种解法：
#include <iostream>

// Definition for singly-linked list.
struct ListNode {
    int val;
    ListNode *next;
    ListNode() : val(0), next(nullptr) {}
    ListNode(int x) : val(x), next(nullptr) {}
    ListNode(int x, ListNode *next) : val(x), next(next) {}
 };

class Solution {
public:
    ListNode* mergeTwoLists(ListNode* l1, ListNode* l2) {
        ListNode* dummy = new ListNode(-1);
        ListNode* cur = dummy;

        while (l1 && l2)
        {
            std::cout << l1->val << " " << l2->val <<std::endl;
            if (l1->val < l2->val) {
                cur->next = l1;
                l1 = l1->next;
            }
            else {
                cur->next = l2;
                l2 = l2->next;
            }
            std::cout << "cur->val:" << cur->val << " " <<std::endl;
            cur = cur->next;
        }
        cur->next = l1 ? l1 : l2;
        return dummy->next;
    }
};

int main() {
    Solution A;
    ListNode Node1(4);
    ListNode Node2(3, &Node1);
    ListNode Node6(1, &Node2);
    //ListNode Node3();
    ListNode Node4(5);
    ListNode Node5(1, &Node4);

    ListNode* p = A.mergeTwoLists(&Node5, &Node6);
    while (p) {
        std::cout <<p->val << " ";
        p = p->next;
    }

    return 1;
}

// [Running] cd "d:\CodeTrainning\leetcode\.vscode\" && g++ tempCodeRunnerFile.cc -o tempCodeRunnerFile && "d:\CodeTrainning\leetcode\.vscode\"tempCodeRunnerFile
// 1 1
// cur->val:-1 
// 1 3
// cur->val:1 
// 5 3
// cur->val:1 
// 5 4
// cur->val:3 
// 1 1 3 4 5 
// [Done] exited with code=1 in 1.02 seconds
