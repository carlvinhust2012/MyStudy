/**
 * 61. 旋转链表
 * 给你一个链表的头节点 head ，旋转链表，将链表每个节点向右移动 k 个位置。
 * 输入：head = [1,2,3,4,5], k = 2
 * 输出：[4,5,1,2,3]
 * 输入：head = [0,1,2], k = 4
 * 输出：[2,0,1]
 * 提示：
 * 链表中节点的数目在范围 [0, 500] 内
 * -100 <= Node.val <= 100
 * 0 <= k <= 2 * 109
 * /
 
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

// solution 1
class Solution {
public:
    ListNode* rotateRight(ListNode* head, int k) {
        if (k < 1 || head == nullptr) {
            return head;
        }

        ListNode* cur = head;
        ListNode* prev = head;
        int cnt = 1;
        while (cur->next) {
            prev = cur;
            cur = cur->next;
            cnt++;
        }

        cur->next = (cur == head ? nullptr : head);
        prev->next = nullptr;
        return rotateRight(cur, ((k-1) % cnt));

    }
};

// solution 2
class Solution {
public:
    ListNode* rotateRight(ListNode* head, int k) {
        //相当于把尾巴截断，然后放到头部
        if(head==nullptr) {
            return head;
        }

        ListNode* dummy = new ListNode(0,head);
        int n=0;
        ListNode* tail = dummy;

        while(tail->next){
            n++;
            tail=tail->next;
        }

        if(k%n ==0) {
            return head;
        }
        
        //第n-k个为前端的末尾节点
        int c = n-k;
        ListNode* p = dummy;
        while(c--) {
            p=p->next;
        }
        //开始链接
        dummy->next = p->next;
        tail->next = head;
        p->next = nullptr;
        return dummy->next;
    }
};