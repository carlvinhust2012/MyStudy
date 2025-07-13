/*
96. 不同的二叉搜索树
给你一个整数 n ，求恰由 n 个节点组成且节点值从 1 到 n 互不相同的 二叉搜索树 有多少种？返回满足题意的二叉搜索树的种数。

 

示例 1：
输入：n = 3
输出：5

示例 2：
输入：n = 1
输出：1

提示：

1 <= n <= 19
*/


// solution 1
class Solution {
public:
    int numTrees(int n) {
        vector<int> res(n+1, -1);
        if (n <= 0) {
            return 0;
        }
        return dfs(1, n, res);
    }

    int dfs(int start, int end, vector<int> &res) {
        if (start > end) {
            return 1;
        }
        if (res[end - start + 1] != -1) {
            return res[end - start + 1];
        }

        int cnt = 0;
        for (int i = start; i <= end; i++) {
            cnt += dfs(start, i-1, res) * dfs(i+1, end, res);
        }
        res[end - start + 1] = cnt;
        return cnt;
    }
};

// solution 2
class Solution {
 public:
  int numTrees(int n) {
    memo = std::vector<std::vector<int>>(n, std::vector<int>(n, 0));
    return numTrees(0, n - 1);
  }

  int numTrees(int min, int max) {
    if (min >= max) {
      return 1;
    }

    if (memo[min][max] != 0) {
      return memo[min][max];
    }

    int root_count{0};
    for (int i = min; i <= max; i++) {
      int left_count{numTrees(min, i - 1)};

      int right_count{numTrees(i + 1, max)};

      root_count += left_count * right_count;
    }

    memo[min][max] = root_count;

    return root_count;
  }

 private:
  std::vector<std::vector<int>> memo;
};