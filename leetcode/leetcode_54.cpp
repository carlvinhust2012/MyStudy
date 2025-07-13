/*
54. 螺旋矩阵
给你一个 m 行 n 列的矩阵 matrix ，请按照 顺时针螺旋顺序 ，返回矩阵中的所有元素。
输入：matrix = [[1,2,3],[4,5,6],[7,8,9]]
输出：[1,2,3,6,9,8,7,4,5]
输入：matrix = [[1,2,3,4],[5,6,7,8],[9,10,11,12]]
输出：[1,2,3,4,8,12,11,10,9,5,6,7]

提示：

m == matrix.length
n == matrix[i].length
1 <= m, n <= 10
-100 <= matrix[i][j] <= 100
*/

// solution 1
class Solution {
public:
    vector<int> spiralOrder(vector<vector<int>>& matrix) {
        vector<int> res;
        if (matrix.empty() || matrix[0].empty()) {
            return res;
        }
        int m = matrix.size();    // height
        int n = matrix[0].size(); // length
        int up = 0;
        int down = m - 1;
        int left = 0;
        int right = n - 1;

        while(true) {
            for (int i = left; i <= right; i++) {
                res.push_back(matrix[up][i]);
            }
            if (++up > down) {
                break;
            }
            for (int i = up; i <= down; i++) {
                res.push_back(matrix[i][right]);
            }
            if (--right < left) {
                break;
            }
            for (int i = right; i >= left; i--) {
                res.push_back(matrix[down][i]);
            }
            if (--down < up) {
                break;
            }
            for (int i = down; i >= up; i--) {
                res.push_back(matrix[i][left]);
            }
            if (++left > right) {
                break;
            }
        }
        return res;
    }
};


/*
matrix = [[1,2,3],
          [4,5,6],
          [7,8,9]]
====> output = [1,2,3,6,9,8,7,4,5]
Hint:
step 1: [1,2,3]  matrix[up][i], i from left to right, up = up + 1
step 2: 6]       matrix[i][right], i from up to down, right = right -1;
        9]
step 3: [7,8     matrix[down][i], i from right to left
step 4: [4       matrix[i][left], i from down to up, left = left + 1
step 5: 5        matrix[up][i]
*/