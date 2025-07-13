/*
113. 路径总和 II
给你二叉树的根节点 root 和一个整数目标和 targetSum ，找出所有 从根节点到叶子节点 路径总和等于给定目标和的路径。
叶子节点 是指没有子节点的节点。

示例 1：
输入：root = [5,4,8,11,null,13,4,7,2,null,null,5,1], targetSum = 22
输出：[[5,4,11,2],[5,8,4,5]]

示例 2：
输入：root = [1,2,3], targetSum = 5
输出：[]

示例 3：
输入：root = [1,2], targetSum = 0
输出：[]
 
提示：
树中节点总数在范围 [0, 5000] 内
-1000 <= Node.val <= 1000
-1000 <= targetSum <= 1000

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
private:
    vector<vector<int>> res;
    vector<int> path;
    void traversal(TreeNode* cur, int sum) {
        if (cur->left == nullptr && cur->right == nullptr) {
            if (sum == 0) {
                res.push_back(path);
            }
            return;
        }

        if (cur->left) {
            path.push_back(cur->left->val);
            sum -= cur->left->val;
            traversal(cur->left, sum);
            sum += cur->left->val;
            path.pop_back();
        }

        if (cur->right) {
            path.push_back(cur->right->val);
            sum -= cur->right->val;
            traversal(cur->right, sum);
            sum += cur->right->val;
            path.pop_back();
        }
        return;
    }
public:
    vector<vector<int>> pathSum(TreeNode* root, int targetSum) {
        res.clear();
        path.clear();
        if (root == nullptr) {
            return res;
        }
        path.push_back(root->val);
        traversal(root, (targetSum - root->val));
        return res;
    }
};


class Solution {
public:
    vector<vector<int>> ret;
    vector<int> path;
    void dfs(TreeNode*root,int targetSum) {
        if(root == nullptr) {
            return;
        }
        path.push_back(root->val);
        targetSum -= root->val;
        if(root->left == nullptr && root->right == nullptr 
            && targetSum == 0) {
            ret.push_back(path);
        }
        dfs(root->left, targetSum);
        dfs(root->right, targetSum);
        path.pop_back();
    }
    vector<vector<int>> pathSum(TreeNode* root, int targetSum) {
        dfs(root,targetSum);
        return ret;
    }
};