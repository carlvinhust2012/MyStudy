/*
86. 分隔链表
给你一个链表的头节点 head 和一个特定值 x ，请你对链表进行分隔，使得所有 小于 x 的节点都出现在 大于或等于 x 的节点之前。
你应当 保留 两个分区中每个节点的初始相对位置。
*/

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
    ListNode* partition(ListNode* head, int x) {
        if (head == nullptr) {
            return nullptr;
        }

        ListNode* dummy1 = new ListNode(0);
        ListNode* dummy2 = new ListNode(0);
        ListNode* cur = head;
        ListNode* temp1 = dummy1;
        ListNode* temp2 = dummy2;

        while (cur) {
            if (cur->val < x) {
                temp1->next = cur;
                temp1 = temp1->next;
            }
            if (cur->val >= x) {
                temp2->next = cur;
                temp2 = temp2->next;
            }
            cur = cur->next;
        }
        temp1->next = dummy2->next;
        temp2->next = nullptr;
        ListNode* new_head = dummy1->next;
        delete dummy1;
        delete dummy2;
        return new_head;
    }
};
