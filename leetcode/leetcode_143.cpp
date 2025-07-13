/*
143. 重排链表
给定一个单链表 L 的头节点 head ，单链表 L 表示为：

L0 → L1 → … → Ln - 1 → Ln
请将其重新排列后变为：

L0 → Ln → L1 → Ln - 1 → L2 → Ln - 2 → …
不能只是单纯的改变节点内部的值，而是需要实际的进行节点交换。

 

示例 1：



输入：head = [1,2,3,4]
输出：[1,4,2,3]
示例 2：



输入：head = [1,2,3,4,5]
输出：[1,5,2,4,3]
 

提示：

链表的长度范围为 [1, 5 * 104]
1 <= node.val <= 1000
*/

// solution 1
/**
 * Definition for singly-linked list.
 * struct ListNode {
 *     int val;
 *     ListNode *next;
 *     ListNode() : val(0), next(nullptr) {}
 *     ListNode(int x) : val(x), next(nullptr) {}
 *     ListNode(int x, ListNode *next) : val(x), next(next) {}
 * };
 */
class Solution {
public:
    void reorderList(ListNode* head) {
        if (head == nullptr) {
            return;
        }
        vector<ListNode*> vec;
        ListNode* cur = head;
        while (cur) {
            vec.push_back(cur);
            cur = cur->next;
        }

        int left = 0;
        int right = vec.size() - 1;
        while (left < right) {
            vec[left]->next = vec[right];
            vec[right]->next = vec[++left];
            right--;
        }
        vec[left]->next = nullptr;
    }
};


// solution 2 
/**
 * Definition for singly-linked list.
 * struct ListNode {
 *     int val;
 *     ListNode *next;
 *     ListNode() : val(0), next(nullptr) {}
 *     ListNode(int x) : val(x), next(nullptr) {}
 *     ListNode(int x, ListNode *next) : val(x), next(next) {}
 * };
 */

// 1.快慢指针找到中点 
// 2.拆成两个链表 
// 3.遍历两个链表，后面的塞到前面的“缝隙里”
class Solution {
public:
    ListNode* reverseList(ListNode* head) {
        ListNode* prev = nullptr;
        ListNode* first = head;
        while (first) {
            ListNode* second = first->next;
            first->next = prev;
            prev = first;
            first = second;
        }
        return prev;
    }

    ListNode* mergeList(ListNode* first, ListNode* second) {
        ListNode* dummy = new ListNode(-1);
        ListNode* tail = dummy;
        while (first && second) {
            tail->next = first;
            first = first->next;
            tail = tail->next;

            tail->next = second;
            second = second->next;
            tail = tail->next;
        }
        // 如果还有剩余节点
        if (first) {
            tail->next = first;
        } else if (second) {
            tail->next = second;
        }
        ListNode* res = dummy->next;
        delete dummy;
        return res;
    }
    
    void reorderList(ListNode* head) {
        if (!head || !head->next)
            return;

        // 找到链表的中点
        ListNode* slow = head;
        ListNode* fast = head;
        while (fast->next && fast->next->next) {
            slow = slow->next;
            fast = fast->next->next;
        }

        // 反转后半部分链表
        ListNode* second = reverseList(slow->next);
        slow->next = nullptr; // 断开前半部分和后半部分

        // 合并两个链表
        ListNode* first = head;
        head = mergeList(first, second);
    }
};