/*
209 长度最小的子数组

给定一个含有 n 个正整数的数组和一个正整数 target 。

找出该数组中满足其总和大于等于 target 的长度最小的子数组
 [numsl, numsl+1, ..., numsr-1, numsr] ，并返回其长度。如果不存在符合条件的子数组，返回 0 。

示例 1：
输入：target = 7, nums = [2,3,1,2,4,3]
输出：2
解释：子数组 [4,3] 是该条件下的长度最小的子数组。
示例 2：

输入：target = 4, nums = [1,4,4]
输出：1

示例 3：
输入：target = 11, nums = [1,1,1,1,1,1,1,1]
输出：0

提示：
1 <= target <= 109
1 <= nums.length <= 105
1 <= nums[i] <= 104

进阶：

如果你已经实现 O(n) 时间复杂度的解法, 请尝试设计一个 O(n log(n)) 时间复杂度的解法。
*/

// solution1
class Solution {
public:
    int minSubArrayLen(int target, vector<int>& nums) {
        if (nums.size() == 0) {
            return 0;
        }
        int left = 0, right = 0, sum = 0;
        int res = INT_MAX;
        while (right < nums.size()) {
            sum += nums[right];
            while (sum > target) {
                sum -= nums[left];
                ++left;
            }

            if (sum < target && left != 0) {
                --left;
                sum += nums[left];
            }

            if (sum >= target) {
                res = min(res, right-left+1);
            }
            ++right;
        }
        return res != INT_MAX ? res : 0;
    }
};

// solution 2
class Solution {
public:
    int minSubArrayLen(int target, vector<int>& nums) {
        int n = nums.size();
        if(n == 0) {
            return 0;
        }
        int ans = INT_MAX;
        int start = 0, end = 0;
        int sum = 0;
        while(end < n){
            sum += nums[end];
            while(sum >= target){
                ans = min(ans, end - start + 1);
                sum -= nums[start];
                start++;
            }
            end++;
        }
        return ans == INT_MAX ? 0 : ans;
    }
};