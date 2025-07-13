/*
70. 爬楼梯
已解答
简单
相关标签
相关企业
提示
假设你正在爬楼梯。需要 n 阶你才能到达楼顶。

每次你可以爬 1 或 2 个台阶。你有多少种不同的方法可以爬到楼顶呢？

 

示例 1：

输入：n = 2
输出：2
解释：有两种方法可以爬到楼顶。
1. 1 阶 + 1 阶
2. 2 阶
示例 2：

输入：n = 3
输出：3
解释：有三种方法可以爬到楼顶。
1. 1 阶 + 1 阶 + 1 阶
2. 1 阶 + 2 阶
3. 2 阶 + 1 阶
 

提示：

1 <= n <= 45
*/

class Solution {
public:
    int climbStairs(int n) {

        if (n == 1){
            return 1;
        }
        vector<int> dp(n+1, 0);
        dp[0] = 1;
        dp[1] = 1;
        for (int i = 2; i<=n; i++){
            dp[i] = dp[i-1] + dp[i-2];
        }
        return dp[n];
    }
};

class Solution {
public:
    int res[46] = {0};
    int climbStairs(int n) {
        if (n < 1 || n > 45) {
            return 0;
        }

        if (n == 1) {
            return 1;
        }
        if (n == 2) {
            return 2;
        }
        if (res[n] > 0) {
            return res[n];
        }
        res[n] = climbStairs(n-1) + climbStairs(n-2);
        return res[n];
    }
};

