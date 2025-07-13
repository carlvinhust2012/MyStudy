/*
219 存在重复的元素 II
给你一个整数数组 nums 和一个整数 k ，判断数组中是否存在两个 不同的索引 i 和 j ，满足 nums[i] == nums[j] 且 abs(i - j) <= k 。
如果存在，返回 true ；否则，返回 false 。

示例 1：

输入：nums = [1,2,3,1], k = 3
输出：true
示例 2：

输入：nums = [1,0,1,1], k = 1
输出：true
示例 3：

输入：nums = [1,2,3,1,2,3], k = 2
输出：false

提示：

1 <= nums.length <= 105
-109 <= nums[i] <= 109
0 <= k <= 105
*/

// solution 1
class Solution {
public:
    bool containsNearbyDuplicate(vector<int>& nums, int k) {
            int n=nums.size();
            unordered_set<int>s;

            for(int i=0;i<n;i++)
            {
                if(i>k)
                {
                    s.erase(nums[i-k-1]);
                }
                if(s.count(nums[i]))
                {
                    return true;
                }
                s.emplace(nums[i]);
            }
            return false;
    }
};

// solution 2
class Solution {
public:
    bool containsNearbyDuplicate(vector<int>& nums, int k) {
            int n=nums.size();
            unordered_set<int>s;

            for(int i=0;i<n;i++)
            {
                if(i>k)
                {
                    s.erase(nums[i-k-1]);
                }
                if(s.count(nums[i]))
                {
                    return true;
                }
                s.emplace(nums[i]);
            }
            return false;
    }
};