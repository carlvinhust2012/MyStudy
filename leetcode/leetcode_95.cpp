/*
95. 不同的二叉搜索树 II
给你一个整数 n ，请你生成并返回所有由 n 个节点组成且节点值从 1 到 n 互不相同的不同 二叉搜索树 。
可以按 任意顺序 返回答案。

示例 1：
输入：n = 3
输出：[[1,null,2,null,3],[1,null,3,2],[2,1,3],[3,1,null,null,2],[3,2,null,1]]

示例 2：
输入：n = 1
输出：[[1]]

提示：

1 <= n <= 8
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
    vector<TreeNode*> generateTrees(int n) {
        vector<TreeNode*> res;
        if (n <= 0) {
            return res;
        }
        return genTrees(1, n);
    }

    vector<TreeNode *> genTrees(int left, int right) {
        vector<TreeNode *> res;
        if (left > right) {
            res.push_back(nullptr);
            return res;
        }

        for (int i = left; i <= right; i++) {
            vector<TreeNode *> left_nodes = genTrees(left, i-1);
            vector<TreeNode *> right_nodes = genTrees(i+1, right);
            for (TreeNode* left_node : left_nodes) {
                for (TreeNode* right_node : right_nodes) {
                    TreeNode* i_node = new TreeNode(i);
                    i_node->left = left_node;
                    i_node->right = right_node;
                    res.push_back(i_node);
                }
            }
        }
        return res;
    }
};