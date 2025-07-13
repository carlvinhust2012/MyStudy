// leetcode 43 接雨水
/*
给定 n 个非负整数表示每个宽度为 1 的柱子的高度图，计算按此排列的柱子，下雨之后能接多少雨水。
输入：height = [0,1,0,2,1,0,1,3,2,1,2,1]
输出：6
解释：上面是由数组 [0,1,0,2,1,0,1,3,2,1,2,1] 表示的高度图，在这种情况下，可以接 6 个单位的雨水（蓝色部分表示雨水）。 
示例 2：

输入：height = [4,2,0,3,2,5]
输出：9
 

提示：

n == height.length
1 <= n <= 2 * 104
0 <= height[i] <= 105
*/

// solution 1
class Solution {
public:
    int trap(vector<int>& height) {
        int n = height.size();
        int max_i = 0;
        vector<int> highest(n);

        // 1. find the highest position form left to right
        for (int i = 1; i <= n - 1; i++) {
            if (height[i] >= height[max_i]) {
                for (int j = max_i; j < i; j++) {
                    highest[j] = height[max_i];
                }
                max_i = i;
            }
        }
        // 2. record the highest position
        highest[max_i] = height[max_i];

        // 3. find the highest position form right to max_i
        int max_i1 = n - 1;
        for (int i = n - 2; i >= max_i; i--) {
            if (height[i] >= height[max_i1]) {
                for (int j = max_i1; j > i; j--) {
                    highest[j] = height[max_i1];
                }
                max_i1 = i;
            }
        }

        int sum = 0;
        for (int i = 0; i < n -1; i++) {
            sum += (highest[i] - height[i]);
        }
        return sum;
    }
};

// solution 2
class Solution {
public:
    int trap(vector<int>& height) 
    {
        int result=0;
        stack<int> sta;
        sta.push(0);

        for(int i=1;i<height.size();i++)
        {
            while(!sta.empty()&&height[i]>height[sta.top()])
            {
                int blow=height[sta.top()];
                sta.pop();

                if(!sta.empty())
                {
                    int high=min(height[sta.top()],height[i]);

                    result+=(high-blow)*(i-sta.top()-1);
                }
            }

            sta.push(i);
        }

        return result;
    }
};

// solution 3
class Solution {
public:
    int trap(vector<int>& height) {
        // init the vector 'left', length is 'height.size()', every element is 'height[0]'
        vector<int> left(height.size(), height[0]);  
        vector<int> right(height.size(), height[height.size() - 1]);
        for (int i = 1; i < height.size(); i++) {
            left[i] = max(left[i - 1], height[i]);
        }
        for (int i = height.size() - 2; i >= 0; i--) {
            right[i] = max(right[i + 1], height[i]);
        }

        int res = 0;
        for (int i = 0; i < height.size(); i++) {
            res += min(left[i], right[i]) - height[i];
        }

        return res;
    }
};

