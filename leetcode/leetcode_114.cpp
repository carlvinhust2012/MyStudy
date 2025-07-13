/*
114.二叉树展开为链表
给你二叉树的根结点 root ，请你将它展开为一个单链表：

展开后的单链表应该同样使用 TreeNode ，其中 right 子指针指向链表中下一个结点，而左子指针始终为 null 。
展开后的单链表应该与二叉树 先序遍历 顺序相同。

输入：root = [1,2,5,3,4,null,6]
输出：[1,null,2,null,3,null,4,null,5,null,6]
示例 2：

输入：root = []
输出：[]
示例 3：

输入：root = [0]
输出：[0]
 

提示：

树中结点数在范围 [0, 2000] 内
-100 <= Node.val <= 100
 

进阶：你可以使用原地算法（O(1) 额外空间）展开这棵树吗？
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

// solution 1
class Solution {
public:
    TreeNode* last = nullptr;   
    void flatten(TreeNode* root) {
        if (root == nullptr) {
            cout << "root is null" << endl;
            return;
        }
        cout << "val=" << root->val << endl;
        flatten(root->right);
        flatten(root->left);
        root->right = last;
        cout << "root->right=" << root->right << endl;
        root->left = nullptr;
        cout << "root->left=" << root->left << endl;
        last = root;
        cout << "last->val=" << last->val << endl;

    }
};

/* debug info:
[1,2,5,3,4,null,6]

===step1:
val=1
val=5
val=6
root is null <--flatten(6->right)
root is null <--flatten(6->left)
root->right=0
root->left=0
last->val=6  <--flatten(6)

===step2:
root is null <--flatten(5->left)
root->right=0x503000000190
root->left=0
last->val=5  <--flatten(5)

===step3:
val=2  <--flatten(1->right)
val=4  <--flatten(2->right)
root is null
root is null
root->right=0x503000000100
root->left=0
last->val=4

val=3
root is null
root is null
root->right=0x503000000160
root->left=0
last->val=3

root->right=0x503000000130
root->left=0
last->val=2

root->right=0x5030000000d0
root->left=0
last->val=1

step1: flatten(1)-->flatten(5)->flatten(6)->flatten(6->right)->flatten(6->left)
       because 6->right = nullptr, so flatten(6->right) print "root is null"
       because 6->left  = nullptr, so flatten(6->left) print "root is null"
       root->right = nullptr


*/


// solution 2
class Solution {
public:
    void flatten(TreeNode* root) {
        if (root == nullptr) {
            return;
        }

        TreeNode* prev = nullptr;
        std::stack<TreeNode*> stack;
        stack.push(root);

        while (!stack.empty()) {
            auto cur = stack.top();
            stack.pop();

            if (cur->right != nullptr) {
                stack.push(cur->right);
            }
            if (cur->left != nullptr) {
                stack.push(cur->left);
            }

            cur->left = nullptr;
            cur->right = nullptr;
            if (prev != nullptr) {
                prev->right = cur;
            }

            prev = cur;
        }
        return;
    }
};


/*
stack [1]
cur = 1, stack [2,5], prev = 1
cur = 2, stack [3,4,5] prev->right = 2
cur = 3, stack [4,5] prev->right = 3
cur = 4, stack [5] prev->right = 4
cur = 5, stack [6] prev->right = 5
cur = 6, stack [] prev->right = 6
*/