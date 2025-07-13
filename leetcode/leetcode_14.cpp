/*
14. 最长公共前缀
编写一个函数来查找字符串数组中的最长公共前缀。

如果不存在公共前缀，返回空字符串 ""。


示例 1：

输入：strs = ["flower","flow","flight"]
输出："fl"
示例 2：

输入：strs = ["dog","racecar","car"]
输出：""
解释：输入不存在公共前缀。
 

提示：

1 <= strs.length <= 200
0 <= strs[i].length <= 200
strs[i] 仅由小写英文字母组成
*/

class Solution {
public:
    string longestCommonPrefix(vector<string>& strs) {
        sort(strs.begin(), strs.end());
        for (auto str : strs) {
            cout << "str is " << str << endl;
        }

        string &s1=strs.front();
        string &s2=strs.back();

        int i=0;
        while(i<s1.size() && i<s2.size() && s1[i]==s2[i]){
            ++i;
        }
        return string(s1.begin(),s1.begin()+i);
    }
};
