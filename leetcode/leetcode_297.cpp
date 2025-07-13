/*
297.二叉树的序列化与反序列化
序列化是将一个数据结构或者对象转换为连续的比特位的操作，进而可以将转换后的数据存储在一个文件或者内存中，
同时也可以通过网络传输到另一个计算机环境，采取相反方式重构得到原数据。
请设计一个算法来实现二叉树的序列化与反序列化。这里不限定你的序列 / 反序列化算法执行逻辑，
你只需要保证一个二叉树可以被序列化为一个字符串并且将这个字符串反序列化为原始的树结构。
提示: 输入输出格式与 LeetCode 目前使用的方式一致，详情请参阅 LeetCode 序列化二叉树的格式。
你并非必须采取这种方式，你也可以采用其他的方法解决这个问题。

示例 1：
输入：root = [1,2,3,null,null,4,5]
输出：[1,2,3,null,null,4,5]

示例 2：
输入：root = []
输出：[]

示例 3：
输入：root = [1]
输出：[1]

示例 4：
输入：root = [1,2]
输出：[1,2]

提示：

树中结点数在范围 [0, 104] 内
-1000 <= Node.val <= 1000
*/

/**
 * Definition for a binary tree node.
 * struct TreeNode {
 *     int val;
 *     TreeNode *left;
 *     TreeNode *right;
 *     TreeNode(int x) : val(x), left(NULL), right(NULL) {}
 * };
 */

//solution 1
class Codec {
public:

    // Encodes a tree to a single string.
    string serialize(TreeNode* root) {
        if (root == nullptr) {
            return "#_";
        }
        string res = to_string(root->val) + "_";
        res += serialize(root->left);
        res += serialize(root->right);
        return res;
    }

    // Decodes your encoded data to tree.
    TreeNode* deserialize(string data) {
        std::stringstream ss(data);
        std::string item;
        std::queue<string> que;
        while (std::getline(ss, item, '_')) {
            que.push(item);
        }
        return helper(que);
    }
    TreeNode* helper(std::queue<string>& que) {
        string val = que.front();
        que.pop();
        if (val == "#") {
            return nullptr;
        }
        TreeNode* head = new TreeNode(stoi(val));
        head->left = helper(que);
        head->right = helper(que);
        return head;
    }
};

// Your Codec object will be instantiated and called as such:
// Codec ser, deser;
// TreeNode* ans = deser.deserialize(ser.serialize(root));

//solution 2
class Codec {
public:
    // Encodes a tree to a single string.
    //构造时用逗号分隔，避免反序列化时需要格式转换问题
    string serialize(TreeNode* root) {
        if (root == NULL) return "#";
        
        stringstream ss;
        queue<TreeNode*> que;
        que.push(root);
        
        while (!que.empty()) {
            TreeNode* node = que.front();
            que.pop();
            
            if (node) {
                ss << node->val << ",";
                que.push(node->left);
                que.push(node->right);
            } else {
                ss << "#,";
            }
        }
        
        string result = ss.str();
        result.pop_back(); // 移除末尾的逗号
        return result;
    }

    // Decodes your encoded data to tree.
    TreeNode* deserialize(string data) {
        if (data == "#") return NULL;
        
        stringstream ss(data);
        string val;
        getline(ss, val, ',');
        
        TreeNode* root = new TreeNode(stoi(val));
        queue<TreeNode*> que;
        que.push(root);
        
        while (!que.empty()) {
            TreeNode* node = que.front();
            que.pop();
            
            if (getline(ss, val, ',')) {//getline是流操作，会自动记住当前位置
                if (val != "#") {
                    node->left = new TreeNode(stoi(val));
                    que.push(node->left);
                } else {
                    node->left = NULL;
                }
            }
            
            if (getline(ss, val, ',')) {
                if (val != "#") {
                    node->right = new TreeNode(stoi(val));
                    que.push(node->right);
                } else {
                    node->right = NULL;
                }
            }
        }
        
        return root;
    }
};

//solution 3
class Codec {
public:

    // Encodes a tree to a single string.
    string serialize(TreeNode* root) {
        if (root == nullptr) return ""; // Use X as a placeholder for null
        string res;
        dfs(res, root);
        cout<<res;
        return res;
    }

    void dfs(string& res, TreeNode* root) {
        res += to_string(root->val) + '[';
        if (root->left) {
            res += 'L';
            dfs(res, root->left);
        }
        if (root->right) {
            res += 'R';
            dfs(res, root->right);
        }
        res += ']';
    }

    // Decodes your encoded data to tree.
    TreeNode* deserialize(string data) {
        if (data.empty()) return nullptr;
        int index = 0;
        return dfsDeserialize(data, index);
    }

    TreeNode* dfsDeserialize(const string& data, int& index) {
        string num;
        while (index < data.size() && data[index] != '[') {
            num += data[index];
            index++;
        }
        TreeNode* node = new TreeNode(stoi(num));
        if (index < data.size() && data[index] == '[') {
            index++; // skip '['
            while (index < data.size() && data[index] != ']') {
                if (data[index] == 'R') {
                    index++;
                    node->right = dfsDeserialize(data, index);
                }
                if (data[index] == 'L') {
                    index++;
                    node->left = dfsDeserialize(data, index);
                }
            }
            index++;
        }
        return node;
    }
};

// Usage:
// Codec ser, deser;
// TreeNode* ans = deser.deserialize(ser.serialize(root));