/*
109. 有序链表转换二叉搜索树
给定一个单链表的头节点  head ，其中的元素 按升序排序 ，将其转换为平衡二叉搜索树。

示例 1:
输入: head = [-10,-3,0,5,9]
输出: [0,-3,9,-10,null,5]
解释: 一个可能的答案是[0，-3,9，-10,null,5]，它表示所示的高度平衡的二叉搜索树。

示例 2:
输入: head = []
输出: []

提示:
head 中的节点数在[0, 2 * 104] 范围内
-105 <= Node.val <= 105
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
/**
 * Definition for a binary tree node.
 * struct TreeNode {
 *     int val;
 *     TreeNode *left;
 *     TreeNode *right;
 *     TreeNode() : val(0), left(nullptr), right(nullptr) {}
 *     TreeNode(int x) : val(x), left(nullptr), right(nullptr) {}
 *     TreeNode(int x, TreeNode *left, TreeNode *right) : val(x), left(left), right(right) {}
 * };
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
/**
 * Definition for a binary tree node.
 * struct TreeNode {
 *     int val;
 *     TreeNode *left;
 *     TreeNode *right;
 *     TreeNode() : val(0), left(nullptr), right(nullptr) {}
 *     TreeNode(int x) : val(x), left(nullptr), right(nullptr) {}
 *     TreeNode(int x, TreeNode *left, TreeNode *right) : val(x), left(left), right(right) {}
 * };
 */
class Solution {
public:
    TreeNode* sortedListToBST(ListNode* head) {
        if (head == nullptr) {
            return nullptr;
        }
        TreeNode* root;
        ListNode* slow = head;
        ListNode* fast = head;
        ListNode* prev = head;

        while(fast->next && fast->next->next) {
            fast = fast->next->next;
            prev = slow;
            slow = slow->next;
        }
        
        root = new TreeNode(slow->val);

        if (prev == slow) {
            root->left = nullptr;
        } else {
            ListNode* left_head = head;
            prev->next = nullptr;
            root->left = sortedListToBST(left_head);
            prev->next = slow;
        }
        
        ListNode* right_head = slow->next;
        root->right = sortedListToBST(right_head);
        return root;
    }
};

// solution 2
class Solution {
private:
    vector<int> v;
public:
    void CreateNode(TreeNode*node,int l1,int r1,int l2,int r2)
    {
        if(l1>r1) return;
        else{
            int i=(1+l1+r1)/2;
            node->left=new TreeNode(v[i]);
            CreateNode(node->left,l1,i-1,i+1,r1);
        }
        if(l2>r2) return;
        else{
            int j=(1+l2+r2)/2;
            node->right=new TreeNode(v[j]);
            CreateNode(node->right,l2,j-1,j+1,r2);
        }

    }

    TreeNode* sortedListToBST(ListNode* head) {
        while(head)
        {
            v.push_back(head->val);
            head=head->next;
        }

        int n=v.size();
        if(n==0) return nullptr;
        int i=n/2;
        TreeNode* root=new TreeNode(v[i]);
        CreateNode(root,0,i-1,i+1,n-1);
        return root;
    }
};

// solution 3
class Solution {
public:
    TreeNode* sortedListToBST(ListNode* head) {
        return dfs(head, nullptr);
    }

    // 生成平衡二叉树[left, right)
    TreeNode* dfs(ListNode* left, ListNode* right) {
        if (!left || left == right) {
            return nullptr;
        } 
        if (left->next == right) {
            return new TreeNode(left->val);
        }
        // 求出的中间元素，可能与left一致
        ListNode* mid = getMid(left, right);
        TreeNode* root = new TreeNode(mid->val);
        root->left = dfs(left, mid);
        root->right = dfs(mid->next, right);
        return root;
    }

    // 快慢指针获取中间元素 [left, right)
    ListNode* getMid(ListNode* left, ListNode* right) {
        ListNode* fast = left;
        ListNode* slow = left;
         while (fast && fast != right) {
            fast = fast->next;
            if (fast && fast != right) {
                fast = fast -> next;
                slow = slow ->next;
            }
        }
        return slow;
    }
};